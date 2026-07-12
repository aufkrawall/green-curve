// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// ============================================================================
// Lifecycle apply authorization and terminal-write handling
// ============================================================================

static bool service_lifecycle_pending_logon_snapshot(
    ServiceLifecycleIdentity* identityOut,
    gc_u64* generationOut,
    ServiceLifecycleTrigger* triggerOut,
    LONGLONG* logoffGenerationOut)
{
    if (!identityOut || !generationOut || !triggerOut ||
        !logoffGenerationOut) return false;
    EnterCriticalSection(&g_appLock);
    bool pending = g_serviceLifecycleState.logonPending &&
        !g_serviceLifecycleState.logonWriteIssued &&
        !g_serviceLifecycleState.driverPending &&
        !g_serviceLifecycleState.lockedOut;
    if (pending) {
        *identityOut = g_serviceLifecycleState.pendingLogonIdentity;
        *generationOut = g_serviceLifecycleState.logonGeneration;
        *triggerOut = g_serviceLifecycleState.pendingLogonTrigger;
        *logoffGenerationOut = InterlockedCompareExchange64(
            &g_serviceLogoffEventGeneration, 0, 0);
    }
    LeaveCriticalSection(&g_appLock);
    return pending;
}

static bool service_lifecycle_logon_still_current(
    const ServiceLifecycleIdentity* identity,
    gc_u64 generation)
{
    EnterCriticalSection(&g_appLock);
    bool current = g_serviceLifecycleState.logonPending &&
        !g_serviceLifecycleState.logonWriteIssued &&
        g_serviceLifecycleState.logonGeneration == generation &&
        service_lifecycle_identity_equal(
            &g_serviceLifecycleState.pendingLogonIdentity, identity);
    LeaveCriticalSection(&g_appLock);
    return current;
}

static void service_lifecycle_finish_logon_without_write(
    const ServiceLifecycleIdentity* identity,
    gc_u64 generation,
    ServiceLifecycleTrigger trigger,
    ServiceLifecycleResult result)
{
    EnterCriticalSection(&g_appLock);
    if (g_serviceLifecycleState.logonPending &&
        !g_serviceLifecycleState.logonWriteIssued &&
        g_serviceLifecycleState.logonGeneration == generation &&
        service_lifecycle_identity_equal(
            &g_serviceLifecycleState.pendingLogonIdentity, identity)) {
        g_serviceLifecycleState.logonPending = false;
        g_serviceLifecycleState.pendingLogonTrigger =
            SERVICE_LIFECYCLE_TRIGGER_NONE;
        memset(&g_serviceLifecycleState.pendingLogonIdentity, 0,
            sizeof(g_serviceLifecycleState.pendingLogonIdentity));
        service_lifecycle_set_result_locked(trigger, result);
    }
    LeaveCriticalSection(&g_appLock);
}

static bool service_lifecycle_authorize_logon_write(
    const ServiceLifecycleIdentity* identity,
    gc_u64 generation,
    LONGLONG capturedLogoffGeneration)
{
    ServiceLifecycleEvent event = {};
    event.type = SERVICE_LIFECYCLE_EVENT_LOGON_WRITE_STARTED;
    event.identity = *identity;
    EnterCriticalSection(&g_appLock);
    bool matchingLogoffArrived =
        InterlockedCompareExchange64(
            &g_serviceLogoffEventGeneration, 0, 0) !=
                capturedLogoffGeneration &&
        (DWORD)InterlockedCompareExchange(
            &g_serviceLastLogoffSessionId, -1, -1) == identity->sessionId;
    bool sameGeneration = g_serviceLifecycleState.logonGeneration == generation;
    bool selectedGpuRecoveryPending =
        service_lifecycle_selected_gpu_recovery_cue_pending_locked();
    ServiceLifecycleDecision decision = sameGeneration &&
        !matchingLogoffArrived && !selectedGpuRecoveryPending
        ? service_lifecycle_reduce_locked(&event)
        : ServiceLifecycleDecision{};
    LeaveCriticalSection(&g_appLock);
    return decision.authorizeLogonWrite != 0;
}

