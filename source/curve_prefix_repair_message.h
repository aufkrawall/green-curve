// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_CURVE_PREFIX_REPAIR_MESSAGE_H
#define GREEN_CURVE_CURVE_PREFIX_REPAIR_MESSAGE_H

// User text and the `curve-prepare:` diagnostic for a CurvePrefixRepair
// (curve_prefix_repair.h).  Built from the structured result only.  The
// preview lists every changed point in physical order (lowest voltage first),
// bounded, with its old and new value and whether it becomes a typed value;
// the refusal names the limiting point by voltage first.

#include "curve_feasibility_message.h"
#include "curve_prefix_repair.h"

#define CURVE_PREFIX_REPAIR_PREVIEW_LINES 8

static inline void curve_prefix_repair_format_preview(const CurvePrefixRepair& r,
                                                      const CurvePlanResult& before,
                                                      char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = 0;
    curve_plan_appendf(out, outSize,
        "The %u MHz %s at %u mV (point %d) is below %d earlier point%s between %u and"
        " %u mV, so Apply would refuse it.\n\n"
        "Prepare compatible curve can lower %s in the editor draft:\n",
        before.lockMHz, curve_plan_lock_word(before.lockMode), before.lockVoltUv / 1000u,
        before.lockCi, r.changedCount, r.changedCount == 1 ? "" : "s",
        r.minVoltUv / 1000u, r.maxVoltUv / 1000u,
        r.changedCount == 1 ? "it" : "them");
    // points[] runs from the anchor downward; show lowest voltage first.
    int shown = 0;
    for (int k = r.changedCount - 1; k >= 0 && shown < CURVE_PREFIX_REPAIR_PREVIEW_LINES; --k) {
        const CurvePrefixRepairPoint& p = r.points[k];
        curve_plan_appendf(out, outSize, "  %u mV (point %d): %u -> %u MHz%s\n",
            p.voltUv / 1000u, p.ci, p.oldMHz, p.newMHz,
            p.wasAbsent ? " (stock now; becomes a typed value)" : "");
        ++shown;
    }
    if (r.changedCount > shown)
        curve_plan_appendf(out, outSize, "  ... and %d more\n", r.changedCount - shown);
    curve_plan_appendf(out, outSize,
        "\nThe lock point and its target stay as they are, and no point is raised.");
    if (r.after.disposition == CURVE_PLAN_NEEDS_FRESH_BASELINE)
        curve_plan_appendf(out, outSize,
            " The rest of the curve can only be confirmed during Apply, after the reset to"
            " stock.");
    curve_plan_appendf(out, outSize,
        " Only the draft changes; nothing is applied until you press Apply again.\n\n"
        "Lower these points in the draft?");
}

// Why no repair is offered, appended after the planner's own refusal text.
static inline void curve_prefix_repair_format_refusal(const CurvePrefixRepair& r,
                                                      char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = 0;
    const unsigned int mv = r.limitingVoltUv / 1000u;
    switch (r.status) {
        case CURVE_PREFIX_REPAIR_LIMITED:
            if (r.limitingFloor)
                curve_plan_appendf(out, outSize,
                    "Prepare compatible curve cannot fix this: %u mV (point %d) is inside the"
                    " driver's flat low-voltage floor (%d MHz) and cannot be lowered to %u MHz.",
                    mv, r.limitingCi, r.floorMHz, r.limitingNeededMHz);
            else
                curve_plan_appendf(out, outSize,
                    "Prepare compatible curve cannot fix this: lowering %u mV (point %d) to %u MHz"
                    " needs a %+d MHz point adjustment, beyond the permitted minimum of %+d MHz"
                    " (%s).",
                    mv, r.limitingCi, r.limitingNeededMHz,
                    curve_plan_signed_mhz(r.limitingKHz), curve_plan_signed_mhz(r.rangeMinKHz),
                    curve_plan_range_source_name(r.rangeSource));
            break;
        case CURVE_PREFIX_REPAIR_CONVERSION_REQUIRED:
            curve_plan_appendf(out, outSize,
                "Prepare compatible curve cannot fix this automatically: %u mV (point %d)"
                " follows the GPU offset instead of a typed value, and lowering it would turn"
                " it into one. Type a value of at most %u MHz for it, or lower the GPU offset.",
                mv, r.limitingCi, r.limitingNeededMHz);
            break;
        case CURVE_PREFIX_REPAIR_BASELINE_REQUIRED:
            curve_plan_appendf(out, outSize,
                "Prepare compatible curve cannot prepare a proven fix yet: %u mV (point %d) is"
                " not part of this draft, and its stock value cannot be read while the current"
                " curve carries programmed offsets. Reset to stock, then try again.",
                mv, r.limitingCi);
            break;
        case CURVE_PREFIX_REPAIR_UNRESOLVED: {
            curve_plan_appendf(out, outSize,
                "Prepare compatible curve cannot fix this: lowering the earlier points is not"
                " enough. ");
            const CurvePlanIssue* lead = curve_plan_lead_issue(r.after);
            if (lead) curve_plan_describe_issue(r.after, *lead, out, outSize);
            break;
        }
        default:
            break;
    }
    if (out[0]) curve_plan_appendf(out, outSize, " The draft was not changed.");
}

static inline void curve_prefix_repair_log(const CurvePrefixRepair& r, const char* phase,
                                           CurvePlanLogFn log) {
    if (!log) return;
    char line[320];
    snprintf(line, sizeof(line),
        "curve-prepare: phase=%s status=%s changed=%d span=%u..%u mV first=ci%d"
        " limiting=ci%d/%umV needs=%u MHz adjust=%lld kHz floor=%d after=%s\n",
        phase ? phase : "?", curve_prefix_repair_status_name(r.status), r.changedCount,
        r.minVoltUv / 1000u, r.maxVoltUv / 1000u, r.firstCi, r.limitingCi,
        r.limitingVoltUv / 1000u, r.limitingNeededMHz, r.limitingKHz,
        r.limitingFloor ? 1 : 0,
        r.status == CURVE_PREFIX_REPAIR_PREPARED || r.status == CURVE_PREFIX_REPAIR_UNRESOLVED
            ? curve_plan_disposition_name(r.after.disposition) : "-");
    log(line);
    const int maxLines = r.changedCount < CURVE_PREFIX_REPAIR_PREVIEW_LINES
        ? r.changedCount : CURVE_PREFIX_REPAIR_PREVIEW_LINES;
    for (int k = 0; k < maxLines; ++k) {
        const CurvePrefixRepairPoint& p = r.points[k];
        snprintf(line, sizeof(line),
            "curve-prepare: point ci=%d mv=%u %u->%u MHz absent=%d\n",
            p.ci, p.voltUv / 1000u, p.oldMHz, p.newMHz, p.wasAbsent ? 1 : 0);
        log(line);
    }
}

#endif  // GREEN_CURVE_CURVE_PREFIX_REPAIR_MESSAGE_H
