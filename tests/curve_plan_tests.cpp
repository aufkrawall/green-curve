// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Curve feasibility, writable points, flat-tail anchors and honest Save
// (assertion codes 7800-7899).  Pure headers only: the same helpers the
// Windows apply, the Linux daemon, the live lock detector and the Save
// handlers call.  Fixtures are synthetic and keep only the numeric
// relationships of the reported cases:
//   - Pascal (80 graphics points, rising from ~388 mV, point 22 at 662.5 mV
//     with a 1050 MHz stock base, point 67 at 1125 mV), externally flattened
//     at 1000 MHz from point 21 with sub-MHz bin jitter and an exact-kHz run
//     over 67..79;
//   - Blackwell-shaped tables for the 2400 MHz anchor and 1425 MHz tail cases;
//   - an RTX 5070 floor (180 MHz over 0..44, ramp start moving with
//     temperature).

#include <stdio.h>
#include <string.h>

#include "curve_feasibility_message.h"
#include "curve_plan_fixtures.h"
#include "curve_plan_precision_cases.h"
#include "gui_graph_axis_policy.h"
#include "main_layout_policy.h"
#include "profile_save_policy.h"
#include "vf_flat_tail_recognition.h"

namespace {

// The detector this replaced: visible (>= 700 mV) candidates, exact pass first.
int legacy_exact_first_anchor(const Table& t) {
    int visible[VF_NUM_POINTS];
    int n = 0;
    for (int i = 0; i < VF_NUM_POINTS; ++i)
        if (vf_point_visible_in_editor(t.curve[i].freq_kHz, t.curve[i].volt_uV)) visible[n++] = i;
    for (int vi = 0; vi < n - 1; ++vi) {
        bool same = true;
        for (int j = vi + 1; j < n && same; ++j)
            same = t.curve[visible[j]].freq_kHz == t.curve[visible[vi]].freq_kHz;
        if (same) return visible[vi];
    }
    return -1;
}

int run_writable_point_cases() {
    // 7800-7804: RTX 5070 floor -- bound at the warm read, a cold re-read of
    // the same topology keeps it; the listed set is the voltage rule's.
    Table warm, cold;
    rtx_stock(&warm, 45);
    rtx_stock(&cold, 47);
    VfFloorBinding binding = {};
    if (!vf_floor_binding_update(&binding, warm.curve, warm.offsets, VF_NUM_POINTS, 0)) return 7800;
    if (binding.floor.floorEnd != 45 || binding.floor.floorRun != 45 ||
        binding.floor.floorMHz != 180) return 7801;
    if (vf_floor_binding_update(&binding, cold.curve, cold.offsets, VF_NUM_POINTS, 0)) return 7802;
    if (binding.floor.floorEnd != 45) return 7802;
    if (vf_floor_topology_compute(cold.curve, cold.offsets, VF_NUM_POINTS).floorEnd != 47) return 7803;
    int listed = 0;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        const bool l = vf_point_listed(binding.floor, warm.curve, i);
        if (l != vf_point_visible_in_editor(warm.curve[i].freq_kHz, warm.curve[i].volt_uV))
            return 7804;
        if (l) ++listed;
    }
    if (listed != 87) return 7804;
    // Listing is presentation. A fixed floor remains read-only even where the
    // historic voltage rule keeps it visible (including the 725 mV point).
    if (!vf_point_listed(binding.floor, warm.curve, 44) ||
        vf_point_target_status(binding.floor, warm.curve, 44) != VF_POINT_TARGET_FLOOR) return 8691;
    DesiredSettings floorLock = flatten_request(44, 180);
    CurvePlanResult floorPlan = plan_windows(warm, floorLock,
        CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE, nvml_range(-1000, 1000));
    if (floorPlan.disposition != CURVE_PLAN_REFUSED ||
        !find_issue(floorPlan, CURVE_PLAN_REASON_FLOOR_POINT)) return 8692;
    floorLock = {}; floorLock.hasCurvePoint[44] = 1; floorLock.curvePointMHz[44] = 180;
    floorPlan = plan_windows(warm, floorLock, CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE, nvml_range(-1000, 1000));
    if (floorPlan.disposition != CURVE_PLAN_REFUSED ||
        !find_issue(floorPlan, CURVE_PLAN_REASON_FLOOR_POINT)) return 8693;
    if (vf_point_target_status(binding.floor, warm.curve, 45) != VF_POINT_TARGET_OK) return 8694;

