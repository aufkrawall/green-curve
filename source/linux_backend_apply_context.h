// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The Linux apply transaction's context and its curve-target build.  Split
// out of linux_backend_mutation.cpp for the file-size guidance and included
// from the position it occupied, so the amalgamated ordering is unchanged.
#ifndef GREEN_CURVE_LINUX_BACKEND_APPLY_CONTEXT_H
#define GREEN_CURVE_LINUX_BACKEND_APPLY_CONTEXT_H
#include "curve_feasibility_message.h"
#include "scalar_curve_submission_policy.h"
#include "authoring_modeled_transition_review.h"
#include "vf_control_write_policy.h"
#include "linux_backend_curve_correction.h"
#include "linux_vf_baseline_policy.h"

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
    // Scoped VF replacement zeros its own write set under the transition
    // ceiling before rebuilding targets.
    bool resetVfBaselineBeforeCurve;
    bool transitionBuild;
    int graphicsRouteOverride; // 0=legacy inference, 1=independent scalar, 2=VF component
    // The build the phase plan was made from; a rebuild must keep its shape.
    LinuxCurveTargetBuildResult plannedBuild;
    ScalarCurveSubmissionReview scalarReview{};
    AuthoringModeledPrecheckContext modeledContext{};
};

// The curve targets for this transaction: the replacement-aware build when
// the request owns the VF domain and a committed intent exists, else plain.
static LinuxCurveTargetBuildResult linux_apply_build_curve(
    LinuxApplyTransactionContext* context, int* cleanupPointCountOut,
    bool rebuilding = false, bool useTransactionBaseline = false) {
    if (cleanupPointCountOut) *cleanupPointCountOut = 0;
    if (context->transitionBuild)
        return build_curve_transition_targets(context->gpu, context->previousIntent,
            context->committedIntent, context->curveTargets, context->curveMask,
            cleanupPointCountOut, rebuilding, useTransactionBaseline,
            context->resetVfBaselineBeforeCurve);
    return build_curve_targets(context->gpu, context->desired,
        context->curveTargets, context->curveMask, context->graphicsRouteOverride,
        rebuilding, nullptr, useTransactionBaseline,
        context->resetVfBaselineBeforeCurve);
}

