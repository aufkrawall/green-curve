// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "modeled_curve_transition.h"
#include "scalar_curve_submission_policy.h"

// Backends may adopt only a complete, authorized model. Other transition
// guards remain required when an additional modeled check needs no guard.
[[nodiscard]] static inline bool scalar_curve_adopt_modeled_transition(ScalarCurveSubmissionReview& review,
    const ModeledCurveTransitionPlan& model) {
    if (model.status == MODELED_TRANSITION_NO_GUARD)
        return model.finalPeakHz > 0 && model.intermediatePeakHz > 0 && model.intermediatePeakHz <= model.finalPeakHz;
    if (model.status != MODELED_TRANSITION_GUARD_READY || !model.ceiling.arm || !model.ceiling.required ||
        !model.ceiling.ceilingMHz || model.ceiling.reason != APPLY_CEILING_REASON_MODELED_CURVE_TRANSITION ||
        model.ceiling.symmetricFallbackAllowed || model.ceiling.finalPinIsCeiling || model.ceiling.clampControlAbsent ||
        model.finalPeakHz <= 0 || model.intermediatePeakHz <= model.finalPeakHz ||
        model.ceiling.ceilingMHz > model.finalPeakHz / 1000000) {
        review.modeledCeiling = {}; return false;
    }
    review.modeledCeiling = apply_clock_ceiling_merge_modeled(review.modeledCeiling, model.ceiling, true);
    review.installedGuardRequired = true;
    return review.modeledCeiling.arm;
}
