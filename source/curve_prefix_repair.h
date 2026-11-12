// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_CURVE_PREFIX_REPAIR_H
#define GREEN_CURVE_CURVE_PREFIX_REPAIR_H

// "Prepare compatible curve", operation A: lower a prefix that conflicts with
// a lock, as an editor-draft change the user reviews.  Pure; it never writes
// hardware, and the caller applies an accepted result as ordinary typed
// points (contract 9: automatic restore replays that intent, never this
// algorithm).
//
// Rules (keep every one; the planner is the judge):
//   - The anchor and the plateau never move; no point is ever raised.
//   - Working backward from the anchor, each earlier point's new target is
//     min(its own target, its successor's new target): the smallest change
//     that leaves the prefix non-increasing toward lower voltage.  Points
//     already low enough keep their kind and value.
//   - Only absolute intent is changed here.  A point the request does not own
//     becomes a typed point at the lowered value (listed in the preview); a
//     projected (offset-intent) point that would have to change is reported
//     as needing conversion instead.  An unowned point's current target is
//     its stock base, so the repair needs a trustworthy baseline (an
//     unshaped curve); over a shaped estimate it reports that instead of
//     guessing.
//   - A point that cannot be lowered far enough (inside the flat floor, or
//     beyond the range minimum) refuses the whole repair with its identity,
//     ahead of any conversion or baseline finding.  Lowering only the first
//     offender and hoping is not offered.
//   - The repaired request is re-planned; when another proven constraint
//     (anchor or tail) still fails, the repair is not offered as a fix.

#include "curve_feasibility_plan.h"

enum CurvePrefixRepairStatus {
    CURVE_PREFIX_REPAIR_NOT_NEEDED = 0,
    CURVE_PREFIX_REPAIR_PREPARED = 1,
    CURVE_PREFIX_REPAIR_CONVERSION_REQUIRED = 2,
    CURVE_PREFIX_REPAIR_BASELINE_REQUIRED = 3,
    CURVE_PREFIX_REPAIR_LIMITED = 4,
    CURVE_PREFIX_REPAIR_UNRESOLVED = 5,
    CURVE_PREFIX_REPAIR_NO_LOCK = 6,
};

struct CurvePrefixRepairPoint {
    int ci;
    unsigned int voltUv;
    unsigned int oldMHz;
    unsigned int newMHz;
    bool wasAbsent;  // the request did not own it; it becomes a typed point
};

struct CurvePrefixRepair {
    CurvePrefixRepairStatus status;
    int changedCount;
    CurvePrefixRepairPoint points[VF_NUM_POINTS];
    int firstCi;               // the lowest-voltage changed (or offending) point
    unsigned int minVoltUv;
    unsigned int maxVoltUv;
    int limitingCi;            // LIMITED / CONVERSION / BASELINE: the point
    unsigned int limitingVoltUv;
    unsigned int limitingNeededMHz;  // the value it would have to come down to
    long long limitingKHz;     // LIMITED: the offset it would need
    bool limitingFloor;        // LIMITED: inside the driver's flat floor
    long long rangeMinKHz;     // LIMITED: the permitted minimum offset
    CurvePlanRangeSource rangeSource;
    int floorMHz;
    DesiredSettings repaired;  // PREPARED only; otherwise a copy of the input
    CurvePlanResult after;     // the planner over `repaired`
};

static inline const char* curve_prefix_repair_status_name(CurvePrefixRepairStatus s) {
    switch (s) {
        case CURVE_PREFIX_REPAIR_NOT_NEEDED: return "not-needed";
        case CURVE_PREFIX_REPAIR_PREPARED: return "prepared";
        case CURVE_PREFIX_REPAIR_CONVERSION_REQUIRED: return "conversion-required";
        case CURVE_PREFIX_REPAIR_BASELINE_REQUIRED: return "baseline-required";
        case CURVE_PREFIX_REPAIR_LIMITED: return "limited";
        case CURVE_PREFIX_REPAIR_UNRESOLVED: return "unresolved";
        case CURVE_PREFIX_REPAIR_NO_LOCK: return "no-lock";
        default: return "unknown";
    }
}