    // 7805-7808: Pascal lists every rising point, 22 (662.5 mV) included;
    // the anchor check accepts it.
    Table pascal;
    pascal_stock(&pascal);
    const VfFloorTopology pf = vf_floor_topology_compute(pascal.curve, pascal.offsets, VF_NUM_POINTS);
    if (!pf.valid || pf.floorRun != 0 || pf.floorEnd != 0 || pf.graphicsEnd != VF_NUM_POINTS) return 7805;
    int pascalListed = 0;
    for (int i = 0; i < VF_NUM_POINTS; ++i)
        if (vf_point_listed(pf, pascal.curve, i)) ++pascalListed;
    if (pascalListed != 80) return 7806;
    if (vf_point_target_status(pf, pascal.curve, 22) != VF_POINT_TARGET_OK) return 7807;
    if (vf_point_visible_in_editor(pascal.curve[22].freq_kHz, pascal.curve[22].volt_uV)) return 7808;

    // 7809-7813: a floor ending below 700 mV: floor points refused as
    // anchor/target, the writable points after it accepted; foreign,
    // unpopulated and invalid indices keep their own statuses.
    Table low;
    linear_table(&low, 60, 450000, 10000);  // 450 mV .. 1040 mV
    for (int i = 0; i < 10; ++i) low.curve[i].freq_kHz = 300000;  // floor 0..9
    low.curve[127].volt_uV = 540000;  // out-of-order placeholder
    low.curve[127].freq_kHz = 405000;
    const VfFloorTopology lf = vf_floor_topology_compute(low.curve, low.offsets, VF_NUM_POINTS);
    if (lf.floorEnd != 10 || lf.floorRun != 10 || lf.floorMHz != 300) return 7809;
    if (vf_point_target_status(lf, low.curve, 5) != VF_POINT_TARGET_FLOOR) return 7810;
    if (vf_point_target_status(lf, low.curve, 12) != VF_POINT_TARGET_OK) return 7811;
    if (vf_point_target_status(lf, low.curve, 127) != VF_POINT_TARGET_FOREIGN_DOMAIN ||
        vf_point_target_status(lf, low.curve, 80) != VF_POINT_TARGET_UNPOPULATED ||
        vf_point_target_status(lf, low.curve, 200) != VF_POINT_TARGET_INDEX_INVALID) return 7812;
    // A floor never forms from a short run, and a single value is no floor.
    Table shortRun;
    linear_table(&shortRun, 20, 450000, 10000);
    for (int i = 0; i < 3; ++i) shortRun.curve[i].freq_kHz = 300000;
    if (vf_floor_topology_compute(shortRun.curve, shortRun.offsets, VF_NUM_POINTS).floorRun != 0)
        return 7813;

    // 7814-7815: the editor projection drops a floor point but keeps a
    // listed sub-700 mV point and its provenance.
    DesiredSettings saved = {};
    saved.hasCurvePoint[5] = true;   saved.curvePointMHz[5] = 300;
    saved.hasCurvePoint[12] = true;  saved.curvePointMHz[12] = 1000;
    saved.curvePointFromGpuOffset[12] = true;
    if (vf_filter_unlisted_editor_curve_points(&saved, lf, low.curve) != 1) return 7814;
    if (saved.hasCurvePoint[5] || !saved.hasCurvePoint[12] ||
        !saved.curvePointFromGpuOffset[12]) return 7815;

