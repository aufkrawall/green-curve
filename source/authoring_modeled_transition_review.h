// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once

#include <stdio.h>
#include <string.h>
#include "modeled_curve_transition_factory.h"
#include "scalar_curve_modeled_review.h"

struct AuthoringModeledPrecheckContext {
    HardwareCaptureComposition composition = CAPTURE_COMPOSITION_UNKNOWN;
    bool pinMutationAuthorized = false;
    bool pinAuthorityExplicit = false;
    bool clampAvailable = false;
    bool clampAvailabilityExplicit = false;
    bool multiStepCurve = false;
    gc_i64 outgoingCeilingHz = 0;
};

struct AuthoringModeledTransitionInput {
    const CurvePlanInput* baseline = nullptr;
    const CurveAuthoringTargets* targets = nullptr;
    HardwareCaptureComposition composition = CAPTURE_COMPOSITION_UNKNOWN;
    bool pinMutationAuthorized = false;
    bool clampAvailable = false;
    bool scalarBeforeCurve = false;
    bool multiStepCurve = false;
    gc_i64 scalarBeforeHz = 0;
    gc_i64 scalarAfterHz = 0;
    gc_i64 outgoingCeilingHz = 0;
    bool resetsToStock = false;
    bool pureNonLoweringCapturedDelta = false;
    bool hasOutgoingSurfaceHandoff = false;
};

struct AuthoringModeledTransitionOutcome {
    bool allowed = false;
    bool deferredToBaseline = false;
    bool adopted = false;
    ModeledCurveTransitionPlan plan{};
};

template <class D>
static inline bool authoring_pin_mutation_authorized(const D& d, bool releasesCurveCeiling = false) {
    if (d.hasLock && d.lockMode != LOCK_MODE_NONE) return true;
    if (releasesCurveCeiling) return true;
    const gc_u32 scope = desired_application_scope_kind(d.applicationScope);
    if (scope == DESIRED_SCOPE_UPDATE_INCLUDED) return false;
    if (scope == DESIRED_SCOPE_REPLACE_PROFILE) return true;
    if (scope == DESIRED_SCOPE_LEGACY) {
        if (d.hasGpuOffset || d.resetOcBeforeApply) return true;
        for (int i = 0; i < VF_NUM_POINTS; ++i)
            if (desired_curve_point_present(&d, i)) return true;
    }
    return false;
}

static inline bool authoring_targets_pure_non_lowering_captured_delta(
    const DesiredSettings& d, const CurveAuthoringTargets& targets,
    const gc_i64* liveOffsetsHz, bool hasLock, bool hasOutgoingSurfaceHandoff,
    bool multiStepCurve, bool resetsToStock, gc_i64 scalarBeforeHz, gc_i64 scalarAfterHz) {
    if (hasLock || hasOutgoingSurfaceHandoff || multiStepCurve || resetsToStock ||
        scalarAfterHz < scalarBeforeHz || !targets.pointCount)
        return false;
    bool anyOwned = false;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        if (d.hasCurvePoint[i]) return false;
        if (!targets.mask[i]) continue;
        if (targets.preservedMask[i]) continue;
        if (!targets.capturedMask[i] || targets.absoluteMask[i] || targets.offsetHz[i] < 0)
            return false;
        if (liveOffsetsHz && targets.offsetHz[i] < liveOffsetsHz[i]) return false;
        anyOwned = true;
    }
    return anyOwned;
}

static inline bool authoring_all_populated_fixed_negative(
    const CurvePlanInput& baseline, const CurveAuthoringTargets& targets) {
    if (!baseline.curve || !baseline.floor.valid || baseline.floor.graphicsEnd <= 0 ||
        baseline.floor.graphicsEnd > VF_NUM_POINTS)
        return false;
    int populatedCount = 0;
    for (int i = 0; i < baseline.floor.graphicsEnd; ++i) {
        if (!baseline.curve[i].freq_kHz) continue;
        ++populatedCount;
        if (!targets.mask[i] || targets.absoluteMask[i] || targets.baselineDependentMask[i] ||
            targets.offsetHz[i] >= 0)
            return false;
    }
    return populatedCount > 0;
}

static inline bool authoring_modeled_transition_refuse(
    ScalarCurveSubmissionReview& review, char* why, size_t whySize, const char* msg) {
    review.installedGuardRequired = false;
    review.refusedWithoutWrite = true;
    if (why && whySize) snprintf(why, whySize, "%s", msg);
    return false;
}

static inline const char* authoring_modeled_status_message(ModeledCurveTransitionStatus status) {
    switch (status) {
        case MODELED_TRANSITION_PIN_AUTHORITY_REQUIRED:
            return scalar_curve_submission_reason(SCALAR_CURVE_PIN_EVIDENCE_REQUIRED);
        case MODELED_TRANSITION_GUARD_UNAVAILABLE:
            return scalar_curve_submission_reason(SCALAR_CURVE_GUARD_UNAVAILABLE);
        case MODELED_TRANSITION_BASELINE_REQUIRED:
            return "a fresh post-reset or unshaped curve baseline is required to model transition protection";
        default:
            return modeled_curve_transition_status_name(status);
    }
}

