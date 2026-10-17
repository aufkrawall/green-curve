// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_GPU_BACKEND_APPLY_TARGETS_H
#define GREEN_CURVE_GPU_BACKEND_APPLY_TARGETS_H

#include "curve_point_offset_policy.h"

// Fill `shiftOut` with each point's bounded base shift across this apply's
// NVML write (curve_point_bounded_scalar_shift_khz()) and return true, or
// return false when it is not measurable (the target build then keeps the
// full component).  Logs one summary line either way when it matters: a
// measured shift is also the field answer to the NVML/VF composition question.
static bool apply_measure_provenance_scalar_shift(const DesiredSettings* desired,
    bool offsetInVfTable, bool scalarWritten, bool scalarBeforeKnownZero,
    int writtenScalarKHz, const int* preFreqKHz, const int* preOffsetsKHz,
    const int* postFreqKHz, const int* postOffsetsKHz, int* shiftOut) {
    int provenancePoints = 0;
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        shiftOut[ci] = 0;
        if (desired->hasCurvePoint[ci] && desired->curvePointFromGpuOffset[ci]) provenancePoints++;
    }
    if (offsetInVfTable || provenancePoints == 0) return false;
    if (!curve_point_scalar_shift_measurable(offsetInVfTable, scalarWritten, scalarBeforeKnownZero)) {
        debug_log("nvml shift: not measurable (written=%d beforeZero=%d); %d provenance point(s)"
                  " keep their full component\n", scalarWritten ? 1 : 0,
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
    // (curve_point_bounded_scalar_shift_khz()), or null when it was not
    // measurable.  Subtracted from a provenance point's component only.
    const int* provenanceScalarShiftKHz) {
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
        if (desired->hasCurvePoint[ci] && !tail) {
            CurvePointOffsetRequest want = {};
            want.fromGpuOffset = desired->curvePointFromGpuOffset[ci];
            want.gpuOffsetComponentKHz = gpu_offset_component_mhz_for_point(ci,
                desired->gpuOffsetMHz, desiredActiveGpuOffsetExcludeLowCount) * 1000;
            want.absoluteMHz = desired->curvePointMHz[ci];
            want.liveBaseKHz = curve_point_stock_base_khz(
                originalCurveFreqkHz[ci], originalCurveOffsets[ci]);
            offset = curve_point_target_offset_khz(&want);
            // Measured, not assumed: the part of the component this apply's
            // NVML write already moved the point by (zero when it did not).
            if (want.fromGpuOffset && provenanceScalarShiftKHz)
                offset -= provenanceScalarShiftKHz[ci];
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