    // 7816: the topology key ignores frequencies but not voltages.
    if (vf_floor_topology_key(warm.curve, VF_NUM_POINTS, 0) !=
        vf_floor_topology_key(cold.curve, VF_NUM_POINTS, 0)) return 7816;
    Table moved = warm;
    moved.curve[60].volt_uV += 5000;
    if (vf_floor_topology_key(moved.curve, VF_NUM_POINTS, 0) ==
        vf_floor_topology_key(warm.curve, VF_NUM_POINTS, 0)) return 7816;
    return 0;
}

int run_detect_cases() {
    // 7820-7823 (DETECT-01): the jittered plateau starting at the writable
    // sub-700 mV point 21 wins over the exact-kHz run at 67; the old order
    // picked 67.
    Table ext;
    pascal_external_flatten(&ext);
    const VfFloorTopology floor = vf_floor_topology_compute(ext.curve, ext.offsets, VF_NUM_POINTS);
    if (legacy_exact_first_anchor(ext) != 67) return 7820;
    VfFlatTailRecognition rec = vf_recognize_flat_tail(ext.curve, floor, 1, 3);
    if (!rec.found || rec.anchorCi != 21 || rec.exact) return 7821;
    if (rec.plateauMHz != 1000 || rec.tailCount != 58 || rec.maxDeviationKHz != 400) return 7822;
    if (ext.curve[rec.anchorCi].volt_uV >= (unsigned int)MIN_VISIBLE_VOLT_mV * 1000u) return 7823;

    // 7824: a flatten Green Curve wrote (exact collapsed tail) is found at
    // its own anchor.
    Table own;
    pascal_stock(&own);
    for (int i = 40; i < 80; ++i) own.curve[i].freq_kHz = own.curve[40].freq_kHz;
    rec = vf_recognize_flat_tail(own.curve,
        vf_floor_topology_compute(own.curve, own.offsets, VF_NUM_POINTS), 1, 3);
    if (!rec.found || rec.anchorCi != 40 || !rec.exact) return 7824;

    // 7825: a floor point at the plateau frequency is never the anchor; the
    // floor is the binding's (stock base), not the flattened live table.
    Table rtx;
    rtx_stock(&rtx, 45);
    VfFloorBinding binding = {};
    vf_floor_binding_update(&binding, rtx.curve, rtx.offsets, VF_NUM_POINTS, 0);
    Table flat = rtx;
    for (int i = 45; i < 127; ++i) {
        flat.offsets[i] = 180000 - (int)rtx.curve[i].freq_kHz;
        flat.curve[i].freq_kHz = 180000;
    }
    rec = vf_recognize_flat_tail(flat.curve, binding.floor, 1, 3);
    if (!rec.found || rec.anchorCi != 45) return 7825;

    // 7826: no plateau, nothing found.
    Table rising;
    linear_table(&rising, 60, 700000, 10000);
    rec = vf_recognize_flat_tail(rising.curve,
        vf_floor_topology_compute(rising.curve, rising.offsets, VF_NUM_POINTS), 1, 3);
    if (rec.found) return 7826;

    // 7827: a tolerant plateau needs three tail points; an exact one, one.
    Table shortTail;
    linear_table(&shortTail, 30, 700000, 10000);
    shortTail.curve[28].freq_kHz = 1500400;
    shortTail.curve[29].freq_kHz = 1499600;
    shortTail.curve[27].freq_kHz = 1500000;
    rec = vf_recognize_flat_tail(shortTail.curve,
        vf_floor_topology_compute(shortTail.curve, shortTail.offsets, VF_NUM_POINTS), 1, 3);
    if (rec.found) return 7827;
    shortTail.curve[28].freq_kHz = 1500000;
    shortTail.curve[29].freq_kHz = 1500000;
    rec = vf_recognize_flat_tail(shortTail.curve,
        vf_floor_topology_compute(shortTail.curve, shortTail.offsets, VF_NUM_POINTS), 1, 3);
    if (!rec.found || rec.anchorCi != 27 || !rec.exact) return 7827;
    return 0;
}

