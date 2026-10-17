// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// "Offset intent vs absolute MHz" follow-ups to the 2026-10-17 correction-loop
// fix (assertion codes 6570-6599).  Pure headers only; no GPU, no OS state.
//
// The simulated driver here models a PRE-Blackwell board: a uniform GPU offset
// is a separate scalar (the NVML graphics offset) that the driver adds to
// every VF point on top of the per-point delta, and the live readback includes
// it.  No Pascal..Ada hardware has run these shapes; the readback-includes-
// the-scalar assumption is the same one the apply's lock-anchor math already
// relies on (see llm-wiki/clock-transition-audit.md).

#include <climits>
#include <cstring>

#include "apply_correction_policy.h"

namespace {

constexpr int N = 12;

struct ScalarSim {
    long long stockKHz[N];
    int deltaKHz[N];      // per-point VF delta (what the curve batch writes)
    int scalarKHz;        // NVML graphics offset, added to every point
    unsigned int liveKHz[N];
};

void scalar_sim_init(ScalarSim* d, int scalarKHz) {
    memset(d, 0, sizeof(*d));
    d->scalarKHz = scalarKHz;
    for (int i = 0; i < N; i++) d->stockKHz[i] = 1500000LL + 50000LL * i;
}

void scalar_sim_refresh(ScalarSim* d) {
    for (int i = 0; i < N; i++) {
        long long v = d->stockKHz[i] + d->scalarKHz + d->deltaKHz[i];
        d->liveKHz[i] = v < 0 ? 0u : (unsigned int)v;
    }
}

void scalar_sim_write(ScalarSim* d, const int* offsets, const bool* mask) {
    for (int i = 0; i < N; i++)
        if (mask[i]) d->deltaKHz[i] = offsets[i];
    scalar_sim_refresh(d);
}

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

// The verifier's offset branch: an offset-intent point fails only when its
// programmed delta is above the target.
bool verify_offsets(const Request& r, const ScalarSim& d) {
    for (int i = 0; i < N; i++) {
        if (!r.hasPoint[i]) continue;
        if (!apply_correction_point_offset_is_intent(r.q, i)) continue;
        if ((long long)d.deltaKHz[i] > (long long)r.targetOffsetKHz[i] + 12000) return false;
    }
    return true;
}

// A `base_plus_gpu_offset` profile on a pre-Blackwell board: +475 MHz uniform
// GPU offset (written as the NVML scalar), points 2..5 reconstructed as offset
// intent.  Builds each point's delta through the shared policy exactly the way
// apply_build_curve_targets() does, with `scalarKHz` as the scalar argument.
void build_reconstructed(Request* r, const ScalarSim& d, int componentKHz, int scalarKHz) {
    request_init(r);
    for (int i = 2; i <= 5; i++) {
        r->hasPoint[i] = 1;
        r->fromGpuOffset[i] = 1;
        r->pointMHz[i] = (unsigned int)((d.stockKHz[i] + componentKHz) / 1000);
        CurvePointOffsetRequest want = {};
        want.fromGpuOffset = true;
        want.gpuOffsetComponentKHz = componentKHz;
        want.scalarGpuOffsetKHz = scalarKHz;
        want.absoluteMHz = r->pointMHz[i];
        want.liveBaseKHz = curve_point_stock_base_khz(d.liveKHz[i], d.deltaKHz[i]);
        r->targetOffsetKHz[i] = (int)curve_point_target_offset_khz(&want);
    }
}

int run_scalar_offset_tests() {
    // 6570: the policy nets the scalar out of an offset-intent point's delta.
    {
        CurvePointOffsetRequest want = {};
        want.fromGpuOffset = true;
        want.gpuOffsetComponentKHz = 475000;
        want.scalarGpuOffsetKHz = 475000;
        want.absoluteMHz = 2797;
        want.liveBaseKHz = 2322000;
        if (curve_point_target_offset_khz(&want) != 0) return 6570;
        // Blackwell / VF-table offset: no scalar, the delta carries it all.
        want.scalarGpuOffsetKHz = 0;
        if (curve_point_target_offset_khz(&want) != 475000) return 6571;
        // An excluded point under a scalar still left from a uniform offset
        // must cancel it, exactly like the selective batch's own formula.
        want.gpuOffsetComponentKHz = 0;
        want.scalarGpuOffsetKHz = 475000;
        if (curve_point_target_offset_khz(&want) != -475000) return 6572;
        // A typed absolute ignores the scalar: its live base already holds it.
        want.fromGpuOffset = false;
        want.absoluteMHz = 2900;
        want.liveBaseKHz = 2797000;
        if (curve_point_target_offset_khz(&want) != 103000) return 6573;
        // The scalar exists only where the offset is not in the VF table.
        if (curve_point_scalar_gpu_offset_khz(true, 475000) != 0 ||
            curve_point_scalar_gpu_offset_khz(false, 475000) != 475000 ||
            curve_point_scalar_gpu_offset_khz(false, 0) != 0) return 6574;
    }
    // 6575: THE regression.  The NVML scalar is already +475 MHz when the
    // batch is built; every reconstructed point must land at stock + 475, not
    // stock + 950, and the offset verifier must agree with what was written.
    {
        ScalarSim d;
        scalar_sim_init(&d, 475000);
        scalar_sim_refresh(&d);
        Request r;
        build_reconstructed(&r, d, 475000, d.scalarKHz);
        bool mask[N] = {};
        for (int i = 2; i <= 5; i++) mask[i] = true;
        scalar_sim_write(&d, r.targetOffsetKHz, mask);
        for (int i = 2; i <= 5; i++) {
            if (d.liveKHz[i] != (unsigned int)(d.stockKHz[i] + 475000)) return 6575;
            if (d.deltaKHz[i] != 0) return 6576;
        }
        if (!verify_offsets(r, d)) return 6577;
        // A correction pass (the tail missed, say) holds the same delta.
        unsigned int freq[N];
        int programmed[N];
        for (int i = 0; i < N; i++) { freq[i] = d.liveKHz[i]; programmed[i] = d.deltaKHz[i]; }
        const ApplyCorrectionReadback rb = {freq, programmed};
        int offsets[N];
        bool cmask[N];
        ApplyCorrectionPlan plan = apply_correction_plan_pass(r.q, rb, kRange, offsets, cmask);
        if (plan.offsetIntentCount != 4 || offsets[3] != 0 || !cmask[3]) return 6578;
    }
    // 6579: negative control -- the pre-fix build (no scalar netted out) on the
    // same board doubles the offset, and the offset verifier cannot see it:
    // that blindness is why the target build itself has to be right.
    {
        ScalarSim d;
        scalar_sim_init(&d, 475000);
        scalar_sim_refresh(&d);
        Request r;
        build_reconstructed(&r, d, 475000, 0);
        bool mask[N] = {};
        for (int i = 2; i <= 5; i++) mask[i] = true;
        scalar_sim_write(&d, r.targetOffsetKHz, mask);
        if (d.liveKHz[3] != (unsigned int)(d.stockKHz[3] + 950000)) return 6579;
        if (!verify_offsets(r, d)) return 6580;
    }
    // 6581: an absolute derived from the live readback (lock anchor, pre-tail
    // neighbour) strips the scalar too: stock + component, on either family.
    {
        // Pre-Blackwell: live 2797 = stock 2322 + scalar 475 + delta 0.
        if (curve_point_offset_intent_live_khz(2797000, 0, 475000, 475000) != 2797000) return 6581;
        // Blackwell: live 2797 = stock 2322 + delta 475, no scalar.
        if (curve_point_offset_intent_live_khz(2797000, 475000, 0, 475000) != 2797000) return 6582;
        // The base moved a bin since the profile was saved: follow it.
        if (curve_point_offset_intent_live_khz(2827000, 475000, 0, 475000) != 2827000) return 6583;
        // A request carrying no GPU offset: just the stock base.
        if (curve_point_offset_intent_live_khz(2797000, 475000, 0, 0) != 2322000) return 6584;
        if (curve_point_offset_intent_live_khz(1000, 475000, 0, 0) != 0) return 6585;
    }
    return 0;
}

}  // namespace

int run_offset_intent_tests() {
    if (int r = run_scalar_offset_tests()) return r;
    return 0;
}