static void service_lifecycle_complete_logon_write(
    const ServiceLifecycleIdentity* identity,
    bool success,
    bool writeAttempted,
    ServiceLifecycleTrigger trigger)
{
    ServiceLifecycleEvent event = {};
    event.type = SERVICE_LIFECYCLE_EVENT_LOGON_WRITE_FINISHED;
    event.identity = *identity;
    event.success = success;
    event.writeAttempted = writeAttempted;
    EnterCriticalSection(&g_appLock);
    ServiceLifecycleDecision decision = service_lifecycle_reduce_locked(&event);
    service_lifecycle_set_result_locked(trigger, decision.result);
    LeaveCriticalSection(&g_appLock);
}

static bool service_lifecycle_revalidate_logon_context(
    const ServiceSessionConfigContext* originalContext,
    const DesiredSettings* originalDesired,
    int originalSlot,
    ServiceProfileSource originalSource,
    char* detail,
    size_t detailSize)
{
    ServiceSessionConfigContext currentContext = {};
    if (!service_resolve_session_config_context(
            originalContext->identity.sessionId, &currentContext,
            detail, detailSize)) {
        return false;
    }
    if (!service_lifecycle_identity_equal(
            &originalContext->identity, &currentContext.identity)) {
        set_message(detail, detailSize, "session identity changed before write");
        return false;
    }
    DesiredSettings currentDesired = {};
    int currentSlot = 0;
    ServiceProfileSource currentSource = SERVICE_PROFILE_SOURCE_NONE;
    ServiceLogonProfileResolveResult resolved =
        service_load_logon_profile_from_context(&currentContext,
            &currentDesired, &currentSlot, &currentSource);
    if (resolved == SERVICE_LOGON_PROFILE_RESOLVED &&
        memcmp(&currentContext.configuredGpu, &originalContext->configuredGpu,
            sizeof(currentContext.configuredGpu)) != 0) {
        set_message(detail, detailSize,
            "configured target GPU changed before write");
        return false;
    }
    if (resolved != SERVICE_LOGON_PROFILE_RESOLVED ||
        currentSlot != originalSlot || currentSource != originalSource) {
        set_message(detail, detailSize, "configured logon profile changed before write");
        return false;
    }
    char matchDetail[256] = {};
    if (!desired_settings_match_active_service_intent(
            originalDesired, &currentDesired, matchDetail,
            sizeof(matchDetail))) {
        set_message(detail, detailSize, "configured profile changed: %s",
            matchDetail[0] ? matchDetail : "intent differs");
        return false;
    }
    return true;
}

static bool service_lifecycle_target_gpu_still_matches(
    const GpuAdapterInfo* expected,
    const GpuAdapterInfo* current)
{
    if (!expected || !current || !expected->valid || !current->valid) return false;
    if (expected->pciInfoValid && current->pciInfoValid) {
        return gpu_adapter_has_same_pci_identity(expected, current);
    }
    // Weak ordinal identity is safe only on an unambiguous one-adapter system.
    return g_app.adapterCount == 1 &&
        expected->nvapiIndex == current->nvapiIndex;
}