int run_planner_cases() {
    char message[512];
    // 7840-7844 (PLAN-01): 2400 MHz at 800 mV over a 1162 MHz base.
    Table b;
    linear_table(&b, 80, 700000, 10000);
    b.curve[10].freq_kHz = 1162000;
    for (int i = 11; i < 80; ++i) b.curve[i].freq_kHz = 1170000u + (unsigned int)(i - 11) * 25000u;
    CurvePlanResult r = plan_windows(b, flatten_request(10, 2400),
        CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE, nvml_range(-1000, 1000));
    if (r.disposition != CURVE_PLAN_REFUSED) return 7840;
    const CurvePlanIssue* anchor = find_issue(r, CURVE_PLAN_REASON_ANCHOR_ABOVE_RANGE);
    if (!anchor || !anchor->proven || anchor->adjustmentKHz != 1238000 ||
        anchor->limitKHz != 1000000) return 7841;
    if (r.anchorOnlyMaxKHz != 2162000) return 7842;
    if (find_issue(r, CURVE_PLAN_REASON_MISSING_POINT)) return 7843;
    curve_plan_format_message(r, true, message, sizeof(message));
    if (!strstr(message, "2400 MHz at 800 mV requires a +1238 MHz point adjustment") ||
        !strstr(message, "+1000 MHz (driver graphics offset range)") ||
        !strstr(message, "2162 MHz") || !strstr(message, "Nothing was changed.")) return 7844;
    curve_plan_format_message(r, false, message, sizeof(message));
    if (strstr(message, "Nothing was changed.")) return 7844;

    // 7845-7849 (PLAN-02): 1425 MHz at 820 mV; the tail stays at 1432 MHz at
    // 915 mV and reaches 2205 MHz at the far end.  First AND worst retained.
    Table t;
    linear_table(&t, 100, 700000, 5000);
    t.curve[24].freq_kHz = 1300000;
    for (int i = 25; i < 43; ++i) t.curve[i].freq_kHz = 1325000u + (unsigned int)(i - 25) * 60000u;
    for (int i = 43; i < 100; ++i)
        t.curve[i].freq_kHz = (unsigned int)(2432000 + (long long)(i - 43) * (3205000 - 2432000) / 56);
    r = plan_windows(t, flatten_request(24, 1425), CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE,
                     nvml_range(-1000, 1000));
    const CurvePlanIssue* tail = find_issue(r, CURVE_PLAN_REASON_TAIL_UNREACHABLE);
    if (r.disposition != CURVE_PLAN_REFUSED || !tail || !tail->proven) return 7845;
    if (tail->ci != 43 || tail->voltUv != 915000 || tail->targetMHz != 1432) return 7846;
    if (tail->worstCi != 99 || tail->worstMHz != 2205 || tail->affectedCount != 57) return 7847;
    if (r.tailLowerBoundKHz != 2205000) return 7848;
    curve_plan_format_message(r, true, message, sizeof(message));
    if (!strstr(message, "point 43 at 915 mV stays at 1432 MHz") ||
        !strstr(message, "57 tail points are affected; the worst is point 99")) return 7849;

    // 7850-7851 (PLAN-03): the anchor-only reach (2162 MHz) is below what the
    // tail allows: no target exists under these constraints.
    Table c = b;
    c.curve[60].freq_kHz = 3300000;
    for (int i = 61; i < 80; ++i) c.curve[i].freq_kHz = 3300000;
    r = plan_windows(c, flatten_request(10, 2162), CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE,
                     nvml_range(-1000, 1000));
    if (find_issue(r, CURVE_PLAN_REASON_ANCHOR_ABOVE_RANGE) ||
        !find_issue(r, CURVE_PLAN_REASON_TAIL_UNREACHABLE)) return 7850;
    if (!(r.tailLowerBoundKHz > r.anchorOnlyMaxKHz) || r.anchorOnlyMaxKHz != 2162000) return 7851;

    // 7852-7856 (PLAN-04): Pascal at stock, the old 1000 MHz flatten at 67:
    // refused at point 22 (662 mV, stock 1050 MHz), worst and count kept.
    Table p;
    pascal_stock(&p);
    r = plan_windows(p, flatten_request(67, 1000), CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE,
                     nvml_range(-1000, 1000));
    const CurvePlanIssue* prefix = find_issue(r, CURVE_PLAN_REASON_PREFIX_ABOVE_PLATEAU);
    if (r.disposition != CURVE_PLAN_REFUSED || !prefix || !prefix->proven) return 7852;
    if (prefix->ci != 22 || prefix->voltUv != 662500 || prefix->targetMHz != 1050 ||
        !prefix->belowVoltageRule || prefix->kind != CURVE_PLAN_POINT_ABSENT) return 7853;
    if (prefix->affectedCount != 45 || prefix->worstCi != 66) return 7854;
    curve_plan_format_message(r, true, message, sizeof(message));
    if (!strstr(message, "point 22 at 662 mV, which stays at its stock 1050 MHz") ||
        !strstr(message, "Points below 700 mV are listed")) return 7855;
    // A typed prefix point above the plateau is proven even over a shaped curve.
    DesiredSettings typed = flatten_request(40, 1300);
    typed.hasCurvePoint[30] = true;
    typed.curvePointMHz[30] = 1400;
    Table shaped = p;
    shaped.offsets[50] = 100000;
    shaped.curve[50].freq_kHz += 100000;
    r = plan_windows(shaped, typed, CURVE_PLAN_EVIDENCE_LIVE_SHAPED_ESTIMATE, nvml_range(-1000, 1000));
    prefix = find_issue(r, CURVE_PLAN_REASON_PREFIX_ABOVE_PLATEAU);
    if (r.disposition != CURVE_PLAN_REFUSED || !prefix || prefix->ci != 30 ||
        prefix->kind != CURVE_PLAN_POINT_ABSOLUTE) return 7856;

    // 7857-7859 (PLAN-05): a shaped outgoing curve makes an untouched point
    // read 250 MHz above its stock base: an estimate, not a refusal; the
    // post-reset baseline accepts the same request.
    Table out;
    linear_table(&out, 60, 700000, 10000);
    for (int i = 0; i < 60; ++i) out.curve[i].freq_kHz = 800000u + (unsigned int)i * 20000u;
    Table stock = out;
    out.curve[19].freq_kHz += 250000;           // coupling artifact, offset 0
    out.offsets[20] = 475000;
    out.curve[20].freq_kHz += 475000;
    const DesiredSettings switchReq = flatten_request(30, 1400);
    r = plan_windows(out, switchReq, CURVE_PLAN_EVIDENCE_LIVE_SHAPED_ESTIMATE, nvml_range(-1000, 1000));
    if (r.disposition != CURVE_PLAN_NEEDS_FRESH_BASELINE) return 7857;
    if (curve_plan_classify_pre_write_evidence(out.curve, out.offsets,
            vf_floor_topology_compute(out.curve, out.offsets, VF_NUM_POINTS), true) !=
        CURVE_PLAN_EVIDENCE_LIVE_SHAPED_ESTIMATE) return 7858;
    r = plan_windows(stock, switchReq, CURVE_PLAN_EVIDENCE_TRANSACTION_BASELINE, nvml_range(-1000, 1000));
    if (r.disposition != CURVE_PLAN_REPRESENTABLE || r.issueCount != 0) return 7859;

    // 7860-7861 (PLAN-06): a fallback range keeps the same decisions and says
    // it is the application's range, not a measured limit.
    CurvePlanRange fallback = nvml_range(-1000, 1000);
    fallback.source = CURVE_PLAN_RANGE_POLICY_FALLBACK;
    r = plan_windows(b, flatten_request(10, 2400), CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE, fallback);
    if (r.disposition != CURVE_PLAN_REFUSED || r.range.source != CURVE_PLAN_RANGE_POLICY_FALLBACK) return 7860;
    curve_plan_format_message(r, true, message, sizeof(message));
    if (!strstr(message, "(application fallback)")) return 7861;

    // 7862-7865 (PLAN-07): asymmetric, zero-width and inverted ranges.
    r = plan_windows(b, flatten_request(10, 2400), CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE,
                     nvml_range(-500, 1500));
    if (r.disposition != CURVE_PLAN_REPRESENTABLE) return 7862;
    r = plan_windows(b, flatten_request(10, 500), CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE,
                     nvml_range(-500, 1500));
    anchor = find_issue(r, CURVE_PLAN_REASON_ANCHOR_BELOW_RANGE);
    if (!anchor || anchor->adjustmentKHz != -662000 || anchor->limitKHz != -500000) return 7863;
    r = plan_windows(b, flatten_request(10, 1162), CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE,
                     nvml_range(0, 0));
    if (!find_issue(r, CURVE_PLAN_REASON_FLATTEN_UNSUPPORTED)) return 7864;
    DesiredSettings pinned = {};
    pinned.hasCurvePoint[10] = true;
    pinned.curvePointMHz[10] = 1162;  // exactly its base: a zero offset
    pinned.hasCurvePoint[11] = true;
    pinned.curvePointMHz[11] = 1171;  // one MHz above its base
    r = plan_windows(b, pinned, CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE, nvml_range(0, 0));
    const CurvePlanIssue* point = find_issue(r, CURVE_PLAN_REASON_POINT_OUT_OF_RANGE);
    if (!point || point->ci != 11 || point->affectedCount != 1) return 7865;
    const VfOffsetRange inverted = vf_offset_range_from_probe(true, 5000, -5000);
    if (inverted.known || inverted.minKHz != -VF_OFFSET_RANGE_FALLBACK_LIMIT_KHZ) return 7865;

    // 7866-7869 (PLAN-08): sub-700 mV anchors are accepted where writable;
    // floor anchors, foreign and unpopulated points are refused by name.
    Table low;
    linear_table(&low, 60, 450000, 10000);
    for (int i = 0; i < 10; ++i) low.curve[i].freq_kHz = 300000;
    low.curve[127].volt_uV = 540000;
    low.curve[127].freq_kHz = 405000;
    r = plan_windows(low, flatten_request(12, 1050), CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE,
                     nvml_range(-1000, 1000));
    if (r.disposition != CURVE_PLAN_REPRESENTABLE) return 7866;
    r = plan_windows(low, flatten_request(5, 300), CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE,
                     nvml_range(-1000, 1000));
    const CurvePlanIssue* floorIssue = find_issue(r, CURVE_PLAN_REASON_FLOOR_POINT);
    if (r.disposition != CURVE_PLAN_REFUSED || !floorIssue || floorIssue->ci != 5) return 7867;
    curve_plan_format_message(r, true, message, sizeof(message));
    if (!strstr(message, "flat low-voltage floor (300 MHz)")) return 7867;
    if (!find_issue(plan_windows(low, flatten_request(127, 900), CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE,
            nvml_range(-1000, 1000)), CURVE_PLAN_REASON_FOREIGN_DOMAIN)) return 7868;
    if (!find_issue(plan_windows(low, flatten_request(80, 900), CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE,
            nvml_range(-1000, 1000)), CURVE_PLAN_REASON_MISSING_POINT)) return 7869;

    // 7870-7871: evidence classes and an unreadable curve.
    if (curve_plan_classify_pre_write_evidence(p.curve, p.offsets,
            vf_floor_topology_compute(p.curve, p.offsets, VF_NUM_POINTS), true) !=
            CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE ||
        curve_plan_classify_pre_write_evidence(p.curve, p.offsets,
            vf_floor_topology_compute(p.curve, p.offsets, VF_NUM_POINTS), false) !=
            CURVE_PLAN_EVIDENCE_LIVE_SHAPED_ESTIMATE) return 7870;
    CurvePlanInput none = {};
    const DesiredSettings nothing = {};
    none.desired = &nothing;
    if (curve_plan_evaluate(none).disposition != CURVE_PLAN_UNAVAILABLE) return 7871;

    // 7872: issue capacity keeps first entries and counts the rest.
    CurvePlanResult many = {};
    CurvePlanIssue filler = {};
    for (int i = 0; i < CURVE_PLAN_MAX_ISSUES + 3; ++i) curve_plan_add_issue(&many, filler);
    if (many.issueCount != CURVE_PLAN_MAX_ISSUES || many.omittedIssues != 3) return 7872;
    return 0;
}

