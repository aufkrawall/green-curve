// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_CURVE_FEASIBILITY_PLAN_H
#define GREEN_CURVE_CURVE_FEASIBILITY_PLAN_H

// Read-only feasibility of one curve request against one observation of the
// GPU's VF table.  Pure: no globals, no driver calls, no draft mutation.  The
// Windows apply and the Linux daemon both run it before their first setter;
// the Windows apply runs it again over its post-reset readback, where the
// same reasons describe a failure the recovery path then handles.
//
// A boolean answer was the problem it replaces.  The three issue-17 failures
// were a conflicting prefix (Pascal), a tail floor (Blackwell, 1425 MHz) and an
// anchor upper bound (Blackwell, 2400 MHz); each came out as "lower the
// preceding point" or a generic range error, which named the first symptom
// and none of the constraint.  The result carries the disposition, the
// evidence it rests on, and up to CURVE_PLAN_MAX_ISSUES typed issues, each
// with its first and worst offending point and the affected count.
//
// Evidence decides proof.  A violation computed from a request alone (a typed
// prefix MHz above the plateau, a lock anchor outside the graphics domain) is
// proven by any observation.  A violation computed from a baseline -- an
// unowned point's stock frequency, the offset an anchor needs -- is proven
// only when the baseline is trustworthy:
//   - TRANSACTION_BASELINE: the post-reset readback inside the apply.
//   - UNSHAPED_LIVE: every graphics point reads a zero programmed offset, so
//     the observed curve IS the driver's stock curve at this moment; the
//     reset-to-stock the apply would run cannot change it.
//   - LIVE_SHAPED_ESTIMATE: some offset is programmed.  Live minus offset is
//     not the stock base next to a large offset step (observed on Blackwell:
//     an untouched point read ~115 MHz above stock), nor on a collapsed
//     flatten tail.  Such violations make the result NEEDS_FRESH_BASELINE,
//     and the guarded apply re-plans over its post-reset readback.
// Nothing here widens a range or invents baseline bounds.

#include <limits.h>

#include "apply_lock_pretail_policy.h"
#include "curve_point_offset_policy.h"
#include "vf_offset_range_policy.h"
#include "vf_writable_point_policy.h"

enum CurvePlanDisposition {
    CURVE_PLAN_REPRESENTABLE = 0,
    CURVE_PLAN_REFUSED = 1,
    CURVE_PLAN_NEEDS_FRESH_BASELINE = 2,
    CURVE_PLAN_UNAVAILABLE = 3,
};

enum CurvePlanReason {
    CURVE_PLAN_REASON_NONE = 0,
    CURVE_PLAN_REASON_PREFIX_ABOVE_PLATEAU = 1,
    CURVE_PLAN_REASON_ANCHOR_ABOVE_RANGE = 2,
    CURVE_PLAN_REASON_ANCHOR_BELOW_RANGE = 3,
    CURVE_PLAN_REASON_TAIL_UNREACHABLE = 4,
    CURVE_PLAN_REASON_POINT_OUT_OF_RANGE = 5,
    CURVE_PLAN_REASON_MISSING_POINT = 6,
    CURVE_PLAN_REASON_FOREIGN_DOMAIN = 7,
    CURVE_PLAN_REASON_FLOOR_POINT = 8,
    CURVE_PLAN_REASON_FLATTEN_UNSUPPORTED = 9,
    CURVE_PLAN_REASON_ARITHMETIC_OVERFLOW = 10,
    CURVE_PLAN_REASON_READ_FAILED = 11,
};

enum CurvePlanEvidence {
    CURVE_PLAN_EVIDENCE_UNAVAILABLE = 0,
    CURVE_PLAN_EVIDENCE_LIVE_SHAPED_ESTIMATE = 1,
    CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE = 2,
    CURVE_PLAN_EVIDENCE_TRANSACTION_BASELINE = 3,
};

