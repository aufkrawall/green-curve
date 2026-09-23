// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Recovery to stock after a partial apply.  Split out of main_gpu_front.cpp
// for the file-size guidance and included from the exact position it occupied,
// so the amalgamated declaration order is unchanged.
#ifndef GREEN_CURVE_MAIN_GPU_ROLLBACK_H
#define GREEN_CURVE_MAIN_GPU_ROLLBACK_H

// Return the GPU to stock after a partial apply, and report what was actually
// proved rather than assuming it worked.
//
// THE BUG THIS SHAPE EXISTS FOR (source-confirmed 2026-09-16, audit CT-04):
// this function used to return void and discard the result of every reset it
// performed, then call nvmlDeviceResetGpuLockedClocks() unconditionally at the
// end.  When the VF curve reset failed -- which is exactly when rollback is
// running -- the raised curve stayed on the GPU and the clock cap holding it
// down was removed anyway.  Worse, the apply calls this AFTER its transition
// guard has already decided to retain() the clamp, so the unconditional unlock
// silently undid that decision.
//
// The caller needs the per-domain outcome, so it is returned rather than
// logged: "the curve reset failed" and "the cap is gone" must not be two
// independent facts that nothing correlates.
static ApplyRecoveryResult rollback_to_safe_defaults() {
    ApplyRecoveryResult recovery = {};
    debug_log("rollback_to_safe_defaults: partial apply detected, reverting to safe defaults\n");
    auto retry_op = [](auto op, int maxRetries, const char* label) -> bool {
        for (int attempt = 0; attempt < maxRetries; attempt++) {
            if (op()) return true;
            if (attempt + 1 < maxRetries) Sleep(10);
        }
        debug_log("rollback: %s did not reset after %d attempts\n", label, maxRetries);
        return false;
    };
    recovery = reset_core_clock_controls(vf_curve_global_gpu_offset_supported(), true,
        [&]() { return nvapi_set_gpu_offset(0, true); },
        [&]() {
            if (!nvapi_read_curve()) return false;
            int zeros[VF_NUM_POINTS] = {};
            bool mask[VF_NUM_POINTS] = {};
            bool any = false;
            for (int ci = 0; ci < VF_NUM_POINTS; ++ci)
                any |= mask[ci] = g_app.curve[ci].freq_kHz != 0;
            return any && apply_curve_offsets_verified(zeros, mask, 2);
        });
    debug_log("rollback core reset: vfGlobal=%d scalarOk=%d curveOk=%d\n",
        vf_curve_global_gpu_offset_supported(), recovery.gpuOffset.verified,
        recovery.curve.verified);
    // Reset memory offset.
    if (g_app.memClockOffsetkHz != 0) {
        retry_op([&]() { return nvapi_set_mem_offset(0); }, 3, "Memory offset");
    }
    // Reset power limit to default.
    if (g_app.powerLimitPct != 100) {
        retry_op([&]() { return nvapi_set_power_limit(100); }, 3, "Power limit");
    }
    // Stop fan runtime and return to driver auto.
    stop_fan_curve_runtime();
    if (app_is_service_process() && g_serviceFanThread) {
        stop_service_fan_runtime_thread();
    }
    if (!g_app.fanIsAuto || g_app.activeFanMode != FAN_MODE_AUTO) {
        char fanDetail[128] = {};
        if (nvml_set_fan_auto(fanDetail, sizeof(fanDetail))) {
            g_app.fanIsAuto = true;
            g_app.activeFanMode = FAN_MODE_AUTO;
            g_app.activeFanFixedPercent = 0;
        } else {
            debug_log("rollback: Fan control did not return to driver auto: %s\n",
                fanDetail[0] ? fanDetail : "");
        }
    }
    clear_runtime_selective_gpu_offset_request();
    // Also clear in-memory selective offset state so the GUI does not
    // display stale values after rollback.
    g_app.appliedGpuOffsetExcludeLowCount = 0;
    g_app.appliedGpuOffsetMHz = 0;
    // Reset NVML locked clocks -- but ONLY once the clock state it was capping
    // has actually been returned to stock.
    //
    // CT-04.  This used to be unconditional, with a comment arguing that a
    // clamp left standing over a GPU whose every other control says "stock" is
    // an invisible cap.  That argument is sound and still holds -- for the
    // case where the resets SUCCEEDED.  What it missed is the case rollback
    // actually runs in: a failed reset means the GPU is NOT at stock, and
    // lifting the cap there hands the user a raised curve with nothing holding
    // it down, which is strictly worse than an invisible cap.  An invisible
    // cap is a support question; an uncapped raised curve is a TDR.
    //
    // The un-gated part is preserved: when the resets verified, the clamp goes,
    // including the F-APPLY-CEILING transition clamp that is armed for FLATTEN
    // and unpinned requests and therefore lives outside LOCK_MODE_HARD.
    if (!apply_recovery_permits_release(recovery)) {
        debug_log("rollback: KEEPING the locked-clock restriction -- recovery could not"
                  " prove stock (curve attempted=%d verified=%d, gpuOffset attempted=%d"
                  " verified=%d, lockMode=%s). Releasing it would uncap a curve that is"
                  " still raised\n",
            recovery.curve.attempted ? 1 : 0, recovery.curve.verified ? 1 : 0,
            recovery.gpuOffset.attempted ? 1 : 0, recovery.gpuOffset.verified ? 1 : 0,
            lock_mode_name(g_app.lockMode));
        return recovery;
    }
    if (apply_clock_control_proven_absent(
            g_app.gpuFamily == GPU_FAMILY_PASCAL,
            g_app.transitionClockCapActive,
            g_app.lockMode == LOCK_MODE_HARD ||
                g_app.appliedLockMode == LOCK_MODE_HARD)) {
        recovery.restrictionReleased = true;
        debug_log("rollback: NVML locked-clock release inapplicable on Pascal;"
                  " stock controls verified and no cap was installed\n");
    } else if (g_nvml_api.resetGpuLockedClocks) {
        if (nvml_ensure_ready()) {
            nvmlReturn_t r = g_nvml_api.resetGpuLockedClocks(g_app.nvmlDevice);
            recovery.restrictionReleased = (r == NVML_SUCCESS);
            if (recovery.restrictionReleased) g_app.transitionClockCapActive = false;
            debug_log("rollback: resetGpuLockedClocks (lockMode=%s) → %s\n",
                lock_mode_name(g_app.lockMode),
                r == NVML_SUCCESS ? "ok" : nvml_err_name(r));
            if (r != NVML_SUCCESS) {
                // A failed unlock is not benign: it may mean a restriction is
                // still in force that nothing in the UI accounts for.
                debug_log("rollback: the locked-clock release FAILED; a clock"
                          " restriction may still be active and is reported as such\n");
            }
        }
    } else {
        // No release entry point means nothing here could have installed a
        // restriction either -- the guard requires both halves of the NVML
        // pair before it arms.
        recovery.restrictionReleased = true;
    }
    return recovery;
}

#endif  // GREEN_CURVE_MAIN_GPU_ROLLBACK_H