int run_save_and_round_trip_cases() {
    // 7880-7884 (SAVE-01): the no-edit branch over another writer's curve.
    if (profile_save_uses_gui_capture(false, false, false)) return 7880;
    Table ext;
    pascal_external_flatten(&ext);
    int componentKHz[VF_NUM_POINTS] = {};
    int first = -1;
    const int omitted = profile_save_count_unsaved_programmed_points(
        ext.curve, ext.offsets, componentKHz, &first);
    if (omitted != 58 || first != 22) return 7881;
    DesiredSettings liveSave = {};
    if (profile_save_notice(false, omitted, &liveSave, false, false, 0) !=
        PROFILE_SAVE_NOTICE_OMITTED_EXTERNAL_CURVE) return 7882;
    // A Green Curve GPU offset in the VF table is explained by the saved
    // component; one on the NVML route leaves the table at zero.
    Table offsetCurve;
    pascal_stock(&offsetCurve);
    for (int i = 0; i < 80; ++i) {
        offsetCurve.offsets[i] = i >= 5 ? 50000 : 0;
        componentKHz[i] = i >= 5 ? 50000 : 0;
    }
    if (profile_save_count_unsaved_programmed_points(offsetCurve.curve, offsetCurve.offsets,
            componentKHz, nullptr) != 0) return 7883;
    if (profile_save_notice(false, 0, &liveSave, false, false, 0) != PROFILE_SAVE_NOTICE_NONE)
        return 7883;
    // GUI capture: a detected lock with inferred pre-anchor offsets.
    DesiredSettings detected = flatten_request(67, 1000);
    for (int i = 25; i < 67; ++i) {
        detected.hasCurvePoint[i] = true;
        detected.curvePointMHz[i] = 1000;
        detected.curvePointFromGpuOffset[i] = true;
    }
    memset(componentKHz, 0, sizeof(componentKHz));
    const int inferred = profile_save_count_inferred_pre_anchor_points(&detected, componentKHz);
    if (inferred != 42) return 7884;
    if (profile_save_notice(true, 0, &detected, false, false, inferred) !=
            PROFILE_SAVE_NOTICE_DETECTED_APPROXIMATION ||
        profile_save_notice(true, 0, &detected, true, false, inferred) != PROFILE_SAVE_NOTICE_NONE ||
        profile_save_notice(true, 0, &detected, false, true, inferred) != PROFILE_SAVE_NOTICE_NONE)
        return 7884;

    // 7885-7889 (ROUND-01): external flatten -> detected anchor -> slot ->
    // reset to stock -> load -> plan.  The slot holds the flatten at its true
    // (sub-700 mV) anchor; both the pre-write and the post-reset plan accept
    // it.  The old anchor (67) is refused at point 22, as reported.
    VfFloorBinding binding = {};
    vf_floor_binding_update(&binding, ext.curve, ext.offsets, VF_NUM_POINTS, 0);
    const VfFlatTailRecognition rec = vf_recognize_flat_tail(ext.curve, binding.floor, 1, 3);
    if (!rec.found || rec.anchorCi != 21 || rec.plateauMHz != 1000) return 7885;
    DesiredSettings slot = flatten_request(rec.anchorCi, rec.plateauMHz);
    if (profile_save_count_inferred_pre_anchor_points(&slot, nullptr) != 0) return 7886;
    Table stock;
    pascal_stock(&stock);
    if (vf_filter_unlisted_editor_curve_points(&slot, binding.floor, stock.curve) != 0 ||
        vf_point_target_status(binding.floor, stock.curve, slot.lockCi) != VF_POINT_TARGET_OK)
        return 7887;
    CurvePlanResult r = plan_windows(stock, slot, CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE,
                                     nvml_range(-1000, 1000));
    if (r.disposition != CURVE_PLAN_REPRESENTABLE || r.issueCount != 0) return 7888;
    r = plan_windows(stock, slot, CURVE_PLAN_EVIDENCE_TRANSACTION_BASELINE, nvml_range(-1000, 1000));
    if (r.disposition != CURVE_PLAN_REPRESENTABLE) return 7888;
    r = plan_windows(stock, flatten_request(legacy_exact_first_anchor(ext), 1000),
                     CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE, nvml_range(-1000, 1000));
    const CurvePlanIssue* prefix = find_issue(r, CURVE_PLAN_REASON_PREFIX_ABOVE_PLATEAU);
    if (r.disposition != CURVE_PLAN_REFUSED || !prefix || prefix->ci != 22) return 7889;
    return 0;
}

