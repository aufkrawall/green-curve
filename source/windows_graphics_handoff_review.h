// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "graphics_offset_control.h"
#include "scalar_curve_submission_policy.h"

static bool apply_graphics_handoff_precheck(const DesiredSettings* d,
    ScalarCurveSubmissionReview& review, char* why, size_t size) {
    if (!d) return false;
    const gc_u32 owned = g_app.gpuOffsetOwnedSurface;
    const gc_u32 incoming = desired_named_graphics_surface(*d);
    if (!owned || (!d->resetOcBeforeApply && (!d->hasGpuOffset || incoming == owned))) return true;
    const auto release = graphics_offset_release_plan(owned, g_nvml_api, g_app.nvmlDevice);
    const auto plan = scalar_curve_handoff_plan(*d, graphics_offset_control_ready(release),
        (long long)release.before.value * 1000000,
        g_nvml_api.setGpuLockedClocks && g_nvml_api.resetGpuLockedClocks);
    debug_log("graphics-handoff: preflight outgoing=%u incoming=%u status=%d previousMHz=%d\n",
        (unsigned)owned, (unsigned)incoming, (int)plan.status, release.before.value);
    if (!scalar_curve_submission_allowed(plan)) {
        set_message(why, size, "Graphics control handoff: %s. Nothing was changed.", scalar_curve_submission_reason(plan.status));
        return false;
    }
    if (incoming) {
        const auto next = graphics_offset_control_plan(*d, g_nvml_api, g_app.nvmlDevice);
        if (!graphics_offset_control_ready(next)) {
            set_message(why, size, "Graphics control handoff: the incoming control is unavailable. Nothing was changed.");
            return false;
        }
        review.beforeValid = true; review.beforeHz = (long long)next.before.value * 1000000;
    }
    review.outgoingSurface = owned;
    review.outgoingBeforeHz = (long long)release.before.value * 1000000;
    review.installedGuardRequired = plan.status == SCALAR_CURVE_INSTALL_GUARD;
    return true;
}