// Where the VF point range came from.  A policy fallback is a range the
// application is willing to WRITE against, not a measured hardware limit, and
// every message says which one it was.
enum CurvePlanRangeSource {
    CURVE_PLAN_RANGE_POLICY_FALLBACK = 0,
    CURVE_PLAN_RANGE_NVML_GRAPHICS = 1,
    CURVE_PLAN_RANGE_PSTATE = 2,
};

// How a requested non-tail point is interpreted.
enum CurvePlanPointKind {
    CURVE_PLAN_POINT_ABSENT = 0,     // not in the request: stock or left live
    CURVE_PLAN_POINT_ABSOLUTE = 1,   // a frequency the user asked for
    CURVE_PLAN_POINT_PROJECTED = 2,  // offset intent (GPU-offset projection
                                     // or an offset kept from the curve)
};

// How the lock tail is written.  Windows FLATTEN and Linux FLATTEN push every
// tail point to the range minimum and let the driver's monotonic rule collapse
// them onto the anchor; Linux HARD writes the lock frequency to every tail
// point; Windows HARD writes only the anchor.
enum CurvePlanTailStrategy {
    CURVE_PLAN_TAIL_UNIFORM_MIN_OFFSET = 0,
    CURVE_PLAN_TAIL_ABSOLUTE_TARGET = 1,
    CURVE_PLAN_TAIL_ANCHOR_ONLY = 2,
};

struct CurvePlanRange {
    int minKHz;
    int maxKHz;
    CurvePlanRangeSource source;
};

#define CURVE_PLAN_MAX_ISSUES 8

struct CurvePlanIssue {
    CurvePlanReason reason;
    bool proven;
    int ci;                     // first offending point (or the anchor)
    unsigned int voltUv;
    unsigned int targetMHz;     // the point's effective target / plateau
    long long baseKHz;          // baseline the check used (0 when none)
    long long adjustmentKHz;    // required point offset (anchor/explicit)
    long long limitKHz;         // range bound crossed, or plateau kHz
    int worstCi;                // worst offending point (== ci when one)
    unsigned int worstVoltUv;
    unsigned int worstMHz;
    int affectedCount;
    CurvePlanPointKind kind;    // of the first offending point
    bool inferredFromProgrammed;  // offset kept from the curve on the GPU
    bool belowVoltageRule;      // below MIN_VISIBLE_VOLT_mV (newly listed)
};

struct CurvePlanResult {
    CurvePlanDisposition disposition;
    CurvePlanEvidence evidence;
    CurvePlanRange range;
    LockMode lockMode;
    int lockCi;
    unsigned int lockMHz;
    unsigned int lockVoltUv;
    int issueCount;
    int omittedIssues;
    CurvePlanIssue issues[CURVE_PLAN_MAX_ISSUES];
    // Anchor-only reach: base(anchor) + range max, 0 when not computed.  Not a
    // feasible target by itself; the tail and prefix bound it too.
    long long anchorOnlyMaxKHz;
    // Lowest plateau the uniform-minimum tail can reach: max over the tail of
    // base + range min, 0 when not computed.
    long long tailLowerBoundKHz;
};

struct CurvePlanInput {
    const VFCurvePoint* curve;       // observed frequencies and voltages
    const int* programmedOffsetsKHz; // read with `curve`
    VfFloorTopology floor;           // the binding's flat floor
    CurvePlanEvidence evidence;
    CurvePlanRange range;
    CurvePlanTailStrategy tailStrategy;
    // Lock provenance mode for offset-intent prefix points; see
    // apply_lock_pretail_policy.h.  STORED trusts the stored MHz only.
    ApplyLockPretailProvenance provenanceMode;
    // True when the apply resets offsets to stock first, so a point the
    // request does not own lands at its stock base; false when it keeps its
    // live value.
    bool unownedAtStock;
    // The Windows apply has always refused a pinned (HARD) lock below a
    // pre-tail point too; Linux checks the prefix for FLATTEN only.
    bool prefixAppliesToHardLock;
    // Optional per-point override of `unownedAtStock` for points the request
    // does not own: true where the transaction writes a zero offset (Linux
    // releases a previously owned point that way).  Null: use the flag.
    const bool* unownedStockMask;
    const DesiredSettings* desired;
    // The lock mode the apply will actually use for `desired`'s lock (the
    // Windows apply resolves a mode-less lock to FLATTEN); NONE plans no lock.
    LockMode lockMode;
    // This request's GPU-offset component per point (kHz), already resolved
    // against the exclude-low rule; null means zero everywhere.
    const int* gpuComponentKHz;
};

