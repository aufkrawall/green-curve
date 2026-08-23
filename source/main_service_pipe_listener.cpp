// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Bounded multi-instance named-pipe listener.
//
// The historical design served ONE connection at a time on a single thread,
// so any authenticated local process could connect and stall -- sending
// nothing, or dribbling bytes -- and make every GUI request queue behind its
// full read timeout. This shard replaces that loop with a FIXED pool of
// transport workers (no unbounded thread creation), each owning one pipe
// instance end to end. Command execution itself stays serialized inside
// service_execute_checked_request(); only transport I/O overlaps.
//
// Handle discipline mirrors the proven single-slot scheme it replaces: every
// worker atomically publishes its current instance handle into a fixed slot,
// unpublishes before closing, and the host watchdog reclaims the slot of a
// worker that died mid-NVML (Strict Handle Checking makes double-close fatal,
// so exactly one side ever wins the CAS-close).
//
// The retired pre-read experiment's wake/recycle events are gone: the broad
// transition-safe ACL is never rebuilt, and the stop event alone wakes
// listeners.

// main_service_pipe_primitives.h (ACL builder, readiness globals) was already
// included by main_service_pipe.cpp earlier in this aggregate; this shard has
// no include guard and must not be included twice.

enum {
    // Transport concurrency bound. Well above any real GUI/CLI overlap (GUI
    // polls serially; CLI bursts briefly), far below any resource concern:
    // six threads x ~8.4 KB request/response wire structs.
    SERVICE_PIPE_WORKER_COUNT = 6,
};

// Worker stack reservation, in bytes (matches the historical pipe thread).
static const SIZE_T kWorkerStackBytes = 1024ULL * 1024ULL;

static HANDLE g_servicePipeWorkerThreads[SERVICE_PIPE_WORKER_COUNT] = {};
static HANDLE volatile g_servicePipeWorkerPipes[SERVICE_PIPE_WORKER_COUNT] = {};
static volatile LONG g_servicePipeFirstInstanceClaimed = 0;
static volatile LONG g_servicePipeReadinessPublished = 0;
static HANDLE g_serviceDeviceNotifyHandle = nullptr;

// The primary worker doubles as the startup sentinel: the host waits on its
// readiness event OR this thread's death before deciding startup outcome.
HANDLE gc_pipe_listener_primary_thread() {
    return g_servicePipeWorkerThreads[0];
}

