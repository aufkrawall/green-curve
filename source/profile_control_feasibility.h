// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_capture_curve_plan.h"
#include "power_ratio_control.h"

enum ProfileControlFeasibility { PROFILE_CONTROL_READY, PROFILE_CONTROL_CONDITIONAL, PROFILE_CONTROL_UNAVAILABLE, PROFILE_CONTROL_REFUSED };
enum ProfileControlIssueKind { PROFILE_CONTROL_MISSING, PROFILE_CONTROL_GRID, PROFILE_CONTROL_RANGE, PROFILE_CONTROL_ROUTE,
    PROFILE_CONTROL_POWER_CONVERSION, PROFILE_CONTROL_FAN_REVIEW, PROFILE_CONTROL_ARITHMETIC };
enum ProfileControlUnit { PROFILE_CONTROL_HZ, PROFILE_CONTROL_UV, PROFILE_CONTROL_MW, PROFILE_CONTROL_NO_UNIT };
struct ProfileControlIssue {
    gc_u32 domain;
    ProfileControlIssueKind kind;
    gc_i64 requested, minimum, maximum, grid;
    ProfileControlUnit unit;
};
struct ProfileControlPlan {
    ProfileControlFeasibility disposition = PROFILE_CONTROL_READY;
    bool curveChecked = false;
    CurvePlanResult curve{};
    int issueCount = 0, omittedIssues = 0;
    ProfileControlIssue issues[9]{};
};
static inline const char* profile_control_disposition_name(ProfileControlFeasibility d) {
    switch (d) {
        case PROFILE_CONTROL_READY: return "representable";
        case PROFILE_CONTROL_CONDITIONAL: return "conditional";
        case PROFILE_CONTROL_UNAVAILABLE: return "unavailable";
        case PROFILE_CONTROL_REFUSED: return "refused";
    }
    return "invalid";
}
static inline const char* profile_control_issue_name(ProfileControlIssueKind k) {
    switch (k) {
        case PROFILE_CONTROL_MISSING: return "required-control-unavailable";
        case PROFILE_CONTROL_GRID: return "explicit-grid-conversion-required";
        case PROFILE_CONTROL_RANGE: return "outside-control-range";
        case PROFILE_CONTROL_ROUTE: return "control-selection-unresolved";
        case PROFILE_CONTROL_POWER_CONVERSION: return "explicit-power-conversion-required";
        case PROFILE_CONTROL_FAN_REVIEW: return "fan-capability-review-required";
        case PROFILE_CONTROL_ARITHMETIC: return "arithmetic-overflow";
    }
    return "invalid";
}
static inline void profile_control_note(ProfileControlPlan& p, ProfileControlFeasibility disposition,
    gc_u32 domain, ProfileControlIssueKind kind, gc_i64 value = 0, gc_i64 lo = 0, gc_i64 hi = 0, gc_i64 grid = 0,
    ProfileControlUnit unit = PROFILE_CONTROL_HZ) {
    if (disposition > p.disposition) p.disposition = disposition;
    if (p.issueCount < 9) p.issues[p.issueCount++] = {domain, kind, value, lo, hi, grid, unit};
    else ++p.omittedIssues;
}
static inline void profile_control_scalar(ProfileControlPlan& p, gc_u32 domain,
    gc_i64 value, const HardwareCaptureScalar& s, gc_u32 surface = 0, ProfileControlUnit unit = PROFILE_CONTROL_HZ) {
    if (surface && surface != s.surface) {
        profile_control_note(p, PROFILE_CONTROL_UNAVAILABLE, domain, PROFILE_CONTROL_ROUTE, value, 0, 0, 0, unit); return;
    }
    if (!s.valid || !s.writable || s.grid <= 0) {
        profile_control_note(p, PROFILE_CONTROL_UNAVAILABLE, domain, PROFILE_CONTROL_MISSING, value, 0, 0, 0, unit); return;
    }
    if (value % s.grid) profile_control_note(p, PROFILE_CONTROL_REFUSED, domain, PROFILE_CONTROL_GRID, value, 0, 0, s.grid, unit);
    else if (s.rangeValid && (value < s.minimum || value > s.maximum))
        profile_control_note(p, PROFILE_CONTROL_REFUSED, domain, PROFILE_CONTROL_RANGE, value, s.minimum, s.maximum, 0, unit);
}