static inline const char* curve_plan_disposition_name(CurvePlanDisposition d) {
    switch (d) {
        case CURVE_PLAN_REPRESENTABLE: return "representable";
        case CURVE_PLAN_REFUSED: return "refused";
        case CURVE_PLAN_NEEDS_FRESH_BASELINE: return "needs-fresh-baseline";
        case CURVE_PLAN_UNAVAILABLE: return "unavailable";
        default: return "unknown";
    }
}

static inline const char* curve_plan_reason_name(CurvePlanReason r) {
    switch (r) {
        case CURVE_PLAN_REASON_NONE: return "none";
        case CURVE_PLAN_REASON_PREFIX_ABOVE_PLATEAU: return "prefix-above-plateau";
        case CURVE_PLAN_REASON_ANCHOR_ABOVE_RANGE: return "anchor-above-range";
        case CURVE_PLAN_REASON_ANCHOR_BELOW_RANGE: return "anchor-below-range";
        case CURVE_PLAN_REASON_TAIL_UNREACHABLE: return "tail-unreachable";
        case CURVE_PLAN_REASON_POINT_OUT_OF_RANGE: return "point-out-of-range";
        case CURVE_PLAN_REASON_MISSING_POINT: return "missing-point";
        case CURVE_PLAN_REASON_FOREIGN_DOMAIN: return "foreign-domain";
        case CURVE_PLAN_REASON_FLOOR_POINT: return "flat-floor-point";
        case CURVE_PLAN_REASON_FLATTEN_UNSUPPORTED: return "flatten-unsupported";
        case CURVE_PLAN_REASON_ARITHMETIC_OVERFLOW: return "arithmetic-overflow";
        case CURVE_PLAN_REASON_READ_FAILED: return "read-failed";
        default: return "unknown";
    }
}

static inline const char* curve_plan_evidence_name(CurvePlanEvidence e) {
    switch (e) {
        case CURVE_PLAN_EVIDENCE_UNAVAILABLE: return "unavailable";
        case CURVE_PLAN_EVIDENCE_LIVE_SHAPED_ESTIMATE: return "live-shaped-estimate";
        case CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE: return "unshaped-live";
        case CURVE_PLAN_EVIDENCE_TRANSACTION_BASELINE: return "transaction-baseline";
        default: return "unknown";
    }
}

static inline const char* curve_plan_range_source_name(CurvePlanRangeSource s) {
    switch (s) {
        case CURVE_PLAN_RANGE_POLICY_FALLBACK: return "application fallback";
        case CURVE_PLAN_RANGE_NVML_GRAPHICS: return "driver graphics offset range";
        case CURVE_PLAN_RANGE_PSTATE: return "driver P-state range";
        default: return "unknown";
    }
}

// Every populated graphics offset zero and no graphics scalar shaping the
// readback: the observation is the stock curve.  `scalarKnownZero` is false
// when a separate graphics clock offset is (or may be) in effect, whose
// composition with the VF readback is not modelled here.
static inline CurvePlanEvidence curve_plan_classify_pre_write_evidence(
    const VFCurvePoint* curve, const int* programmedOffsetsKHz,
    const VfFloorTopology& floor, bool scalarKnownZero) {
    if (!curve || !programmedOffsetsKHz || !floor.valid)
        return CURVE_PLAN_EVIDENCE_UNAVAILABLE;
    if (!scalarKnownZero) return CURVE_PLAN_EVIDENCE_LIVE_SHAPED_ESTIMATE;
    for (int ci = 0; ci < floor.graphicsEnd && ci < VF_NUM_POINTS; ++ci) {
        if (curve[ci].freq_kHz != 0 && programmedOffsetsKHz[ci] != 0)
            return CURVE_PLAN_EVIDENCE_LIVE_SHAPED_ESTIMATE;
    }
    return CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE;
}

