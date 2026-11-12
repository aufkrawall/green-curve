// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_GPU_BACKEND_APPLY_LOCK_PRETAIL_H
#define GREEN_CURVE_GPU_BACKEND_APPLY_LOCK_PRETAIL_H

// The Windows apply's two uses of the shared curve feasibility planner
// (curve_feasibility_plan.h): once before the transition clamp or the
// reset-to-stock write anything, once over the apply's own post-reset (or,
// without a reset, pre-first-write) readback.  Included by
// gpu_backend_apply.cpp only.
//
// The planner replaced the bare pre-tail check that used to live here.  That
// check answered one question (is some pre-tail point above the lock?) and
// reported the first offender as "lower the preceding point"; anchor and tail
// range failures surfaced later as a generic target error after the reset had
// already run.  The pre-tail rules themselves (apply_lock_pretail_policy.h's
// provenance modes) are unchanged and evaluated by the planner.

#include "apply_lock_pretail_policy.h"
#include "curve_feasibility_message.h"

static void apply_curve_plan_log_line(const char* line) {
    debug_log("%s", line ? line : "");
}

static CurvePlanRange apply_curve_plan_range() {
    VfOffsetRangeSource source = VF_OFFSET_RANGE_SOURCE_FALLBACK;
    const VfOffsetRange range = vf_offset_range_current_with_source(&source);
    CurvePlanRange out = {};
    out.minKHz = range.minKHz;
    out.maxKHz = range.maxKHz;
    out.source = source == VF_OFFSET_RANGE_SOURCE_NVML_GRAPHICS
        ? CURVE_PLAN_RANGE_NVML_GRAPHICS
        : source == VF_OFFSET_RANGE_SOURCE_PSTATE ? CURVE_PLAN_RANGE_PSTATE
                                                  : CURVE_PLAN_RANGE_POLICY_FALLBACK;
    return out;
}

// The mode the apply itself resolves for a lock (gpu_backend_apply.cpp).
static LockMode apply_curve_plan_lock_mode(const DesiredSettings* desired) {
    if (desired->lockMode != LOCK_MODE_NONE) return desired->lockMode;
    if (g_app.lockMode != LOCK_MODE_NONE) return g_app.lockMode;
    return LOCK_MODE_FLATTEN;
}

static void apply_curve_plan_components(const DesiredSettings* desired,
                                        int excludeLow, int* componentKHz) {
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        componentKHz[ci] = desired->hasGpuOffset
            ? gpu_offset_component_mhz_for_point(ci, desired->gpuOffsetMHz, excludeLow) * 1000
            : 0;
    }
}

static bool apply_curve_plan_has_curve_work(const DesiredSettings* desired) {
    if (!desired) return false;
    if (desired->hasLock && desired->lockCi >= 0 && desired->lockMHz > 0) return true;
    for (int ci = 0; ci < VF_NUM_POINTS; ci++)
        if (desired->hasCurvePoint[ci] && desired->curvePointMHz[ci] > 0) return true;
    return false;
}

// The planner input for one request over one observation.  `componentKHz`
// (VF_NUM_POINTS entries) is caller storage the input points into, so it must
// outlive every use of the returned input.
static CurvePlanInput apply_curve_plan_input(const DesiredSettings* desired,
    LockMode lockMode, const VFCurvePoint* curve, const int* offsetsKHz,
    CurvePlanEvidence evidence, ApplyLockPretailProvenance provenance,
    bool unownedAtStock, int excludeLow, int* componentKHz) {
    apply_curve_plan_components(desired, excludeLow, componentKHz);
    CurvePlanInput in = {};
    in.curve = curve;
    in.programmedOffsetsKHz = offsetsKHz;
    in.floor = g_app.vfFloorBinding.floor;
    in.evidence = evidence;
    in.range = apply_curve_plan_range();
    in.tailStrategy = lockMode == LOCK_MODE_HARD ? CURVE_PLAN_TAIL_ANCHOR_ONLY
                                                 : CURVE_PLAN_TAIL_UNIFORM_MIN_OFFSET;
    in.provenanceMode = provenance;
    in.unownedAtStock = unownedAtStock;
    in.prefixAppliesToHardLock = true;
    in.desired = desired;
    in.lockMode = lockMode;
    in.gpuComponentKHz = componentKHz;
    return in;
}

