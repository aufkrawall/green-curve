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
    CURVE_PLAN_REASON_POINT_INTENT_CONFLICT = 12,
    CURVE_PLAN_REASON_POINT_GRID = 13,
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
    CURVE_PLAN_POINT_CAPTURED = 3,
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
    long long adjustmentHz;     // exact physical adjustment, including sub-kHz remainder
    long long targetHz, baseHz, limitHz, worstHz;
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
    long long lockHz, anchorOnlyMaxHz, tailLowerBoundHz;
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
    int offsetGridHz; // 0 = the legacy physical-kHz interface
    // Matching exact arrays for this observation, or both null. Never rebuild
    // fractional baselines from display caches for precise absolute authority.
    const long long* frequencyHz;
    const long long* programmedOffsetsHz;
    const long long* gpuComponentHz;
    int lockGridHz; // named hard-pin target grid; 0 means unknown
};

#include "curve_plan_precision.h"

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
        case CURVE_PLAN_REASON_POINT_INTENT_CONFLICT: return "point-intent-conflict";
        case CURVE_PLAN_REASON_POINT_GRID: return "point-grid";
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
    const VfFloorTopology& floor, bool scalarKnownZero, const long long* programmedOffsetsHz = nullptr) {
    if (!curve || !programmedOffsetsKHz || !floor.valid)
        return CURVE_PLAN_EVIDENCE_UNAVAILABLE;
    if (!scalarKnownZero) return CURVE_PLAN_EVIDENCE_LIVE_SHAPED_ESTIMATE;
    for (int ci = 0; ci < floor.graphicsEnd && ci < VF_NUM_POINTS; ++ci) {
        if (curve[ci].freq_kHz != 0 && (programmedOffsetsHz ? programmedOffsetsHz[ci] != 0 : programmedOffsetsKHz[ci] != 0))
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
    return curve_plan_neighbour_step(baseKHz, curve, ci, end, 1000);
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
    long long worstExcessHz;
};

static inline void curve_plan_aggregate_note_hz(CurvePlanAggregate* a,
    CurvePlanReason reason, bool proven, int ci, const VFCurvePoint* curve,
    long long targetHz, long long baseHz, long long adjustmentHz,
    long long limitHz, long long excessHz, CurvePlanPointKind kind, bool inferred) {
    const unsigned int volt = curve[ci].volt_uV;
    if (!a->any) {
        a->any = true;
        a->issue = {};
        a->issue.reason = reason;
        a->issue.ci = ci;
        a->issue.voltUv = volt;
        a->issue.targetMHz = curve_plan_display_hz(targetHz);
        a->issue.targetHz = targetHz;
        a->issue.baseKHz = baseHz / 1000;
        a->issue.baseHz = baseHz;
        a->issue.adjustmentKHz = adjustmentHz / 1000;
        a->issue.adjustmentHz = adjustmentHz;
        a->issue.limitKHz = limitHz / 1000;
        a->issue.limitHz = limitHz;
        a->issue.kind = kind;
        a->issue.inferredFromProgrammed = inferred;
        a->issue.belowVoltageRule = volt / 1000u < (unsigned int)MIN_VISIBLE_VOLT_mV;
        a->worstExcessHz = LLONG_MIN;
    }
    if (proven) a->anyProven = true;
    ++a->issue.affectedCount;
    if (excessHz > a->worstExcessHz) {
        a->worstExcessHz = excessHz;
        a->issue.worstCi = ci;
        a->issue.worstVoltUv = volt;
        a->issue.worstMHz = curve_plan_display_hz(targetHz);
        a->issue.worstHz = targetHz;
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
    long long targetHz;
    CurvePlanPointKind kind;
    bool requested;  // the request owns the point (typed or projected)
    bool proven;     // before the one-bin gate, which applies to all but typed
    bool inferred;
};

static inline CurvePlanPrefixTarget curve_plan_prefix_point_target(
    const CurvePlanInput& in, long long baseKHz, int ci, bool baselineProves) {
    CurvePlanPrefixTarget t = {};
    const DesiredSettings* d = in.desired;
    const long long baseHz = in.frequencyHz ? curve_plan_base_hz(in, ci) : baseKHz * 1000;
    const long long componentHz = curve_plan_component_hz(in, ci);
    const long long baseTargetHz = baseHz + componentHz;
    const bool requested = desired_curve_point_present(d, ci);
    const bool fromOffset = requested && d->curvePointFromGpuOffset[ci] != 0;
    const bool offsetIntent = fromOffset && in.provenanceMode != APPLY_LOCK_PRETAIL_PROVENANCE_STORED;
    t.requested = requested;
    if (d->curvePointCaptured[ci]) {
        t.kind = CURVE_PLAN_POINT_CAPTURED;
        t.targetHz = baseHz + d->curvePointCapturedOffsetHz[ci];
        t.proven = baselineProves;
    } else if (requested && !fromOffset) {
        t.targetHz = curve_plan_absolute_hz(d, ci);
        t.kind = CURVE_PLAN_POINT_ABSOLUTE;
        t.proven = true;
    } else if (requested && !offsetIntent) {
        // The stored projection retains the established separate-scalar rule.
        t.targetHz = (long long)d->curvePointMHz[ci] * 1000000;
        t.kind = CURVE_PLAN_POINT_PROJECTED;
        t.proven = true;
        t.inferred = componentHz == 0;
    } else if (offsetIntent && in.provenanceMode == APPLY_LOCK_PRETAIL_PROVENANCE_AGREEMENT) {
        const long long storedHz = (long long)d->curvePointMHz[ci] * 1000000;
        t.targetHz = storedHz < baseTargetHz ? storedHz : baseTargetHz;
        t.kind = CURVE_PLAN_POINT_PROJECTED;
        t.proven = true;
        t.inferred = componentHz == 0;
    } else if (offsetIntent || (d->hasGpuOffset && componentHz != 0)) {
        t.targetHz = baseTargetHz;
        t.kind = offsetIntent ? CURVE_PLAN_POINT_PROJECTED : CURVE_PLAN_POINT_ABSENT;
        t.proven = baselineProves;
        t.inferred = offsetIntent && componentHz == 0;
    } else if (in.unownedStockMask ? in.unownedStockMask[ci] : in.unownedAtStock) {
        t.targetHz = baseHz;
        t.proven = baselineProves;
    } else {
        t.targetHz = in.frequencyHz ? in.frequencyHz[ci] : (long long)in.curve[ci].freq_kHz * 1000;
        t.proven = true;
        t.inferred = in.programmedOffsetsKHz[ci] != 0;
    }
    if (t.targetHz < 0) t.targetHz = 0;
    t.targetMHz = curve_plan_display_hz(t.targetHz);
    return t;
}

static inline void curve_plan_grid_issue(CurvePlanResult* r, const CurvePlanInput& in,
    int ci, long long targetHz, long long adjustmentHz, int gridHz, bool proven) {
    CurvePlanIssue issue{};
    issue.reason = CURVE_PLAN_REASON_POINT_GRID;
    issue.proven = proven;
    issue.ci = issue.worstCi = ci;
    issue.voltUv = issue.worstVoltUv = in.curve[ci].volt_uV;
    issue.targetHz = issue.worstHz = targetHz;
    issue.targetMHz = issue.worstMHz = curve_plan_display_hz(targetHz);
    issue.adjustmentHz = adjustmentHz;
    issue.adjustmentKHz = adjustmentHz / 1000;
    issue.limitHz = gridHz;
    issue.affectedCount = 1;
    curve_plan_add_issue(r, issue);
}

static inline CurvePlanResult curve_plan_evaluate(const CurvePlanInput& in) {
    CurvePlanResult r = {};
    r.evidence = in.evidence;
    r.range = in.range;
    r.lockCi = -1;
    const DesiredSettings* d = in.desired;
    int invalidCi = -1;
    if (d && (!desired_curve_intent_valid(d, &invalidCi) || !desired_authoring_valid(d))) {
        CurvePlanIssue issue{};
        issue.reason = CURVE_PLAN_REASON_POINT_INTENT_CONFLICT;
        issue.proven = true;
        issue.ci = issue.worstCi = invalidCi;
        issue.affectedCount = 1;
        curve_plan_add_issue(&r, issue);
        r.disposition = CURVE_PLAN_REFUSED;
        return r;
    }
    if (!in.curve || !in.programmedOffsetsKHz || !d || !in.floor.valid ||
        in.evidence == CURVE_PLAN_EVIDENCE_UNAVAILABLE || !curve_plan_exact_samples_valid(in)) {
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
    const long long plateauHz = curve_plan_lock_hz(d);
    const bool precise = curve_plan_uses_precise_values(d);
    const bool hasLock = d->hasLock && plateauHz > 0 && d->lockCi >= 0 &&
        d->lockCi < VF_NUM_POINTS && lockMode != LOCK_MODE_NONE;
    if (hasLock) {
        r.lockMode = lockMode;
        r.lockCi = d->lockCi;
        r.lockMHz = d->lockMHz;
        r.lockHz = plateauHz;
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

    long long baseHz[VF_NUM_POINTS] = {};
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci)
        if (in.curve[ci].freq_kHz) baseHz[ci] = curve_plan_base_hz(in, ci);
    const long long minimumHz = (long long)in.range.minKHz * 1000;
    const long long maximumHz = (long long)in.range.maxKHz * 1000;
    const int end = in.floor.graphicsEnd < VF_NUM_POINTS ? in.floor.graphicsEnd : VF_NUM_POINTS;
    const int gridHz = in.offsetGridHz > 0 ? in.offsetGridHz : 1000;
    auto beyondBin = [&](int ci, long long excessHz) {
        return excessHz > curve_plan_neighbour_step(baseHz, in.curve, ci, end, 1000000);
    };

    CurvePlanAggregate pointRange = {};
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        if (!desired_curve_point_has_target(d, ci)) continue;
        if (lockUsable && ci >= d->lockCi) continue;
        const long long targetHz = curve_plan_absolute_hz(d, ci);
        if (!curve_plan_check_point_topology(&r, in, ci, curve_plan_display_hz(targetHz))) continue;
        long long adjustmentHz = 0;
        if (!desired_curve_point_offset_from_base_hz(d, ci, baseHz[ci], curve_plan_component_hz(in, ci), &adjustmentHz)) {
            CurvePlanIssue issue{};
            issue.reason = CURVE_PLAN_REASON_ARITHMETIC_OVERFLOW;
            issue.proven = true; issue.ci = issue.worstCi = ci; issue.affectedCount = 1;
            curve_plan_add_issue(&r, issue);
            continue;
        }
        const bool offsetIntent = d->curvePointCaptured[ci] || d->curvePointFromGpuOffset[ci];
        if (adjustmentHz % gridHz) {
            curve_plan_grid_issue(&r, in, ci, targetHz, adjustmentHz, gridHz, offsetIntent || baselineProves);
            continue;
        }
        long long excessHz = 0, limitHz = 0;
        if (adjustmentHz > maximumHz) { excessHz = adjustmentHz - maximumHz; limitHz = maximumHz; }
        else if (adjustmentHz < minimumHz) { excessHz = minimumHz - adjustmentHz; limitHz = minimumHz; }
        else continue;
        const bool proven = offsetIntent || (baselineProves && beyondBin(ci, excessHz));
        curve_plan_aggregate_note_hz(&pointRange, CURVE_PLAN_REASON_POINT_OUT_OF_RANGE,
            proven, ci, in.curve, targetHz, baseHz[ci], adjustmentHz, limitHz,
            excessHz, d->curvePointCaptured[ci] ? CURVE_PLAN_POINT_CAPTURED :
                offsetIntent ? CURVE_PLAN_POINT_PROJECTED : CURVE_PLAN_POINT_ABSOLUTE, false);
    }

    if (lockUsable) {
        CurvePlanAggregate prefix = {};
        const bool checkPrefix = lockMode != LOCK_MODE_HARD || in.prefixAppliesToHardLock;
        for (int ci = 0; checkPrefix && ci < d->lockCi && ci < in.floor.graphicsEnd; ++ci) {
            if (!in.curve[ci].freq_kHz) continue;
            const CurvePlanPrefixTarget t = curve_plan_prefix_point_target(in, baseHz[ci] / 1000, ci, baselineProves);
            // Legacy verification keeps its established display-bin rule;
            // precise authority never acquires a display-rounding tolerance.
            const long long targetHz = precise ? t.targetHz : (long long)t.targetMHz * 1000000;
            if (targetHz <= plateauHz) continue;
            const long long excessHz = targetHz - plateauHz;
            bool proven = t.proven;
            if (proven && t.kind != CURVE_PLAN_POINT_ABSOLUTE) proven = beyondBin(ci, excessHz);
            curve_plan_aggregate_note_hz(&prefix, CURVE_PLAN_REASON_PREFIX_ABOVE_PLATEAU,
                proven, ci, in.curve, targetHz, baseHz[ci], 0, plateauHz,
                excessHz, t.kind, t.inferred);
        }
        curve_plan_aggregate_flush(&r, &prefix);

        const int a = d->lockCi;
        const long long adjustmentHz = plateauHz - baseHz[a];
        r.anchorOnlyMaxHz = baseHz[a] + maximumHz;
        r.anchorOnlyMaxKHz = r.anchorOnlyMaxHz / 1000;
        if (adjustmentHz % gridHz)
            curve_plan_grid_issue(&r, in, a, plateauHz, adjustmentHz, gridHz, baselineProves);
        if (d->authoring.lockPrecise && lockMode == LOCK_MODE_HARD &&
            in.lockGridHz > 0 && plateauHz % in.lockGridHz)
            curve_plan_grid_issue(&r, in, a, plateauHz, plateauHz, in.lockGridHz, true);
        if (adjustmentHz > maximumHz || adjustmentHz < minimumHz) {
            CurvePlanIssue issue = {};
            const bool above = adjustmentHz > maximumHz;
            issue.reason = above ? CURVE_PLAN_REASON_ANCHOR_ABOVE_RANGE : CURVE_PLAN_REASON_ANCHOR_BELOW_RANGE;
            const long long excessHz = above ? adjustmentHz - maximumHz : minimumHz - adjustmentHz;
            issue.proven = baselineProves && beyondBin(a, excessHz);
            issue.ci = issue.worstCi = a;
            issue.voltUv = issue.worstVoltUv = r.lockVoltUv;
            issue.targetMHz = issue.worstMHz = curve_plan_display_hz(plateauHz);
            issue.targetHz = issue.worstHz = plateauHz;
            issue.baseKHz = baseHz[a] / 1000; issue.baseHz = baseHz[a];
            issue.adjustmentKHz = adjustmentHz / 1000; issue.adjustmentHz = adjustmentHz;
            issue.limitHz = above ? maximumHz : minimumHz; issue.limitKHz = issue.limitHz / 1000;
            issue.affectedCount = 1;
            issue.belowVoltageRule = r.lockVoltUv / 1000u < (unsigned int)MIN_VISIBLE_VOLT_mV;
            curve_plan_add_issue(&r, issue);
        }

        CurvePlanAggregate tail = {};
        for (int ci = a + 1; ci < in.floor.graphicsEnd && ci < VF_NUM_POINTS; ++ci) {
            if (!in.curve[ci].freq_kHz) continue;
            if (in.tailStrategy == CURVE_PLAN_TAIL_UNIFORM_MIN_OFFSET && lockMode == LOCK_MODE_FLATTEN) {
                const long long lowestHz = baseHz[ci] + minimumHz;
                if (lowestHz > r.tailLowerBoundHz) r.tailLowerBoundHz = lowestHz;
                r.tailLowerBoundKHz = r.tailLowerBoundHz / 1000;
                const long long comparedHz = precise ? lowestHz : (long long)curve_plan_display_hz(lowestHz) * 1000000;
                if (comparedHz <= plateauHz) continue;
                const long long excessHz = comparedHz - plateauHz;
                curve_plan_aggregate_note_hz(&tail, CURVE_PLAN_REASON_TAIL_UNREACHABLE,
                    baselineProves && beyondBin(ci, excessHz), ci, in.curve,
                    lowestHz, baseHz[ci], minimumHz, minimumHz, excessHz, CURVE_PLAN_POINT_ABSENT, false);
            } else if (in.tailStrategy == CURVE_PLAN_TAIL_ABSOLUTE_TARGET) {
                const long long targetAdjustmentHz = plateauHz - baseHz[ci];
                if (targetAdjustmentHz % gridHz)
                    curve_plan_grid_issue(&r, in, ci, plateauHz, targetAdjustmentHz, gridHz, baselineProves);
                long long excessHz = 0, limitHz = 0;
                if (targetAdjustmentHz > maximumHz) { excessHz = targetAdjustmentHz - maximumHz; limitHz = maximumHz; }
                else if (targetAdjustmentHz < minimumHz) { excessHz = minimumHz - targetAdjustmentHz; limitHz = minimumHz; }
                else continue;
                curve_plan_aggregate_note_hz(&tail, CURVE_PLAN_REASON_TAIL_UNREACHABLE,
                    baselineProves && beyondBin(ci, excessHz), ci, in.curve, plateauHz,
                    baseHz[ci], targetAdjustmentHz, limitHz, excessHz, CURVE_PLAN_POINT_ABSENT, false);
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
