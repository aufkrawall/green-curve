// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_APPLY_CORRECTION_POLICY_H
#define GREEN_CURVE_APPLY_CORRECTION_POLICY_H

// The Windows apply's VF correction loop, as pure decisions over a readback.
//
// The loop runs when the first curve batch did not verify: re-derive every
// point's offset from where the driver says it landed, write, read back,
// classify each point's progress, verify, and stop on success, on a fixed
// point, on the time budget, or when nothing is left to correct.  Each of those
// decisions has already been wrong once on hardware:
//
//   - a non-tail point re-derived against the reset-time base reproduced the
//     same offset every pass and never moved (2026-09-17, profile 1);
//   - a correction triggered by the tail overwrote an already-correct point
//     with an invented offset (same day; see curve_point_offset_policy.h);
//   - a NON-tail point the driver would not move never ended the loop, so it
//     ran all 25 passes holding the hardware gate;
//   - the fixed-point exit ran before verification and rolled back a curve
//     that was within tolerance.
//
// All of them used to live inline in gpu_backend_apply.cpp, reading g_app,
// where no test could reach them.  They live here now; the apply supplies the
// readback and the hardware write, and the regression harness supplies a
// recorded or simulated driver instead (tests/apply_correction_tests.cpp).
//
// Nothing in this header touches hardware, globals, clocks or logs.  Anything
// worth logging is returned so the caller logs it with the same wording.

#include <climits>

#include "curve_point_offset_policy.h"
#include "vf_offset_range_policy.h"

// Upper bound on correction passes.  The budget and the fixed-point exit
// normally end the loop far earlier; this bounds a driver that keeps
// improving by one bin per pass.
#define APPLY_CORRECTION_MAX_PASSES 25

// What the driver reports for every VF point right now.
struct ApplyCorrectionReadback {
    const unsigned int* freqKHz;      // live frequency; 0 = point not populated
    const int* programmedOffsetKHz;   // offset currently programmed
};

// The request the correction is steering towards.
struct ApplyCorrectionRequest {
    int pointCount;
    bool hasLock;
    unsigned int lockMHz;
    int lockCi;                        // the anchor point of a locked tail
    bool gpuPolicyViaCurveBatch;       // GPU offset expressed through the batch
    const bool* lockedTailMask;
    const bool* explicitCurveMask;     // points the user typed
    const bool* originalCurvePopulated;
    const unsigned char* hasCurvePoint;  // the verify request's points (gc_bool8)
    unsigned int* curvePointMHz;       // verify targets; lowered on acceptance
    const unsigned char* fromGpuOffset;  // per-point provenance (gc_bool8)
    const int* gpuOffsetComponentKHz;  // this request's per-point GPU offset
};

static inline unsigned int apply_correction_display_mhz(unsigned int freqKHz) {
    long long v = (long long)freqKHz;
    if (v > INT_MAX) v = INT_MAX;
    return (unsigned int)((v + 500) / 1000);
}

static inline bool apply_correction_point_is_tail(const ApplyCorrectionRequest& q, int ci) {
    return q.hasLock && q.lockedTailMask[ci] && q.lockMHz > 0;
}

// The stock base the driver implies for a point: live minus programmed.
static inline long long apply_correction_live_base_khz(const ApplyCorrectionReadback& rb, int ci) {
    return curve_point_stock_base_khz((long long)rb.freqKHz[ci],
                                      (long long)rb.programmedOffsetKHz[ci]);
}

// Offset that moves a point to `targetMHz` from its current base, unclamped.
static inline int apply_correction_required_delta_khz(const ApplyCorrectionReadback& rb,
                                                      int ci, unsigned int targetMHz) {
    long long delta = (long long)targetMHz * 1000LL - apply_correction_live_base_khz(rb, ci);
    if (delta > INT_MAX) delta = INT_MAX;
    if (delta < INT_MIN) delta = INT_MIN;
    return (int)delta;
}

struct ApplyCorrectionPlan {
    bool haveCorrections;
    int tailFloorCount;
    int clampedCount;          // offsets the range clamp altered
    int firstClampedCi;        // -1 when none
    int firstClampedRequestKHz;
    int firstClampedResultKHz;
};

