// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// "Prepare compatible curve", operation A (curve_prefix_repair.h and its
// message header), assertion codes 7900-7939.  HELP-01: a full backward
// repair over several conflicting points, hidden (< 700 mV) ones included,
// that never raises a point and keeps the anchor and plateau.  HELP-02: a
// point that cannot come down (range minimum, flat floor) refuses the whole
// repair with its identity and leaves the draft untouched.  Projected points
// report conversion required (step 3a), a shaped estimate reports that a
// baseline is needed, and a repair that cannot fix an independent tail is not
// offered.  The GUI applies an accepted repair as typed points, so the
// repaired request is re-planned here exactly as Apply would plan it.

#include <stdio.h>
#include <string.h>

#include "curve_plan_fixtures.h"
#include "curve_prefix_repair_message.h"

namespace {

CurvePrefixRepair repair_windows(const Table& t, const DesiredSettings& d,
                                 CurvePlanEvidence evidence, CurvePlanRange range,
                                 const int* componentKHz = nullptr) {
    CurvePlanInput in = plan_input_windows(t, d, evidence, range);
    in.gpuComponentKHz = componentKHz;
    return curve_prefix_repair_prepare(in);
}

const CurvePrefixRepairPoint* find_point(const CurvePrefixRepair& r, int ci) {
    for (int k = 0; k < r.changedCount; ++k)
        if (r.points[k].ci == ci) return &r.points[k];
    return nullptr;
}

bool draft_unchanged(const CurvePrefixRepair& r, const DesiredSettings& d) {
    return memcmp(&r.repaired, &d, sizeof(d)) == 0;
}

int g_logLines = 0;
char g_logFirst[320];
void capture_log(const char* line) {
    if (g_logLines++ == 0) snprintf(g_logFirst, sizeof(g_logFirst), "%s", line);
}

int run_help01_cases() {
    // HELP-01 (7900-7909): Pascal at stock, an old slot's 1000 MHz flatten at
    // point 67 (1125 mV), a typed 950 MHz at point 40, a typed 1400 MHz at
    // point 30 and a typed 700 MHz at point 10.  Points 41..66 come down to
    // the plateau, 20..39 to the typed 950 above them (point 30 included),
    // 19 and below already fit.  Points 20..24 are below 700 mV.
    Table p;
    pascal_stock(&p);
    DesiredSettings d = flatten_request(67, 1000);
    d.hasCurvePoint[40] = true;  d.curvePointMHz[40] = 950;
    d.hasCurvePoint[30] = true;  d.curvePointMHz[30] = 1400;
    d.hasCurvePoint[10] = true;  d.curvePointMHz[10] = 700;
    const CurvePlanRange range = nvml_range(-1000, 1000);
    const CurvePlanResult before = plan_windows(p, d, CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE, range);
    const CurvePlanIssue* prefix = find_issue(before, CURVE_PLAN_REASON_PREFIX_ABOVE_PLATEAU);
    if (before.disposition != CURVE_PLAN_REFUSED || !prefix || !prefix->proven) return 7900;

    const CurvePrefixRepair r = repair_windows(p, d, CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE, range);
    if (r.status != CURVE_PREFIX_REPAIR_PREPARED || r.changedCount != 46) return 7901;
    if (r.firstCi != 20 || r.minVoltUv != 637500 || r.maxVoltUv != pascal_volt_uv(66)) return 7902;
    // No point is raised; the anchor and plateau stay.
    for (int k = 0; k < r.changedCount; ++k)
        if (r.points[k].newMHz >= r.points[k].oldMHz) return 7903;
    if (!r.repaired.hasLock || r.repaired.lockCi != 67 || r.repaired.lockMHz != 1000 ||
        r.repaired.lockMode != LOCK_MODE_FLATTEN) return 7903;
    // The backward bound: plateau above the typed 950, the typed 950 below it.
    const CurvePrefixRepairPoint* p66 = find_point(r, 66);
    const CurvePrefixRepairPoint* p30 = find_point(r, 30);
    const CurvePrefixRepairPoint* p22 = find_point(r, 22);
    const CurvePrefixRepairPoint* p20 = find_point(r, 20);
    if (!p66 || p66->newMHz != 1000 || p66->oldMHz != 1955 || !p66->wasAbsent) return 7904;
    if (!p30 || p30->newMHz != 950 || p30->oldMHz != 1400 || p30->wasAbsent) return 7904;
    if (!p22 || p22->newMHz != 950 || p22->oldMHz != 1050 || !p22->wasAbsent) return 7904;
    if (!p20 || p20->newMHz != 950 || p20->oldMHz != 975) return 7904;
    if (find_point(r, 40) || find_point(r, 19) || find_point(r, 10)) return 7904;
    // Hidden points are repaired like listed ones.
    if (vf_point_visible_in_editor(p.curve[22].freq_kHz, p.curve[22].volt_uV)) return 7905;
    // The repaired draft owns typed targets only, in non-decreasing order
    // (the editor's capture rule), and untouched points keep their kind.
    unsigned int previous = 0;
    for (int ci = 0; ci < 67; ++ci) {
        if (!r.repaired.hasCurvePoint[ci]) continue;
        if (r.repaired.curvePointFromGpuOffset[ci]) return 7906;
        if (r.repaired.curvePointMHz[ci] < previous) return 7906;
        previous = r.repaired.curvePointMHz[ci];
    }
    if (r.repaired.curvePointMHz[10] != 700 || r.repaired.curvePointMHz[40] != 950 ||
        r.repaired.hasCurvePoint[19]) return 7906;
    // Apply plans the accepted draft as representable, before and after the reset.
    if (r.after.disposition != CURVE_PLAN_REPRESENTABLE) return 7907;
    if (plan_windows(p, r.repaired, CURVE_PLAN_EVIDENCE_TRANSACTION_BASELINE, range)
            .disposition != CURVE_PLAN_REPRESENTABLE) return 7907;
    // The preview: count, span, lowest voltage first, bounded, kind change named.
    char message[2048] = {};
    curve_prefix_repair_format_preview(r, before, message, sizeof(message));
    if (!strstr(message, "1000 MHz flatten at 1125 mV (point 67) is below 46 earlier points"
                         " between 637 and 1114 mV") ||
        !strstr(message, "  637 mV (point 20): 975 -> 950 MHz (stock now; becomes a typed value)\n") ||
        !strstr(message, "... and 38 more") ||
        !strstr(message, "Only the draft changes")) return 7908;
    if (strstr(message, "(point 30)")) return 7908;  // ninth line onward is summarized
    // The diagnostic: one summary line plus at most eight point lines.
    g_logLines = 0;
    curve_prefix_repair_log(r, "test", capture_log);
    if (g_logLines != 1 + CURVE_PREFIX_REPAIR_PREVIEW_LINES ||
        !strstr(g_logFirst, "curve-prepare: phase=test status=prepared changed=46") ||
        !strstr(g_logFirst, "after=representable")) return 7909;
    return 0;
}

int run_help02_cases() {
    // HELP-02 (7910-7915): a range minimum the far prefix cannot reach.
    // Point 66 (1114 mV, stock 1955 MHz) would need -955 MHz under a -500 MHz
    // minimum: refused with that identity, the draft untouched.
    Table p;
    pascal_stock(&p);
    const DesiredSettings d = flatten_request(67, 1000);
    CurvePrefixRepair r = repair_windows(p, d, CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE,
                                         nvml_range(-500, 1000));
    if (r.status != CURVE_PREFIX_REPAIR_LIMITED || r.limitingCi != 66 ||
        r.limitingFloor || r.limitingNeededMHz != 1000 || r.limitingKHz != -955321 ||
        r.rangeMinKHz != -500000) return 7910;
    if (!draft_unchanged(r, d)) return 7911;
    char message[768] = {};
    curve_prefix_repair_format_refusal(r, message, sizeof(message));
    if (!strstr(message, "lowering 1114 mV (point 66) to 1000 MHz needs a -955 MHz point"
                         " adjustment, beyond the permitted minimum of -500 MHz") ||
        !strstr(message, "The draft was not changed.")) return 7912;

    // A point inside the driver's flat floor cannot come down at all: an RTX
    // 5070 floor (180 MHz over 0..44) under a 150 MHz flatten at point 50.
    // Points 45..49 could be lowered, point 44 (725 mV) cannot.
    Table rtx;
    rtx_stock(&rtx, 45);
    const DesiredSettings low = flatten_request(50, 150);
    if (plan_windows(rtx, low, CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE, nvml_range(-1000, 1000))
            .disposition != CURVE_PLAN_REFUSED) return 7913;
    r = repair_windows(rtx, low, CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE, nvml_range(-1000, 1000));
    if (r.status != CURVE_PREFIX_REPAIR_LIMITED || r.limitingCi != 44 || !r.limitingFloor ||
        r.floorMHz != 180) return 7914;
    if (!draft_unchanged(r, low)) return 7914;
    curve_prefix_repair_format_refusal(r, message, sizeof(message));
    if (!strstr(message, "725 mV (point 44) is inside the driver's flat low-voltage floor"
                         " (180 MHz) and cannot be lowered to 150 MHz")) return 7915;

    // A point that cannot come down outranks one that only needs a decision:
    // a projected point nearer the anchor does not hide the floor.
    DesiredSettings mixed = low;
    mixed.hasCurvePoint[49] = true;
    mixed.curvePointMHz[49] = 542;
    mixed.curvePointFromGpuOffset[49] = true;
    r = repair_windows(rtx, mixed, CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE, nvml_range(-1000, 1000));
    if (r.status != CURVE_PREFIX_REPAIR_LIMITED || r.limitingCi != 44 || !r.limitingFloor ||
        !draft_unchanged(r, mixed)) return 7915;
    return 0;
}

int run_decision_cases() {
    Table p;
    pascal_stock(&p);
    const CurvePlanRange range = nvml_range(-1000, 1000);
    char message[768] = {};

    // 7920-7922: a projected (offset-intent) point that must come down is
    // reported as needing conversion, never converted silently (step 3a).
    DesiredSettings projected = flatten_request(67, 1000);
    projected.hasCurvePoint[30] = true;
    projected.curvePointMHz[30] = 1400;
    projected.curvePointFromGpuOffset[30] = true;
    CurvePrefixRepair r = repair_windows(p, projected, CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE, range);
    if (r.status != CURVE_PREFIX_REPAIR_CONVERSION_REQUIRED || r.limitingCi != 30 ||
        r.limitingNeededMHz != 1000) return 7920;
    if (!draft_unchanged(r, projected)) return 7921;
    curve_prefix_repair_format_refusal(r, message, sizeof(message));
    if (!strstr(message, "follows the GPU offset") ||
        !strstr(message, "at most 1000 MHz")) return 7921;
    // An unowned point the GPU offset shifts is the same decision.
    DesiredSettings covered = flatten_request(67, 1000);
    covered.hasGpuOffset = true;
    covered.gpuOffsetMHz = 50;
    int component[VF_NUM_POINTS];
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) component[ci] = 50000;
    r = repair_windows(p, covered, CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE, range, component);
    if (r.status != CURVE_PREFIX_REPAIR_CONVERSION_REQUIRED || r.limitingCi != 66) return 7922;

