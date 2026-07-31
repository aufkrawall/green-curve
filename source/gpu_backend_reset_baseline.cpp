// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The reset-to-stock-baseline step every profile-switching Apply runs first.
// Split out of gpu_backend_apply.cpp, which had reached its size ratchet, and
// included from the position it occupied so the amalgamated ordering is
// unchanged.  The long comment inside is the whole point of the file: the
// ordering of these writes, and the fact that the per-point VF reset is NOT
// optional, are both measured behaviour that has been re-litigated before.
static bool reset_oc_before_gui_apply(const DesiredSettings* desired,
    char* result, size_t resultSize) {
    int resetOffsets[VF_NUM_POINTS] = {};
    bool resetMask[VF_NUM_POINTS] = {};
    char failures[512] = {};
    auto append_failure = [&](const char* text) {
        if (!text || !text[0]) return;
        if (failures[0]) StringCchCatA(failures, ARRAY_COUNT(failures), "; ");
        StringCchCatA(failures, ARRAY_COUNT(failures), text);
        debug_log("reset-before-apply failure: %s\n", text);
    };
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (g_app.curve[ci].freq_kHz != 0) resetMask[ci] = true;
    }
    bool hadCurveOffsets = false;
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (g_app.freqOffsets[ci] != 0) {
            hadCurveOffsets = true;
            break;
        }
    }
    set_last_apply_phase("apply: reset OC baseline");
    // Reset GPU offset first to avoid dangerous transient where VF curve tail
    // points snap to factory base frequencies (~3300+ MHz on modern GPUs) while
    // the GPU offset from the previous profile is still active — that spike
    // (e.g. 3300 base + 475 old offset = 3775 MHz effective) causes TDR/crashes.
    if (desired && desired->hasGpuOffset &&
        g_app.gpuClockOffsetkHz != 0 && !nvapi_set_gpu_offset(0)) {
        append_failure("GPU offset did not reset");
    }
    // A reset-to-clean-VF-baseline is not ownership of unrelated controls.
    // Only reset power when the incoming request itself owns power and will
    // immediately write its requested target in the apply phase.
    if (desired && desired->hasPowerLimit &&
        g_app.powerLimitPct != 100 && !nvapi_set_power_limit(100)) {
        append_failure("Power target did not reset");
    }
    // Do NOT reset memory offset here — abruptly dropping from +3000 to 0
    // while VRAM is under game load causes TDRs. The new profile's memory
    // offset will be applied directly in the main apply phase.
    //
    // The per-point VF-curve reset-to-zero is REQUIRED, not just a ~1s cost. The
    // selective-offset / boost apply is deliberately DELTA-based for temperature
    // independence (see the "blow out to 700+ MHz" note below): each boost point is
    // written as originalOffset - currentGpuComponent + desiredGpuComponent. For
    // EXCLUDED / stock points the current & desired GPU components are both 0, so the
    // target collapses to originalCurveOffsets[ci] — i.e. whatever is CURRENTLY on the
    // point. This reset-to-zero is what makes that "current" a clean stock baseline;
    // skipping it leaves the PREVIOUS profile's offset on any point the new profile
    // doesn't re-boost (e.g. +475 MHz stranded on excluded points 70-75 when switching
    // to a milder profile — observed 2026-07-04, build 355 `skip_reset_curve_write`
    // experiment). It cannot be cheaply removed without reworking the boost to absolute
    // targets, which the delta design exists to avoid. So it always runs.
    if (hadCurveOffsets && !apply_curve_offsets_verified(resetOffsets, resetMask, 2)) {
        append_failure("VF curve offsets did not reset");
    }
    if (failures[0]) {
        set_message(result, resultSize, "Reset before apply failed: %s", failures);
        return false;
    }
    g_app.lastApplyUsedGpuOffset = false;
    read_live_curve_snapshot_settled(4, 25, nullptr);
    refresh_global_state(result, resultSize);
    debug_log("reset-before-apply: OC baseline reset succeeded\n");
    return true;
}
