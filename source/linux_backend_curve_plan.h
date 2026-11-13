// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The Linux apply's use of the shared curve feasibility planner
// (curve_feasibility_plan.h).  Included by linux_backend_mutation.cpp after
// the curve-target builders, so the plan judges exactly the targets the
// transaction would write, before its first setter.
//
// Linux had no pre-tail or anchor preflight at all: a flatten under a higher
// pre-tail point was written and left non-flat, and an anchor beyond the range
// was clamped silently on the way to the driver.  A PROVEN refusal now
// returns before any phase runs.  An estimate over a shaped curve is logged
// and the transaction proceeds as before (post-write curve verification on
// Linux is separate work).
#ifndef GREEN_CURVE_LINUX_BACKEND_CURVE_PLAN_H
#define GREEN_CURVE_LINUX_BACKEND_CURVE_PLAN_H

#include "curve_feasibility_message.h"

static void linux_curve_plan_log_line(const char* line) {
    lb_log("%s", line ? line : "");
}

static CurvePlanRange linux_curve_plan_range(LinuxGpuState* g) {
    int mn = 0, mx = 0;
    curve_offset_range_khz(g, &mn, &mx);
    CurvePlanRange range = {};
    range.minKHz = mn;
    range.maxKHz = mx;
    // Both reported sources are NVML's graphics bounds (curve_offset_range_khz()).
    const bool curveKnown = g->curveOffsetRangeKnown &&
        g->curveOffsetMinKHz <= g->curveOffsetMaxKHz;
    const bool gpuKnown = g->gpuOffsetMinMHz <= g->gpuOffsetMaxMHz &&
        (g->gpuOffsetMinMHz || g->gpuOffsetMaxMHz);
    range.source = (curveKnown || gpuKnown) ? CURVE_PLAN_RANGE_NVML_GRAPHICS
                                            : CURVE_PLAN_RANGE_POLICY_FALLBACK;
    return range;
}

// `planned` is the request the curve targets were built from (the committed
// intent for a replacement), `targets`/`mask` the transaction's own build.
static CurvePlanResult linux_backend_curve_plan(LinuxGpuState* g,
    const DesiredSettings* planned, const LinuxHardwareSnapshot* snapshot,
    const int* targets, const bool* mask) {
    if (vf_floor_binding_update(&g->vfFloor, g->curve, g->freqOffsets,
                                VF_NUM_POINTS, 0)) {
        const VfFloorTopology& floor = g->vfFloor.floor;
        lb_log("vf floor: bound topology key=%08x graphicsEnd=%d first=ci%d floorEnd=ci%d"
               " floorRun=%d floorMHz=%d\n", g->vfFloor.topologyKey, floor.graphicsEnd,
               floor.firstPopulated, floor.floorEnd, floor.floorRun, floor.floorMHz);
    }
    int componentKHz[VF_NUM_POINTS] = {};
    gc_i64 componentHz[VF_NUM_POINTS] = {};
    bool stockMask[VF_NUM_POINTS] = {};
    int exclude = planned->gpuOffsetExcludeLowCount;
    if (exclude < 0) exclude = 0;
    int ordinal = 0;
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        if (g->curve[ci].freq_kHz == 0) continue;
        if (planned->hasGpuOffset && ordinal >= exclude)
            componentKHz[ci] = planned->gpuOffsetMHz * 1000;
        const bool excluded = planned->authoring.identityExclusions
            ? planned->authoring.graphicsExcluded[ci] != 0 : ordinal < exclude;
        if (planned->hasGpuOffset && !excluded) componentHz[ci] = desired_graphics_hz(*planned);
        ++ordinal;
        // A point the request does not own keeps its live offset, unless the
        // transaction releases it with a zero offset (replacement cleanup).
        stockMask[ci] = mask && targets && mask[ci] && targets[ci] == 0;
    }
    const bool scalarKnownZero = snapshot && snapshot->gpuOffsetValid &&
        snapshot->gpuOffsetMHz == 0;
    CurvePlanInput in = {};
    in.curve = g->curve;
    in.programmedOffsetsKHz = g->freqOffsets;
    in.floor = g->vfFloor.floor;
    in.evidence = curve_plan_classify_pre_write_evidence(g->curve, g->freqOffsets,
        in.floor, scalarKnownZero, g->vfOffsetsHzValid ? g->freqOffsetsHz : nullptr);
    in.range = linux_curve_plan_range(g);
    in.tailStrategy = planned->lockMode == LOCK_MODE_HARD
        ? CURVE_PLAN_TAIL_ABSOLUTE_TARGET : CURVE_PLAN_TAIL_UNIFORM_MIN_OFFSET;
    // Linux offset points always hold their component in the VF table; the
    // base is stock only when nothing is programmed.
    in.provenanceMode = apply_lock_pretail_provenance_mode(true,
        in.evidence == CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE);
    in.unownedAtStock = false;
    in.unownedStockMask = stockMask;
    in.prefixAppliesToHardLock = false;
    in.desired = planned;
    in.lockMode = planned->lockMode;
    in.gpuComponentKHz = componentKHz;
    in.gpuComponentHz = componentHz;
    if (g->vfFrequenciesHzValid && g->vfOffsetsHzValid) {
        in.frequencyHz = g->curveFrequencyHz; in.programmedOffsetsHz = g->freqOffsetsHz;
    }
    in.lockGridHz = 1000000;
    in.offsetGridHz = vf_graphics_offset_grid_hz(g->backend);
    CurvePlanResult plan = curve_plan_evaluate(in);
    curve_plan_log(plan, "pre-write", linux_curve_plan_log_line);
    return plan;
}

#endif  // GREEN_CURVE_LINUX_BACKEND_CURVE_PLAN_H
