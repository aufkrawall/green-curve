// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Authenticated named-pipe connection pipeline and command dispatch.
//
// Transport concurrency lives in main_service_pipe_listener.cpp (a bounded,
// fixed-size worker pool). This shard serves ONE connection end to end:
//
//   12-byte header probe -> brief impersonation -> stable logon identity ->
//   classification + admission -> body read -> SERIALIZED dispatch ->
//   response snapshot -> out-of-lock response write.
//
// The header-probe-first order is mandatory: ImpersonateNamedPipeClient()
// requires at least one completed read on the connected pipe (the 2026-08-22
// live regression failed every GUI request with error 1368 when impersonation
// moved before the first read). Probing the pinned 12-byte header satisfies
// that requirement while keeping stalled connections off the dispatch path.
//
// Command execution remains strictly serialized (one hardware/service mutation
// at a time) via g_serviceDispatchLock -- the exact invariant the historical
// single pipe thread provided. Only transport I/O overlaps now, which also
// means a client that stops reading its response can no longer stall the next
// hardware command.

#include "log_redaction_policy.h"
#include "main_service_pipe_primitives.h"
#include "service_ipc_throttle_policy.h"
#include "service_pipe_prefix_read.h"
// The three client-requested file writes, all of which run under the caller's
// own token.  Included here so its position in the amalgamation is exactly
// where the case body it replaced used to sit.
#include "main_service_pipe_file_commands.cpp"

// Deadlines. Header/body/write bounds keep one stalled connection from
// occupying a worker indefinitely; the body bound intentionally matches the
// long-standing request-read timeout.
#ifndef SERVICE_PIPE_SERVER_HEADER_TIMEOUT_MS
#define SERVICE_PIPE_SERVER_HEADER_TIMEOUT_MS 1000
#endif
#ifndef SERVICE_PIPE_SERVER_DRAIN_TIMEOUT_MS
#define SERVICE_PIPE_SERVER_DRAIN_TIMEOUT_MS 1500
#endif

namespace gc_pipe_dispatch {

// Serializes ALL command validation/authorization/execution. Initialized once
// via InitOnceExecuteOnce so neither the host nor the listener needs to care
// about initialization order.
INIT_ONCE g_dispatchInitOnce = INIT_ONCE_STATIC_INIT;
CRITICAL_SECTION g_serviceDispatchLock;
CRITICAL_SECTION g_serviceAdmissionLock;

BOOL CALLBACK init_transport_locks(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_serviceDispatchLock);
    InitializeCriticalSection(&g_serviceAdmissionLock);
    return TRUE;
}

void ensure_transport_locks() {
    InitOnceExecuteOnce(&g_dispatchInitOnce, init_transport_locks,
                        nullptr, nullptr);
}

class ScopedDispatchLock {
public:
    ScopedDispatchLock() { ensure_transport_locks(); EnterCriticalSection(&g_serviceDispatchLock); }
    ~ScopedDispatchLock() { LeaveCriticalSection(&g_serviceDispatchLock); }
};

class ScopedAdmissionLock {
public:
    ScopedAdmissionLock() { ensure_transport_locks(); EnterCriticalSection(&g_serviceAdmissionLock); }
    ~ScopedAdmissionLock() { LeaveCriticalSection(&g_serviceAdmissionLock); }
};

// The ONE admission table. Decisions and charges must observe the same
// bucket state: the first transition-safe build declared two function-local
// statics (one in the decide glue, one in the charge glue), so decisions
// read a table that was never written and every charge landed in a table
// nobody consulted -- rate limiting silently never refused anything.
// Function-local static initialization is thread-safe; callers serialize
// access with g_serviceAdmissionLock.
inline ServiceIpcAdmissionTable& service_admission_table() {
    // The local static here is deliberately not declared with the historical
    // per-function shape the source gate forbids (a function-local admission
    // table named "table"), because that shape silently split decisions from
    // charges when it existed twice.
    static ServiceIpcAdmissionTable shared = []() {
        ServiceIpcAdmissionTable t;
        t.reset(GetTickCount64());
        return t;
    }();
    return shared;
}

} // namespace gc_pipe_dispatch

