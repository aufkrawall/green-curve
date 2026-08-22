// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Included after the runtime-identity shard, which owns the active-session
// lookup this policy consumes.
#include "service_pipe_acl_policy.h"

static ServiceOperationTracker g_serviceOperationTracker = {};
static bool g_serviceOperationTrackerLoaded = false;

static void ensure_service_operation_tracker_loaded() {
    if (g_serviceOperationTrackerLoaded) return;
    g_serviceOperationTrackerLoaded = true;
    if (!service_load_operation_record(&g_serviceOperationTracker)) {
        debug_log("service operation: no valid persisted result was loaded\n");
    }
}

class ServiceOperationRequestGuard {
public:
    ServiceOperationRequestGuard(const ServiceRequest* request,
        ServiceResponse* response, const char* commandName)
        : request_(request), response_(response), commandName_(commandName),
          started_(false), execute_(false), startedAt_(GetTickCount64()) {
        if (!request_ || !response_) return;
        ensure_service_operation_tracker_loaded();
        response_->operationId = request_->operationId;
        const ServiceOperationRecord* existing = nullptr;
        ServiceOperationBeginResult begin = service_operation_begin(
            &g_serviceOperationTracker, request_->operationId, &existing);
        if (begin == SERVICE_OPERATION_BEGIN_STARTED) {
            started_ = true;
            execute_ = true;
            response_->operationState = SERVICE_OPERATION_IN_PROGRESS;
            debug_log("service operation: id=%llu command=%s state=in-progress\n",
                (unsigned long long)request_->operationId,
                commandName_ ? commandName_ : "mutation");
            if (!service_store_operation_record(request_->operationId,
                    SERVICE_OPERATION_IN_PROGRESS, SERVICE_STATUS_ERROR,
                    SERVICE_OUTCOME_SEVERITY_ERROR, "operation started")) {
                debug_log("service operation: id=%llu could not persist in-progress correlation\n",
                    (unsigned long long)request_->operationId);
            }
            return;
        }
        if (begin == SERVICE_OPERATION_BEGIN_DUPLICATE && existing) {
            response_->operationState = existing->state;
            response_->status = existing->responseStatus;
            // The replay is the same answer, severity included: a retry after a
            // lost response must not read as cleaner than the original.
            response_->outcomeSeverity = existing->outcomeSeverity;
            StringCchCopyA(response_->message, ARRAY_COUNT(response_->message),
                existing->message[0] ? existing->message :
                (existing->state == SERVICE_OPERATION_IN_PROGRESS
                    ? "Operation is still in progress" : "Operation result cached"));
            debug_log("service operation: id=%llu command=%s deduplicated state=%u\n",
                (unsigned long long)request_->operationId,
                commandName_ ? commandName_ : "mutation",
                (unsigned int)existing->state);
            return;
        }
        response_->status = SERVICE_STATUS_ERROR;
        response_->operationState = SERVICE_OPERATION_OUTCOME_UNKNOWN;
        StringCchCopyA(response_->message, ARRAY_COUNT(response_->message),
            "Mutation request is missing a valid operation ID");
    }

    ~ServiceOperationRequestGuard() {
        if (!started_ || !request_ || !response_) return;
        // Resolve here rather than only at the write-out stamp: this is what
        // gets PERSISTED, so the record a later retry replays has to hold the
        // same severity the live answer carries.  The resolver is idempotent,
        // so the write-out stamp still runs harmlessly over it.
        response_->outcomeSeverity = service_response_resolve_outcome_severity(
            response_->status, response_->outcomeSeverity);
        service_operation_complete(&g_serviceOperationTracker,
            request_->operationId, response_->status,
            response_->outcomeSeverity, response_->message);
        response_->operationState = response_->status == SERVICE_STATUS_OK
            ? SERVICE_OPERATION_SUCCEEDED : SERVICE_OPERATION_FAILED;
        if (!service_store_operation_record(request_->operationId,
                response_->operationState, response_->status,
                response_->outcomeSeverity, response_->message)) {
            debug_log("service operation: id=%llu could not persist completion\n",
                (unsigned long long)request_->operationId);
        }
        debug_log("service operation: id=%llu command=%s state=%s severity=%s durationMs=%llu\n",
            (unsigned long long)request_->operationId,
            commandName_ ? commandName_ : "mutation",
            response_->status == SERVICE_STATUS_OK ? "succeeded" : "failed",
            service_outcome_severity_name(response_->outcomeSeverity),
            (unsigned long long)(GetTickCount64() - startedAt_));
    }

    bool execute() const { return execute_; }

private:
    const ServiceRequest* request_;
    ServiceResponse* response_;
    const char* commandName_;
    bool started_;
    bool execute_;
    ULONGLONG startedAt_;
};

