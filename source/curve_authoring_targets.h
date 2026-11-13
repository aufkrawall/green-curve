// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "curve_feasibility_plan.h"
#include "vf_curve_units.h"

enum CurveAuthoringTargetStatus {
    CURVE_AUTHORING_TARGET_OK,
    CURVE_AUTHORING_TARGET_INVALID,
    CURVE_AUTHORING_TARGET_SAMPLES_UNAVAILABLE,
    CURVE_AUTHORING_TARGET_BINDING_CHANGED,
    CURVE_AUTHORING_TARGET_ROUTE_UNRESOLVED,
    CURVE_AUTHORING_TARGET_POINT_UNWRITABLE,
    CURVE_AUTHORING_TARGET_UNREPRESENTABLE,
};
struct CurveAuthoringTargetIssue {
    CurveAuthoringTargetStatus status = CURVE_AUTHORING_TARGET_OK;
    int ci = -1;
    gc_i64 requestedHz = 0;
};
struct CurveAuthoringTargets {
    int pointCount = 0;
    bool requiresFreshBaseline = false;
    bool mask[VF_NUM_POINTS]{};
    gc_i64 offsetHz[VF_NUM_POINTS]{};
    bool absoluteMask[VF_NUM_POINTS]{};
    gc_i64 absoluteHz[VF_NUM_POINTS]{};
    bool capturedMask[VF_NUM_POINTS]{};
    bool preservedMask[VF_NUM_POINTS]{};
    unsigned int voltageUv[VF_NUM_POINTS]{};
    bool baselineDependentMask[VF_NUM_POINTS]{};
};

static inline bool curve_authoring_needs_exact_targets(const DesiredSettings* d) {
    if (!d) return false;
    if (d->authoring.lockPrecise || (d->authoring.graphicsPrecise &&
        d->authoring.graphicsSurface == DESIRED_GRAPHICS_VF_COMPONENT)) return true;
    for (int i = 0; i < VF_NUM_POINTS; ++i)
        if (d->authoring.pointFrequencyPrecise[i] ||
            (d->authoring.topologyBound && d->curvePointCaptured[i])) return true;
    return false;
}