    // 7925-7926: over a shaped estimate an unowned point's stock value is
    // not known, so no proven repair is prepared; a typed point still proves
    // the refusal itself.
    DesiredSettings typed = flatten_request(67, 1000);
    typed.hasCurvePoint[30] = true;
    typed.curvePointMHz[30] = 1400;
    if (plan_windows(p, typed, CURVE_PLAN_EVIDENCE_LIVE_SHAPED_ESTIMATE, range).disposition !=
        CURVE_PLAN_REFUSED) return 7925;
    r = repair_windows(p, typed, CURVE_PLAN_EVIDENCE_LIVE_SHAPED_ESTIMATE, range);
    if (r.status != CURVE_PREFIX_REPAIR_BASELINE_REQUIRED || r.limitingCi != 66 ||
        !draft_unchanged(r, typed)) return 7926;
    curve_prefix_repair_format_refusal(r, message, sizeof(message));
    if (!strstr(message, "Reset to stock, then try again.")) return 7926;

    // 7928-7930: a prefix repair is not advertised as fixing an independent
    // tail.  60 points from 700 mV, stock 800 + 20 MHz per point, a 1000 MHz
    // flatten at point 30 under a -450 MHz minimum: 11..29 could come down,
    // but point 59 stays at 1530 MHz.
    Table lin;
    linear_table(&lin, 60, 700000, 10000);
    for (int i = 0; i < 60; ++i) lin.curve[i].freq_kHz = 800000u + (unsigned int)i * 20000u;
    const DesiredSettings tail = flatten_request(30, 1000);
    r = repair_windows(lin, tail, CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE, nvml_range(-450, 1000));
    if (r.status != CURVE_PREFIX_REPAIR_UNRESOLVED || r.changedCount != 19) return 7928;
    if (!draft_unchanged(r, tail) || !find_issue(r.after, CURVE_PLAN_REASON_TAIL_UNREACHABLE) ||
        find_issue(r.after, CURVE_PLAN_REASON_PREFIX_ABOVE_PLATEAU)) return 7929;
    curve_prefix_repair_format_refusal(r, message, sizeof(message));
    if (!strstr(message, "lowering the earlier points is not enough") ||
        !strstr(message, "cannot pull the whole tail down")) return 7930;

    // 7932-7933: nothing to repair at the true anchor; no lock, or a lock on
    // a floor point, is not a repair request.
    if (repair_windows(p, flatten_request(21, 1000), CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE, range)
            .status != CURVE_PREFIX_REPAIR_NOT_NEEDED) return 7932;
    const DesiredSettings none = {};
    if (repair_windows(p, none, CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE, range).status !=
        CURVE_PREFIX_REPAIR_NO_LOCK) return 7933;
    Table rtx;
    rtx_stock(&rtx, 45);
    if (repair_windows(rtx, flatten_request(10, 180), CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE, range)
            .status != CURVE_PREFIX_REPAIR_NO_LOCK) return 7933;
    return 0;
}

}  // namespace

int run_curve_prefix_repair_tests() {
    if (int failure = run_help01_cases()) return failure;
    if (int failure = run_help02_cases()) return failure;
    return run_decision_cases();
}
