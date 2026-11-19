// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_GPU_BACKEND_APPLY_TARGETS_H
#define GREEN_CURVE_GPU_BACKEND_APPLY_TARGETS_H

#include "curve_point_offset_policy.h"
#include "desired_curve_intent.h"

// Fill `shiftOut` with each point's bounded base shift across this apply's
// NVML write (curve_point_bounded_scalar_shift_khz()) and return true, or
// return false when it is not measurable (the caller must resolve retained
// evidence for a live scalar before building targets). Logs one summary line
// either way when it matters: a
// measured shift is also the field answer to the NVML/VF composition question.
static bool apply_measure_provenance_scalar_shift(const DesiredSettings* desired,
    bool offsetInVfTable, bool scalarWritten, bool scalarBeforeKnownZero,
    int writtenScalarKHz, const int* preFreqKHz, const int* preOffsetsKHz,
    const int* postFreqKHz, const int* postOffsetsKHz, int* shiftOut) {
    int provenancePoints = 0;
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        shiftOut[ci] = 0;
        if (desired->hasCurvePoint[ci] && desired->curvePointFromGpuOffset[ci] &&
            !desired_curve_point_in_explicit_lock_tail(desired, ci)) provenancePoints++;
    }
    if (offsetInVfTable || provenancePoints == 0) return false;
    if (!curve_point_scalar_shift_measurable(offsetInVfTable, scalarWritten, scalarBeforeKnownZero)) {
        debug_log("nvml shift: no fresh measurement (written=%d beforeZero=%d); %d provenance point(s)"
                  " require retained evidence for a live scalar\n", scalarWritten ? 1 : 0,
                  scalarBeforeKnownZero ? 1 : 0, provenancePoints);
        return false;
    }
    int measured = 0, full = 0, none = 0, partial = 0;
    long long rawMin = LLONG_MAX, rawMax = LLONG_MIN;
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        if (preFreqKHz[ci] <= 0 || postFreqKHz[ci] <= 0) continue;
        const long long raw = ((long long)postFreqKHz[ci] - postOffsetsKHz[ci]) -
            ((long long)preFreqKHz[ci] - preOffsetsKHz[ci]);
        if (raw < rawMin) rawMin = raw;
        if (raw > rawMax) rawMax = raw;
        const long long bounded = curve_point_bounded_scalar_shift_khz(preFreqKHz[ci],
            preOffsetsKHz[ci], postFreqKHz[ci], postOffsetsKHz[ci], writtenScalarKHz);
        shiftOut[ci] = (int)bounded;
        measured++;
        if (bounded == writtenScalarKHz) full++;
        else if (bounded == 0) none++;
        else partial++;
    }
    debug_log("nvml shift: written=%d kHz points=%d full=%d none=%d partial=%d raw=%lld..%lld kHz;"
              " %d provenance point(s) get component minus measured shift\n",
        writtenScalarKHz, measured, full, none, partial,
        measured ? rawMin : 0, measured ? rawMax : 0, provenancePoints);
    return true;
}

enum CurveTargetBuildFailureReason {
    CURVE_TARGET_BUILD_OK = 0,
    CURVE_TARGET_BUILD_INVALID_INTENT = 1,
    CURVE_TARGET_BUILD_FLATTEN_UNSUPPORTED = 2,
    CURVE_TARGET_BUILD_UNPOPULATED_POINT = 3,
    CURVE_TARGET_BUILD_CAPTURED_OUT_OF_RANGE = 4,
    CURVE_TARGET_BUILD_OFFSET_OVERFLOW = 5,
    CURVE_TARGET_BUILD_OFFSET_OUT_OF_RANGE = 6,
};

struct CurveTargetBuildFailure {
    CurveTargetBuildFailureReason reason;
    int ci;
    bool isAnchor;
    unsigned int targetMHz;
    long long baseKHz;
    long long offsetKHz;
    long long offsetHz;
    int rangeMinKHz;
    int rangeMaxKHz;
};