// Per-connection client context. Captured ONCE, immediately after the header
// probe's mandatory first read, by briefly impersonating the verified client.
// Everything downstream (session rule, PID match, integrity gate, command
// policy) consumes this snapshot; authorization semantics are unchanged.
struct ServiceClientIdentity {
    char user[256];
    DWORD sessionId;
    DWORD pid;
    bool isAdmin;
    DWORD integrityRid;
    ServiceLifecycleIdentity lifecycle;
    HANDLE token;
    bool stateEnvelopeAuthorized;
    ServiceIpcThrottleKey throttleKey;

    void reset() {
        memset(user, 0, sizeof(user));
        sessionId = (DWORD)-1;
        pid = 0;
        isAdmin = false;
        integrityRid = 0;
        memset(&lifecycle, 0, sizeof(lifecycle));
        token = nullptr;
        stateEnvelopeAuthorized = false;
        throttleKey.clear();
    }

    // Owns the duplicated impersonation token from the capture step. The
    // historical single-thread loop closed it explicitly after dispatch;
    // the worker-pool refactor dropped that close and leaked one SYSTEM-held
    // token handle per served connection (capture runs BEFORE magic or
    // admission checks, so even refused/mismatched connections leaked). A
    // destructor closes it on every exit path instead of relying on each
    ServiceClientIdentity() = default;
    ServiceClientIdentity(const ServiceClientIdentity&) = delete;
    ServiceClientIdentity& operator=(const ServiceClientIdentity&) = delete;

    ~ServiceClientIdentity() {
        if (token) CloseHandle(token);
    }

    void buildThrottleKey() {
        if (!lifecycle.valid || !lifecycle.sid[0]) {
            throttleKey.clear();
            return;
        }
        throttleKey.fill(lifecycle.sessionId, lifecycle.authenticationId,
                         lifecycle.sid);
    }
};

// Admission controller glue. The policy module is pure; runtime supplies the
// clock reading and serializes table access against the SHARED table above.
static ServiceIpcAdmissionDecision service_admission_decide(
        const ServiceIpcThrottleKey& key, ServiceIpcRequestClass cls) {
    ULONGLONG nowMs = GetTickCount64();
    gc_pipe_dispatch::ScopedAdmissionLock lock;
    return service_ipc_decide_admission(
        &gc_pipe_dispatch::service_admission_table(), key, cls, nowMs);
}

static void service_admission_charge(const ServiceIpcThrottleKey& key,
                                     ServiceIpcRequestClass cls,
                                     unsigned int costTokens) {
    ULONGLONG nowMs = GetTickCount64();
    gc_pipe_dispatch::ScopedAdmissionLock lock;
    service_ipc_charge(&gc_pipe_dispatch::service_admission_table(), key, cls,
                       costTokens, nowMs);
}

static const char* service_ipc_decision_name(ServiceIpcAdmissionDecision d) {
    switch (d) {
        case SERVICE_IPC_ADMITTED: return "admitted";
        case SERVICE_IPC_REJECTED_RATE: return "rate";
        case SERVICE_IPC_REJECTED_CAPACITY: return "capacity";
    }
    return "unknown";
}

