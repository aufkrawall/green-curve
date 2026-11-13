// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
// Target construction for the Linux transaction; included after scalar and
// range helpers. Shared exact compilation retains its Hz authority channels.
#include "linux_curve_authoring_targets.h"
static CurvePlanInput linux_backend_curve_plan_input(LinuxGpuState*, const DesiredSettings*,
    const LinuxHardwareSnapshot*, const int*, const bool*, int*, gc_i64*, bool*);

// Authoring requests retain physical Hz; older requests keep their existing
// kHz builder. Both use the established uniform-floor flatten strategy.
static LinuxCurveTargetBuildResult build_curve_targets(LinuxGpuState* g,
    const DesiredSettings* d, int* targetOffsets, bool* pointMask, int graphicsRouteOverride = 0,
    bool rebuilding = false) {
    if (curve_authoring_needs_exact_targets(d)) {
        int componentKHz[VF_NUM_POINTS]{};
        gc_i64 componentHz[VF_NUM_POINTS]{};
        bool stockMask[VF_NUM_POINTS]{};
        CurvePlanInput in = linux_backend_curve_plan_input(g, d, nullptr, nullptr, nullptr,
            componentKHz, componentHz, stockMask);
        if (rebuilding && d->resetOcBeforeApply) in.evidence = CURVE_PLAN_EVIDENCE_TRANSACTION_BASELINE;
        const bool composed = d->authoring.graphicsPrecise
            ? d->authoring.graphicsSurface == DESIRED_GRAPHICS_VF_COMPONENT
            : graphicsRouteOverride == 2 || (graphicsRouteOverride == 0 && desired_gpu_offset_uses_curve(d));
        CurveAuthoringTargetIssue issue;
        auto built = linux_build_authoring_curve_targets(in, g->backend, g->vfMask, composed,
            targetOffsets, pointMask, issue, d->resetOcBeforeApply && !rebuilding);
        if (built.failed) lb_log("curve-authoring: target compile refused status=%d ci=%d requestedHz=%lld\n",
            (int)issue.status, issue.ci, issue.requestedHz);
        return built;
    }
    int mn = 0, mx = 0;
    curve_offset_range_khz(g, &mn, &mx);
    (void)mx;
    return linux_build_curve_targets(g->curve, g->freqOffsets, d, mn,
        targetOffsets, pointMask, graphicsRouteOverride);
}

static LinuxCurveTargetBuildResult build_curve_transition_targets(
    LinuxGpuState* g, const DesiredSettings* previousIntent,
    const DesiredSettings* committedIntent, int* targetOffsets,
    bool* pointMask, int* cleanupPointCountOut, bool rebuilding = false) {
    if (curve_authoring_needs_exact_targets(previousIntent) || curve_authoring_needs_exact_targets(committedIntent)) {
        if (cleanupPointCountOut) *cleanupPointCountOut = 0;
        LinuxCurveTargetBuildResult failed{}; failed.failed = true;
        if (!committedIntent) return failed;
        if (vf_floor_binding_update(&g->vfFloor, g->curve, g->freqOffsets, VF_NUM_POINTS, 0))
            lb_log("curve-authoring: bound topology key=%08x floorEnd=%d\n", g->vfFloor.topologyKey, g->vfFloor.floor.floorEnd);
        DesiredSettings incoming{}, outgoing{}; char why[160]{};
        if (!desired_rebind_topology(*committedIntent, g->curve, g->vfFloor.floor, &incoming, why, sizeof(why)) ||
            (previousIntent && !desired_rebind_topology(*previousIntent, g->curve, g->vfFloor.floor, &outgoing, why, sizeof(why)))) {
            lb_log("curve-authoring: transition binding refused (%s)\n", why); return failed;
        }
        auto result = build_curve_targets(g, &incoming, targetOffsets, pointMask, 0, rebuilding);
        if (result.failed) return result;
        const bool oldInVf = service_scoped_graphics_offset_in_vf(outgoing, false, true);
        int ordinal = 0;
        for (int i = 0; i < VF_NUM_POINTS; ++i) {
            const int pointOrdinal = ordinal;
            if (i < g->vfFloor.floor.graphicsEnd && g->curve[i].freq_kHz) ++ordinal;
            if (!service_scoped_point_owned(outgoing, i, pointOrdinal, oldInVf) || pointMask[i]) continue;
            if (vf_point_target_status(g->vfFloor.floor, g->curve, i) != VF_POINT_TARGET_OK) {
                if (desired_curve_point_present(&outgoing, i)) return failed;
                continue;
            }
            targetOffsets[i] = 0; pointMask[i] = result.pointMask[i] = true;
            result.pointCapturedIntent[i] = result.pointExactOffset[i] = true;
            result.pointCapturedOffsetHz[i] = result.pointExactOffsetHz[i] = 0;
            result.pointVoltageUv[i] = g->curve[i].volt_uV;
            ++result.pointCount;
            if (cleanupPointCountOut) ++*cleanupPointCountOut;
        }
        return result;
    }
    int mn = 0, mx = 0;
    curve_offset_range_khz(g, &mn, &mx);
    (void)mx;
    return linux_build_curve_transition_targets(
        g->curve, g->freqOffsets, previousIntent, committedIntent, mn,
        targetOffsets, pointMask, cleanupPointCountOut);
}