// The driver quantizes VF frequencies to bins.  A baseline-derived excess no
// larger than the local bin spacing (the distance to the nearest distinct
// neighbouring base) cannot be told apart from where the anchor itself will
// land, so it is left to the protected Apply's verification instead of being
// called proven.  Not a tolerance: the issue is still reported.
static inline long long curve_plan_quant_step_khz(const long long* baseKHz,
                                                  const VFCurvePoint* curve, int ci,
                                                  int end) {
    long long best = 0;
    for (int dir = -1; dir <= 1; dir += 2) {
        for (int j = ci + dir; j >= 0 && j < end; j += dir) {
            if (curve[j].freq_kHz == 0) continue;
            long long delta = baseKHz[j] - baseKHz[ci];
            if (delta < 0) delta = -delta;
            if (delta == 0) continue;
            if (best == 0 || delta < best) best = delta;
            break;
        }
    }
    return best > 0 ? best : 1000;
}

static inline bool curve_plan_baseline_proves(CurvePlanEvidence e) {
    return e == CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE ||
        e == CURVE_PLAN_EVIDENCE_TRANSACTION_BASELINE;
}

static inline unsigned int curve_plan_display_mhz(long long kHz) {
    return apply_lock_pretail_display_mhz(kHz);
}

// Record one issue.  The first CURVE_PLAN_MAX_ISSUES are kept; the rest only
// count.  Callers aggregate per reason first, so one issue already carries
// that reason's first and worst points.
static inline void curve_plan_add_issue(CurvePlanResult* r, const CurvePlanIssue& issue) {
    if (!r) return;
    if (r->issueCount < CURVE_PLAN_MAX_ISSUES) {
        r->issues[r->issueCount++] = issue;
    } else {
        ++r->omittedIssues;
    }
}

// Aggregates one reason across points: first by index, worst by excess.
struct CurvePlanAggregate {
    bool any;
    bool anyProven;
    CurvePlanIssue issue;
    long long worstExcessKHz;
};

static inline void curve_plan_aggregate_note(CurvePlanAggregate* a,
    CurvePlanReason reason, bool proven, int ci, const VFCurvePoint* curve,
    unsigned int targetMHz, long long baseKHz, long long adjustmentKHz,
    long long limitKHz, long long excessKHz, CurvePlanPointKind kind,
    bool inferred) {
    const unsigned int volt = curve[ci].volt_uV;
    if (!a->any) {
        a->any = true;
        a->issue = {};
        a->issue.reason = reason;
        a->issue.ci = ci;
        a->issue.voltUv = volt;
        a->issue.targetMHz = targetMHz;
        a->issue.baseKHz = baseKHz;
        a->issue.adjustmentKHz = adjustmentKHz;
        a->issue.limitKHz = limitKHz;
        a->issue.kind = kind;
        a->issue.inferredFromProgrammed = inferred;
        a->issue.belowVoltageRule = volt / 1000u < (unsigned int)MIN_VISIBLE_VOLT_mV;
        a->worstExcessKHz = LLONG_MIN;
    }
    if (proven) a->anyProven = true;
    ++a->issue.affectedCount;
    if (excessKHz > a->worstExcessKHz) {
        a->worstExcessKHz = excessKHz;
        a->issue.worstCi = ci;
        a->issue.worstVoltUv = volt;
        a->issue.worstMHz = targetMHz;
    }
}

static inline void curve_plan_aggregate_flush(CurvePlanResult* r, CurvePlanAggregate* a) {
    if (!a->any) return;
    a->issue.proven = a->anyProven;
    curve_plan_add_issue(r, a->issue);
}