int run_ui_cases() {
    // 7890-7893 (UI-01): the grid at 80 and the largest possible count, at
    // the minimum viewport and every DPI case, stays a valid plan.
    const int dpis[] = { 96, 110, 120, 134, 144, 168, 192, 216, 240, 288 };
    const int counts[] = { 55, 80, VF_NUM_POINTS };
    for (int dpi : dpis) {
        const int w = main_layout_scale_px(MAIN_LAYOUT_MIN_VIEWPORT_WIDTH_LOGICAL, dpi);
        const int h = main_layout_scale_px(MAIN_LAYOUT_MIN_VIEWPORT_HEIGHT_LOGICAL, dpi);
        for (int count : counts) {
            const MainLayoutPlan plan = main_layout_build_plan(w, h, dpi, count);
            if (plan.columns < 1 || plan.columns > MAIN_LAYOUT_MAX_COLUMNS ||
                plan.columns * plan.rowsPerColumn < count) return 7890;
            for (int vi = 0; vi < count; ++vi) {
                const MainLayoutPointCell cell = main_layout_point_cell(plan, vi);
                if (cell.left < 0 || cell.right > plan.contentWidth ||
                    cell.top < plan.pointStartY ||
                    cell.bottom > plan.globalControlsY - main_layout_scale_px(6, dpi)) return 7891;
            }
        }
    }
    // The graph axis starts at or below the first listed point.
    if (gui_graph_voltage_axis_floor_mv(662, 700, 50, 10) != 650 ||
        gui_graph_voltage_axis_floor_mv(388, 700, 50, 10) != 350 ||
        gui_graph_voltage_axis_floor_mv(710, 700, 50, 10) != 700 ||
        gui_graph_voltage_axis_floor_mv(0, 700, 50, 10) != 700) return 7892;
    const GuiGraphVoltageRange axis = gui_graph_voltage_axis(388, 1246,
        gui_graph_voltage_axis_floor_mv(388, 700, 50, 10), 1250, 50, 300, 10);
    if (axis.minMv > 388 || axis.maxMv < 1246) return 7892;
    // A flag written under the 700 mV rule answers with that rule: point 22
    // was hidden in the writer's editor although it is listed now.
    Table p;
    pascal_stock(&p);
    const VfFloorTopology pf = vf_floor_topology_compute(p.curve, p.offsets, VF_NUM_POINTS);
    if (!vf_point_listed(pf, p.curve, 22) ||
        vf_point_visible_in_editor(p.curve[22].freq_kHz, p.curve[22].volt_uV) ||
        !vf_point_visible_in_editor(p.curve[25].freq_kHz, p.curve[25].volt_uV)) return 7893;
    return 0;
}

}  // namespace

int run_curve_plan_tests() {
    if (int failure = curve_plan_precision_cases()) return failure;
    if (int failure = run_writable_point_cases()) return failure;
    if (int failure = run_detect_cases()) return failure;
    if (int failure = run_planner_cases()) return failure;
    if (int failure = run_save_and_round_trip_cases()) return failure;
    return run_ui_cases();
}
