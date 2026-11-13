// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
static int curve_plan_precision_cases() {
    Table table{};
    long long frequency[VF_NUM_POINTS]{}, offsets[VF_NUM_POINTS]{}, component[VF_NUM_POINTS]{};
    for (int i = 0; i < 4; ++i) {
        table.curve[i] = {(unsigned)(600000 + i * 100000), (unsigned)(650000 + i * 10000)};
        frequency[i] = (long long)table.curve[i].freq_kHz * 1000 + 500;
    }
    DesiredSettings d{};
    d.hasCurvePoint[2] = 1; d.curvePointMHz[2] = 800;
    d.authoring.pointFrequencyPrecise[2] = 1; d.authoring.pointFrequencyHz[2] = 800000750;
    CurvePlanInput in = plan_input_windows(table, d, CURVE_PLAN_EVIDENCE_TRANSACTION_BASELINE, nvml_range(-1000, 1000));
    in.frequencyHz = frequency; in.programmedOffsetsHz = offsets; in.offsetGridHz = 500;
    CurvePlanResult r = curve_plan_evaluate(in);
    if (r.disposition != CURVE_PLAN_REFUSED || !find_issue(r, CURVE_PLAN_REASON_POINT_GRID)) return 8760;
    in.frequencyHz = in.programmedOffsetsHz = nullptr;
    if (curve_plan_evaluate(in).disposition != CURVE_PLAN_UNAVAILABLE) return 8761;
    in.frequencyHz = frequency;
    if (curve_plan_evaluate(in).disposition != CURVE_PLAN_UNAVAILABLE) return 8762;
    in.programmedOffsetsHz = offsets;
    d.authoring.pointFrequencyHz[2] = 800001000;
    if (curve_plan_evaluate(in).disposition != CURVE_PLAN_REPRESENTABLE) return 8763;
    in.offsetGridHz = 1000;
    if (curve_plan_evaluate(in).disposition != CURVE_PLAN_REFUSED) return 8764;
    in.offsetGridHz = 500;
    in.range = nvml_range(0, 0); d.authoring.pointFrequencyHz[2] = 800000000;
    r = curve_plan_evaluate(in);
    const CurvePlanIssue* range = find_issue(r, CURVE_PLAN_REASON_POINT_OUT_OF_RANGE);
    if (r.disposition != CURVE_PLAN_NEEDS_FRESH_BASELINE || !range || range->adjustmentHz != -500) return 8765;
    in.range = nvml_range(-1000, 1000);
    d = {}; d.hasGpuOffset = 1; d.authoring.graphicsPrecise = 1; d.authoring.graphicsOffsetHz = 500;
    d.hasCurvePoint[2] = d.curvePointFromGpuOffset[2] = 1;
    in.gpuComponentHz = component; component[2] = 500;
    if (curve_plan_evaluate(in).disposition != CURVE_PLAN_REPRESENTABLE) return 8766;
    component[2] = 501;
    r = curve_plan_evaluate(in);
    if (r.disposition != CURVE_PLAN_REFUSED || !find_issue(r, CURVE_PLAN_REASON_POINT_GRID)) return 8767;
    in.gpuComponentHz = nullptr;
    if (curve_plan_evaluate(in).disposition != CURVE_PLAN_UNAVAILABLE) return 8768;
    d = {}; d.hasLock = 1; d.lockCi = 2; d.lockMHz = 800; d.lockMode = LOCK_MODE_FLATTEN;
    d.authoring.lockPrecise = 1; d.authoring.lockFrequencyHz = 800000500;
    d.hasCurvePoint[1] = 1; d.curvePointMHz[1] = 800;
    d.authoring.pointFrequencyPrecise[1] = 1; d.authoring.pointFrequencyHz[1] = 800001000;
    in.lockMode = LOCK_MODE_FLATTEN;
    r = curve_plan_evaluate(in);
    if (r.disposition != CURVE_PLAN_REFUSED || !find_issue(r, CURVE_PLAN_REASON_PREFIX_ABOVE_PLATEAU)) return 8769;
    d.hasCurvePoint[0] = 1; d.curvePointMHz[0] = 800;
    d.authoring.pointFrequencyPrecise[0] = 1;
    d.authoring.pointFrequencyHz[1] = 800001500;
    d.authoring.pointFrequencyHz[0] = 800001000;
    r = curve_plan_evaluate(in);
    const CurvePlanIssue* prefix = find_issue(r, CURVE_PLAN_REASON_PREFIX_ABOVE_PLATEAU);
    if (!prefix || prefix->ci != 0 || prefix->worstCi != 1 || prefix->affectedCount != 2 ||
        prefix->targetHz != 800001000 || prefix->worstHz != 800001500 || prefix->limitHz != 800000500) return 8776;
    char text[768]{};
    curve_plan_format_message(r, true, text, sizeof(text));
    if (!strstr(text, "800000500 Hz")) return 8777;
    d.hasCurvePoint[0] = 0; d.curvePointMHz[0] = 0;
    d.authoring.pointFrequencyPrecise[0] = 0; d.authoring.pointFrequencyHz[0] = 0;
    d.hasCurvePoint[1] = 0; d.curvePointMHz[1] = 0;
    d.authoring.pointFrequencyPrecise[1] = 0; d.authoring.pointFrequencyHz[1] = 0;
    d.authoring.lockFrequencyHz = 800000750;
    r = curve_plan_evaluate(in);
    if (r.disposition != CURVE_PLAN_REFUSED || !find_issue(r, CURVE_PLAN_REASON_POINT_GRID)) return 8770;
    d.authoring.lockFrequencyHz = 800000500;
    d.lockMode = in.lockMode = LOCK_MODE_HARD;
    if (curve_plan_evaluate(in).disposition != CURVE_PLAN_UNAVAILABLE) return 8778;
    in.lockGridHz = 1000000;
    r = curve_plan_evaluate(in);
    if (r.disposition != CURVE_PLAN_REFUSED || !find_issue(r, CURVE_PLAN_REASON_POINT_GRID)) return 8779;
    d.lockMode = in.lockMode = LOCK_MODE_FLATTEN;
    frequency[2] += 1000;
    if (curve_plan_evaluate(in).disposition != CURVE_PLAN_UNAVAILABLE) return 8771;
    long long value = 19;
    d = {}; d.hasCurvePoint[2] = 1; d.curvePointMHz[2] = 800;
    d.authoring.pointFrequencyPrecise[2] = 1; d.authoring.pointFrequencyHz[2] = 800001000;
    if (!desired_curve_point_offset_from_base_hz(&d, 2, 800000500, 0, &value) || value != 500) return 8772;
    value = 19;
    if (desired_curve_point_offset_from_base_hz(&d, 2, -1, 0, &value) || value != 19 ||
        desired_curve_point_offset_from_base_hz(&d, 2, 0, 0, nullptr)) return 8773;
    d.curvePointFromGpuOffset[2] = 1;
    if (desired_curve_point_offset_from_base_hz(&d, 2, 0, LLONG_MAX, &value) || value != 19) return 8774;
    offsets[2] = 500;
    if (curve_plan_classify_pre_write_evidence(table.curve, table.offsets, in.floor, true, offsets) !=
        CURVE_PLAN_EVIDENCE_LIVE_SHAPED_ESTIMATE) return 8775;
    offsets[2] = 0;
    if (curve_plan_classify_pre_write_evidence(table.curve, table.offsets, in.floor, true, offsets) !=
        CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE) return 8775;
    return 0;
}