// Topology issue for one point a request names (anchor or explicit target).
static inline bool curve_plan_check_point_topology(CurvePlanResult* r,
    const CurvePlanInput& in, int ci, unsigned int targetMHz) {
    const VfPointTargetStatus status = vf_point_target_status(in.floor, in.curve, ci);
    if (status == VF_POINT_TARGET_OK) return true;
    CurvePlanIssue issue = {};
    issue.reason = status == VF_POINT_TARGET_FLOOR ? CURVE_PLAN_REASON_FLOOR_POINT
        : status == VF_POINT_TARGET_FOREIGN_DOMAIN ? CURVE_PLAN_REASON_FOREIGN_DOMAIN
        : CURVE_PLAN_REASON_MISSING_POINT;
    issue.proven = true;
    issue.ci = ci;
    issue.worstCi = ci;
    issue.voltUv = (ci >= 0 && ci < VF_NUM_POINTS) ? in.curve[ci].volt_uV : 0u;
    issue.worstVoltUv = issue.voltUv;
    issue.targetMHz = targetMHz;
    issue.worstMHz = targetMHz;
    issue.limitKHz = (long long)in.floor.floorMHz * 1000LL;
    issue.affectedCount = 1;
    issue.belowVoltageRule = issue.voltUv / 1000u < (unsigned int)MIN_VISIBLE_VOLT_mV;
    curve_plan_add_issue(r, issue);
    return false;
}

// What one populated pre-anchor point will land at under this request, and
// whether that value is evidence enough to refuse.  Shared by the planner's
// prefix check and the prefix repair (curve_prefix_repair.h), so the repair
// lowers exactly the values the planner judges.
struct CurvePlanPrefixTarget {
    unsigned int targetMHz;
    CurvePlanPointKind kind;
    bool requested;  // the request owns the point (typed or projected)
    bool proven;     // before the one-bin gate, which applies to all but typed
    bool inferred;
};

static inline CurvePlanPrefixTarget curve_plan_prefix_point_target(
    const CurvePlanInput& in, long long baseKHz, int ci, bool baselineProves) {
    CurvePlanPrefixTarget t = {};
    const DesiredSettings* d = in.desired;
    const bool requested = d->hasCurvePoint[ci] && d->curvePointMHz[ci] > 0;
    const bool fromOffset = requested && d->curvePointFromGpuOffset[ci] != 0;
    const bool offsetIntent = fromOffset &&
        in.provenanceMode != APPLY_LOCK_PRETAIL_PROVENANCE_STORED;
    const long long componentKHz = in.gpuComponentKHz ? in.gpuComponentKHz[ci] : 0;
    const unsigned int baseTargetMHz = curve_plan_display_mhz(baseKHz + componentKHz);
    t.requested = requested;
    t.kind = CURVE_PLAN_POINT_ABSENT;
    if (requested && !fromOffset) {
        // Typed by the user: exact, needs no baseline.
        t.targetMHz = d->curvePointMHz[ci];
        t.kind = CURVE_PLAN_POINT_ABSOLUTE;
        t.proven = true;
    } else if (requested && !offsetIntent) {
        // STORED route: the stored MHz is judged as the established pre-tail
        // policy does (separate NVML scalar, composition not yet measured).
        t.targetMHz = d->curvePointMHz[ci];
        t.kind = CURVE_PLAN_POINT_PROJECTED;
        t.proven = true;
        t.inferred = componentKHz == 0;
    } else if (offsetIntent &&
               in.provenanceMode == APPLY_LOCK_PRETAIL_PROVENANCE_AGREEMENT) {
        // Refused only when the stored preview AND base + component agree, as
        // the established policy does over a shaped base.
        t.targetMHz = d->curvePointMHz[ci] < baseTargetMHz ? d->curvePointMHz[ci]
                                                           : baseTargetMHz;
        t.kind = CURVE_PLAN_POINT_PROJECTED;
        t.proven = true;
        t.inferred = componentKHz == 0;
    } else if (offsetIntent || (d->hasGpuOffset && componentKHz != 0)) {
        t.targetMHz = baseTargetMHz;
        t.kind = offsetIntent ? CURVE_PLAN_POINT_PROJECTED : CURVE_PLAN_POINT_ABSENT;
        t.proven = baselineProves;
        t.inferred = offsetIntent && componentKHz == 0;
    } else if (in.unownedStockMask ? in.unownedStockMask[ci] : in.unownedAtStock) {
        // Lands on its stock base after the reset.
        t.targetMHz = curve_plan_display_mhz(baseKHz);
        t.proven = baselineProves;
    } else {
        // Left at its live value: a direct observation, not a baseline.
        t.targetMHz = curve_plan_display_mhz(in.curve[ci].freq_kHz);
        t.proven = true;
        t.inferred = in.programmedOffsetsKHz[ci] != 0;
    }
    return t;
}

