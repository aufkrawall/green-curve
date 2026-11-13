// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "desired_graphics_surface.h"

enum ScalarCurveSubmissionStatus {
    SCALAR_CURVE_NO_SPLIT, SCALAR_CURVE_ORDER_SAFE, SCALAR_CURVE_INSTALL_GUARD,
    SCALAR_CURVE_CURRENT_UNAVAILABLE, SCALAR_CURVE_PIN_EVIDENCE_REQUIRED, SCALAR_CURVE_GUARD_UNAVAILABLE,
};
struct ScalarCurveSubmissionPlan {
    ScalarCurveSubmissionStatus status = SCALAR_CURVE_NO_SPLIT;
    int loweredCaptured = 0;
    bool baselineDependent = false;
};
struct ScalarCurveSubmissionReview {
    bool installedGuardRequired = false, beforeValid = false, refusedWithoutWrite = false;
    long long beforeHz = 0;
};
static inline unsigned int scalar_curve_rollback_phases(const ScalarCurveSubmissionReview& review,
    unsigned int phases, unsigned int scalarPhase) {
    return review.refusedWithoutWrite ? phases & ~scalarPhase : phases;
}
template <class Result>
static inline void scalar_curve_finalize_write_evidence(const ScalarCurveSubmissionReview& review,
    unsigned int scalarPhase, Result& result) {
    if (review.refusedWithoutWrite && !(result.attemptedPhases & ~scalarPhase))
        result.anyWrite = result.rollbackAttempted = result.rollbackSucceeded = false;
}
static inline bool scalar_curve_submission_allowed(const ScalarCurveSubmissionPlan& p) {
    return p.status == SCALAR_CURVE_NO_SPLIT || p.status == SCALAR_CURVE_ORDER_SAFE || p.status == SCALAR_CURVE_INSTALL_GUARD;
}
static inline const char* scalar_curve_submission_reason(ScalarCurveSubmissionStatus status) {
    switch (status) {
        case SCALAR_CURVE_NO_SPLIT: return "no separate scalar and curve submission";
        case SCALAR_CURVE_ORDER_SAFE: return "the scalar does not rise ahead of a curve reduction";
        case SCALAR_CURVE_INSTALL_GUARD: return "the declared clock target must protect the scalar-to-curve transition";
        case SCALAR_CURVE_CURRENT_UNAVAILABLE: return "the selected scalar or exact curve controls cannot be read";
        case SCALAR_CURVE_PIN_EVIDENCE_REQUIRED: return "a scalar increase before curve reduction needs reviewed transition and pin evidence or an explicit clock target";
        case SCALAR_CURVE_GUARD_UNAVAILABLE: return "the scalar-to-curve transition requires an available clock ceiling";
    }
    return "invalid scalar-to-curve transition";
}

// A named scalar and VF offsets are different submissions. Do not invent a
// lockless clamp from sampled MHz while a lower external pin may be unreadable.
// Absolute points rebase after a scalar write, so their reduction is unknown
// without a proven composition model. Raw captured controls can be compared
// directly. Safe single-batch paths remain independent of GPU family.
template <class D>
static inline ScalarCurveSubmissionPlan scalar_curve_submission_plan(const D& d,
    bool scalarCurrentValid, long long scalarCurrentHz, const long long* currentOffsetsHz,
    bool exactOffsetsValid, bool clampAvailable, bool scalarSurfaceHandback = false) {
    ScalarCurveSubmissionPlan p;
    if (!d.authoring.topologyBound || !desired_named_graphics_surface(d)) return p;
    bool curve = d.hasLock && d.lockMode != LOCK_MODE_NONE;
    for (int i = 0; i < 128; ++i) curve |= d.hasCurvePoint[i] || d.curvePointCaptured[i];
    if (!curve) return p;
    if (!scalarCurrentValid || !exactOffsetsValid || !currentOffsetsHz) {
        p.status = SCALAR_CURVE_CURRENT_UNAVAILABLE; return p;
    }
    p.status = SCALAR_CURVE_ORDER_SAFE;
    if (d.authoring.graphicsOffsetHz <= scalarCurrentHz && !scalarSurfaceHandback) return p;
    p.baselineDependent = scalarSurfaceHandback || (d.hasLock && d.lockMode != LOCK_MODE_NONE);
    for (int i = 0; i < 128; ++i) {
        p.baselineDependent |= d.hasCurvePoint[i] != 0;
        p.loweredCaptured += d.curvePointCaptured[i] && d.curvePointCapturedOffsetHz[i] < currentOffsetsHz[i];
    }
    if (!p.loweredCaptured && !p.baselineDependent) return p;
    const bool declaredTarget = d.hasLock && d.lockMode != LOCK_MODE_NONE && d.lockMHz > 0 &&
        (d.authoring.lockPrecise ? d.authoring.lockFrequencyHz > 0 : d.lockMHz > 0);
    p.status = !declaredTarget ? SCALAR_CURVE_PIN_EVIDENCE_REQUIRED :
        !clampAvailable ? SCALAR_CURVE_GUARD_UNAVAILABLE : SCALAR_CURVE_INSTALL_GUARD;
    return p;
}