// Curve targets are built before any phase runs.  When an earlier phase
// zeroed a non-zero NVML graphics offset or reset the VF baseline, rebuild
// them from a fresh VF readback (see linux_curve_targets_rebuild_after_scalar()).
static bool linux_apply_rebuild_curve_targets(LinuxApplyTransactionContext* context) {
    if (!context->rebuildCurveTargets) return true;
    LinuxGpuState* g = context->gpu;
    if (!nvapi_read_curve(g)) {
        lb_log("apply: curve re-read after baseline/offset clear failed;"
               " refusing to write targets built over the old baseline\n");
        return false;
    }
    const bool freshBaseline = context->desired->resetOcBeforeApply ||
        context->resetVfBaselineBeforeCurve;
    const bool scalarZero = context->desired->resetOcBeforeApply ||
        context->composedGpuOffset ||
        (context->snapshot && context->snapshot->gpuOffsetValid &&
         context->snapshot->gpuOffsetMHz == 0);
    // A scoped reset can leave externally owned offsets in the live sample.
    // Use shaped evidence for that sample, including the unowned prefix.
    const bool unshapedVf = !linux_curve_has_nonzero_offsets(g->curve, g->freqOffsets,
        g->freqOffsetsHz, g->vfOffsetsHzValid);
    const bool transactionBaseline = freshBaseline && scalarZero && unshapedVf;
    int cleanup = 0;
    LinuxCurveTargetBuildResult rebuilt =
        linux_apply_build_curve(context, &cleanup, true, transactionBaseline);
    if (rebuilt.failed || rebuilt.deferred) {
        lb_log("curve-authoring: fresh target compilation remains unresolved; refusing VF write\n");
        return false;
    }
    const LinuxCurveTargetBuildResult& planned = context->plannedBuild;
    if (!linux_curve_rebuild_keeps_shape(planned, rebuilt)) {
        const int ci = linux_curve_rebuild_first_changed_point(planned, rebuilt);
        lb_log("apply: curve rebuild after baseline/offset clear changed shape"
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
    const DesiredSettings* intended = context->transitionBuild ? context->committedIntent : context->desired;
    if (intended && (curve_authoring_needs_exact_targets(intended) || freshBaseline)) {
        DesiredSettings rebound{};
        const DesiredSettings* planIntent = intended;
        if (curve_authoring_needs_exact_targets(intended)) {
            char why[160]{};
            if (!desired_rebind_topology(*intended, g->curve, g->vfFloor.floor, &rebound, why, sizeof(why))) {
                lb_log("curve-authoring: rebuilt intent binding refused (%s)\n", why); return false;
            }
            planIntent = &rebound;
        }
        int componentKHz[VF_NUM_POINTS]{};
        gc_i64 componentHz[VF_NUM_POINTS]{};
        bool stockMask[VF_NUM_POINTS]{};
        auto in = linux_backend_curve_plan_input(g, planIntent, context->snapshot,
            context->curveTargets, context->curveMask, componentKHz, componentHz, stockMask);
        if (transactionBaseline) {
            in.evidence = CURVE_PLAN_EVIDENCE_TRANSACTION_BASELINE;
            const bool vfComponent = !planIntent->authoring.graphicsPrecise ||
                planIntent->authoring.graphicsSurface == DESIRED_GRAPHICS_VF_COMPONENT;
            in.provenanceMode = apply_lock_pretail_provenance_mode(vfComponent, true);
        }
        const CurvePlanResult checked = curve_plan_evaluate(in);
        curve_plan_log(checked, "post-baseline", [](const char* line) { lb_log("%s", line); });
        if (checked.disposition == CURVE_PLAN_REFUSED || checked.disposition == CURVE_PLAN_UNAVAILABLE || rebuilt.deferred)
            return false;
    }
    lb_log("apply: curve targets rebuilt after baseline/offset clear"
           " (points=%d cleanup=%d composed=%d vfBaselineReset=%d)\n",
           rebuilt.pointCount, cleanup, rebuilt.composedGpuOffset ? 1 : 0,
           context->resetVfBaselineBeforeCurve ? 1 : 0);
    context->plannedBuild = rebuilt;
    return true;
}

// Legacy full reset uses every populated point. Scoped preparation receives
// the already planned write set, including points released by the old profile.
// A fresh sample measures the curve after the selected offsets settle.
static bool linux_apply_reset_baseline_vf_curve(LinuxGpuState* g, const bool* writeMask = nullptr) {
    int vfResetTargets[VF_NUM_POINTS] = {};
    gc_i64 vfResetHz[VF_NUM_POINTS] = {};
    bool vfResetMask[VF_NUM_POINTS] = {};
    const int unmaskedCount = linux_vf_baseline_reset_mask(g->curve, writeMask, vfResetMask);
    const int count = linux_vf_baseline_reset_mask(g->curve, writeMask, vfResetMask, g->vfMask);
    lb_log("apply: VF baseline reset scope=%s points=%d skippedUneditable=%d\n",
           writeMask ? "scoped" : "full", count, unmaskedCount - count);
    if (!apply_curve_offsets_verified(g, vfResetTargets, vfResetMask, 2, vfResetHz, vfResetMask)) {
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
static bool linux_apply_verify_curve_write(LinuxApplyTransactionContext* context,
    bool logFailure = true) {
    LinuxGpuState* g = context->gpu;
    char why[160] = {};
    if (!nvapi_read_vf_snapshot(g, why, sizeof(why))) {
        if (logFailure) lb_log("curve verify: post-write curve re-read failed (%s)\n", why);
        return false;
    }
    const DesiredSettings* intended = context->transitionBuild ? context->committedIntent : context->desired;
    char detail[160] = {};
    bool ok = linux_verify_curve_absolute_targets(g->curve, context->plannedBuild,
        detail, sizeof(detail), g->curveFrequencyHz, g->vfFrequenciesHzValid) &&
        linux_verify_curve_captured_targets(g->freqOffsetsHz,
            context->plannedBuild, detail, sizeof(detail), g->vfOffsetsHzValid) &&
        linux_verify_projected_offsets(g->curve, g->freqOffsets, context->plannedBuild,
            context->curveTargets, intended, detail, sizeof(detail));
    if (ok && intended && curve_authoring_needs_exact_targets(intended)) {
        ok = linux_verify_authoring_flatten_monotonicity(
            g->curve, g->curveFrequencyHz, g->freqOffsetsHz,
            g->vfFrequenciesHzValid, g->vfOffsetsHzValid,
            context->plannedBuild, intended->hasLock ? intended->lockMode : LOCK_MODE_NONE,
            intended->hasLock ? intended->lockCi : -1, detail, sizeof(detail));
    }
    if (!ok && logFailure) lb_log("curve verify: %s\n", detail);
    return ok;
}

static bool linux_apply_resolve_scope(LinuxGpuState* g, const DesiredSettings* d,
    const DesiredSettings* previous, ServiceScopedApplyPlan* plan, char* why, size_t whySize) {
    vf_floor_binding_update(&g->vfFloor, g->curve, g->freqOffsets, VF_NUM_POINTS, 0);
    if (!service_build_scoped_apply_plan(previous, *d, g->curve, g->vfFloor.floor,
            false, plan, why, whySize, true)) return false;
    lb_log("apply-scope: scope=%u cleanupPoints=%d retainedLock=%d remapped=%d releasesCurveCeiling=%d domains=0x%X\n",
        (unsigned)d->applicationScope, plan->cleanupPoints, plan->retainedLock ? 1 : 0, plan->remappedPoints,
        plan->releasesCurveCeiling ? 1 : 0, (unsigned)service_desired_mutation_domains(&plan->hardware));
    return true;
}

#endif
