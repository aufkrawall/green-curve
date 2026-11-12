// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_CURVE_FEASIBILITY_MESSAGE_H
#define GREEN_CURVE_CURVE_FEASIBILITY_MESSAGE_H

// User text and the `curve-plan:` diagnostic for a CurvePlanResult
// (curve_feasibility_plan.h).  Generated from the structured result only, so
// the words cannot drift from the decision: physical voltage and clock lead,
// point indices follow, and the range always names its source.  "Nothing was
// changed." is appended only when the caller says no setter ran.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "curve_feasibility_plan.h"

static inline int curve_plan_signed_mhz(long long kHz) {
    const long long r = kHz >= 0 ? (kHz + 500) / 1000 : -((-kHz + 500) / 1000);
    if (r > INT_MAX) return INT_MAX;
    if (r < INT_MIN) return INT_MIN;
    return (int)r;
}

[[gnu::format(printf, 3, 4)]]
static inline void curve_plan_appendf(char* out, size_t outSize, const char* fmt, ...) {
    if (!out || outSize == 0) return;
    const size_t used = strlen(out);
    if (used + 1 >= outSize) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out + used, outSize - used, fmt, ap);
    va_end(ap);
}

static inline const char* curve_plan_lock_word(LockMode mode) {
    return mode == LOCK_MODE_HARD ? "pin" : "flatten";
}

// One sentence group for one issue.
static inline void curve_plan_describe_issue(const CurvePlanResult& r,
                                             const CurvePlanIssue& i,
                                             char* out, size_t outSize) {
    const unsigned int mv = i.voltUv / 1000u;
    const unsigned int worstMv = i.worstVoltUv / 1000u;
    const char* source = curve_plan_range_source_name(r.range.source);
    switch (i.reason) {
        case CURVE_PLAN_REASON_PREFIX_ABOVE_PLATEAU: {
            const char* how = i.kind == CURVE_PLAN_POINT_ABSOLUTE ? "is set to"
                : i.inferredFromProgrammed ? "keeps an offset programmed on the GPU and lands at"
                : i.kind == CURVE_PLAN_POINT_PROJECTED ? "lands at"
                : "stays at its stock";
            curve_plan_appendf(out, outSize,
                "The %u MHz %s at %u mV (point %d) is below point %d at %u mV, which %s %u MHz.",
                r.lockMHz, curve_plan_lock_word(r.lockMode), r.lockVoltUv / 1000u,
                r.lockCi, i.ci, mv, how, i.targetMHz);
            if (i.affectedCount > 1)
                curve_plan_appendf(out, outSize,
                    " %d earlier points are above the target; the highest is point %d at %u mV (%u MHz).",
                    i.affectedCount, i.worstCi, worstMv, i.worstMHz);
            if (i.belowVoltageRule)
                curve_plan_appendf(out, outSize,
                    " Points below %d mV are listed at the start of the curve.", MIN_VISIBLE_VOLT_mV);
            curve_plan_appendf(out, outSize,
                " Lower those points or raise the target.");
            break;
        }
        case CURVE_PLAN_REASON_ANCHOR_ABOVE_RANGE:
            curve_plan_appendf(out, outSize,
                "%u MHz at %u mV requires a %+d MHz point adjustment. The current permitted"
                " maximum is %+d MHz (%s). The anchor-only estimate is %u MHz; the full tail"
                " also needs validation.",
                i.targetMHz, mv, curve_plan_signed_mhz(i.adjustmentKHz),
                curve_plan_signed_mhz(i.limitKHz), source,
                curve_plan_display_mhz(r.anchorOnlyMaxKHz));
            break;
        case CURVE_PLAN_REASON_ANCHOR_BELOW_RANGE:
            curve_plan_appendf(out, outSize,
                "%u MHz at %u mV requires a %+d MHz point adjustment. The current permitted"
                " minimum is %+d MHz (%s).",
                i.targetMHz, mv, curve_plan_signed_mhz(i.adjustmentKHz),
                curve_plan_signed_mhz(i.limitKHz), source);
            break;
        case CURVE_PLAN_REASON_TAIL_UNREACHABLE:
            if (r.lockMode == LOCK_MODE_FLATTEN && i.adjustmentKHz == i.limitKHz) {
                curve_plan_appendf(out, outSize,
                    "The %u MHz flatten at %u mV cannot pull the whole tail down: with the"
                    " %+d MHz minimum (%s), point %d at %u mV stays at %u MHz or above.",
                    r.lockMHz, r.lockVoltUv / 1000u, curve_plan_signed_mhz(i.limitKHz),
                    source, i.ci, mv, i.targetMHz);
                if (i.affectedCount > 1)
                    curve_plan_appendf(out, outSize,
                        " %d tail points are affected; the worst is point %d at %u mV (%u MHz).",
                        i.affectedCount, i.worstCi, worstMv, i.worstMHz);
                if (r.tailLowerBoundKHz > 0)
                    curve_plan_appendf(out, outSize,
                        " The lowest plateau this tail allows is about %u MHz.",
                        curve_plan_display_mhz(r.tailLowerBoundKHz));
            } else {
                curve_plan_appendf(out, outSize,
                    "Holding %u MHz on point %d at %u mV needs a %+d MHz adjustment, beyond the"
                    " permitted %+d MHz (%s).",
                    r.lockMHz, i.ci, mv, curve_plan_signed_mhz(i.adjustmentKHz),
                    curve_plan_signed_mhz(i.limitKHz), source);
                if (i.affectedCount > 1)
                    curve_plan_appendf(out, outSize, " %d tail points are affected.",
                        i.affectedCount);
            }
            break;
        case CURVE_PLAN_REASON_POINT_OUT_OF_RANGE:
            curve_plan_appendf(out, outSize,
                "%u MHz at %u mV (point %d) requires a %+d MHz point adjustment, beyond the"
                " permitted %+d MHz (%s).",
                i.targetMHz, mv, i.ci, curve_plan_signed_mhz(i.adjustmentKHz),
                curve_plan_signed_mhz(i.limitKHz), source);
            if (i.affectedCount > 1)
                curve_plan_appendf(out, outSize, " %d points are affected; the worst is point %d at %u mV.",
                    i.affectedCount, i.worstCi, worstMv);
            break;
        case CURVE_PLAN_REASON_MISSING_POINT:
            curve_plan_appendf(out, outSize,
                "Point %d is not a populated graphics VF point on this GPU.", i.ci);
            break;
        case CURVE_PLAN_REASON_FOREIGN_DOMAIN:
            curve_plan_appendf(out, outSize,
                "Point %d belongs to another clock domain, not the graphics VF curve.", i.ci);
            break;
        case CURVE_PLAN_REASON_FLOOR_POINT:
            curve_plan_appendf(out, outSize,
                "Point %d at %u mV is inside the driver's flat low-voltage floor (%d MHz);"
                " writes there are clamped back to the floor.",
                i.ci, mv, curve_plan_signed_mhz(i.limitKHz));
            break;
        case CURVE_PLAN_REASON_FLATTEN_UNSUPPORTED:
            curve_plan_appendf(out, outSize,
                "This driver's VF offset range (%s) cannot flatten the curve.", source);
            break;
        case CURVE_PLAN_REASON_ARITHMETIC_OVERFLOW:
            curve_plan_appendf(out, outSize, "The curve request is out of numeric range.");
            break;
        case CURVE_PLAN_REASON_POINT_INTENT_CONFLICT:
            curve_plan_appendf(out, outSize,
                "VF point %d has contradictory tuning intent or overlaps the lock tail.", i.ci);
            break;
        case CURVE_PLAN_REASON_POINT_GRID:
            curve_plan_appendf(out, outSize,
                "Captured VF point %d needs an explicit precision conversion for this write surface.", i.ci);
            break;
        case CURVE_PLAN_REASON_READ_FAILED:
        default:
            curve_plan_appendf(out, outSize, "The VF curve could not be read.");
            break;
    }
}

