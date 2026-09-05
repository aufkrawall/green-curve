// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Native Windows named-pipe regression fixture for the transition-safe IPC
// transport. Built and executed by `python build.py --test` on win32 hosts
// only (the Linux fixtures are the cross-compiled counterparts).
//
// The 2026-08-22 live incident: moving ImpersonateNamedPipeClient() BEFORE
// the server's first pipe read failed every GUI request with error 1368
// (ERROR_CANNOT_IMPERSONATE), and an active-session SDDL broke scheduled
// logon handoffs. These tests pin the corrected invariants:
//
//   1. a message-mode partial read of the pinned 12-byte request header
//      completes with ERROR_MORE_DATA and IS a valid first read;
//   2. impersonation succeeds after that probe (the incident regression --
//      it fails on any reintroduced pre-read design);
//   3. the remaining body is readable exactly into the wire structure;
//   4. hostile tail bytes beyond the declared structure are drained without
//     delaying the refusal response;
//   5. one stalled connection does not prevent another from completing;
//   6. a timed-out probe ends cleanly (no crash, no hang, no leak);
//   7. the server disconnects only after the client consumed the response
//      (DisconnectNamedPipe discards unread buffered data, so an unsequenced
//      disconnect turns into a spurious client ERROR_BROKEN_PIPE), and the
//      client verifies the answer it received rather than ignoring it.
//
// Sequencing between server and client threads is event-driven (the server
// signals instance-ready and client-connected), so there are no blind sleeps;
// case 5's "healthy beats the stall" property is asserted by ORDERING
// (the healthy exchange completes strictly before the stalled probe's
// deadline fires) rather than by a wall-clock bound.
//
// Exit code 0 = pass; non-zero identifies the failing assertion.

#include <windows.h>
#include <strsafe.h>
#include <stdio.h>
#include <string.h>

#include "gpu_core.h"
#include "service_protocol.h"
#include "service_ipc_throttle_policy.h"
#include "service_pipe_prefix_read.h"

#ifndef ARRAY_COUNT
#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#endif

static const wchar_t* kFixturePipeName =
    L"\\\\.\\pipe\\greencurve-test-transition-safe";

// Header-probe deadline used by both the fixture server and main's ordering
// assertion, so the two can never drift apart.
enum {
    kFixtureProbeTimeoutMs = 1500,
    kFixtureMaxExtraBodyBytes = 64,
};

struct ClientContext {
    bool stall;           // connect but send nothing
    DWORD extraBodyBytes; // bytes appended beyond sizeof(ServiceRequest)
    // Signaled once this client is done with the response (read completed,
    // read failed, or the client never got that far), so the server never
    // disconnects while the answer is still buffered. Null for clients that
    // never read (the stalled case).
    HANDLE responseConsumedEvent;
};

// DisconnectNamedPipe discards everything still buffered in the pipe, so a
// server-side disconnect racing the client's ReadFile would surface as a
// spurious client ERROR_BROKEN_PIPE (exit 907, 2026-08-29 CI) instead of the
// fixture's answer. The server therefore waits for this notification before
// disconnecting; it stays bounded so even a dead client releases it.
static void notify_response_consumed(ClientContext* ctx) {
    if (ctx && ctx->responseConsumedEvent) SetEvent(ctx->responseConsumedEvent);
}

// CancelIoEx only requests cancellation. Keep the OVERLAPPED and event alive
// until Windows reports the operation complete, matching the production fix
// this fixture exercises.
static void fixture_cancel_and_join(HANDLE pipe, OVERLAPPED* ov) {
    if (!pipe || pipe == INVALID_HANDLE_VALUE || !ov) return;
    CancelIoEx(pipe, ov);
    DWORD ignored = 0;
    GetOverlappedResult(pipe, ov, &ignored, TRUE);
}

