// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_CURVE_POINT_OFFSET_POLICY_H
#define GREEN_CURVE_CURVE_POINT_OFFSET_POLICY_H

// THE one answer to "what offset does this VF point want?".
//
// An offset is the only thing this program writes. The driver owns the stock
// base and moves it with load and temperature, so a point's absolute MHz is
// never a controllable quantity -- it is base + offset, and base is whatever
// the driver felt like reporting at the moment it was sampled.
//
// That rule was previously re-derived at four sites: the initial target build,
// the correction loop, the Linux target build, and verification. They drifted,
// and the drift was the 2026-09-17 under-load failures. The last one is the
// clearest: a profile-1 apply wrote point 74 correctly at offset 0, the TAIL
// then missed by one VF bin because the base had moved, that sent the apply
// into the correction loop -- and the correction loop, which still derived
// `absolute - live base` on its own, overwrote point 74 with +495000 kHz and
// failed the apply on a value it had just invented. Fixing the build site
// alone could never have caught that; only one shared answer can.
//
// So: any call site that needs a point's offset calls this. Verification asks
// the same question by comparing against the offset this produced.

struct CurvePointOffsetRequest {
    // Provenance: this point's intent IS an offset. Its absolute MHz is a
    // projection over a base sampled at some earlier moment (profile save, an
    // editor refresh, a previous apply) and must not be reconstructed from a
    // base sampled now -- that is what turns the asked-for offset into a
    // different one every time the driver re-reports the base.
    bool fromGpuOffset;
    // This request's own GPU-offset component for the point, already resolved
    // against the exclude-low count. Zero for a profile carrying no GPU offset,
    // which is exactly what a point saved with `pointN_offset_khz=0` asks for.
    int gpuOffsetComponentKHz;
    // The absolute target, honoured only for a point the user really typed.
    unsigned int absoluteMHz;
    // The stock base as read right now. Consulted ONLY for a genuine absolute,
    // because only there is the caller actually asking to hit a frequency.
    long long liveBaseKHz;
};

// Which question a NON-tail point answers: "hold this offset" or "hit this
// absolute MHz". Only a point the user typed asks the second one. A point the
// selective GPU-offset batch synthesized (it is not in the request at all, the
// apply projects its MHz over the base sampled after reset) or a point the
// request marks offset-derived asks the first. Verification and the correction
// loop must ask this identically: when they did not, a cold-boot base that
// moved by one VF bin sent the correction chasing the projected MHz with
// positive offsets on excluded points, which verification then refused as
// "above requested" (2026-10-17 logon apply).
static inline bool curve_point_offset_is_intent(bool explicitPoint, bool fromGpuOffset,
                                                bool selectiveBatch) {
    return !explicitPoint && (selectiveBatch || fromGpuOffset);
}

// How far (MHz) a landed point falls short of its request, for the post-apply
// SHORTFALL diagnostic. An offset-intent point is short only by the offset it
// is missing; its MHz preview was projected over a base the driver has since
// re-reported (on a cold boot by a whole VF bin or more), so judging it by MHz
// raised a false alarm on points that held their offset exactly.
static inline unsigned int curve_point_shortfall_mhz(bool offsetIntent, int targetOffsetKHz,
                                                     int programmedOffsetKHz,
                                                     unsigned int targetMHz,
                                                     unsigned int actualMHz) {
    if (offsetIntent) {
        long long shortKHz = (long long)targetOffsetKHz - (long long)programmedOffsetKHz;
        return shortKHz > 0 ? (unsigned int)(shortKHz / 1000) : 0u;
    }
    return targetMHz > actualMHz ? targetMHz - actualMHz : 0u;
}

static inline long long curve_point_target_offset_khz(const CurvePointOffsetRequest* r) {
    if (!r) return 0;
    // Preserve the established per-point component contract. Composition with
    // a separate NVML graphics offset has not been measured on older GPUs;
    // do not compensate for it based on an assumed additive driver model.
    if (r->fromGpuOffset)
        return (long long)r->gpuOffsetComponentKHz;
    long long base = r->liveBaseKHz < 0 ? 0 : r->liveBaseKHz;
    return (long long)r->absoluteMHz * 1000 - base;
}

// Convenience for the common shape: caller has the live frequency and the
// offset currently programmed into the point, and wants the stock base implied
// by them.
static inline long long curve_point_stock_base_khz(long long liveFreqKHz,
                                                   long long programmedOffsetKHz) {
    long long base = liveFreqKHz - programmedOffsetKHz;
    return base < 0 ? 0 : base;
}

// The MHz an offset-intent point lands at over the base the driver reports
// NOW: live minus the programmed VF offset plus the request's component.
// Uses the same base derivation as typed points and the lock tail, without
// inferring how a separate NVML graphics offset affects the readback.
// Used where an absolute is needed for such a point
// (a lock anchor, a pre-tail neighbour) instead of its stored MHz, which is a
// projection over a base sampled earlier.
static inline long long curve_point_offset_intent_live_khz(long long liveFreqKHz,
                                                           long long programmedOffsetKHz,
                                                           long long gpuOffsetComponentKHz) {
    return curve_point_stock_base_khz(liveFreqKHz, programmedOffsetKHz) +
        gpuOffsetComponentKHz;
}

