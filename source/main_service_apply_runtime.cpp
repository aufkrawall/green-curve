// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Service-owned settings apply/reset and authoritative in-memory intent.

static bool service_desired_has_owned_intent(const DesiredSettings* desired) {
    if (!desired) return false;
    if (desired->hasLock || desired->hasGpuOffset || desired->hasMemOffset ||
        desired->hasPowerLimit || desired->hasFan) return true;
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        if (desired->hasCurvePoint[ci]) return true;
    }
    return false;
}
static bool service_apply_desired_settings(const DesiredSettings* desired, bool interactive,
    char* result, size_t resultSize, bool* writeAttemptedOut,
    bool replaceActiveIntent,
    const DesiredSettings* replacementIntent) {
    if (writeAttemptedOut) *writeAttemptedOut = false;
    if (!desired) {
        set_message(result, resultSize, "No desired settings provided");
        return false;
    }
    if (!service_desired_has_owned_intent(desired)) {
        set_message(result, resultSize,
            "No Green Curve-owned settings were requested");
        return false;
    }
    char detail[256] = {};
    set_last_apply_phase("service apply: hardware initialize");
    if (!hardware_initialize(detail, sizeof(detail))) {
        set_message(result, resultSize, "%s", detail[0] ? detail : "Hardware initialization failed");
        set_last_apply_phase("service apply: hardware initialize failed");
        return false;
    }
    int requestedCurvePoints = 0;
    if (desired) {
        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
            if (desired->hasCurvePoint[ci]) requestedCurvePoints++;
        }
    }
    debug_log("service_apply_desired_settings: interactive=%d gpu=%d exclude=%d mem=%d power=%d fanMode=%d lockCi=%d lockMHz=%u curvePoints=%d\n",
        interactive ? 1 : 0,
        desired && desired->hasGpuOffset ? desired->gpuOffsetMHz : 0,
        desired && desired->hasGpuOffset ? desired->gpuOffsetExcludeLowCount : 0,
        desired && desired->hasMemOffset ? desired->memOffsetMHz : 0,
        desired && desired->hasPowerLimit ? desired->powerLimitPct : 0,
        desired && desired->hasFan ? desired->fanMode : -1,
        desired && desired->hasLock ? desired->lockCi : -1,
        desired && desired->hasLock ? desired->lockMHz : 0u,
        requestedCurvePoints);
    set_last_apply_phase("service apply: apply desired settings");
    bool hardwareWriteAttempted = false;
    bool ok = apply_desired_settings_service(desired, interactive,
        result, resultSize, &hardwareWriteAttempted);
    if (writeAttemptedOut) *writeAttemptedOut = hardwareWriteAttempted;
    if (ok) {
        set_last_apply_phase("service apply: capture authoritative state");
        // Update the active desired state BEFORE capturing the control state
        // so that current_applied_gpu_offset_mhz() (called by populate_control_state)
        // sees the new desired values rather than stale previous-profile values.
        // Otherwise a profile 2 with hasGpuOffset=true/gpuOffsetMHz=0 would fall
        // back to the previous profile's selective offset (e.g. 475/60) because
        // g_serviceActiveDesired had not been updated yet and tail points with
        // non-zero flatten offsets satisfied live_curve_has_any_nonzero_offsets().
        DesiredSettings mergedActiveDesired = {};
        bool hadActiveDesired = g_serviceHasActiveDesired;
        if (replaceActiveIntent) {
            // A named profile is a complete Green Curve ownership declaration,
            // even when it intentionally owns only fan or only curve fields.
            // Never inherit omitted controls from another profile/account.
            mergedActiveDesired = replacementIntent
                ? *replacementIntent : *desired;
            debug_log("service apply: replacing active ownership with named/lifecycle intent\n");
        } else if (hadActiveDesired) {
            mergedActiveDesired = g_serviceActiveDesired;
        } else {
            initialize_desired_settings_defaults(&mergedActiveDesired);
        }
        bool replaceOcCurveIntent = !replaceActiveIntent &&
            desired_updates_curve_or_gpu_offset_state(desired);
        if (replaceOcCurveIntent) {
            for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                mergedActiveDesired.hasCurvePoint[ci] = false;
                mergedActiveDesired.curvePointMHz[ci] = 0;
            }
            mergedActiveDesired.hasLock = false;
            mergedActiveDesired.lockCi = -1;
            mergedActiveDesired.lockMHz = 0;
            mergedActiveDesired.lockTracksAnchor = true;
        }
        if (!replaceActiveIntent) {
            merge_desired_settings(&mergedActiveDesired, desired);
        }
        if (replaceOcCurveIntent && desired->hasLock) {
            mergedActiveDesired.hasLock = true;
            mergedActiveDesired.lockCi = desired->lockCi;
            mergedActiveDesired.lockMHz = desired->lockMHz;
            mergedActiveDesired.lockMode = desired->lockMode;
            mergedActiveDesired.lockTracksAnchor = desired->lockTracksAnchor;
        }
        if (!replaceActiveIntent &&
            desired_is_fan_only_apply_request(desired) &&
            g_serviceHasActiveDesired) {
            debug_log("service apply: merged fan-only request into active desired, preserving lockCi=%d lockMHz=%u curvePoints=%d\n",
                mergedActiveDesired.hasLock ? mergedActiveDesired.lockCi : -1,
                mergedActiveDesired.hasLock ? mergedActiveDesired.lockMHz : 0u,
                desired_curve_point_count(&mergedActiveDesired));
        }
        g_serviceActiveDesired = mergedActiveDesired;
        g_serviceActiveDesired.resetOcBeforeApply = false;
        if (g_app.selectedGpu.valid) {
            g_serviceActiveDesiredGpu = g_app.selectedGpu;
        } else if (g_app.selectedGpuIndex < g_app.adapterCount && g_app.adapters[g_app.selectedGpuIndex].valid) {
            g_serviceActiveDesiredGpu = g_app.adapters[g_app.selectedGpuIndex];
        } else {
            memset(&g_serviceActiveDesiredGpu, 0, sizeof(g_serviceActiveDesiredGpu));
        }
        // Missing fields remain missing. Never adopt live settings as Green
        // Curve intent: another tuning tool may own them, and standby/driver
        // restoration must replay only settings we actually wrote.
        g_serviceHasActiveDesired = true;
        InterlockedExchange(&g_serviceReapplyInProgress, 0);
        service_clear_restart_reapply_snapshot();
        populate_control_state(&g_serviceControlState);
        g_serviceControlStateValid = true;
        mark_service_telemetry_cache_updated("service apply");
        if (g_serviceActiveDesired.hasLock) {
            debug_log("service apply: preserving requested lock intent ci=%d mhz=%u after live readback\n",
                g_serviceActiveDesired.lockCi,
                g_serviceActiveDesired.lockMHz);
        }
        if (g_app.fanCurveRuntimeActive || g_app.fanFixedRuntimeActive) {
            ensure_service_fan_runtime_thread();
        } else {
            stop_service_fan_runtime_thread();
        }
    } else {
        clear_service_authoritative_state();
        set_last_apply_phase("service apply: capture partial state");
        populate_control_state(&g_serviceControlState);
        g_serviceControlStateValid = true;
        mark_service_telemetry_cache_updated("service apply partial");
        // RC3 fix: do NOT clear g_serviceHasActiveDesired / g_serviceActiveDesired
        // on transient apply failure.  Previously, a single failed apply (e.g.
        // mid-recovery) wiped the active desired, so the next recovery had
        // nothing to reapply and the service was stuck in "crash recovery
        // active, skipping hardware_initialize" forever.  Preserving the
        // active desired lets the next recovery reapply the same settings.
        // The disk snapshot is also kept up to date so a service restart
        // mid-recovery can still restore the previous profile.
        debug_log("service apply: apply FAILED, preserving active desired (hasLock=%d lockCi=%d)\n",
            g_serviceActiveDesired.hasLock ? 1 : 0,
            g_serviceActiveDesired.hasLock ? g_serviceActiveDesired.lockCi : -1);
        service_write_restart_reapply_snapshot();
    }
    set_last_apply_phase(ok ? "service apply: complete" : "service apply: failed");
    return ok;
}