static CurvePlanResult apply_curve_plan_run(const DesiredSettings* desired,
    LockMode lockMode, const VFCurvePoint* curve, const int* offsetsKHz,
    CurvePlanEvidence evidence, ApplyLockPretailProvenance provenance,
    bool unownedAtStock, int excludeLow, const char* phase) {
    int componentKHz[VF_NUM_POINTS] = {};
    const CurvePlanInput in = apply_curve_plan_input(desired, lockMode, curve,
        offsetsKHz, evidence, provenance, unownedAtStock, excludeLow, componentKHz);
    CurvePlanResult plan = curve_plan_evaluate(in);
    curve_plan_log(plan, phase, apply_curve_plan_log_line);
    return plan;
}

// CT-02 preflight: a request that EXPLICITLY names a lock point must name a
// writable graphics point -- populated, in the graphics domain, and, below
// 700 mV, outside the driver's flat floor (the rule the editor lists by).
// Returns false with `result` set when it does not.
static bool apply_curve_plan_anchor_is_writable(const DesiredSettings* desired,
    char* result, size_t resultSize) {
    if (!desired->hasLock || desired->lockCi < 0 || desired->lockMHz == 0) return true;
    const int ci = desired->lockCi;
    const VfPointTargetStatus status =
        vf_point_target_status(g_app.vfFloorBinding.floor, g_app.curve, ci);
    if (status == VF_POINT_TARGET_OK) return true;
    const unsigned int mv = ci < VF_NUM_POINTS ? g_app.curve[ci].volt_uV / 1000u : 0u;
    if (status == VF_POINT_TARGET_FLOOR) {
        set_message(result, resultSize,
            "The requested %s lock point %d (%u mV) is inside the driver's flat"
            " low-voltage floor (%d MHz); writes there are clamped back to the"
            " floor. Nothing was changed. Re-pick the lock point on the curve.",
            lock_mode_name(desired->lockMode), ci, mv,
            g_app.vfFloorBinding.floor.floorMHz);
    } else {
        set_message(result, resultSize,
            "The requested %s lock point (curve index %d) is not available on this"
            " GPU, so the profile cannot be applied as requested. Nothing was"
            " changed. Re-pick the lock point on the curve.",
            lock_mode_name(desired->lockMode), ci);
    }
    debug_log("apply_desired_settings: REFUSED before any write -- requested"
              " lock ci=%d mode=%s lockMHz=%u status=%s (numVisible=%d"
              " floorEnd=%d); refusing instead of silently applying an"
              " unlocked profile\n",
        ci, lock_mode_name(desired->lockMode), desired->lockMHz,
        vf_point_target_status_name(status), g_app.numVisible,
        g_app.vfFloorBinding.floor.floorEnd);
    set_last_apply_phase("apply: refused (lock point not writable)");
    return false;
}

// How a reset-first request is judged before any write, over the curve and
// programmed offsets currently held in g_app (no hardware read here): the
// GPU-offset route, the pre-tail provenance rule and the evidence class.  The
// service's precheck refreshes g_app first; any other pre-write judgement of a
// request must build its input here too, so it cannot drift from Apply's.
struct ApplyCurvePlanPreWriteRoute {
    int excludeLow;
    bool offsetInVfTable;
    ApplyLockPretailProvenance provenance;
    CurvePlanEvidence evidence;
};

static ApplyCurvePlanPreWriteRoute apply_curve_plan_pre_write_route(
    const DesiredSettings* desired) {
    ApplyCurvePlanPreWriteRoute route = {};
    const int excludeLow = (desired->hasGpuOffset &&
        desired->gpuOffsetExcludeLowCount > 0 && desired->gpuOffsetMHz != 0)
        ? desired->gpuOffsetExcludeLowCount : 0;
    // A reset-before-apply clears VF curve offsets, but it only clears the
    // separate NVML graphics scalar when the request itself owns the GPU-offset
    // domain.  A sparse lock/curve request therefore retains a uniform scalar,
    // and the lock projection must keep using the separate-NVML route.
    int currentAppliedMHz = 0;
    int currentActiveExclude = 0;
    if (!desired->hasGpuOffset) {
        currentAppliedMHz = current_applied_gpu_offset_mhz();
        currentActiveExclude = current_applied_gpu_offset_excludes_low_points()
            ? g_app.appliedGpuOffsetExcludeLowCount : 0;
    }
    const bool offsetInVfTable = curve_point_apply_uses_vf_table_offset(
        vf_curve_global_gpu_offset_supported(), desired->hasGpuOffset != 0,
        desired->gpuOffsetMHz, excludeLow, currentAppliedMHz, currentActiveExclude);
    route.excludeLow = excludeLow;
    route.offsetInVfTable = offsetInVfTable;
    // The base here may still carry the outgoing profile's shape, so a
    // provenance point is refused only when its stored MHz agrees.
    route.provenance = apply_lock_pretail_provenance_mode(offsetInVfTable, false);
    // An unshaped curve (every offset zero) IS the stock curve; anything else
    // is an estimate whose baseline-derived failures need the reset first.
    route.evidence = curve_plan_classify_pre_write_evidence(
        g_app.curve, g_app.freqOffsets, g_app.vfFloorBinding.floor,
        g_app.gpuClockOffsetkHz == 0);
    return route;
}