static bool create_restricted_pipe_security_descriptor(PSECURITY_DESCRIPTOR* outSd) {
    *outSd = nullptr;
    // SYSTEM/Administrators always retain full control.  For normal clients the
    // ACL is narrowed to the active session's exact user SID.  This shrinks
    // pre-authentication exposure: a process in another local account cannot
    // hold a listener instance while waiting for server-side authorization.
    //
    // The old stale-ACL failure is avoided by the existing auto-reset recycle
    // event, not by widening the DACL to every authenticated account.  Session
    // transitions signal that event; the next listener instance resolves the
    // newly active token.  If no active session can be resolved (for example at
    // boot), the fallback admits authenticated local users but the per-request
    // active-session check still refuses them.  PIPE_REJECT_REMOTE_CLIENTS
    // keeps this local-only.
    DWORD activeSessionId = (DWORD)-1;
    const bool activeSessionKnown =
        get_active_interactive_session_id(&activeSessionId);
    wchar_t activeSessionSid[128] = {};
    bool activeSidKnown = false;
    if (activeSessionKnown) {
        HANDLE sessionToken = nullptr;
        if (WTSQueryUserToken(activeSessionId, &sessionToken)) {
            DWORD returned = 0;
            GetTokenInformation(sessionToken, TokenUser, nullptr, 0, &returned);
            DWORD error = GetLastError();
            if (error == ERROR_INSUFFICIENT_BUFFER && returned > 0) {
                TOKEN_USER* user = (TOKEN_USER*)HeapAlloc(
                    GetProcessHeap(), HEAP_ZERO_MEMORY, returned);
                if (user && GetTokenInformation(sessionToken, TokenUser, user,
                            returned, &returned)) {
                    PWSTR sidText = nullptr;
                    if (ConvertSidToStringSidW(user->User.Sid, &sidText) &&
                        sidText) {
                        HRESULT copied = StringCchCopyW(
                            activeSessionSid, ARRAY_COUNT(activeSessionSid),
                            sidText);
                        activeSidKnown = SUCCEEDED(copied);
                        if (!activeSidKnown) {
                            debug_log("pipe_server: active-session SID text "
                                      "was too long; using boot fallback ACL\n");
                        }
                    }
                    if (sidText) LocalFree(sidText);
                }
                if (user) HeapFree(GetProcessHeap(), 0, user);
            }
            CloseHandle(sessionToken);
        }
        if (!activeSidKnown) {
            debug_log("pipe_server: active-session token/SID unavailable; "
                      "using authenticated-users fallback until next recycle\n");
        }
    }

    WCHAR sddl[SERVICE_PIPE_ACL_SDDL_CHARS] = {};
    if (!service_pipe_acl_sddl_for_session(
            activeSessionKnown && activeSidKnown,
            activeSidKnown ? activeSessionSid : nullptr,
            sddl, ARRAY_COUNT(sddl))) {
        debug_log("pipe_server: failed building active-session pipe SDDL\n");
        return false;
    }
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, outSd, nullptr) == FALSE) {
        debug_log("pipe_server: failed building pipe security descriptor (error %lu)\n", GetLastError());
        *outSd = nullptr;
        return false;
    }
    return true;
}

// Close a pipe handle owned by the pipe server thread, atomically clearing the
// shared g_servicePipeHandle slot first so the main-loop watchdog cannot also
// close it.  A double-close raises STATUS_INVALID_HANDLE (c0000008) under the
// Strict Handle Check mitigation, which is NOT an access violation so the VEH
// does not catch it — it reaches the unhandled filter and terminates the whole
// service process (GUI then sees ERROR_BROKEN_PIPE / error 109).  Exactly one
// of the pipe thread (here) or the watchdog wins the slot via the atomic CAS
// and performs the single close.  The pipe thread can only be VEH-terminated
// while executing NVML/NVAPI inside a command handler — never inside this
// function — so there is no leak window between the CAS and CloseHandle.
static void service_close_owned_pipe(HANDLE pipe) {
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) return;
    HANDLE prev = (HANDLE)InterlockedCompareExchangePointer(
        (PVOID volatile*)&g_servicePipeHandle, INVALID_HANDLE_VALUE, pipe);
    if (prev == pipe) {
        CloseHandle(pipe);
    }
    // else: the watchdog already reclaimed (and will close) this handle.
}

static HANDLE g_servicePipeReadyEvent = nullptr;
static volatile LONG g_servicePipeStartupError = ERROR_IO_PENDING;

// Reject cheaply before spending the full request deadline on a connection that
// no command could authorize.  The scheduled handoff may arrive before its
// session is WTS-active, so active-session state intentionally remains a
// per-command decision; medium integrity, however, is required by every command
// and can be checked without trusting any payload bytes.
static bool service_preauth_connection_is_admissible(HANDLE pipe) {
    char error[256] = {};
    char callerUser[256] = {};
    DWORD sessionId = (DWORD)-1;
    DWORD pid = 0;
    DWORD integrityRid = 0;
    ServiceLifecycleIdentity identity = {};
    if (!get_pipe_client_identity(pipe, callerUser, ARRAY_COUNT(callerUser),
            &sessionId, &pid, nullptr, &identity, &integrityRid, nullptr,
            error, sizeof(error))) {
        debug_log("service_pipe_server: pre-auth identity rejected pid=%lu: %s\n",
            (unsigned long)pid,
            error[0] ? error : "unknown");
        return false;
    }
    if (integrityRid < SECURITY_MANDATORY_MEDIUM_RID) {
        char userToken[32] = {};
        gc_log_identifier_token(callerUser, userToken, sizeof(userToken));
        debug_log("service auth reject: low-integrity pre-auth pid=%lu session=%lu "
                  "integrity=%lu user=%s\n",
            (unsigned long)pid, (unsigned long)sessionId,
            (unsigned long)integrityRid, userToken);
        return false;
    }
    return true;
}
