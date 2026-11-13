// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "graphics_offset_control.h"
#include "scalar_curve_submission_policy.h"

static inline bool linux_authoring_scalar_transition_precheck(const LinuxGpuState* g, const DesiredSettings* d,
    const LinuxHardwareSnapshot& snapshot, ScalarCurveSubmissionReview& review, char* why, size_t size) {
    review = {};
    if (!d) return false;
    const auto p = scalar_curve_submission_plan(*d, snapshot.gpuOffsetValid && snapshot.gpuOffsetExact &&
        snapshot.gpuOffsetSurface == desired_named_graphics_surface(*d),
        (long long)snapshot.gpuOffsetMHz * 1000000, g->freqOffsetsHz, g->vfOffsetsHzValid,
        g->nvml.setGpuLockedClocks && g->nvml.resetGpuLockedClocks);
    if (p.status != SCALAR_CURVE_NO_SPLIT)
        lb_log("scalar-curve: preflight status=%d loweredCaptured=%d baselineDependent=%d\n",
            (int)p.status, p.loweredCaptured, p.baselineDependent ? 1 : 0);
    if (!scalar_curve_submission_allowed(p)) {
        if (why && size) snprintf(why, size, "Curve transition: %s. Nothing was changed.", scalar_curve_submission_reason(p.status));
        return false;
    }
    review.installedGuardRequired = p.status == SCALAR_CURVE_INSTALL_GUARD;
    review.beforeValid = p.status != SCALAR_CURVE_NO_SPLIT && snapshot.gpuOffsetValid && snapshot.gpuOffsetExact;
    review.beforeHz = (long long)snapshot.gpuOffsetMHz * 1000000;
    return true;
}

// Replace only the rollback scalar channel with a read of the reviewed
// surface. A preferred getter for a different surface cannot supply its
// preimage. Failure leaves the complete hardware snapshot untouched.
static inline bool linux_bind_graphics_snapshot(LinuxGpuState* g, const DesiredSettings* d,
    LinuxHardwareSnapshot* snapshot, char* why, size_t whySize) {
    if (!d || !d->authoring.graphicsPrecise ||
        d->authoring.graphicsSurface == DESIRED_GRAPHICS_VF_COMPONENT) return true;
    const auto p = graphics_offset_control_plan(*d, g->nvml, g->nvmlDevice);
    if (!graphics_offset_control_ready(p)) {
        if (why && whySize) snprintf(why, whySize, "Graphics offset: %s", graphics_offset_control_status_text(p.status));
        lb_log("graphics-offset: preflight refused status=%d surface=%u targetHz=%lld\n",
            (int)p.status, (unsigned)p.surface, desired_graphics_hz(*d));
        return false;
    }
    snapshot->gpuOffsetValid = snapshot->gpuOffsetExact = true;
    snapshot->gpuOffsetSurface = p.surface; snapshot->gpuOffsetMHz = p.before.value;
    snapshot->availableMutationDomains |= SERVICE_MUTATION_DOMAIN_GPU_OFFSET;
    return true;
}

static inline bool linux_apply_named_graphics_offset(LinuxGpuState* g, const DesiredSettings* d,
    ScalarCurveSubmissionReview* review = nullptr) {
    if (review) review->refusedWithoutWrite = false;
    const auto p = graphics_offset_control_plan(*d, g->nvml, g->nvmlDevice);
    if (!graphics_offset_control_ready(p)) {
        lb_log("graphics-offset: fresh preflight refused status=%d surface=%u\n", (int)p.status, (unsigned)p.surface);
        return false;
    }
    if (review && review->beforeValid && (gc_i64)p.before.value * 1000000 != review->beforeHz) {
        review->refusedWithoutWrite = true;
        lb_log("graphics-offset: reviewed current changed surface=%u; refusing before scalar submission\n", (unsigned)p.surface);
        return false;
    }
    if (p.status == GRAPHICS_OFFSET_INERT) return true;
    const auto written = nvml_exact_clock_write(g->nvml, g->nvmlDevice, NVML_CLOCK_GRAPHICS, p.surface, p.targetMHz);
    lb_log("graphics-offset: precise surface=%u target=%d attempted=%d write=%d read=%d valid=%d actual=%d verified=%d\n",
        (unsigned)p.surface, p.targetMHz, written.attempted ? 1 : 0, (int)written.writeStatus,
        (int)written.after.status, written.after.valid ? 1 : 0, written.after.value, written.verified ? 1 : 0);
    return written.verified;
}