static bool service_lifecycle_select_context_gpu(
    const ServiceSessionConfigContext* context,
    GpuAdapterInfo* targetOut,
    char* detail,
    size_t detailSize)
{
    if (targetOut) memset(targetOut, 0, sizeof(*targetOut));
    if (!context) {
        set_message(detail, detailSize, "configured GPU context is missing");
        return false;
    }
    unsigned int resolvedIndex = context->selectedGpuIndex;
    ConfiguredGpuResolveResult resolution = resolve_configured_gpu_selection(
        &context->configuredGpu, g_app.adapters, g_app.adapterCount,
        &resolvedIndex);
    if (resolution == CONFIGURED_GPU_RESOLVE_LEGACY_ORDINAL &&
        g_app.adapterCount > 1) {
        set_message(detail, detailSize,
            "legacy GPU ordinal is unsafe on a multi-adapter system; select the GPU again or republish the shared profile to persist its PCI identity");
        return false;
    }
    if (resolution == CONFIGURED_GPU_RESOLVE_NOT_FOUND ||
        resolution == CONFIGURED_GPU_RESOLVE_AMBIGUOUS) {
        set_message(detail, detailSize,
            "configured GPU identity is %s",
            resolution == CONFIGURED_GPU_RESOLVE_AMBIGUOUS
                ? "ambiguous" : "not present");
        return false;
    }
    if (resolvedIndex >= g_app.adapterCount ||
        resolvedIndex >= MAX_GPU_ADAPTERS) {
        set_message(detail, detailSize,
            "configured GPU index %u is not available",
            resolvedIndex);
        return false;
    }
    GpuAdapterInfo target = g_app.adapters[resolvedIndex];
    if (!target.valid) {
        set_message(detail, detailSize, "configured GPU is not valid");
        return false;
    }
    if (!target.pciInfoValid && g_app.adapterCount > 1) {
        set_message(detail, detailSize,
            "configured GPU identity is ambiguous on a multi-adapter system");
        return false;
    }
    if (target.pciInfoValid &&
        !gpu_adapter_has_valid_pci_location(&target) &&
        !gpu_adapter_pci_base_identity_is_unique(
            &target, g_app.adapters, g_app.adapterCount)) {
        set_message(detail, detailSize,
            "configured GPU PCI identity is duplicated and has no BDF");
        return false;
    }
    bool change = !g_app.selectedGpuIdentityValid ||
        g_app.selectedGpuIndex != target.nvapiIndex ||
        (target.pciInfoValid && g_app.selectedGpu.pciInfoValid &&
         !gpu_adapter_has_same_pci_identity(&g_app.selectedGpu, &target));
    if (change) {
        debug_log("lifecycle logon: selecting immutable context GPU nvapi=%u nvml=%u pci=%d name=%s\n",
            target.nvapiIndex, target.nvmlIndex,
            target.pciInfoValid ? 1 : 0,
            target.name[0] ? target.name : "<unnamed>");
        reset_gpu_runtime_selection();
        g_app.selectedGpuIndex = target.nvapiIndex;
        g_app.selectedNvmlIndex = target.nvmlIndex;
        g_app.selectedGpuExplicit = true;
        g_app.selectedGpu = target;
        g_app.selectedGpuIdentityValid = true;
        g_app.selectedGpuOrdinalFallback = !target.pciInfoValid;
    }
    // Freeze the immutable per-session target through the final apply.  This
    // prevents a concurrent/global g_app.configPath change from making a later
    // hardware_initialize() re-read another account's selected_index.
    g_app.selectedGpuExplicit = true;
    if (targetOut) *targetOut = target;
    return true;
}

