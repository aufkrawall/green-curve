// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "gpu_core.h"
#include "scalar_curve_submission_policy.h"
#include "apply_clock_ceiling_policy.h"
#include <stdio.h>

int run_scalar_curve_submission_tests() {
    DesiredSettings d{}; gc_i64 offsets[VF_NUM_POINTS]{};
    if (scalar_curve_submission_plan(d, false, 0, nullptr, false, false).status != SCALAR_CURVE_NO_SPLIT) return 8948;
    d.hasGpuOffset = 1; d.gpuOffsetMHz = 100; d.authoring.graphicsPrecise = d.authoring.topologyBound = 1;
    d.authoring.graphicsOffsetHz = 100000000; d.authoring.graphicsSurface = DESIRED_GRAPHICS_VF_COMPONENT;
    d.curvePointCaptured[2] = 1; d.curvePointCapturedOffsetHz[2] = -500;
    if (scalar_curve_submission_plan(d, true, 0, offsets, true, false).status != SCALAR_CURVE_NO_SPLIT) return 8949;
    d.authoring.graphicsSurface = DESIRED_GRAPHICS_P0;
    if (scalar_curve_submission_plan(d, false, 0, offsets, true, true).status != SCALAR_CURVE_CURRENT_UNAVAILABLE) return 8950;
    if (scalar_curve_submission_plan(d, true, 0, offsets, false, true).status != SCALAR_CURVE_CURRENT_UNAVAILABLE) return 8951;
    if (scalar_curve_submission_plan(d, true, 100000000, offsets, true, false).status != SCALAR_CURVE_ORDER_SAFE) return 8952;
    if (scalar_curve_submission_plan(d, true, 200000000, offsets, true, false).status != SCALAR_CURVE_ORDER_SAFE) return 8953;
    auto p = scalar_curve_submission_plan(d, true, 0, offsets, true, true);
    if (p.status != SCALAR_CURVE_PIN_EVIDENCE_REQUIRED || p.loweredCaptured != 1 || scalar_curve_submission_allowed(p)) return 8954;
    d.curvePointCapturedOffsetHz[2] = 500;
    if (scalar_curve_submission_plan(d, true, 0, offsets, true, false).status != SCALAR_CURVE_ORDER_SAFE) return 8955;
    offsets[2] = 500; d.curvePointCapturedOffsetHz[2] = 0;
    if (scalar_curve_submission_plan(d, true, 0, offsets, true, true).status != SCALAR_CURVE_PIN_EVIDENCE_REQUIRED) return 8956;
    d.hasLock = 1; d.lockMode = LOCK_MODE_FLATTEN; d.lockMHz = 1000;
    if (scalar_curve_submission_plan(d, true, 0, offsets, true, false).status != SCALAR_CURVE_GUARD_UNAVAILABLE) return 8957;
    p = scalar_curve_submission_plan(d, true, 0, offsets, true, true);
    if (p.status != SCALAR_CURVE_INSTALL_GUARD || !scalar_curve_submission_allowed(p) || !p.baselineDependent) return 8958;
    d.lockMode = LOCK_MODE_HARD;
    if (scalar_curve_submission_plan(d, true, 0, offsets, true, true).status != SCALAR_CURVE_INSTALL_GUARD) return 8959;
    d.hasLock = 0; d.curvePointCaptured[2] = 0; d.hasCurvePoint[2] = 1; d.curvePointMHz[2] = 2000;
    if (scalar_curve_submission_plan(d, true, 0, offsets, true, true).status != SCALAR_CURVE_PIN_EVIDENCE_REQUIRED) return 8960;
    d.hasCurvePoint[2] = 0;
    if (scalar_curve_submission_plan(d, false, 0, nullptr, false, false).status != SCALAR_CURVE_NO_SPLIT) return 8961;
    const ApplyClockCeilingReason reasons[] = {APPLY_CEILING_REASON_REQUESTED_LOCK, APPLY_CEILING_REASON_REQUESTED_FLATTEN, APPLY_CEILING_REASON_RESET_DROPS_CAP};
    for (const auto reason : reasons) {
        if (!apply_clock_ceiling_transition_must_refuse(true, APPLY_CEILING_ARM_UNSUPPORTED, reason, true) ||
            !apply_clock_ceiling_transition_must_refuse(true, APPLY_CEILING_ARM_REFUSED, reason, true) ||
            apply_clock_ceiling_transition_must_refuse(true, APPLY_CEILING_ARM_INSTALLED, reason, true)) return 8962;
    }
    if (apply_clock_ceiling_transition_must_refuse(true, APPLY_CEILING_ARM_UNSUPPORTED, APPLY_CEILING_REASON_REQUESTED_FLATTEN) ||
        !apply_clock_ceiling_transition_must_refuse(false, APPLY_CEILING_ARM_NOT_NEEDED, APPLY_CEILING_REASON_REQUESTED_FLATTEN, true)) return 8963;
    d.hasCurvePoint[2] = 1; d.hasLock = 1; d.lockMode = LOCK_MODE_FLATTEN; d.lockMHz = 0;
    d.authoring.lockPrecise = 1; d.authoring.lockFrequencyHz = 500;
    if (scalar_curve_submission_plan(d, true, 0, offsets, true, true).status != SCALAR_CURVE_PIN_EVIDENCE_REQUIRED) return 8964;
    d.hasLock = 0; d.hasCurvePoint[2] = 0; d.curvePointCaptured[2] = 1; d.curvePointCapturedOffsetHz[2] = 1000;
    if (scalar_curve_submission_plan(d, true, 200000000, offsets, true, true, true).status != SCALAR_CURVE_PIN_EVIDENCE_REQUIRED) return 8981;
    d = {};
    if (scalar_curve_handoff_plan(d, false, 0, true).status != SCALAR_CURVE_CURRENT_UNAVAILABLE ||
        scalar_curve_handoff_plan(d, true, 0, false).status != SCALAR_CURVE_ORDER_SAFE ||
        scalar_curve_handoff_plan(d, true, -1000000, true).status != SCALAR_CURVE_PIN_EVIDENCE_REQUIRED) return 9144;
    d.hasLock = 1; d.lockMode = LOCK_MODE_HARD; d.lockMHz = 1000;
    if (scalar_curve_handoff_plan(d, true, -1000000, false).status != SCALAR_CURVE_GUARD_UNAVAILABLE ||
        scalar_curve_handoff_plan(d, true, -1000000, true).status != SCALAR_CURVE_INSTALL_GUARD) return 9145;
    return 0;
}
#ifdef GC_SCALAR_CURVE_FIXTURE_MAIN
int main() { const int code = run_scalar_curve_submission_tests(); if (code) fprintf(stderr, "scalar curve: %d\n", code); return code; }
#endif