// Offsets for one correction pass.  Every offset is recomputed ABSOLUTELY from
// the current readback (live - programmed recovers the stock base), never
// accumulated, and every non-tail point that has an original base goes through
// the one shared offset rule.  Locked-tail points beyond the anchor get the
// uniform FLATTEN floor: the driver ignores graduated tail deltas.
static inline ApplyCorrectionPlan apply_correction_plan_pass(
        const ApplyCorrectionRequest& q, const ApplyCorrectionReadback& rb,
        const VfOffsetRange& range, int* offsetsOut, bool* maskOut) {
    ApplyCorrectionPlan plan = {};
    plan.firstClampedCi = -1;
    auto clamp = [&](int ci, int requested) {
        bool altered = false;
        int v = vf_offset_range_clamp_khz(range, requested, &altered);
        if (altered) {
            if (plan.clampedCount == 0) {
                plan.firstClampedCi = ci;
                plan.firstClampedRequestKHz = requested;
                plan.firstClampedResultKHz = v;
            }
            plan.clampedCount++;
        }
        return v;
    };
    const int floorKHz = vf_offset_range_flatten_floor_khz(range);
    for (int ci = 0; ci < q.pointCount; ci++) {
        offsetsOut[ci] = 0;
        maskOut[ci] = false;
    }
    for (int ci = 0; ci < q.pointCount; ci++) {
        if (rb.freqKHz[ci] == 0) continue;
        const bool isTail = apply_correction_point_is_tail(q, ci);
        unsigned int targetMHz = 0;
        if (isTail) targetMHz = q.lockMHz;
        else if (q.hasCurvePoint[ci]) targetMHz = q.curvePointMHz[ci];
        else continue;
        if (!isTail && q.originalCurvePopulated[ci]) {
            CurvePointOffsetRequest want = {};
            want.fromGpuOffset = q.fromGpuOffset[ci] != 0;
            want.gpuOffsetComponentKHz = q.gpuOffsetComponentKHz[ci];
            want.absoluteMHz = targetMHz;
            want.liveBaseKHz = apply_correction_live_base_khz(rb, ci);
            long long diff = curve_point_target_offset_khz(&want);
            if (diff > INT_MAX) diff = INT_MAX;
            if (diff < INT_MIN) diff = INT_MIN;
            offsetsOut[ci] = clamp(ci, (int)diff);
        } else if (isTail && ci != q.lockCi) {
            offsetsOut[ci] = floorKHz;
            plan.tailFloorCount++;
        } else {
            offsetsOut[ci] = clamp(ci, apply_correction_required_delta_khz(rb, ci, targetMHz));
        }
        maskOut[ci] = true;
        plan.haveCorrections = true;
    }
    return plan;
}

enum ApplyCorrectionPointEvent {
    APPLY_CORRECTION_EVENT_NONE = 0,
    APPLY_CORRECTION_EVENT_TAIL_STUCK = 1 << 0,
    APPLY_CORRECTION_EVENT_TAIL_OUT_OF_RANGE = 1 << 1,
    APPLY_CORRECTION_EVENT_ACCEPTED_READBACK = 1 << 2,
    APPLY_CORRECTION_EVENT_STRICT_DIVERGED = 1 << 3,
};

// Per-point detail for the caller's log lines.  `events` is 0 for a point
// that converged exactly or was not examined.
struct ApplyCorrectionPointReport {
    int events;
    unsigned int actualMHz;
    unsigned int targetMHz;
    int errorKHz;
    int prevErrorKHz;          // INT_MAX before the first measurement
    int requiredDeltaKHz;
};

struct ApplyCorrectionPassStats {
    int converging, worsening, stuck, outOfRange;
    int acceptedNonTail, strictDiverged;
    int unconverged, improved;
    bool fixedPoint;
};

// Start every point's error history as "never measured".
static inline void apply_correction_reset_history(int pointCount, int* prevErrorKHz) {
    for (int ci = 0; ci < pointCount; ci++) prevErrorKHz[ci] = INT_MAX;
}

