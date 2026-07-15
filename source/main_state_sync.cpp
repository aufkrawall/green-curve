static bool hardware_initialize(char* detail, size_t detailSize) {
    if (g_app.gpuHandle && g_app.loaded && g_app.vfBackend) return true;
    set_last_apply_phase("hardware initialize: begin");
    debug_log("hardware_initialize: (re)initializing GPU backend\n");
    g_app.lastApplyUsedGpuOffset = true;
    if (!nvapi_init()) {
        set_message(detail, detailSize, "Failed to initialize NvAPI");
        set_last_apply_phase("hardware initialize: NvAPI init failed");
        return false;
    }
    set_last_apply_phase("hardware initialize: enumerate GPU");
    if (!nvapi_enum_gpu()) {
        set_message(detail, detailSize, "No NVIDIA GPU found");
        set_last_apply_phase("hardware initialize: no GPU found");
        return false;
    }
    nvapi_get_name();
    nvapi_read_gpu_metadata();
    bool offsetsOk = false;
    set_last_apply_phase("hardware initialize: VF curve readback");
    if (!read_live_curve_snapshot_settled(4, 40, &offsetsOk)) {
        set_message(detail, detailSize, "Failed to read VF curve from GPU");
        set_last_apply_phase("hardware initialize: VF curve read failed");
        return false;
    }
    (void)offsetsOk;
    // Skip refresh_global_state() while recovering from a recent NVML crash
    // (GPU device reconnect / driver restart via restart64.exe).
    // refresh_global_state() issues NVML reads (power limit, clock offsets,
    // fans) that can access-violate while the GPU kernel driver is still in a
    // transitional state after the reconnect — even though NVAPI (used for the
    // VF curve readback above) has already recovered.  A crash here kills
    // whichever thread called hardware_initialize(): the fan runtime recovery
    // thread OR the pipe server thread answering a GUI snapshot, breaking the
    // GUI connection.
    //
    // nvml_crash_recovery_active() is true only while the VEH has flagged a
    // recent stale-handle crash and the driver has not yet settled (the fan
    // runtime thread clears g_nvmlCrashCount the instant a recovery reapply
    // succeeds).  On normal startup it is false and the refresh runs as usual.
    // NOTE: an earlier version checked `g_app.gpuHandle == nullptr` here, but
    // nvapi_enum_gpu() above always sets gpuHandle non-null first, so the skip
    // was dead code and refresh_global_state() always ran — that was the NVML
    // access-violation the recovery loop kept hitting.
    if (nvml_crash_recovery_active()) {
        debug_log("hardware_initialize: skipping global state refresh during NVML crash recovery (crashCount=%ld)\n",
            (long)g_nvmlCrashCount);
    } else {
        set_last_apply_phase("hardware initialize: global state refresh");
        refresh_global_state(detail, detailSize);
    }
    initialize_gui_fan_settings_from_live_state(false);
    // Preserve the service active desired state across reinitializations
    // (e.g. after a driver TDR) so the GUI does not lose track of what
    // the service had applied.
    if (g_serviceHasActiveDesired) {
        debug_log("hardware_initialize: preserving existing service active desired state\n");
    } else {
        g_serviceActiveDesired = {};
        g_serviceActiveDesiredGpu = {};
        g_serviceHasActiveDesired = false;
    }
    // Initialization, telemetry and lifecycle readiness probes are strictly
    // read-only.  A stale/non-effective NVML memory-offset register may be
    // diagnosed and displayed conservatively, but uncertainty is never
    // "corrected" with a hardware write.  Only APPLY/RESET or an authorized
    // lifecycle restoration may mutate GPU state.
    if (!g_serviceHasActiveDesired && g_app.memClockOffsetkHz != 0
        && (g_app.smiMemMaxMHz == 0 || g_app.pstateMemMaxMHz == 0))
    {
        debug_log("hardware_initialize: stale mem VF offset %d kHz detected"
            " (smi=%u pstate=%u); diagnostic only, no write\n",
            g_app.memClockOffsetkHz, g_app.smiMemMaxMHz, g_app.pstateMemMaxMHz);
    }
#ifdef GREEN_CURVE_SERVICE_BINARY
    trim_working_set();
#endif
    set_last_apply_phase("hardware initialize: complete");
    return true;
}