static inline CurvePrefixRepair curve_prefix_repair_prepare(const CurvePlanInput& in) {
    CurvePrefixRepair out = {};
    out.firstCi = -1;
    out.limitingCi = -1;
    const DesiredSettings* d = in.desired;
    if (d) out.repaired = *d;
    if (!d || !in.curve || !in.programmedOffsetsKHz || !in.floor.valid ||
        !d->hasLock || d->lockMHz == 0 || in.lockMode == LOCK_MODE_NONE ||
        vf_point_target_status(in.floor, in.curve, d->lockCi) != VF_POINT_TARGET_OK) {
        out.status = CURVE_PREFIX_REPAIR_NO_LOCK;
        return out;
    }
    const int a = d->lockCi;
    const unsigned int plateau = d->lockMHz;
    const bool baselineProves = curve_plan_baseline_proves(in.evidence);
    // Backward pass from the anchor.
    unsigned int bound = plateau;
    CurvePrefixRepairStatus problem = CURVE_PREFIX_REPAIR_PREPARED;
    for (int ci = a - 1; ci >= 0; --ci) {
        if (ci >= in.floor.graphicsEnd || in.curve[ci].freq_kHz == 0) continue;
        const long long baseKHz = curve_point_stock_base_khz(in.curve[ci].freq_kHz,
                                                             in.programmedOffsetsKHz[ci]);
        const long long componentKHz = in.gpuComponentKHz ? in.gpuComponentKHz[ci] : 0;
        // The planner's own value for this point (curve_plan_prefix_point_target).
        const CurvePlanPrefixTarget t =
            curve_plan_prefix_point_target(in, baseKHz, ci, baselineProves);
        const bool requested = t.requested;
        // Lowering a point that follows the GPU offset (projected, or unowned
        // but shifted by the offset) would turn it into a typed value.
        const bool projected = t.kind == CURVE_PLAN_POINT_PROJECTED ||
            (!requested && d->hasGpuOffset && componentKHz != 0);
        const unsigned int target = t.targetMHz;
        if (target <= bound) {
            bound = target;
            continue;
        }
        // This point must come down to `bound`.
        CurvePrefixRepairPoint& p = out.points[out.changedCount++];
        p.ci = ci;
        p.voltUv = in.curve[ci].volt_uV;
        p.oldMHz = target;
        p.newMHz = bound;
        p.wasAbsent = !requested;
        out.firstCi = ci;
        if (out.minVoltUv == 0 || p.voltUv < out.minVoltUv) out.minVoltUv = p.voltUv;
        if (p.voltUv > out.maxVoltUv) out.maxVoltUv = p.voltUv;
        // A point that cannot be lowered at all outranks one that only needs a
        // decision (conversion, baseline): that decision would not help.
        if (problem == CURVE_PREFIX_REPAIR_LIMITED) continue;
        const long long neededKHz = (long long)bound * 1000LL - baseKHz;
        const bool inFloor = vf_point_in_flat_floor(in.floor, in.curve, ci);
        CurvePrefixRepairStatus pointProblem = CURVE_PREFIX_REPAIR_PREPARED;
        if (inFloor || !vf_point_listed(in.floor, in.curve, ci) ||
            neededKHz < in.range.minKHz) {
            pointProblem = CURVE_PREFIX_REPAIR_LIMITED;
        } else if (projected) {
            pointProblem = CURVE_PREFIX_REPAIR_CONVERSION_REQUIRED;
        } else if (!requested && !baselineProves) {
            pointProblem = CURVE_PREFIX_REPAIR_BASELINE_REQUIRED;
        }
        if (pointProblem == CURVE_PREFIX_REPAIR_PREPARED) continue;
        if (problem != CURVE_PREFIX_REPAIR_PREPARED &&
            pointProblem != CURVE_PREFIX_REPAIR_LIMITED) continue;
        problem = pointProblem;
        out.limitingCi = ci;
        out.limitingVoltUv = in.curve[ci].volt_uV;
        out.limitingNeededMHz = bound;
        out.limitingKHz = neededKHz;
        out.limitingFloor = inFloor;
    }
    out.rangeMinKHz = in.range.minKHz;
    out.rangeSource = in.range.source;
    out.floorMHz = in.floor.floorMHz;
    if (out.changedCount == 0) {
        out.status = CURVE_PREFIX_REPAIR_NOT_NEEDED;
        return out;
    }
    if (problem != CURVE_PREFIX_REPAIR_PREPARED) {
        out.status = problem;
        return out;
    }
    for (int k = 0; k < out.changedCount; ++k) {
        const CurvePrefixRepairPoint& p = out.points[k];
        out.repaired.hasCurvePoint[p.ci] = true;
        out.repaired.curvePointMHz[p.ci] = p.newMHz;
        out.repaired.curvePointFromGpuOffset[p.ci] = false;
    }
    CurvePlanInput after = in;
    after.desired = &out.repaired;
    out.after = curve_plan_evaluate(after);
    if (out.after.disposition == CURVE_PLAN_REFUSED) {
        out.status = CURVE_PREFIX_REPAIR_UNRESOLVED;
        out.repaired = *d;
        return out;
    }
    out.status = CURVE_PREFIX_REPAIR_PREPARED;
    return out;
}

#endif  // GREEN_CURVE_CURVE_PREFIX_REPAIR_H
