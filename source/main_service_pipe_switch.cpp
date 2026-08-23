// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The service command switch. Included verbatim inside
// service_execute_checked_request() so every handler still runs serialized
// under g_serviceDispatchLock. In-scope names: ServiceRequest* request,
// ServiceResponse* response, ServiceClientIdentity* caller.
//
// Split out of main_service_pipe.cpp only to keep both shards within the
// project's source-size guideline; it is not an include boundary for reuse.

switch (request->command) {
    case SERVICE_CMD_PING:
        response->status = SERVICE_STATUS_OK;
        StringCchCopyA(response->message, ARRAY_COUNT(response->message), "pong");
        break;
    case SERVICE_CMD_LOGON_HANDOFF:
        if (request->applyOrigin != SERVICE_APPLY_ORIGIN_LOGON) {
            response->status = SERVICE_STATUS_ERROR;
            StringCchCopyA(response->message, ARRAY_COUNT(response->message),
                "Invalid logon handoff origin");
        } else if (!service_lifecycle_post_logon_handoff(
                       &caller->lifecycle)) {
            response->status = SERVICE_STATUS_ERROR;
            StringCchCopyA(response->message, ARRAY_COUNT(response->message),
                "Lifecycle coordinator is not accepting logon handoffs");
        } else {
            response->status = SERVICE_STATUS_OK;
            StringCchPrintfA(response->message, ARRAY_COUNT(response->message),
                "Logon handoff accepted for session %lu",
                (unsigned long)caller->sessionId);
            char handoffUserToken[32] = {};
            gc_log_identifier_token(caller->user, handoffUserToken,
                                    sizeof(handoffUserToken));
            debug_log("service logon handoff: authenticated caller pid=%lu session=%lu user=%s; profile/settings payload ignored\n",
                (unsigned long)caller->pid, (unsigned long)caller->sessionId,
                handoffUserToken);
        }
        populate_service_snapshot(&response->snapshot);
        break;
    case SERVICE_CMD_GET_SNAPSHOT: {
        service_handle_snapshot_request(response);
        break;
    }
    case SERVICE_CMD_GET_TELEMETRY:
        service_handle_telemetry_request(response);
        break;
    case SERVICE_CMD_APPLY: {
        ServiceOperationRequestGuard operation(request, response,
            "apply");
        if (!operation.execute()) {
            populate_service_snapshot(&response->snapshot);
            if (g_serviceHasActiveDesired) response->desired = g_serviceActiveDesired;
            if (g_serviceControlStateValid) response->controlState = g_serviceControlState;
            break;
        }
        char result[512] = {};
        if (!service_mutation_preconditions_match(
                request, result, sizeof(result))) {
            response->status = SERVICE_STATUS_STALE_STATE;
            StringCchCopyA(response->message,
                ARRAY_COUNT(response->message), result);
            debug_log("service APPLY rejected by reconnect precondition: operation=%llu instance=%llu gpuGeneration=%llu topology=%llu reason=%s\n",
                (unsigned long long)request->operationId,
                (unsigned long long)request->expectedServiceInstanceId,
                (unsigned long long)request->expectedGpuGeneration,
                (unsigned long long)request->expectedTopologySignature,
                result);
            break;
        }
        bool enforcePublishedGpuBinding = false;
        ConfiguredGpuSelection publishedGpuBinding = {};
        if (!service_request_apply_origin_valid(request)) {
            response->status = SERVICE_STATUS_ERROR;
            StringCchCopyA(response->message, ARRAY_COUNT(response->message),
                "Apply request is missing a valid typed origin");
            debug_log("service APPLY rejected: invalid origin=%u source=%s\n",
                request->applyOrigin,
                request->source[0] ? request->source : "<none>");
            break;
        }
        ServiceApplyOrigin applyOrigin =
            (ServiceApplyOrigin)request->applyOrigin;
        bool explicitUserApply =
            service_apply_origin_is_explicit(applyOrigin);
        if (!explicitUserApply) {
            DWORD lockoutReason = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
            if (service_auto_restore_is_locked_out(&lockoutReason)) {
                response->status = SERVICE_STATUS_ERROR;
                StringCchPrintfA(response->message,
                    ARRAY_COUNT(response->message),
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
        if (!service_apply_shared_only_policy(request,
                caller->isAdmin, caller->user,
                &enforcePublishedGpuBinding,
                &publishedGpuBinding, result, sizeof(result))) {
            response->status = SERVICE_STATUS_ERROR;
            StringCchCopyA(response->message,
                ARRAY_COUNT(response->message), result);
            break;
        }
        // Reject apply while recovering from a GPU device reconnect:
        // the NVML/NVAPI writes would access-violate on the still-
        // transitional driver and kill the executing thread
        // (GUI sees ERROR_BROKEN_PIPE).  The fan runtime thread
        // auto-reapplies the active profile once the driver settles.
        //
        // Allow apply if GPU data is already loaded (g_app.loaded
        // is true) — the crash window was restored by recovery as a
        // safety measure, but the handles are fresh and valid.
        // RC7: block ALL GUI applies during crash recovery, even if
        // g_app.loaded is true.  NVML writes (mem offset, fan speed)
        // access-violate on the transitional driver and kill the
        // executing thread (GUI sees ERROR_BROKEN_PIPE).  The dedicated
        // reapply thread handles writes during the recovery window and
        // survives VEH crashes via the health-check monitor.
        if (!explicitUserApply && nvml_crash_recovery_active()) {
            debug_log("service APPLY rejected: NVML crash recovery in progress (loaded=%d)\n",
                g_app.loaded ? 1 : 0);
            response->status = SERVICE_STATUS_ERROR;
            StringCchCopyA(response->message,
                ARRAY_COUNT(response->message),
                "GPU driver is recovering after a device reconnect; please retry in a few seconds.");
            populate_service_snapshot(&response->snapshot);
            if (g_serviceHasActiveDesired) response->desired = g_serviceActiveDesired;
            if (g_serviceControlStateValid) response->controlState = g_serviceControlState;
            break;
        }
        debug_log("ipc raw: hasMem=%d memRaw=%d hasGpu=%d gpuRaw=%d exclRaw=%d\n",
            request->desired.hasMemOffset ? 1 : 0,
            request->desired.memOffsetMHz,
            request->desired.hasGpuOffset ? 1 : 0,
            request->desired.gpuOffsetMHz,
            request->desired.gpuOffsetExcludeLowCount);
        int rawGpuMHz = request->desired.gpuOffsetMHz;
        validate_desired_settings_for_ipc(&request->desired);
        request->desired.resetOcBeforeApply = request->resetOcBeforeApply != 0;
        if (request->desired.hasGpuOffset && rawGpuMHz != request->desired.gpuOffsetMHz) {
            debug_log("ipc validated: GPU offset clamped from %d to %d MHz (out of [-1000,1000] IPC range)\n",
                rawGpuMHz, request->desired.gpuOffsetMHz);
        }
        debug_log("ipc validated: hasMem=%d mem=%d\n",
            request->desired.hasMemOffset ? 1 : 0,
            request->desired.memOffsetMHz);
        lock_service_runtime();
        if (service_update_install_reject_mutation(
                response, "APPLY")) {
            unlock_service_runtime();
            break;
        }
        result[0] = '\0';
        if (!service_mutation_preconditions_match(
                request, result, sizeof(result))) {
            response->status = SERVICE_STATUS_STALE_STATE;
            StringCchCopyA(response->message,
                ARRAY_COUNT(response->message), result);
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
                    caller->sessionId, "explicit user Apply");
            if (unsafeDriverTransition) {
                response->status = SERVICE_STATUS_ERROR;
                StringCchCopyA(response->message,
                    ARRAY_COUNT(response->message),
                    "GPU driver recovery was superseded, but the driver is still transitional; retry Apply explicitly when it is ready.");
                populate_service_snapshot(&response->snapshot);
                if (g_serviceHasActiveDesired) {
                    response->desired = g_serviceActiveDesired;
                }
                if (g_serviceControlStateValid) {
                    response->controlState = g_serviceControlState;
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
                response->status = SERVICE_STATUS_ERROR;
                StringCchPrintfA(response->message,
                    ARRAY_COUNT(response->message),
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
                response->status = SERVICE_STATUS_ERROR;
                StringCchCopyA(response->message,
                    ARRAY_COUNT(response->message),
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
            if (ok) request->targetGpu = publishedTarget;
        }
        if (ok) {
            ok = service_prepare_requested_gpu(request, result,
                sizeof(result));
        }
        bool hadPreviousIntent = g_serviceHasActiveDesired;
        GpuAdapterInfo previousRestoreTarget =
            g_serviceActiveDesiredGpu;
        ServiceProfileSource profileSource =
            SERVICE_PROFILE_SOURCE_AD_HOC;
        unsigned int profileSlot = 0;
        bool replaceActiveIntent =
            service_profile_identity_replaces_active_intent(
                service_validate_requested_profile_metadata(
                    request, &profileSource, &profileSlot));
        DesiredSettings hardwareRequest = request->desired;
        if (replaceActiveIntent) {
            service_build_profile_transition_request(
                hadPreviousIntent ? &g_serviceActiveDesired : nullptr,
                &request->desired, &hardwareRequest);
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
        gc_u32 applySeverity =
            (gc_u32)SERVICE_OUTCOME_SEVERITY_ERROR;
        if (ok) ok = service_apply_desired_settings(
            &hardwareRequest,
            (request->flags & SERVICE_REQUEST_FLAG_INTERACTIVE) != 0,
            result, sizeof(result), &writeAttempted,
            replaceActiveIntent,
            replaceActiveIntent ? &request->desired : nullptr,
            &applySeverity);
        if (!writeAttempted && hadPreviousIntent) {
            service_refresh_selected_gpu_notification_best_effort(
                &previousRestoreTarget,
                "restore prior target after service apply preflight loss");
        }
        if (ok) {
            service_refresh_selected_gpu_notification_best_effort(
                &g_serviceActiveDesiredGpu,
                "successful service apply target");
            service_capture_owner_identity(caller->user, caller->sessionId);
            service_record_apply_profile_identity(request,
                profileSource, profileSlot);
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
        response->status = ok ? SERVICE_STATUS_OK : SERVICE_STATUS_ERROR;
        // Only a real apply can report a partial verify; every
        // preflight refusal above left the response at its zeroed
        // default and is resolved to ERROR by the write-out stamp.
        response->outcomeSeverity = applySeverity;
        StringCchCopyA(response->message, ARRAY_COUNT(response->message), result);
        populate_service_snapshot(&response->snapshot);
        if (g_serviceHasActiveDesired) response->desired = g_serviceActiveDesired;
        if (g_serviceControlStateValid) response->controlState = g_serviceControlState;
        debug_log("service response APPLY: ok=%d severity=%s controlValid=%d gpu=%d exclude=%d mem=%d power=%d fanMode=%d fanPct=%d\n",
            ok ? 1 : 0,
            service_outcome_severity_name(applySeverity),
            response->controlState.valid ? 1 : 0,
            response->controlState.gpuOffsetMHz,
            response->controlState.gpuOffsetExcludeLowCount,
            response->controlState.memOffsetMHz,
            response->controlState.powerLimitPct,
            response->controlState.fanMode,
            response->controlState.fanFixedPercent);
        unlock_service_runtime();
        break;
    }
    case SERVICE_CMD_RESET: {
        ServiceOperationRequestGuard operation(request, response,
            "reset");
        if (!operation.execute()) {
            populate_service_snapshot(&response->snapshot);
            if (g_serviceHasActiveDesired) response->desired = g_serviceActiveDesired;
            if (g_serviceControlStateValid) response->controlState = g_serviceControlState;
            break;
        }
        char result[512] = {};
        if (!service_mutation_preconditions_match(
                request, result, sizeof(result))) {
            response->status = SERVICE_STATUS_STALE_STATE;
            StringCchCopyA(response->message,
                ARRAY_COUNT(response->message), result);
            debug_log("service RESET rejected by reconnect precondition: operation=%llu instance=%llu gpuGeneration=%llu topology=%llu reason=%s\n",
                (unsigned long long)request->operationId,
                (unsigned long long)request->expectedServiceInstanceId,
                (unsigned long long)request->expectedGpuGeneration,
                (unsigned long long)request->expectedTopologySignature,
                result);
            break;
        }
        // Reject reset while recovering from a GPU device reconnect:
        // service_reset_all() issues NVAPI/NVML writes + refresh
        // that would access-violate on the transitional driver and
        // kill the executing thread (GUI sees ERROR_BROKEN_PIPE).
        //
        // Allow reset if GPU data is already loaded (g_app.loaded is
        // true) — the crash window was restored by recovery as a
        // safety measure, but the handles are fresh and valid.
        // RC7: block ALL resets during crash recovery (same reason
        // as APPLY — writes access-violate on the transitional driver).
        lock_service_runtime();
        if (service_update_install_reject_mutation(
                response, "RESET")) {
            unlock_service_runtime();
            break;
        }
        result[0] = '\0';
        if (!service_mutation_preconditions_match(
                request, result, sizeof(result))) {
            response->status = SERVICE_STATUS_STALE_STATE;
            StringCchCopyA(response->message,
                ARRAY_COUNT(response->message), result);
            unlock_service_runtime();
            break;
        }
        bool unsafeDriverTransition =
            service_explicit_supersede_automatic_work_locked(
                caller->sessionId, "explicit user Reset");
        if (unsafeDriverTransition) {
            response->status = SERVICE_STATUS_ERROR;
            StringCchCopyA(response->message,
                ARRAY_COUNT(response->message),
                "GPU driver recovery was superseded, but the driver is still transitional; retry Reset explicitly when it is ready.");
            populate_service_snapshot(&response->snapshot);
            if (g_serviceHasActiveDesired) {
                response->desired = g_serviceActiveDesired;
            }
            if (g_serviceControlStateValid) {
                response->controlState = g_serviceControlState;
            }
            unlock_service_runtime();
            break;
        }
        bool ok = service_prepare_requested_gpu(request, result, sizeof(result));
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
            service_capture_owner_identity(caller->user, caller->sessionId);
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
        response->status = ok ? SERVICE_STATUS_OK : SERVICE_STATUS_ERROR;
        StringCchCopyA(response->message, ARRAY_COUNT(response->message), result);
        populate_service_snapshot(&response->snapshot);
        if (g_serviceHasActiveDesired) response->desired = g_serviceActiveDesired;
        if (g_serviceControlStateValid) response->controlState = g_serviceControlState;
        debug_log("service response RESET: ok=%d gpu=%d exclude=%d fanMode=%d fanPct=%d\n",
            ok ? 1 : 0,
            response->controlState.gpuOffsetMHz,
            response->controlState.gpuOffsetExcludeLowCount,
            response->controlState.fanMode,
            response->controlState.fanFixedPercent);
        unlock_service_runtime();
        break;
    }
    case SERVICE_CMD_GET_ACTIVE_DESIRED:
        lock_service_runtime();
        response->status = SERVICE_STATUS_OK;
        if (g_serviceHasActiveDesired) response->desired = g_serviceActiveDesired;
        populate_service_snapshot(&response->snapshot);
        if (g_serviceControlStateValid) response->controlState = g_serviceControlState;
        unlock_service_runtime();
        break;
    case SERVICE_CMD_GET_OPERATION_RESULT: {
        ensure_service_operation_tracker_loaded();
        response->operationId = request->operationId;
        const ServiceOperationRecord* record = service_operation_find(
            &g_serviceOperationTracker, request->operationId);
        if (!record) {
            response->status = SERVICE_STATUS_ERROR;
            response->operationState = SERVICE_OPERATION_OUTCOME_UNKNOWN;
            StringCchCopyA(response->message,
                ARRAY_COUNT(response->message),
                "Operation outcome is unknown to this service generation");
        } else {
            response->status = record->responseStatus;
            response->operationState = record->state;
            response->outcomeSeverity = record->outcomeSeverity;
            StringCchCopyA(response->message,
                ARRAY_COUNT(response->message),
                record->message[0] ? record->message :
                "Operation result available");
        }
        populate_service_snapshot(&response->snapshot);
        if (g_serviceHasActiveDesired) response->desired = g_serviceActiveDesired;
        if (g_serviceControlStateValid) response->controlState = g_serviceControlState;
        break;
    }
    case SERVICE_CMD_WRITE_LOG_SNAPSHOT:
    case SERVICE_CMD_WRITE_JSON_SNAPSHOT:
    case SERVICE_CMD_WRITE_PROBE_REPORT:
        service_handle_file_write_command(*request, *response,
            caller->token, caller->pid);
        break;
    // v19 updater; see main_service_update_commands.cpp.
    case SERVICE_CMD_GET_UPDATE_STATE:
    case SERVICE_CMD_CHECK_FOR_UPDATE:
    case SERVICE_CMD_INSTALL_UPDATE:
    case SERVICE_CMD_SET_UPDATE_POLICY:
        service_handle_update_command(*request, *response, caller->pid,
            caller->sessionId, caller->user);
        break;
    default:
        response->status = SERVICE_STATUS_ERROR;
        StringCchCopyA(response->message, ARRAY_COUNT(response->message), "Unsupported service command");
        break;
}