static inline CurvePlanResult curve_plan_evaluate(const CurvePlanInput& in) {
    CurvePlanResult r = {};
    r.evidence = in.evidence;
    r.range = in.range;
    r.lockCi = -1;
    const DesiredSettings* d = in.desired;
    if (!in.curve || !in.programmedOffsetsKHz || !d || !in.floor.valid ||
        in.evidence == CURVE_PLAN_EVIDENCE_UNAVAILABLE) {
        CurvePlanIssue issue = {};
        issue.reason = CURVE_PLAN_REASON_READ_FAILED;
        issue.ci = -1;
        issue.worstCi = -1;
        curve_plan_add_issue(&r, issue);
        r.disposition = CURVE_PLAN_UNAVAILABLE;
        return r;
    }
    const bool baselineProves = curve_plan_baseline_proves(in.evidence);
    const LockMode lockMode = in.lockMode;
    const bool hasLock = d->hasLock && d->lockMHz > 0 && d->lockCi >= 0 &&
        d->lockCi < VF_NUM_POINTS && lockMode != LOCK_MODE_NONE;
    if (hasLock) {
        r.lockMode = lockMode;
        r.lockCi = d->lockCi;
        r.lockMHz = d->lockMHz;
        r.lockVoltUv = in.curve[d->lockCi].volt_uV;
    }
    bool lockUsable = hasLock &&
        curve_plan_check_point_topology(&r, in, d->lockCi, d->lockMHz);
    if (lockUsable && lockMode == LOCK_MODE_FLATTEN &&
        in.tailStrategy == CURVE_PLAN_TAIL_UNIFORM_MIN_OFFSET) {
        const VfOffsetRange vr = {in.range.minKHz, in.range.maxKHz, true};
        if (!vf_offset_range_supports_flatten(vr)) {
            CurvePlanIssue issue = {};
            issue.reason = CURVE_PLAN_REASON_FLATTEN_UNSUPPORTED;
            issue.proven = true;
            issue.ci = d->lockCi;
            issue.worstCi = d->lockCi;
            issue.voltUv = r.lockVoltUv;
            issue.limitKHz = in.range.minKHz;
            issue.affectedCount = 1;
            curve_plan_add_issue(&r, issue);
            lockUsable = false;
        }
    }

    long long baseKHz[VF_NUM_POINTS] = {};
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        if (in.curve[ci].freq_kHz == 0) continue;
        baseKHz[ci] = curve_point_stock_base_khz(in.curve[ci].freq_kHz,
                                                 in.programmedOffsetsKHz[ci]);
    }
    const unsigned int lockMHz = d->lockMHz;
    const long long plateauKHz = (long long)lockMHz * 1000LL;
    const int end = in.floor.graphicsEnd < VF_NUM_POINTS ? in.floor.graphicsEnd : VF_NUM_POINTS;
    // A baseline-derived excess proves a refusal only beyond one bin.
    auto beyondBin = [&](int ci, long long excessKHz) {
        return excessKHz > curve_plan_quant_step_khz(baseKHz, in.curve, ci, end);
    };

    // Explicit points outside the tail: topology, then range.
    CurvePlanAggregate pointRange = {};
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        if (!d->hasCurvePoint[ci] || d->curvePointMHz[ci] == 0) continue;
        if (lockUsable && ci >= d->lockCi) continue;  // the lock owns the tail
        if (!curve_plan_check_point_topology(&r, in, ci, d->curvePointMHz[ci])) continue;
        CurvePointOffsetRequest want = {};
        want.fromGpuOffset = d->curvePointFromGpuOffset[ci] != 0;
        want.gpuOffsetComponentKHz = in.gpuComponentKHz ? in.gpuComponentKHz[ci] : 0;
        want.absoluteMHz = d->curvePointMHz[ci];
        want.liveBaseKHz = baseKHz[ci];
        const long long offset = curve_point_target_offset_khz(&want);
        long long excess = 0;
        long long limit = 0;
        if (offset > in.range.maxKHz) { excess = offset - in.range.maxKHz; limit = in.range.maxKHz; }
        else if (offset < in.range.minKHz) { excess = in.range.minKHz - offset; limit = in.range.minKHz; }
        else continue;
        // A projected offset is the request's own number; an absolute one is
        // only as good as the baseline it was subtracted from.
        const bool proven = want.fromGpuOffset || (baselineProves && beyondBin(ci, excess));
        curve_plan_aggregate_note(&pointRange, CURVE_PLAN_REASON_POINT_OUT_OF_RANGE,
            proven, ci, in.curve, d->curvePointMHz[ci], baseKHz[ci], offset, limit,
            excess, want.fromGpuOffset ? CURVE_PLAN_POINT_PROJECTED : CURVE_PLAN_POINT_ABSOLUTE,
            false);
    }

    if (lockUsable) {
        // Prefix: every populated graphics point below the anchor, including
        // points the editor does not list (contract 11).
        CurvePlanAggregate prefix = {};
        const bool checkPrefix = lockMode != LOCK_MODE_HARD || in.prefixAppliesToHardLock;
        for (int ci = 0; checkPrefix && ci < d->lockCi && ci < in.floor.graphicsEnd; ++ci) {
            if (in.curve[ci].freq_kHz == 0) continue;
            const CurvePlanPrefixTarget t =
                curve_plan_prefix_point_target(in, baseKHz[ci], ci, baselineProves);
            const unsigned int targetMHz = t.targetMHz;
            if (targetMHz <= lockMHz) continue;
            const long long prefixExcessKHz = ((long long)targetMHz - (long long)lockMHz) * 1000LL;
            bool proven = t.proven;
            if (proven && t.kind != CURVE_PLAN_POINT_ABSOLUTE)
                proven = beyondBin(ci, prefixExcessKHz);
            curve_plan_aggregate_note(&prefix, CURVE_PLAN_REASON_PREFIX_ABOVE_PLATEAU,
                proven, ci, in.curve, targetMHz, baseKHz[ci], 0, plateauKHz,
                prefixExcessKHz, t.kind, t.inferred);
        }
        curve_plan_aggregate_flush(&r, &prefix);

        // Anchor reach.
        const int a = d->lockCi;
        const long long anchorAdjust = plateauKHz - baseKHz[a];
        r.anchorOnlyMaxKHz = baseKHz[a] + in.range.maxKHz;
        if (anchorAdjust > in.range.maxKHz || anchorAdjust < in.range.minKHz) {
            CurvePlanIssue issue = {};
            const bool above = anchorAdjust > in.range.maxKHz;
            issue.reason = above ? CURVE_PLAN_REASON_ANCHOR_ABOVE_RANGE
                                 : CURVE_PLAN_REASON_ANCHOR_BELOW_RANGE;
            const long long anchorExcess = above ? anchorAdjust - in.range.maxKHz
                                                 : in.range.minKHz - anchorAdjust;
            issue.proven = baselineProves && beyondBin(a, anchorExcess);
            issue.ci = a;
            issue.worstCi = a;
            issue.voltUv = r.lockVoltUv;
            issue.worstVoltUv = r.lockVoltUv;
            issue.targetMHz = lockMHz;
            issue.worstMHz = lockMHz;
            issue.baseKHz = baseKHz[a];
            issue.adjustmentKHz = anchorAdjust;
            issue.limitKHz = above ? in.range.maxKHz : in.range.minKHz;
            issue.affectedCount = 1;
            issue.belowVoltageRule = r.lockVoltUv / 1000u < (unsigned int)MIN_VISIBLE_VOLT_mV;
            curve_plan_add_issue(&r, issue);
        }

        // Tail reach: the WHOLE populated tail, not the first mismatch.
        CurvePlanAggregate tail = {};
        for (int ci = a + 1; ci < in.floor.graphicsEnd && ci < VF_NUM_POINTS; ++ci) {
            if (in.curve[ci].freq_kHz == 0) continue;
            if (in.tailStrategy == CURVE_PLAN_TAIL_UNIFORM_MIN_OFFSET &&
                lockMode == LOCK_MODE_FLATTEN) {
                const long long lowest = baseKHz[ci] + in.range.minKHz;
                if (lowest > r.tailLowerBoundKHz) r.tailLowerBoundKHz = lowest;
                const unsigned int lowestMHz = curve_plan_display_mhz(lowest);
                if (lowestMHz <= lockMHz) continue;
                const long long tailExcess =
                    ((long long)lowestMHz - (long long)lockMHz) * 1000LL;
                curve_plan_aggregate_note(&tail, CURVE_PLAN_REASON_TAIL_UNREACHABLE,
                    baselineProves && beyondBin(ci, tailExcess), ci, in.curve,
                    lowestMHz, baseKHz[ci], in.range.minKHz, in.range.minKHz,
                    tailExcess, CURVE_PLAN_POINT_ABSENT, false);
            } else if (in.tailStrategy == CURVE_PLAN_TAIL_ABSOLUTE_TARGET) {
                const long long adjust = plateauKHz - baseKHz[ci];
                long long excess = 0;
                long long limit = 0;
                if (adjust > in.range.maxKHz) { excess = adjust - in.range.maxKHz; limit = in.range.maxKHz; }
                else if (adjust < in.range.minKHz) { excess = in.range.minKHz - adjust; limit = in.range.minKHz; }
                else continue;
                curve_plan_aggregate_note(&tail, CURVE_PLAN_REASON_TAIL_UNREACHABLE,
                    baselineProves && beyondBin(ci, excess), ci, in.curve, lockMHz,
                    baseKHz[ci], adjust, limit, excess, CURVE_PLAN_POINT_ABSENT, false);
            }
        }
        curve_plan_aggregate_flush(&r, &tail);
    }
    curve_plan_aggregate_flush(&r, &pointRange);

    bool anyProven = false;
    bool anyConditional = false;
    for (int i = 0; i < r.issueCount; ++i) {
        if (r.issues[i].proven) anyProven = true;
        else anyConditional = true;
    }
    if (r.omittedIssues > 0) anyConditional = anyConditional || !anyProven;
    r.disposition = anyProven ? CURVE_PLAN_REFUSED
        : anyConditional ? CURVE_PLAN_NEEDS_FRESH_BASELINE
        : CURVE_PLAN_REPRESENTABLE;
    return r;
}

// The issue a one-line summary leads with: the first proven one, else the
// first.
static inline const CurvePlanIssue* curve_plan_lead_issue(const CurvePlanResult& r) {
    for (int i = 0; i < r.issueCount; ++i)
        if (r.issues[i].proven) return &r.issues[i];
    return r.issueCount > 0 ? &r.issues[0] : nullptr;
}

#endif  // GREEN_CURVE_CURVE_FEASIBILITY_PLAN_H
