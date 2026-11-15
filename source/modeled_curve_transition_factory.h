// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "modeled_curve_transition.h"
#include "curve_authoring_targets.h"
#include "desired_graphics_surface.h"

struct ModeledCurveTransitionControls {
    HardwareCaptureComposition composition = CAPTURE_COMPOSITION_UNKNOWN;
    bool scalarBeforeCurve = false, multiStepCurve = false;
    bool pinMutationAuthorized = false, clampAvailable = false;
    gc_i64 scalarBeforeHz = 0, scalarAfterHz = 0, outgoingCeilingHz = 0;
};
// The caller supplies a fresh read under the runtime lane, after proving zero
// scalar/VF controls or verifying Reset. Source-file observations are not this
// baseline. Prospective offsets stay separate from the source and actual writes.
[[nodiscard]] static inline ModeledCurveTransitionPlan
modeled_curve_transition_from_baseline(const CurvePlanInput &baseline, const CurveAuthoringTargets &targets,
                                       const ModeledCurveTransitionControls &controls,
                                       CurveAuthoringTargets *prospective = nullptr) {
    ModeledCurveTransitionPlan error;
    const auto fail = [&](ModeledCurveTransitionStatus status, int ci = -1, gc_i64 hz = 0) {
        error.status = status;
        error.issueCi = ci;
        error.issueHz = hz;
        return error;
    };
    if (!controls.multiStepCurve && (!controls.scalarBeforeCurve || controls.scalarAfterHz <= controls.scalarBeforeHz))
        return {};
    if (!baseline.desired || !baseline.curve || !baseline.programmedOffsetsKHz || !baseline.frequencyHz ||
        !baseline.programmedOffsetsHz || !baseline.floor.valid || baseline.floor.graphicsEnd <= 0 ||
        baseline.floor.graphicsEnd > VF_NUM_POINTS)
        return fail(MODELED_TRANSITION_TOPOLOGY_REQUIRED);
    if (!desired_authoring_valid(baseline.desired) || !desired_curve_intent_valid(baseline.desired) ||
        !baseline.desired->authoring.topologyBound ||
        (baseline.desired->hasLock && baseline.desired->lockMode != LOCK_MODE_NONE))
        return fail(MODELED_TRANSITION_INVALID);
    const gc_u32 scalarSurface = desired_named_graphics_surface(*baseline.desired);
    if ((scalarSurface &&
         (!controls.scalarBeforeCurve || controls.scalarAfterHz != desired_graphics_hz(*baseline.desired))) ||
        (!scalarSurface && (controls.scalarBeforeCurve || controls.scalarAfterHz)))
        return fail(MODELED_TRANSITION_BINDING_CHANGED);
    if (controls.scalarAfterHz % 1000000)
        return fail(MODELED_TRANSITION_TARGET_UNREPRESENTABLE, -1, controls.scalarAfterHz);
    if (!curve_plan_baseline_proves(baseline.evidence) || controls.scalarBeforeHz ||
        !curve_plan_exact_samples_valid(baseline))
        return fail(MODELED_TRANSITION_BASELINE_REQUIRED);
    if (controls.scalarAfterHz && controls.composition != CAPTURE_COMPOSITION_INDEPENDENT)
        return fail(MODELED_TRANSITION_COMPOSITION_REQUIRED);
    const gc_i64 grid = baseline.offsetGridHz ? baseline.offsetGridHz : 1000;
    const gc_i64 minimum = (gc_i64)baseline.range.minKHz * 1000, maximum = (gc_i64)baseline.range.maxKHz * 1000;
    if (grid <= 0 || minimum > maximum)
        return fail(MODELED_TRANSITION_TARGET_UNREPRESENTABLE);
    ModeledCurveTransitionInput input;
    input.baseline = baseline.evidence;
    input.composition = controls.composition;
    input.completeTopology = true;
    input.scalarBeforeCurve = controls.scalarBeforeCurve;
    input.multiStepCurve = controls.multiStepCurve;
    input.pinMutationAuthorized = controls.pinMutationAuthorized;
    input.clampAvailable = controls.clampAvailable;
    input.scalarBeforeHz = controls.scalarBeforeHz;
    input.scalarAfterHz = controls.scalarAfterHz;
    input.outgoingCeilingHz = controls.outgoingCeilingHz;
    CurveAuthoringTargets next = targets;
    next.requiresFreshBaseline = false;
    int writes = 0;
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        const bool populated = ci < baseline.floor.graphicsEnd && baseline.curve[ci].freq_kHz;
        const gc_u32 voltage = populated ? baseline.curve[ci].volt_uV : 0;
        if (baseline.desired->authoring.voltageUv[ci] != voltage || (populated && targets.voltageUv[ci] != voltage))
            return fail(MODELED_TRANSITION_BINDING_CHANGED, ci);
        if (!populated) {
            if (targets.mask[ci] || desired_curve_point_present(baseline.desired, ci))
                return fail(MODELED_TRANSITION_BINDING_CHANGED, ci);
            continue;
        }
        if (baseline.programmedOffsetsHz[ci])
            return fail(MODELED_TRANSITION_BASELINE_REQUIRED, ci);
        if ((targets.absoluteMask[ci] || targets.capturedMask[ci] || targets.preservedMask[ci]) && !targets.mask[ci])
            return fail(MODELED_TRANSITION_INVALID, ci);
        if (targets.absoluteMask[ci] && targets.capturedMask[ci])
            return fail(MODELED_TRANSITION_INVALID, ci);
        const auto kind = desired_curve_point_kind(baseline.desired, ci);
        if (kind != DESIRED_CURVE_ABSENT && !targets.mask[ci])
            return fail(MODELED_TRANSITION_BINDING_CHANGED, ci);
        gc_i64 after = 0;
        if (targets.mask[ci]) {
            ++writes;
            if (vf_point_target_status(baseline.floor, baseline.curve, ci) != VF_POINT_TARGET_OK)
                return fail(MODELED_TRANSITION_TARGET_UNREPRESENTABLE, ci);
            after = targets.offsetHz[ci];
            if ((kind == DESIRED_CURVE_ABSOLUTE) != targets.absoluteMask[ci] ||
                (kind == DESIRED_CURVE_CAPTURED) != targets.capturedMask[ci])
                return fail(MODELED_TRANSITION_BINDING_CHANGED, ci);
            if (targets.absoluteMask[ci]) {
                if (targets.absoluteHz[ci] != curve_plan_absolute_hz(baseline.desired, ci))
                    return fail(MODELED_TRANSITION_BINDING_CHANGED, ci);
                gc_i64 raisedBase = 0;
                if (!modeled_curve_add(baseline.frequencyHz[ci], controls.scalarAfterHz, raisedBase) ||
                    raisedBase <= 0 || targets.absoluteHz[ci] <= 0 ||
                    !modeled_curve_add(targets.absoluteHz[ci], -raisedBase, after))
                    return fail(MODELED_TRANSITION_ARITHMETIC, ci);
            } else {
                gc_i64 expected = 0;
                if (kind != DESIRED_CURVE_ABSENT) {
                    if (!desired_curve_point_offset_from_base_hz(baseline.desired, ci, baseline.frequencyHz[ci],
                                                                 curve_plan_component_hz(baseline, ci), &expected))
                        return fail(MODELED_TRANSITION_BINDING_CHANGED, ci);
                } else if (!targets.preservedMask[ci])
                    expected = curve_plan_component_hz(baseline, ci);
                if (after != expected)
                    return fail(MODELED_TRANSITION_BINDING_CHANGED, ci);
            }
            if (after < minimum || after > maximum || after % grid)
                return fail(MODELED_TRANSITION_TARGET_UNREPRESENTABLE, ci, after);
            next.offsetHz[ci] = after;
        }
        input.points[input.count++] = {ci, voltage, baseline.frequencyHz[ci], 0, after};
    }
    if (!input.count || (gc_u32)input.count != baseline.desired->authoring.topologyCount ||
        writes != targets.pointCount)
        return fail(MODELED_TRANSITION_BINDING_CHANGED);
    for (int i = 1; i < input.count; ++i) {
        const ModeledCurveTransitionPoint key = input.points[i];
        int j = i - 1;
        while (j >= 0 && input.points[j].voltageUv > key.voltageUv) {
            input.points[j + 1] = input.points[j];
            --j;
        }
        input.points[j + 1] = key;
    }
    const auto plan = modeled_curve_transition_plan(input, true);
    if (plan.status == MODELED_TRANSITION_INVALID || plan.status == MODELED_TRANSITION_ARITHMETIC)
        return plan;
    if (prospective)
        *prospective = next;
    return plan;
}
