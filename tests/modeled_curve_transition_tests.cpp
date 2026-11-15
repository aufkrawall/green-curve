// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "gpu_core.h"
#include "modeled_curve_transition.h"
#include "scalar_curve_modeled_review.h"

int run_modeled_curve_transition_tests() {
    ModeledCurveTransitionInput in;
    in.count = 4; in.completeTopology = true; in.baseline = CURVE_PLAN_EVIDENCE_TRANSACTION_BASELINE;
    in.composition = CAPTURE_COMPOSITION_INDEPENDENT; in.scalarBeforeCurve = true;
    in.pinMutationAuthorized = in.clampAvailable = true; in.scalarAfterHz = 100000000;
    for (int i = 0; i < 4; ++i) in.points[i] = {21 + i, (gc_u32)(650000 + 12500*i), 400000000 + 100000000LL*i, 0,
        i == 0 ? 0 : -(100000000LL + 200000000LL*(i-1))};
    const auto original = in; auto p = modeled_curve_transition_plan(in);
    if (p.status != MODELED_TRANSITION_GUARD_READY || p.finalPeakHz != 500000000 || p.intermediatePeakHz != 800000000 ||
        p.finalPeakCi != 21 || p.intermediatePeakCi != 24 || p.collapsedPoints != 2 || p.ceiling.ceilingMHz != 500 ||
        !p.ceiling.required || !p.ceiling.arm || p.ceiling.finalPinIsCeiling || p.ceiling.symmetricFallbackAllowed) return 9550;
    in.outgoingCeilingHz = 450000500; p = modeled_curve_transition_plan(in);
    if (p.ceiling.ceilingMHz != 450 || p.finalPeakHz != 500000000 || p.ceiling.symmetricFallbackAllowed) return 9551;
    in.outgoingCeilingHz = 900000000;
    if (modeled_curve_transition_plan(in).ceiling.ceilingMHz != 500) return 9552;
    in = original; in.pinMutationAuthorized = false; p = modeled_curve_transition_plan(in);
    if (p.status != MODELED_TRANSITION_PIN_AUTHORITY_REQUIRED || p.ceiling.arm || !p.ceiling.required) return 9553;
    in = original; in.clampAvailable = false; p = modeled_curve_transition_plan(in);
    if (p.status != MODELED_TRANSITION_GUARD_UNAVAILABLE || p.ceiling.arm || !p.ceiling.clampControlAbsent ||
        !apply_clock_ceiling_transition_must_refuse(p.ceiling.required, APPLY_CEILING_ARM_UNSUPPORTED, p.ceiling.reason) ||
        !apply_clock_ceiling_transition_must_refuse(p.ceiling.required, APPLY_CEILING_ARM_REFUSED, p.ceiling.reason) ||
        apply_clock_ceiling_transition_must_refuse(p.ceiling.required, APPLY_CEILING_ARM_INSTALLED, p.ceiling.reason)) return 9554;
    for (const auto evidence : {CURVE_PLAN_EVIDENCE_UNAVAILABLE, CURVE_PLAN_EVIDENCE_LIVE_SHAPED_ESTIMATE}) {
        in = original; in.baseline = evidence;
        if (modeled_curve_transition_plan(in).status != MODELED_TRANSITION_BASELINE_REQUIRED) return 9555;
    }
    in = original; in.baseline = CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE;
    if (modeled_curve_transition_plan(in).status != MODELED_TRANSITION_GUARD_READY) return 9556;
    for (const auto composition : {CAPTURE_COMPOSITION_UNKNOWN, CAPTURE_COMPOSITION_ALIASED}) {
        in = original; in.composition = composition;
        if (modeled_curve_transition_plan(in).status != MODELED_TRANSITION_COMPOSITION_REQUIRED) return 9557;
    }
    in = original; in.completeTopology = false;
    if (modeled_curve_transition_plan(in).status != MODELED_TRANSITION_TOPOLOGY_REQUIRED) return 9558;
    in = {}; // Neither missing baseline nor absent clamp creates a lockless single-batch requirement.
    if (modeled_curve_transition_plan(in).status != MODELED_TRANSITION_NO_GUARD) return 9559;
    in = original; in.scalarBeforeCurve = false; in.scalarAfterHz = 0; in.clampAvailable = false;
    if (modeled_curve_transition_plan(in).status != MODELED_TRANSITION_NO_GUARD) return 9560;
    in = original; in.scalarAfterHz = -100000000;
    if (modeled_curve_transition_plan(in).status != MODELED_TRANSITION_NO_GUARD) return 9561;
    in = original; in.scalarBeforeCurve = false; in.scalarAfterHz = 0; in.multiStepCurve = true;
    in.points[0].afterOffsetHz = 200000000; p = modeled_curve_transition_plan(in);
    if (p.status != MODELED_TRANSITION_GUARD_READY || p.finalPeakHz != 600000000 || p.intermediatePeakHz != 700000000 ||
        p.ceiling.ceilingMHz != 600) return 9562;
    in = original; in.points[3].afterOffsetHz = 100000000; p = modeled_curve_transition_plan(in);
    if (p.status != MODELED_TRANSITION_NO_GUARD || p.finalPeakHz != 900000000 || p.intermediatePeakHz != 800000000 || p.ceiling.required) return 9563;
    in = original; in.points[0].stockHz += 500; p = modeled_curve_transition_plan(in);
    if (p.finalPeakHz != 500000500 || p.ceiling.ceilingMHz != 500) return 9564;
    in = original; in.points[0].stockHz = std::numeric_limits<gc_i64>::max();
    if (modeled_curve_transition_plan(in).status != MODELED_TRANSITION_ARITHMETIC) return 9565;
    in = original; in.scalarAfterHz = std::numeric_limits<gc_i64>::min();
    in.multiStepCurve = true; in.points[0].afterOffsetHz = 1;
    if (modeled_curve_transition_plan(in).status != MODELED_TRANSITION_ARITHMETIC) return 9566;
    for (int bad = 0; bad < 7; ++bad) {
        in = original;
        if (bad == 0) in.count = 129; else if (bad == 1) in.count = 0;
        else if (bad == 2) in.points[1].ci = in.points[0].ci; else if (bad == 3) in.points[0].ci = -1;
        else if (bad == 4) in.points[1].voltageUv = in.points[0].voltageUv; else if (bad == 5) in.outgoingCeilingHz = -1;
        else in.points[0].stockHz = 0;
        if (modeled_curve_transition_plan(in).status != MODELED_TRANSITION_INVALID) return 9567;
    }
    in = original; in.scalarBeforeCurve = false; in.multiStepCurve = true;
    if (modeled_curve_transition_plan(in).status != MODELED_TRANSITION_INVALID) return 9568;
    in = original; in.outgoingCeilingHz = 999999;
    if (modeled_curve_transition_plan(in).status != MODELED_TRANSITION_ARITHMETIC) return 9569;
    gc_i64 sum = 17;
    if (modeled_curve_add(std::numeric_limits<gc_i64>::min(), -1, sum) || sum != 17 ||
        modeled_curve_add(std::numeric_limits<gc_i64>::max(), 1, sum) || sum != 17 ||
        !modeled_curve_add(std::numeric_limits<gc_i64>::min(), 1, sum) || sum != std::numeric_limits<gc_i64>::min() + 1) return 9570;
    p = modeled_curve_transition_plan(original); ScalarCurveSubmissionReview review;
    if (!scalar_curve_adopt_modeled_transition(review, p) || !review.installedGuardRequired ||
        review.modeledCeiling.ceilingMHz != 500 || !review.modeledCeiling.arm) return 9571;
    auto base = apply_clock_ceiling_plan(true, false, LOCK_MODE_NONE, 0, true, true, true, 450);
    auto merged = apply_clock_ceiling_merge_modeled(base, review.modeledCeiling, true);
    if (!merged.required || !merged.arm || merged.ceilingMHz != 450 || merged.symmetricFallbackAllowed ||
        merged.reason != APPLY_CEILING_REASON_MODELED_CURVE_TRANSITION ||
        !apply_clock_ceiling_transition_must_refuse(true, APPLY_CEILING_ARM_UNSUPPORTED, merged.reason)) return 9572;
    base = apply_clock_ceiling_plan(true, true, LOCK_MODE_HARD, 450, true);
    merged = apply_clock_ceiling_merge_modeled(base, review.modeledCeiling, true);
    if (merged.ceilingMHz != 450 || !merged.finalPinIsCeiling || merged.symmetricFallbackAllowed) return 9573;
    base = apply_clock_ceiling_plan(true, true, LOCK_MODE_HARD, 600, true);
    merged = apply_clock_ceiling_merge_modeled(base, review.modeledCeiling, true);
    if (merged.ceilingMHz != 500 || merged.finalPinIsCeiling || merged.symmetricFallbackAllowed) return 9574;
    base.required = true; base.ceilingMHz = 0;
    merged = apply_clock_ceiling_merge_modeled(base, review.modeledCeiling, true);
    if (!merged.required || merged.arm || merged.ceilingMHz) return 9575;
    auto broken = review.modeledCeiling; broken.symmetricFallbackAllowed = true;
    merged = apply_clock_ceiling_merge_modeled({}, broken, true);
    if (!merged.required || merged.arm || merged.ceilingMHz || merged.symmetricFallbackAllowed) return 9576;
    in = original; in.points[3].afterOffsetHz = 100000000;
    const auto safe = modeled_curve_transition_plan(in);
    if (!scalar_curve_adopt_modeled_transition(review, safe) || review.modeledCeiling.ceilingMHz != 500 ||
        scalar_curve_adopt_modeled_transition(review, {})) return 9577;
    p.status = MODELED_TRANSITION_BASELINE_REQUIRED;
    if (scalar_curve_adopt_modeled_transition(review, p) || review.modeledCeiling.required || !review.installedGuardRequired) return 9578;
    broken = {}; broken.required = true; broken.arm = true; broken.ceilingMHz = 600;
    if (apply_clock_ceiling_merge_modeled({}, broken, true).arm) return 9579;
    merged = apply_clock_ceiling_merge_modeled({}, modeled_curve_transition_plan(original).ceiling, false);
    if (merged.arm || !merged.required || !merged.clampControlAbsent || merged.ceilingMHz != 500) return 9580;
    p = modeled_curve_transition_plan(original); p.ceiling.ceilingMHz = 501;
    if (scalar_curve_adopt_modeled_transition(review, p) || review.modeledCeiling.required) return 9581;
    p = modeled_curve_transition_plan(original); p.ceiling.clampControlAbsent = true;
    if (scalar_curve_adopt_modeled_transition(review, p) || review.modeledCeiling.required) return 9582;
    in = original; in.count = VF_NUM_POINTS;
    for (int i = 0; i < VF_NUM_POINTS; ++i) in.points[i] = {i, (gc_u32)(650000 + i*1250),
        1000000000LL + i*1000000LL, 0, -200000000};
    p = modeled_curve_transition_plan(in);
    if (p.status != MODELED_TRANSITION_GUARD_READY || p.finalPeakHz != 1027000000 || p.finalPeakCi != 127 ||
        p.intermediatePeakHz != 1227000000 || p.ceiling.ceilingMHz != 1027) return 9592;
    broken = p.ceiling; broken.clampControlAbsent = true;
    merged = apply_clock_ceiling_merge_modeled({}, broken, true);
    if (merged.arm || !merged.required || merged.ceilingMHz) return 9593;
    return 0;
}