static void service_lifecycle_attempt_logon() {
    ServiceLifecycleIdentity identity = {};
    gc_u64 generation = 0;
    ServiceLifecycleTrigger trigger = SERVICE_LIFECYCLE_TRIGGER_NONE;
    LONGLONG logoffGeneration = 0;
    if (!service_lifecycle_pending_logon_snapshot(
            &identity, &generation, &trigger, &logoffGeneration)) return;

    char detail[256] = {};
    ServiceSessionIdentityCheckResult identityCheck =
        service_verify_active_session_identity(&identity, nullptr,
            detail, sizeof(detail));
    if (identityCheck == SERVICE_SESSION_IDENTITY_TRANSIENT) {
        EnterCriticalSection(&g_appLock);
        service_lifecycle_set_result_locked(trigger,
            SERVICE_LIFECYCLE_RESULT_TRANSIENT_NOT_READY);
        LeaveCriticalSection(&g_appLock);
        debug_log("lifecycle logon: identity prerequisite not ready: %s\n", detail);
        return;
    }
    if (identityCheck == SERVICE_SESSION_IDENTITY_SUPERSEDED) {
        service_lifecycle_finish_logon_without_write(&identity, generation, trigger,
            SERVICE_LIFECYCLE_RESULT_SUPERSEDED);
        debug_log("lifecycle logon: identity superseded before config resolve: %s\n", detail);
        return;
    }

    ServiceSessionConfigContext context = {};
    if (!service_resolve_session_config_context(identity.sessionId, &context,
            detail, sizeof(detail))) {
        EnterCriticalSection(&g_appLock);
        service_lifecycle_set_result_locked(trigger,
            SERVICE_LIFECYCLE_RESULT_TRANSIENT_NOT_READY);
        LeaveCriticalSection(&g_appLock);
        debug_log("lifecycle logon: profile context not ready: %s\n", detail);
        return;
    }
    if (!service_lifecycle_identity_equal(&identity, &context.identity)) {
        service_lifecycle_finish_logon_without_write(&identity, generation, trigger,
            SERVICE_LIFECYCLE_RESULT_SUPERSEDED);
        return;
    }

    DesiredSettings desired = {};
    int slot = 0;
    ServiceProfileSource profileSource = SERVICE_PROFILE_SOURCE_NONE;
    // Arm the watches before the first selector read.  A post-read sample below
    // detects a rename/write that raced this attempt, eliminating the startup
    // lost-wakeup that previously made an early task handoff disappear forever.
    service_lifecycle_watch_config_context(&context);
    ServiceLogonProfileResolveResult profileResult =
        service_load_logon_profile_from_context(&context, &desired,
            &slot, &profileSource);
    if (service_lifecycle_consume_config_change_if_signaled()) {
        profileResult = SERVICE_LOGON_PROFILE_TRANSIENT;
        debug_log("lifecycle logon: config changed while resolving profile; retaining intent for a clean reread\n");
        // consume_config_change_if_signaled() rearms the directory handle.  The
        // change we just consumed may have been the only one, so queue an
        // immediate clean pass rather than waiting forever for a second edit.
        service_lifecycle_signal();
    }
    if (profileResult == SERVICE_LOGON_PROFILE_TRANSIENT) {
        EnterCriticalSection(&g_appLock);
        service_lifecycle_set_result_locked(trigger,
            SERVICE_LIFECYCLE_RESULT_TRANSIENT_NOT_READY);
        LeaveCriticalSection(&g_appLock);
        return;
    }
    if (profileResult == SERVICE_LOGON_PROFILE_NONE) {
        service_lifecycle_finish_logon_without_write(&identity, generation, trigger,
            SERVICE_LIFECYCLE_RESULT_NO_PROFILE);
        return;
    }
    if (profileResult != SERVICE_LOGON_PROFILE_RESOLVED) {
        service_lifecycle_finish_logon_without_write(&identity, generation, trigger,
            SERVICE_LIFECYCLE_RESULT_FAILED);
        return;
    }

    DWORD lockoutReason = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
    if (service_auto_restore_is_locked_out(&lockoutReason)) {
        EnterCriticalSection(&g_appLock);
        ServiceLifecycleEvent lockout = {};
        lockout.type = SERVICE_LIFECYCLE_EVENT_LOCKOUT;
        ServiceLifecycleDecision decision =
            service_lifecycle_reduce_locked(&lockout);
        service_lifecycle_set_result_locked(trigger, decision.result);
        LeaveCriticalSection(&g_appLock);
        return;
    }

    lock_service_runtime();
    lockoutReason = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
    if (service_auto_restore_is_locked_out(&lockoutReason)) {
        unlock_service_runtime();
        EnterCriticalSection(&g_appLock);
        ServiceLifecycleEvent lockout = {};
        lockout.type = SERVICE_LIFECYCLE_EVENT_LOCKOUT;
        ServiceLifecycleDecision decision =
            service_lifecycle_reduce_locked(&lockout);
        service_lifecycle_set_result_locked(trigger, decision.result);
        LeaveCriticalSection(&g_appLock);
        debug_log("lifecycle logon: sticky lockout appeared while waiting for runtime serialization; no write\n");
        return;
    }
    bool writeAuthorized = false;
    bool writeAttempted = false;
    bool success = false;
    do {
        if (!service_lifecycle_logon_still_current(&identity, generation)) break;
        identityCheck = service_verify_active_session_identity(
            &identity, nullptr, detail, sizeof(detail));
        if (identityCheck != SERVICE_SESSION_IDENTITY_MATCH) break;
        if (nvml_crash_recovery_active()) {
            set_message(detail, sizeof(detail), "driver recovery is active");
            break;
        }
        if (!hardware_initialize(detail, sizeof(detail)) ||
            !g_app.loaded || g_app.numPopulated <= 0) {
            break;
        }
        GpuAdapterInfo targetGpu = {};
        if (!service_lifecycle_select_context_gpu(&context, &targetGpu,
                detail, sizeof(detail)) || g_app.deviceRemoved) {
            if (!detail[0]) {
                set_message(detail, sizeof(detail), "selected GPU is not ready");
            }
            break;
        }
        if (!g_app.loaded && !hardware_initialize(detail, sizeof(detail))) {
            break;
        }
        if (!service_lifecycle_revalidate_logon_context(&context, &desired,
                slot, profileSource, detail, sizeof(detail))) {
            break;
        }
        identityCheck = service_verify_active_session_identity(
            &identity, nullptr, detail, sizeof(detail));
        GpuAdapterInfo currentTarget = g_app.selectedGpu;
        if (!currentTarget.valid &&
            g_app.selectedGpuIndex < g_app.adapterCount) {
            currentTarget = g_app.adapters[g_app.selectedGpuIndex];
        }
        if (identityCheck != SERVICE_SESSION_IDENTITY_MATCH ||
            g_app.deviceRemoved ||
            !service_lifecycle_target_gpu_still_matches(
                &targetGpu, &currentTarget)) {
            set_message(detail, sizeof(detail),
                "session or selected GPU changed before write");
            break;
        }
        if (!service_lifecycle_authorize_logon_write(
                &identity, generation, logoffGeneration)) {
            set_message(detail, sizeof(detail), "logon work was superseded");
            break;
        }
        writeAuthorized = true;

        bool hadPreviousIntent = g_serviceHasActiveDesired;
        GpuAdapterInfo previousRestoreTarget =
            g_serviceActiveDesiredGpu;
        service_refresh_selected_gpu_notification_best_effort(
            &currentTarget, "lifecycle logon pre-write target");
        ServiceSelectedGpuWriteEpoch gpuEpoch =
            service_selected_gpu_capture_write_epoch();
        DesiredSettings applyRequest = {};
        service_build_profile_transition_request(
            g_serviceHasActiveDesired ? &g_serviceActiveDesired : nullptr,
            &desired, &applyRequest);
        char result[512] = {};
        if (service_selected_gpu_write_epoch_is_current(gpuEpoch)) {
            success = service_apply_desired_settings(&applyRequest, false,
                result, sizeof(result), &writeAttempted, true, &desired);
        } else {
            StringCchCopyA(result, ARRAY_COUNT(result),
                "Selected GPU changed/was removed immediately before logon write");
        }
        if (!writeAttempted && hadPreviousIntent) {
            service_refresh_selected_gpu_notification_best_effort(
                &previousRestoreTarget,
                "restore prior target after logon preflight loss");
        }
        StringCchCopyA(detail, ARRAY_COUNT(detail),
            result[0] ? result : (success ? "applied" : "apply failed"));
        if (success) {
            service_refresh_selected_gpu_notification_best_effort(
                &g_serviceActiveDesiredGpu,
                "successful lifecycle logon target");
            service_capture_owner_identity("logon automation",
                identity.sessionId);
            EnterCriticalSection(&g_appLock);
            g_serviceActiveProfileSource = profileSource;
            g_serviceActiveProfileSlot = (unsigned int)slot;
            LeaveCriticalSection(&g_appLock);
            service_write_restart_reapply_snapshot();
            service_record_oc_apply_stamp();
        } else if (writeAttempted) {
            EnterCriticalSection(&g_appLock);
            ServiceLifecycleEvent lockout = {};
            lockout.type = SERVICE_LIFECYCLE_EVENT_LOCKOUT;
            service_lifecycle_reduce_locked(&lockout);
            LeaveCriticalSection(&g_appLock);
            service_disable_automatic_restore(
                SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED,
                "configured logon profile write failed");
        }
    } while (false);
    unlock_service_runtime();

    if (writeAuthorized) {
        service_lifecycle_complete_logon_write(&identity, success,
            writeAttempted, trigger);
        debug_log("lifecycle logon: hardware write slot=%d source=%u attempted=%d success=%d detail=%s\n",
            slot, (unsigned int)profileSource, writeAttempted ? 1 : 0,
            success ? 1 : 0,
            detail[0] ? detail : "none");
    } else if (identityCheck == SERVICE_SESSION_IDENTITY_SUPERSEDED) {
        service_lifecycle_finish_logon_without_write(&identity, generation, trigger,
            SERVICE_LIFECYCLE_RESULT_SUPERSEDED);
    } else {
        EnterCriticalSection(&g_appLock);
        service_lifecycle_set_result_locked(trigger,
            SERVICE_LIFECYCLE_RESULT_TRANSIENT_NOT_READY);
        LeaveCriticalSection(&g_appLock);
        debug_log("lifecycle logon: prerequisites remain pending without hardware mutation: %s\n",
            detail[0] ? detail : "not ready");
    }
}

