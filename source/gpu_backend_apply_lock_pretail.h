// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_GPU_BACKEND_APPLY_LOCK_PRETAIL_H
#define GREEN_CURVE_GPU_BACKEND_APPLY_LOCK_PRETAIL_H

// The Windows apply's two uses of apply_lock_pretail_policy.h.  Included by
// gpu_backend_apply.cpp only.

#include "apply_lock_pretail_policy.h"

// Inputs the policy needs from the apply's own view of the curve.  `base` is
// the per-point stock base; `unowned` what a point the request does not name is
// left at.
struct ApplyLockPretailArrays {
    bool populated[VF_NUM_POINTS];
    bool tail[VF_NUM_POINTS];
    long long base[VF_NUM_POINTS];
    long long unowned[VF_NUM_POINTS];
    int gpuComponentMHz[VF_NUM_POINTS];
};

static void apply_lock_pretail_format_refusal(char* out, size_t outSize,
    unsigned int lockMHz, int lockCi, int offendingCi, unsigned int offendingMHz,
    bool selectiveOffsetHint) {
    set_message(out, outSize,
        "Curve lock %u MHz at point %d is below pre-tail point %d (%u MHz).%s"
        " Lower the preceding point or raise the lock target.",
        lockMHz, lockCi, offendingCi, offendingMHz,
        selectiveOffsetHint
            ? " Enable 'exclude low VF points' to exclude pre-tail points from the GPU offset."
            : "");
}

// The pre-write half, for requests that reset to stock first.  Runs BEFORE the
// transition clamp or the reset touch anything, on a fresh curve read, with
// each point's stock base (live minus programmed offset) standing in for the
// post-reset readback.  Returns false with `result` set when the request must
// be refused; true otherwise, including when the curve cannot be read here (the
// post-reset check still runs and now recovers properly).
static bool apply_lock_pretail_precheck_before_reset(const DesiredSettings* desired,
    char* result, size_t resultSize) {
    if (!desired || !desired->resetOcBeforeApply) return true;
    if (!desired->hasLock || desired->lockCi < 0 ||
        desired->lockCi >= VF_NUM_POINTS || desired->lockMHz == 0) return true;
    if (desired->hasGpuOffset && g_app.gpuOffsetRangeKnown &&
        (desired->gpuOffsetMHz < g_app.gpuClockOffsetMinMHz ||
         desired->gpuOffsetMHz > g_app.gpuClockOffsetMaxMHz)) {
        return true;  // reported by the apply's own range check
    }
    int lockVi = -1;
    for (int vi = 0; vi < g_app.numVisible; vi++) {
        if (g_app.visibleMap[vi] == desired->lockCi) { lockVi = vi; break; }
    }
    if (lockVi < 0) return true;  // refused earlier by the visibility check
    if (!nvapi_read_curve()) {
        debug_log("apply lock precheck: curve read failed; deferring the pre-tail"
                  " check to the post-reset backstop\n");
        return true;
    }
    ApplyLockPretailArrays a = {};
    for (int vi = lockVi; vi < g_app.numVisible; vi++) {
        int ci = g_app.visibleMap[vi];
        if (ci >= 0 && ci < VF_NUM_POINTS) a.tail[ci] = true;
    }
    const int excludeLow = (desired->hasGpuOffset &&
        desired->gpuOffsetExcludeLowCount > 0 && desired->gpuOffsetMHz != 0)
        ? desired->gpuOffsetExcludeLowCount : 0;
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        a.populated[ci] = g_app.curve[ci].freq_kHz > 0;
        a.base[ci] = apply_lock_pretail_stock_base_khz(g_app.curve[ci].freq_kHz,
                                                       g_app.freqOffsets[ci]);
        // After the reset every point the request does not name sits at stock.
        a.unowned[ci] = a.base[ci];
        a.gpuComponentMHz[ci] = desired->hasGpuOffset
            ? gpu_offset_component_mhz_for_point(ci, desired->gpuOffsetMHz, excludeLow)
            : 0;
    }
    unsigned int offendingMHz = 0;
    int offendingCi = apply_lock_pretail_first_violation(VF_NUM_POINTS,
        desired->lockMHz, a.populated, a.tail, desired->hasCurvePoint,
        desired->curvePointMHz, desired->hasGpuOffset, a.base,
        a.gpuComponentMHz, a.unowned, &offendingMHz);
    if (offendingCi < 0) return true;
    apply_lock_pretail_format_refusal(result, resultSize, desired->lockMHz,
        desired->lockCi, offendingCi, offendingMHz,
        desired->hasGpuOffset && desired->gpuOffsetMHz != 0 &&
            !desired->hasCurvePoint[offendingCi]);
    debug_log("apply lock precheck: REFUSED before any write -- lockCi=%d lockMHz=%u"
              " mode=%s preTailCi=%d target=%u stockBase=%lld kHz gpuOffset=%d"
              " exclude=%d explicit=%d\n",
        desired->lockCi, desired->lockMHz, lock_mode_name(desired->lockMode),
        offendingCi, offendingMHz, a.base[offendingCi],
        desired->hasGpuOffset ? desired->gpuOffsetMHz : 0, excludeLow,
        desired->hasCurvePoint[offendingCi] ? 1 : 0);
    return false;
}

#endif