// Control feasibility is separate from operation authorization and transition
// feasibility. This pure preview does not possess outgoing ownership, rollback
// evidence or a transition-protection proof, and cannot approve an Apply.
static inline ProfileControlPlan profile_control_plan(const DesiredSettings& d, const HardwareCaptureObservation& o) {
    ProfileControlPlan p;
    if (!o.valid || !hardware_capture_observation_valid(o) ||
        !desired_authoring_valid(&d) || !desired_scalar_intent_valid(&d) || !desired_curve_intent_valid(&d)) {
        profile_control_note(p, PROFILE_CONTROL_UNAVAILABLE, 0, PROFILE_CONTROL_MISSING); return p;
    }
    const gc_u32 graphics = d.authoring.graphicsSurface;
    bool needsCurve = d.hasLock || (d.hasGpuOffset && graphics == DESIRED_GRAPHICS_VF_COMPONENT);
    bool hasProjection = false;
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        needsCurve |= desired_curve_point_present(&d, ci);
        hasProjection |= d.hasCurvePoint[ci] && d.curvePointFromGpuOffset[ci];
    }
    if (hasProjection && d.hasGpuOffset && graphics != DESIRED_GRAPHICS_VF_COMPONENT)
        profile_control_note(p, PROFILE_CONTROL_UNAVAILABLE, PROFILE_DOMAIN_GRAPHICS_OFFSET, PROFILE_CONTROL_ROUTE);
    if (needsCurve) {
        auto storage = std::make_unique<ProfileCaptureCurvePlan>();
        if (!profile_capture_curve_plan_input(o, d, *storage))
            profile_control_note(p, PROFILE_CONTROL_UNAVAILABLE, PROFILE_DOMAIN_VF_CURVE, PROFILE_CONTROL_MISSING);
        else {
            p.curveChecked = true; p.curve = curve_plan_evaluate(storage->input);
            ProfileControlFeasibility status = PROFILE_CONTROL_READY;
            if (p.curve.disposition == CURVE_PLAN_REFUSED) status = PROFILE_CONTROL_REFUSED;
            else if (p.curve.disposition == CURVE_PLAN_UNAVAILABLE) status = PROFILE_CONTROL_UNAVAILABLE;
            else if (p.curve.disposition == CURVE_PLAN_NEEDS_FRESH_BASELINE) status = PROFILE_CONTROL_CONDITIONAL;
            bool baselineDependent = d.hasLock;
            for (int ci = 0; ci < VF_NUM_POINTS; ++ci)
                baselineDependent |= d.hasCurvePoint[ci] && !d.curvePointFromGpuOffset[ci];
            if (status == PROFILE_CONTROL_READY && baselineDependent && !curve_plan_baseline_proves(p.curve.evidence))
                status = PROFILE_CONTROL_CONDITIONAL;
            if (status > p.disposition) p.disposition = status;
        }
    }
    if (d.hasGpuOffset && graphics == DESIRED_GRAPHICS_VF_COMPONENT && o.vfLimits.valid && o.vfLimits.gridValid) {
        bool writes = false;
        for (gc_u32 at = 0; at < o.pointCount; ++at) {
            const auto& point = o.points[at];
            const auto kind = desired_curve_point_kind(&d, (int)point.index);
            const bool tail = d.hasLock && d.lockMode != LOCK_MODE_NONE && (int)point.index >= d.lockCi;
            writes |= !tail && (kind == DESIRED_CURVE_ABSENT || kind == DESIRED_CURVE_PROJECTED) &&
                point.writableState == 1 && !(d.authoring.identityExclusions && d.authoring.graphicsExcluded[point.index]);
        }
        const gc_i64 hz = desired_graphics_hz(d);
        if (writes && hz % o.vfLimits.gridHz)
            profile_control_note(p, PROFILE_CONTROL_REFUSED, PROFILE_DOMAIN_GRAPHICS_OFFSET, PROFILE_CONTROL_GRID, hz, 0, 0, o.vfLimits.gridHz);
        else if (writes && (hz < o.vfLimits.minimumHz || hz > o.vfLimits.maximumHz))
            profile_control_note(p, PROFILE_CONTROL_REFUSED, PROFILE_DOMAIN_GRAPHICS_OFFSET, PROFILE_CONTROL_RANGE, hz, o.vfLimits.minimumHz, o.vfLimits.maximumHz);
    }
    if (d.hasGpuOffset && graphics != DESIRED_GRAPHICS_VF_COMPONENT) {
        if (!d.authoring.graphicsPrecise || (graphics != DESIRED_GRAPHICS_P0 && graphics != DESIRED_GRAPHICS_GLOBAL))
            profile_control_note(p, PROFILE_CONTROL_UNAVAILABLE, PROFILE_DOMAIN_GRAPHICS_OFFSET, PROFILE_CONTROL_ROUTE);
        else profile_control_scalar(p, PROFILE_DOMAIN_GRAPHICS_OFFSET, desired_graphics_hz(d), o.graphics, graphics);
    }
    if (d.hasMemOffset) profile_control_scalar(p, PROFILE_DOMAIN_MEMORY, desired_memory_hz(d), o.memory, d.memOffsetSurface);
    if (d.hasPowerLimit) {
        const auto& v = o.power;
        if (!v.currentValid || !v.defaultValid || !v.writable || v.gridMw <= 0 || v.currentMw > UINT_MAX ||
            v.defaultMw > UINT_MAX || v.minimumMw > UINT_MAX || v.maximumMw > UINT_MAX || v.gridMw > UINT_MAX)
            profile_control_note(p, PROFILE_CONTROL_UNAVAILABLE, PROFILE_DOMAIN_POWER, PROFILE_CONTROL_MISSING, 0, 0, 0, 0, PROFILE_CONTROL_MW);
        else {
            PowerRatioSurface s{};
            s.currentValid = v.currentValid; s.defaultValid = v.defaultValid; s.writable = v.writable; s.rangeKnown = v.rangeValid;
            s.currentMw = (unsigned int)v.currentMw; s.defaultMw = (unsigned int)v.defaultMw;
            s.minimumMw = (unsigned int)v.minimumMw; s.maximumMw = (unsigned int)v.maximumMw; s.gridMw = (unsigned int)v.gridMw;
            const auto power = power_ratio_control_plan(desired_power_ratio(d), s);
            if (power.status != POWER_RATIO_READY) {
                const bool refused = power.status == POWER_RATIO_CONVERSION_REQUIRED || power.status == POWER_RATIO_OUT_OF_RANGE || power.status == POWER_RATIO_INVALID;
                profile_control_note(p, refused ? PROFILE_CONTROL_REFUSED : PROFILE_CONTROL_UNAVAILABLE, PROFILE_DOMAIN_POWER,
                    power.status == POWER_RATIO_CONVERSION_REQUIRED ? PROFILE_CONTROL_POWER_CONVERSION :
                    power.status == POWER_RATIO_OUT_OF_RANGE ? PROFILE_CONTROL_RANGE :
                    power.status == POWER_RATIO_INVALID ? PROFILE_CONTROL_ARITHMETIC : PROFILE_CONTROL_MISSING,
                    tuning_power_ratio_resolve(desired_power_ratio(d), s.defaultMw).requestedMw,
                    v.minimumMw, v.maximumMw, v.gridMw, PROFILE_CONTROL_MW);
            }
        }
    }
    if (d.hasXbarOffsetKhz) profile_control_scalar(p, PROFILE_DOMAIN_XBAR, (gc_i64)d.xbarOffsetKhz * 1000, o.xbarClock);
    if (d.hasXbarMsvddOffsetUv) profile_control_scalar(p, PROFILE_DOMAIN_XBAR, d.xbarMsvddOffsetUv, o.xbarVoltage, 0, PROFILE_CONTROL_UV);
    if (d.hasSysClkOffsetKhz) profile_control_scalar(p, PROFILE_DOMAIN_SYS_CLK, (gc_i64)d.sysClkOffsetKhz * 1000, o.systemClock);
    if (d.hasVideoClkOffsetKhz) profile_control_scalar(p, PROFILE_DOMAIN_VIDEO_CLK, (gc_i64)d.videoClkOffsetKhz * 1000, o.videoClock);
    if (d.hasFan) profile_control_note(p, PROFILE_CONTROL_CONDITIONAL, PROFILE_DOMAIN_FAN, PROFILE_CONTROL_FAN_REVIEW, 0, 0, 0, 0, PROFILE_CONTROL_NO_UNIT);
    return p;
}