// The whole message: the lead issue, a count of the rest, then the outcome.
static inline void curve_plan_format_message(const CurvePlanResult& r,
                                             bool nothingChanged,
                                             char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = 0;
    const CurvePlanIssue* lead = curve_plan_lead_issue(r);
    if (!lead) {
        curve_plan_appendf(out, outSize, "The curve request passed its checks.");
        return;
    }
    curve_plan_describe_issue(r, *lead, out, outSize);
    const int others = r.issueCount - 1 + r.omittedIssues;
    if (others > 0)
        curve_plan_appendf(out, outSize, " %d further constraint%s also fail%s.",
            others, others == 1 ? "" : "s", others == 1 ? "s" : "");
    if (r.disposition == CURVE_PLAN_NEEDS_FRESH_BASELINE)
        curve_plan_appendf(out, outSize,
            " This is an estimate from a curve that still carries programmed offsets;"
            " full validation requires the normal protected Apply.");
    if (nothingChanged) curve_plan_appendf(out, outSize, " Nothing was changed.");
}

// One bounded summary line plus one line per issue (at most
// CURVE_PLAN_MAX_ISSUES), for the platform's own logger.
typedef void (*CurvePlanLogFn)(const char* line);

static inline void curve_plan_log(const CurvePlanResult& r, const char* phase,
                                  CurvePlanLogFn log) {
    if (!log) return;
    char line[384];
    snprintf(line, sizeof(line),
        "curve-plan: phase=%s disposition=%s evidence=%s lock=%s ci=%d mhz=%u mv=%u"
        " range=%d..%d kHz source=%s issues=%d omitted=%d anchorOnlyMax=%lld kHz"
        " tailLowerBound=%lld kHz\n",
        phase ? phase : "?", curve_plan_disposition_name(r.disposition),
        curve_plan_evidence_name(r.evidence), lock_mode_name(r.lockMode), r.lockCi,
        r.lockMHz, r.lockVoltUv / 1000u, r.range.minKHz, r.range.maxKHz,
        curve_plan_range_source_name(r.range.source), r.issueCount, r.omittedIssues,
        r.anchorOnlyMaxKHz, r.tailLowerBoundKHz);
    log(line);
    for (int k = 0; k < r.issueCount; ++k) {
        const CurvePlanIssue& i = r.issues[k];
        snprintf(line, sizeof(line),
            "curve-plan: issue=%s proven=%d first=ci%d/%umV target=%u MHz base=%lld kHz"
            " adjust=%lld kHz limit=%lld kHz worst=ci%d/%umV/%u MHz count=%d kind=%d"
            " inferred=%d below700=%d\n",
            curve_plan_reason_name(i.reason), i.proven ? 1 : 0, i.ci, i.voltUv / 1000u,
            i.targetMHz, i.baseKHz, i.adjustmentKHz, i.limitKHz, i.worstCi,
            i.worstVoltUv / 1000u, i.worstMHz, i.affectedCount, (int)i.kind,
            i.inferredFromProgrammed ? 1 : 0, i.belowVoltageRule ? 1 : 0);
        log(line);
    }
}

#endif  // GREEN_CURVE_CURVE_FEASIBILITY_MESSAGE_H