// Deadline-bounded exact I/O with cancellation joined on every timeout path --
// the same contract as the production service_pipe_io_exact() primitive.
static bool fixture_io_exact(HANDLE pipe, bool write, void* data,
        DWORD dataSize, DWORD timeoutMs) {
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;
    BOOL started = write ? WriteFile(pipe, data, dataSize, nullptr, &ov)
                         : ReadFile(pipe, data, dataSize, nullptr, &ov);
    DWORD err = started ? ERROR_SUCCESS : GetLastError();
    bool ok = false;
    if (started || err == ERROR_IO_PENDING || err == ERROR_MORE_DATA) {
        if (WaitForSingleObject(ov.hEvent, timeoutMs) == WAIT_OBJECT_0) {
            DWORD transferred = 0;
            BOOL result = GetOverlappedResult(pipe, &ov, &transferred, FALSE);
            DWORD completeErr = result ? ERROR_SUCCESS : GetLastError();
            ok = ((result || completeErr == ERROR_MORE_DATA) &&
                  transferred == dataSize);
        } else {
            fixture_cancel_and_join(pipe, &ov);
        }
    }
    CloseHandle(ov.hEvent);
    return ok;
}

// Client half of one exchange.
static DWORD WINAPI client_thread_proc(void* parameter) {
    ClientContext* ctx = (ClientContext*)parameter;
    // Bounded retry on transient connect states (instance not yet listening /
    // all instances busy), mirroring the production client. No blind sleeps:
    // WaitNamedPipeW blocks on a real kernel signal.
    HANDLE pipe = INVALID_HANDLE_VALUE;
    ULONGLONG deadlineMs = GetTickCount64() + 4000;
    while (true) {
        pipe = CreateFileW(kFixturePipeName, GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                           nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        DWORD err = GetLastError();
        if ((err != ERROR_PIPE_BUSY && err != ERROR_FILE_NOT_FOUND) ||
            !service_ipc_time_at_or_after(deadlineMs, GetTickCount64())) {
            return 900;
        }
        WaitNamedPipeW(kFixturePipeName, 250);
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
        CloseHandle(pipe);
        return 901;
    }

    OVERLAPPED ov = {};
    ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
        CloseHandle(pipe);
        return 902;
    }
    if (!ctx->stall) {
        ServiceRequest request = {};
        request.magic = SERVICE_PROTOCOL_MAGIC;
        request.version = SERVICE_PROTOCOL_VERSION;
        request.command = SERVICE_CMD_PING;
        StringCchCopyA(request.source, ARRAY_COUNT(request.source),
                       "transition-safe fixture");
        if (ctx->extraBodyBytes > kFixtureMaxExtraBodyBytes) {
            CloseHandle(ov.hEvent);
            CloseHandle(pipe);
            return 903;
        }
        // A message-mode pipe treats every WriteFile call as a separate
        // message. Keep the hostile tail in the SAME message as the request;
        // separate one-byte writes race the server's drain/response/disconnect
        // path and test scheduling rather than the production invariant.
        BYTE wire[sizeof(ServiceRequest) + kFixtureMaxExtraBodyBytes] = {};
        memcpy(wire, &request, sizeof(request));
        memset(wire + sizeof(request), 0xAB, ctx->extraBodyBytes);
        DWORD wireBytes = (DWORD)sizeof(request) + ctx->extraBodyBytes;
        BOOL started = WriteFile(pipe, wire, wireBytes, nullptr, &ov);
        DWORD err = started ? ERROR_SUCCESS : GetLastError();
        if (!started && err != ERROR_IO_PENDING) {
            CloseHandle(ov.hEvent);
            CloseHandle(pipe);
            return 904;
        }
        if (WaitForSingleObject(ov.hEvent, 4000) != WAIT_OBJECT_0) {
            fixture_cancel_and_join(pipe, &ov);
            CloseHandle(ov.hEvent);
            CloseHandle(pipe);
            return 905;
        }
        DWORD transferred = 0;
        if (!GetOverlappedResult(pipe, &ov, &transferred, FALSE) ||
            transferred != wireBytes) {
            CloseHandle(ov.hEvent);
            CloseHandle(pipe);
            return 906;
        }
    }
    // Read the response (the stalled case simply times out here).
    ServiceResponse response = {};
    ResetEvent(ov.hEvent);
    BOOL started = ReadFile(pipe, &response, sizeof(response), nullptr, &ov);
    DWORD err = started ? ERROR_SUCCESS : GetLastError();
    if (!started && err != ERROR_IO_PENDING) {
        // Log the concrete error: an immediate refusal is the signature of a
        // response that was discarded by a disconnect before it was read.
        fprintf(stderr,
                "pipe fixture client: response ReadFile refused immediately "
                "(GetLastError=%lu)\n", err);
        CloseHandle(ov.hEvent);
        CloseHandle(pipe);
        notify_response_consumed(ctx);
        return 907;
    }
    DWORD wait = WaitForSingleObject(ov.hEvent, 4000);
    // A completed-but-FAILED read also signals the event (e.g. ERROR_BROKEN_PIPE
    // after the server vanished); GetOverlappedResult is what tells the two
    // outcomes apart, so a broken or short answer can never pass silently.
    DWORD transferred = 0;
    BOOL readOk = wait == WAIT_OBJECT_0 &&
                  GetOverlappedResult(pipe, &ov, &transferred, FALSE);
    if (wait != WAIT_OBJECT_0) fixture_cancel_and_join(pipe, &ov);
    CloseHandle(ov.hEvent);
    CloseHandle(pipe);
    notify_response_consumed(ctx);
    if (wait != WAIT_OBJECT_0) return 908;
    if (ctx->stall) {
        // The stalled exchange expects NO response: any clean END of the
        // pending read -- in practice the ERROR_BROKEN_PIPE completion when
        // the server's probe times out and disconnects -- is the expected
        // terminal state, not a pong. Only the wait bound matters here.
        return 0;
    }
    if (!readOk || transferred != sizeof(response)) return 909;
    if (response.magic != SERVICE_PROTOCOL_MAGIC ||
        response.version != SERVICE_PROTOCOL_VERSION ||
        response.status != SERVICE_STATUS_OK) {
        fprintf(stderr,
                "pipe fixture client: response content wrong "
                "(magic=%u version=%u status=%u)\n",
                (unsigned)response.magic, (unsigned)response.version,
                (unsigned)response.status);
        return 910;
    }
    return 0;
}

