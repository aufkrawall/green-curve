// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
// Included after the exact named setter. Recovery uses the same durable mask.
static bool nvapi_release_graphics_handoff(const DesiredSettings& d,
    ScalarCurveSubmissionReview& review, bool guardInstalled) {
    if (!review.outgoingSurface || review.outgoingReleased) return true;
    scalar_curve_note_write(&review, false);
    if (review.installedGuardRequired && !guardInstalled) {
        debug_log("graphics-handoff: refusing release without installed protection\n"); return false;
    }
    const auto release = graphics_offset_release_plan(review.outgoingSurface, g_nvml_api, g_app.nvmlDevice);
    if (!graphics_offset_control_ready(release) ||
        (long long)release.before.value * 1000000 != review.outgoingBeforeHz) {
        debug_log("graphics-handoff: outgoing control changed before release\n"); return false;
    }
    const gc_u32 incoming = desired_named_graphics_surface(d);
    if (review.beforeValid) {
        const auto next = graphics_offset_control_plan(d, g_nvml_api, g_app.nvmlDevice);
        if (!graphics_offset_control_ready(next) || (long long)next.before.value * 1000000 != review.beforeHz) {
            debug_log("graphics-handoff: incoming control changed before release\n"); return false;
        }
    }
    if (!nvapi_prepare_named_gpu_offset(review.outgoingSurface) ||
        (incoming && !nvapi_prepare_named_gpu_offset(incoming))) return false;
    if (release.before.value && !nvapi_write_named_gpu_offset(0, review.outgoingSurface, &review)) return false;
    review.outgoingReleased = true;
    // Keep the outgoing ownership until the incoming write or complete handover.
    // A subsequent read-only refusal must preserve ownership of an owned zero.
    debug_log("graphics-handoff: released surface=%u previousHz=%lld attempted=%d protected=%d\n",
        (unsigned)review.outgoingSurface, review.outgoingBeforeHz, review.writeAttempted ? 1 : 0, guardInstalled ? 1 : 0);
    return true;
}
static bool nvapi_graphics_baseline_review(const DesiredSettings& d, ScalarCurveSubmissionReview& review) {
    const gc_u32 incoming = desired_named_graphics_surface(d);
    if (!incoming || !review.beforeValid) return true;
    const auto current = nvml_exact_clock_read(g_nvml_api, g_app.nvmlDevice, NVML_CLOCK_GRAPHICS, incoming);
    if (!current.valid || current.value) return false;
    review.beforeHz = 0; return true;
}
static bool nvapi_verify_graphics_handoff(const DesiredSettings& d, const ScalarCurveSubmissionReview& review) {
    if (!review.outgoingSurface) return true;
    if (!review.outgoingReleased) return false;
    const gc_u32 incoming = desired_named_graphics_surface(d);
    const gc_u32 surfaces[]{review.outgoingSurface, incoming};
    bool ok = true;
    for (gc_u32 surface : surfaces) {
        if (!surface) continue;
        const int expected = surface == incoming ? (int)(desired_graphics_hz(d) / 1000000) : 0;
        const auto current = nvml_exact_clock_read(g_nvml_api, g_app.nvmlDevice, NVML_CLOCK_GRAPHICS, surface);
        if (!current.valid || current.value != expected) {
            g_app.gpuOffsetUncertainSurface = surface; g_app.readback.gpuOffset = false; ok = false;
            debug_log("graphics-handoff: final mismatch surface=%u expected=%d actual=%d readable=%d\n",
                (unsigned)surface, expected, current.value, current.valid ? 1 : 0);
        }
    }
    return ok;
}
static void nvapi_retire_graphics_handoff(const DesiredSettings& d, const ScalarCurveSubmissionReview& review) {
    if (!review.outgoingSurface || !review.outgoingReleased) return;
    const gc_u32 incoming = desired_named_graphics_surface(d);
    g_app.gpuOffsetRecoverySurfaces = graphics_scalar_recovery_bit(incoming);
    g_app.gpuOffsetOwnedSurface = incoming;
    g_app.gpuOffsetUncertainSurface = 0;
}
template<class Guard>
static bool apply_release_graphics_handoff(const DesiredSettings& d, ScalarCurveSubmissionReview& review,
    Guard& guard, char* why, size_t size, bool* attempted) {
    if (nvapi_release_graphics_handoff(d, review, guard.armed)) return true;
    if (review.refusedWithoutWrite)
        return apply_authoring_scalar_review_refusal(guard, why, size, attempted);
    return apply_recover_clock_failure(guard, "Named graphics control handoff failed", why, size);
}