static void service_lifecycle_attempt_standby_restore() {
    EnterCriticalSection(&g_appLock);
    bool pending = g_serviceLifecycleState.standbyPending &&
        !g_serviceLifecycleState.standbyWriteIssued &&
        !g_serviceLifecycleState.driverPending &&
        !g_serviceLifecycleState.lockedOut;
    LeaveCriticalSection(&g_appLock);
    if (!pending) return;

    DWORD lockoutReason = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
    if (service_auto_restore_is_locked_out(&lockoutReason)) {
        EnterCriticalSection(&g_appLock);
        ServiceLifecycleEvent event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_LOCKOUT;
        service_lifecycle_reduce_locked(&event);
        LeaveCriticalSection(&g_appLock);
        return;
    }

    DesiredSettings desired = {};
    GpuAdapterInfo target = {};
    lock_service_runtime();
    lockoutReason = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
    if (service_auto_restore_is_locked_out(&lockoutReason)) {
        unlock_service_runtime();
        EnterCriticalSection(&g_appLock);
        ServiceLifecycleEvent event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_LOCKOUT;
        service_lifecycle_reduce_locked(&event);
        LeaveCriticalSection(&g_appLock);
        debug_log("lifecycle standby: sticky lockout appeared while waiting for runtime serialization; no write\n");
        return;
    }
    if (!g_serviceHasActiveDesired) {
        unlock_service_runtime();
        EnterCriticalSection(&g_appLock);
        g_serviceLifecycleState.standbyPending = false;
        g_serviceLastLifecycleTrigger = SERVICE_LIFECYCLE_TRIGGER_STANDBY_RESUME;
        g_serviceLastLifecycleResult = SERVICE_LIFECYCLE_RESULT_NO_PROFILE;
        LeaveCriticalSection(&g_appLock);
        debug_log("lifecycle standby: no in-memory active intent; no write\n");
        return;
    }
    service_build_full_restore_request(&g_serviceActiveDesired, &desired);
    target = g_serviceActiveDesiredGpu;
    char detail[512] = {};
    bool ready = hardware_initialize(detail, sizeof(detail));
    if (ready && target.valid) {
        ready = service_select_restart_reapply_gpu(&target,
            detail, sizeof(detail));
    }

    ServiceLifecycleDecision authorization = {};
    if (ready && !g_app.deviceRemoved) {
        ServiceLifecycleEvent start = {};
        start.type = SERVICE_LIFECYCLE_EVENT_STANDBY_WRITE_STARTED;
        EnterCriticalSection(&g_appLock);
        if (!service_lifecycle_selected_gpu_recovery_cue_pending_locked()) {
            authorization = service_lifecycle_reduce_locked(&start);
        }
        LeaveCriticalSection(&g_appLock);
    }

    bool success = false;
    bool writeAuthorized = authorization.authorizeStandbyWrite != 0;
    bool writeAttempted = false;
    ServiceOcApplyProofStamp matureProof = {};
    ULONGLONG matureProofAgeMs = 0;
    bool preserveMatureProof = false;
    bool proofCommitOk = false;
    if (writeAuthorized) {
        preserveMatureProof = service_capture_mature_oc_apply_proof(
            &matureProof, &matureProofAgeMs);
        GpuAdapterInfo standbyTarget = g_app.selectedGpu.valid
            ? g_app.selectedGpu : target;
        service_refresh_selected_gpu_notification_best_effort(
            &standbyTarget, "standby restore pre-write target");
        ServiceSelectedGpuWriteEpoch gpuEpoch =
            service_selected_gpu_capture_write_epoch();
        if (service_selected_gpu_write_epoch_is_current(gpuEpoch)) {
            success = service_apply_desired_settings(&desired, false,
                detail, sizeof(detail), &writeAttempted, true);
        } else {
            StringCchCopyA(detail, ARRAY_COUNT(detail),
                "Selected GPU changed/was removed immediately before standby restore");
        }
        if (success) {
            service_refresh_selected_gpu_notification_best_effort(
                &g_serviceActiveDesiredGpu,
                "successful standby restore target");
            if (preserveMatureProof) {
                proofCommitOk = service_restore_mature_oc_apply_proof(
                    matureProof);
            } else {
                proofCommitOk = service_record_oc_apply_stamp();
            }
            service_write_restart_reapply_snapshot();
        } else if (writeAttempted) {
            EnterCriticalSection(&g_appLock);
            ServiceLifecycleEvent lockout = {};
            lockout.type = SERVICE_LIFECYCLE_EVENT_LOCKOUT;
            service_lifecycle_reduce_locked(&lockout);
            LeaveCriticalSection(&g_appLock);
            service_disable_automatic_restore(
                SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED,
                "standby resume restore write failed");
        }
    }
    unlock_service_runtime();

    if (writeAuthorized) {
        ServiceLifecycleEvent finish = {};
        finish.type = SERVICE_LIFECYCLE_EVENT_STANDBY_WRITE_FINISHED;
        finish.success = success;
        finish.writeAttempted = writeAttempted;
        EnterCriticalSection(&g_appLock);
        ServiceLifecycleDecision decision = service_lifecycle_reduce_locked(&finish);
        service_lifecycle_set_result_locked(
            SERVICE_LIFECYCLE_TRIGGER_STANDBY_RESUME, decision.result);
        LeaveCriticalSection(&g_appLock);
        debug_log("lifecycle standby: full-intent restore attempted=%d success=%d proof=%s priorAgeMs=%llu detail=%s\n",
            writeAttempted ? 1 : 0, success ? 1 : 0,
            !success ? "not-committed" : !proofCommitOk
                ? "commit-failed-lockout" : preserveMatureProof
                ? "preserved-mature" : "restarted",
            (unsigned long long)matureProofAgeMs,
            detail[0] ? detail : "none");
    } else {
        // Readiness did not authorize a hardware write.  Keep the single
        // generation pending until a real PnP/readiness signal arrives.
        debug_log("lifecycle standby: GPU prerequisite not ready; restore remains pending: %s\n",
            detail[0] ? detail : "unknown");
    }
}