static void populate_service_snapshot_locked(ServiceSnapshot* snapshot,
    DWORD snapshotLockoutReason) {
    if (!snapshot) return;
    memset(snapshot, 0, sizeof(*snapshot));
    int snapshotGpuOffsetMHz = g_app.appliedGpuOffsetMHz;
    int snapshotGpuOffsetExcludeLowCount = (g_app.appliedGpuOffsetExcludeLowCount > 0 && snapshotGpuOffsetMHz != 0) ? g_app.appliedGpuOffsetExcludeLowCount : 0;
    if (g_serviceControlStateValid && control_state_has_meaningful_gpu(&g_serviceControlState)) {
        snapshotGpuOffsetMHz = g_serviceControlState.gpuOffsetMHz;
        snapshotGpuOffsetExcludeLowCount = (g_serviceControlState.gpuOffsetExcludeLowCount > 0 && snapshotGpuOffsetMHz != 0) ? g_serviceControlState.gpuOffsetExcludeLowCount : 0;
    } else {
        int desiredServiceOffsetMHz = 0;
        int desiredServiceExcludeLowCount = 0;
        if (service_active_desired_gpu_offset_fallback(&desiredServiceOffsetMHz, &desiredServiceExcludeLowCount)) {
            snapshotGpuOffsetMHz = desiredServiceOffsetMHz;
            snapshotGpuOffsetExcludeLowCount = desiredServiceExcludeLowCount;
        }
    }
    snapshot->initialized = g_app.gpuHandle != nullptr;
    snapshot->loaded = g_app.loaded;
    snapshot->fanSupported = g_app.fanSupported;
    snapshot->fanRangeKnown = g_app.fanRangeKnown;
    snapshot->fanIsAuto = g_app.fanIsAuto;
    snapshot->fanCurveRuntimeActive = g_app.fanCurveRuntimeActive;
    snapshot->fanFixedRuntimeActive = g_app.fanFixedRuntimeActive;
    snapshot->gpuOffsetRangeKnown = g_app.gpuOffsetRangeKnown;
    snapshot->memOffsetRangeKnown = g_app.memOffsetRangeKnown;
    snapshot->curveOffsetRangeKnown = g_app.curveOffsetRangeKnown;
    snapshot->gpuTemperatureValid = g_app.gpuTemperatureValid;
    snapshot->vfReadSupported = g_app.vfBackend && g_app.vfBackend->readSupported;
    snapshot->vfWriteSupported = g_app.vfBackend && g_app.vfBackend->writeSupported;
    snapshot->vfBestGuess = vf_backend_is_best_guess(g_app.vfBackend);
    snapshot->adapterCount = g_app.adapterCount;
    snapshot->selectedAdapterIndex = g_app.selectedGpuIndex;
    snapshot->selectedAdapterOrdinalFallback = g_app.selectedGpuOrdinalFallback;
    memcpy(snapshot->adapters, g_app.adapters, sizeof(snapshot->adapters));
    snapshot->gpuFamily = g_app.gpuFamily;
    snapshot->numPopulated = g_app.numPopulated;
    snapshot->gpuClockOffsetkHz = g_app.gpuClockOffsetkHz;
    snapshot->memClockOffsetkHz = g_app.memClockOffsetkHz;
    snapshot->gpuClockOffsetMinMHz = g_app.gpuClockOffsetMinMHz;
    snapshot->gpuClockOffsetMaxMHz = g_app.gpuClockOffsetMaxMHz;
    snapshot->memOffsetMinMHz = g_app.memClockOffsetMinMHz;
    snapshot->memOffsetMaxMHz = g_app.memClockOffsetMaxMHz;
    snapshot->curveOffsetMinkHz = g_app.curveOffsetMinkHz;
    snapshot->curveOffsetMaxkHz = g_app.curveOffsetMaxkHz;
    snapshot->powerLimitPct = g_app.powerLimitPct;
    snapshot->powerLimitDefaultmW = g_app.powerLimitDefaultmW;
    snapshot->powerLimitCurrentmW = g_app.powerLimitCurrentmW;
    snapshot->powerLimitMinmW = g_app.powerLimitMinmW;
    snapshot->powerLimitMaxmW = g_app.powerLimitMaxmW;
    snapshot->appliedGpuOffsetMHz = snapshotGpuOffsetMHz;
    snapshot->appliedGpuOffsetExcludeLowCount = snapshotGpuOffsetExcludeLowCount;
    snapshot->lastApplyUsedGpuOffset = g_app.lastApplyUsedGpuOffset;
    // RC7 fix: only report the lock from active desired if the hardware
    // is actually loaded and the reapply has confirmed the settings stuck.
    // When hardware_initialize fails (e.g. nvapi_init FAILED after recovery),
    // g_app.loaded is false and there is no applied lock regardless of what
    // the stale active desired claims.  The hardware state is unknown/stock.
    // While g_serviceReapplyInProgress is set, the GPU was just reconnected
    // and the desired lock has NOT been reapplied — fall through to live
    // curve detection so the GUI does not falsely display a locked tail.
    bool reapplyPending = InterlockedExchangeAdd(&g_serviceReapplyInProgress, 0) != 0;
    bool snapshotLockFromActiveDesired = g_app.isServiceProcess
        && g_app.loaded
        && g_app.gpuHandle
        && g_serviceHasActiveDesired
        && g_serviceActiveDesired.hasLock
        && g_serviceActiveDesired.lockCi >= 0
        && g_serviceActiveDesired.lockCi < VF_NUM_POINTS
        && g_serviceActiveDesired.lockMHz > 0
        && !reapplyPending;
    if (snapshotLockFromActiveDesired) {
        snapshot->hasLock = true;
        snapshot->lockCi = g_serviceActiveDesired.lockCi;
        snapshot->lockMHz = g_serviceActiveDesired.lockMHz;
        snapshot->lockMode = (int)g_serviceActiveDesired.lockMode;
        snapshot->lockTracksAnchor = g_serviceActiveDesired.lockTracksAnchor;
        if (snapshot->lockCi != g_app.lockedCi || snapshot->lockMHz != g_app.lockedFreq) {
            debug_log("populate_service_snapshot: reporting active desired lock ci=%d mhz=%u mode=%s instead of live-detected ci=%d mhz=%u\n",
                snapshot->lockCi,
                snapshot->lockMHz,
                lock_mode_name(g_serviceActiveDesired.lockMode),
                g_app.lockedCi,
                g_app.lockedFreq);
        }
    } else {
        snapshot->hasLock = (g_app.lockedCi >= 0 && g_app.lockedFreq > 0);
        snapshot->lockCi = g_app.lockedCi;
        snapshot->lockMHz = g_app.lockedFreq;
        snapshot->lockMode = (int)g_app.lockMode;
        snapshot->lockTracksAnchor = g_app.guiLockTracksAnchor;
    }
    int snapshotFanMode = current_green_curve_fan_intent_mode();
    int snapshotFanFixedPercent = current_green_curve_fan_intent_fixed_percent();
    const FanCurveConfig* snapshotFanCurve = current_green_curve_fan_intent_curve();
    if (snapshotFanMode == FAN_MODE_AUTO && !g_app.fanIsAuto) {
        debug_log_on_change("fan intent: external live fan policy observed fanIsAuto=0 gcIntent=Auto; preserving Auto in service snapshot\n");
    }
    snapshot->activeFanMode = snapshotFanMode;
    snapshot->activeFanFixedPercent = snapshotFanFixedPercent;
    snapshot->gpuTemperatureC = g_app.gpuTemperatureC;
    snapshot->fanCount = g_app.fanCount;
    snapshot->fanMinPct = g_app.fanMinPct;
    snapshot->fanMaxPct = g_app.fanMaxPct;
    memcpy(snapshot->fanPercent, g_app.fanPercent, sizeof(snapshot->fanPercent));
    memcpy(snapshot->fanTargetPercent, g_app.fanTargetPercent, sizeof(snapshot->fanTargetPercent));
    memcpy(snapshot->fanRpm, g_app.fanRpm, sizeof(snapshot->fanRpm));
    memcpy(snapshot->fanPolicy, g_app.fanPolicy, sizeof(snapshot->fanPolicy));
    memcpy(snapshot->fanControlSignal, g_app.fanControlSignal, sizeof(snapshot->fanControlSignal));
    memcpy(snapshot->fanTargetMask, g_app.fanTargetMask, sizeof(snapshot->fanTargetMask));
    memcpy(snapshot->curve, g_app.curve, sizeof(snapshot->curve));
    memcpy(snapshot->freqOffsets, g_app.freqOffsets, sizeof(snapshot->freqOffsets));
    copy_fan_curve(&snapshot->activeFanCurve, snapshotFanCurve);
    StringCchCopyA(snapshot->gpuName, ARRAY_COUNT(snapshot->gpuName), g_app.gpuName);
    StringCchCopyA(snapshot->ownerUser, ARRAY_COUNT(snapshot->ownerUser), g_app.backgroundServiceOwnerUser);
    snapshot->ownerSessionId = g_app.backgroundServiceOwnerSessionId;
    snapshot->ownerUtcMs = g_app.backgroundServiceOwnerUtcMs;
    // Recovery is now a process-bound controlled continuation rather than an
    // in-process reload/thread.  Keep the legacy snapshot fields meaningful
    // without reviving the removed cooldown/timestamp state.
    bool reapplyInProgress =
        InterlockedExchangeAdd(&g_serviceReapplyInProgress, 0) != 0;
    snapshot->serviceInRecovery = reapplyInProgress ||
        g_serviceControlledRecoveryValidated ||
        InterlockedExchangeAdd(&g_serviceRestartPreparing, 0) != 0 ||
        InterlockedExchangeAdd(&g_serviceRestartRequested, 0) != 0;
    snapshot->lastRecoveryTickMs = 0;
    snapshot->serviceReapplyInProgress = reapplyInProgress;
    snapshot->activeProfileSource = (gc_u32)g_serviceActiveProfileSource;
    snapshot->activeProfileSlot = (gc_u32)g_serviceActiveProfileSlot;
    snapshot->lastLifecycleTrigger = (gc_u32)g_serviceLastLifecycleTrigger;
    snapshot->lastLifecycleResult = (gc_u32)g_serviceLastLifecycleResult;
    snapshot->autoRestoreLockoutReason = snapshotLockoutReason;
}

