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
    return 0;
}
