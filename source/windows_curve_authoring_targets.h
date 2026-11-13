// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "curve_authoring_targets.h"
#include "curve_point_verify_policy.h"
#include <stdio.h>

// Preserve controls outside the authored write set when a memory setter can
// clear them. Those transport writes confer no point ownership. Corrections
// retain the transaction's original preimage instead of adopting clobbered
// controls from the memory setter as a new baseline.
static inline bool windows_build_authoring_curve_targets(const CurvePlanInput& in,
    const VfBackendSpec* backend, const unsigned char* membership, bool composeGraphics,
    bool preserveUnowned, const CurveAuthoringTargets* original,
    CurveAuthoringTargets& output, CurveAuthoringTargetIssue& issue, bool allowDeferredBaseline = false) {
    CurveAuthoringTargets next;
    if (!curve_authoring_build_targets(in, backend, membership, composeGraphics, next, issue, allowDeferredBaseline)) return false;
    if (preserveUnowned) for (int i = 0; i < VF_NUM_POINTS && i < in.floor.graphicsEnd; ++i) {
        if (!in.curve[i].freq_kHz || next.mask[i]) continue;
        const gc_i64 hz = original && original->preservedMask[i]
            ? original->offsetHz[i] : in.programmedOffsetsHz[i];
        if (!(membership[i / 8] & (1u << (i % 8)))) {
            if (hz) { issue = {CURVE_AUTHORING_TARGET_POINT_UNWRITABLE, i, hz}; return false; }
            continue;
        }
        int raw = 0;
        if (hz < (gc_i64)in.range.minKHz * 1000 || hz > (gc_i64)in.range.maxKHz * 1000 ||
            !vf_graphics_offset_driver_units_hz(backend, hz, &raw)) {
            issue = {CURVE_AUTHORING_TARGET_UNREPRESENTABLE, i, hz}; return false;
        }
        next.mask[i] = next.preservedMask[i] = true;
        next.offsetHz[i] = hz; ++next.pointCount;
    }
    output = next;
    return true;
}

static inline bool windows_verify_authoring_curve_targets(const VFCurvePoint* curve,
    const gc_i64* frequencyHz, const gc_i64* offsetsHz, bool frequenciesValid, bool offsetsValid,
    const CurveAuthoringTargets& targets, LockMode lockMode, int lockCi, char* why, size_t whySize) {
    const auto fail = [&](const char* message, int ci) {
        if (why && whySize) snprintf(why, whySize, "%s (point %d)", message, ci);
        return false;
    };
    if (!curve || !frequencyHz || !offsetsHz || !frequenciesValid || !offsetsValid || targets.requiresFreshBaseline)
        return fail("Fresh exact curve readback is unavailable", -1);
    auto legacyFrequency = [&](int i) { return curve[i].freq_kHz; };
    auto exactFrequency = [&](int i) { return frequencyHz[i]; };
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        if (curve[i].volt_uV != targets.voltageUv[i])
            return fail("The curve voltage binding changed", i);
        if ((curve[i].freq_kHz && (frequencyHz[i] <= 0 || frequencyHz[i] / 1000 != curve[i].freq_kHz)) ||
            (targets.mask[i] && !curve[i].freq_kHz))
            return fail("The curve readback no longer matches the reviewed points", i);
        if (!targets.mask[i]) continue;
        // A hard pin owns its tail frequency; preservation outside its actual
        // VF write set still owns the original raw control, not a preview.
        if (lockMode == LOCK_MODE_HARD && lockCi >= 0 && i >= lockCi && !targets.preservedMask[i]) continue;
        if (targets.absoluteMask[i]) {
            if (!curve_point_absolute_target_verified_hz_over(legacyFrequency, exactFrequency,
                    i, VF_NUM_POINTS, targets.absoluteHz[i]))
                return fail("The exact frequency target did not verify", i);
        } else if (offsetsHz[i] != targets.offsetHz[i])
            return fail("The programmed curve offset did not retain its exact value", i);
    }
    if (lockMode == LOCK_MODE_FLATTEN && lockCi >= 0 && lockCi < VF_NUM_POINTS) {
        const gc_i64 plateau = targets.absoluteHz[lockCi];
        for (int i = 0; i < lockCi; ++i) {
            if (!curve[i].freq_kHz) continue;
            const gc_i64 base = frequencyHz[i] > offsetsHz[i] ? frequencyHz[i] - offsetsHz[i] : 0;
            const gc_i64 intended = targets.absoluteMask[i] ? targets.absoluteHz[i]
                : targets.mask[i] ? base + targets.offsetHz[i] : frequencyHz[i];
            if (intended > plateau)
                return fail("A preceding point would raise the requested flat tail", i);
        }
    }
    return true;
}

// A correction may change the offset needed for an absolute frequency, but
// cannot grow the reviewed write set or change the meaning of a point.
static inline bool windows_authoring_curve_shape_matches(const CurveAuthoringTargets& original,
    const CurveAuthoringTargets& next) {
    if (original.pointCount != next.pointCount || next.requiresFreshBaseline) return false;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        if (original.voltageUv[i] != next.voltageUv[i] || original.mask[i] != next.mask[i] ||
            original.absoluteMask[i] != next.absoluteMask[i] || original.absoluteHz[i] != next.absoluteHz[i] ||
            original.capturedMask[i] != next.capturedMask[i] || original.preservedMask[i] != next.preservedMask[i] ||
            original.baselineDependentMask[i] != next.baselineDependentMask[i] ||
            (original.mask[i] && !original.baselineDependentMask[i] && original.offsetHz[i] != next.offsetHz[i])) return false;
    }
    return true;
}
