// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The VF curve TARGET BUILDER: turning an apply's intent plus a fresh reading
// of the live curve into the per-point control offsets the batch writer sends.
//
// Split out of gpu_backend_apply.cpp, which is far over the project's
// file-size guidance, and included from the exact position it occupied so the
// amalgamated ordering is unchanged.
//
// THE ONE INVARIANT WORTH KNOWING HERE.  Targets are DELTA-based, not absolute,
// and that is deliberate: a boost point is written as
// `originalOffset - currentGpuComponent + desiredGpuComponent`, so the same
// request produces the same intended delta whatever the GPU's temperature has
// done to its base clocks.  Rebuilding from stored absolute samples instead
// would inflate the offset every time the card ran cooler than when the
// profile was saved.  See the long note in gpu_backend_reset_baseline.cpp for
// why the per-point reset-to-zero that makes "current" a clean baseline is not
// optional either.
//
// The FLATTEN tail floor is the other thing that looks wrong and is not.  It
// writes a blanket range minimum onto every tail point rather than an exact
// per-point delta, because NVIDIA enforces a monotonic non-decreasing VF curve:
// pushing the tail far down makes the driver collapse it onto the anchor
// point's frequency, which IS the requested plateau.  That floor now comes from
// vf_offset_range_policy.h, the same struct the batch pre-check derives its
// refusal limit from -- CT-03 was the two disagreeing, so that the planner
// generated a floor its own pre-check then refused, and the resulting "refused
// everything" was reported to the user as a successful apply.
#ifndef GREEN_CURVE_GPU_BACKEND_APPLY_TARGETS_H
#define GREEN_CURVE_GPU_BACKEND_APPLY_TARGETS_H