namespace gc_pipe_listener {

void publish_worker_pipe(int index, HANDLE pipe) {
    InterlockedExchangePointer(
        (PVOID volatile*)&g_servicePipeWorkerPipes[index], pipe);
}
// Unpublish-and-close owned solely by whichever side holds the slot value:
// the worker itself, or the reaper that reclaimed it after thread death.
void retire_worker_pipe(int index) {
    HANDLE orphan = (HANDLE)InterlockedExchangePointer(
        (PVOID volatile*)&g_servicePipeWorkerPipes[index],
        INVALID_HANDLE_VALUE);
    if (orphan != INVALID_HANDLE_VALUE && orphan != nullptr) {
        DisconnectNamedPipe(orphan);
        CloseHandle(orphan);
    }
}

// Publish listener readiness/failure exactly like the historical single
// thread: the first successfully listening instance opens the gate.
void publish_readiness(DWORD error) {
    if (InterlockedExchange(&g_servicePipeReadinessPublished, 1) == 0) {
        InterlockedExchange(&g_servicePipeStartupError, (LONG)error);
        if (g_servicePipeReadyEvent) SetEvent(g_servicePipeReadyEvent);
    }
}

// Create one server pipe instance. The first-pipe-instance claim flag (see
// below) is used exactly once per process.
// exactly once per process so a foreign squatting listener still fails
// startup loudly, while later instances (and respawns) attach regardless.
HANDLE create_pipe_instance(const WCHAR* pipeName) {
    PSECURITY_DESCRIPTOR securityDescriptor = nullptr;
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    if (!create_restricted_pipe_security_descriptor(&securityDescriptor)) {
        debug_log("pipe_server: cannot create restricted ACL, failing listener closed\n");
        return INVALID_HANDLE_VALUE;
    }
    if (!securityDescriptor) {
        debug_log("pipe_server: restricted ACL creation returned no descriptor, failing listener closed\n");
        return INVALID_HANDLE_VALUE;
    }
    sa.lpSecurityDescriptor = securityDescriptor;

    bool claimFirst =
        InterlockedCompareExchange(&g_servicePipeFirstInstanceClaimed, 1,
                                   0) == 0;
    HANDLE pipe = CreateNamedPipeW(
        pipeName,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
            (claimFirst ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        SERVICE_PIPE_WORKER_COUNT,
        sizeof(ServiceResponse),
        sizeof(ServiceRequest),
        1000,
        sa.lpSecurityDescriptor ? &sa : nullptr);
    if (securityDescriptor) {
        LocalFree(securityDescriptor);
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        DWORD pipeError = GetLastError();
        debug_log("pipe_server: CreateNamedPipe failed (error=%lu first=%d)\n",
            pipeError, claimFirst ? 1 : 0);
        if (claimFirst) {
            // Only a first-instance failure is fatal at startup: it means
            // someone else owns our pipe name or our ACL/name setup broke.
            publish_readiness(pipeError);
        }
        return INVALID_HANDLE_VALUE;
    }
    return pipe;
}

} // namespace gc_pipe_listener

// One transport worker: create instance -> listen -> serve connection ->
// repeat on the SAME handle (DisconnectNamedPipe + fresh ConnectNamedPipe),
// forever, until the stop event fires or an unrecoverable setup error occurs
// (then the thread exits and the watchdog respawns it).
static DWORD WINAPI service_pipe_worker_thread_proc(void* parameter) {
    const int index = (int)(intptr_t)parameter;
    WCHAR pipeName[128] = {};
    if (!background_service_pipe_name(pipeName, ARRAY_COUNT(pipeName))) return 1;

    HANDLE pipe = gc_pipe_listener::create_pipe_instance(pipeName);
    if (pipe == INVALID_HANDLE_VALUE) {
        // A claimed-first failure already published its fatal startup error;
        // any other instance failure is non-fatal (the watchdog respawns this
        // slot and other workers keep the name alive).
        return 1;
    }

    OVERLAPPED ov = {};
    ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
        CloseHandle(pipe);
        gc_pipe_listener::publish_readiness(GetLastError());
        return 1;
    }

    // This instance is now accepting connections; open the startup gate.
    // Idempotent: exactly the first publication wins.
    gc_pipe_listener::publish_readiness(ERROR_SUCCESS);

    gc_pipe_listener::publish_worker_pipe(index, pipe);

    while (true) {
        ResetEvent(ov.hEvent);
        BOOL connected = ConnectNamedPipe(pipe, &ov);
        DWORD connectErr = connected ? ERROR_SUCCESS : GetLastError();
        if (!connected && connectErr == ERROR_IO_PENDING) {
            // Stop event precedes ov.hEvent in the wait order.
            HANDLE waitHandles[2] = {};
            waitHandles[0] = g_serviceStopEvent ? g_serviceStopEvent
                                                : ov.hEvent;
            waitHandles[1] = ov.hEvent;
            DWORD stopIdx = g_serviceStopEvent ? 0 : (DWORD)-1;
            DWORD waitResult = WaitForMultipleObjects(
                g_serviceStopEvent ? 2 : 1, waitHandles, FALSE, INFINITE);
            if (stopIdx != (DWORD)-1 &&
                waitResult == WAIT_OBJECT_0 + stopIdx) {
                CancelIoEx(pipe, &ov);
                break;
            }
            connected = waitResult ==
                WAIT_OBJECT_0 + (g_serviceStopEvent ? 1 : 0);
            if (!connected) {
                debug_log("pipe_server: worker %d connect wait failed (error %lu)\n",
                    index, GetLastError());
                break;
            }
        } else if (!connected && connectErr == ERROR_PIPE_CONNECTED) {
            // A client slipped in between CreateNamedPipe and ConnectNamedPipe:
            // the connection is already established, so fall through and serve
            // it. (No assignment needed here -- nothing reads `connected`.)
        } else if (!connected) {
            debug_log("pipe_server: worker %d ConnectNamedPipe failed (error %lu)\n",
                index, connectErr);
            break;
        }

        // Serve exactly one connection end to end (probe -> identity ->
        // admission -> body -> serialized dispatch -> response). The helper
        // always leaves the instance disconnected afterwards.
        service_serve_pipe_connection(pipe);
    }