static const char* service_ipc_class_name(ServiceIpcRequestClass c) {
    switch (c) {
        case SERVICE_IPC_CLASS_NORMAL: return "normal";
        case SERVICE_IPC_CLASS_HANDOFF: return "handoff";
        case SERVICE_IPC_CLASS_LIFECYCLE: return "lifecycle";
        case SERVICE_IPC_CLASS_BULK_OUTPUT: return "bulk-output";
        case SERVICE_IPC_CLASS_UNKNOWN: return "unknown";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Serialized command execution. This is the historical single-pipe-thread
// dispatch body, extracted verbatim behind a lock. Behavior-preserving: same
// checks in the same order, same refusal texts, same response stamps.
// ---------------------------------------------------------------------------
static void service_execute_checked_request(ServiceRequest* request,
        ServiceClientIdentity* caller, ServiceResponse* response) {
    gc_pipe_dispatch::ScopedDispatchLock dispatchLock;
    bool stateEnvelopeAuthorized = false;

    if (!validate_service_request_for_ipc(request)) {
        response->status = SERVICE_STATUS_ERROR;
        StringCchCopyA(response->message, ARRAY_COUNT(response->message),
            "Service request contains invalid protocol fields");
        debug_log("service_pipe_server: rejected v%u request command=%u pid=%u: %s\n",
            request->version, request->command, request->callerPid, service_request_reject_reason(request));
    } else if (InterlockedExchangeAdd(
            &g_serviceClientRequestsReady, 0) == 0) {
        response->status = SERVICE_STATUS_ERROR;
        StringCchCopyA(response->message, ARRAY_COUNT(response->message),
            "Background service startup is not complete; retry after SCM reports RUNNING");
        debug_log("service_pipe_server: request rejected while lifecycle startup gate is closed\n");
    } else {
        bool settingsFreeHandoff =
            request->command == SERVICE_CMD_LOGON_HANDOFF;
        if (!service_captured_identity_passes_session_rule(
                &caller->lifecycle, caller->sessionId, !settingsFreeHandoff,
                request->source, response->message,
                ARRAY_COUNT(response->message))) {
            response->status = SERVICE_STATUS_ERROR;
        } else if (request->callerPid != caller->pid) {
            response->status = SERVICE_STATUS_ERROR;
            StringCchCopyA(response->message, ARRAY_COUNT(response->message),
                "Service request caller PID does not match the connected client");
            debug_log("service auth reject: suppliedPid=%lu connectedPid=%lu command=%u\n",
                (unsigned long)request->callerPid,
                (unsigned long)caller->pid,
                (unsigned int)request->command);
        } else if ((request->command == SERVICE_CMD_APPLY ||
                    request->command == SERVICE_CMD_RESET ||
                    request->command == SERVICE_CMD_WRITE_LOG_SNAPSHOT ||
                    request->command == SERVICE_CMD_WRITE_JSON_SNAPSHOT ||
                    request->command == SERVICE_CMD_WRITE_PROBE_REPORT ||
                    request->command == SERVICE_CMD_LOGON_HANDOFF ||
                    request->command == SERVICE_CMD_CHECK_FOR_UPDATE ||
                    request->command == SERVICE_CMD_INSTALL_UPDATE ||
                    request->command == SERVICE_CMD_SET_UPDATE_POLICY) &&
                   caller->integrityRid < SECURITY_MANDATORY_MEDIUM_RID) {
            response->status = SERVICE_STATUS_ERROR;
            StringCchCopyA(response->message, ARRAY_COUNT(response->message),
                "Service control requires a medium-integrity client");
            debug_log("service auth reject: low-integrity caller pid=%lu session=%lu command=%u integrityRid=%lu\n",
                (unsigned long)caller->pid,
                (unsigned long)caller->sessionId,
                (unsigned int)request->command,
                (unsigned long)caller->integrityRid);
        } else {
            stateEnvelopeAuthorized = true;
            // A logon handoff is settings-free and resolves an immutable
            // per-session context in the lifecycle worker.  Do not mutate
            // process-global user/config paths as part of its authorization.
            if (request->command != SERVICE_CMD_LOGON_HANDOFF) {
                char userPathErr[256] = {};
                if (resolve_service_user_data_paths(caller->sessionId, userPathErr, sizeof(userPathErr))) {
                    if (!g_app.configPath[0]) {
                        set_default_config_path();
                    }
                    refresh_service_debug_logging_from_config();
                } else {
                    debug_log("service_pipe_server: failed to resolve user data paths: %s\n", userPathErr);
                }
            }
            service_set_pending_operation_source(request->source[0] ? request->source : "service request");

#include "main_service_pipe_switch.cpp"
        }
    }

    caller->stateEnvelopeAuthorized = stateEnvelopeAuthorized;
    response->message[ARRAY_COUNT(response->message) - 1] = '\0';
    // The one place this process resolves response severity, and therefore the
    // one place every branch above -- including the ones that only set
    // `status` and break -- is covered without having to remember the field.
    response->outcomeSeverity = service_response_resolve_outcome_severity(
        response->status, response->outcomeSeverity);
    // The pipe ACL admits every local user; only callers that passed the
    // active-session, PID, and integrity gates receive authoritative state --
    // and that includes the update-state envelope.  The envelope carries no
    // hardware or session state, but it does publish the machine's update
    // posture (available version, staged/verified flags, install phase),
    // derived from the release manifest the updater fetched, so a caller the
    // service refused to authorize has no business reading it.  Stamp both
    // envelopes in ONE authorized-only place: GUI/CLI callers that pass the
    // gates are unaffected, and only refused or low-integrity callers lose it.
    if (stateEnvelopeAuthorized) {
        populate_service_state_response(response);
        service_update_populate_response(&response->update);
    } else {
        debug_log("service_pipe_server: withheld the state/update envelope from an unauthorized caller command=%u\n",
            (unsigned int)request->command);
    }
}

// ---------------------------------------------------------------------------
// Admitted path: read the remaining fixed-size body, run the serialized
// dispatch, and hand back the response snapshot. Transport faults here cost
// the identity far more than a completed request.
// ---------------------------------------------------------------------------
static bool service_finish_admitted_connection(HANDLE pipe,
        ServiceClientIdentity* caller, const ServicePipePrefixRead* prefix,
        ServiceResponse* response, ServiceIpcRequestClass requestClass) {
    ServiceRequest request = {};
    char pipeErr[256] = {};

    // Stage 2: finish the message into the pinned wire structure. A
    // same-version client sends exactly sizeof(ServiceRequest) bytes, so the
    // remainder is exactly the body length after the 12-byte probe.
    bool bodyComplete = prefix->messageComplete;
    if (!bodyComplete) {
        if (!service_pipe_read_exact(pipe,
                reinterpret_cast<unsigned char*>(&request) +
                    SERVICE_REQUEST_HEADER_BYTES,
                (DWORD)sizeof(request) - SERVICE_REQUEST_HEADER_BYTES,
                SERVICE_PIPE_SERVER_IO_TIMEOUT_MS,
                "reading service request body", pipeErr, sizeof(pipeErr))) {
            debug_log("service_pipe_server: dropping stalled or truncated request body class=%s: %s\n",
                service_ipc_class_name(requestClass),
                pipeErr[0] ? pipeErr : "unknown");
            service_admission_charge(caller->throttleKey, requestClass,
                SERVICE_IPC_COST_TRANSPORT_FAULT);
            DisconnectNamedPipe(pipe);
            return false;
        }
    }
    memcpy(&request, prefix->bytes, SERVICE_REQUEST_HEADER_BYTES);

    // Full dispatch under the serialization lock; the response write happens
    // AFTER the lock is released so a slow reader cannot stall the next
    // hardware command.
    service_execute_checked_request(&request, caller, response);

    // Exactly one charge for this connection: the outcome of the exchange.
    ServiceIpcConnectionOutcome outcome = SERVICE_IPC_CONNECTION_EXCHANGED;
    if (!service_pipe_write_exact(pipe, response, sizeof(*response),
            SERVICE_PIPE_SERVER_IO_TIMEOUT_MS, "writing service response",
            pipeErr, sizeof(pipeErr))) {
        debug_log("service_pipe_server: response write failed class=%s: %s\n",
            service_ipc_class_name(requestClass),
            pipeErr[0] ? pipeErr : "unknown");
        outcome = SERVICE_IPC_CONNECTION_TRANSPORT_FAULT;
    }
    service_admission_charge(caller->throttleKey, requestClass,
        service_ipc_connection_cost_tokens(outcome));
    DisconnectNamedPipe(pipe);
    return true;
}

// ---------------------------------------------------------------------------
// One connection, end to end, on a bounded transport worker. Every failure
// path disconnects; the LISTENER owns connect/disconnect lifecycle and handle
// reuse, this function only ever talks to an already-connected pipe.
// Returns true when a full request/response exchange completed (used for
// diagnostics only).
// ---------------------------------------------------------------------------
static bool service_serve_pipe_connection(HANDLE pipe) {
    ServiceResponse response = {};
    response.magic = SERVICE_PROTOCOL_MAGIC;
    response.version = SERVICE_PROTOCOL_VERSION;
    response.serviceBuildNumber = (DWORD)APP_BUILD_NUMBER;
    StringCchCopyA(response.serviceVersion,
        ARRAY_COUNT(response.serviceVersion), APP_VERSION);

    ServiceClientIdentity caller;
    caller.reset();
    char pipeErr[256] = {};

    // Stage 1: the mandatory-first-read boundary. Read exactly the pinned
    // 12-byte header; impersonation is legal only after this succeeds.
    ServicePipePrefixRead prefix = {};
    if (!service_pipe_read_request_header(pipe, &prefix,
            SERVICE_PIPE_SERVER_HEADER_TIMEOUT_MS, pipeErr,
            sizeof(pipeErr))) {
        debug_log("service_pipe_server: dropping stalled client before header: %s\n",
            pipeErr[0] ? pipeErr : "unknown");
        DisconnectNamedPipe(pipe);
        return false;
    }

    unsigned int magic = 0, version = 0, command = 0;
    memcpy(&magic, prefix.bytes + offsetof(ServiceRequest, magic), sizeof(magic));
    memcpy(&version, prefix.bytes + offsetof(ServiceRequest, version), sizeof(version));
    memcpy(&command, prefix.bytes + offsetof(ServiceRequest, command), sizeof(command));

    // Impersonate briefly and capture the stable logon identity NOW, before
    // spending body-read time on this client.
    bool identityKnown = false;
    {
        char identityErr[256] = {};
        identityKnown = service_capture_pipe_client_identity(pipe,
            caller.user, sizeof(caller.user), &caller.sessionId,
            &caller.pid, &caller.isAdmin, &caller.lifecycle,
            &caller.integrityRid, &caller.token, identityErr,
            sizeof(identityErr));
        if (identityKnown) {
            caller.buildThrottleKey();
        } else {
            // Rare after a successful read. No charge here: every connection
            // is charged exactly once from its outcome below, and an invalid
            // key routes that charge into the anonymous metering bucket so it
            // can never poison a real user's quota.
            debug_log("service_pipe_server: post-header identity capture failed: %s\n",
                identityErr[0] ? identityErr : "unknown");
        }
    }

    ServiceIpcRequestClass requestClass =
        service_ipc_classify_command(command);

    // Exactly one charge per finished connection, derived from its outcome
    // (service_ipc_connection_cost_tokens). Refused connections charge
    // nothing -- the refusal itself is the punishment, and refunding them
    // would let a flooder harvest tokens from its own refusals. A failed
    // response write supersedes whatever the branch outcome was.
    ServiceIpcConnectionOutcome connectionOutcome =
        SERVICE_IPC_CONNECTION_EXCHANGED;

    if ((identityKnown && magic != SERVICE_PROTOCOL_MAGIC) ||
        (identityKnown && magic == SERVICE_PROTOCOL_MAGIC &&
         version != SERVICE_PROTOCOL_VERSION)) {
        // Wrong magic/version: the declared body length cannot be trusted.
        // Drain the remainder in bounded chunks so the mismatch response can
        // actually be delivered (the old code silently timed out for 2s
        // against old-version clients), then answer with the existing
        // payload-free protocol-mismatch form.
        if (!prefix.messageComplete) {
            service_pipe_drain_inbound_message(pipe,
                SERVICE_PIPE_SERVER_DRAIN_TIMEOUT_MS, nullptr, 0);
        }
        response.status = SERVICE_STATUS_VERSION_MISMATCH;
        StringCchCopyA(response.message, ARRAY_COUNT(response.message),
            "Service protocol mismatch");
        debug_log("service_pipe_server: protocol mismatch header magic=0x%08x version=%u class=%s\n",
            magic, version, service_ipc_class_name(requestClass));
        connectionOutcome = SERVICE_IPC_CONNECTION_PROTOCOL_MISMATCH;
    } else if (!identityKnown ||
               requestClass == SERVICE_IPC_CLASS_UNKNOWN) {
        // Unknown commands are dropped before dispatch; unidentifiable
        // connections are dropped outright. The invalid-key charge routes to
        // the anonymous metering bucket.
        if (!prefix.messageComplete) {
            service_pipe_drain_inbound_message(pipe,
                SERVICE_PIPE_SERVER_DRAIN_TIMEOUT_MS, nullptr, 0);
        }
        response.status = SERVICE_STATUS_ERROR;
        StringCchCopyA(response.message, ARRAY_COUNT(response.message),
            "Unsupported service command");
        debug_log("service_pipe_server: dropping pre-dispatch connection identityKnown=%d command=%u\n",
            identityKnown ? 1 : 0, command);
        connectionOutcome = identityKnown
            ? SERVICE_IPC_CONNECTION_UNKNOWN_COMMAND
            : SERVICE_IPC_CONNECTION_IDENTITY_UNKNOWN;
    } else {
        ServiceIpcAdmissionDecision decision =
            service_admission_decide(caller.throttleKey, requestClass);
        if (decision != SERVICE_IPC_ADMITTED) {
            // Refuse with the generic payload-free error form. No charge:
            // the bucket state that produced the refusal is the punishment.
            if (!prefix.messageComplete) {
                service_pipe_drain_inbound_message(pipe,
                    SERVICE_PIPE_SERVER_DRAIN_TIMEOUT_MS, nullptr, 0);
            }
            response.status = SERVICE_STATUS_ERROR;
            StringCchCopyA(response.message, ARRAY_COUNT(response.message),
                "Service is busy handling other requests; retry shortly");
            char userToken[32] = {};
            gc_log_identifier_token(caller.user, userToken,
                                    sizeof(userToken));
            debug_log("service_pipe_server: admission refused decision=%s class=%s pid=%lu session=%lu user=%s\n",
                service_ipc_decision_name(decision),
                service_ipc_class_name(requestClass),
                (unsigned long)caller.pid,
                (unsigned long)caller.sessionId,
                userToken);
            connectionOutcome = SERVICE_IPC_CONNECTION_ADMISSION_REFUSED;
        } else {
            return service_finish_admitted_connection(pipe, &caller,
                &prefix, &response, requestClass);
        }
    }

    response.message[ARRAY_COUNT(response.message) - 1] = '\0';
    response.outcomeSeverity = service_response_resolve_outcome_severity(
        response.status, response.outcomeSeverity);
    if (!service_pipe_write_exact(pipe, &response, sizeof(response),
            SERVICE_PIPE_SERVER_IO_TIMEOUT_MS, "writing service response",
            pipeErr, sizeof(pipeErr))) {
        debug_log("service_pipe_server: refusal write failed: %s\n",
            pipeErr[0] ? pipeErr : "unknown");
        connectionOutcome = SERVICE_IPC_CONNECTION_TRANSPORT_FAULT;
    }
    service_admission_charge(caller.throttleKey, requestClass,
        service_ipc_connection_cost_tokens(connectionOutcome));
    DisconnectNamedPipe(pipe);
    return true;
}