static void populate_service_snapshot(ServiceSnapshot* snapshot) {
    if (!snapshot) return;
    DWORD snapshotLockoutReason = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
    service_auto_restore_is_locked_out(&snapshotLockoutReason);
    EnterCriticalSection(&g_appLock);
    populate_service_snapshot_locked(snapshot, snapshotLockoutReason);
    LeaveCriticalSection(&g_appLock);
}

static void populate_control_state_locked(ControlState* state) {
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->valid = true;
    state->hasGpuOffset = true;
    state->gpuOffsetMHz = current_applied_gpu_offset_mhz();
    state->gpuOffsetExcludeLowCount = (current_applied_gpu_offset_excludes_low_points() && state->gpuOffsetMHz != 0) ? g_app.appliedGpuOffsetExcludeLowCount : 0;
    state->hasMemOffset = true;
    state->memOffsetMHz = mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz);
    state->hasPowerLimit = true;
    state->powerLimitPct = g_app.powerLimitPct;
    state->hasFan = true;
    state->fanMode = current_green_curve_fan_intent_mode();
    state->fanFixedPercent = current_green_curve_fan_intent_fixed_percent();
    state->fanCurrentPercent = current_displayed_fan_percent();
    state->fanCurrentTemperatureC = g_app.gpuTemperatureValid ? g_app.gpuTemperatureC : 0;
    copy_fan_curve(&state->fanCurve, current_green_curve_fan_intent_curve());
    ensure_valid_fan_curve_config(&state->fanCurve);
}

static void populate_control_state(ControlState* state) {
    if (!state) return;
    EnterCriticalSection(&g_appLock);
    populate_control_state_locked(state);
    LeaveCriticalSection(&g_appLock);
}

#include "main_service_state_envelope.cpp"

