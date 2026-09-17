// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_GPU_BACKEND_APPLY_TARGETS_H
#define GREEN_CURVE_GPU_BACKEND_APPLY_TARGETS_H

// Offset-derived points remain delta-based across temperature changes. Explicit
// points own their absolute target and override the generic offset policy.
static bool apply_build_curve_targets(
    const DesiredSettings* desired, bool curveRequest, bool preserveCurveAcrossMem,
    bool hasLock, LockMode lockMode, int lockCi, unsigned int lockMhz,
    bool gpuPolicyViaCurveBatch, int desiredActiveGpuOffsetExcludeLowCount,
    int currentAppliedGpuOffsetMHz, int currentActiveGpuOffsetExcludeLowCount,
    const bool* originalCurvePopulated, const int* originalCurveOffsets,
    const int* originalCurveFreqkHz, const bool* lockedTailMask,
    int* targetCurveOffsets, bool* targetCurveMask) {
    const VfOffsetRange range = vf_offset_range_current();
    if (hasLock && lockMode == LOCK_MODE_FLATTEN &&
        !vf_offset_range_supports_flatten(range)) return false;
    if (!curveRequest && !preserveCurveAcrossMem) return true;
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        if (!originalCurvePopulated[ci]) {
            if (desired->hasCurvePoint[ci]) return false;
            continue;
        }
        long long offset = originalCurveOffsets[ci];
        bool write = preserveCurveAcrossMem;
        if (gpuPolicyViaCurveBatch) {
            bool detected = currentAppliedGpuOffsetMHz != 0 ||
                currentActiveGpuOffsetExcludeLowCount > 0;
            int oldComponent = detected ? gpu_offset_component_mhz_for_point(ci,
                currentAppliedGpuOffsetMHz, currentActiveGpuOffsetExcludeLowCount) * 1000
                : originalCurveOffsets[ci];
            offset = (long long)originalCurveOffsets[ci] - oldComponent +
                (long long)gpu_offset_component_mhz_for_point(ci,
                    desired->gpuOffsetMHz, desiredActiveGpuOffsetExcludeLowCount) * 1000;
            write = true;
        }
        const bool tail = hasLock && lockMhz > 0 && lockedTailMask[ci];
        if (desired->hasCurvePoint[ci] && !tail) {
            long long base = (long long)originalCurveFreqkHz[ci] - originalCurveOffsets[ci];
            if (base < 0) base = 0;
            offset = (long long)desired->curvePointMHz[ci] * 1000 - base;
            write = true;
        }
        if (tail && ci == lockCi) {
            long long base = (long long)originalCurveFreqkHz[ci] - originalCurveOffsets[ci];
            if (base < 0) base = 0;
            offset = (long long)lockMhz * 1000 - base;
            write = true;
        } else if (tail && lockMode == LOCK_MODE_FLATTEN) {
            offset = vf_offset_range_flatten_floor_khz(range);
            write = true;
        }
        if (write && (offset < INT_MIN || offset > INT_MAX ||
                      !vf_offset_range_permits_khz(range, (int)offset))) {
            debug_log("curve target refused: ci=%d offset=%lld range=%d..%d\n",
                ci, offset, range.minKHz, range.maxKHz);
            return false;
        }
        targetCurveOffsets[ci] = (int)offset;
        targetCurveMask[ci] = write;
    }
    return true;
}
#endif