// The planner input a reset-first request gets before any write, over the
// curve in g_app.  `componentKHz` is caller storage (see apply_curve_plan_input).
static CurvePlanInput apply_curve_plan_pre_write_input(const DesiredSettings* desired,
    const ApplyCurvePlanPreWriteRoute& route, int* componentKHz) {
    return apply_curve_plan_input(desired,
        apply_curve_plan_lock_mode(desired), g_app.curve, g_app.freqOffsets,
        route.evidence, route.provenance, true, route.excludeLow, componentKHz);
}

// The pre-write half, for requests that reset to stock first.  Runs BEFORE the
// transition clamp or the reset touch anything, on a fresh curve read.
// Returns false with `result` set when the planner PROVES a refusal; true
// otherwise -- including an estimate that needs the post-reset baseline (the
// backstop decides that one) and a curve that cannot be read here.
static bool apply_curve_plan_precheck_before_writes(const DesiredSettings* desired,
    char* result, size_t resultSize) {
    if (!desired || !desired->resetOcBeforeApply) return true;
    if (!apply_curve_plan_has_curve_work(desired)) return true;
    if (desired->hasGpuOffset && g_app.gpuOffsetRangeKnown &&
        (desired->gpuOffsetMHz < g_app.gpuClockOffsetMinMHz ||
         desired->gpuOffsetMHz > g_app.gpuClockOffsetMaxMHz)) {
        return true;  // reported by the apply's own range check
    }
    // Frequencies and programmed offsets from the same moment: a base built
    // from a fresh frequency read and a cached offset table is neither stock
    // nor live.
    if (!nvapi_read_curve() || !nvapi_read_offsets()) {
        debug_log("curve-plan: phase=pre-write curve or offset read failed; deferring"
                  " to the post-reset backstop\n");
        return true;
    }
    const ApplyCurvePlanPreWriteRoute route = apply_curve_plan_pre_write_route(desired);
    int componentKHz[VF_NUM_POINTS] = {};
    const CurvePlanResult plan = curve_plan_evaluate(
        apply_curve_plan_pre_write_input(desired, route, componentKHz));
    curve_plan_log(plan, "pre-write", apply_curve_plan_log_line);
    if (plan.disposition != CURVE_PLAN_REFUSED) return true;
    curve_plan_format_message(plan, true, result, resultSize);
    debug_log("curve-plan: REFUSED before any write -- lockCi=%d lockMHz=%u mode=%s"
              " gpuOffset=%d exclude=%d provenance=%s vfTableOffset=%d\n",
        desired->lockCi, desired->lockMHz, lock_mode_name(desired->lockMode),
        desired->hasGpuOffset ? desired->gpuOffsetMHz : 0, route.excludeLow,
        apply_lock_pretail_provenance_name(route.provenance),
        route.offsetInVfTable ? 1 : 0);
    return false;
}

// The backstop over the apply's own originals: the post-reset readback when
// the request reset to stock (a verified transaction baseline), otherwise the
// pre-first-write readback.  `resolved` carries the lock the apply resolved
// (an interactive stored lock included).
static CurvePlanResult apply_curve_plan_backstop(const DesiredSettings* resolved,
    LockMode lockMode, const bool* originalCurvePopulated,
    const int* originalCurveFreqkHz, const int* originalCurveOffsets,
    const unsigned int* originalCurveVoltUv, ApplyLockPretailProvenance provenance,
    int excludeLow) {
    VFCurvePoint curve[VF_NUM_POINTS] = {};
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (!originalCurvePopulated[ci]) continue;
        curve[ci].freq_kHz = (unsigned int)originalCurveFreqkHz[ci];
        curve[ci].volt_uV = originalCurveVoltUv[ci];
    }
    const CurvePlanEvidence evidence = resolved->resetOcBeforeApply
        ? CURVE_PLAN_EVIDENCE_TRANSACTION_BASELINE
        : curve_plan_classify_pre_write_evidence(curve, originalCurveOffsets,
                                                 g_app.vfFloorBinding.floor,
                                                 g_app.gpuClockOffsetkHz == 0);
    return apply_curve_plan_run(resolved, lockMode, curve, originalCurveOffsets,
        evidence, provenance, resolved->resetOcBeforeApply != 0, excludeLow,
        resolved->resetOcBeforeApply ? "post-reset" : "pre-first-write");
}

#endif
