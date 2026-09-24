// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The Windows apply's VF correction loop (apply_correction_policy.h), driven
// by a simulated driver instead of NvAPI.  Assertion codes 6500-6549.
//
// The simulated driver keeps a stock base and a programmed offset per point,
// and reports live = base + offset, optionally with the quirks the loop was
// built around: monotonic collapse of a floored tail onto its anchor, a point
// the driver will not move, a base that moves under load between passes, a
// point that only creeps towards its target, and VF-bin quantisation.

#include <climits>
#include <cstring>

#include "apply_correction_policy.h"

namespace {

constexpr int N = 16;

struct SimDriver {
    long long baseKHz[N];
    int programmedKHz[N];
    unsigned int liveKHz[N];
    bool populated[N];
    bool ignoreWrites[N];     // the driver keeps this point where it is
    bool creep[N];            // moves at most 1 MHz towards the request per write
    int quantumKHz;           // live is floored to this bin (0 = exact)
    bool monotonic;           // live is the running maximum, like NVIDIA's curve
    int writes;
};

void sim_init(SimDriver* d) {
    memset(d, 0, sizeof(*d));
    for (int i = 0; i < N; i++) {
        d->populated[i] = true;
        d->baseKHz[i] = 1500000LL + 50000LL * i;
    }
}

void sim_refresh(SimDriver* d) {
    unsigned int running = 0;
    for (int i = 0; i < N; i++) {
        if (!d->populated[i]) { d->liveKHz[i] = 0; continue; }
        long long v = d->baseKHz[i] + d->programmedKHz[i];
        if (v < 0) v = 0;
        if (d->quantumKHz > 0) v -= v % d->quantumKHz;
        unsigned int live = (unsigned int)v;
        if (d->monotonic && live < running) live = running;
        running = live;
        d->liveKHz[i] = live;
    }
}

void sim_write(SimDriver* d, const int* offsets, const bool* mask) {
    d->writes++;
    for (int i = 0; i < N; i++) {
        if (!mask[i] || d->ignoreWrites[i]) continue;
        if (d->creep[i]) {
            int step = offsets[i] > d->programmedKHz[i] ? 1000 : -1000;
            if (offsets[i] != d->programmedKHz[i]) d->programmedKHz[i] += step;
            continue;
        }
        d->programmedKHz[i] = offsets[i];
    }
    sim_refresh(d);
}

struct Request {
    bool tail[N];
    bool explicitMask[N];
    bool origPopulated[N];
    unsigned char hasPoint[N];
    unsigned int pointMHz[N];
    unsigned char fromGpuOffset[N];
    int gpuComponentKHz[N];
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
    r->q.gpuOffsetComponentKHz = r->gpuComponentKHz;
}

void request_point(Request* r, int ci, unsigned int mhz) {
    r->hasPoint[ci] = 1;
    r->pointMHz[ci] = mhz;
    r->explicitMask[ci] = true;
}

// FLATTEN lock at `anchor`: every point from the anchor on is tail.
void request_lock(Request* r, int anchor, unsigned int lockMHz, bool viaCurveBatch) {
    r->q.hasLock = true;
    r->q.lockMHz = lockMHz;
    r->q.lockCi = anchor;
    r->q.gpuPolicyViaCurveBatch = viaCurveBatch;
    for (int i = anchor; i < N; i++) {
        r->tail[i] = true;
        r->hasPoint[i] = 1;
        r->pointMHz[i] = lockMHz;
    }
}

const VfOffsetRange kRange = {-1000000, 1000000, true};

// Tolerance verification, the shape of curve_targets_match_request().
bool sim_verify(const Request& r, const SimDriver& d, unsigned int tolMHz) {
    for (int i = 0; i < N; i++) {
        if (!r.hasPoint[i]) continue;
        if (d.liveKHz[i] == 0) return false;
        unsigned int actual = apply_correction_display_mhz(d.liveKHz[i]);
        unsigned int target = (r.q.hasLock && r.tail[i] && r.q.lockMHz) ? r.q.lockMHz : r.pointMHz[i];
        int diff = (int)actual - (int)target;
        if (diff < -(int)tolMHz || diff > (int)tolMHz) return false;
    }
    return true;
}

struct LoopRun {
    ApplyCorrectionLoopOutcome outcome;
    ApplyCorrectionPlan lastPlan;
    ApplyCorrectionPassStats lastStats;
    int firstPassOffsets[N];
    bool firstPassMask[N];
};

// The same wiring gpu_backend_apply.cpp uses, with the simulated driver in
// place of the readback and the write.  `budgetPasses` = passes the budget
// lets start (0 = unlimited).  `baseDriftKHz` moves every base once, after
// the first write, the way a load step moves it.
LoopRun run_loop(Request* r, SimDriver* d, unsigned int tolMHz, int budgetPasses = 0,
                 long long baseDriftKHz = 0) {
    LoopRun run = {};
    unsigned int freq[N];
    int programmed[N];
    const ApplyCorrectionReadback rb = {freq, programmed};
    auto snapshot = [&]() {
        for (int i = 0; i < N; i++) { freq[i] = d->liveKHz[i]; programmed[i] = d->programmedKHz[i]; }
    };
    int prevError[N];
    apply_correction_reset_history(N, prevError);
    ApplyCorrectionPointReport reports[N];
    run.outcome = apply_run_correction_loop(APPLY_CORRECTION_MAX_PASSES,
        [&](int pass) { return budgetPasses == 0 || pass < budgetPasses; },
        [&](int pass) {
            int offsets[N];
            bool mask[N];
            snapshot();
            run.lastPlan = apply_correction_plan_pass(r->q, rb, kRange, offsets, mask);
            if (pass == 0) {
                memcpy(run.firstPassOffsets, offsets, sizeof(offsets));
                memcpy(run.firstPassMask, mask, sizeof(mask));
            }
            if (!run.lastPlan.haveCorrections) return false;
            sim_write(d, offsets, mask);
            if (pass == 0 && baseDriftKHz) {
                for (int i = 0; i < N; i++) d->baseKHz[i] += baseDriftKHz;
                sim_refresh(d);
            }
            return true;
        },
        [&](int pass) {
            snapshot();
            run.lastStats = apply_correction_classify_pass(r->q, rb, kRange, pass, prevError, reports);
            return run.lastStats.fixedPoint;
        },
        [&](int) { return sim_verify(*r, *d, tolMHz); });
    return run;
}

int run_plan_tests() {
    // 6500: an exact driver converges on the first pass with absolute offsets.
    {
        SimDriver d; sim_init(&d); sim_refresh(&d);
        Request r; request_init(&r);
        request_point(&r, 3, 1700);   // base 1650
        request_point(&r, 5, 1720);   // base 1750
        LoopRun run = run_loop(&r, &d, 8);
        if (!run.outcome.verified || run.outcome.passesRun != 1 ||
            run.firstPassOffsets[3] != 50000 || run.firstPassOffsets[5] != -30000 ||
            run.firstPassMask[4] || !run.firstPassMask[3]) return 6500;
    }
    // 6501: offsets are recomputed from live - programmed, never accumulated:
    // a point already at its target keeps exactly its programmed offset.
    {
        SimDriver d; sim_init(&d);
        d.programmedKHz[3] = 50000;
        sim_refresh(&d);
        Request r; request_init(&r);
        request_point(&r, 3, 1700);
        unsigned int freq[N]; int prog[N];
        for (int i = 0; i < N; i++) { freq[i] = d.liveKHz[i]; prog[i] = d.programmedKHz[i]; }
        const ApplyCorrectionReadback rb = {freq, prog};
        int offsets[N]; bool mask[N];
        ApplyCorrectionPlan plan = apply_correction_plan_pass(r.q, rb, kRange, offsets, mask);
        if (!plan.haveCorrections || offsets[3] != 50000) return 6501;
    }
    // 6502: a base that moves under load between passes is followed: pass 2
    // re-derives against the new base and verifies.
    {
        SimDriver d; sim_init(&d); sim_refresh(&d);
        Request r; request_init(&r);
        request_point(&r, 3, 1700);
        LoopRun run = run_loop(&r, &d, 8, 0, 15000 * 2);   // +30 MHz after pass 1
        if (!run.outcome.verified || run.outcome.passesRun != 2 ||
            d.programmedKHz[3] != 20000) return 6502;
    }
    // 6503: a point the driver will not move ends the loop at the fixed point
    // on pass 2, not after 25 passes holding the hardware gate.
    {
        SimDriver d; sim_init(&d); sim_refresh(&d);
        d.ignoreWrites[4] = true;
        Request r; request_init(&r);
        request_point(&r, 3, 1700);
        request_point(&r, 4, 1740);   // base 1700, never moves
        LoopRun run = run_loop(&r, &d, 8);
        if (run.outcome.verified || !run.outcome.fixedPoint ||
            run.outcome.passesRun != 2 || d.writes != 2) return 6503;
    }
    // 6504: nothing requested and no lock -> nothing to correct, no write.
    {
        SimDriver d; sim_init(&d); sim_refresh(&d);
        Request r; request_init(&r);
        LoopRun run = run_loop(&r, &d, 8);
        if (run.outcome.verified || !run.outcome.nothingToCorrect ||
            run.outcome.passesRun != 1 || d.writes != 0) return 6504;
    }
    // 6505: unpopulated points are never planned.
    {
        SimDriver d; sim_init(&d);
        d.populated[3] = false;
        sim_refresh(&d);
        Request r; request_init(&r);
        request_point(&r, 3, 1700);
        request_point(&r, 5, 1800);
        LoopRun run = run_loop(&r, &d, 8);
        if (run.firstPassMask[3] || !run.firstPassMask[5]) return 6505;
    }
    // 6506: a provenance-offset point takes its GPU-offset component whatever
    // its projected MHz says (the 2026-09-17 invented +495000 kHz).
    {
        SimDriver d; sim_init(&d); sim_refresh(&d);
        Request r; request_init(&r);
        request_point(&r, 3, 2150);              // stale projection
        r.explicitMask[3] = false;
        r.fromGpuOffset[3] = 1;
        r.gpuComponentKHz[3] = 0;
        request_point(&r, 5, 1800);              // unrelated point that misses
        unsigned int freq[N]; int prog[N];
        for (int i = 0; i < N; i++) { freq[i] = d.liveKHz[i]; prog[i] = d.programmedKHz[i]; }
        const ApplyCorrectionReadback rb = {freq, prog};
        int offsets[N]; bool mask[N];
        apply_correction_plan_pass(r.q, rb, kRange, offsets, mask);
        if (!mask[3] || offsets[3] != 0 || offsets[5] != 50000) return 6506;
        r.gpuComponentKHz[3] = 45000;
        apply_correction_plan_pass(r.q, rb, kRange, offsets, mask);
        if (offsets[3] != 45000) return 6507;
    }
    // 6508: a point without an original base uses the plain required delta.
    {
        SimDriver d; sim_init(&d);
        d.programmedKHz[3] = 20000;
        sim_refresh(&d);
        Request r; request_init(&r);
        request_point(&r, 3, 1700);
        r.origPopulated[3] = false;
        unsigned int freq[N]; int prog[N];
        for (int i = 0; i < N; i++) { freq[i] = d.liveKHz[i]; prog[i] = d.programmedKHz[i]; }
        const ApplyCorrectionReadback rb = {freq, prog};
        int offsets[N]; bool mask[N];
        apply_correction_plan_pass(r.q, rb, kRange, offsets, mask);
        if (offsets[3] != 50000) return 6508;
    }
    // 6509: an offset beyond the range is clamped to the range and reported.
    {
        SimDriver d; sim_init(&d); sim_refresh(&d);
        Request r; request_init(&r);
        request_point(&r, 3, 1650 + 1200);
        unsigned int freq[N]; int prog[N];
        for (int i = 0; i < N; i++) { freq[i] = d.liveKHz[i]; prog[i] = d.programmedKHz[i]; }
        const ApplyCorrectionReadback rb = {freq, prog};
        int offsets[N]; bool mask[N];
        ApplyCorrectionPlan plan = apply_correction_plan_pass(r.q, rb, kRange, offsets, mask);
        if (offsets[3] != kRange.maxKHz || plan.clampedCount != 1 || plan.firstClampedCi != 3 ||
            plan.firstClampedRequestKHz != 1200000 || plan.firstClampedResultKHz != kRange.maxKHz)
            return 6509;
    }
    return 0;
}

int run_lock_tests() {
    // 6510: FLATTEN through the curve batch.  Tail points beyond the anchor get
    // the range floor, the anchor its own delta; a monotonic driver collapses
    // the floored tail onto the anchor and the lock verifies on pass 1.
    {
        SimDriver d; sim_init(&d);
        d.monotonic = true;
        sim_refresh(&d);
        Request r; request_init(&r);
        request_lock(&r, 10, 1980, true);          // anchor base 2000
        LoopRun run = run_loop(&r, &d, 8);
        if (!run.outcome.verified || run.outcome.passesRun != 1 ||
            run.lastPlan.tailFloorCount != N - 11 ||
            run.firstPassOffsets[10] != -20000 ||
            run.firstPassOffsets[11] != kRange.minKHz ||
            run.firstPassOffsets[N - 1] != kRange.minKHz) return 6510;
        for (int i = 10; i < N; i++)
            if (apply_correction_display_mhz(d.liveKHz[i]) != 1980) return 6511;
    }
    // 6512: a tail point whose error is unchanged across passes is "stuck"
    // (curve-batch GPU policy only) and diverges strictly.
    {
        Request r; request_init(&r);
        request_lock(&r, 12, 2100, true);
        unsigned int freq[N] = {}; int prog[N] = {};
        for (int i = 0; i < N; i++) freq[i] = 1500000u + 50000u * i;
        freq[13] = 2130000;                          // 30 MHz above the lock
        prog[13] = kRange.minKHz + 1;
        const ApplyCorrectionReadback rb = {freq, prog};
        VfOffsetRange wide = {INT_MIN, INT_MAX, true};
        int prevError[N];
        apply_correction_reset_history(N, prevError);
        ApplyCorrectionPointReport rep[N];
        ApplyCorrectionPassStats s0 = apply_correction_classify_pass(r.q, rb, wide, 0, prevError, rep);
        if (s0.fixedPoint || s0.stuck || prevError[13] != 30000) return 6512;
        ApplyCorrectionPassStats s1 = apply_correction_classify_pass(r.q, rb, wide, 1, prevError, rep);
        if (s1.stuck < 1 || !(rep[13].events & APPLY_CORRECTION_EVENT_TAIL_STUCK) ||
            !(rep[13].events & APPLY_CORRECTION_EVENT_STRICT_DIVERGED) ||
            rep[13].prevErrorKHz != 30000 || !s1.fixedPoint) return 6513;
    }
    // 6514: a required delta outside a known range is out of range.
    {
        Request r; request_init(&r);
        request_lock(&r, 12, 2100, true);
        unsigned int freq[N] = {}; int prog[N] = {};
        for (int i = 0; i < N; i++) freq[i] = 1500000u + 50000u * i;
        freq[14] = 3200000;                          // needs -1,100,000 kHz
        const ApplyCorrectionReadback rb = {freq, prog};
        int prevError[N];
        apply_correction_reset_history(N, prevError);
        ApplyCorrectionPointReport rep[N];
        ApplyCorrectionPassStats s = apply_correction_classify_pass(r.q, rb, kRange, 0, prevError, rep);
        if (s.outOfRange < 1 || !(rep[14].events & APPLY_CORRECTION_EVENT_TAIL_OUT_OF_RANGE) ||
            !(rep[14].events & APPLY_CORRECTION_EVENT_STRICT_DIVERGED)) return 6514;
        // The same readback against an unknown (fallback) range is not
        // evidence of divergence.
        VfOffsetRange fallback = kRange;
        fallback.known = false;
        apply_correction_reset_history(N, prevError);
        s = apply_correction_classify_pass(r.q, rb, fallback, 0, prevError, rep);
        if (rep[14].events & APPLY_CORRECTION_EVENT_TAIL_OUT_OF_RANGE) return 6515;
    }
    // 6516: without the curve-batch GPU policy, a DERIVED non-tail point that
    // read back LOWER than its preview is accepted and its target lowered; an
    // explicit one is kept strict; a higher readback is never accepted.
    {
        Request r; request_init(&r);
        request_lock(&r, 12, 2100, false);
        request_point(&r, 5, 1780);
        r.explicitMask[5] = false;                   // derived
        request_point(&r, 6, 1830);                  // explicit
        request_point(&r, 7, 1830);
        r.explicitMask[7] = false;                   // derived, reads HIGH
        unsigned int freq[N] = {}; int prog[N] = {};
        for (int i = 0; i < N; i++) freq[i] = 1500000u + 50000u * i;
        freq[5] = 1770000; freq[6] = 1820000; freq[7] = 1850000;
        for (int i = 12; i < N; i++) freq[i] = 2100000;
        const ApplyCorrectionReadback rb = {freq, prog};
        int prevError[N];
        apply_correction_reset_history(N, prevError);
        ApplyCorrectionPointReport rep[N];
        ApplyCorrectionPassStats s = apply_correction_classify_pass(r.q, rb, kRange, 0, prevError, rep);
        if (!(rep[5].events & APPLY_CORRECTION_EVENT_ACCEPTED_READBACK) || r.pointMHz[5] != 1770 ||
            prevError[5] != INT_MAX) return 6516;
        if ((rep[6].events & APPLY_CORRECTION_EVENT_ACCEPTED_READBACK) || r.pointMHz[6] != 1830 ||
            prevError[6] != 10000) return 6517;
        if ((rep[7].events & APPLY_CORRECTION_EVENT_ACCEPTED_READBACK) || r.pointMHz[7] != 1830)
            return 6518;
        if (s.acceptedNonTail != 1 || s.unconverged != 3) return 6519;
        // With the curve-batch GPU policy nothing is ever accepted.
        request_point(&r, 5, 1780);
        r.explicitMask[5] = false;
        r.q.gpuPolicyViaCurveBatch = true;
        apply_correction_reset_history(N, prevError);
        s = apply_correction_classify_pass(r.q, rb, kRange, 0, prevError, rep);
        if (s.acceptedNonTail != 0 || r.pointMHz[5] != 1780) return 6520;
    }
    // 6521: the flat-tail monotonic check names the first tail point that sits
    // below its predecessor's target, and passes a flat tail.
    {
        unsigned int freq[N]; unsigned char has[N]; unsigned int mhz[N]; bool tail[N];
        for (int i = 0; i < N; i++) {
            freq[i] = 1500000u + 50000u * i; has[i] = 1; mhz[i] = 1500 + 50 * i;
            tail[i] = i >= 10;
        }
        unsigned int prev = 0;
        if (apply_correction_tail_monotonic_violation(N, freq, has, mhz, tail, 2000, &prev) != -1)
            return 6521;
        mhz[9] = 2020;
        if (apply_correction_tail_monotonic_violation(N, freq, has, mhz, tail, 2000, &prev) != 10 ||
            prev != 2020) return 6522;
        has[9] = 0;                                  // an unrequested point is not a target
        if (apply_correction_tail_monotonic_violation(N, freq, has, mhz, tail, 2000, &prev) != -1)
            return 6523;
    }
    return 0;
}

int run_loop_control_tests() {
    // 6524: the budget stops further passes; what landed is still verified
    // (and here it did not).
    {
        SimDriver d; sim_init(&d); sim_refresh(&d);
        d.creep[3] = true;
        Request r; request_init(&r);
        request_point(&r, 3, 1700);
        LoopRun run = run_loop(&r, &d, 8, 3);
        if (run.outcome.verified || !run.outcome.budgetExhausted ||
            run.outcome.passesRun != 3 || d.writes != 3) return 6524;
    }
    // 6525: a point that keeps improving is never a fixed point; the loop runs
    // to the pass limit and no further.
    {
        SimDriver d; sim_init(&d); sim_refresh(&d);
        d.creep[3] = true;
        Request r; request_init(&r);
        request_point(&r, 3, 1700);
        LoopRun run = run_loop(&r, &d, 8);
        if (run.outcome.verified || run.outcome.fixedPoint || run.outcome.budgetExhausted ||
            run.outcome.passesRun != APPLY_CORRECTION_MAX_PASSES ||
            d.writes != APPLY_CORRECTION_MAX_PASSES) return 6525;
    }
    // 6526: a creeping point that reaches tolerance verifies before it is
    // exact (tolerance 8 MHz: 50 MHz away, 1 MHz per pass -> pass 42 would be
    // exact; inside tolerance from pass 42 - 8).
    {
        SimDriver d; sim_init(&d); sim_refresh(&d);
        d.creep[3] = true;
        Request r; request_init(&r);
        request_point(&r, 3, 1670);                  // 20 MHz away
        LoopRun run = run_loop(&r, &d, 8);
        if (!run.outcome.verified || run.outcome.passesRun != 12) return 6526;
    }
    // 6527: VERIFY BEFORE THE FIXED-POINT EXIT.  A quantised driver lands the
    // point inside tolerance but never exactly; the pass that reports "no
    // point improved" must still verify and succeed.
    {
        SimDriver d; sim_init(&d);
        d.quantumKHz = 15000;
        sim_refresh(&d);
        Request r; request_init(&r);
        request_point(&r, 3, 1700);                  // lands at 1695 every time
        // Verification that only passes from pass 2 on reproduces the order:
        // pass 2 is both the fixed point and the verified pass.
        int verifyCalls = 0;
        unsigned int freq[N]; int programmed[N];
        const ApplyCorrectionReadback rb = {freq, programmed};
        auto snapshot = [&]() {
            for (int i = 0; i < N; i++) { freq[i] = d.liveKHz[i]; programmed[i] = d.programmedKHz[i]; }
        };
        int prevError[N];
        apply_correction_reset_history(N, prevError);
        bool sawFixedPoint = false;
        ApplyCorrectionLoopOutcome out = apply_run_correction_loop(APPLY_CORRECTION_MAX_PASSES,
            [](int) { return true; },
            [&](int) {
                int offsets[N]; bool mask[N];
                snapshot();
                apply_correction_plan_pass(r.q, rb, kRange, offsets, mask);
                sim_write(&d, offsets, mask);
                return true;
            },
            [&](int pass) {
                snapshot();
                bool fp = apply_correction_classify_pass(r.q, rb, kRange, pass, prevError, nullptr).fixedPoint;
                sawFixedPoint |= fp;
                return fp;
            },
            [&](int pass) { ++verifyCalls; return pass >= 1 && sim_verify(r, d, 8); });
        if (!sawFixedPoint || !out.verified || !out.fixedPoint || out.passesRun != 2 ||
            verifyCalls != 2) return 6527;
    }
    // 6528: loop driver order, scripted: the budget is asked before a pass
    // writes, and nothing runs after a verified pass.
    {
        int order[16] = {}; int n = 0;
        ApplyCorrectionLoopOutcome out = apply_run_correction_loop(5,
            [&](int) { order[n++] = 1; return true; },
            [&](int) { order[n++] = 2; return true; },
            [&](int) { order[n++] = 3; return false; },
            [&](int pass) { order[n++] = 4; return pass == 1; });
        const int want[] = {1, 2, 3, 4, 1, 2, 3, 4};
        if (!out.verified || out.passesRun != 2 || n != 8 || memcmp(order, want, sizeof(want)) != 0)
            return 6528;
    }
    // 6529: a budget refusal on the very first pass writes nothing.
    {
        int writes = 0;
        ApplyCorrectionLoopOutcome out = apply_run_correction_loop(5,
            [](int) { return false; },
            [&](int) { ++writes; return true; },
            [](int) { return false; },
            [](int) { return true; });
        if (out.verified || !out.budgetExhausted || out.passesRun != 0 || writes != 0) return 6529;
    }
    // 6530: first-pass classification never declares a fixed point (there is
    // no previous error to compare against).
    {
        SimDriver d; sim_init(&d);
        d.ignoreWrites[3] = true;
        sim_refresh(&d);
        Request r; request_init(&r);
        request_point(&r, 3, 1700);
        unsigned int freq[N]; int prog[N];
        for (int i = 0; i < N; i++) { freq[i] = d.liveKHz[i]; prog[i] = d.programmedKHz[i]; }
        const ApplyCorrectionReadback rb = {freq, prog};
        int prevError[N];
        apply_correction_reset_history(N, prevError);
        ApplyCorrectionPassStats s = apply_correction_classify_pass(r.q, rb, kRange, 0, prevError, nullptr);
        if (s.fixedPoint || s.unconverged != 1 || s.improved != 0) return 6530;
    }
    // 6531: display rounding matches displayed_curve_mhz().
    if (apply_correction_display_mhz(1499500) != 1500 ||
        apply_correction_display_mhz(1499499) != 1499 ||
        apply_correction_display_mhz(0) != 0 ||
        apply_correction_display_mhz(0xFFFFFFFFu) != (unsigned int)(((long long)INT_MAX + 500) / 1000))
        return 6531;
    return 0;
}

}  // namespace

int run_apply_correction_tests() {
    if (int r = run_plan_tests()) return r;
    if (int r = run_lock_tests()) return r;
    if (int r = run_loop_control_tests()) return r;
    return 0;
}
