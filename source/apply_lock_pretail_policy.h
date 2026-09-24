// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_APPLY_LOCK_PRETAIL_POLICY_H
#define GREEN_CURVE_APPLY_LOCK_PRETAIL_POLICY_H

#include <climits>

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
//   - a point the request names:        its requested MHz
//   - otherwise, with a GPU offset:     stock base + this request's per-point
//                                       offset component
//   - otherwise:                        `unownedKHz` (the value the point is
//                                       left at -- stock after a reset, the
//                                       current live value without one)
//
// Returns the first offending curve index, or -1.  All arrays have
// `numPoints` entries.
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
template <typename Flag>
static inline int apply_lock_pretail_first_violation(
    int numPoints, unsigned int lockMHz,
    const bool* populated, const bool* tailMask,
    const Flag* requestHasPoint, const unsigned int* requestPointMHz,
    bool requestHasGpuOffset, const long long* stockBaseKHz,
    const int* gpuComponentMHz, const long long* unownedKHz,
    unsigned int* offendingTargetMHzOut) {
    if (offendingTargetMHzOut) *offendingTargetMHzOut = 0;
    if (lockMHz == 0 || numPoints <= 0 || !populated || !tailMask ||
        !requestHasPoint || !requestPointMHz || !stockBaseKHz ||
        !gpuComponentMHz || !unownedKHz)
        return -1;
    for (int ci = 0; ci < numPoints; ci++) {
        if (!populated[ci]) continue;
        if (tailMask[ci]) break;
        unsigned int targetMHz = 0;
        if (requestHasPoint[ci]) {
            targetMHz = requestPointMHz[ci];
        } else if (requestHasGpuOffset) {
            long long targetKHz = stockBaseKHz[ci] +
                (long long)gpuComponentMHz[ci] * 1000LL;
            targetMHz = apply_lock_pretail_display_mhz(targetKHz);
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