static void apply_build_curve_targets(
    const DesiredSettings* desired, bool curveRequest, bool preserveCurveAcrossMem,
    bool hasLock, LockMode lockMode, int lockCi, unsigned int lockMhz,
    bool gpuPolicyViaCurveBatch, int desiredActiveGpuOffsetExcludeLowCount,
    int currentAppliedGpuOffsetMHz, int currentActiveGpuOffsetExcludeLowCount,
    const bool* originalCurvePopulated, const int* originalCurveOffsets,
    const int* originalCurveFreqkHz, const bool* lockedTailMask,
    int* targetCurveOffsets, bool* targetCurveMask) {
    if (curveRequest || preserveCurveAcrossMem) {
        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
            if (!originalCurvePopulated[ci]) continue;
            targetCurveOffsets[ci] = originalCurveOffsets[ci];
            if (preserveCurveAcrossMem) targetCurveMask[ci] = true;
        }
        if (gpuPolicyViaCurveBatch) {
            bool currentDetected = (currentAppliedGpuOffsetMHz != 0 || currentActiveGpuOffsetExcludeLowCount > 0);
            for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                if (!originalCurvePopulated[ci]) continue;
                int desiredPointGpuOffsetkHz = gpu_offset_component_mhz_for_point(ci, desired->gpuOffsetMHz, desiredActiveGpuOffsetExcludeLowCount) * 1000;
                int currentPointGpuOffsetkHz;
                if (currentDetected) {
                    currentPointGpuOffsetkHz = gpu_offset_component_mhz_for_point(ci, currentAppliedGpuOffsetMHz, currentActiveGpuOffsetExcludeLowCount) * 1000;
                } else {
                    currentPointGpuOffsetkHz = originalCurveOffsets[ci];
                }
                int targetOffset = clamp_freq_delta_khz(originalCurveOffsets[ci] - currentPointGpuOffsetkHz + desiredPointGpuOffsetkHz);
                targetCurveOffsets[ci] = targetOffset;
                targetCurveMask[ci] = true;
            }
            debug_log("selective offset: currentMHz=%d desiredMHz=%d currentExcl=%d desiredExcl=%d detected=%d hasLock=%d lockMHz=%d\n",
                currentAppliedGpuOffsetMHz, desired->gpuOffsetMHz,
                currentActiveGpuOffsetExcludeLowCount,
                desiredActiveGpuOffsetExcludeLowCount,
                currentDetected ? 1 : 0,
                hasLock ? 1 : 0, lockMhz);
        }
        if (hasLock && lockMhz > 0) {
            // When a selective offset is active, the selective path above already
            // set correct per-point deltas (+offset for included points, 0 for
            // excluded points). Using absolute profile targets for boost-region
            // points would overwrite those deltas with temperature-dependent
            // values that can blow out to 700+ MHz when the cold base curve is
            // elevated. Only the tail-flatten loop is needed in this case.
            if (!gpuPolicyViaCurveBatch) {
                for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                    if (!desired->hasCurvePoint[ci]) continue;
                    if (lockedTailMask[ci]) continue;
                    if (!originalCurvePopulated[ci]) continue;
                    long long base = (long long)originalCurveFreqkHz[ci] - (long long)originalCurveOffsets[ci];
                    if (base < 0) base = 0;
                    long long target = (long long)desired->curvePointMHz[ci] * 1000LL;
                    long long diff = target - base;
                    if (diff > INT_MAX) diff = INT_MAX;
                    if (diff < INT_MIN) diff = INT_MIN;
                    targetCurveOffsets[ci] = clamp_freq_delta_khz((int)diff);
                    targetCurveMask[ci] = true;
                }
            }
            // Determine the uniform floor offset for tail points.
            // Per-point tail offsets are ineffective on Blackwell: the
            // driver ignores individual tail-point deltas and the correction
            // loop cannot converge. The solution is to apply a single
            // uniform negative offset to ALL tail points, which floors
            // the tail and lets the lock point control the entire region.
            // Use the minimum supported driver offset for this purpose.
            int floorTailOffsetKHz = 0;
            if (lockMode == LOCK_MODE_FLATTEN) {
                // CT-03.  The floor is the range's own minimum, so the batch
                // pre-check below -- which derives its limit from the SAME
                // struct -- can never refuse a floor this planner produced.
                // The previous `else` branch hardcoded -1,000,000 kHz against
                // a check that allowed 500,000 kHz on unprobeable hardware.
                floorTailOffsetKHz =
                    vf_offset_range_flatten_floor_khz(vf_offset_range_current());
            }
            for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                if (!lockedTailMask[ci]) continue;
                if (!originalCurvePopulated[ci]) continue;
                if (ci == lockCi) {
                    long long base = (long long)originalCurveFreqkHz[ci] - (long long)originalCurveOffsets[ci];
                    if (base < 0) base = 0;
                    long long target = (long long)lockMhz * 1000LL;
                    long long diff = target - base;
                    if (diff > INT_MAX) diff = INT_MAX;
                    if (diff < INT_MIN) diff = INT_MIN;
                    targetCurveOffsets[ci] = clamp_freq_delta_khz((int)diff);
                    targetCurveMask[ci] = true;
                } else if (lockMode == LOCK_MODE_FLATTEN) {
                    targetCurveOffsets[ci] = floorTailOffsetKHz;
                    targetCurveMask[ci] = true;
                }
            }
        } else if (gpuPolicyViaCurveBatch && !hasLock) {
            // When the selective GPU offset is active without a lock, the explicit
            // curve point path is skipped because the selective offset already
            // handles all populated points. The locked tail above also handles
            // the case where both lock and selective offset are active.
        } else {
            // No lock, no selective offset — explicit curve points only.
            for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                if (!desired->hasCurvePoint[ci]) continue;
                if (!originalCurvePopulated[ci]) continue;
                long long base = (long long)originalCurveFreqkHz[ci] - (long long)originalCurveOffsets[ci];
                if (base < 0) base = 0;
                long long target = (long long)desired->curvePointMHz[ci] * 1000LL;
                long long diff = target - base;
                if (diff > INT_MAX) diff = INT_MAX;
                if (diff < INT_MIN) diff = INT_MIN;
                targetCurveOffsets[ci] = clamp_freq_delta_khz((int)diff);
                targetCurveMask[ci] = true;
            }
        }
    }
}

#endif  // GREEN_CURVE_GPU_BACKEND_APPLY_TARGETS_H