static void apply_service_snapshot_to_app(const ServiceSnapshot* snapshot) {
    if (!snapshot) return;
    EnterCriticalSection(&g_appLock);
#ifndef GREEN_CURVE_SERVICE_BINARY
    GpuAdapterInfo previousSelectedGpu = g_app.selectedGpu;
#endif
    g_app.serviceSnapshotAuthoritative = true;
    g_app.serviceActiveProfileSource = (ServiceProfileSource)snapshot->activeProfileSource;
    g_app.serviceActiveProfileSlot = snapshot->activeProfileSlot;
    g_app.serviceLastLifecycleTrigger = (ServiceLifecycleTrigger)snapshot->lastLifecycleTrigger;
    g_app.serviceLastLifecycleResult = (ServiceLifecycleResult)snapshot->lastLifecycleResult;
    g_app.serviceAutoRestoreLockoutReason =
        (ServiceAutoRestoreLockoutReason)snapshot->autoRestoreLockoutReason;
    g_app.loaded = snapshot->loaded;
    g_app.fanSupported = snapshot->fanSupported;
    g_app.fanRangeKnown = snapshot->fanRangeKnown;
    g_app.fanIsAuto = snapshot->fanIsAuto;
    g_app.fanCurveRuntimeActive = snapshot->fanCurveRuntimeActive;
    g_app.fanFixedRuntimeActive = snapshot->fanFixedRuntimeActive;
    g_app.gpuOffsetRangeKnown = snapshot->gpuOffsetRangeKnown;
    g_app.memOffsetRangeKnown = snapshot->memOffsetRangeKnown;
    g_app.curveOffsetRangeKnown = snapshot->curveOffsetRangeKnown;
    g_app.gpuTemperatureValid = snapshot->gpuTemperatureValid;
    g_app.adapterCount = snapshot->adapterCount > MAX_GPU_ADAPTERS ? MAX_GPU_ADAPTERS : snapshot->adapterCount;
    g_app.selectedGpuIndex = snapshot->selectedAdapterIndex;
    g_app.selectedNvmlIndex = snapshot->selectedAdapterIndex;
    g_app.selectedGpuOrdinalFallback = snapshot->selectedAdapterOrdinalFallback;
    memcpy(g_app.adapters, snapshot->adapters, sizeof(g_app.adapters));
    if (g_app.selectedGpuIndex < g_app.adapterCount) {
        g_app.selectedGpu = g_app.adapters[g_app.selectedGpuIndex];
        g_app.selectedGpuIdentityValid = g_app.selectedGpu.valid;
    }
#ifndef GREEN_CURVE_SERVICE_BINARY
    bool selectedGpuChanged = previousSelectedGpu.valid != g_app.selectedGpu.valid ||
        previousSelectedGpu.deviceId != g_app.selectedGpu.deviceId ||
        previousSelectedGpu.subSystemId != g_app.selectedGpu.subSystemId ||
        previousSelectedGpu.pciRevisionId != g_app.selectedGpu.pciRevisionId ||
        previousSelectedGpu.extDeviceId != g_app.selectedGpu.extDeviceId ||
        previousSelectedGpu.pciDomain != g_app.selectedGpu.pciDomain ||
        previousSelectedGpu.pciBus != g_app.selectedGpu.pciBus ||
        previousSelectedGpu.pciDevice != g_app.selectedGpu.pciDevice ||
        previousSelectedGpu.pciFunction != g_app.selectedGpu.pciFunction;
#endif
    g_app.gpuFamily = snapshot->gpuFamily;
    g_app.numPopulated = snapshot->numPopulated;
    g_app.gpuClockOffsetkHz = snapshot->gpuClockOffsetkHz;
    g_app.memClockOffsetkHz = snapshot->memClockOffsetkHz;
    g_app.gpuClockOffsetMinMHz = snapshot->gpuClockOffsetMinMHz;
    g_app.gpuClockOffsetMaxMHz = snapshot->gpuClockOffsetMaxMHz;
    g_app.memClockOffsetMinMHz = snapshot->memOffsetMinMHz;
    g_app.memClockOffsetMaxMHz = snapshot->memOffsetMaxMHz;
    g_app.curveOffsetMinkHz = snapshot->curveOffsetMinkHz;
    g_app.curveOffsetMaxkHz = snapshot->curveOffsetMaxkHz;
    g_app.powerLimitPct = snapshot->powerLimitPct;
    g_app.powerLimitDefaultmW = snapshot->powerLimitDefaultmW;
    g_app.powerLimitCurrentmW = snapshot->powerLimitCurrentmW;
    g_app.powerLimitMinmW = snapshot->powerLimitMinmW;
    g_app.powerLimitMaxmW = snapshot->powerLimitMaxmW;
    g_app.appliedGpuOffsetMHz = snapshot->appliedGpuOffsetMHz;
    g_app.appliedGpuOffsetExcludeLowCount =
        snapshot->appliedGpuOffsetMHz != 0
            ? snapshot->appliedGpuOffsetExcludeLowCount : 0;
    g_app.lastApplyUsedGpuOffset = snapshot->lastApplyUsedGpuOffset;
    g_app.activeFanMode = snapshot->activeFanMode;
    g_app.activeFanFixedPercent = snapshot->activeFanFixedPercent;
    g_app.gpuTemperatureC = snapshot->gpuTemperatureC;
    g_app.fanCount = snapshot->fanCount;
    g_app.fanMinPct = snapshot->fanMinPct;
    g_app.fanMaxPct = snapshot->fanMaxPct;
    memcpy(g_app.fanPercent, snapshot->fanPercent, sizeof(g_app.fanPercent));
    memcpy(g_app.fanTargetPercent, snapshot->fanTargetPercent, sizeof(g_app.fanTargetPercent));
    memcpy(g_app.fanRpm, snapshot->fanRpm, sizeof(g_app.fanRpm));
    memcpy(g_app.fanPolicy, snapshot->fanPolicy, sizeof(g_app.fanPolicy));
    memcpy(g_app.fanControlSignal, snapshot->fanControlSignal, sizeof(g_app.fanControlSignal));
    memcpy(g_app.fanTargetMask, snapshot->fanTargetMask, sizeof(g_app.fanTargetMask));
    memcpy(g_app.curve, snapshot->curve, sizeof(g_app.curve));
    memcpy(g_app.freqOffsets, snapshot->freqOffsets, sizeof(g_app.freqOffsets));
    copy_fan_curve(&g_app.activeFanCurve, &snapshot->activeFanCurve);
    StringCchCopyA(g_app.gpuName, ARRAY_COUNT(g_app.gpuName), snapshot->gpuName);
    StringCchCopyA(g_app.backgroundServiceOwnerUser, ARRAY_COUNT(g_app.backgroundServiceOwnerUser), snapshot->ownerUser);
    g_app.backgroundServiceOwnerSessionId = snapshot->ownerSessionId;
    g_app.backgroundServiceOwnerUtcMs = snapshot->ownerUtcMs;
    rebuild_visible_map();
    // F-REL-2e: clear a stale ADOPTED GUI lock when the service has authoritatively
    // reset to no-lock (e.g. reset-to-defaults after an out-of-band restart) and the
    // user has no unsaved edits.  This runs OUTSIDE should_accept_service_curve_lock_
    // detection() — that gate returns false whenever lockedCi>=0, which would
    // otherwise pin the stale lock checkbox / point value / "Lock:" header forever
    // after a reset.  Narrow on purpose: only fires when the snapshot authoritatively
    // reports no lock (a real recovery still reports the active desired lock, so this
    // never fights an in-flight reapply), the user is NOT dirty-editing, and the
    // current GUI lock matches the last applied/adopted lock (never a fresh,
    // unapplied user edit).
    if (snapshot->loaded && !snapshot->hasLock && !gui_state_dirty()
        && g_app.lockedCi >= 0
        && g_app.lockedCi == g_app.appliedLockCi
        && g_app.lockedFreq == g_app.appliedLockFreq) {
        debug_log("apply_service_snapshot_to_app: service reports no lock and GUI is not dirty — "
            "clearing stale adopted GUI lock ci=%d mhz=%u and forcing a full GUI refresh to match the reset state\n",
            g_app.lockedCi, g_app.lockedFreq);
        g_app.lockedCi = -1;
        g_app.lockedFreq = 0;
        g_app.lockMode = LOCK_MODE_NONE;
        g_app.lockedVi = -1;
        g_app.appliedLockCi = -1;
        g_app.appliedLockFreq = 0;
        g_app.appliedLockVi = -1;
        g_app.appliedLockMode = LOCK_MODE_NONE;
        g_app.guiLockTracksAnchor = true;
        // The per-second telemetry poll deliberately does NOT resync the editable
        // curve/lock controls (so it never wipes in-progress edits).  Since we just
        // cleared an adopted lock to follow a service reset, request the same full
        // visual resync the Refresh button performs (graph, per-point fields, lock
        // checkboxes, "Lock:" header) so the WHOLE GUI reflects the default state —
        // not just internal state.  Without this only field state changes and the
        // graph/checkbox/header stay stale until the user presses Refresh.
        g_guiForceFullRefresh = true;
    }
    // Sync lockMode from snapshot when the lock point matches, even when
    // should_accept_service_curve_lock_detection() returns false (which it
    // does when the GUI already has a lock set).  Gate: NEVER while the GUI
    // holds divergent pending lock intent (lockMode != appliedLockMode, e.g.
    // a FLATTEN->HARD checkbox click or a loaded HARD profile at the same
    // point) or unsaved edits — the per-second telemetry snapshot still
    // carries the previously APPLIED mode and would silently revert the
    // user's pin before Apply ("No changes to apply") and corrupt saves.
    if (snapshot->loaded && snapshot->hasLock && snapshot->lockCi >= 0 && snapshot->lockMHz > 0
        && g_app.lockedCi == snapshot->lockCi && g_app.lockedFreq == snapshot->lockMHz
        && (g_app.lockMode != (LockMode)snapshot->lockMode || g_app.appliedLockMode != (LockMode)snapshot->lockMode)) {
        if (lock_mode_sync_allowed((int)g_app.lockMode, (int)g_app.appliedLockMode, gui_state_dirty())) {
            g_app.lockMode = (LockMode)snapshot->lockMode;
            g_app.appliedLockMode = (LockMode)snapshot->lockMode;
            debug_log("apply_service_snapshot_to_app: synced lockMode=%s from snapshot (same lock point ci=%d mhz=%u)\n",
                lock_mode_name(g_app.lockMode), g_app.lockedCi, g_app.lockedFreq);
        } else {
            // Per-second telemetry calls this; log only when the skipped
            // state changes so a pending pin doesn't spam the debug log.
            static int lastSkipLogged = -1;
            int skipState = ((int)g_app.lockMode << 8) | ((int)g_app.appliedLockMode << 4) | (snapshot->lockMode & 0xF);
            if (skipState != lastSkipLogged) {
                lastSkipLogged = skipState;
                debug_log("apply_service_snapshot_to_app: lockMode sync skipped (pending lock intent gui=%s applied=%s snapshot=%s dirty=%d ci=%d mhz=%u)\n",
                    lock_mode_name(g_app.lockMode),
                    lock_mode_name(g_app.appliedLockMode),
                    lock_mode_name((LockMode)snapshot->lockMode),
                    gui_state_dirty() ? 1 : 0,
                    g_app.lockedCi, g_app.lockedFreq);
            }
        }
    }
    if (snapshot->loaded && should_accept_service_curve_lock_detection()) {
        if (snapshot->hasLock && snapshot->lockCi >= 0 && snapshot->lockMHz > 0) {
            g_app.lockedCi = snapshot->lockCi;
            g_app.lockedFreq = snapshot->lockMHz;
            g_app.lockMode = (LockMode)snapshot->lockMode;
            g_app.lockedVi = -1;
            for (int vi = 0; vi < g_app.numVisible; vi++) {
                if (g_app.visibleMap[vi] == snapshot->lockCi) {
                    g_app.lockedVi = vi;
                    break;
                }
            }
            g_app.guiLockTracksAnchor = snapshot->lockTracksAnchor;
            g_app.appliedLockVi = g_app.lockedVi;
            g_app.appliedLockCi = g_app.lockedCi;
            g_app.appliedLockFreq = g_app.lockedFreq;
            g_app.appliedLockMode = (LockMode)snapshot->lockMode;
            debug_log("apply_service_snapshot_to_app: adopted service lock ci=%d mhz=%u mode=%s visible=%d\n",
                g_app.lockedCi, g_app.lockedFreq, lock_mode_name(g_app.lockMode), g_app.lockedVi);
        } else {
            // RC7 fix: when the snapshot reports hasLock=false (e.g. after
            // a GPU device reconnect where the reapply hasn't completed, or
            // after a RESET), clear the GUI-side lock FIRST so that
            // detect_locked_tail_from_curve() can properly re-detect from
            // the live curve.  Without this, should_accept_service_curve_
            // lock_detection() would return false (lockedCi >= 0 from old
            // snapshot) and detect_locked_tail_from_curve would see
            // should_auto_detect_locked_tail_from_live_curve() return false,
            // preserving the stale lock indefinitely.
            g_app.lockedCi = -1;
            g_app.lockedFreq = 0;
            g_app.lockMode = LOCK_MODE_NONE;
            g_app.guiLockTracksAnchor = true;
            g_app.lockedVi = -1;
            g_app.appliedLockCi = -1;
            g_app.appliedLockFreq = 0;
            g_app.appliedLockVi = -1;
            g_app.appliedLockMode = LOCK_MODE_NONE;
            detect_locked_tail_from_curve();
        }
    }

    // Sync GUI fan mode to the service snapshot only when the user hasn't
    // explicitly changed it (e.g. after loading a profile but before applying).
    if (!gui_state_dirty()) {
        g_app.guiFanMode = snapshot->activeFanMode;
        if (snapshot->activeFanMode == FAN_MODE_FIXED) {
            g_app.guiFanFixedPercent = clamp_percent(snapshot->activeFanFixedPercent);
        } else {
            g_app.guiFanFixedPercent = clamp_percent(current_displayed_fan_percent());
        }
        ensure_valid_fan_curve_config(&g_app.guiFanCurve);
        if (snapshot->activeFanMode == FAN_MODE_CURVE) {
            copy_fan_curve(&g_app.guiFanCurve, &snapshot->activeFanCurve);
        }
    }
    memset(&g_app.serviceControlState, 0, sizeof(g_app.serviceControlState));
    g_app.serviceControlState.valid = true;
    g_app.serviceControlState.hasGpuOffset = true;
    g_app.serviceControlState.gpuOffsetMHz = snapshot->appliedGpuOffsetMHz;
    g_app.serviceControlState.gpuOffsetExcludeLowCount =
        (snapshot->appliedGpuOffsetExcludeLowCount > 0 &&
         snapshot->appliedGpuOffsetMHz != 0)
            ? snapshot->appliedGpuOffsetExcludeLowCount : 0;
    g_app.serviceControlState.hasMemOffset = true;
    int snapshotMemOffsetMHz = mem_display_mhz_from_driver_khz(snapshot->memClockOffsetkHz);
    g_app.serviceControlState.memOffsetMHz = snapshotMemOffsetMHz;
    g_app.serviceControlState.hasPowerLimit = true;
    g_app.serviceControlState.powerLimitPct = snapshot->powerLimitPct;
    g_app.serviceControlState.hasFan = true;
    g_app.serviceControlState.fanMode = snapshot->activeFanMode;
    g_app.serviceControlState.fanFixedPercent =
        clamp_percent(snapshot->activeFanFixedPercent);
    g_app.serviceControlState.fanCurrentPercent = current_displayed_fan_percent();
    g_app.serviceControlState.fanCurrentTemperatureC =
        snapshot->gpuTemperatureValid ? snapshot->gpuTemperatureC : 0;
    copy_fan_curve(&g_app.serviceControlState.fanCurve,
        &snapshot->activeFanCurve);
    ensure_valid_fan_curve_config(&g_app.serviceControlState.fanCurve);
    log_locked_tail_drift_diagnostics();
    g_app.serviceControlStateValid = true;
    LeaveCriticalSection(&g_appLock);
#ifndef GREEN_CURVE_SERVICE_BINARY
    if (selectedGpuChanged)
        gui_mutation_advance_gpu_epoch("service snapshot GPU identity");
    sync_applied_profile_from_service_metadata();
    // Auto-save the GPU selection for single-GPU systems when no [gpu]
    // section exists yet.  The GPU selector combo is disabled when there is
    // only one adapter, so the user cannot trigger a save through the UI.
    // Without this, share/unshare operations fail because they verify that a
    // GPU binding is present in the config before publishing to the shared
    // profile bank.
    if (g_app.adapterCount == 1 && g_app.adapters[0].valid &&
        g_app.configPath[0] && !config_section_has_keys(g_app.configPath, "gpu")) {
        char gpuErr[256] = {};
        if (!save_configured_gpu_selection_atomic(0, gpuErr, sizeof(gpuErr))) {
            debug_log("gpu selection: auto-save failed for single adapter: %s\n",
                gpuErr[0] ? gpuErr : "unknown error");
        }
    }
#endif
}