static void service_lifecycle_attempt_driver_restore() {
    EnterCriticalSection(&g_appLock);
    bool pending = g_serviceLifecycleState.driverPending &&
        !g_serviceLifecycleState.driverWriteIssued &&
        !g_serviceLifecycleState.lockedOut;
    LeaveCriticalSection(&g_appLock);
    if (!pending) return;

    // All continuation/proof checks and any fail-closed state transition are
    // serialized with explicit Apply/Reset. A pre-lock proof read could race a
    // newly successful explicit Apply and erase the intent it just published.
    char detail[512] = {};
    lock_service_runtime();
    // Selected-device removal in the old process only establishes recovery
    // precedence and prepares the nonce-bound restart. It is never permission
    // for that stale-DLL process to write. The fresh service sets this flag
    // only after synchronously validating helper, nonce, parent, snapshot,
    // freshness, and SCM demand-start reason.
    if (!g_serviceControlledRecoveryValidated) {
        unlock_service_runtime();
        debug_log("lifecycle driver recovery: old/non-validated or explicitly superseded process will not write\n");
        return;
    }
    DWORD lockoutReason = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
    ULONGLONG proofAgeMs = 0;
    if (!service_auto_restore_allowed_after_driver_event(
            &lockoutReason, &proofAgeMs)) {
        service_disable_automatic_restore(
            lockoutReason != SERVICE_AUTO_RESTORE_LOCKOUT_NONE
                ? lockoutReason
                : SERVICE_AUTO_RESTORE_LOCKOUT_UNSTABLE_APPLY,
            "driver restore authorization changed while waiting for runtime serialization");
        EnterCriticalSection(&g_appLock);
        ServiceLifecycleEvent lockout = {};
        lockout.type = SERVICE_LIFECYCLE_EVENT_LOCKOUT;
        service_lifecycle_reduce_locked(&lockout);
        LeaveCriticalSection(&g_appLock);
        unlock_service_runtime();
        return;
    }
    if (!g_serviceHasActiveDesired || !g_serviceActiveDesiredGpu.valid) {
        service_disable_automatic_restore(
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED,
            "validated recovery intent lost its owned GPU identity");
        unlock_service_runtime();
        return;
    }
    bool ready = hardware_initialize(detail, sizeof(detail)) &&
        !g_app.deviceRemoved;
    if (ready) {
        ready = service_select_restart_reapply_gpu(
            &g_serviceActiveDesiredGpu, detail, sizeof(detail));
    }
    if (!ready) {
        unlock_service_runtime();
        debug_log("lifecycle driver recovery: GPU prerequisite not ready; waiting for a real PnP/readiness signal: %s\n",
            detail[0] ? detail : "unknown");
        return;
    }

    ServiceLifecycleEvent start = {};
    start.type = SERVICE_LIFECYCLE_EVENT_DRIVER_WRITE_STARTED;
    start.driverProofReady = true;
    start.controlledRecoveryValidated = true;
    EnterCriticalSection(&g_appLock);
    ServiceLifecycleDecision authorization = service_lifecycle_reduce_locked(&start);
    LeaveCriticalSection(&g_appLock);
    if (!authorization.authorizeDriverWrite) {
        unlock_service_runtime();
        return;
    }

    DesiredSettings desired = {};
    service_build_full_restore_request(
        &g_serviceActiveDesired, &desired);
    bool writeAttempted = false;
    GpuAdapterInfo driverTarget = g_app.selectedGpu.valid
        ? g_app.selectedGpu : g_serviceActiveDesiredGpu;
    service_refresh_selected_gpu_notification_best_effort(
        &driverTarget, "driver recovery pre-write target");
    ServiceSelectedGpuWriteEpoch gpuEpoch =
        service_selected_gpu_capture_write_epoch();
    bool success = false;
    if (service_selected_gpu_write_epoch_is_current(gpuEpoch)) {
        success = service_apply_desired_settings(&desired, false,
            detail, sizeof(detail), &writeAttempted, true);
    } else {
        StringCchCopyA(detail, ARRAY_COUNT(detail),
            "Selected GPU changed/was removed immediately before driver restore");
    }
    if (success) {
        service_refresh_selected_gpu_notification_best_effort(
            &g_serviceActiveDesiredGpu,
            "successful driver recovery target");
        service_record_oc_apply_stamp();
        g_serviceActiveProfileSource =
            g_serviceControlledRecoveryProfileSource;
        g_serviceActiveProfileSlot =
            g_serviceControlledRecoveryProfileSlot;
        service_write_restart_reapply_snapshot();
        g_serviceControlledRecoveryValidated = false;
        memset(&g_serviceControlledRecoveryDesired, 0,
            sizeof(g_serviceControlledRecoveryDesired));
        memset(&g_serviceControlledRecoveryTargetGpu, 0,
            sizeof(g_serviceControlledRecoveryTargetGpu));
        g_serviceControlledRecoveryProfileSource =
            SERVICE_PROFILE_SOURCE_NONE;
        g_serviceControlledRecoveryProfileSlot = 0;
        InterlockedExchange(&g_serviceReapplyInProgress, 0);
    } else if (writeAttempted) {
        // Serialize the terminal failure with explicit Apply: this runtime lock
        // is still held, so a later explicit success wins chronologically.
        EnterCriticalSection(&g_appLock);
        ServiceLifecycleEvent lockout = {};
        lockout.type = SERVICE_LIFECYCLE_EVENT_LOCKOUT;
        service_lifecycle_reduce_locked(&lockout);
        LeaveCriticalSection(&g_appLock);
        service_disable_automatic_restore(
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED,
            "controlled driver-recovery hardware write failed");
    }
    unlock_service_runtime();

    ServiceLifecycleEvent finish = {};
    finish.type = SERVICE_LIFECYCLE_EVENT_DRIVER_WRITE_FINISHED;
    finish.success = success;
    finish.writeAttempted = writeAttempted;
    EnterCriticalSection(&g_appLock);
    ServiceLifecycleDecision decision = service_lifecycle_reduce_locked(&finish);
    service_lifecycle_set_result_locked(
        SERVICE_LIFECYCLE_TRIGGER_DRIVER_RECOVERY, decision.result);
    LeaveCriticalSection(&g_appLock);
    debug_log("lifecycle driver recovery: full-intent write attempted=%d success=%d detail=%s\n",
        writeAttempted ? 1 : 0, success ? 1 : 0,
        detail[0] ? detail : "none");
    if (success) {
        // Driver recovery has released precedence. Revisit any coalesced logon
        // or standby intent in a new reducer pass rather than writing it from
        // the stale-DLL process or before the controlled restore.
        service_lifecycle_signal();
    }
}
