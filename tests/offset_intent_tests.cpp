// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// "Offset intent vs absolute MHz" follow-ups to the 2026-10-17 correction-loop
// fix (assertion codes 6800-6829, 6852-6861).  Pure headers only; no GPU, no OS state.
//
// These tests exercise requested offsets and supplied readbacks. They do not
// model composition between NVML graphics offsets and VF deltas, which needs
// measured hardware evidence before changing the established contract.

#include <climits>
#include <cstring>
#include <initializer_list>

#include "apply_correction_policy.h"
#include "apply_lock_pretail_policy.h"

namespace {

constexpr int N = 12;

struct Request {
    bool tail[N];
    bool explicitMask[N];
    bool origPopulated[N];
    unsigned char hasPoint[N];
    unsigned int pointMHz[N];
    unsigned char fromGpuOffset[N];
    int targetOffsetKHz[N];
    ApplyCorrectionRequest q;
};

void request_init(Request* r) {
    memset(r, 0, sizeof(*r));
    for (int i = 0; i < N; i++) r->origPopulated[i] = true;
    r->q.pointCount = N;
    r->q.offsetInVfTable = true;
    r->q.lockCi = -1;
    r->q.lockedTailMask = r->tail;
    r->q.explicitCurveMask = r->explicitMask;
    r->q.originalCurvePopulated = r->origPopulated;
    r->q.hasCurvePoint = r->hasPoint;
    r->q.curvePointMHz = r->pointMHz;
    r->q.fromGpuOffset = r->fromGpuOffset;
    r->q.targetOffsetKHz = r->targetOffsetKHz;
}

const VfOffsetRange kRange = {-1000000, 1000000, true};

// Keep the full requested component, including zero and negative offsets,
// regardless of the saved MHz projection or the currently reported base.
int run_offset_contract_tests() {
    for (int component : {-150000, 0, 475000}) {
        for (long long base : {0LL, 2322000LL, 2797000LL}) {
            CurvePointOffsetRequest want = {};
            want.fromGpuOffset = true;
            want.gpuOffsetComponentKHz = component;
            want.absoluteMHz = 2797;
            want.liveBaseKHz = base;
            if (curve_point_target_offset_khz(&want) != component) return 6800;
        }
    }
    CurvePointOffsetRequest typed = {};
    typed.gpuOffsetComponentKHz = 475000;
    typed.absoluteMHz = 2900;
    typed.liveBaseKHz = 2797000;
    if (curve_point_target_offset_khz(&typed) != 103000) return 6801;
    if (curve_point_stock_base_khz(1000, 475000) != 0) return 6802;
    // Live-derived anchors use the same live-minus-VF-offset base as typed
    // points and the lock tail. No additional graphics-offset subtraction.
    if (curve_point_offset_intent_live_khz(2797000, 0, 475000) != 3272000) return 6803;
    if (curve_point_offset_intent_live_khz(2797000, 475000, 475000) != 2797000) return 6804;
    if (curve_point_offset_intent_live_khz(2827000, 475000, 475000) != 2827000) return 6805;
    if (curve_point_offset_intent_live_khz(2797000, 475000, 0) != 2322000) return 6806;
    // A separate-NVML-route anchor retains its stored value for either sign
    // of the requested component; VF-table provenance still follows the base.
    for (int component : {-150000, 0, 475000}) {
        if (curve_point_stored_lock_anchor_khz(true, true, 2797, 2827000, 0,
                component, false) != 2797000) return 6808;
        if (curve_point_stored_lock_anchor_khz(true, false, 2797, 2827000, 0,
                component, true) != 2797000) return 6809;
    }
    // Correction consumes the same target, even if a readback's base moves.
    Request r;
    request_init(&r);
    r.hasPoint[3] = r.fromGpuOffset[3] = 1;
    r.pointMHz[3] = 2797;
    r.targetOffsetKHz[3] = 475000;
    unsigned int live[N] = {};
    int programmed[N] = {}, offsets[N] = {};
    bool mask[N] = {};
    live[3] = 2827000;
    programmed[3] = 475000;
    const ApplyCorrectionReadback rb = {live, programmed};
    ApplyCorrectionPlan plan = apply_correction_plan_pass(r.q, rb, kRange, offsets, mask);
    if (plan.offsetIntentCount != 1 || offsets[3] != 475000 || !mask[3]) return 6807;
    return 0;
}

// Pre-tail judging (apply_lock_pretail_policy.h) and the post-correction
// flat-tail check (apply_correction_tail_monotonic_violation): a requested
// point with offset provenance is judged where its offset puts it over the
// base the driver reports now, never by its stored projection.
int run_pretail_offset_intent_tests() {
    constexpr int P = 8;
    bool populated[P], tail[P], hasPoint[P] = {}, fromOffset[P] = {};
    unsigned int pointMHz[P] = {};
    long long base[P], unowned[P];
    int component[P];
    for (int i = 0; i < P; i++) {
        populated[i] = true;
        tail[i] = i >= 5;
        base[i] = 2000000LL + 100000LL * i;   // stock 2000..2700 MHz
        unowned[i] = base[i];
        component[i] = 0;
    }
    unsigned int target = 0;
    // 6816: a cold-boot projection.  Point 4 was saved as base 2350 + 0 and is
    // stored at 2350 MHz; the driver now reports its base at 2400, so with no
    // GPU offset the point runs at 2400 -- above a 2380 lock.  Judging the
    // stored 2350 let the lock through and the tail could not stay flat.
    hasPoint[4] = true;
    fromOffset[4] = true;
    pointMHz[4] = 2350;
    if (apply_lock_pretail_first_violation(P, APPLY_LOCK_PRETAIL_PROVENANCE_LIVE_BASE, 2380u, populated, tail, hasPoint, fromOffset,
            pointMHz, false, base, component, unowned, &target) != 4 || target != 2400)
        return 6816;
    // 6817: the reverse -- a warm-saved projection (2450) above the lock while
    // the live base + component (2400 + 0) is below it: no false refusal.
    pointMHz[4] = 2450;
    if (apply_lock_pretail_first_violation(P, APPLY_LOCK_PRETAIL_PROVENANCE_LIVE_BASE, 2420u, populated, tail, hasPoint, fromOffset,
            pointMHz, false, base, component, unowned, &target) != -1)
        return 6817;
    // 6818: under a GPU offset the provenance point takes its component, like
    // an unrequested neighbour (2400 + 100 > 2450).
    for (int i = 0; i < P; i++) component[i] = 100;
    if (apply_lock_pretail_first_violation(P, APPLY_LOCK_PRETAIL_PROVENANCE_LIVE_BASE, 2450u, populated, tail, hasPoint, fromOffset,
            pointMHz, true, base, component, unowned, &target) != 4 || target != 2500)
        return 6818;
    // Separate NVML route: a requested provenance point keeps its stored MHz,
    // just as before the unmeasured scalar compensation was introduced.
    pointMHz[4] = 2440;
    if (apply_lock_pretail_first_violation(P, APPLY_LOCK_PRETAIL_PROVENANCE_STORED, 2450u, populated, tail, hasPoint, fromOffset,
            pointMHz, true, base, component, unowned, &target) != -1) return 6810;
    pointMHz[4] = 2460;
    if (apply_lock_pretail_first_violation(P, APPLY_LOCK_PRETAIL_PROVENANCE_STORED, 2450u, populated, tail, hasPoint, fromOffset,
            pointMHz, true, base, component, unowned, &target) != 4 || target != 2460)
        return 6811;
    // 6819: a TYPED point keeps its absolute, whatever the base says.
    fromOffset[4] = false;
    pointMHz[4] = 2440;
    if (apply_lock_pretail_first_violation(P, APPLY_LOCK_PRETAIL_PROVENANCE_LIVE_BASE, 2450u, populated, tail, hasPoint, fromOffset,
            pointMHz, true, base, component, unowned, &target) != -1)
        return 6819;

    // 6870-6876: the PRE-RESET base (live minus programmed while another
    // profile is still applied) is only an estimate.  Field case, Blackwell:
    // switching from a profile with typed points at 74/75 and a flatten lock
    // to one with +475 MHz on points 70+ and a 2957 MHz lock.  The untouched
    // point 73 read 2512 MHz with offset 0 (stock ~2397, as saved), so
    // base + 475 = 2987 refused every switch, although the stored projection
    // 2872 and the post-reset stock base both satisfy the lock.
    {
        constexpr int Q = 4;
        bool qPopulated[Q] = {true, true, true, true};
        bool qTail[Q] = {false, false, false, true};
        bool qHas[Q] = {false, false, true, true};
        bool qFrom[Q] = {false, false, true, false};
        unsigned int qMHz[Q] = {0, 0, 2872, 2957};
        long long qBase[Q] = {2300000, 2350000, 2512000, 2442000};
        long long qUnowned[Q] = {2300000, 2350000, 2512000, 2442000};
        int qComponent[Q] = {0, 0, 475, 475};
        if (apply_lock_pretail_first_violation(Q, APPLY_LOCK_PRETAIL_PROVENANCE_AGREEMENT,
                2957u, qPopulated, qTail, qHas, qFrom, qMHz, true, qBase, qComponent,
                qUnowned, &target) != -1)
            return 6870;
        // The same inputs judged as a post-reset readback do refuse: that mode
        // is only correct for a base read with every offset cleared.
        if (apply_lock_pretail_first_violation(Q, APPLY_LOCK_PRETAIL_PROVENANCE_LIVE_BASE,
                2957u, qPopulated, qTail, qHas, qFrom, qMHz, true, qBase, qComponent,
                qUnowned, &target) != 2 || target != 2987)
            return 6871;
        // After the reset the real stock base (2397) passes in LIVE_BASE too.
        qBase[2] = 2397000;
        if (apply_lock_pretail_first_violation(Q, APPLY_LOCK_PRETAIL_PROVENANCE_LIVE_BASE,
                2957u, qPopulated, qTail, qHas, qFrom, qMHz, true, qBase, qComponent,
                qUnowned, &target) != -1)
            return 6872;
        // When both estimates are above the lock the pre-check still refuses,
        // naming the lower (more conservative) of the two.
        qBase[2] = 2512000;
        qMHz[2] = 3000;
        if (apply_lock_pretail_first_violation(Q, APPLY_LOCK_PRETAIL_PROVENANCE_AGREEMENT,
                2957u, qPopulated, qTail, qHas, qFrom, qMHz, true, qBase, qComponent,
                qUnowned, &target) != 2 || target != 2987)
            return 6873;
        // Typed points and unrequested points are unaffected by the mode.
        qFrom[2] = false;
        if (apply_lock_pretail_first_violation(Q, APPLY_LOCK_PRETAIL_PROVENANCE_AGREEMENT,
                2957u, qPopulated, qTail, qHas, qFrom, qMHz, true, qBase, qComponent,
                qUnowned, &target) != 2 || target != 3000)
            return 6874;
    }
    if (apply_lock_pretail_provenance_mode(false, false) != APPLY_LOCK_PRETAIL_PROVENANCE_STORED ||
        apply_lock_pretail_provenance_mode(false, true) != APPLY_LOCK_PRETAIL_PROVENANCE_STORED ||
        apply_lock_pretail_provenance_mode(true, true) != APPLY_LOCK_PRETAIL_PROVENANCE_LIVE_BASE ||
        apply_lock_pretail_provenance_mode(true, false) != APPLY_LOCK_PRETAIL_PROVENANCE_AGREEMENT)
        return 6875;
    if (strcmp(apply_lock_pretail_provenance_name(APPLY_LOCK_PRETAIL_PROVENANCE_AGREEMENT),
               "agreement") != 0)
        return 6876;

    // 6880-6883: which points the NVML/VF composition evidence line reports.
    {
        constexpr int R = 6;
        bool rPopulated[R] = {true, true, true, true, true, true};
        bool rTail[R] = {false, false, false, false, true, true};
        bool rHas[R] = {false, false, true, true, true, true};
        bool rFrom[R] = {false, false, false, true, false, false};
        int prov = 9, ref = 9;
        // Separate NVML route, live scalar, a provenance point at 3 (2 is typed):
        // the reference is the nearest unrequested point below it.
        if (!curve_point_nvml_composition_probe(R, false, 100000, rPopulated, rTail, rHas,
                rFrom, &prov, &ref) || prov != 3 || ref != 1)
            return 6880;
        // The question does not arise on the VF-table route or without a scalar.
        if (curve_point_nvml_composition_probe(R, true, 100000, rPopulated, rTail, rHas,
                rFrom, &prov, &ref) || prov != -1 || ref != -1 ||
            curve_point_nvml_composition_probe(R, false, 0, rPopulated, rTail, rHas,
                rFrom, &prov, &ref))
            return 6881;
        // No provenance point below the tail: nothing to report.
        rFrom[3] = false;
        rFrom[4] = true;   // a tail point never counts
        if (curve_point_nvml_composition_probe(R, false, 100000, rPopulated, rTail, rHas,
                rFrom, &prov, &ref))
            return 6882;
        // A provenance point with no unrequested point below still reports.
        rFrom[0] = true;
        rHas[0] = true;
        if (!curve_point_nvml_composition_probe(R, false, 100000, rPopulated, rTail, rHas,
                rFrom, &prov, &ref) || prov != 0 || ref != -1)
            return 6883;
    }

    // 6820: the flat-tail check after correction.  Point 4 is offset intent
    // (provenance) holding +0; its stored projection 2350 is a whole bin below
    // where it really runs (live 2400), above the 2380 lock.
    {
        Request r;
        request_init(&r);
        unsigned int freq[N] = {};
        int programmed[N] = {};
        r.q.hasLock = true;
        r.q.lockMHz = 2380;
        r.q.lockCi = 5;
        for (int i = 0; i < N; i++) freq[i] = 2000000u + 100000u * (unsigned int)i;
        for (int i = 5; i < N; i++) {
            r.tail[i] = true;
            r.hasPoint[i] = 1;
            r.pointMHz[i] = 2380;
            freq[i] = 2400000;                // the driver's running maximum
        }
        r.hasPoint[4] = 1;
        r.fromGpuOffset[4] = 1;
        r.pointMHz[4] = 2350;
        r.targetOffsetKHz[4] = 0;
        const ApplyCorrectionReadback rb = {freq, programmed};
        unsigned int prev = 0;
        if (apply_correction_tail_monotonic_violation(r.q, rb, &prev) != 5 || prev != 2400)
            return 6820;
        // 6821: the reverse: projection 2450, live 2300 + 0 under a 2380 lock.
        freq[4] = 2300000;
        r.pointMHz[4] = 2450;
        if (apply_correction_tail_monotonic_violation(r.q, rb, &prev) != -1) return 6821;
        // 6822: a driver-limited offset is judged by its TARGET offset over the
        // live base: programmed +0 of a +100 MHz target on base 2300 = 2400.
        r.targetOffsetKHz[4] = 100000;
        if (apply_correction_tail_monotonic_violation(r.q, rb, &prev) != 5 || prev != 2400)
            return 6822;
        // The separate NVML route retains the stored-frequency check; this
        // flag must not change the offset target correction itself writes.
        r.q.offsetInVfTable = false;
        r.pointMHz[4] = 2370;
        if (apply_correction_tail_monotonic_violation(r.q, rb, &prev) != -1) return 6812;
        r.pointMHz[4] = 2390;
        if (apply_correction_tail_monotonic_violation(r.q, rb, &prev) != 5 || prev != 2390)
            return 6813;
        int offsets[N] = {};
        bool mask[N] = {};
        apply_correction_plan_pass(r.q, rb, kRange, offsets, mask);
        if (!mask[4] || offsets[4] != 100000) return 6814;
        r.q.offsetInVfTable = true;
        // 6823: a typed predecessor is still judged by its MHz.
        r.fromGpuOffset[4] = 0;
        r.explicitMask[4] = true;
        r.pointMHz[4] = 2370;
        if (apply_correction_tail_monotonic_violation(r.q, rb, &prev) != -1) return 6823;
    }
    return 0;
}

// The boost tally (apply_correction_boost_tally) behind the outcome severity
// and the "N of M boost points matched" summary.
int run_boost_tally_tests() {
    auto tol8 = [](int) { return 8u; };
    Request r;
    request_init(&r);
    r.q.gpuPolicyViaCurveBatch = true;
    unsigned int freq[N] = {};
    int programmed[N] = {};
    for (int i = 0; i < N; i++) freq[i] = 2000000u + 50000u * (unsigned int)i;
    // Points 1..4 synthesized by the selective batch (+200 MHz target), point 6
    // typed at 2310 MHz, point 0 excluded (target 0).
    for (int i = 0; i <= 4; i++) {
        r.hasPoint[i] = 1;
        r.targetOffsetKHz[i] = i == 0 ? 0 : 200000;
        r.pointMHz[i] = 2000 + 50 * i + (i == 0 ? 0 : 200);
    }
    r.hasPoint[6] = 1;
    r.explicitMask[6] = true;
    r.pointMHz[6] = 2310;
    r.targetOffsetKHz[6] = 10000;                  // what the first batch wrote
    const ApplyCorrectionReadback rb = {freq, programmed};

    // 6824: the first batch: points 1..4 still at 0, the typed point short.
    ApplyBoostTally t = apply_correction_boost_tally(r.q, rb, tol8);
    if (t.applied != 1 || t.failed != 5 || t.userFailed != 5 || t.userApplied != 0 ||
        t.firstFailedCi != 1 || !t.firstFailedOffsetIntent) return 6824;
    // 6825: after correction everything holds: the offsets landed and the
    // typed point sits at its MHz with a DIFFERENT offset than the first batch
    // chose (its base moved a bin).  The old offset-only count called it failed.
    for (int i = 1; i <= 4; i++) {
        programmed[i] = 200000;
        freq[i] += 200000;
    }
    programmed[6] = -40000;
    freq[6] = 2310000;
    t = apply_correction_boost_tally(r.q, rb, tol8);
    if (t.failed != 0 || t.userFailed != 0 || t.applied != 6 || t.userApplied != 5 ||
        t.firstFailedCi != -1) return 6825;
    // 6826: the cold-boot shape -- a whole VF bin of base movement on an
    // offset-intent point that holds its offset is still matched; its preview
    // MHz is irrelevant.
    freq[2] += 30000;
    t = apply_correction_boost_tally(r.q, rb, tol8);
    if (t.failed != 0) return 6826;
    // 6827: a driver-limited offset is unmatched (verification accepts it as
    // safe; the tally reports it as partial), and so is a typed point off its
    // MHz by more than the tolerance.
    programmed[3] = 150000;
    t = apply_correction_boost_tally(r.q, rb, tol8);
    if (t.failed != 1 || t.userFailed != 1 || t.firstFailedCi != 3 ||
        t.firstFailedProgrammedKHz != 150000 || t.firstFailedTargetOffsetKHz != 200000)
        return 6827;
    programmed[3] = 200000;
    freq[6] = 2330000;
    t = apply_correction_boost_tally(r.q, rb, tol8);
    if (t.failed != 1 || t.firstFailedCi != 6 || t.firstFailedOffsetIntent ||
        t.firstFailedTargetMHz != 2310) return 6828;
    // 6829: locked-tail points belong to the flatten tally, not this one.
    r.q.hasLock = true;
    r.q.lockMHz = 2300;
    r.q.lockCi = 6;
    for (int i = 6; i < N; i++) r.tail[i] = true;
    t = apply_correction_boost_tally(r.q, rb, tol8);
    if (t.failed != 0 || t.applied != 5) return 6829;
    return 0;
}

int run_pre_blackwell_offset_routing_tests() {
    // 6852: VF-table offset routing predicate across Blackwell and pre-Blackwell.
    if (!curve_point_apply_uses_vf_table_offset(true, true, 150, 0, 0, 0)) return 6852;
    if (!curve_point_apply_uses_vf_table_offset(true, true, 150, 5, 0, 0)) return 6852;
    if (!curve_point_apply_uses_vf_table_offset(false, false, 0, 0, 0, 0)) return 6852;
    if (!curve_point_apply_uses_vf_table_offset(false, true, 0, 0, 0, 0)) return 6852;
    if (!curve_point_apply_uses_vf_table_offset(false, true, 150, 5, 0, 0)) return 6852;
    if (curve_point_apply_uses_vf_table_offset(false, true, 150, 0, 0, 0)) return 6852;

    // 6858: a sparse curve/lock request that does not own the GPU-offset domain
    // must follow the offset route it retains.  A live uniform scalar stays on
    // the separate NVML route; a live selective offset stays on the VF route;
    // no live offset leaves the request on the VF-table route.  An explicit
    // request for zero owns the scalar clear, so the eventual route is VF.
    if (curve_point_apply_uses_vf_table_offset(false, false, 0, 0, 150, 0))
        return 6858;
    if (!curve_point_apply_uses_vf_table_offset(false, false, 0, 0, 150, 5))
        return 6858;
    if (!curve_point_apply_uses_vf_table_offset(false, false, 0, 0, 0, 0))
        return 6858;
    if (!curve_point_apply_uses_vf_table_offset(false, true, 0, 0, 150, 0))
        return 6858;

    // 6853: Pre-Blackwell uniform (+150, excl=0) -> selective (+150, excl=5)
    // clears the prior scalar NVML offset before writing the VF curve batch.
    CurveOffsetRoutingPlan p = curve_point_plan_gpu_offset_routing(
        false, true, 150, 5, 150, 0);
    if (p.activeDesiredExcludeLowCount != 5 || !p.policyChangeRequested ||
        p.shouldApplyGpuOffset || !p.gpuPolicyViaCurveBatch ||
        !p.zeroPriorScalarBeforeCurveBatch || p.curveBatchGpuOffsetMHz != 150 ||
        !p.lastApplyUsedGpuOffset) {
        return 6853;
    }

    // 6854: Pre-Blackwell selective (+150, excl=5) -> uniform (+150, excl=0)
    // writes the uniform NVML offset and strips the selective VF batch to 0.
    p = curve_point_plan_gpu_offset_routing(false, true, 150, 0, 150, 5);
    if (p.activeDesiredExcludeLowCount != 0 || !p.policyChangeRequested ||
        !p.shouldApplyGpuOffset || !p.gpuPolicyViaCurveBatch ||
        p.zeroPriorScalarBeforeCurveBatch || p.curveBatchGpuOffsetMHz != 0 ||
        !p.lastApplyUsedGpuOffset) {
        return 6854;
    }

    // 6855: Pre-Blackwell selective (+150, excl=5) -> zero (0, excl=0)
    // strips the selective VF batch without a redundant NVML write.
    p = curve_point_plan_gpu_offset_routing(false, true, 0, 0, 150, 5);
    if (p.activeDesiredExcludeLowCount != 0 || !p.policyChangeRequested ||
        p.shouldApplyGpuOffset || !p.gpuPolicyViaCurveBatch ||
        p.zeroPriorScalarBeforeCurveBatch || p.curveBatchGpuOffsetMHz != 0 ||
        p.lastApplyUsedGpuOffset) {
        return 6855;
    }

    // 6856: Blackwell selective (+150, excl=5) -> uniform (+150, excl=0)
    // stays entirely on the VF curve batch and zeros prior VF offsets first.
    p = curve_point_plan_gpu_offset_routing(true, true, 150, 0, 150, 5);
    if (p.activeDesiredExcludeLowCount != 0 || !p.policyChangeRequested ||
        p.shouldApplyGpuOffset || !p.gpuPolicyViaCurveBatch ||
        !p.zeroPriorScalarBeforeCurveBatch || p.curveBatchGpuOffsetMHz != 150 ||
        !p.lastApplyUsedGpuOffset) {
        return 6856;
    }

    // 6857: Unchanged non-zero uniform or selective offset with a curve/lock
    // edit preserves lastApplyUsedGpuOffset = true even when gpuPolicyViaCurveBatch is false.
    p = curve_point_plan_gpu_offset_routing(false, true, 150, 0, 150, 0);
    if (p.policyChangeRequested || p.gpuPolicyViaCurveBatch || !p.lastApplyUsedGpuOffset)
        return 6857;
    p = curve_point_plan_gpu_offset_routing(false, true, 150, 5, 150, 5);
    if (p.policyChangeRequested || p.gpuPolicyViaCurveBatch || !p.lastApplyUsedGpuOffset)
        return 6857;
    p = curve_point_plan_gpu_offset_routing(false, false, 0, 0, 150, 5);
    if (p.policyChangeRequested || !p.lastApplyUsedGpuOffset)
        return 6857;

    return 0;
}

int run_gpu_offset_range_preflight_tests() {
    // 6860: unknown ranges are permissive, because they carry no evidence that
    // the request is invalid.  Known ranges are inclusive at both endpoints.
    if (!curve_point_gpu_offset_request_in_range(false, 999, true, -100, 100))
        return 6860;
    if (!curve_point_gpu_offset_request_in_range(true, 999, false, -100, 100))
        return 6860;
    if (!curve_point_gpu_offset_request_in_range(true, -100, true, -100, 100) ||
        !curve_point_gpu_offset_request_in_range(true, 100, true, -100, 100))
        return 6860;

    // 6861: an out-of-range owned offset must be detectable before the reset
    // path runs, in either direction, including zero outside a positive range.
    if (curve_point_gpu_offset_request_in_range(true, -101, true, -100, 100) ||
        curve_point_gpu_offset_request_in_range(true, 101, true, -100, 100) ||
        curve_point_gpu_offset_request_in_range(true, 0, true, 10, 20))
        return 6861;
    return 0;
}

}  // namespace

int run_offset_intent_tests() {
    if (int r = run_offset_contract_tests()) return r;
    if (int r = run_pretail_offset_intent_tests()) return r;
    if (int r = run_boost_tally_tests()) return r;
    if (int r = run_pre_blackwell_offset_routing_tests()) return r;
    if (int r = run_gpu_offset_range_preflight_tests()) return r;
    return 0;
}

