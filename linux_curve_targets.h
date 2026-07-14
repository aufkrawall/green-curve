// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_LINUX_CURVE_TARGETS_H
#define GREEN_CURVE_LINUX_CURVE_TARGETS_H

#include <limits.h>

struct LinuxCurveTargetBuildResult {
    int pointCount;
    bool composedGpuOffset;
    bool lockTail;
};

// Compose the complete Linux VF target in one pass.  Absolute explicit points
// win over GPU offset; selective/global GPU offset fills the remaining eligible
// pre-lock points; flatten/hard-lock owns the tail.  Offsets are always relative
// to the stock/base curve (live frequency minus the currently applied offset).
static inline LinuxCurveTargetBuildResult linux_build_curve_targets(
    const VFCurvePoint* curve, const int* currentOffsets,
    const DesiredSettings* desired, int minimumOffsetKHz,
    int* targetOffsets, bool* pointMask) {
    LinuxCurveTargetBuildResult result = {};
    if (!curve || !currentOffsets || !desired || !targetOffsets || !pointMask)
        return result;
    bool hardLock = desired->hasLock &&
        desired->lockMode == LOCK_MODE_HARD && desired->lockMHz > 0 &&
        desired->lockCi >= 0 && desired->lockCi < VF_NUM_POINTS;
    bool flatten = desired->hasLock &&
        desired->lockMode == LOCK_MODE_FLATTEN && desired->lockMHz > 0 &&
        desired->lockCi >= 0 && desired->lockCi < VF_NUM_POINTS;
    bool hasExplicit = false;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (desired->hasCurvePoint[i] && desired->curvePointMHz[i] > 0) {
            hasExplicit = true;
            break;
        }
    }
    result.composedGpuOffset = desired->hasGpuOffset &&
        (desired->gpuOffsetExcludeLowCount > 0 || hasExplicit || hardLock || flatten);
    result.lockTail = hardLock || flatten;
    int exclude = desired->gpuOffsetExcludeLowCount;
    if (exclude < 0) exclude = 0;
    if (exclude > VF_NUM_POINTS) exclude = VF_NUM_POINTS;

    int populatedOrdinal = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        targetOffsets[i] = 0;
        pointMask[i] = false;
        if (curve[i].freq_kHz == 0) continue;
        int ordinal = populatedOrdinal++;
        long long baseKHz = (long long)curve[i].freq_kHz -
            (long long)currentOffsets[i];
        long long targetKHz = 0;
        bool write = false;

        if ((hardLock || flatten) && i >= desired->lockCi) {
            if (i == desired->lockCi || hardLock) {
                targetKHz = (long long)desired->lockMHz * 1000LL;
            } else {
                targetOffsets[i] = minimumOffsetKHz;
                pointMask[i] = true;
                result.pointCount++;
                continue;
            }
            write = true;
        } else if (desired->hasCurvePoint[i] &&
                   desired->curvePointMHz[i] > 0) {
            targetKHz = (long long)desired->curvePointMHz[i] * 1000LL;
            write = true;
        } else if (result.composedGpuOffset && ordinal >= exclude) {
            targetKHz = baseKHz +
                (long long)desired->gpuOffsetMHz * 1000LL;
            write = true;
        }

        if (!write) continue;
        long long offset = targetKHz - baseKHz;
        if (offset < INT_MIN) offset = INT_MIN;
        if (offset > INT_MAX) offset = INT_MAX;
        targetOffsets[i] = (int)offset;
        pointMask[i] = true;
        result.pointCount++;
    }
    return result;
}

#endif // GREEN_CURVE_LINUX_CURVE_TARGETS_H