// Compile only reviewed, resolved point-table authority. Named graphics
// scalars are separate writes: they never become an invented VF component.
// Callers run the complete feasibility/transition plan before mutation and
// repeat this compilation over a fresh transaction baseline when necessary.
// On failure output is untouched; issue contains only bounded numeric facts.
static inline bool curve_authoring_build_targets(const CurvePlanInput& in,
    const VfBackendSpec* backend, const unsigned char* membership,
    bool composeGraphics, CurveAuthoringTargets& output, CurveAuthoringTargetIssue& issue,
    bool allowDeferredBaseline = false) {
    issue = {};
    const auto fail = [&](CurveAuthoringTargetStatus status, int ci = -1, gc_i64 hz = 0) {
        issue.status = status; issue.ci = ci; issue.requestedHz = hz; return false;
    };
    const DesiredSettings* d = in.desired;
    if (!d || !in.curve || !in.programmedOffsetsKHz || !in.floor.valid || in.floor.graphicsEnd <= 0 ||
        in.floor.graphicsEnd > VF_NUM_POINTS || !membership ||
        !backend || !backend->writeSupported || !desired_authoring_valid(d) || !desired_curve_intent_valid(d))
        return fail(CURVE_AUTHORING_TARGET_INVALID);
    if (!in.frequencyHz || !in.programmedOffsetsHz || in.evidence == CURVE_PLAN_EVIDENCE_UNAVAILABLE ||
        !curve_plan_exact_samples_valid(in))
        return fail(CURVE_AUTHORING_TARGET_SAMPLES_UNAVAILABLE);
    if (!d->authoring.topologyBound) return fail(CURVE_AUTHORING_TARGET_BINDING_CHANGED);
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        const gc_i64 voltage = i < in.floor.graphicsEnd && in.curve[i].freq_kHz ? in.curve[i].volt_uV : 0;
        if (d->authoring.voltageUv[i] != voltage) return fail(CURVE_AUTHORING_TARGET_BINDING_CHANGED, i);
    }
    if (d->hasGpuOffset && d->authoring.graphicsPrecise &&
        (d->authoring.graphicsSurface == DESIRED_GRAPHICS_AUTO ||
         composeGraphics != (d->authoring.graphicsSurface == DESIRED_GRAPHICS_VF_COMPONENT)))
        return fail(CURVE_AUTHORING_TARGET_ROUTE_UNRESOLVED);
    if (in.range.minKHz > in.range.maxKHz) return fail(CURVE_AUTHORING_TARGET_INVALID);
    const bool lock = d->hasLock && in.lockMode != LOCK_MODE_NONE;
    if (lock && vf_point_target_status(in.floor, in.curve, d->lockCi) != VF_POINT_TARGET_OK)
        return fail(CURVE_AUTHORING_TARGET_POINT_UNWRITABLE, d->lockCi);
    const gc_i64 plateau = curve_plan_lock_hz(d);
    const gc_i64 minimum = (gc_i64)in.range.minKHz * 1000;
    const gc_i64 maximum = (gc_i64)in.range.maxKHz * 1000;
    if (lock && (plateau <= 0 || (in.lockMode == LOCK_MODE_FLATTEN &&
        in.tailStrategy == CURVE_PLAN_TAIL_UNIFORM_MIN_OFFSET && minimum >= 0)))
        return fail(CURVE_AUTHORING_TARGET_UNREPRESENTABLE, d->lockCi, plateau);
    if (lock && in.lockMode == LOCK_MODE_HARD && (in.lockGridHz <= 0 || plateau % in.lockGridHz))
        return fail(CURVE_AUTHORING_TARGET_UNREPRESENTABLE, d->lockCi, plateau);
    CurveAuthoringTargets next{};
    for (int i = 0; i < VF_NUM_POINTS; ++i)
        next.voltageUv[i] = in.curve[i].volt_uV;
    int ordinal = 0;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        const bool populated = i < in.floor.graphicsEnd && in.curve[i].freq_kHz != 0;
        const bool tail = lock && populated && i >= d->lockCi;
        const bool explicitPoint = desired_curve_point_has_target(d, i) && !tail;
        const bool excluded = d->authoring.identityExclusions
            ? d->authoring.graphicsExcluded[i] != 0 : ordinal < d->gpuOffsetExcludeLowCount;
        const bool writable = populated && vf_point_target_status(in.floor, in.curve, i) == VF_POINT_TARGET_OK &&
            (membership[i / 8] & (1u << (i % 8))) != 0;
        if (populated) ++ordinal;
        if (explicitPoint && !writable) return fail(CURVE_AUTHORING_TARGET_POINT_UNWRITABLE, i);
        if (!populated) continue;
        const bool lockWrites = tail && (i == d->lockCi || in.tailStrategy != CURVE_PLAN_TAIL_ANCHOR_ONLY);
        const bool globalWrites = composeGraphics && d->hasGpuOffset && !excluded && writable;
        if (!explicitPoint && !lockWrites && !globalWrites) continue;
        if (!writable) return fail(CURVE_AUTHORING_TARGET_POINT_UNWRITABLE, i);
        const gc_i64 base = curve_plan_base_hz(in, i);
        gc_i64 hz = 0;
        bool baselineDependent = false;
        if (lockWrites) {
            hz = i != d->lockCi && in.tailStrategy == CURVE_PLAN_TAIL_UNIFORM_MIN_OFFSET
                ? minimum : plateau - base;
            next.absoluteMask[i] = in.lockMode != LOCK_MODE_HARD;
            next.absoluteHz[i] = plateau;
            baselineDependent = i == d->lockCi || in.tailStrategy != CURVE_PLAN_TAIL_UNIFORM_MIN_OFFSET;
        } else if (explicitPoint) {
            const DesiredCurvePointKind kind = desired_curve_point_kind(d, i);
            if (kind == DESIRED_CURVE_PROJECTED && d->hasGpuOffset && !composeGraphics)
                return fail(CURVE_AUTHORING_TARGET_ROUTE_UNRESOLVED, i);
            const gc_i64 component = composeGraphics && d->hasGpuOffset && !excluded ? desired_graphics_hz(*d) : 0;
            if (!desired_curve_point_offset_from_base_hz(d, i, base, component, &hz))
                return fail(CURVE_AUTHORING_TARGET_UNREPRESENTABLE, i);
            next.capturedMask[i] = kind == DESIRED_CURVE_CAPTURED;
            next.absoluteMask[i] = kind == DESIRED_CURVE_ABSOLUTE;
            baselineDependent = next.absoluteMask[i];
            if (next.absoluteMask[i]) next.absoluteHz[i] = curve_plan_absolute_hz(d, i);
        } else hz = desired_graphics_hz(*d);
        int raw = 0;
        const bool inRange = hz >= minimum && hz <= maximum;
        const bool representable = vf_graphics_offset_driver_units_hz(backend, hz, &raw);
        if (!inRange || !representable) {
            // A range failure over a shaped estimate cannot prove refusal.
            // Keep only its write shape until an authorized baseline rebuild;
            // the deferred result must never reach a writer.
            if (allowDeferredBaseline && baselineDependent && !curve_plan_baseline_proves(in.evidence) &&
                hz % vf_graphics_offset_grid_hz(backend) == 0) {
                next.requiresFreshBaseline = true; hz = 0;
            } else return fail(CURVE_AUTHORING_TARGET_UNREPRESENTABLE, i, hz);
        }
        next.baselineDependentMask[i] = baselineDependent;
        next.mask[i] = true; next.offsetHz[i] = hz; ++next.pointCount;
    }
    output = next;
    return true;
}
