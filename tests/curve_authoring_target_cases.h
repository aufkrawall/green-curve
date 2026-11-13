// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "curve_authoring_targets.h"
#include "vf_backends.h"
#include "platform.h"
#include "linux_curve_authoring_targets.h"
static int curve_authoring_target_cases() {
    Table table{}; DesiredSettings d{};
    gc_i64 frequency[VF_NUM_POINTS]{}, offsets[VF_NUM_POINTS]{}, components[VF_NUM_POINTS]{};
    unsigned char membership[32]{}; membership[0] = 15;
    d.authoring.topologyBound = 1; d.authoring.topologyCount = 4;
    for (int i = 0; i < 4; ++i) {
        table.curve[i] = {(unsigned)(600000 + i * 100000), (unsigned)(650000 + i * 10000)};
        frequency[i] = (gc_i64)table.curve[i].freq_kHz * 1000 + 500;
        d.authoring.voltageUv[i] = table.curve[i].volt_uV; components[i] = 500;
    }
    d.hasGpuOffset = 1; d.authoring.graphicsPrecise = 1; d.authoring.graphicsOffsetHz = 500;
    d.authoring.graphicsSurface = DESIRED_GRAPHICS_VF_COMPONENT;
    d.authoring.identityExclusions = 1; d.authoring.graphicsExcluded[0] = 1;
    d.curvePointCaptured[0] = d.curvePointCaptured[1] = 1; d.curvePointCapturedOffsetHz[1] = -500;
    d.hasCurvePoint[2] = 1; d.curvePointMHz[2] = 800;
    d.authoring.pointFrequencyPrecise[2] = 1; d.authoring.pointFrequencyHz[2] = 800001000;
    d.hasCurvePoint[3] = d.curvePointFromGpuOffset[3] = 1;
    CurvePlanInput in = plan_input_windows(table, d, CURVE_PLAN_EVIDENCE_TRANSACTION_BASELINE, nvml_range(-1000, 1000));
    in.frequencyHz = frequency; in.programmedOffsetsHz = offsets; in.gpuComponentHz = components; in.offsetGridHz = 500;
    CurveAuthoringTargets out; CurveAuthoringTargetIssue issue;
    if (!curve_authoring_build_targets(in, &g_vfBackendPascal, membership, true, out, issue) || out.pointCount != 4 ||
        !out.mask[0] || out.offsetHz[0] || out.offsetHz[1] != -500 || out.offsetHz[2] != 500 || out.offsetHz[3] != 500 ||
        !out.capturedMask[0] || out.absoluteMask[0] || !out.absoluteMask[2] || out.absoluteHz[2] != 800001000 ||
        out.absoluteMask[3] || out.mask[4]) return 8783;
    unsigned char prior[sizeof(out)]; memcpy(prior, &out, sizeof(out));
    d.curvePointCapturedOffsetHz[1] = -250;
    if (curve_authoring_build_targets(in, &g_vfBackendPascal, membership, true, out, issue) ||
        issue.status != CURVE_AUTHORING_TARGET_UNREPRESENTABLE || issue.ci != 1 || issue.requestedHz != -250 ||
        memcmp(prior, &out, sizeof(out))) return 8784;
    d.curvePointCapturedOffsetHz[1] = -500;
    ++d.authoring.voltageUv[1];
    if (curve_authoring_build_targets(in, &g_vfBackendPascal, membership, true, out, issue) ||
        issue.status != CURVE_AUTHORING_TARGET_BINDING_CHANGED || memcmp(prior, &out, sizeof(out))) return 8785;
    --d.authoring.voltageUv[1];
    in.programmedOffsetsHz = nullptr;
    if (curve_authoring_build_targets(in, &g_vfBackendPascal, membership, true, out, issue) ||
        issue.status != CURVE_AUTHORING_TARGET_SAMPLES_UNAVAILABLE || memcmp(prior, &out, sizeof(out))) return 8786;
    in.programmedOffsetsHz = offsets;
    if (curve_authoring_build_targets(in, &g_vfBackendPascal, membership, false, out, issue) ||
        issue.status != CURVE_AUTHORING_TARGET_ROUTE_UNRESOLVED || memcmp(prior, &out, sizeof(out))) return 8787;
    membership[0] &= (unsigned char)~8u;
    if (curve_authoring_build_targets(in, &g_vfBackendPascal, membership, true, out, issue) ||
        issue.status != CURVE_AUTHORING_TARGET_POINT_UNWRITABLE || issue.ci != 3 || memcmp(prior, &out, sizeof(out))) return 8788;
    membership[0] = 15;
    d.curvePointCaptured[0] = 0;
    if (!curve_authoring_build_targets(in, &g_vfBackendPascal, membership, true, out, issue) ||
        out.pointCount != 3 || out.mask[0]) return 8789;
    int projected[VF_NUM_POINTS]{}; bool mask[VF_NUM_POINTS]{};
    auto builtLinux = linux_build_authoring_curve_targets(in, &g_vfBackendPascal, membership, true, projected, mask, issue);
    if (builtLinux.failed || builtLinux.deferred || builtLinux.pointCount != 3 || !builtLinux.pointExactOffset[2] ||
        builtLinux.pointExactOffsetHz[2] != 500 || !builtLinux.pointFrequencyPrecise[2] ||
        builtLinux.pointTargetHz[2] != 800001000 || builtLinux.pointCapturedIntent[2] || !builtLinux.pointCapturedIntent[1] ||
        !builtLinux.pointExactOffset[3] || builtLinux.pointCapturedIntent[3]) return 8790;
    char why[160]{};
    if (!linux_verify_curve_absolute_targets(table.curve, builtLinux, why, sizeof(why), frequency, true) ||
        linux_verify_curve_absolute_targets(table.curve, builtLinux, why, sizeof(why), frequency, false)) return 8791;
    const gc_i64 priorFrequency = frequency[2]; const auto priorKHz = table.curve[2].freq_kHz;
    frequency[2] = 808001500; table.curve[2].freq_kHz = 808001;
    if (linux_verify_curve_absolute_targets(table.curve, builtLinux, why, sizeof(why), frequency, true)) return 8792;
    frequency[2] = 808001000;
    if (!linux_verify_curve_absolute_targets(table.curve, builtLinux, why, sizeof(why), frequency, true)) return 8793;
    frequency[2] = priorFrequency; table.curve[2].freq_kHz = priorKHz;
    gc_i64 actualOffsets[VF_NUM_POINTS]{}; actualOffsets[1] = -500; actualOffsets[2] = actualOffsets[3] = 500;
    if (!linux_verify_curve_captured_targets(actualOffsets, builtLinux, why, sizeof(why))) return 8794;
    actualOffsets[2] = 0;
    if (linux_verify_curve_captured_targets(actualOffsets, builtLinux, why, sizeof(why))) return 8795;
    d.curvePointMHz[2] = 2000; d.authoring.pointFrequencyHz[2] = 2000000000;
    in.evidence = CURVE_PLAN_EVIDENCE_LIVE_SHAPED_ESTIMATE;
    builtLinux = linux_build_authoring_curve_targets(in, &g_vfBackendPascal, membership, true,
        projected, mask, issue, true);
    if (builtLinux.failed || !builtLinux.deferred || !builtLinux.pointMask[2] || builtLinux.pointExactOffsetHz[2] ||
        builtLinux.pointTargetHz[2] != 2000000000 ||
        linux_verify_curve_captured_targets(actualOffsets, builtLinux, why, sizeof(why))) return 8796;
    in.evidence = CURVE_PLAN_EVIDENCE_TRANSACTION_BASELINE;
    auto refused = linux_build_authoring_curve_targets(in, &g_vfBackendPascal, membership, true, projected, mask, issue, true);
    if (!refused.failed || issue.status != CURVE_AUTHORING_TARGET_UNREPRESENTABLE) return 8797;
    for (int i = 0; i < 4; ++i) { table.curve[i].freq_kHz += 700000; frequency[i] += 700000000; }
    auto rebuilt = linux_build_authoring_curve_targets(in, &g_vfBackendPascal, membership, true, projected, mask, issue);
    if (rebuilt.failed || rebuilt.deferred || !linux_curve_rebuild_keeps_shape(builtLinux, rebuilt) ||
        rebuilt.pointExactOffsetHz[2] != 499999500 || rebuilt.pointTargetHz[2] != builtLinux.pointTargetHz[2]) return 8798;
    d = {}; d.authoring.topologyBound = 1; d.authoring.topologyCount = 4;
    for (int i = 0; i < 4; ++i) {
        table.curve[i].freq_kHz = (unsigned)(600000 + i * 100000);
        frequency[i] = (gc_i64)table.curve[i].freq_kHz * 1000 + 500;
        d.authoring.voltageUv[i] = table.curve[i].volt_uV;
    }
    d.hasLock = 1; d.lockCi = 2; d.lockMHz = 800; d.lockMode = LOCK_MODE_HARD;
    d.authoring.lockPrecise = 1; d.authoring.lockFrequencyHz = 800000000;
    d.hasCurvePoint[3] = 1; d.curvePointMHz[3] = 1200; // overridden by the lock tail
    in.lockMode = LOCK_MODE_HARD; in.lockGridHz = 1000000; in.tailStrategy = CURVE_PLAN_TAIL_ANCHOR_ONLY;
    if (!curve_authoring_build_targets(in, &g_vfBackendPascal, membership, false, out, issue) ||
        out.pointCount != 1 || !out.mask[2] || out.mask[3] || out.offsetHz[2] != -500 || out.absoluteMask[2]) return 8804;
    d.lockMode = in.lockMode = LOCK_MODE_FLATTEN;
    d.authoring.lockFrequencyHz = 800000500; in.tailStrategy = CURVE_PLAN_TAIL_UNIFORM_MIN_OFFSET;
    if (!curve_authoring_build_targets(in, &g_vfBackendPascal, membership, false, out, issue) ||
        out.pointCount != 2 || !out.mask[2] || !out.mask[3] || out.offsetHz[2] || out.offsetHz[3] != -1000000000 ||
        !out.absoluteMask[3] || out.absoluteHz[3] != 800000500) return 8805;
    d = {}; d.authoring.topologyBound = 1; d.authoring.topologyCount = 4;
    d.hasGpuOffset = 1; d.authoring.graphicsPrecise = 1; d.authoring.graphicsOffsetHz = 1000;
    d.authoring.graphicsSurface = DESIRED_GRAPHICS_VF_COMPONENT;
    d.authoring.topologyCount = 5; membership[0] = 31;
    table.curve[4] = {1000000, 690000}; frequency[4] = 1000000500;
    for (int i = 0; i < 5; ++i) {
        d.authoring.voltageUv[i] = table.curve[i].volt_uV;
        components[i] = 1000;
        if (i < 4) { table.curve[i].freq_kHz = 600000; frequency[i] = 600000500; }
    }
    in.lockMode = LOCK_MODE_NONE;
    in.floor = vf_floor_topology_compute(table.curve, table.offsets, VF_NUM_POINTS);
    if (!curve_authoring_build_targets(in, &g_vfBackendFuture, membership, true, out, issue) ||
        out.pointCount != 1 || out.mask[0] || out.mask[1] || out.mask[2] || out.mask[3] ||
        !out.mask[4] || out.offsetHz[4] != 1000) return 8806;
    d.curvePointCaptured[0] = 1;
    if (curve_authoring_build_targets(in, &g_vfBackendFuture, membership, true, out, issue) ||
        issue.status != CURVE_AUTHORING_TARGET_POINT_UNWRITABLE || issue.ci != 0) return 8807;
    d = {}; d.authoring.topologyBound = 1; d.authoring.topologyCount = 4;
    for (int i = 0; i < 4; ++i) {
        table.curve[i] = {(unsigned)(600000 + i * 100000), (unsigned)(650000 + i * 10000)};
        frequency[i] = (gc_i64)table.curve[i].freq_kHz * 1000 + 500;
        d.authoring.voltageUv[i] = table.curve[i].volt_uV;
    }
    table.curve[4] = {}; frequency[4] = 0; membership[0] = 15;
    table.curve[2].freq_kHz = 2000000000; frequency[2] = 2000000000000;
    table.offsets[2] = -1000000000; offsets[2] = -1000000000000;
    d.hasCurvePoint[2] = 1; d.curvePointMHz[2] = 2000;
    d.authoring.pointFrequencyPrecise[2] = 1; d.authoring.pointFrequencyHz[2] = 2000000000;
    in.floor = vf_floor_topology_compute(table.curve, table.offsets, VF_NUM_POINTS);
    in.evidence = CURVE_PLAN_EVIDENCE_LIVE_SHAPED_ESTIMATE;
    if (!curve_authoring_build_targets(in, &g_vfBackendPascal, membership, false, out, issue, true) ||
        !out.requiresFreshBaseline || out.pointCount != 1 || !out.mask[2]) return 8808;
    const auto uncertain = curve_plan_evaluate(in);
    if (uncertain.disposition != CURVE_PLAN_NEEDS_FRESH_BASELINE ||
        !find_issue(uncertain, CURVE_PLAN_REASON_POINT_OUT_OF_RANGE)) return 8809;
    return 0;
}
