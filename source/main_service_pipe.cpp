// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Authenticated named-pipe listener and command dispatch.

#include "main_service_pipe_primitives.h"

static DWORD WINAPI service_pipe_server_thread_proc(void*) {
    WCHAR pipeName[128] = {};
    if (!background_service_pipe_name(pipeName, ARRAY_COUNT(pipeName))) return 1;

    while (!g_serviceStopEvent || WaitForSingleObject(g_serviceStopEvent, 0) != WAIT_OBJECT_0) {
        PSECURITY_DESCRIPTOR securityDescriptor = nullptr;
        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(sa);
        if (create_restricted_pipe_security_descriptor(&securityDescriptor)) {
            sa.lpSecurityDescriptor = securityDescriptor;
        } else {
            debug_log("pipe_server: cannot create restricted ACL, failing listener closed\n");
            if (securityDescriptor) {
                LocalFree(securityDescriptor);
                securityDescriptor = nullptr;
            }
            InterlockedExchange(&g_servicePipeStartupError, ERROR_INVALID_SECURITY_DESCR);
            if (g_servicePipeReadyEvent) SetEvent(g_servicePipeReadyEvent);
            return 1;
        }
        if (!securityDescriptor) {
            debug_log("pipe_server: restricted ACL creation returned no descriptor, failing listener closed\n");
            InterlockedExchange(&g_servicePipeStartupError, ERROR_INVALID_SECURITY_DESCR);
            if (g_servicePipeReadyEvent) SetEvent(g_servicePipeReadyEvent);
            return 1;
        }

        HANDLE pipe = CreateNamedPipeW(
            pipeName,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            4,
            sizeof(ServiceResponse),
            sizeof(ServiceRequest),
            1000,
            sa.lpSecurityDescriptor ? &sa : nullptr);
        if (securityDescriptor) {
            LocalFree(securityDescriptor);
            securityDescriptor = nullptr;
        }
        if (pipe == INVALID_HANDLE_VALUE) {
            DWORD pipeError = GetLastError();
            debug_log("pipe_server: CreateNamedPipe failed (error=%lu)\n", pipeError);
            InterlockedExchange(&g_servicePipeStartupError, (LONG)pipeError);
            if (g_servicePipeReadyEvent) SetEvent(g_servicePipeReadyEvent);
            return 1;
        }
        // Publish the pipe handle so the main-loop watchdog can reclaim it if
        // the VEH terminates this thread inside a command handler.  The slot
        // is INVALID here (every close path clears it via service_close_owned_pipe
        // / the watchdog), so this is a clean publish — we must NOT close the
        // previous value (it was the just-freed handle from the prior iteration;
        // closing it double-closes and the Strict Handle Check mitigation turns
        // that into a process-killing STATUS_INVALID_HANDLE).
        InterlockedExchangePointer((PVOID volatile*)&g_servicePipeHandle, pipe);

        OVERLAPPED ov = {};
        ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) {
            service_close_owned_pipe(pipe);
            continue;
        }
        BOOL connected = ConnectNamedPipe(pipe, &ov);
        DWORD connectErr = connected ? ERROR_SUCCESS : GetLastError();
        if (connected || connectErr == ERROR_IO_PENDING ||
            connectErr == ERROR_PIPE_CONNECTED) {
            InterlockedExchange(&g_servicePipeStartupError, ERROR_SUCCESS);
            if (g_servicePipeReadyEvent) SetEvent(g_servicePipeReadyEvent);
        }
        if (!connected && connectErr == ERROR_IO_PENDING) {
            // Build the wait set from the events that actually exist.  Index 0
            // is always the stop event (breaks the loop).  The pipe-wake and
            // pipe-recycle events recycle the current instance (close + create a
            // fresh one).  The recycle event is signaled by
            // service_handle_session_change() on an active-user change so the
            // next connect gets a clean instance (the ACL itself is user-agnostic;
            // active-session enforcement is server-side in
            // service_caller_is_authorized).
            HANDLE waitHandles[4] = {};
            DWORD waitCount = 0;
            DWORD stopIdx = (DWORD)-1, wakeIdx = (DWORD)-1, recycleIdx = (DWORD)-1;
            if (g_serviceStopEvent) { stopIdx = waitCount; waitHandles[waitCount++] = g_serviceStopEvent; }
            if (g_servicePipeWakeEvent) { wakeIdx = waitCount; waitHandles[waitCount++] = g_servicePipeWakeEvent; }
            if (g_servicePipeRecycleEvent) { recycleIdx = waitCount; waitHandles[waitCount++] = g_servicePipeRecycleEvent; }
            waitHandles[waitCount++] = ov.hEvent; // always last
            DWORD ovIdx = waitCount - 1;
            DWORD waitResult = WaitForMultipleObjects(waitCount, waitHandles, FALSE, INFINITE);
            if (stopIdx != (DWORD)-1 && waitResult == WAIT_OBJECT_0 + stopIdx) {
                CancelIoEx(pipe, &ov);
                CloseHandle(ov.hEvent);
                DisconnectNamedPipe(pipe);
                service_close_owned_pipe(pipe);
                break;
            }
            auto recycle_current_pipe = [&]() {
                CancelIoEx(pipe, &ov);
                CloseHandle(ov.hEvent);
                DisconnectNamedPipe(pipe);
                service_close_owned_pipe(pipe);
            };
            if (wakeIdx != (DWORD)-1 && waitResult == WAIT_OBJECT_0 + wakeIdx) {
                recycle_current_pipe();
                continue;
            }
            if (recycleIdx != (DWORD)-1 && waitResult == WAIT_OBJECT_0 + recycleIdx) {
                debug_log("pipe_server: recycling instance for ACL rebuild after session change\n");
                recycle_current_pipe();
                continue;
            }
            connected = waitResult == WAIT_OBJECT_0 + ovIdx;
        } else if (!connected && connectErr == ERROR_PIPE_CONNECTED) {
            connected = TRUE;
        }
        CloseHandle(ov.hEvent);
        if (!connected) {
            DisconnectNamedPipe(pipe);
            service_close_owned_pipe(pipe);
            continue;
        }

        ServiceRequest request = {};
        ServiceResponse response = {};
        response.magic = SERVICE_PROTOCOL_MAGIC;
        response.version = SERVICE_PROTOCOL_VERSION;
        response.serviceBuildNumber = (DWORD)APP_BUILD_NUMBER;
        StringCchCopyA(response.serviceVersion, ARRAY_COUNT(response.serviceVersion), APP_VERSION);
        char callerUser[256] = {};
        DWORD callerSessionId = (DWORD)-1;
        DWORD callerPid = 0;
        DWORD callerIntegrityRid = 0;
        HANDLE callerToken = nullptr;
        bool callerIsAdmin = false;
        ServiceLifecycleIdentity callerLifecycleIdentity = {};
        char pipeErr[256] = {};
        if (!service_pipe_read_exact(pipe, &request, sizeof(request), SERVICE_PIPE_SERVER_IO_TIMEOUT_MS, "reading service request", pipeErr, sizeof(pipeErr))) {
            debug_log("service_pipe_server: dropping stalled or invalid client read: %s\n", pipeErr[0] ? pipeErr : "unknown");
            DisconnectNamedPipe(pipe);
            service_close_owned_pipe(pipe);
            continue;
        }
        if (request.magic != SERVICE_PROTOCOL_MAGIC || request.version != SERVICE_PROTOCOL_VERSION) {
            response.status = SERVICE_STATUS_VERSION_MISMATCH;
            StringCchCopyA(response.message, ARRAY_COUNT(response.message), "Service protocol mismatch");
        } else if (!validate_service_request_for_ipc(&request)) {
            response.status = SERVICE_STATUS_ERROR;
            StringCchCopyA(response.message, ARRAY_COUNT(response.message),
                "Service request contains invalid protocol fields");
            debug_log("service_pipe_server: rejected malformed v11 request command=%u pid=%u\n",
                request.command, request.callerPid);
        } else if (InterlockedExchangeAdd(
                &g_serviceClientRequestsReady, 0) == 0) {
            response.status = SERVICE_STATUS_ERROR;
            StringCchCopyA(response.message, ARRAY_COUNT(response.message),
                "Background service startup is not complete; retry after SCM reports RUNNING");
            debug_log("service_pipe_server: request rejected while lifecycle startup gate is closed\n");
        } else {
            bool settingsFreeHandoff =
                request.command == SERVICE_CMD_LOGON_HANDOFF;
            if (!service_caller_is_authorized(pipe, request.source,
                    !settingsFreeHandoff, response.message,
                    ARRAY_COUNT(response.message), callerUser,
                    sizeof(callerUser), &callerSessionId, &callerPid,
                    &callerIsAdmin, &callerLifecycleIdentity,
                    &callerIntegrityRid, &callerToken)) {
                response.status = SERVICE_STATUS_ERROR;
            } else if (request.callerPid != callerPid) {
                response.status = SERVICE_STATUS_ERROR;
                StringCchCopyA(response.message, ARRAY_COUNT(response.message),
                    "Service request caller PID does not match the connected client");
                debug_log("service auth reject: suppliedPid=%lu connectedPid=%lu command=%u\n",
                    (unsigned long)request.callerPid,
                    (unsigned long)callerPid,
                    (unsigned int)request.command);
            } else if ((request.command == SERVICE_CMD_APPLY ||
                        request.command == SERVICE_CMD_RESET ||
                        request.command == SERVICE_CMD_WRITE_LOG_SNAPSHOT ||
                        request.command == SERVICE_CMD_WRITE_JSON_SNAPSHOT ||
                        request.command == SERVICE_CMD_WRITE_PROBE_REPORT ||
                        request.command == SERVICE_CMD_LOGON_HANDOFF) &&
                       callerIntegrityRid < SECURITY_MANDATORY_MEDIUM_RID) {
                response.status = SERVICE_STATUS_ERROR;
                StringCchCopyA(response.message, ARRAY_COUNT(response.message),
                    "Service control requires a medium-integrity client");
                debug_log("service auth reject: low-integrity caller pid=%lu session=%lu command=%u integrityRid=%lu\n",
                    (unsigned long)callerPid,
                    (unsigned long)callerSessionId,
                    (unsigned int)request.command,
                    (unsigned long)callerIntegrityRid);
            } else {
                // A logon handoff is settings-free and resolves an immutable
                // per-session context in the lifecycle worker.  Do not mutate
                // process-global user/config paths as part of its authorization.
                if (request.command != SERVICE_CMD_LOGON_HANDOFF) {
                    char userPathErr[256] = {};
                    if (resolve_service_user_data_paths(callerSessionId, userPathErr, sizeof(userPathErr))) {
                        if (!g_app.configPath[0]) {
                            set_default_config_path();
                        }
                        refresh_service_debug_logging_from_config();
                    } else {
                        debug_log("service_pipe_server: failed to resolve user data paths: %s\n", userPathErr);
                    }
                }
            service_set_pending_operation_source(request.source[0] ? request.source : "service request");
            switch (request.command) {
                case SERVICE_CMD_PING:
                    response.status = SERVICE_STATUS_OK;
                    StringCchCopyA(response.message, ARRAY_COUNT(response.message), "pong");
                    break;
                case SERVICE_CMD_LOGON_HANDOFF:
                    if (request.applyOrigin != SERVICE_APPLY_ORIGIN_LOGON) {
                        response.status = SERVICE_STATUS_ERROR;
                        StringCchCopyA(response.message, ARRAY_COUNT(response.message),
                            "Invalid logon handoff origin");
                    } else if (!service_lifecycle_post_logon_handoff(
                                   &callerLifecycleIdentity)) {
                        response.status = SERVICE_STATUS_ERROR;
                        StringCchCopyA(response.message, ARRAY_COUNT(response.message),
                            "Lifecycle coordinator is not accepting logon handoffs");
                    } else {
                        response.status = SERVICE_STATUS_OK;
                        StringCchPrintfA(response.message, ARRAY_COUNT(response.message),
                            "Logon handoff accepted for session %lu",
                            (unsigned long)callerSessionId);
                        debug_log("service logon handoff: authenticated caller pid=%lu session=%lu user=%s; profile/settings payload ignored\n",
                            (unsigned long)callerPid, (unsigned long)callerSessionId,
                            callerUser[0] ? callerUser : "<unknown>");
                    }
                    populate_service_snapshot(&response.snapshot);
                    break;
                case SERVICE_CMD_GET_SNAPSHOT: {
                    service_handle_snapshot_request(&response);
                    break;
                }
                case SERVICE_CMD_GET_TELEMETRY: {
                    char detail[256] = {};
                    bool lockAcquired = try_lock_service_runtime(250);
                    if (!lockAcquired) {
                        debug_log("service telemetry: runtime lock busy (recovery reapply in progress), serving cached telemetry\n");
                        response.status = SERVICE_STATUS_OK;
                        StringCchCopyA(response.message, ARRAY_COUNT(response.message), "telemetry cached");
                        populate_service_snapshot(&response.snapshot);
                        if (g_serviceControlStateValid) response.controlState = g_serviceControlState;
                        break;
                    }
                    bool telemetryReady = service_refresh_telemetry_for_request(
                        detail, sizeof(detail));
                    if (!telemetryReady) {
                        debug_log("service telemetry: hardware initialize unavailable: %s\n", detail[0] ? detail : "unknown");
                    }
                    response.status = SERVICE_STATUS_OK;
                    StringCchCopyA(response.message, ARRAY_COUNT(response.message), detail[0] ? detail : "telemetry ready");
                    populate_service_snapshot(&response.snapshot);
                    if (g_serviceControlStateValid) response.controlState = g_serviceControlState;
                    unlock_service_runtime();
                    // Routine telemetry is a cached observation, not lifecycle
                    // readiness authority. The bootstrap hardware probe above
                    // is reached only before the first initialized snapshot;
                    // normal snapshot/PnP/config events own prerequisite wakes.
                    break;
                }
                case SERVICE_CMD_APPLY: {
                    ServiceOperationRequestGuard operation(&request, &response,
                        "apply");
                    if (!operation.execute()) {
                        populate_service_snapshot(&response.snapshot);
                        if (g_serviceHasActiveDesired) response.desired = g_serviceActiveDesired;
                        if (g_serviceControlStateValid) response.controlState = g_serviceControlState;
                        break;
                    }
                    char result[512] = {};
                    if (!service_mutation_preconditions_match(
                            &request, result, sizeof(result))) {
                        response.status = SERVICE_STATUS_STALE_STATE;
                        StringCchCopyA(response.message,
                            ARRAY_COUNT(response.message), result);
                        debug_log("service APPLY rejected by reconnect precondition: operation=%llu instance=%llu gpuGeneration=%llu topology=%llu reason=%s\n",
                            (unsigned long long)request.operationId,
                            (unsigned long long)request.expectedServiceInstanceId,
                            (unsigned long long)request.expectedGpuGeneration,
                            (unsigned long long)request.expectedTopologySignature,
                            result[0] ? result : "unknown");
                        break;
                    }
                    bool enforcePublishedGpuBinding = false;
                    ConfiguredGpuSelection publishedGpuBinding = {};
                    if (!service_request_apply_origin_valid(&request)) {
                        response.status = SERVICE_STATUS_ERROR;
                        StringCchCopyA(response.message, ARRAY_COUNT(response.message),
                            "Apply request is missing a valid typed origin");
                        debug_log("service APPLY rejected: invalid origin=%u source=%s\n",
                            request.applyOrigin,
                            request.source[0] ? request.source : "<none>");
                        break;
                    }
                    ServiceApplyOrigin applyOrigin =
                        (ServiceApplyOrigin)request.applyOrigin;
                    bool explicitUserApply =
                        service_apply_origin_is_explicit(applyOrigin);
                    if (!explicitUserApply) {
                        DWORD lockoutReason = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
                        if (service_auto_restore_is_locked_out(&lockoutReason)) {
                            response.status = SERVICE_STATUS_ERROR;
                            StringCchPrintfA(response.message,
                                ARRAY_COUNT(response.message),
                                "Automatic apply is disabled: %s",
                                service_auto_restore_lockout_reason_name(
                                    lockoutReason));
                            debug_log("service APPLY rejected: automatic origin=%u honors sticky lockout=%s\n",
                                (unsigned int)applyOrigin,
                                service_auto_restore_lockout_reason_name(
                                    lockoutReason));
                            break;
                        }
                    }
                    // A restricted caller may apply only an authoritative
                    // machine-bank profile, including its published GPU target.
                    // This check precedes the runtime lock, so rejection performs
                    // no hardware work and requires no unlock.
                    if (!service_apply_shared_only_policy(&request,
                            callerIsAdmin, callerUser,
                            &enforcePublishedGpuBinding,
                            &publishedGpuBinding, result, sizeof(result))) {
                        response.status = SERVICE_STATUS_ERROR;
                        StringCchCopyA(response.message,
                            ARRAY_COUNT(response.message), result);
                        break;
                    }
                    // Reject apply while recovering from a GPU device reconnect:
                    // the NVML/NVAPI writes would access-violate on the still-
                    // transitional driver and kill this pipe server thread
                    // (GUI sees ERROR_BROKEN_PIPE).  The fan runtime thread
                    // auto-reapplies the active profile once the driver settles.
                    //
                    // Allow apply if GPU data is already loaded (g_app.loaded
                    // is true) — the crash window was restored by recovery as a
                    // safety measure, but the handles are fresh and valid.
                    // RC7: block ALL GUI applies during crash recovery, even if
                    // g_app.loaded is true.  NVML writes (mem offset, fan speed)
                    // access-violate on the transitional driver and kill the pipe
                    // server thread (GUI sees ERROR_BROKEN_PIPE).  The dedicated
                    // reapply thread handles writes during the recovery window and
                    // survives VEH crashes via the health-check monitor.
                    if (!explicitUserApply && nvml_crash_recovery_active()) {
                        debug_log("service APPLY rejected: NVML crash recovery in progress (loaded=%d)\n",
                            g_app.loaded ? 1 : 0);
                        response.status = SERVICE_STATUS_ERROR;
                        StringCchCopyA(response.message, ARRAY_COUNT(response.message),
                            "GPU driver is recovering after a device reconnect; please retry in a few seconds.");
                        populate_service_snapshot(&response.snapshot);
                        if (g_serviceHasActiveDesired) response.desired = g_serviceActiveDesired;
                        if (g_serviceControlStateValid) response.controlState = g_serviceControlState;
                        break;
                    }
                    debug_log("ipc raw: hasMem=%d memRaw=%d hasGpu=%d gpuRaw=%d exclRaw=%d\n",
                        request.desired.hasMemOffset ? 1 : 0,
                        request.desired.memOffsetMHz,
                        request.desired.hasGpuOffset ? 1 : 0,
                        request.desired.gpuOffsetMHz,
                        request.desired.gpuOffsetExcludeLowCount);
                    int rawGpuMHz = request.desired.gpuOffsetMHz;
                    validate_desired_settings_for_ipc(&request.desired);
                    request.desired.resetOcBeforeApply = request.resetOcBeforeApply != 0;
                    if (request.desired.hasGpuOffset && rawGpuMHz != request.desired.gpuOffsetMHz) {
                        debug_log("ipc validated: GPU offset clamped from %d to %d MHz (out of [-1000,1000] IPC range)\n",
                            rawGpuMHz, request.desired.gpuOffsetMHz);
                    }
                    debug_log("ipc validated: hasMem=%d mem=%d\n",
                        request.desired.hasMemOffset ? 1 : 0,
                        request.desired.memOffsetMHz);
                    lock_service_runtime();
                    result[0] = '\0';
                    if (!service_mutation_preconditions_match(
                            &request, result, sizeof(result))) {
                        response.status = SERVICE_STATUS_STALE_STATE;
                        StringCchCopyA(response.message,
                            ARRAY_COUNT(response.message), result);
                        unlock_service_runtime();
                        break;
                    }
                    if (explicitUserApply) {
                        // Serialize supersession with lifecycle writes. If an
                        // automatic write already owns the runtime lock it is
                        // already irreversible and completes first; otherwise
                        // this cancellation wins before it can be authorized.
                        bool unsafeDriverTransition =
                            service_explicit_supersede_automatic_work_locked(
                                callerSessionId, "explicit user Apply");
                        if (unsafeDriverTransition) {
                            response.status = SERVICE_STATUS_ERROR;
                            StringCchCopyA(response.message,
                                ARRAY_COUNT(response.message),
                                "GPU driver recovery was superseded, but the driver is still transitional; retry Apply explicitly when it is ready.");
                            populate_service_snapshot(&response.snapshot);
                            if (g_serviceHasActiveDesired) {
                                response.desired = g_serviceActiveDesired;
                            }
                            if (g_serviceControlStateValid) {
                                response.controlState = g_serviceControlState;
                            }
                            unlock_service_runtime();
                            break;
                        }
                    } else {
                        // Authorization is checked again under the same lock as
                        // the sole write. A failed request ahead of us may have
                        // latched lockout while this automatic request waited.
                        DWORD currentLockout = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
                        if (service_auto_restore_is_locked_out(&currentLockout)) {
                            response.status = SERVICE_STATUS_ERROR;
                            StringCchPrintfA(response.message,
                                ARRAY_COUNT(response.message),
                                "Automatic apply is disabled: %s",
                                service_auto_restore_lockout_reason_name(
                                    currentLockout));
                            debug_log("service APPLY rejected under runtime lock: automatic origin=%u lockout=%s\n",
                                (unsigned int)applyOrigin,
                                service_auto_restore_lockout_reason_name(
                                    currentLockout));
                            unlock_service_runtime();
                            break;
                        }
                        EnterCriticalSection(&g_appLock);
                        bool driverRecoveryPending =
                            g_serviceLifecycleState.driverPending;
                        LeaveCriticalSection(&g_appLock);
                        if (driverRecoveryPending ||
                            g_serviceControlledRecoveryValidated ||
                            InterlockedExchangeAdd(
                                &g_serviceRestartRequested, 0) != 0 ||
                            InterlockedExchangeAdd(
                                &g_serviceRestartPreparing, 0) != 0) {
                            response.status = SERVICE_STATUS_ERROR;
                            StringCchCopyA(response.message,
                                ARRAY_COUNT(response.message),
                                "Automatic apply is deferred because controlled driver recovery has precedence");
                            debug_log("service APPLY rejected under runtime lock: automatic origin=%u controlled recovery has precedence\n",
                                (unsigned int)applyOrigin);
                            unlock_service_runtime();
                            break;
                        }
                    }
                    bool ok = true;
                    if (enforcePublishedGpuBinding) {
                        GpuAdapterInfo publishedTarget = {};
                        ok = service_resolve_configured_gpu_target(
                            &publishedGpuBinding, &publishedTarget,
                            result, sizeof(result));
                        if (ok) request.targetGpu = publishedTarget;
                    }
                    if (ok) {
                        ok = service_prepare_requested_gpu(&request, result,
                            sizeof(result));
                    }
                    bool hadPreviousIntent = g_serviceHasActiveDesired;
                    GpuAdapterInfo previousRestoreTarget =
                        g_serviceActiveDesiredGpu;
                    ServiceProfileSource profileSource =
                        SERVICE_PROFILE_SOURCE_AD_HOC;
                    unsigned int profileSlot = 0;
                    service_validate_requested_profile_metadata(&request,
                        &profileSource, &profileSlot);
                    bool replaceActiveIntent =
                        profileSource == SERVICE_PROFILE_SOURCE_USER_SLOT ||
                        profileSource == SERVICE_PROFILE_SOURCE_SHARED_SLOT;
                    DesiredSettings hardwareRequest = request.desired;
                    if (replaceActiveIntent) {
                        service_build_profile_transition_request(
                            hadPreviousIntent ? &g_serviceActiveDesired : nullptr,
                            &request.desired, &hardwareRequest);
                    }
                    if (ok) {
                        GpuAdapterInfo applyTarget = g_app.selectedGpu;
                        if (!applyTarget.valid &&
                            g_app.selectedGpuIndex < g_app.adapterCount) {
                            applyTarget =
                                g_app.adapters[g_app.selectedGpuIndex];
                        }
                        service_refresh_selected_gpu_notification_best_effort(
                            &applyTarget, "service apply pre-write target");
                    }
                    bool writeAttempted = false;
                    ServiceSelectedGpuWriteEpoch gpuEpoch =
                        service_selected_gpu_capture_write_epoch();
                    EnterCriticalSection(&g_appLock);
                    bool exactRecoveryCuePending =
                        service_lifecycle_selected_gpu_recovery_cue_pending_locked();
                    LeaveCriticalSection(&g_appLock);
                    if (ok && !service_selected_gpu_write_epoch_is_current(
                            gpuEpoch)) {
                        ok = false;
                        StringCchCopyA(result, ARRAY_COUNT(result),
                            "Selected GPU changed or was removed immediately before Apply");
                    } else if (ok && exactRecoveryCuePending) {
                        ok = false;
                        StringCchCopyA(result, ARRAY_COUNT(result),
                            "Selected GPU recovery has precedence over Apply; retry explicitly after recovery");
                    }
                    if (ok) ok = service_apply_desired_settings(
                        &hardwareRequest,
                        (request.flags & SERVICE_REQUEST_FLAG_INTERACTIVE) != 0,
                        result, sizeof(result), &writeAttempted,
                        replaceActiveIntent,
                        replaceActiveIntent ? &request.desired : nullptr);
                    if (!writeAttempted && hadPreviousIntent) {
                        service_refresh_selected_gpu_notification_best_effort(
                            &previousRestoreTarget,
                            "restore prior target after service apply preflight loss");
                    }
                    if (ok) {
                        service_refresh_selected_gpu_notification_best_effort(
                            &g_serviceActiveDesiredGpu,
                            "successful service apply target");
                        service_capture_owner_identity(callerUser, callerSessionId);
                        EnterCriticalSection(&g_appLock);
                        g_serviceActiveProfileSource = profileSource;
                        g_serviceActiveProfileSlot = profileSlot;
                        LeaveCriticalSection(&g_appLock);
                        service_write_restart_reapply_snapshot();
                        // Every successful client Apply starts a fresh awake-
                        // time proving period. Automatic writes never clear history.
                        bool proofRecorded = service_record_oc_apply_stamp();
                        if (explicitUserApply && proofRecorded) {
                            // Only a deliberate successful user write with a
                            // durably published fresh proof acknowledges
                            // instability/TDR history.
                            bool lockoutCleared =
                                service_clear_auto_restore_lockout();
                            bool historyCleared =
                                service_clear_restart_history();
                            if (lockoutCleared && historyCleared) {
                                EnterCriticalSection(&g_appLock);
                                g_serviceLifecycleState.lockedOut = false;
                                LeaveCriticalSection(&g_appLock);
                            } else {
                                service_latch_auto_restore_lockout(
                                    SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED,
                                    "explicit Apply could not durably re-arm automatic restoration");
                            }
                        }
                    } else if (writeAttempted) {
                        // A failed real hardware write is terminal. Readiness or
                        // target validation failures before the write remain
                        // ordinary request failures and do not invent a lockout.
                        EnterCriticalSection(&g_appLock);
                        ServiceLifecycleEvent lockoutEvent = {};
                        lockoutEvent.type = SERVICE_LIFECYCLE_EVENT_LOCKOUT;
                        service_lifecycle_reduce_locked(&lockoutEvent);
                        LeaveCriticalSection(&g_appLock);
                        service_disable_automatic_restore(SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED,
                            explicitUserApply
                                ? "explicit apply hardware write did not complete"
                                : "automatic apply hardware write did not complete");
                    }
                    response.status = ok ? SERVICE_STATUS_OK : SERVICE_STATUS_ERROR;
                    StringCchCopyA(response.message, ARRAY_COUNT(response.message), result);
                    populate_service_snapshot(&response.snapshot);
                    if (g_serviceHasActiveDesired) response.desired = g_serviceActiveDesired;
                    if (g_serviceControlStateValid) response.controlState = g_serviceControlState;
                    debug_log("service response APPLY: ok=%d controlValid=%d gpu=%d exclude=%d mem=%d power=%d fanMode=%d fanPct=%d\n",
                        ok ? 1 : 0,
                        response.controlState.valid ? 1 : 0,
                        response.controlState.gpuOffsetMHz,
                        response.controlState.gpuOffsetExcludeLowCount,
                        response.controlState.memOffsetMHz,
                        response.controlState.powerLimitPct,
                        response.controlState.fanMode,
                        response.controlState.fanFixedPercent);
                    unlock_service_runtime();
                    break;
                }
                case SERVICE_CMD_RESET: {
                    ServiceOperationRequestGuard operation(&request, &response,
                        "reset");
                    if (!operation.execute()) {
                        populate_service_snapshot(&response.snapshot);
                        if (g_serviceHasActiveDesired) response.desired = g_serviceActiveDesired;
                        if (g_serviceControlStateValid) response.controlState = g_serviceControlState;
                        break;
                    }
                    char result[512] = {};
                    if (!service_mutation_preconditions_match(
                            &request, result, sizeof(result))) {
                        response.status = SERVICE_STATUS_STALE_STATE;
                        StringCchCopyA(response.message,
                            ARRAY_COUNT(response.message), result);
                        debug_log("service RESET rejected by reconnect precondition: operation=%llu instance=%llu gpuGeneration=%llu topology=%llu reason=%s\n",
                            (unsigned long long)request.operationId,
                            (unsigned long long)request.expectedServiceInstanceId,
                            (unsigned long long)request.expectedGpuGeneration,
                            (unsigned long long)request.expectedTopologySignature,
                            result[0] ? result : "unknown");
                        break;
                    }
                    // Reject reset while recovering from a GPU device reconnect:
                    // service_reset_all() issues NVAPI/NVML writes + refresh
                    // that would access-violate on the transitional driver and
                    // kill this pipe server thread (GUI sees ERROR_BROKEN_PIPE).
                    //
                    // Allow reset if GPU data is already loaded (g_app.loaded is
                    // true) — the crash window was restored by recovery as a
                    // safety measure, but the handles are fresh and valid.
                    // RC7: block ALL resets during crash recovery (same reason
                    // as APPLY — writes access-violate on the transitional driver).
                    lock_service_runtime();
                    result[0] = '\0';
                    if (!service_mutation_preconditions_match(
                            &request, result, sizeof(result))) {
                        response.status = SERVICE_STATUS_STALE_STATE;
                        StringCchCopyA(response.message,
                            ARRAY_COUNT(response.message), result);
                        unlock_service_runtime();
                        break;
                    }
                    bool unsafeDriverTransition =
                        service_explicit_supersede_automatic_work_locked(
                            callerSessionId, "explicit user Reset");
                    if (unsafeDriverTransition) {
                        response.status = SERVICE_STATUS_ERROR;
                        StringCchCopyA(response.message,
                            ARRAY_COUNT(response.message),
                            "GPU driver recovery was superseded, but the driver is still transitional; retry Reset explicitly when it is ready.");
                        populate_service_snapshot(&response.snapshot);
                        if (g_serviceHasActiveDesired) {
                            response.desired = g_serviceActiveDesired;
                        }
                        if (g_serviceControlStateValid) {
                            response.controlState = g_serviceControlState;
                        }
                        unlock_service_runtime();
                        break;
                    }
                    bool ok = service_prepare_requested_gpu(&request, result, sizeof(result));
                    if (ok) {
                        GpuAdapterInfo resetTarget = g_app.selectedGpu;
                        if (!resetTarget.valid &&
                            g_app.selectedGpuIndex < g_app.adapterCount) {
                            resetTarget =
                                g_app.adapters[g_app.selectedGpuIndex];
                        }
                        service_refresh_selected_gpu_notification_best_effort(
                            &resetTarget, "explicit reset pre-write target");
                    }
                    bool resetWriteAttempted = false;
                    ServiceSelectedGpuWriteEpoch resetEpoch =
                        service_selected_gpu_capture_write_epoch();
                    EnterCriticalSection(&g_appLock);
                    bool exactResetRecoveryCuePending =
                        service_lifecycle_selected_gpu_recovery_cue_pending_locked();
                    LeaveCriticalSection(&g_appLock);
                    if (ok && !service_selected_gpu_write_epoch_is_current(
                            resetEpoch)) {
                        ok = false;
                        StringCchCopyA(result, ARRAY_COUNT(result),
                            "Selected GPU changed or was removed immediately before Reset");
                    } else if (ok && exactResetRecoveryCuePending) {
                        ok = false;
                        StringCchCopyA(result, ARRAY_COUNT(result),
                            "Selected GPU recovery has precedence over Reset; retry explicitly after recovery");
                    }
                    if (ok) ok = service_reset_all(result, sizeof(result),
                        &resetWriteAttempted);
                    if (ok) {
                        GpuAdapterInfo resetTarget = g_app.selectedGpu;
                        if (!resetTarget.valid &&
                            g_app.selectedGpuIndex < g_app.adapterCount) {
                            resetTarget = g_app.adapters[g_app.selectedGpuIndex];
                        }
                        service_refresh_selected_gpu_notification_best_effort(
                            &resetTarget, "successful explicit reset target");
                        service_capture_owner_identity(callerUser, callerSessionId);
                        EnterCriticalSection(&g_appLock);
                        g_serviceActiveProfileSource = SERVICE_PROFILE_SOURCE_NONE;
                        g_serviceActiveProfileSlot = 0;
                        LeaveCriticalSection(&g_appLock);
                        // OC reset to defaults — clear the stabilization window stamp.
                        service_clear_oc_apply_stamp();
                    } else if (resetWriteAttempted) {
                        EnterCriticalSection(&g_appLock);
                        ServiceLifecycleEvent lockoutEvent = {};
                        lockoutEvent.type = SERVICE_LIFECYCLE_EVENT_LOCKOUT;
                        service_lifecycle_reduce_locked(&lockoutEvent);
                        LeaveCriticalSection(&g_appLock);
                        service_disable_automatic_restore(
                            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED,
                            "explicit Reset hardware write did not complete");
                    }
                    response.status = ok ? SERVICE_STATUS_OK : SERVICE_STATUS_ERROR;
                    StringCchCopyA(response.message, ARRAY_COUNT(response.message), result);
                    populate_service_snapshot(&response.snapshot);
                    if (g_serviceHasActiveDesired) response.desired = g_serviceActiveDesired;
                    if (g_serviceControlStateValid) response.controlState = g_serviceControlState;
                    debug_log("service response RESET: ok=%d gpu=%d exclude=%d fanMode=%d fanPct=%d\n",
                        ok ? 1 : 0,
                        response.controlState.gpuOffsetMHz,
                        response.controlState.gpuOffsetExcludeLowCount,
                        response.controlState.fanMode,
                        response.controlState.fanFixedPercent);
                    unlock_service_runtime();
                    break;
                }
                case SERVICE_CMD_GET_ACTIVE_DESIRED:
                    lock_service_runtime();
                    response.status = SERVICE_STATUS_OK;
                    if (g_serviceHasActiveDesired) response.desired = g_serviceActiveDesired;
                    populate_service_snapshot(&response.snapshot);
                    if (g_serviceControlStateValid) response.controlState = g_serviceControlState;
                    unlock_service_runtime();
                    break;
                case SERVICE_CMD_GET_OPERATION_RESULT: {
                    ensure_service_operation_tracker_loaded();
                    response.operationId = request.operationId;
                    const ServiceOperationRecord* record = service_operation_find(
                        &g_serviceOperationTracker, request.operationId);
                    if (!record) {
                        response.status = SERVICE_STATUS_ERROR;
                        response.operationState = SERVICE_OPERATION_OUTCOME_UNKNOWN;
                        StringCchCopyA(response.message,
                            ARRAY_COUNT(response.message),
                            "Operation outcome is unknown to this service generation");
                    } else {
                        response.status = record->responseStatus;
                        response.operationState = record->state;
                        StringCchCopyA(response.message,
                            ARRAY_COUNT(response.message),
                            record->message[0] ? record->message :
                            "Operation result available");
                    }
                    populate_service_snapshot(&response.snapshot);
                    if (g_serviceHasActiveDesired) response.desired = g_serviceActiveDesired;
                    if (g_serviceControlStateValid) response.controlState = g_serviceControlState;
                    break;
                }
                case SERVICE_CMD_WRITE_LOG_SNAPSHOT:
                case SERVICE_CMD_WRITE_JSON_SNAPSHOT:
                case SERVICE_CMD_WRITE_PROBE_REPORT: {
                    char detail[256] = {};
                    lock_service_runtime();
                    bool ok = hardware_initialize(detail, sizeof(detail));
                    if (!ok && request.command != SERVICE_CMD_WRITE_PROBE_REPORT) {
                        response.status = SERVICE_STATUS_ERROR;
                        StringCchCopyA(response.message, ARRAY_COUNT(response.message), detail[0] ? detail : "Hardware initialization failed");
                    } else {
                        bool offsetsOk = false;
                        if (ok && !read_live_curve_snapshot_settled(4, 40, &offsetsOk)) {
                            debug_log("service file command: live curve refresh failed before file write\n");
                        }
                        if (ok) {
                            refresh_global_state(detail, sizeof(detail));
                        }
                        char fileErr[256] = {};
                        bool writeOk = false;
                        ScopedServiceClientImpersonation impersonation(callerToken);
                        if (!impersonation.active()) {
                            set_message(fileErr, sizeof(fileErr),
                                "Failed impersonating the authenticated client for output write (error %lu)",
                                GetLastError());
                        } else if (!service_validate_file_write_path(request.path,
                                       fileErr, sizeof(fileErr))) {
                            debug_log("service file command: caller-scoped path validation failed command=%u pid=%lu: %s\n",
                                (unsigned int)request.command,
                                (unsigned long)callerPid,
                                fileErr[0] ? fileErr : "unknown");
                        } else if (request.command == SERVICE_CMD_WRITE_LOG_SNAPSHOT) {
                            writeOk = write_log_snapshot(request.path, fileErr,
                                sizeof(fileErr));
                        } else if (request.command == SERVICE_CMD_WRITE_JSON_SNAPSHOT) {
                            writeOk = write_json_snapshot(request.path, fileErr,
                                sizeof(fileErr));
                        } else {
                            writeOk = write_probe_report(request.path, fileErr, sizeof(fileErr));
                        }
                        if (writeOk) {
                            char verifyErr[256] = {};
                            if (!service_verify_written_file_path(request.path, verifyErr, sizeof(verifyErr))) {
                                writeOk = false;
                                StringCchCopyA(fileErr, sizeof(fileErr), verifyErr);
                            }
                        }
                        response.status = writeOk ? SERVICE_STATUS_OK : SERVICE_STATUS_ERROR;
                        if (writeOk) {
                            StringCchPrintfA(response.message, ARRAY_COUNT(response.message), "Wrote %s", request.path[0] ? request.path : "requested output file");
                        } else {
                            StringCchCopyA(response.message, ARRAY_COUNT(response.message), fileErr[0] ? fileErr : "Failed writing requested file");
                        }
                        populate_service_snapshot(&response.snapshot);
                        if (g_serviceControlStateValid) response.controlState = g_serviceControlState;
                    }
                    unlock_service_runtime();
                    break;
                }
                default:
                    response.status = SERVICE_STATUS_ERROR;
                    StringCchCopyA(response.message, ARRAY_COUNT(response.message), "Unsupported service command");
                    break;
            }
            }
        }

        if (callerToken) {
            CloseHandle(callerToken);
            callerToken = nullptr;
        }

        response.message[ARRAY_COUNT(response.message) - 1] = '\0';
        // Every response, including errors and mutation outcomes, carries one
        // coherent authoritative envelope.  This final capture deliberately
        // replaces any command-local cached copies above.
        populate_service_state_response(&response);
        pipeErr[0] = 0;
        if (!service_pipe_write_exact(pipe, &response, sizeof(response), SERVICE_PIPE_SERVER_IO_TIMEOUT_MS, "writing service response", pipeErr, sizeof(pipeErr))) {
            debug_log("service_pipe_server: response write failed: %s\n", pipeErr[0] ? pipeErr : "unknown");
        }
        DisconnectNamedPipe(pipe);
        service_close_owned_pipe(pipe);
    }
    return 0;
}

static HANDLE g_servicePipeThread = nullptr;
static HANDLE g_serviceDeviceNotifyHandle = nullptr;

// service_handle_session_change is defined in main_service_sessions.cpp
// (included before this shard).  The SCM SESSIONCHANGE handler routes through it.