struct ServerOutcome {
    // Only one listener may pass FILE_FLAG_FIRST_PIPE_INSTANCE, mirroring
    // the production rule.
    bool firstInstance;
    bool probeExpectedPrefix;
    bool impersonated;
    bool bodyReadExact;
    bool responded;
    bool probeTimedOut;
    // Signaled once the pipe INSTANCE exists (before ConnectNamedPipe), so
    // main can deterministically start clients instead of sleeping.
    HANDLE readyEvent;
    // Signaled once a CLIENT is fully connected, so main knows a stalled
    // exchange has actually been established before starting the next one.
    HANDLE connectedEvent;
    // Created by main for servers that answer: the client signals it once the
    // response is consumed, releasing the server's pre-disconnect wait.
    HANDLE responseConsumedEvent;
    // Timestamps for the ordering assertion: when the connection was
    // established and when the header probe finished (timeout or success).
    ULONGLONG connectedAtMs;
    ULONGLONG probeEndedMs;
};

static ServiceResponse make_fixture_response() {
    ServiceResponse response = {};
    response.magic = SERVICE_PROTOCOL_MAGIC;
    response.version = SERVICE_PROTOCOL_VERSION;
    response.status = SERVICE_STATUS_OK;
    StringCchCopyA(response.message, ARRAY_COUNT(response.message), "pong");
    return response;
}