static void apply_service_desired_to_gui(const DesiredSettings* desired) {
    if (!desired) return;
    if (desired->hasGpuOffset) {
        g_app.appliedGpuOffsetMHz = desired->gpuOffsetMHz;
        g_app.appliedGpuOffsetExcludeLowCount = (desired->gpuOffsetExcludeLowCount > 0 && desired->gpuOffsetMHz != 0) ? desired->gpuOffsetExcludeLowCount : 0;
        if (!gui_state_dirty()) {
            g_app.guiGpuOffsetMHz = desired->gpuOffsetMHz;
            g_app.guiGpuOffsetExcludeLowCount = (desired->gpuOffsetExcludeLowCount > 0 && desired->gpuOffsetMHz != 0) ? desired->gpuOffsetExcludeLowCount : 0;
        }
    }
    if (!gui_state_dirty()) {
        memset(g_app.guiCurvePointExplicit, 0, sizeof(g_app.guiCurvePointExplicit));
        for (int vi = 0; vi < g_app.numVisible; vi++) {
            int ci = g_app.visibleMap[vi];
            if (ci < 0 || ci >= VF_NUM_POINTS) continue;
            g_app.guiCurvePointExplicit[ci] = desired->hasCurvePoint[ci];
            if (desired->hasCurvePoint[ci] && g_app.hEditsMhz[vi]) {
                set_edit_value(g_app.hEditsMhz[vi], desired->curvePointMHz[ci]);
            }
        }
        if (desired->hasLock && desired->lockCi >= 0 && desired->lockMHz > 0) {
            g_app.lockedCi = desired->lockCi;
            g_app.lockedFreq = desired->lockMHz;
            g_app.lockMode = desired->lockMode;
            g_app.lockedVi = -1;
            for (int vi = 0; vi < g_app.numVisible; vi++) {
                if (g_app.visibleMap[vi] == desired->lockCi) {
                    g_app.lockedVi = vi;
                    break;
                }
            }
            g_app.appliedLockVi = g_app.lockedVi;
            g_app.appliedLockCi = g_app.lockedCi;
            g_app.appliedLockFreq = g_app.lockedFreq;
            g_app.appliedLockMode = desired->lockMode;
            g_app.guiLockTracksAnchor = desired->lockTracksAnchor;
        } else if (g_app.lockedFreq == 0 && g_app.lockedCi < 0) {
            g_app.lockedVi = -1;
            g_app.lockedCi = -1;
            g_app.lockedFreq = 0;
            g_app.lockMode = LOCK_MODE_NONE;
            g_app.appliedLockVi = -1;
            g_app.appliedLockCi = -1;
            g_app.appliedLockFreq = 0;
            g_app.appliedLockMode = LOCK_MODE_NONE;
            g_app.guiLockTracksAnchor = true;
        }
    }
    if (desired->hasFan) {
        if (!gui_state_dirty()) {
            g_app.guiFanMode = desired->fanMode;
            if (desired->fanMode == FAN_MODE_FIXED) {
                g_app.guiFanFixedPercent = clamp_percent(desired->fanPercent);
            } else {
                g_app.guiFanFixedPercent = clamp_percent(current_displayed_fan_percent());
            }
            copy_fan_curve(&g_app.guiFanCurve, &desired->fanCurve);
            ensure_valid_fan_curve_config(&g_app.guiFanCurve);
        }
    }
    // Adopt the service's active curve intent as the drift-free baseline, regardless
    // of GUI dirty state (this is what the hardware is actually set to, not live
    // readback). Keeps fan-only detection and the editor/graph accurate across
    // reconnects and telemetry-driven refreshes without importing boost drift.
    capture_applied_curve_baseline(desired);
}

