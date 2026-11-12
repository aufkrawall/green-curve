// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The Linux apply transaction's context and its curve-target build.  Split
// out of linux_backend_mutation.cpp for the file-size guidance and included
// from the position it occupied, so the amalgamated ordering is unchanged.
#ifndef GREEN_CURVE_LINUX_BACKEND_APPLY_CONTEXT_H
#define GREEN_CURVE_LINUX_BACKEND_APPLY_CONTEXT_H

struct LinuxApplyTransactionContext {
    LinuxGpuState* gpu;
    const DesiredSettings* desired;
    const LinuxHardwareSnapshot* snapshot;
    // The intent this daemon last committed.  NVML exposes no getter for a
    // configured locked-clock range, so this is the only record of an OUTGOING
    // pin -- and the transition ceiling is bounded by the lower of the
    // outgoing and incoming entitlements, so it has to reach the arming phase.
    const DesiredSettings* committedIntent;
    const DesiredSettings* previousIntent;
    int curveTargets[VF_NUM_POINTS];
    bool curveMask[VF_NUM_POINTS];
    int fanTargetPercent;
    bool fanUseDriverAuto;
    // The offset is composed into the curve: the GPU_OFFSET phase, when it
    // runs, writes 0 (clears a stale NVML offset) instead of the request.
    bool composedGpuOffset;
    // A non-zero NVML offset is zeroed before the curve write: rebuild the
    // curve targets from a fresh readback in the CURVE phase.
    bool rebuildCurveTargets;
    bool transitionBuild;
    // The build the phase plan was made from; a rebuild must keep its shape.
    LinuxCurveTargetBuildResult plannedBuild;
};

// The curve targets for this transaction: the replacement-aware build when
// the request owns the VF domain and a committed intent exists, else plain.
static LinuxCurveTargetBuildResult linux_apply_build_curve(
    LinuxApplyTransactionContext* context, int* cleanupPointCountOut) {
    if (cleanupPointCountOut) *cleanupPointCountOut = 0;
    if (context->transitionBuild)
        return build_curve_transition_targets(context->gpu, context->previousIntent,
            context->committedIntent, context->curveTargets, context->curveMask,
            cleanupPointCountOut);
    return build_curve_targets(context->gpu, context->desired,
        context->curveTargets, context->curveMask);
}

// Curve targets are built before any phase runs.  When an earlier phase
// zeroed a non-zero NVML graphics offset, rebuild them from a fresh VF
// readback (see linux_curve_targets_rebuild_after_scalar()).
static bool linux_apply_rebuild_curve_targets(LinuxApplyTransactionContext* context) {
    if (!context->rebuildCurveTargets) return true;
    LinuxGpuState* g = context->gpu;
    if (!nvapi_read_curve(g)) {
        lb_log("apply: curve re-read after NVML offset clear failed;"
               " refusing to write targets built over the old offset\n");
        return false;
    }
    int cleanup = 0;
    LinuxCurveTargetBuildResult rebuilt =
        linux_apply_build_curve(context, &cleanup);
    const LinuxCurveTargetBuildResult& planned = context->plannedBuild;
    if (!linux_curve_rebuild_keeps_shape(planned, rebuilt)) {
        const int ci = linux_curve_rebuild_first_changed_point(planned, rebuilt);
        lb_log("apply: curve rebuild after NVML offset clear changed shape"
               " (points %d->%d composed %d->%d lockTail %d->%d"
               " firstCi=%d write=%d->%d voltageUv=%u->%u); refusing to write it\n",
               planned.pointCount, rebuilt.pointCount,
               planned.composedGpuOffset ? 1 : 0, rebuilt.composedGpuOffset ? 1 : 0,
               planned.lockTail ? 1 : 0, rebuilt.lockTail ? 1 : 0, ci,
               ci >= 0 && planned.pointMask[ci] ? 1 : 0,
               ci >= 0 && rebuilt.pointMask[ci] ? 1 : 0,
               ci >= 0 ? planned.pointVoltageUv[ci] : 0u,
               ci >= 0 ? rebuilt.pointVoltageUv[ci] : 0u);
        return false;
    }
    lb_log("apply: curve targets rebuilt after NVML offset clear"
           " (points=%d cleanup=%d composed=%d)\n",
           rebuilt.pointCount, cleanup, rebuilt.composedGpuOffset ? 1 : 0);
    return true;
}

// LINUX_MUTATION_RESET_BASELINE's per-point VF curve reset-to-zero (parity
// with Windows reset_oc_before_gui_apply()): a point this apply does not
// re-boost would otherwise keep the PREVIOUS profile's raw offset, since a
// point outside this transaction's write set never appears in
// curveTargets/curveMask at all. Always runs when resetOcBeforeApply is
// requested, not only when a scalar was nonzero, matching the unconditional
// Windows contract. Re-reads the full curve afterward so every curve/lock
// target this apply computes next is measured from the settled baseline,
// not the pre-reset sample.
static bool linux_apply_reset_baseline_vf_curve(LinuxGpuState* g) {
    int vfResetTargets[VF_NUM_POINTS] = {};
    bool vfResetMask[VF_NUM_POINTS] = {};
    for (int i = 0; i < VF_NUM_POINTS; i++)
        vfResetMask[i] = g->curve[i].freq_kHz != 0;
    if (!apply_curve_offsets_verified(g, vfResetTargets, vfResetMask, 2)) {
        lb_log("apply: VF curve offsets did not reset to stock baseline\n");
        return false;
    }
    char why[160] = {};
    if (!nvapi_read_vf_snapshot(g, why, sizeof(why))) {
        lb_log("apply: curve re-read after VF baseline reset failed (%s)\n", why);
        return false;
    }
    return true;
}

// LINUX_MUTATION_CURVE's post-write verification: offset convergence alone
// does not prove the curve landed where requested, since the driver can
// move a point's stock base between the write and this check (one VF bin
// under load on Blackwell). Re-read the full curve and verify every
// absolute-intent point's resulting frequency, matching Windows
// gpu_backend_apply_verify.h. LINUX_MUTATION_LOCK, which releases the
// F-APPLY-CEILING transition ceiling, only runs if this returns true.
static bool linux_apply_verify_curve_write(LinuxApplyTransactionContext* context) {
    LinuxGpuState* g = context->gpu;
    char why[160] = {};
    if (!nvapi_read_vf_snapshot(g, why, sizeof(why))) {
        lb_log("curve verify: post-write curve re-read failed (%s)\n", why);
        return false;
    }
    char detail[160] = {};
    bool ok = linux_verify_curve_absolute_targets(g->curve, context->plannedBuild,
        detail, sizeof(detail));
    if (!ok) lb_log("curve verify: %s\n", detail);
    return ok;
}

#endif