// Classify one pass's readback.  Convergence here is EXACT equality, stricter
// than verification's tolerance on purpose: it drives the fixed-point exit,
// which the caller must test only AFTER verification (a fixed point can be the
// requested result within tolerance).  Updates `prevErrorKHz`, and lowers a
// derived non-tail target to its readback where acceptance applies.
static inline ApplyCorrectionPassStats apply_correction_classify_pass(
        const ApplyCorrectionRequest& q, const ApplyCorrectionReadback& rb,
        const VfOffsetRange& range, int correctionPass, int* prevErrorKHz,
        ApplyCorrectionPointReport* reports) {
    ApplyCorrectionPassStats s = {};
    for (int ci = 0; ci < q.pointCount; ci++) {
        ApplyCorrectionPointReport* r = reports ? &reports[ci] : nullptr;
        if (r) *r = ApplyCorrectionPointReport{};
        if (!q.hasCurvePoint[ci]) continue;
        if (rb.freqKHz[ci] == 0) continue;
        const bool tail = q.lockedTailMask[ci];
        const unsigned int actualMHz = apply_correction_display_mhz(rb.freqKHz[ci]);
        const unsigned int targetMHz = apply_correction_point_is_tail(q, ci)
            ? q.lockMHz : q.curvePointMHz[ci];
        if (actualMHz == targetMHz) continue;
        s.unconverged++;
        long long actualKHz = (long long)rb.freqKHz[ci];
        long long targetKHz = (long long)targetMHz * 1000LL;
        long long err = actualKHz > targetKHz ? actualKHz - targetKHz : targetKHz - actualKHz;
        const int errorKHz = err > INT_MAX ? INT_MAX : (int)err;
        const int prev = prevErrorKHz[ci];
        const int requiredDeltaKHz = apply_correction_required_delta_khz(rb, ci, targetMHz);
        const bool measured = prev != INT_MAX;
        if (measured && errorKHz < prev) s.improved++;
        int events = 0;
        bool diverged = range.known &&
            (requiredDeltaKHz < range.minKHz || requiredDeltaKHz > range.maxKHz);
        if (q.gpuPolicyViaCurveBatch && apply_correction_point_is_tail(q, ci)) {
            if (!diverged && measured && errorKHz < prev) {
                s.converging++;
            } else if (!diverged && measured && errorKHz == prev) {
                s.stuck++;
                diverged = true;
                events |= APPLY_CORRECTION_EVENT_TAIL_STUCK;
            } else if (!diverged && measured && errorKHz > prev) {
                s.worsening++;
                diverged = true;
            } else if (diverged) {
                s.outOfRange++;
                events |= APPLY_CORRECTION_EVENT_TAIL_OUT_OF_RANGE;
            } else {
                s.converging++;
            }
        } else if (diverged) {
            s.outOfRange++;
        }
        const bool userExplicitPoint = q.explicitCurveMask[ci] && !tail;
        // Only derived, lower readbacks may be accepted.
        const bool acceptNonTailReadback = !q.gpuPolicyViaCurveBatch
            && q.hasLock
            && !tail
            && q.lockMHz > 0
            && !userExplicitPoint
            && actualMHz <= targetMHz;
        if (acceptNonTailReadback) {
            s.acceptedNonTail++;
            events |= APPLY_CORRECTION_EVENT_ACCEPTED_READBACK;
            q.curvePointMHz[ci] = actualMHz;
        } else if (diverged) {
            s.strictDiverged++;
            events |= APPLY_CORRECTION_EVENT_STRICT_DIVERGED;
            prevErrorKHz[ci] = errorKHz;
        } else {
            prevErrorKHz[ci] = errorKHz;
        }
        if (r) {
            r->events = events;
            r->actualMHz = actualMHz;
            r->targetMHz = targetMHz;
            r->errorKHz = errorKHz;
            r->prevErrorKHz = prev;
            r->requiredDeltaKHz = requiredDeltaKHz;
        }
    }
    // Once no point improves, a pass with unchanged inputs writes the same
    // offsets and reads back the same frequencies: no later pass can help.
    s.fixedPoint = correctionPass > 0 && s.unconverged > 0 && s.improved == 0;
    return s;
}

struct ApplyCorrectionLoopOutcome {
    bool verified;
    bool budgetExhausted;
    bool fixedPoint;        // stopped because the last pass was a fixed point
    bool nothingToCorrect;  // a pass found no point to write
    int passesRun;          // passes that started (planned and, if any, wrote)
};

// The loop's control flow.  Callbacks, all taking the 0-based pass index:
//   mayStart(pass)   -> bool  time budget: may this pass start at all?
//   planWrite(pass)  -> bool  plan and write; false = nothing to correct
//   classify(pass)   -> bool  classify the new readback; true = fixed point
//   verify(pass)     -> bool  does the readback satisfy the request?
// Verification runs BEFORE the fixed-point exit; see the header comment.
template <typename MayStart, typename PlanWrite, typename Classify, typename Verify>
static ApplyCorrectionLoopOutcome apply_run_correction_loop(int maxPasses,
        MayStart mayStart, PlanWrite planWrite, Classify classify, Verify verify) {
    ApplyCorrectionLoopOutcome out = {};
    for (int pass = 0; pass < maxPasses; pass++) {
        if (!mayStart(pass)) {
            out.budgetExhausted = true;
            break;
        }
        out.passesRun = pass + 1;
        if (!planWrite(pass)) {
            out.nothingToCorrect = true;
            break;
        }
        const bool fixedPoint = classify(pass);
        if (verify(pass)) {
            out.verified = true;
            out.fixedPoint = fixedPoint;
            break;
        }
        if (fixedPoint) {
            out.fixedPoint = true;
            break;
        }
    }
    return out;
}

// A FLATTEN/NONE lock must keep a flat tail: a tail point whose predecessor's
// target is above the lock would force the driver to raise the plateau.
// Returns the first violating point index, or -1; `previousTargetOut` gets the
// predecessor target that caused it.
static inline int apply_correction_tail_monotonic_violation(int pointCount,
        const unsigned int* freqKHz, const unsigned char* hasCurvePoint,
        const unsigned int* curvePointMHz, const bool* lockedTailMask,
        unsigned int lockMHz, unsigned int* previousTargetOut) {
    unsigned int lastTargetMHz = 0;
    if (previousTargetOut) *previousTargetOut = 0;
    for (int ci = 0; ci < pointCount; ci++) {
        if (!freqKHz[ci] || !hasCurvePoint[ci]) continue;
        const unsigned int currentTarget = lockedTailMask[ci] ? lockMHz : curvePointMHz[ci];
        if (lockedTailMask[ci] && ci > 0 && lastTargetMHz > currentTarget) {
            if (previousTargetOut) *previousTargetOut = lastTargetMHz;
            return ci;
        }
        lastTargetMHz = currentTarget;
    }
    return -1;
}

#endif  // GREEN_CURVE_APPLY_CORRECTION_POLICY_H