    CancelIoEx(pipe, &ov);
    CloseHandle(ov.hEvent);
    DisconnectNamedPipe(pipe);
    gc_pipe_listener::retire_worker_pipe(index);
    return 0;
}

// Start the listener pool. Returns false only when the primary worker thread
// could not be created at all (the host treats that as fatal startup, exactly
// like the historical single-thread CreateThread failure).
static bool service_pipe_listener_start() {
    gc_pipe_dispatch::ensure_transport_locks();
    DWORD threadId = 0;
    g_servicePipeWorkerThreads[0] = CreateThread(nullptr, kWorkerStackBytes,
        service_pipe_worker_thread_proc, (void*)(intptr_t)0,
        STACK_SIZE_PARAM_IS_A_RESERVATION, &threadId);
    if (!g_servicePipeWorkerThreads[0]) {
        debug_log("service_main: FATAL failed to create primary pipe worker (error %lu)\n",
            GetLastError());
        return false;
    }
    // Additional capacity workers are best-effort at startup: a transient
    // failure here reduces parallelism until the watchdog respawns the slot.
    for (int i = 1; i < SERVICE_PIPE_WORKER_COUNT; ++i) {
        g_servicePipeWorkerThreads[i] = CreateThread(nullptr, kWorkerStackBytes,
            service_pipe_worker_thread_proc, (void*)(intptr_t)i,
            STACK_SIZE_PARAM_IS_A_RESERVATION, &threadId);
        if (!g_servicePipeWorkerThreads[i]) {
            debug_log("service_main: pipe worker %d deferred (error %lu); watchdog will respawn\n",
                i, GetLastError());
        }
    }
    return true;
}

// Host watchdog tick: reclaim and respawn workers whose threads died (VEH
// kills a thread stuck in NVML on a transitional driver). Same CAS-reclaim
// contract as the historical single g_servicePipeHandle slot, generalized to
// the fixed slot array.
static void service_pipe_listener_reap_and_respawn() {
    for (int i = 0; i < SERVICE_PIPE_WORKER_COUNT; ++i) {
        HANDLE thread = g_servicePipeWorkerThreads[i];
        if (!thread) continue;
        DWORD wait = WaitForSingleObject(thread, 0);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_FAILED) continue;
        gc_pipe_listener::retire_worker_pipe(i);
        CloseHandle(thread);
        g_servicePipeWorkerThreads[i] = nullptr;
        DWORD threadId = 0;
        g_servicePipeWorkerThreads[i] = CreateThread(nullptr, kWorkerStackBytes,
            service_pipe_worker_thread_proc, (void*)(intptr_t)i,
            STACK_SIZE_PARAM_IS_A_RESERVATION, &threadId);
        debug_log("service_main: pipe worker %d died; respawned=%d\n",
            i, g_servicePipeWorkerThreads[i] ? 1 : 0);
    }
}

// Graceful shutdown: sweep any already-dead workers' slots without respawning,
// then join every live worker (each wakes from its connect wait via the stop
// event, or finishes its deadline-bounded current connection first).
static void service_pipe_listener_stop_and_join() {
    for (int i = 0; i < SERVICE_PIPE_WORKER_COUNT; ++i) {
        HANDLE thread = g_servicePipeWorkerThreads[i];
        if (!thread) continue;
        DWORD exited = WaitForSingleObject(thread, 0);
        if (exited == WAIT_OBJECT_0 || exited == WAIT_FAILED) {
            gc_pipe_listener::retire_worker_pipe(i);
            CloseHandle(thread);
            g_servicePipeWorkerThreads[i] = nullptr;
        }
    }
    for (int i = 0; i < SERVICE_PIPE_WORKER_COUNT; ++i) {
        HANDLE thread = g_servicePipeWorkerThreads[i];
        if (!thread) continue;
        WaitForSingleObject(thread, INFINITE);
        gc_pipe_listener::retire_worker_pipe(i);
        CloseHandle(thread);
        g_servicePipeWorkerThreads[i] = nullptr;
    }
}