// Offset-derived points remain delta-based across temperature changes. Explicit
// points own their absolute target and override the generic offset policy.
static bool apply_build_curve_targets(
    const DesiredSettings* desired, bool curveRequest, bool preserveCurveAcrossMem,
    bool hasLock, LockMode lockMode, int lockCi, unsigned int lockMhz,
    bool gpuPolicyViaCurveBatch, int desiredActiveGpuOffsetExcludeLowCount,
    int currentAppliedGpuOffsetMHz, int currentActiveGpuOffsetExcludeLowCount,
    const bool* originalCurvePopulated, const int* originalCurveOffsets,
    const int* originalCurveFreqkHz, const bool* lockedTailMask,
    int* targetCurveOffsets, bool* targetCurveMask,
    // The GPU offset the curve batch carries (CurveOffsetRoutingPlan): the
    // request's own, or 0 where a separate NVML scalar owns it.  Required, so
    // no caller can fall back to writing the offset into both places.
    int batchOffsetMHz,
    // Per point, the measured base shift of this apply's NVML write
    // (curve_point_bounded_scalar_shift_khz()), freshly measured or retained
    // for the same GPU session/scalar/topology, or null when no separate
    // scalar needs compensation. Subtracted from provenance points only.
    const int* provenanceScalarShiftKHz,
    CurveTargetBuildFailure* failureOut = nullptr) {
    if (failureOut) {
        *failureOut = {};
        failureOut->ci = -1;
    }
    auto recordFailure = [&](CurveTargetBuildFailureReason reason, int ci, bool isAnchor,
                             unsigned int targetMHz, long long baseKHz,
                             long long offsetKHz, long long offsetHz,
                             const VfOffsetRange& range) {
        if (!failureOut) return;
        failureOut->reason = reason;
        failureOut->ci = ci;
        failureOut->isAnchor = isAnchor;
        failureOut->targetMHz = targetMHz;
        failureOut->baseKHz = baseKHz;
        failureOut->offsetKHz = offsetKHz;
        failureOut->offsetHz = offsetHz;
        failureOut->rangeMinKHz = range.minKHz;
        failureOut->rangeMaxKHz = range.maxKHz;
    };
    const VfOffsetRange range = vf_offset_range_current();
    if (!desired_curve_intent_valid(desired)) {
        recordFailure(CURVE_TARGET_BUILD_INVALID_INTENT, -1, false, 0, 0, 0, 0, range);
        debug_log("curve target refused: invalid curve intent\n");
        return false;
    }
    int capturedCount = 0, fractionalCount = 0;
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci)
        if (desired->curvePointCaptured[ci]) {
            ++capturedCount;
            if (desired->curvePointCapturedOffsetHz[ci] % 1000) ++fractionalCount;
        }
    if (capturedCount) debug_log("curve target intent: captured=%d subKHz=%d; programmed Hz offsets own the targets\n",
        capturedCount, fractionalCount);
    if (hasLock && lockMode == LOCK_MODE_FLATTEN &&
        !vf_offset_range_supports_flatten(range)) {
        recordFailure(CURVE_TARGET_BUILD_FLATTEN_UNSUPPORTED, lockCi, true, lockMhz, 0,
            0, 0, range);
        debug_log("curve target refused: flatten unsupported by range=%d..%d\n",
            range.minKHz, range.maxKHz);
        return false;
    }
    if (!curveRequest && !preserveCurveAcrossMem) return true;
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        if (!originalCurvePopulated[ci]) {
            if (desired_curve_point_present(desired, ci)) {
                recordFailure(CURVE_TARGET_BUILD_UNPOPULATED_POINT, ci, false,
                    desired->curvePointMHz[ci], 0, 0, 0, range);
                debug_log("curve target refused: ci=%d requested but unpopulated\n", ci);
                return false;
            }
            continue;
        }
        long long offset = originalCurveOffsets[ci];
        long long pointBase = curve_point_stock_base_khz(originalCurveFreqkHz[ci], originalCurveOffsets[ci]);
        unsigned int pointTargetMHz = desired->curvePointMHz[ci];
        bool write = preserveCurveAcrossMem;
        if (gpuPolicyViaCurveBatch) {
            bool detected = currentAppliedGpuOffsetMHz != 0 ||
                currentActiveGpuOffsetExcludeLowCount > 0;
            int oldComponent = detected ? gpu_offset_component_mhz_for_point(ci,
                currentAppliedGpuOffsetMHz, currentActiveGpuOffsetExcludeLowCount) * 1000
                : originalCurveOffsets[ci];
            offset = (long long)originalCurveOffsets[ci] - oldComponent +
                (long long)gpu_offset_component_mhz_for_point(ci,
                    batchOffsetMHz, desiredActiveGpuOffsetExcludeLowCount) * 1000;
            write = true;
        }
        const bool tail = hasLock && lockMhz > 0 && lockedTailMask[ci];
        // A projected point must NOT be re-derived from the live base.  Its
        // absolute MHz was itself built over a DIFFERENT base sample, so
        // `absolute - live base` turns the offset that was asked for into a
        // different one the moment the driver re-reports the base -- which it
        // does under load.  The offset the request asks for is the whole intent
        // and needs no base at all: it is this request's own component for the
        // point, which is zero for a profile that carries no GPU offset (a
        // point saved with `pointN_offset_khz=0` means "leave this at stock").
        // Routing-independent on purpose: a re-apply that requests no offset
        // CHANGE has gpuPolicyViaCurveBatch == false, and a profile with no GPU
        // offset never sets it at all, but neither point is less projected.
        if (desired_curve_point_present(desired, ci) && !tail) {
            const bool projected = desired_curve_point_kind(desired, ci) == DESIRED_CURVE_PROJECTED;
            if (desired->curvePointCaptured[ci]) {
                long long hz = 0;
                if (!desired_curve_point_offset_hz(desired, ci, 0, 0, &hz) ||
                    hz < (long long)range.minKHz * 1000 || hz > (long long)range.maxKHz * 1000) {
                    recordFailure(CURVE_TARGET_BUILD_CAPTURED_OUT_OF_RANGE, ci, false,
                        0, pointBase, hz / 1000, hz, range);
                    debug_log("curve target refused: ci=%d captured offsetHz=%lld range=%d..%d kHz\n",
                        ci, hz, range.minKHz, range.maxKHz);
                    return false;
                }
                // This array is the legacy correction/display projection. The
                // exact writer takes captured authority from DesiredSettings.
                offset = hz / 1000;
            } else if (!desired_curve_point_offset_khz(desired, ci,
                    pointBase,
                    gpu_offset_component_mhz_for_point(ci, desired->gpuOffsetMHz,
                        desiredActiveGpuOffsetExcludeLowCount) * 1000, &offset)) {
                recordFailure(CURVE_TARGET_BUILD_OFFSET_OVERFLOW, ci, false,
                    pointTargetMHz, pointBase, 0, 0, range);
                debug_log("curve target refused: ci=%d offset computation overflow (target=%u MHz base=%lld kHz)\n",
                    ci, pointTargetMHz, pointBase);
                return false;
            }
            // Measured, not assumed: the part of the component this apply's
            // NVML write already moved the point by (zero when it did not).
            if (projected && provenanceScalarShiftKHz)
                offset -= provenanceScalarShiftKHz[ci];
            write = true;
        }
        const bool isAnchor = tail && ci == lockCi;
        if (isAnchor) {
            long long base = (long long)originalCurveFreqkHz[ci] - originalCurveOffsets[ci];
            if (base < 0) base = 0;
            pointBase = base;
            pointTargetMHz = lockMhz;
            offset = (long long)lockMhz * 1000 - base;
            write = true;
        } else if (tail && lockMode == LOCK_MODE_FLATTEN) {
            offset = vf_offset_range_flatten_floor_khz(range);
            write = true;
        }
        if (write && (offset < INT_MIN || offset > INT_MAX ||
                      !vf_offset_range_permits_khz(range, (int)offset))) {
            const long long offsetHz = (offset > LLONG_MAX / 1000 || offset < LLONG_MIN / 1000)
                ? (offset > 0 ? LLONG_MAX : LLONG_MIN) : offset * 1000LL;
            recordFailure(CURVE_TARGET_BUILD_OFFSET_OUT_OF_RANGE, ci, isAnchor,
                pointTargetMHz, pointBase, offset, offsetHz, range);
            debug_log("curve target refused: ci=%d anchor=%d target=%u MHz base=%lld kHz offset=%lld range=%d..%d\n",
                ci, isAnchor ? 1 : 0, pointTargetMHz, pointBase, offset, range.minKHz, range.maxKHz);
            return false;
        }
        targetCurveOffsets[ci] = (int)offset;
        targetCurveMask[ci] = write;
    }
    return true;
}
#endif