static bool service_reset_all(char* result, size_t resultSize,
    bool* hardwareWriteAttemptedOut) {
    if (hardwareWriteAttemptedOut) *hardwareWriteAttemptedOut = false;
    char detail[256] = {};
    if (!hardware_initialize(detail, sizeof(detail))) {
        set_message(result, resultSize, "%s", detail[0] ? detail : "Hardware initialization failed");
        return false;
    }
    if (!service_invalidate_oc_apply_proof_before_write()) {
        set_message(result, resultSize,
            "Could not invalidate the previous stability proof; no reset write was attempted");
        return false;
    }
    if (hardwareWriteAttemptedOut) *hardwareWriteAttemptedOut = true;

    int resetOffsets[VF_NUM_POINTS] = {};
    bool resetMask[VF_NUM_POINTS] = {};
    int successCount = 0;
    int failCount = 0;
    char failureDetails[1024] = {};
    auto append_failure = [&](const char* fmt, ...) {
        char part[256] = {};
        va_list ap;
        va_start(ap, fmt);
        StringCchVPrintfA(part, ARRAY_COUNT(part), fmt, ap);
        va_end(ap);
        if (!part[0]) return;
        if (failureDetails[0]) StringCchCatA(failureDetails, ARRAY_COUNT(failureDetails), "; ");
        StringCchCatA(failureDetails, ARRAY_COUNT(failureDetails), part);
    };
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (g_app.curve[ci].freq_kHz == 0) continue;
        resetMask[ci] = true;
    }
    bool hadCurveOffsets = false;
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (g_app.freqOffsets[ci] != 0) {
            hadCurveOffsets = true;
            break;
        }
    }
    if (hadCurveOffsets) {
        if (apply_curve_offsets_verified(resetOffsets, resetMask, 2)) successCount++;
        else {
            failCount++;
            append_failure("VF curve offsets did not reset cleanly");
        }
    }
    if (g_app.gpuClockOffsetkHz != 0) {
        if (nvapi_set_gpu_offset(0)) successCount++;
        else {
            failCount++;
            append_failure("GPU offset did not reset to default");
        }
    }
    if (g_app.memClockOffsetkHz != 0) {
        if (nvapi_set_mem_offset(0)) successCount++;
        else {
            failCount++;
            append_failure("Memory offset did not reset to default");
        }
    }
    if (g_app.powerLimitPct != 100) {
        if (nvapi_set_power_limit(100)) successCount++;
        else {
            failCount++;
            append_failure("Power limit did not reset to default");
        }
    }

    // Stop the service-owned fan maintenance first so it cannot immediately
    // reassert a manual target after we restore driver auto.
    stop_fan_curve_runtime();
    if (g_app.isServiceProcess && g_serviceFanThread) {
        stop_service_fan_runtime_thread();
    }

    // Reset NVML locked clocks (hard lock)
    if (g_nvml_api.resetGpuLockedClocks) {
        if (nvml_ensure_ready()) {
            nvmlReturn_t r = g_nvml_api.resetGpuLockedClocks(g_app.nvmlDevice);
            if (r == NVML_SUCCESS) {
                successCount++;
                debug_log("service_reset_all: resetGpuLockedClocks ok\n");
            } else {
                // Not a failure if no lock was active
                debug_log("service_reset_all: resetGpuLockedClocks → %s (may be benign)\n", nvml_err_name(r));
            }
        }
    }

    if (!g_app.fanIsAuto || g_app.activeFanMode != FAN_MODE_AUTO) {
        char fanDetail[128] = {};
        if (nvml_set_fan_auto(fanDetail, sizeof(fanDetail))) {
            successCount++;
            g_app.fanIsAuto = true;
            g_app.activeFanMode = FAN_MODE_AUTO;
            g_app.activeFanFixedPercent = 0;
        } else {
            failCount++;
            append_failure("Fan control did not return to driver auto%s%s",
                fanDetail[0] ? ": " : "",
                fanDetail[0] ? fanDetail : "");
        }
    }

    // F-RESET-INTENT: drop the active-desired intent NOW, before the post-reset
    // state refresh + control-state population below. A reset means "stop applying
    // anything", so lock/fan intent derivation must see NO intent. Clearing it only
    // afterwards (as this used to) left detect_locked_tail_from_curve() preserving
    // the old lock and initialize_gui_fan_settings_from_live_state() re-reading the
    // stale desired fan mode into g_app.activeFanMode — so the RESET snapshot
    // reported the old Custom Curve fan mode + lock and the GUI re-adopted them.
    g_serviceHasActiveDesired = false;
    memset(&g_serviceActiveDesired, 0, sizeof(g_serviceActiveDesired));
    memset(&g_serviceActiveDesiredGpu, 0, sizeof(g_serviceActiveDesiredGpu));
    EnterCriticalSection(&g_appLock);
    g_serviceActiveProfileSource = SERVICE_PROFILE_SOURCE_NONE;
    g_serviceActiveProfileSlot = 0;
    LeaveCriticalSection(&g_appLock);

    // Clear persisted runtime state BEFORE refreshing so the refresh sees the
    // true post-reset hardware state rather than the old persisted request.
    if (failCount == 0) {
        clear_runtime_selective_gpu_offset_request();
    }

    if (!refresh_global_state(detail, sizeof(detail))) {
        append_failure("Failed to refresh live state after reset%s%s",
            detail[0] ? ": " : "",
            detail[0] ? detail : "");
        failCount++;
    }
    if (g_app.fanSupported) {
        char fanDetail[128] = {};
        nvml_read_fans(fanDetail, sizeof(fanDetail));
    }
    initialize_gui_fan_settings_from_live_state(false);
    if (g_app.fanIsAuto) {
        g_app.guiFanMode = FAN_MODE_AUTO;
        g_app.guiFanFixedPercent = 0;
        fan_curve_set_default(&g_app.guiFanCurve);
        fan_curve_set_default(&g_app.activeFanCurve);
    }
    // (active-desired intent was cleared above, before refresh_global_state, so the
    // post-reset lock/fan derivation and control-state snapshot reflect true stock)
    if (failCount == 0) {
        g_app.appliedGpuOffsetMHz = 0;
        g_app.appliedGpuOffsetExcludeLowCount = 0;
        InterlockedExchange(&g_serviceReapplyInProgress, 0);
        service_clear_restart_reapply_snapshot();
        clear_service_authoritative_state();
        populate_control_state(&g_serviceControlState);
        g_serviceControlStateValid = true;
        set_message(result, resultSize, "Reset applied.");
        return true;
    }
    // RC3 fix: on partial reset failure, keep the on-disk snapshot so the
    // next recovery (or service restart) can restore the previous profile.
    // The in-memory g_serviceActiveDesired was already cleared above — that
    // is correct for reset, which is a "stop applying anything" operation;
    // but the disk snapshot is the safety net for a service restart after a
    // partial reset.
    service_write_restart_reapply_snapshot();
    populate_control_state(&g_serviceControlState);
    g_serviceControlStateValid = true;
    mark_service_telemetry_cache_updated("service reset");
    set_message(result, resultSize, "Reset applied %d OK, %d failed: %s", successCount, failCount, failureDetails[0] ? failureDetails : "one or more reset steps failed");
    return false;
}
