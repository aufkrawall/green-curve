// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "modeled_curve_transition_factory.h"
#include "scalar_curve_modeled_review.h"
#include <memory>

namespace {
struct Host {
    DesiredSettings desired{};
    VFCurvePoint curve[VF_NUM_POINTS]{};
    int offsetsKHz[VF_NUM_POINTS]{};
    gc_i64 frequencies[VF_NUM_POINTS]{}, offsets[VF_NUM_POINTS]{}, components[VF_NUM_POINTS]{};
    CurveAuthoringTargets targets{};
    CurvePlanInput baseline{};
    ModeledCurveTransitionControls controls{};
    Host() {
        desired.authoring.topologyBound = 1;
        desired.authoring.topologyCount = 3;
        desired.hasGpuOffset = true;
        desired.gpuOffsetMHz = 100;
        desired.authoring.graphicsPrecise = 1;
        desired.authoring.graphicsSurface = DESIRED_GRAPHICS_P0;
        desired.authoring.graphicsOffsetHz = 100000000;
        baseline.desired = &desired;
        baseline.curve = curve;
        baseline.programmedOffsetsKHz = offsetsKHz;
        baseline.frequencyHz = frequencies;
        baseline.programmedOffsetsHz = offsets;
        baseline.gpuComponentHz = components;
        baseline.floor = {true, 24, 21, 21, 0, 0};
        baseline.evidence = CURVE_PLAN_EVIDENCE_TRANSACTION_BASELINE;
        baseline.offsetGridHz = 1000;
        baseline.range = {-1000000, 1000000, CURVE_PLAN_RANGE_POLICY_FALLBACK};
        controls.scalarBeforeCurve = controls.pinMutationAuthorized = controls.clampAvailable = true;
        controls.composition = CAPTURE_COMPOSITION_INDEPENDENT;
        controls.scalarAfterHz = 100000000;
        targets.pointCount = 3;
        for (int i = 0; i < 3; ++i) {
            const int ci = 21 + i;
            const gc_u32 uv = 650000 + i * 12500;
            frequencies[ci] = 1000000000LL + i * 200000000LL;
            curve[ci] = {(unsigned int)(frequencies[ci] / 1000), uv};
            desired.authoring.voltageUv[ci] = targets.voltageUv[ci] = uv;
            desired.curvePointCaptured[ci] = 1;
            desired.curvePointCapturedOffsetHz[ci] = -(100000000LL + i * 200000000LL);
            targets.mask[ci] = targets.capturedMask[ci] = true;
            targets.offsetHz[ci] = desired.curvePointCapturedOffsetHz[ci];
        }
    }
};
} // namespace
int run_modeled_curve_factory_tests() {
    auto h = std::make_unique<Host>();
    CurveAuthoringTargets output;
    auto plan = modeled_curve_transition_from_baseline(h->baseline, h->targets, h->controls, &output);
    if (plan.status != MODELED_TRANSITION_GUARD_READY || plan.finalPeakHz != 1000000000 ||
        plan.intermediatePeakHz != 1500000000 || plan.ceiling.ceilingMHz != 1000 || output.offsetHz[23] != -500000000 ||
        h->targets.offsetHz[23] != -500000000)
        return 9600;
    // Rebase absolute authority for the future scalar without modifying source intent.
    h->targets.absoluteMask[22] = true;
    h->targets.capturedMask[22] = false;
    h->targets.absoluteHz[22] = 900000000;
    h->desired.curvePointCaptured[22] = 0;
    h->desired.curvePointCapturedOffsetHz[22] = 0;
    h->desired.hasCurvePoint[22] = 1;
    h->desired.curvePointMHz[22] = 900;
    h->desired.authoring.pointFrequencyPrecise[22] = 1;
    h->desired.authoring.pointFrequencyHz[22] = 900000000;
    h->targets.offsetHz[22] = -300000000;
    h->targets.requiresFreshBaseline = true;
    plan = modeled_curve_transition_from_baseline(h->baseline, h->targets, h->controls, &output);
    if (plan.status != MODELED_TRANSITION_GUARD_READY || output.offsetHz[22] != -400000000 ||
        output.requiresFreshBaseline || h->targets.offsetHz[22] != -300000000 || h->targets.absoluteHz[22] != 900000000)
        return 9601;
    h->targets.absoluteHz[22] = h->desired.authoring.pointFrequencyHz[22] = 900000500;
    output.pointCount = 17;
    plan = modeled_curve_transition_from_baseline(h->baseline, h->targets, h->controls, &output);
    if (plan.status != MODELED_TRANSITION_TARGET_UNREPRESENTABLE || plan.issueCi != 22 || plan.issueHz != -399999500 ||
        output.pointCount != 17)
        return 9602;
    h->baseline.offsetGridHz = 500;
    if (modeled_curve_transition_from_baseline(h->baseline, h->targets, h->controls, &output).status !=
            MODELED_TRANSITION_GUARD_READY ||
        output.offsetHz[22] != -399999500)
        return 9603;
    h = std::make_unique<Host>();
    h->offsets[21] = -1000;
    h->offsetsKHz[21] = -1;
    output.pointCount = 17;
    if (modeled_curve_transition_from_baseline(h->baseline, h->targets, h->controls, &output).status !=
            MODELED_TRANSITION_BASELINE_REQUIRED ||
        output.pointCount != 17)
        return 9604;
    h = std::make_unique<Host>();
    h->controls.scalarBeforeHz = 1000000;
    if (modeled_curve_transition_from_baseline(h->baseline, h->targets, h->controls).status !=
        MODELED_TRANSITION_BASELINE_REQUIRED)
        return 9605;
    h = std::make_unique<Host>();
    h->baseline.evidence = CURVE_PLAN_EVIDENCE_LIVE_SHAPED_ESTIMATE;
    if (modeled_curve_transition_from_baseline(h->baseline, h->targets, h->controls).status !=
        MODELED_TRANSITION_BASELINE_REQUIRED)
        return 9606;
    h = std::make_unique<Host>();
    h->targets.voltageUv[22] += 1;
    if (modeled_curve_transition_from_baseline(h->baseline, h->targets, h->controls).status !=
        MODELED_TRANSITION_BINDING_CHANGED)
        return 9607;
    h = std::make_unique<Host>();
    h->desired.authoring.voltageUv[22] += 1;
    if (modeled_curve_transition_from_baseline(h->baseline, h->targets, h->controls).status !=
        MODELED_TRANSITION_BINDING_CHANGED)
        return 9608;
    h = std::make_unique<Host>();
    h->targets.pointCount = 2;
    if (modeled_curve_transition_from_baseline(h->baseline, h->targets, h->controls).status !=
        MODELED_TRANSITION_BINDING_CHANGED)
        return 9609;
    h = std::make_unique<Host>();
    h->targets.mask[40] = true;
    if (modeled_curve_transition_from_baseline(h->baseline, h->targets, h->controls).status !=
        MODELED_TRANSITION_BINDING_CHANGED)
        return 9610;
    h = std::make_unique<Host>();
    h->targets.offsetHz[23] = -1000000500;
    h->desired.curvePointCapturedOffsetHz[23] = -1000000500;
    if (modeled_curve_transition_from_baseline(h->baseline, h->targets, h->controls).status !=
        MODELED_TRANSITION_TARGET_UNREPRESENTABLE)
        return 9611;
    h = std::make_unique<Host>();
    h->controls.composition = CAPTURE_COMPOSITION_UNKNOWN;
    if (modeled_curve_transition_from_baseline(h->baseline, h->targets, h->controls).status !=
        MODELED_TRANSITION_COMPOSITION_REQUIRED)
        return 9612;
    h->controls.scalarBeforeCurve = false;
    h->controls.scalarAfterHz = 0;
    if (modeled_curve_transition_from_baseline(h->baseline, h->targets, h->controls).status !=
        MODELED_TRANSITION_NO_GUARD)
        return 9613;
    h->controls.multiStepCurve = true;
    h->targets.offsetHz[21] = 200000000;
    h->desired.hasGpuOffset = false;
    h->desired.gpuOffsetMHz = 0;
    h->desired.authoring.graphicsPrecise = 0;
    h->desired.authoring.graphicsSurface = 0;
    h->desired.authoring.graphicsOffsetHz = 0;
    h->desired.curvePointCapturedOffsetHz[21] = 200000000;
    if (modeled_curve_transition_from_baseline(h->baseline, h->targets, h->controls).status !=
        MODELED_TRANSITION_GUARD_READY)
        return 9614;
    h = std::make_unique<Host>();
    h->controls.pinMutationAuthorized = false;
    if (modeled_curve_transition_from_baseline(h->baseline, h->targets, h->controls).status !=
        MODELED_TRANSITION_PIN_AUTHORITY_REQUIRED)
        return 9615;
    h = std::make_unique<Host>();
    h->controls.clampAvailable = false;
    if (modeled_curve_transition_from_baseline(h->baseline, h->targets, h->controls).status !=
        MODELED_TRANSITION_GUARD_UNAVAILABLE)
        return 9616;
    h = std::make_unique<Host>();
    h->baseline.floor.floorEnd = 22;
    h->baseline.floor.floorRun = 1;
    if (modeled_curve_transition_from_baseline(h->baseline, h->targets, h->controls).status !=
        MODELED_TRANSITION_TARGET_UNREPRESENTABLE)
        return 9617;
    h = std::make_unique<Host>();
    h->desired.curvePointCapturedOffsetHz[23] -= 1000;
    if (modeled_curve_transition_from_baseline(h->baseline, h->targets, h->controls).status !=
        MODELED_TRANSITION_BINDING_CHANGED)
        return 9618;
    h = std::make_unique<Host>();
    h->controls.scalarAfterHz += 1000000;
    if (modeled_curve_transition_from_baseline(h->baseline, h->targets, h->controls).status !=
        MODELED_TRANSITION_BINDING_CHANGED)
        return 9619;
    h = std::make_unique<Host>();
    h->targets.absoluteMask[22] = true;
    h->targets.capturedMask[22] = false;
    h->targets.absoluteHz[22] = 900000000;
    if (modeled_curve_transition_from_baseline(h->baseline, h->targets, h->controls).status !=
        MODELED_TRANSITION_BINDING_CHANGED)
        return 9620;
    h = std::make_unique<Host>();
    h->targets.mask[22] = h->targets.capturedMask[22] = false;
    --h->targets.pointCount;
    if (modeled_curve_transition_from_baseline(h->baseline, h->targets, h->controls).status !=
        MODELED_TRANSITION_BINDING_CHANGED)
        return 9621;
    h = std::make_unique<Host>();
    h->controls.scalarAfterHz = h->desired.authoring.graphicsOffsetHz = 100000500;
    if (modeled_curve_transition_from_baseline(h->baseline, h->targets, h->controls).status !=
        MODELED_TRANSITION_TARGET_UNREPRESENTABLE)
        return 9622;
    h = std::make_unique<Host>();
    for (int ci = 21; ci < 24; ++ci) {
        h->desired.curvePointCaptured[ci] = 0;
        h->desired.curvePointCapturedOffsetHz[ci] = 0;
        h->desired.hasCurvePoint[ci] = 1;
        h->desired.curvePointMHz[ci] = (unsigned int)(h->frequencies[ci] / 1000000) + 200;
        h->desired.authoring.pointFrequencyPrecise[ci] = 1;
        h->desired.authoring.pointFrequencyHz[ci] = h->frequencies[ci] + 200000000;
        h->targets.absoluteMask[ci] = true;
        h->targets.capturedMask[ci] = false;
        h->targets.absoluteHz[ci] = h->desired.authoring.pointFrequencyHz[ci];
        h->targets.offsetHz[ci] = 200000000;
    }
    plan = modeled_curve_transition_from_baseline(h->baseline, h->targets, h->controls, &output);
    ScalarCurveSubmissionReview review;
    if (plan.status != MODELED_TRANSITION_NO_GUARD || plan.finalPeakHz != 1600000000 ||
        plan.intermediatePeakHz != 1500000000 || output.offsetHz[23] != 100000000 ||
        !scalar_curve_adopt_modeled_transition(review, plan) || review.installedGuardRequired)
        return 9623;
    return 0;
}
