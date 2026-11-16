// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "curve_plan_fixtures.h"
#include "platform.h"
#include "linux_curve_targets.h"
#include "service_lifecycle_policy.h"
#include "desired_settings_ui_policy.h"
#include "linux_daemon_state.h"
#include "apply_correction_policy.h"
#include "gpu_backend_apply_correction.h"
#include "profile_desired_ini_ext.h"
#include "service_scoped_apply_policy.h"
#include <map>
#include <string>

int run_captured_intent_runtime_tests() {
    DesiredSettings desired{};
    desired.curvePointCaptured[22] = 1;
    long long offset = 1;
    if (!desired_curve_point_present(&desired, 22) ||
        desired_curve_point_kind(&desired, 22) != DESIRED_CURVE_CAPTURED ||
        !desired_curve_point_offset_khz(&desired, 22, 1200000, 400000, &offset) || offset)
        return 8040;
    desired.curvePointCapturedOffsetHz[22] = -15000000;
    if (!desired_curve_point_offset_khz(&desired, 22, 2200000, 500000, &offset) ||
        offset != -15000) return 8041;
    DesiredSettings changed = desired;
    changed.curvePointCapturedOffsetHz[22] = 0;
    if (desired_curve_point_semantically_equal(&desired, &changed, 22)) return 8042;
    changed = desired;
    changed.hasCurvePoint[22] = 1;
    if (desired_curve_intent_valid(&changed) || service_desired_bool_fields_valid(&changed))
        return 8043;
    changed = desired;
    changed.curvePointMHz[22] = 1000;
    if (desired_curve_intent_valid(&changed)) return 8044;
    changed = desired;
    changed.curvePointCapturedOffsetHz[22] = 15000500;
    if (desired_curve_point_offset_khz(&changed, 22, 0, 0, &offset)) return 8045;
    if (!service_desired_has_curve_point(&desired) ||
        service_desired_curve_point_count(&desired) != 1 ||
        !(service_desired_mutation_domains(&desired) & SERVICE_MUTATION_DOMAIN_VF_CURVE) ||
        !service_intent_owns_vf_cleanup(&desired)) return 8046;

    Table table{};
    pascal_stock(&table);
    CurvePlanResult plan = plan_windows(table, desired,
        CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE, nvml_range(-1000, 1000));
    if (plan.disposition != CURVE_PLAN_REPRESENTABLE) return 8047;
    plan = plan_windows(table, changed, CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE,
        nvml_range(-1000, 1000));
    if (!find_issue(plan, CURVE_PLAN_REASON_POINT_GRID) ||
        plan.disposition != CURVE_PLAN_REFUSED) return 8048;
    CurvePlanInput precise = plan_input_windows(table, changed,
        CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE, nvml_range(-1000, 1000));
    precise.offsetGridHz = 500; // Pascal Linux raw control surface
    plan = curve_plan_evaluate(precise);
    if (plan.disposition != CURVE_PLAN_REPRESENTABLE) return 8099;
    changed.curvePointCapturedOffsetHz[22] = 1000000500;
    plan = curve_plan_evaluate(precise);
    const CurvePlanIssue* rangeIssue = find_issue(plan, CURVE_PLAN_REASON_POINT_OUT_OF_RANGE);
    if (plan.disposition != CURVE_PLAN_REFUSED || !rangeIssue ||
        rangeIssue->adjustmentHz != 1000000500) return 8100;
    changed = desired;
    changed.hasLock = 1;
    changed.lockCi = 22;
    changed.lockMHz = 1000;
    changed.lockMode = LOCK_MODE_FLATTEN;
    plan = plan_windows(table, changed, CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE,
        nvml_range(-1000, 1000));
    if (!find_issue(plan, CURVE_PLAN_REASON_POINT_INTENT_CONFLICT)) return 8049;

    desired.curvePointCaptured[23] = 1; // independent owned zero
    desired.curvePointCapturedOffsetHz[22] = 15000000;
    int targets[VF_NUM_POINTS]{};
    bool mask[VF_NUM_POINTS]{};
    LinuxCurveTargetBuildResult built = linux_build_curve_targets(table.curve, table.offsets,
        &desired, -1000000, targets, mask);
    if (built.pointCount != 2 || !mask[22] || !mask[23] || targets[22] != 15000 || targets[23] ||
        built.pointAbsoluteIntent[22] || !built.pointCapturedIntent[22]) return 8050;
    char detail[128]{};
    gc_i64 readback[VF_NUM_POINTS]{};
    readback[22] = 15000000;
    if (!linux_verify_curve_captured_targets(readback, built, detail, sizeof(detail))) return 8051;
    readback[22] = 14999000;
    if (linux_verify_curve_captured_targets(readback, built, detail, sizeof(detail))) return 8052;
    readback[22] = 15001000;
    if (linux_verify_curve_captured_targets(readback, built, detail, sizeof(detail))) return 8053;
    readback[22] = 15000000;
    if (linux_verify_curve_captured_targets(readback, built, detail, sizeof(detail), false))
        return 8101;
    desired.curvePointCapturedOffsetHz[22] = 15000500;
    built = linux_build_curve_targets(table.curve, table.offsets, &desired,
        -1000000, targets, mask);
    readback[22] = 15000500;
    if (built.pointCapturedOffsetHz[22] != 15000500 ||
        !linux_verify_curve_captured_targets(readback, built, detail, sizeof(detail))) return 8102;
    desired.curvePointCapturedOffsetHz[22] = 15000000;

    DesiredSettings previous{};
    previous.hasPowerLimit = 1;
    previous.powerLimitPct = 80;
    DesiredSettings merged = service_merge_desired_after_mutation(&previous, &desired);
    if (!merged.curvePointCaptured[23] || merged.curvePointCapturedOffsetHz[22] != 15000000 ||
        !merged.hasPowerLimit || merged.powerLimitPct != 80) return 8054;
    DesiredSettings request{};
    if (!service_build_full_restore_request(&merged, &request) ||
        !request.curvePointCaptured[23] || request.curvePointCapturedOffsetHz[22] != 15000000)
        return 8055;
    request.resetOcBeforeApply = 1;
    merged = service_merge_desired_after_mutation(&merged, &request);
    if (!merged.curvePointCaptured[23]) return 8056;
    service_project_desired_to_available_domains(&merged, SERVICE_MUTATION_DOMAIN_POWER);
    if (merged.curvePointCaptured[23] || merged.curvePointCapturedOffsetHz[22]) return 8057;

    gc_bool8 present[VF_NUM_POINTS]{}, fromOffset[VF_NUM_POINTS]{};
    ApplyCorrectionRequest correction{};
    apply_bind_correction_curve_intent(&correction, &desired, present, fromOffset);
    if (!present[22] || !present[23] || !fromOffset[22] || !fromOffset[23] ||
        correction.curvePointMHz[22]) return 8058;
    GpuAdapterInfo gpu{};
    LinuxDaemonStateRecord record{};
    desired.hasCurvePoint[22] = 1;
    linux_daemon_record_initialize(&record, LINUX_DAEMON_RECORD_ACTIVE, &gpu, &desired);
    if (linux_daemon_record_valid(&record)) return 8059;
    DesiredSettings projection{};
    projection.hasCurvePoint[22] = projection.curvePointFromGpuOffset[22] = 1;
    projection.curvePointMHz[22] = 1000;
    changed = projection;
    changed.curvePointMHz[22] = 1025;
    if (!desired_curve_point_semantically_equal(&projection, &changed, 22)) return 8060;

    // desired_prepare_replacement_request and legacy scope upgrade rules.
    {
        DesiredSettings leg{}; leg.hasPowerLimit = 1; leg.powerLimitPct = 80;
        desired_prepare_replacement_request(&leg);
        if (leg.applicationScope != DESIRED_SCOPE_LEGACY || !leg.resetOcBeforeApply) return 8061;

        DesiredSettings scoped{};
        scoped.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED;
        scoped.curvePointCaptured[5] = 1; scoped.curvePointCapturedOffsetHz[5] = -500;
        desired_prepare_replacement_request(&scoped);
        if (desired_application_scope_kind(scoped.applicationScope) != DESIRED_SCOPE_REPLACE_PROFILE ||
            scoped.resetOcBeforeApply) return 8062;

        DesiredSettings prevAuthoring{};
        prevAuthoring.applicationScope = DESIRED_SCOPE_REPLACE_PROFILE;
        prevAuthoring.curvePointCaptured[5] = 1;
        DesiredSettings legacyFollower{}; legacyFollower.hasFan = 1; legacyFollower.fanMode = FAN_MODE_AUTO;
        service_upgrade_legacy_scalar_scope(&prevAuthoring, &legacyFollower);
        if (legacyFollower.applicationScope != DESIRED_SCOPE_UPDATE_INCLUDED) return 8063;
        legacyFollower.applicationScope = DESIRED_SCOPE_LEGACY;
        legacyFollower.resetOcBeforeApply = 1;
        service_upgrade_legacy_scalar_scope(&prevAuthoring, &legacyFollower);
        if (legacyFollower.applicationScope != DESIRED_SCOPE_REPLACE_PROFILE ||
            legacyFollower.resetOcBeforeApply) return 8071;
    }

    // Extended INI round-trip and strict rejection (DOC-01, DOC-02, CAP-10).
    {
        DesiredSettings iniSrc{};
        iniSrc.lockCi = 22;
        iniSrc.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED | DESIRED_SCOPE_REPLACE_VF_POINTS;
        iniSrc.hasGpuOffset = 1; iniSrc.gpuOffsetMHz = 0;
        iniSrc.authoring.graphicsPrecise = 1;
        iniSrc.authoring.graphicsSurface = DESIRED_GRAPHICS_VF_COMPONENT;
        iniSrc.authoring.graphicsOffsetHz = 0;
        iniSrc.authoring.identityExclusions = 1;
        iniSrc.authoring.graphicsExcluded[2] = 1;
        iniSrc.authoring.topologyBound = 1;
        iniSrc.authoring.topologyCount = 4;
        iniSrc.authoring.voltageUv[2] = 650000;
        iniSrc.authoring.voltageUv[3] = 662500;
        iniSrc.authoring.voltageUv[21] = 850000;
        iniSrc.authoring.voltageUv[22] = 862500;
        iniSrc.curvePointCaptured[3] = 1; iniSrc.curvePointCapturedOffsetHz[3] = -15000500;
        iniSrc.curvePointCaptured[21] = 1; iniSrc.curvePointCapturedOffsetHz[21] = 0;
        iniSrc.hasCurvePoint[22] = 1; iniSrc.curvePointMHz[22] = 1000;
        iniSrc.authoring.pointFrequencyPrecise[22] = 1;
        iniSrc.authoring.pointFrequencyHz[22] = 1000000500;
        iniSrc.hasLock = 1; iniSrc.lockMode = LOCK_MODE_FLATTEN; iniSrc.lockMHz = 1100;
        iniSrc.authoring.lockPrecise = 1; iniSrc.authoring.lockFrequencyHz = 1100000500;
        desired_memory_set_precise(&iniSrc, -500000, CLOCK_OFFSET_SURFACE_P0);
        desired_power_set_ratio(&iniSrc, {249, 250});
        if (desired_ini_should_emit_all_domains(&iniSrc)) return 8064;
        DesiredSettings iniLeg{};
        if (!desired_ini_should_emit_all_domains(&iniLeg)) return 8065;

        std::map<std::string, std::string> ctrlMap, curveMap;
        auto putCtrl = [&](const char* k, const char* v) -> bool { ctrlMap[k] = v; return true; };
        auto putCurve = [&](const char* k, const char* v) -> bool { curveMap[k] = v; return true; };
        if (!desired_ini_append_controls_ext(putCtrl, &iniSrc) ||
            !desired_ini_append_curve_ext(putCurve, &iniSrc)) return 8066;

        DesiredSettings loaded{};
        loaded.lockCi = iniSrc.lockCi; loaded.hasGpuOffset = 1;
        loaded.hasCurvePoint[22] = 1; loaded.curvePointMHz[22] = 1000;
        loaded.hasLock = 1; loaded.lockMode = LOCK_MODE_FLATTEN; loaded.lockMHz = 1100;
        loaded.hasMemOffset = 1; loaded.memOffsetMHz = iniSrc.memOffsetMHz;
        loaded.hasPowerLimit = 1; loaded.powerLimitPct = iniSrc.powerLimitPct;
        auto readFrom = [](const std::map<std::string, std::string>& m,
                           const char* key, char* buf, size_t bufSize) -> bool {
            auto it = m.find(key);
            if (it == m.end()) return false;
            snprintf(buf, bufSize, "%s", it->second.c_str());
            return true;
        };
        char err[192]{};
        if (!desired_ini_load_controls_ext(
                [&](const char* k, char* b, size_t n) { return readFrom(ctrlMap, k, b, n); },
                &loaded, err, sizeof(err)) ||
            !desired_ini_load_curve_ext(
                [&](const char* k, char* b, size_t n) { return readFrom(curveMap, k, b, n); },
                &loaded, err, sizeof(err)) ||
            !desired_ini_validate_loaded(&loaded, err, sizeof(err)) ||
            !desired_settings_equal(&iniSrc, &loaded)) return 8067;

        // Corrupt INI entries must fail loudly instead of silently clearing.
        auto badCtrl = ctrlMap; badCtrl["application_scope"] = "99";
        DesiredSettings bad = loaded;
        if (desired_ini_load_controls_ext(
                [&](const char* k, char* b, size_t n) { return readFrom(badCtrl, k, b, n); },
                &bad, err, sizeof(err))) return 8068;

        auto badPow = ctrlMap; badPow["power_ratio_den"] = "0";
        bad = {};
        if (!desired_ini_load_controls_ext(
                [&](const char* k, char* b, size_t n) { return readFrom(badPow, k, b, n); },
                &bad, err, sizeof(err)) ||
            desired_ini_validate_loaded(&bad, err, sizeof(err))) return 8069;

        auto badCurve = curveMap; badCurve.erase("point3_captured");
        bad = loaded;
        for (int i = 0; i < VF_NUM_POINTS; ++i) {
            bad.curvePointCaptured[i] = 0; bad.curvePointCapturedOffsetHz[i] = 0;
        }
        if (desired_ini_load_curve_ext(
                [&](const char* k, char* b, size_t n) { return readFrom(badCurve, k, b, n); },
                &bad, err, sizeof(err))) return 8070;
    }
    return 0;
}