// The GPU-offset range check used before an apply may touch hardware.  An
// unknown range is permissive because it carries no evidence of an invalid
// request; a known range is inclusive at both endpoints, matching the write
// validation below.
static inline bool curve_point_gpu_offset_request_in_range(
    bool hasGpuOffset, int gpuOffsetMHz, bool rangeKnown,
    int rangeMinMHz, int rangeMaxMHz) {
    if (!hasGpuOffset || !rangeKnown) return true;
    return gpuOffsetMHz >= rangeMinMHz && gpuOffsetMHz <= rangeMaxMHz;
}

// Which route will OWN the GPU offset once this apply reaches its curve
// writes.  Global-VF backends (Blackwell) always use the VF table.  On older
// families a uniform offset is a separate NVML scalar; a selective offset is
// expressed through the VF table.  A sparse request that does not name the GPU
// offset preserves whichever route is already active.
//
// `currentAppliedGpuOffsetMHz` and `currentActiveExcludeLowCount` are the
// retained live state.  They matter only when `requestHasGpuOffset` is false:
// a reset-before-apply does not clear a uniform NVML scalar unless the request
// itself owns the GPU-offset domain.
static inline bool curve_point_apply_uses_vf_table_offset(
    bool globalVfOffsetSupported, bool requestHasGpuOffset,
    int desiredGpuOffsetMHz, int desiredExcludeLowCount,
    int currentAppliedGpuOffsetMHz, int currentActiveExcludeLowCount) {
    if (globalVfOffsetSupported) return true;
    if (requestHasGpuOffset) {
        if (desiredGpuOffsetMHz == 0) return true;
        return desiredExcludeLowCount > 0;
    }
    if (currentAppliedGpuOffsetMHz != 0 &&
        currentActiveExcludeLowCount <= 0) return false;
    return true;
}

struct CurveOffsetRoutingPlan {
    int activeDesiredExcludeLowCount;
    bool policyChangeRequested;
    bool shouldApplyGpuOffset;
    bool gpuPolicyViaCurveBatch;
    bool zeroPriorScalarBeforeCurveBatch;
    int curveBatchGpuOffsetMHz;
    bool lastApplyUsedGpuOffset;
};

static inline CurveOffsetRoutingPlan curve_point_plan_gpu_offset_routing(
    bool globalVfOffsetSupported, bool requestHasGpuOffset,
    int desiredGpuOffsetMHz, int desiredExcludeLowCount,
    int currentAppliedGpuOffsetMHz, int currentActiveExcludeLowCount) {
    CurveOffsetRoutingPlan p = {};
    p.lastApplyUsedGpuOffset = requestHasGpuOffset
        ? (desiredGpuOffsetMHz != 0)
        : (currentAppliedGpuOffsetMHz != 0);
    if (!requestHasGpuOffset) return p;
    p.activeDesiredExcludeLowCount =
        (desiredExcludeLowCount > 0 && desiredGpuOffsetMHz != 0)
            ? desiredExcludeLowCount
            : 0;
    p.policyChangeRequested =
        (desiredGpuOffsetMHz != currentAppliedGpuOffsetMHz) ||
        (p.activeDesiredExcludeLowCount != currentActiveExcludeLowCount);
    const bool leavingSelectiveOnSplitBackend =
        !globalVfOffsetSupported && currentActiveExcludeLowCount > 0 &&
        p.activeDesiredExcludeLowCount <= 0;
    p.shouldApplyGpuOffset = p.policyChangeRequested &&
        p.activeDesiredExcludeLowCount <= 0 &&
        (currentActiveExcludeLowCount <= 0 ||
         (leavingSelectiveOnSplitBackend && desiredGpuOffsetMHz != 0));
    p.gpuPolicyViaCurveBatch = p.policyChangeRequested &&
        (currentActiveExcludeLowCount > 0 || p.activeDesiredExcludeLowCount > 0);
    p.zeroPriorScalarBeforeCurveBatch = p.gpuPolicyViaCurveBatch &&
        currentAppliedGpuOffsetMHz != 0 &&
        (globalVfOffsetSupported || currentActiveExcludeLowCount <= 0);
    p.curveBatchGpuOffsetMHz =
        (!globalVfOffsetSupported && p.activeDesiredExcludeLowCount <= 0)
            ? 0
            : desiredGpuOffsetMHz;
    return p;
}

// The anchor of an interactive stored lock when the request names no lock MHz.
// A typed anchor point: its MHz. On the VF-table route an anchor with offset
// provenance follows today's base. Keep the stored MHz on the separate NVML
// route until its interaction with VF offsets is measured. No request point:
// the live MHz, as before.
static inline long long curve_point_stored_lock_anchor_khz(bool requested, bool fromGpuOffset,
                                                           unsigned int requestedMHz,
                                                           long long liveFreqKHz,
                                                           long long programmedOffsetKHz,
                                                           long long gpuOffsetComponentKHz,
                                                           bool offsetInVfTable) {
    if (requested && (!fromGpuOffset || !offsetInVfTable))
        return (long long)requestedMHz * 1000;
    if (requested)
        return curve_point_offset_intent_live_khz(liveFreqKHz, programmedOffsetKHz,
                                                  gpuOffsetComponentKHz);
    return liveFreqKHz < 0 ? 0 : liveFreqKHz;
}

#endif
