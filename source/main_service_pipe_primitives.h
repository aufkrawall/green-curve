// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

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
                    "operation started")) {
                debug_log("service operation: id=%llu could not persist in-progress correlation\n",
                    (unsigned long long)request_->operationId);
            }
            return;
        }
        if (begin == SERVICE_OPERATION_BEGIN_DUPLICATE && existing) {
            response_->operationState = existing->state;
            response_->status = existing->responseStatus;
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
        service_operation_complete(&g_serviceOperationTracker,
            request_->operationId, response_->status, response_->message);
        response_->operationState = response_->status == SERVICE_STATUS_OK
            ? SERVICE_OPERATION_SUCCEEDED : SERVICE_OPERATION_FAILED;
        if (!service_store_operation_record(request_->operationId,
                response_->operationState, response_->status,
                response_->message)) {
            debug_log("service operation: id=%llu could not persist completion\n",
                (unsigned long long)request_->operationId);
        }
        debug_log("service operation: id=%llu command=%s state=%s durationMs=%llu\n",
            (unsigned long long)request_->operationId,
            commandName_ ? commandName_ : "mutation",
            response_->status == SERVICE_STATUS_OK ? "succeeded" : "failed",
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
    // SYSTEM (full), Administrators (full), Authenticated Users (read+write).
    //
    // F-SEC-3 ("only the active interactive session may drive GPU OC/RESET") is
    // enforced SERVER-SIDE by service_caller_is_authorized(): the server
    // impersonates the exact connected client and duplicates its thread token;
    // PID is diagnostic correlation only. The token session is rejected unless
    // it equals the active interactive session. The pipe ACL only needs to admit
    // authenticated LOCAL users (PIPE_REJECT_REMOTE_CLIENTS blocks the network),
    // and must NOT bake a specific console-user SID into the ACL:
    //
    //   Root cause of "service not responding after switching accounts" — a
    //   per-user ACL grants write to whoever was the console user when the
    //   listening instance was created.  After a logoff/login (or fast-user-
    //   switch) the now-active user is a *different* SID, so their GUI is denied
    //   at CreateFile and the server's ConnectNamedPipe never completes — the
    //   stale-ACL instance stays listening and keeps rejecting the new user
    //   until the service is restarted (reboot).  A user-agnostic ACL lets the
    //   new active user connect immediately; the server-side check still rejects
    //   any non-active session with a clear message.
    const WCHAR* sddl = L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)";
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