static inline bool authoring_modeled_transition_preflight(
    const AuthoringModeledTransitionInput& in, ScalarCurveSubmissionReview& review,
    AuthoringModeledTransitionOutcome& outcome, char* why, size_t whySize) {
    outcome = {};
    if (!in.baseline || !in.baseline->desired || !in.targets)
        return authoring_modeled_transition_refuse(
            review, why, whySize, "invalid modeled curve transition review input");
    const DesiredSettings& d = *in.baseline->desired;
    const bool hasLock = d.hasLock && in.baseline->lockMode != LOCK_MODE_NONE;
    if (!in.scalarBeforeCurve && !in.multiStepCurve) {
        outcome.allowed = true;
        outcome.plan.status = MODELED_TRANSITION_NO_GUARD;
        return true;
    }
    const gc_u32 scalarSurface = desired_named_graphics_surface(d);
    ModeledCurveTransitionControls controls{};
    controls.composition = in.composition;
    controls.scalarBeforeCurve = scalarSurface != 0;
    controls.multiStepCurve = in.multiStepCurve;
    controls.pinMutationAuthorized = in.pinMutationAuthorized;
    controls.clampAvailable = in.clampAvailable;
    controls.scalarBeforeHz = in.scalarBeforeHz;
    controls.scalarAfterHz = in.scalarAfterHz;
    controls.outgoingCeilingHz = in.outgoingCeilingHz;

    if (hasLock) {
        if (in.composition == CAPTURE_COMPOSITION_INDEPENDENT &&
            curve_plan_baseline_proves(in.baseline->evidence)) {
            outcome.plan = modeled_curve_transition_from_baseline(
                *in.baseline, *in.targets, controls);
            if (outcome.plan.status == MODELED_TRANSITION_GUARD_READY) {
                if (!scalar_curve_adopt_modeled_transition(review, outcome.plan))
                    return authoring_modeled_transition_refuse(
                        review, why, whySize, "modeled transition ceiling adoption failed");
                outcome.adopted = true;
            } else if (outcome.plan.status != MODELED_TRANSITION_NO_GUARD &&
                       outcome.plan.status != MODELED_TRANSITION_PIN_AUTHORITY_REQUIRED &&
                       outcome.plan.status != MODELED_TRANSITION_GUARD_UNAVAILABLE) {
                return authoring_modeled_transition_refuse(
                    review, why, whySize, authoring_modeled_status_message(outcome.plan.status));
            }
        }
        outcome.allowed = true;
        return true;
    }

    if (in.pureNonLoweringCapturedDelta && !in.resetsToStock &&
        !in.hasOutgoingSurfaceHandoff && !in.multiStepCurve &&
        in.scalarAfterHz >= in.scalarBeforeHz) {
        outcome.allowed = true;
        outcome.plan.status = MODELED_TRANSITION_NO_GUARD;
        return true;
    }

    if (!in.pinMutationAuthorized) {
        if (!in.resetsToStock &&
            in.baseline->evidence == CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE &&
            (in.composition == CAPTURE_COMPOSITION_INDEPENDENT ||
             (in.scalarBeforeHz == 0 && in.scalarAfterHz == 0))) {
            outcome.plan = modeled_curve_transition_from_baseline(
                *in.baseline, *in.targets, controls);
            if (outcome.plan.status == MODELED_TRANSITION_NO_GUARD) {
                outcome.allowed = true;
                return true;
            }
        }
        return authoring_modeled_transition_refuse(
            review, why, whySize,
            scalar_curve_submission_reason(SCALAR_CURVE_PIN_EVIDENCE_REQUIRED));
    }

    if (in.resetsToStock) {
        const bool usesScalar = (in.scalarBeforeCurve && in.scalarAfterHz != 0) ||
            in.scalarAfterHz > 0;
        if (usesScalar && in.composition != CAPTURE_COMPOSITION_INDEPENDENT)
            return authoring_modeled_transition_refuse(
                review, why, whySize,
                modeled_curve_transition_status_name(MODELED_TRANSITION_COMPOSITION_REQUIRED));
        if (!d.authoring.topologyBound || !in.baseline->floor.valid)
            return authoring_modeled_transition_refuse(
                review, why, whySize,
                modeled_curve_transition_status_name(MODELED_TRANSITION_TOPOLOGY_REQUIRED));
        if (curve_plan_baseline_proves(in.baseline->evidence) && !in.scalarBeforeHz) {
            outcome.plan = modeled_curve_transition_from_baseline(
                *in.baseline, *in.targets, controls);
            if (outcome.plan.status != MODELED_TRANSITION_NO_GUARD &&
                outcome.plan.status != MODELED_TRANSITION_GUARD_READY)
                return authoring_modeled_transition_refuse(
                    review, why, whySize, authoring_modeled_status_message(outcome.plan.status));
        } else if (!in.clampAvailable &&
                   ((in.scalarBeforeCurve && in.scalarAfterHz > 0) || in.multiStepCurve) &&
                   authoring_all_populated_fixed_negative(*in.baseline, *in.targets)) {
            return authoring_modeled_transition_refuse(
                review, why, whySize,
                scalar_curve_submission_reason(SCALAR_CURVE_GUARD_UNAVAILABLE));
        }
        review.deferredModeledCheck = true;
        outcome.allowed = true;
        outcome.deferredToBaseline = true;
        return true;
    }

    if (!curve_plan_baseline_proves(in.baseline->evidence))
        return authoring_modeled_transition_refuse(
            review, why, whySize,
            authoring_modeled_status_message(MODELED_TRANSITION_BASELINE_REQUIRED));

    outcome.plan = modeled_curve_transition_from_baseline(*in.baseline, *in.targets, controls);
    if (outcome.plan.status == MODELED_TRANSITION_NO_GUARD) {
        outcome.allowed = true;
        return true;
    }
    if (outcome.plan.status == MODELED_TRANSITION_GUARD_READY) {
        if (!scalar_curve_adopt_modeled_transition(review, outcome.plan))
            return authoring_modeled_transition_refuse(
                review, why, whySize, "modeled transition ceiling adoption failed");
        outcome.allowed = true;
        outcome.adopted = true;
        return true;
    }
    return authoring_modeled_transition_refuse(
        review, why, whySize, authoring_modeled_status_message(outcome.plan.status));
}

