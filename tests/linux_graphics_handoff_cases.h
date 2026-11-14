// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Production submission helpers with scripted driver calls and lock boundaries.
struct LinuxApplyTransactionContext {
    LinuxGpuState* gpu;
    const DesiredSettings* desired;
    const DesiredSettings* committedIntent;
    ScalarCurveSubmissionReview scalarReview{};
    bool composedGpuOffset = false;
};
static bool g_linuxCeilingArmed;
static int handoffUnlocks;
static bool linux_apply_reset_baseline(LinuxGpuState* g, const DesiredSettings* d) {
    return linux_write_named_graphics(g, desired_named_graphics_surface(*d), 0);
}
static bool linux_apply_write_final_lock(LinuxGpuState*, const DesiredSettings*) { ++handoffUnlocks; return true; }
#include "linux_apply_graphics_submission.h"

static int linux_graphics_handoff_cases() {
    Driver state{}; LinuxGpuState g{};
    DesiredSettings d{}; d.hasGpuOffset = 1; d.gpuOffsetMHz = 100;
    d.authoring.graphicsPrecise = 1; d.authoring.graphicsSurface = DESIRED_GRAPHICS_GLOBAL;
    d.authoring.graphicsOffsetHz = 100000000;
    LinuxHardwareSnapshot snapshot{}; ScalarCurveSubmissionReview review{}; char why[256]{};
    auto prepare = [&] {
        initialize(&state, &g); snapshot = {}; review = {}; handoffUnlocks = 0; g_linuxCeilingArmed = false;
        state.p0Value = -50; state.globalValue = 17;
        g.graphicsRecovery.ownedSurface = CLOCK_OFFSET_SURFACE_P0;
        g.graphicsRecovery.covered = GRAPHICS_SCALAR_RECOVERY_P0;
        g.nvml.setGpuLockedClocks = scalar_guard_set; g.nvml.resetGpuLockedClocks = scalar_guard_reset;
        return linux_capture_named_graphics(&g, &snapshot, GRAPHICS_SCALAR_RECOVERY_P0) &&
            linux_bind_graphics_snapshot(&g, &d, &snapshot, why, sizeof(why));
    };
    if (!prepare() || linux_authoring_scalar_transition_precheck(&g, &d, snapshot, review, why, sizeof(why)) ||
        state.p0Writes || state.globalWrites || state.capWrites) return 9122;
    d.hasLock = 1; d.lockMode = LOCK_MODE_FLATTEN; d.lockMHz = 1000;
    if (!prepare() || !linux_authoring_scalar_transition_precheck(&g, &d, snapshot, review, why, sizeof(why)) ||
        !review.installedGuardRequired || review.outgoingSurface != CLOCK_OFFSET_SURFACE_P0 || !review.beforeValid ||
        !linux_graphics_apply_precheck(&g, &d, true, why, sizeof(why), true)) return 9123;
    LinuxApplyTransactionContext context{&g, &d, &d, review};
    if (linux_apply_graphics_submission(&context) || state.p0Writes || state.globalWrites ||
        !context.scalarReview.refusedWithoutWrite) return 9124;
    g_linuxCeilingArmed = true; ++state.p0Value;
    if (linux_apply_graphics_submission(&context) || state.p0Writes || state.globalWrites) return 9125;
    --state.p0Value; ++state.globalValue;
    if (linux_apply_graphics_submission(&context) || state.p0Writes || state.globalWrites) return 9126;
    --state.globalValue;
    if (!linux_apply_graphics_submission(&context) || state.p0Value || state.globalValue != 100 ||
        state.p0Writes != 1 || state.globalWrites != 1 || !context.scalarReview.writeAttempted ||
        context.scalarReview.refusedWithoutWrite || g.graphicsRecovery.attempted != GRAPHICS_SCALAR_RECOVERY_ALL ||
        !linux_apply_graphics_finish(&context) || handoffUnlocks != 1 ||
        g.graphicsRecovery.covered != GRAPHICS_SCALAR_RECOVERY_ALL) return 9127;
    ++state.p0Value; // A later VF write invalidates the release before unlock.
    if (linux_apply_graphics_finish(&context) || handoffUnlocks != 1 || !g.graphicsRecovery.uncertain) return 9128;
    auto restore = [&] {
        return graphics_scalar_restore(g.graphicsRecovery, snapshot.graphicsPreimage,
            [&](gc_u32 surface, int value) { return nvml_set_clock_offset(&g, NVML_CLOCK_GRAPHICS, value, surface); },
            [&](gc_u32 surface, int value) { return linux_named_graphics_matches(&g, surface, value); });
    };
    if (!restore() || state.p0Value != -50 || state.globalValue != 17 ||
        g.graphicsRecovery.covered != GRAPHICS_SCALAR_RECOVERY_P0 || g.graphicsRecovery.uncertain) return 9129;
    if (!prepare() || !linux_authoring_scalar_transition_precheck(&g, &d, snapshot, review, why, sizeof(why))) return 9130;
    context.scalarReview = review; g_linuxCeilingArmed = true;
    if (!linux_release_graphics_handoff(&g, &d, context.scalarReview, true)) return 9131;
    state.failGlobalRead = true;
    if (linux_apply_graphics_submission(&context) || context.scalarReview.refusedWithoutWrite ||
        scalar_curve_rollback_phases(context.scalarReview, LINUX_MUTATION_GPU_OFFSET, LINUX_MUTATION_GPU_OFFSET) !=
            LINUX_MUTATION_GPU_OFFSET || g.graphicsRecovery.attempted != GRAPHICS_SCALAR_RECOVERY_P0) return 9132;
    state.failGlobalRead = false;
    if (!restore() || state.p0Value != -50 || state.globalValue != 17) return 9133;
    if (!prepare() || !linux_authoring_scalar_transition_precheck(&g, &d, snapshot, review, why, sizeof(why))) return 9134;
    context.scalarReview = review; g_linuxCeilingArmed = true; state.globalWriteClobbersP0 = true;
    if (!linux_apply_graphics_submission(&context) || linux_apply_graphics_finish(&context) ||
        handoffUnlocks || !g.graphicsRecovery.uncertain) return 9135;
    state.globalWriteClobbersP0 = false;
    if (!restore()) return 9136;
    // VF routing releases the owned API, preserving the other observed API.
    d.authoring.graphicsSurface = DESIRED_GRAPHICS_VF_COMPONENT;
    if (!prepare() || !linux_authoring_scalar_transition_precheck(&g, &d, snapshot, review, why, sizeof(why))) return 9137;
    context.scalarReview = review; context.composedGpuOffset = true; g_linuxCeilingArmed = true;
    if (!linux_apply_graphics_submission(&context) || state.p0Value || state.globalValue != 17 ||
        state.p0Writes != 1 || state.globalWrites || !linux_apply_graphics_finish(&context)) return 9138;
    // Owned zero needs no clamp or setter, and verification must not reset an external pin.
    d.hasLock = 0; d.lockMode = LOCK_MODE_NONE; d.lockMHz = 0;
    if (!prepare()) return 9139;
    state.p0Value = snapshot.graphicsPreimage.p0MHz = 0;
    if (!linux_authoring_scalar_transition_precheck(&g, &d, snapshot, review, why, sizeof(why)) ||
        review.installedGuardRequired) return 9140;
    context.scalarReview = review;
    if (!linux_apply_graphics_submission(&context) || !linux_apply_graphics_finish(&context) ||
        state.p0Writes || state.globalWrites || handoffUnlocks) return 9141;
    // An authorized baseline changes only the incoming review binding to its proven zero.
    d.authoring.graphicsSurface = DESIRED_GRAPHICS_GLOBAL; d.hasLock = 1;
    d.lockMode = LOCK_MODE_FLATTEN; d.lockMHz = 1000; d.resetOcBeforeApply = 1;
    if (!prepare() || !linux_authoring_scalar_transition_precheck(&g, &d, snapshot, review, why, sizeof(why))) return 9142;
    context.scalarReview = review; context.composedGpuOffset = false; g_linuxCeilingArmed = true;
    if (!linux_apply_graphics_baseline(&context) || context.scalarReview.beforeHz ||
        snapshot.graphicsPreimage.globalMHz != 17 || !linux_apply_graphics_submission(&context) ||
        !linux_apply_graphics_finish(&context) || state.p0Value || state.globalValue != 100) return 9143;
    d.resetOcBeforeApply = 0; d.hasLock = 0; d.lockMode = LOCK_MODE_NONE; d.lockMHz = 0;
    if (!prepare()) return 9146;
    state.p0Value = snapshot.graphicsPreimage.p0MHz = 0;
    if (!linux_authoring_scalar_transition_precheck(&g, &d, snapshot, review, why, sizeof(why))) return 9147;
    context.scalarReview = review; state.globalWriteClobbersP0 = true;
    if (!linux_apply_graphics_submission(&context) || linux_apply_graphics_finish(&context) ||
        g.graphicsRecovery.attempted != GRAPHICS_SCALAR_RECOVERY_ALL || restore() ||
        g.graphicsRecovery.uncertain != GRAPHICS_SCALAR_RECOVERY_ALL || handoffUnlocks) return 9148;
    state.globalWriteClobbersP0 = false;
    if (!restore() || state.p0Value || state.globalValue != 17 || g.graphicsRecovery.uncertain) return 9149;
    // A VF baseline must use the prepared global API even when P0 is preferred.
    initialize(&state, &g); snapshot = {}; review = {}; handoffUnlocks = 0;
    state.p0Value = 17; state.globalValue = -50;
    g.graphicsRecovery.ownedSurface = CLOCK_OFFSET_SURFACE_GLOBAL;
    g.graphicsRecovery.covered = GRAPHICS_SCALAR_RECOVERY_GLOBAL;
    g.nvml.setGpuLockedClocks = scalar_guard_set; g.nvml.resetGpuLockedClocks = scalar_guard_reset;
    d.authoring.graphicsSurface = DESIRED_GRAPHICS_VF_COMPONENT; d.resetOcBeforeApply = 1;
    d.hasLock = 1; d.lockMode = LOCK_MODE_FLATTEN; d.lockMHz = 1000;
    if (!linux_capture_named_graphics(&g, &snapshot, GRAPHICS_SCALAR_RECOVERY_GLOBAL) ||
        !linux_authoring_scalar_transition_precheck(&g, &d, snapshot, review, why, sizeof(why))) return 9150;
    context.scalarReview = review; context.composedGpuOffset = true; g_linuxCeilingArmed = true;
    if (!linux_apply_graphics_baseline(&context) || !linux_apply_graphics_submission(&context) ||
        !linux_apply_graphics_finish(&context) || state.p0Value != 17 || state.globalValue ||
        state.p0Writes || state.globalWrites != 2 || handoffUnlocks != 1) return 9151;
    return 0;
}