static void apply_control_state_to_gui(const ControlState* state) {
    if (!state || !state->valid) return;
    if (!state->hasGpuOffset && !state->hasMemOffset &&
        !state->hasPowerLimit && !state->hasFan) {
        debug_log("apply_control_state_to_gui: ignoring empty service control update\n");
        return;
    }

    ControlState merged = {};
    if (g_app.serviceControlStateValid) merged = g_app.serviceControlState;
    merged.valid = true;
    if (state->hasGpuOffset) {
        merged.hasGpuOffset = true;
        merged.gpuOffsetMHz = state->gpuOffsetMHz;
        merged.gpuOffsetExcludeLowCount = (state->gpuOffsetExcludeLowCount > 0 && state->gpuOffsetMHz != 0) ? state->gpuOffsetExcludeLowCount : 0;
    }
    if (state->hasMemOffset) {
        merged.hasMemOffset = true;
        merged.memOffsetMHz = state->memOffsetMHz;
    }
    if (state->hasPowerLimit) {
        merged.hasPowerLimit = true;
        merged.powerLimitPct = state->powerLimitPct;
    }
    if (state->hasFan) {
        merged.hasFan = true;
        merged.fanMode = state->fanMode;
        merged.fanFixedPercent = state->fanFixedPercent;
        merged.fanCurrentPercent = state->fanCurrentPercent;
        merged.fanCurrentTemperatureC = state->fanCurrentTemperatureC;
        copy_fan_curve(&merged.fanCurve, &state->fanCurve);
        ensure_valid_fan_curve_config(&merged.fanCurve);
    }
    g_app.serviceControlStateValid = true;
    g_app.serviceControlState = merged;
    bool updateGui = !gui_state_dirty();
    if (state->hasGpuOffset) {
        g_app.appliedGpuOffsetMHz = state->gpuOffsetMHz;
        g_app.appliedGpuOffsetExcludeLowCount = (state->gpuOffsetExcludeLowCount > 0 && state->gpuOffsetMHz != 0) ? state->gpuOffsetExcludeLowCount : 0;
        if (updateGui) {
            g_app.guiGpuOffsetMHz = state->gpuOffsetMHz;
            g_app.guiGpuOffsetExcludeLowCount = (state->gpuOffsetExcludeLowCount > 0 && state->gpuOffsetMHz != 0) ? state->gpuOffsetExcludeLowCount : 0;
        }
    }
    if (state->hasMemOffset) {
        g_app.memClockOffsetkHz = mem_driver_khz_from_display_mhz(state->memOffsetMHz);
    }
    if (state->hasPowerLimit) {
        g_app.powerLimitPct = state->powerLimitPct;
    }
    if (state->hasFan) {
        g_app.activeFanMode = state->fanMode;
        if (updateGui) {
            g_app.guiFanMode = state->fanMode;
        }
        if (state->fanMode == FAN_MODE_FIXED) {
            g_app.activeFanFixedPercent = clamp_percent(state->fanFixedPercent);
            if (updateGui) g_app.guiFanFixedPercent = g_app.activeFanFixedPercent;
        } else {
            int currentPercent = state->fanCurrentPercent;
            g_app.activeFanFixedPercent = clamp_percent(currentPercent);
            if (updateGui) g_app.guiFanFixedPercent = clamp_percent(currentPercent);
        }
        copy_fan_curve(&g_app.activeFanCurve, &state->fanCurve);
        ensure_valid_fan_curve_config(&g_app.activeFanCurve);
        if (updateGui) {
            copy_fan_curve(&g_app.guiFanCurve, &state->fanCurve);
            ensure_valid_fan_curve_config(&g_app.guiFanCurve);
        }
        // state->fanMode is Green Curve intent, not necessarily the live driver
        // fan policy.  FanControl or another external controller may make NVML
        // report manual while Green Curve intent remains Auto; keep fanIsAuto
        // sourced from live snapshots/telemetry.
        g_app.fanCurveRuntimeActive = state->fanMode == FAN_MODE_CURVE;
        g_app.fanFixedRuntimeActive = state->fanMode == FAN_MODE_FIXED;
    }
}

