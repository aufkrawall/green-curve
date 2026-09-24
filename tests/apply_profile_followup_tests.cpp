// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// 2026-09-24 risk-audit follow-ups (assertion codes 6430-6499): the Windows
// apply's lock/pre-tail refusal that now runs before reset-to-stock
// (apply_lock_pretail_policy.h) and the Linux fixed-fan maintenance decision
// (fan_fixed_maintenance_policy.h).  Pure headers only; no GPU, no OS state.

#include <cstring>

#include "apply_lock_pretail_policy.h"
#include "fan_fixed_maintenance_policy.h"

namespace {

constexpr int N = 8;

struct PretailFixture {
    bool populated[N];
    bool tail[N];
    bool hasPoint[N];
    unsigned int pointMHz[N];
    long long base[N];
    long long unowned[N];
    int component[N];
};

// A small stock curve: 2000..2700 MHz in 100 MHz steps, all populated, tail
// from index 5.
void pretail_fixture(PretailFixture* f) {
    memset(f, 0, sizeof(*f));
    for (int i = 0; i < N; i++) {
        f->populated[i] = true;
        f->tail[i] = i >= 5;
        f->base[i] = 2000000LL + 100000LL * i;
        f->unowned[i] = f->base[i];
    }
}

int first_violation(const PretailFixture& f, unsigned int lockMHz, bool gpuOffset,
                    unsigned int* target) {
    return apply_lock_pretail_first_violation(N, lockMHz, f.populated, f.tail,
        f.hasPoint, f.pointMHz, gpuOffset, f.base, f.component, f.unowned, target);
}

int run_lock_pretail_tests() {
    PretailFixture f;
    pretail_fixture(&f);
    unsigned int target = 99;
    // Stock ramp below a 2500 MHz lock at index 5: fine.
    if (first_violation(f, 2500, false, &target) != -1 || target != 0) return 6430;
    // Points at and beyond the tail are never pre-tail, however high.
    f.unowned[6] = 9000000;
    if (first_violation(f, 2500, false, &target) != -1) return 6431;
    // A lock below the stock value of a pre-tail point is refused, naming it.
    if (first_violation(f, 2350, false, &target) != 4 || target != 2400) return 6432;
    // A requested point wins over the stock value of that point.
    f.hasPoint[4] = true;
    f.pointMHz[4] = 2300;
    if (first_violation(f, 2350, false, &target) != -1) return 6433;
    f.pointMHz[4] = 2600;
    if (first_violation(f, 2500, false, &target) != 4 || target != 2600) return 6434;
    f.hasPoint[4] = false;
    // The GPU offset raises derived points from their stock BASE.
    for (int i = 0; i < N; i++) f.component[i] = 150;
    if (first_violation(f, 2500, true, &target) != 4 || target != 2550) return 6435;
    // Excluded low points carry no component (the caller supplies 0).
    f.component[4] = 0;
    f.component[3] = 0;
    if (first_violation(f, 2500, true, &target) != -1) return 6436;
    // Unpopulated points are skipped.
    pretail_fixture(&f);
    f.populated[4] = false;
    f.unowned[4] = 9000000;
    if (first_violation(f, 2500, false, &target) != -1) return 6437;
    // No lock, no verdict.
    if (first_violation(f, 0, false, &target) != -1) return 6438;
    // Display rounding matches the apply's displayed_curve_mhz().
    if (apply_lock_pretail_display_mhz(2900499) != 2900 ||
        apply_lock_pretail_display_mhz(2900500) != 2901 ||
        apply_lock_pretail_display_mhz(-5) != 0) return 6439;
    // Stock base = live minus programmed offset, never negative.
    if (apply_lock_pretail_stock_base_khz(2850000u, 150000) != 2700000 ||
        apply_lock_pretail_stock_base_khz(2850000u, -100000) != 2950000 ||
        apply_lock_pretail_stock_base_khz(1000u, 5000) != 0) return 6440;

    // THE regression: the pre-reset view (live frequencies carrying the old
    // profile's offsets) gives the same verdict as the post-reset readback
    // (stock frequencies, zero offsets).  Before, only the post-reset view was
    // consulted, so the refusal came after the reset had already run.
    {
        const unsigned int liveKHz[N] = {2100000, 2250000, 2400000, 2500000,
                                         2600000, 2700000, 2800000, 2900000};
        const int liveOffsets[N] = {100000, 150000, 200000, 200000,
                                    200000, 200000, 200000, 200000};
        PretailFixture pre;
        PretailFixture post;
        pretail_fixture(&pre);
        pretail_fixture(&post);
        for (int i = 0; i < N; i++) {
            pre.base[i] = apply_lock_pretail_stock_base_khz(liveKHz[i], liveOffsets[i]);
            pre.unowned[i] = pre.base[i];
            post.base[i] = (long long)liveKHz[i] - liveOffsets[i];
            post.unowned[i] = post.base[i];
            pre.component[i] = post.component[i] = 100;
        }
        unsigned int preTarget = 0;
        unsigned int postTarget = 0;
        for (unsigned int lock = 2000; lock <= 2600; lock += 25) {
            for (int offset = 0; offset < 2; offset++) {
                int a = first_violation(pre, lock, offset != 0, &preTarget);
                int b = first_violation(post, lock, offset != 0, &postTarget);
                if (a != b || preTarget != postTarget) return 6441;
            }
        }
        // And the pre-reset view is NOT the raw live value: using live
        // frequencies as-is would refuse a lock the stock curve satisfies.
        PretailFixture naive = pre;
        for (int i = 0; i < N; i++) naive.unowned[i] = liveKHz[i];
        if (first_violation(pre, 2450, false, &preTarget) != -1 ||
            first_violation(naive, 2450, false, &preTarget) == -1) return 6442;
    }
    // A gc_bool8-style request mask works through the template.
    {
        PretailFixture g;
        pretail_fixture(&g);
        unsigned char mask[N] = {};
        mask[2] = 1;
        g.pointMHz[2] = 2800;
        if (apply_lock_pretail_first_violation(N, 2500u, g.populated, g.tail, mask,
                g.pointMHz, false, g.base, g.component, g.unowned, &target) != 2 ||
            target != 2800) return 6443;
    }
    return 0;
}

int run_fixed_fan_maintenance_tests() {
    FanFixedMaintenanceInputs in = {};
    in.fixedModeActive = true;
    in.intentKnown = true;
    in.intentPercent = 40;
    in.targetPercent = 40;
    in.policyKnown = true;
    in.policyManual = true;
    // Holding: nothing to write.
    FanFixedMaintenanceDecision d = fan_fixed_maintenance_decide(in);
    if (d.write || d.failure) return 6460;
    // The driver took the fan back (auto policy after a GPU reset or resume).
    in.policyManual = false;
    d = fan_fixed_maintenance_decide(in);
    if (!d.write || d.failure) return 6461;
    // The duty drifted from the target.
    in.policyManual = true;
    in.intentPercent = 55;
    d = fan_fixed_maintenance_decide(in);
    if (!d.write) return 6462;
    // One percent of readback rounding is not drift.
    in.intentPercent = 41;
    d = fan_fixed_maintenance_decide(in);
    if (d.write) return 6463;
    // No intent getter: the policy alone decides, and an unreadable policy is
    // a telemetry failure, not a reason to write blindly.
    in.intentKnown = false;
    in.intentPercent = 0;
    d = fan_fixed_maintenance_decide(in);
    if (d.write || d.failure) return 6464;
    in.policyKnown = false;
    in.policyManual = false;
    d = fan_fixed_maintenance_decide(in);
    if (d.write || !d.failure) return 6465;
    // Not in fixed mode: never.
    in = FanFixedMaintenanceInputs{};
    in.intentKnown = true;
    in.policyKnown = true;
    in.policyManual = false;
    if (fan_fixed_maintenance_decide(in).write) return 6466;
    // The target is clamped the way the write clamps it.
    in.fixedModeActive = true;
    in.policyManual = true;
    in.targetPercent = 140;
    in.intentPercent = 100;
    if (fan_fixed_maintenance_decide(in).write) return 6467;
    return 0;
}

}  // namespace

int run_apply_profile_followup_tests() {
    if (int r = run_lock_pretail_tests()) return r;
    if (int r = run_fixed_fan_maintenance_tests()) return r;
    return 0;
}
