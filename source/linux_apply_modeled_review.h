// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Modeled-curve transition context assembly and post-reset baseline review for
// the Linux backend transaction. Split out of linux_backend_mutation.cpp to
// keep translation-unit source files compact.
#ifndef GREEN_CURVE_LINUX_APPLY_MODELED_REVIEW_H
#define GREEN_CURVE_LINUX_APPLY_MODELED_REVIEW_H

static inline AuthoringModeledPrecheckContext linux_apply_build_modeled_context(
    const LinuxGpuState* g, const DesiredSettings* d, const LinuxHardwareSnapshot& snapshot,
    unsigned int outgoingCeilingMHz, bool releasesCurveCeiling) {
    AuthoringModeledPrecheckContext ctx{};
    ctx.composition = snapshot.composition;
    ctx.pinMutationAuthorized = d && authoring_pin_mutation_authorized(*d, releasesCurveCeiling);
    ctx.pinAuthorityExplicit = true;
    ctx.clampAvailable = g && g->nvml.setGpuLockedClocks != nullptr &&
        g->nvml.resetGpuLockedClocks != nullptr;
    ctx.clampAvailabilityExplicit = true;
    ctx.outgoingCeilingHz = (gc_i64)outgoingCeilingMHz * 1000000LL;
    return ctx;
}

static inline bool linux_apply_post_reset_modeled_review(LinuxApplyTransactionContext* context) {
    if (!context || !context->gpu || !context->desired) return true;
    if (!context->ceiling) {
        // Fail closed: the review arms and reads the transaction's clamp.
        lb_log("linux_apply: modeled review refused without a ceiling transaction\n");
        return false;
    }
    const DesiredSettings* intended = context->transitionBuild ? context->committedIntent : context->desired;
    if (!intended || !curve_authoring_needs_exact_targets(intended)) return true;
    const gc_u32 incomingSurface = desired_named_graphics_surface(*intended);
    const bool multiStep = context->modeledContext.multiStepCurve;
    if (!context->scalarReview.deferredModeledCheck && !incomingSurface && !multiStep) return true;
    LinuxGpuState* g = context->gpu;
    if (!g->vfFrequenciesHzValid || !g->vfOffsetsHzValid) {
        lb_log("curve-authoring: post-reset exact curve samples are unavailable\n");
        return false;
    }
    int componentKHz[VF_NUM_POINTS]{};
    gc_i64 componentHz[VF_NUM_POINTS]{};
    bool stockMask[VF_NUM_POINTS]{};
    CurvePlanInput in = linux_backend_curve_plan_input(g, intended, context->snapshot,
        context->curveTargets, context->curveMask, componentKHz, componentHz, stockMask);
    const bool compose = intended->authoring.graphicsPrecise
        ? intended->authoring.graphicsSurface == DESIRED_GRAPHICS_VF_COMPONENT
        : context->graphicsRouteOverride == 2 ||
          (context->graphicsRouteOverride == 0 && desired_gpu_offset_uses_curve(intended));
    in.evidence = CURVE_PLAN_EVIDENCE_TRANSACTION_BASELINE;
    in.provenanceMode = apply_lock_pretail_provenance_mode(compose, true);
    const CurvePlanResult plan = curve_plan_evaluate(in);
    curve_plan_log(plan, "linux-authoring-post-reset", linux_curve_plan_log_line);
    if (plan.disposition != CURVE_PLAN_REPRESENTABLE) return false;
    CurveAuthoringTargets postTargets;
    CurveAuthoringTargetIssue issue;
    if (!curve_authoring_build_targets(in, g->backend, g->vfMask, compose, postTargets, issue, false)) {
        lb_log("curve-authoring: post-reset targets could not resolve ci=%d status=%d\n",
            issue.ci, (int)issue.status);
        return false;
    }
    if (incomingSurface) {
        if (!linux_named_graphics_matches(g, incomingSurface, 0)) {
            lb_log("curve-authoring: post-reset graphics scalar baseline is nonzero or unreadable\n");
            return false;
        }
        context->scalarReview.beforeValid = true;
        context->scalarReview.beforeHz = 0;
    }
    const gc_i64 afterHz = incomingSurface ? desired_graphics_hz(*intended) : 0;
    AuthoringModeledTransitionInput mIn{};
    mIn.baseline = &in;
    mIn.targets = &postTargets;
    mIn.composition = context->modeledContext.composition;
    mIn.pinMutationAuthorized = context->modeledContext.pinAuthorityExplicit
        ? context->modeledContext.pinMutationAuthorized
        : authoring_pin_mutation_authorized(*intended);
    mIn.clampAvailable = context->modeledContext.clampAvailabilityExplicit
        ? context->modeledContext.clampAvailable
        : (g->nvml.setGpuLockedClocks && g->nvml.resetGpuLockedClocks);
    mIn.scalarBeforeCurve = incomingSurface && postTargets.pointCount > 0 && afterHz > 0;
    mIn.multiStepCurve = multiStep;
    mIn.scalarBeforeHz = 0;
    mIn.scalarAfterHz = afterHz;
    mIn.outgoingCeilingHz = (gc_i64)context->ceiling->outgoingCeilingMHz * 1000000LL;
    mIn.resetsToStock = true;
    AuthoringModeledTransitionOutcome outcome{};
    char mWhy[256]{};
    if (!authoring_modeled_transition_post_reset(mIn, context->scalarReview, outcome, mWhy, sizeof(mWhy))) {
        lb_log("curve-authoring: post-reset modeled transition refused (%s)\n", mWhy);
        return false;
    }
    if (outcome.adopted) {
        char armWhy[256]{};
        if (!linux_apply_arm_modeled_post_reset(g, *context->ceiling, intended, context->scalarReview.modeledCeiling,
                armWhy, sizeof(armWhy))) {
            lb_log("curve-authoring: %s\n", armWhy);
            return false;
        }
    }
    context->scalarReview.deferredModeledCheck = false;
    return true;
}

// When a scoped VF baseline reset accompanies a named graphics scalar, zero
// the planned VF write set and run the post-reset modeled transition review
// before raising the scalar (so the modeled ceiling is armed first).
static inline bool linux_apply_prepare_scoped_vf_before_scalar(LinuxApplyTransactionContext* context) {
    if (!context || !context->gpu || !context->desired) return true;
    if (!context->ceiling) {
        // Fail closed: the review arms and reads the transaction's clamp.
        lb_log("linux_apply: modeled review refused without a ceiling transaction\n");
        return false;
    }
    const DesiredSettings* d = context->desired;
    if (!desired_named_graphics_surface(*d) || !context->resetVfBaselineBeforeCurve ||
        d->resetOcBeforeApply || context->vfBaselineResetDone)
        return true;
    if (!linux_release_graphics_handoff(context->gpu, d, context->scalarReview, context->ceiling->armed))
        return false;
    lb_log("apply: running scoped VF baseline reset and modeled review before named graphics scalar\n");
    context->vfBaselineResetAttempted = true;
    if (!linux_apply_reset_baseline_vf_curve(context->gpu, context->plannedBuild.pointMask))
        return false;
    context->vfBaselineResetDone = true;
    return linux_apply_post_reset_modeled_review(context);
}

#endif // GREEN_CURVE_LINUX_APPLY_MODELED_REVIEW_H