// Compatibility projection for synchronous CLI and pre-window startup paths.
// The transport layer returns the complete immutable envelope; this caller-side
// projection adopts its snapshot, intent and controls together, never through
// the former snapshot/active-desired request chain. Runtime window paths use
// GuiServiceModel instead.
static void apply_ready_service_envelope_to_app(
    const ServiceResponse* response) {
    if (!response || response->state.gpuPhase != SERVICE_GPU_PHASE_READY ||
        (response->state.validSections &
            SERVICE_STATE_SECTION_READY_REQUIRED) !=
                SERVICE_STATE_SECTION_READY_REQUIRED) return;
    apply_service_snapshot_to_app(&response->snapshot);
    g_app.serviceActiveDesiredValid = response->state.activeDesiredValid;
    if (response->state.activeDesiredValid) {
        g_app.serviceActiveDesired = response->desired;
        apply_service_desired_to_gui(&response->desired);
    } else {
        memset(&g_app.serviceActiveDesired, 0,
            sizeof(g_app.serviceActiveDesired));
        memset(g_app.appliedCurveMHz, 0,
            sizeof(g_app.appliedCurveMHz));
    }
    if ((response->state.validSections &
            SERVICE_STATE_SECTION_APPLIED_CONTROLS) != 0)
        apply_control_state_to_gui(&response->controlState);
}

