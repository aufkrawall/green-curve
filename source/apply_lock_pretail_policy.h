// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_APPLY_LOCK_PRETAIL_POLICY_H
#define GREEN_CURVE_APPLY_LOCK_PRETAIL_POLICY_H

#include <climits>

#include "curve_point_offset_policy.h"

// Would a locked request put some point BELOW the lock point above the lock
// target?  The driver cannot keep a flat tail under a higher pre-tail point, so
// such a request is refused -- and the question is when.
//
// It used to be asked only after reset-to-stock had already run, and answered
// with a bare `return false`: the refusal reached the user as "lower the
// preceding point", while the GPU had in fact been reset to its stock curve,
// the transition clock cap stayed installed at the lock frequency, and the
// previous profile was gone.  The answer does not need the reset.  A point's
// stock base is its live frequency minus its currently programmed offset, the
// same value the post-reset readback reports, so the whole check can run
// before the first write.  The post-reset copy remains as the backstop for a
// base that moved between the two samples, and now takes the recovery path.
//
// Per point below the tail, in curve-index order, stopping at the first tail
// point (mirrors the apply's own target build):
//   - a point the user typed:           its requested MHz
//   - a requested point with offset
//     provenance, or an unrequested
//     point under a GPU offset:         stock base + this request's per-point
//                                       offset component (a provenance point's
//                                       stored MHz is a projection over a base
//                                       sampled when the profile was saved; on a
//                                       cold board it sat a VF bin or more away
//                                       from the base the driver reports now;
//                                       see ApplyLockPretailProvenance)
//   - otherwise:                       `unownedKHz` (the value the point is
//                                       left at -- stock after a reset, the
//                                       current live value without one)
//
// Returns the first offending curve index, or -1.  All arrays have
// `numPoints` entries.
//
// How a requested provenance point is judged depends on how trustworthy the
// supplied base is (`ApplyLockPretailProvenance`):
//   - STORED: its stored MHz.  The separate NVML route, until composition with
//     VF offsets has been measured on hardware.
//   - LIVE_BASE: base + component.  The base is a post-reset readback (every
//     programmed offset zero), which IS the stock curve.
//   - AGREEMENT: refused only when BOTH the stored MHz and base + component
//     are above the lock.  For a base derived from a curve still carrying
//     another profile's offsets.  "Live minus programmed" is not the stock
//     base there: the driver reports a point next to a large offset step above
//     its own base + offset (observed on Blackwell: an untouched point read
//     ~115 MHz above stock under a profile whose next point carried +475 MHz),
//     so judging by it alone refused profile switches that the post-reset
//     check accepts.  The post-reset backstop (LIVE_BASE) still catches a real
//     violation, through the recovery path.
enum ApplyLockPretailProvenance {
    APPLY_LOCK_PRETAIL_PROVENANCE_STORED = 0,
    APPLY_LOCK_PRETAIL_PROVENANCE_LIVE_BASE = 1,
    APPLY_LOCK_PRETAIL_PROVENANCE_AGREEMENT = 2,
};

// The mode for one check: `offsetInVfTable` is the route
// (curve_point_apply_uses_vf_table_offset()), `baseIsPostReset` whether the
// base was read after reset-to-stock cleared every programmed offset.
static inline ApplyLockPretailProvenance apply_lock_pretail_provenance_mode(
    bool offsetInVfTable, bool baseIsPostReset) {
    if (!offsetInVfTable) return APPLY_LOCK_PRETAIL_PROVENANCE_STORED;
    return baseIsPostReset ? APPLY_LOCK_PRETAIL_PROVENANCE_LIVE_BASE
                           : APPLY_LOCK_PRETAIL_PROVENANCE_AGREEMENT;
}

static inline const char* apply_lock_pretail_provenance_name(ApplyLockPretailProvenance mode) {
    switch (mode) {
        case APPLY_LOCK_PRETAIL_PROVENANCE_STORED: return "stored";
        case APPLY_LOCK_PRETAIL_PROVENANCE_LIVE_BASE: return "live-base";
        case APPLY_LOCK_PRETAIL_PROVENANCE_AGREEMENT: return "agreement";
        default: return "unknown";
    }
}

static inline unsigned int apply_lock_pretail_display_mhz(long long kHz) {
    if (kHz < 0) kHz = 0;
    if (kHz > INT_MAX) kHz = INT_MAX;
    return (unsigned int)((kHz + 500) / 1000);
}

static inline long long apply_lock_pretail_stock_base_khz(unsigned int liveKHz,
                                                          int programmedOffsetKHz) {
    long long base = (long long)liveKHz - (long long)programmedOffsetKHz;
    return base < 0 ? 0 : base;
}

// `Flag` is bool or the wire's gc_bool8.
template <typename Flag, typename FromFlag>
static inline int apply_lock_pretail_first_violation(
    int numPoints, ApplyLockPretailProvenance provenanceMode, unsigned int lockMHz,
    const bool* populated, const bool* tailMask,
    const Flag* requestHasPoint, const FromFlag* requestPointFromOffset,
    const unsigned int* requestPointMHz,
    bool requestHasGpuOffset, const long long* stockBaseKHz,
    const int* gpuComponentMHz, const long long* unownedKHz,
    unsigned int* offendingTargetMHzOut) {
    if (offendingTargetMHzOut) *offendingTargetMHzOut = 0;
    if (lockMHz == 0 || numPoints <= 0 || !populated || !tailMask ||
        !requestHasPoint || !requestPointFromOffset || !requestPointMHz || !stockBaseKHz ||
        !gpuComponentMHz || !unownedKHz)
        return -1;
    for (int ci = 0; ci < numPoints; ci++) {
        if (!populated[ci]) continue;
        if (tailMask[ci]) break;
        unsigned int targetMHz = 0;
        // With no selective batch in play, a requested point is offset intent
        // exactly when it carries provenance (curve_point_offset_is_intent()).
        const bool offsetIntent = provenanceMode != APPLY_LOCK_PRETAIL_PROVENANCE_STORED &&
            requestHasPoint[ci] && requestPointFromOffset[ci] != 0;
        const unsigned int baseTargetMHz = apply_lock_pretail_display_mhz(
            stockBaseKHz[ci] + (long long)gpuComponentMHz[ci] * 1000LL);
        if (requestHasPoint[ci] && !offsetIntent) {
            targetMHz = requestPointMHz[ci];
        } else if (offsetIntent && provenanceMode == APPLY_LOCK_PRETAIL_PROVENANCE_AGREEMENT) {
            targetMHz = requestPointMHz[ci] < baseTargetMHz ? requestPointMHz[ci] : baseTargetMHz;
        } else if (offsetIntent || requestHasGpuOffset) {
            targetMHz = baseTargetMHz;
        } else {
            targetMHz = apply_lock_pretail_display_mhz(unownedKHz[ci]);
        }
        if (targetMHz > lockMHz) {
            if (offendingTargetMHzOut) *offendingTargetMHzOut = targetMHz;
            return ci;
        }
    }
    return -1;
}

#endif