static inline bool authoring_modeled_transition_post_reset(
    const AuthoringModeledTransitionInput& in, ScalarCurveSubmissionReview& review,
    AuthoringModeledTransitionOutcome& outcome, char* why, size_t whySize) {
    outcome = {};
    if (!in.baseline || !in.baseline->desired || !in.targets ||
        in.baseline->evidence != CURVE_PLAN_EVIDENCE_TRANSACTION_BASELINE) {
        if (why && whySize)
            snprintf(why, whySize, "%s", "post-reset modeled review requires a verified transaction baseline");
        return false;
    }
    const DesiredSettings& d = *in.baseline->desired;
    if (!in.baseline->programmedOffsetsHz || !in.baseline->frequencyHz || !in.baseline->floor.valid ||
        in.baseline->floor.graphicsEnd <= 0 || in.baseline->floor.graphicsEnd > VF_NUM_POINTS) {
        if (why && whySize) snprintf(why, whySize, "%s", "post-reset exact baseline samples are unavailable");
        return false;
    }
    for (int i = 0; i < in.baseline->floor.graphicsEnd; ++i) {
        if (in.baseline->curve[i].freq_kHz && in.baseline->programmedOffsetsHz[i] != 0) {
            if (why && whySize)
                snprintf(why, whySize, "post-reset VF baseline still has nonzero offset at ci%d", i);
            return false;
        }
    }
    if (in.scalarBeforeHz != 0) {
        if (why && whySize)
            snprintf(why, whySize, "%s", "post-reset graphics scalar baseline is nonzero");
        return false;
    }
    if (!in.scalarBeforeCurve && !in.multiStepCurve) {
        outcome.allowed = true;
        outcome.plan.status = MODELED_TRANSITION_NO_GUARD;
        return true;
    }
    const bool hasLock = d.hasLock && in.baseline->lockMode != LOCK_MODE_NONE;
    if (hasLock && in.composition != CAPTURE_COMPOSITION_INDEPENDENT) {
        outcome.allowed = true;
        return true;
    }
    const gc_u32 scalarSurface = desired_named_graphics_surface(d);
    ModeledCurveTransitionControls controls{};
    controls.composition = in.composition;
    controls.scalarBeforeCurve = scalarSurface != 0;
    controls.multiStepCurve = in.multiStepCurve;
    controls.pinMutationAuthorized = in.pinMutationAuthorized;
    controls.clampAvailable = in.clampAvailable;
    controls.scalarBeforeHz = 0;
    controls.scalarAfterHz = in.scalarAfterHz;
    controls.outgoingCeilingHz = in.outgoingCeilingHz;

    outcome.plan = modeled_curve_transition_from_baseline(*in.baseline, *in.targets, controls);
    if (outcome.plan.status == MODELED_TRANSITION_NO_GUARD) {
        outcome.allowed = true;
        return true;
    }
    if (outcome.plan.status == MODELED_TRANSITION_GUARD_READY) {
        if (!scalar_curve_adopt_modeled_transition(review, outcome.plan)) {
            if (why && whySize) snprintf(why, whySize, "%s", "post-reset modeled ceiling adoption failed");
            return false;
        }
        outcome.allowed = true;
        outcome.adopted = true;
        return true;
    }
    if (why && whySize)
        snprintf(why, whySize, "%s", authoring_modeled_status_message(outcome.plan.status));
    return false;
}