static bool get_effective_control_state(ControlState* stateOut) {
    if (!stateOut) return false;
    memset(stateOut, 0, sizeof(*stateOut));
    if (g_app.usingBackgroundService && g_app.serviceControlStateValid &&
        g_app.serviceControlState.valid) {
        *stateOut = g_app.serviceControlState;
        debug_log_on_change("get_effective_control_state: using cached service state gpu=%d exclude=%d fanMode=%d\n",
            stateOut->gpuOffsetMHz,
            stateOut->gpuOffsetExcludeLowCount,
            stateOut->fanMode);
        return stateOut->valid;
    }
    if (g_app.isServiceProcess && g_serviceControlStateValid &&
        g_serviceControlState.valid) {
        *stateOut = g_serviceControlState;
        debug_log("get_effective_control_state: using service-local state gpu=%d exclude=%d fanMode=%d\n",
            stateOut->gpuOffsetMHz,
            stateOut->gpuOffsetExcludeLowCount,
            stateOut->fanMode);
        return stateOut->valid;
    }
    populate_control_state(stateOut);
    debug_log("get_effective_control_state: using local state gpu=%d exclude=%d fanMode=%d\n",
        stateOut->gpuOffsetMHz,
        stateOut->gpuOffsetExcludeLowCount,
        stateOut->fanMode);
    return stateOut->valid;
}

