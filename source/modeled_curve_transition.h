// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "gpu_core.h"
#include "curve_feasibility_plan.h"
#include "hardware_capture_types.h"
#include "apply_clock_ceiling_policy.h"
#include <algorithm>
#include <limits>

struct ModeledCurveTransitionPoint {
    int ci = -1;
    gc_u32 voltageUv = 0;
    gc_i64 stockHz = 0, beforeOffsetHz = 0, afterOffsetHz = 0;
};
struct ModeledCurveTransitionInput {
    // Stock comes from an exact zero-control read or a verified reset readback,
    // never live-minus-offset arithmetic over a shaped capture. The backend
    // must bind this complete graphics topology to its locked fresh transaction.
    CurvePlanEvidence baseline = CURVE_PLAN_EVIDENCE_UNAVAILABLE;
    HardwareCaptureComposition composition = CAPTURE_COMPOSITION_UNKNOWN;
    bool completeTopology = false, scalarBeforeCurve = false, multiStepCurve = false;
    bool pinMutationAuthorized = false, clampAvailable = false;
    gc_i64 scalarBeforeHz = 0, scalarAfterHz = 0, outgoingCeilingHz = 0;
    int count = 0;
    ModeledCurveTransitionPoint points[VF_NUM_POINTS]{};
};
enum ModeledCurveTransitionStatus {
    MODELED_TRANSITION_NO_GUARD, MODELED_TRANSITION_INVALID, MODELED_TRANSITION_BASELINE_REQUIRED,
    MODELED_TRANSITION_COMPOSITION_REQUIRED, MODELED_TRANSITION_TOPOLOGY_REQUIRED,
    MODELED_TRANSITION_ARITHMETIC, MODELED_TRANSITION_PIN_AUTHORITY_REQUIRED,
    MODELED_TRANSITION_GUARD_UNAVAILABLE, MODELED_TRANSITION_GUARD_READY
};
struct ModeledCurveTransitionPlan {
    ModeledCurveTransitionStatus status = MODELED_TRANSITION_NO_GUARD;
    gc_i64 finalPeakHz = 0, intermediatePeakHz = 0;
    int finalPeakCi = -1, intermediatePeakCi = -1, collapsedPoints = 0;
    ApplyClockCeilingPlan ceiling{};
};
static inline const char* modeled_curve_transition_status_name(ModeledCurveTransitionStatus status) {
    switch (status) {
        case MODELED_TRANSITION_NO_GUARD: return "no intermediate peak requires a ceiling";
        case MODELED_TRANSITION_INVALID: return "invalid transition controls or graphics topology";
        case MODELED_TRANSITION_BASELINE_REQUIRED: return "a fresh verified stock baseline is required";
        case MODELED_TRANSITION_COMPOSITION_REQUIRED: return "scalar and VF composition is unresolved";
        case MODELED_TRANSITION_TOPOLOGY_REQUIRED: return "the complete graphics topology is required";
        case MODELED_TRANSITION_ARITHMETIC: return "a modeled frequency or ceiling is not representable";
        case MODELED_TRANSITION_PIN_AUTHORITY_REQUIRED: return "temporary pin changes require explicit authority";
        case MODELED_TRANSITION_GUARD_UNAVAILABLE: return "the modeled transition requires an available ceiling";
        case MODELED_TRANSITION_GUARD_READY: return "install the modeled ceiling before the raising submission";
    }
    return "invalid modeled transition";
}
static inline bool modeled_curve_add(gc_i64 left, gc_i64 right, gc_i64& result) {
    constexpr gc_i64 maximum = std::numeric_limits<gc_i64>::max(), minimum = std::numeric_limits<gc_i64>::min();
    if ((right > 0 && left > maximum - right) || (right < 0 && left < minimum - right)) return false;
    result = left + right; return true;
}
// This plans transition protection only. NO_GUARD never validates a profile,
// grants ownership or replaces the ordinary feasibility/readback checks.
static inline ModeledCurveTransitionPlan modeled_curve_transition_plan(const ModeledCurveTransitionInput& in) {
    ModeledCurveTransitionPlan out;
    const auto fail = [&](ModeledCurveTransitionStatus status) { out.status = status; return out; };
    if (!in.scalarBeforeCurve && !in.multiStepCurve) return out; // Atomic VF-only replay stays uncapped.
    if (in.count <= 0 || in.count > VF_NUM_POINTS || in.outgoingCeilingHz < 0 ||
        (!in.scalarBeforeCurve && in.scalarBeforeHz != in.scalarAfterHz)) return fail(MODELED_TRANSITION_INVALID);
    bool seen[VF_NUM_POINTS]{}, lower = false, raise = in.scalarBeforeCurve && in.scalarAfterHz > in.scalarBeforeHz;
    gc_u32 voltage = 0;
    for (int i = 0; i < in.count; ++i) {
        const auto& p = in.points[i];
        if (p.ci < 0 || p.ci >= VF_NUM_POINTS || seen[p.ci] || p.voltageUv <= voltage || p.stockHz <= 0)
            return fail(MODELED_TRANSITION_INVALID);
        seen[p.ci] = true; voltage = p.voltageUv;
        lower |= p.afterOffsetHz < p.beforeOffsetHz;
        raise |= in.multiStepCurve && p.afterOffsetHz > p.beforeOffsetHz;
    }
    if (!raise || !lower) return out;
    if (!in.completeTopology) return fail(MODELED_TRANSITION_TOPOLOGY_REQUIRED);
    if (!curve_plan_baseline_proves(in.baseline)) return fail(MODELED_TRANSITION_BASELINE_REQUIRED);
    if (in.composition != CAPTURE_COMPOSITION_INDEPENDENT) return fail(MODELED_TRANSITION_COMPOSITION_REQUIRED);
    gc_i64 finalPrefix = 0, intermediatePrefix = 0;
    for (int i = 0; i < in.count; ++i) {
        const auto& p = in.points[i]; gc_i64 scalarBase = 0, finalHz = 0, intermediateHz = 0;
        const gc_i64 intermediateOffset = in.multiStepCurve ? std::max(p.beforeOffsetHz, p.afterOffsetHz) : p.beforeOffsetHz;
        if (!modeled_curve_add(p.stockHz, in.scalarAfterHz, scalarBase) ||
            !modeled_curve_add(scalarBase, p.afterOffsetHz, finalHz) ||
            !modeled_curve_add(scalarBase, intermediateOffset, intermediateHz) || finalHz <= 0 || intermediateHz <= 0)
            return fail(MODELED_TRANSITION_ARITHMETIC);
        // The driver raises a descending raw suffix to its prefix maximum.
        // Include every populated graphics point, including hidden/floor rows.
        if (finalHz < finalPrefix) ++out.collapsedPoints;
        if (finalHz > finalPrefix) { finalPrefix = finalHz; out.finalPeakCi = p.ci; }
        if (intermediateHz > intermediatePrefix) { intermediatePrefix = intermediateHz; out.intermediatePeakCi = p.ci; }
    }
    out.finalPeakHz = finalPrefix; out.intermediatePeakHz = intermediatePrefix;
    if (intermediatePrefix <= finalPrefix) return out;
    const gc_i64 bound = in.outgoingCeilingHz ? std::min(in.outgoingCeilingHz, finalPrefix) : finalPrefix;
    const gc_i64 mhz = bound / 1000000;
    if (mhz <= 0 || (unsigned long long)mhz > UINT_MAX) return fail(MODELED_TRANSITION_ARITHMETIC);
    out.ceiling.required = true; out.ceiling.ceilingMHz = (unsigned int)mhz;
    out.ceiling.reason = APPLY_CEILING_REASON_MODELED_CURVE_TRANSITION;
    // Omitted/unknown pins cannot be overwritten just to protect another
    // domain. The caller needs reviewed release/reset or owned-pin authority.
    if (!in.pinMutationAuthorized) return fail(MODELED_TRANSITION_PIN_AUTHORITY_REQUIRED);
    out.ceiling.clampControlAbsent = !in.clampAvailable; out.ceiling.arm = in.clampAvailable;
    // Lockless replay never permits the symmetric fallback's new clock floor.
    return fail(in.clampAvailable ? MODELED_TRANSITION_GUARD_READY : MODELED_TRANSITION_GUARD_UNAVAILABLE);
}
