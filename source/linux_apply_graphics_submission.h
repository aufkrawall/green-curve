// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
// Included after the transaction context and baseline helpers.
static bool linux_apply_graphics_baseline(LinuxApplyTransactionContext* context) {
    auto* g = context->gpu;
    auto& review = context->scalarReview;
    if (!linux_release_graphics_handoff(g, context->desired, review, g_linuxCeilingArmed)) return false;
    DesiredSettings baseline = *context->desired;
    if (review.outgoingSurface && !desired_named_graphics_surface(baseline)) {
        // Reset the known owned API; a preferred getter is not authority to
        // mutate another API that this transaction did not prepare.
        baseline.hasGpuOffset = 1; baseline.gpuOffsetMHz = 0;
        baseline.authoring.graphicsPrecise = 1; baseline.authoring.graphicsOffsetHz = 0;
        baseline.authoring.graphicsSurface = review.outgoingSurface == CLOCK_OFFSET_SURFACE_P0
            ? DESIRED_GRAPHICS_P0 : DESIRED_GRAPHICS_GLOBAL;
    }
    if (!linux_apply_reset_baseline(g, &baseline)) return false;
    const gc_u32 incoming = desired_named_graphics_surface(*context->desired);
    if (incoming && review.beforeValid) {
        // This is an authorized zero from the baseline, not a new drift sample.
        if (!linux_named_graphics_matches(g, incoming, 0)) return false;
        review.beforeHz = 0;
    }
    return true;
}
static bool linux_apply_graphics_submission(LinuxApplyTransactionContext* context) {
    auto* g = context->gpu;
    const auto* d = context->desired;
    auto& review = context->scalarReview;
    if (!linux_release_graphics_handoff(g, d, review, g_linuxCeilingArmed)) return false;
    if (desired_named_graphics_surface(*d)) return linux_apply_named_graphics_offset(g, d, &review);
    // The owned named control was released explicitly. A VF component must
    // not also zero an unrelated preferred scalar through legacy routing.
    if (review.outgoingSurface && context->composedGpuOffset) return true;
    scalar_curve_note_write(&review, true);
    return nvml_set_clock_offset(g, NVML_CLOCK_GRAPHICS, context->composedGpuOffset ? 0 : d->gpuOffsetMHz);
}
static bool linux_apply_graphics_finish(LinuxApplyTransactionContext* context) {
    if (!linux_verify_graphics_handoff(context->gpu, context->desired, context->scalarReview)) return false;
    // A verification-only phase preserves an unrelated external pin.
    if (!context->desired->hasLock && !g_linuxCeilingArmed) return true;
    return linux_apply_write_final_lock(context->gpu,
        context->committedIntent ? context->committedIntent : context->desired);
}