// Server half of one exchange: accept one client, run the prefix-probe
// pipeline (probe -> impersonate -> remainder -> drain tail -> respond).
static DWORD WINAPI serve_one_connection(void* parameter) {
    ServerOutcome* outcome = (ServerOutcome*)parameter;
    HANDLE pipe = CreateNamedPipeW(
        kFixturePipeName,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
            (outcome->firstInstance ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        2,
        sizeof(ServiceResponse),
        sizeof(ServiceRequest),
        1000,
        nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return 800;
    if (outcome->readyEvent) SetEvent(outcome->readyEvent);

    OVERLAPPED connectOv = {};
    connectOv.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!connectOv.hEvent) {
        CloseHandle(pipe);
        return 801;
    }
    BOOL connected = ConnectNamedPipe(pipe, &connectOv);
    DWORD connectErr = connected ? ERROR_SUCCESS : GetLastError();
    if (!connected && connectErr == ERROR_IO_PENDING) {
        if (WaitForSingleObject(connectOv.hEvent, 4000) != WAIT_OBJECT_0) {
            fixture_cancel_and_join(pipe, &connectOv);
            CloseHandle(connectOv.hEvent);
            CloseHandle(pipe);
            return 802;
        }
    } else if (!connected && connectErr != ERROR_PIPE_CONNECTED) {
        CloseHandle(connectOv.hEvent);
        CloseHandle(pipe);
        return 803;
    }
    CloseHandle(connectOv.hEvent);

    outcome->connectedAtMs = GetTickCount64();
    if (outcome->connectedEvent) SetEvent(outcome->connectedEvent);

    char probeErr[256] = {};
    ServicePipePrefixRead prefix = {};
    BOOL probeOk = service_pipe_read_request_header(pipe, &prefix,
                                                    kFixtureProbeTimeoutMs,
                                                    probeErr,
                                                    sizeof(probeErr));
    outcome->probeEndedMs = GetTickCount64();
    if (!probeOk) {
        // Stalled-client case: a clean timeout IS the expected result here.
        outcome->probeTimedOut = true;
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        return 0;
    }
    outcome->probeExpectedPrefix = prefix.gotExpectedPrefix;

    // THE incident invariant: impersonation must succeed now, after the
    // mandatory first read, without the full body having arrived.
    if (ImpersonateNamedPipeClient(pipe)) {
        outcome->impersonated = true;
        if (!RevertToSelf()) {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            return 810;
        }
    } else {
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        return 811;
    }

    ServiceResponse response = make_fixture_response();

    if (prefix.messageComplete) {
        // Exactly-header-sized message: nothing further to read; still answer.
        outcome->bodyReadExact = true;
        outcome->responded =
            fixture_io_exact(pipe, true, &response, (DWORD)sizeof(response),
                             2000);
    } else {
        ServiceRequest request = {};
        memcpy(&request, prefix.bytes, SERVICE_REQUEST_HEADER_BYTES);
        if (!fixture_io_exact(
                pipe, false,
                reinterpret_cast<unsigned char*>(&request) +
                    SERVICE_REQUEST_HEADER_BYTES,
                (DWORD)sizeof(request) - SERVICE_REQUEST_HEADER_BYTES,
                2000)) {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            return 812;
        }
        outcome->bodyReadExact =
            request.magic == SERVICE_PROTOCOL_MAGIC &&
            request.version == SERVICE_PROTOCOL_VERSION;
        // Consume anything beyond the declared structure without letting a
        // flood delay the answer (and without waiting when nothing follows).
        service_pipe_drain_inbound_message(pipe, 1500, nullptr, 0);
        outcome->responded =
            fixture_io_exact(pipe, true, &response, (DWORD)sizeof(response),
                             2000);
        if (outcome->responded && outcome->responseConsumedEvent) {
            // Never disconnect a delivered-but-unread answer: the disconnect
            // would discard the buffered response and hand the client a
            // spurious ERROR_BROKEN_PIPE instead of the pong. Bounded so a
            // dead client still releases the server (the exchange has already
            // failed by the client's own exit code in that case).
            WaitForSingleObject(outcome->responseConsumedEvent, 4000);
        }
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    return 0;
}

int main() {
    // ---- Cases 1-4: full v20 round trip through the probe pipeline. ----
    {
        ServerOutcome outcome = {};
        outcome.firstInstance = true;
        outcome.readyEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!outcome.readyEvent) return 700;
        outcome.connectedEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!outcome.connectedEvent) return 700;
        outcome.responseConsumedEvent =
            CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!outcome.responseConsumedEvent) return 700;
        HANDLE serverThread =
            CreateThread(nullptr, 0, serve_one_connection, &outcome, 0,
                         nullptr);
        if (!serverThread) return 700;

        // Deterministic sequencing: start the client only after the pipe
        // INSTANCE exists (no sleep guesses). A client that slips in before
        // the server posts ConnectNamedPipe is handled via ERROR_PIPE_CONNECTED.
        if (WaitForSingleObject(outcome.readyEvent, 4000) != WAIT_OBJECT_0)
            return 730;

        ClientContext ctx = {};
        ctx.stall = false;
        ctx.extraBodyBytes = 7; // hostile tail beyond the declared structure
        ctx.responseConsumedEvent = outcome.responseConsumedEvent;
        HANDLE clientThread =
            CreateThread(nullptr, 0, client_thread_proc, &ctx, 0, nullptr);
        if (!clientThread) return 701;

        DWORD clientWait = WaitForSingleObject(clientThread, 10000);
        DWORD clientCode = 999;
        GetExitCodeThread(clientThread, &clientCode);
        CloseHandle(clientThread);
        DWORD serverWait = WaitForSingleObject(serverThread, 10000);
        DWORD serverCode = 999;
        GetExitCodeThread(serverThread, &serverCode);
        CloseHandle(serverThread);
        if (clientWait != WAIT_OBJECT_0 || serverWait != WAIT_OBJECT_0 ||
            clientCode != 0 || serverCode != 0) {
            fprintf(stderr,
                    "full exchange failed: client wait=%lu exit=%lu, "
                    "server wait=%lu exit=%lu\n",
                    clientWait, clientCode, serverWait, serverCode);
            return 702;
        }

        if (!outcome.probeExpectedPrefix) return 703; // ERROR_MORE_DATA probe
        if (!outcome.impersonated) return 704;        // THE incident check
        if (!outcome.bodyReadExact) return 705;
        if (!outcome.responded) return 706;
        CloseHandle(outcome.readyEvent);
        CloseHandle(outcome.connectedEvent);
        CloseHandle(outcome.responseConsumedEvent);
    }

    // ---- Cases 5+6: a stalled client must not block another connection,
    // and its eventual timeout must be clean. Sequencing is fully
    // event-driven: the stalled client is provably CONNECTED AND SILENT
    // before the healthy exchange even starts, and "healthy beats the
    // stall" is asserted by ordering against the stalled probe's own
    // deadline rather than by any wall-clock bound.
    {
        ServerOutcome stalledOutcome = {};
        stalledOutcome.firstInstance = true;
        stalledOutcome.readyEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!stalledOutcome.readyEvent) return 720;
        stalledOutcome.connectedEvent =
            CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!stalledOutcome.connectedEvent) return 720;
        HANDLE stalledServer =
            CreateThread(nullptr, 0, serve_one_connection, &stalledOutcome, 0,
                         nullptr);
        if (!stalledServer) return 720;
        if (WaitForSingleObject(stalledOutcome.readyEvent, 4000) !=
            WAIT_OBJECT_0)
            return 731;
        ClientContext stallCtx = {};
        stallCtx.stall = true;
        HANDLE stalledClient =
            CreateThread(nullptr, 0, client_thread_proc, &stallCtx, 0,
                         nullptr);
        if (!stalledClient) return 721;
        // The stalled client is now PROVEN connected and silent.
        if (WaitForSingleObject(stalledOutcome.connectedEvent, 4000) !=
            WAIT_OBJECT_0)
            return 732;

        // A second, healthy exchange proceeds WHILE the first probe pends.
        ServerOutcome healthyOutcome = {};
        healthyOutcome.firstInstance = false;
        healthyOutcome.readyEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!healthyOutcome.readyEvent) return 722;
        healthyOutcome.responseConsumedEvent =
            CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!healthyOutcome.responseConsumedEvent) return 722;
        HANDLE healthyServer =
            CreateThread(nullptr, 0, serve_one_connection, &healthyOutcome, 0,
                         nullptr);
        if (!healthyServer) return 722;
        if (WaitForSingleObject(healthyOutcome.readyEvent, 4000) !=
            WAIT_OBJECT_0)
            return 733;
        ClientContext healthyCtx = {};
        healthyCtx.responseConsumedEvent = healthyOutcome.responseConsumedEvent;
        HANDLE healthyClient =
            CreateThread(nullptr, 0, client_thread_proc, &healthyCtx, 0,
                         nullptr);
        if (!healthyClient) return 723;

        DWORD healthyWait = WaitForSingleObject(healthyClient, 10000);
        DWORD healthyCode = 998;
        GetExitCodeThread(healthyClient, &healthyCode);
        CloseHandle(healthyClient);
        ULONGLONG healthyDoneMs = GetTickCount64();
        DWORD healthyServerWait = WaitForSingleObject(healthyServer, 10000);
        DWORD healthyServerCode = 998;
        GetExitCodeThread(healthyServer, &healthyServerCode);
        CloseHandle(healthyServer);
        if (healthyWait != WAIT_OBJECT_0 ||
            healthyServerWait != WAIT_OBJECT_0 || healthyCode != 0 ||
            healthyServerCode != 0) {
            fprintf(stderr,
                    "healthy exchange failed: client wait=%lu exit=%lu, "
                    "server wait=%lu exit=%lu\n",
                    healthyWait, healthyCode, healthyServerWait,
                    healthyServerCode);
            return 724;
        }
        if (!healthyOutcome.responded || !healthyOutcome.impersonated)
            return 725;
        // The healthy exchange finished strictly BEFORE the stalled probe's
        // deadline could fire. The probe started when the stalled connection
        // was established and runs for kFixtureProbeTimeoutMs, so any global
        // serialization across the two instances (the incident shape) would
        // push healthyDone past connectedAt + that budget.
        if (healthyDoneMs >=
            stalledOutcome.connectedAtMs + (ULONGLONG)kFixtureProbeTimeoutMs)
            return 726;

        // The stalled probe ended with a clean header-timeout disconnect.
        DWORD stallClientWait = WaitForSingleObject(stalledClient, 10000);
        DWORD stallClientCode = 997;
        GetExitCodeThread(stalledClient, &stallClientCode);
        CloseHandle(stalledClient);
        DWORD stallServerWait = WaitForSingleObject(stalledServer, 10000);
        DWORD stallServerCode = 997;
        GetExitCodeThread(stalledServer, &stallServerCode);
        CloseHandle(stalledServer);
        if (stallClientWait != WAIT_OBJECT_0 ||
            stallServerWait != WAIT_OBJECT_0 || stallClientCode != 0 ||
            stallServerCode != 0) {
            fprintf(stderr,
                    "stalled exchange failed: client wait=%lu exit=%lu, "
                    "server wait=%lu exit=%lu\n",
                    stallClientWait, stallClientCode, stallServerWait,
                    stallServerCode);
            return 727;
        }
        if (stalledOutcome.probeExpectedPrefix || !stalledOutcome.probeTimedOut)
            return 727;
        CloseHandle(stalledOutcome.readyEvent);
        CloseHandle(stalledOutcome.connectedEvent);
        CloseHandle(healthyOutcome.readyEvent);
        CloseHandle(healthyOutcome.responseConsumedEvent);
    }

    printf("windows pipe regression passed\n");
    return 0;
}
