// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "graphics_offset_control.h"
#include "scalar_curve_submission_policy.h"

static inline bool linux_capture_named_graphics(LinuxGpuState* g, LinuxHardwareSnapshot* snapshot, gc_u32 mask) {
    return graphics_scalar_capture(g->graphicsRecovery, snapshot->graphicsPreimage, mask,
        [&](gc_u32 surface, int& value) {
            if (!nvml_exact_clock_surface_available(g->nvml, NVML_CLOCK_GRAPHICS, surface)) return false;
            const auto read = nvml_exact_clock_read(g->nvml, g->nvmlDevice, NVML_CLOCK_GRAPHICS, surface);
            value = read.value; return read.valid;
        });
}

// Preserve independent domains when a named control is unreadable. Only a
// transaction that can submit that control needs its exact preimage.
static inline void linux_capture_graphics_recovery_snapshot(LinuxGpuState* g, LinuxHardwareSnapshot* snapshot) {
    linux_capture_named_graphics(g, snapshot, 0);
    for (gc_u32 bit = 1; bit <= GRAPHICS_SCALAR_RECOVERY_GLOBAL; bit <<= 1)
        if (g->graphicsRecovery.covered & bit) linux_capture_named_graphics(g, snapshot, bit);
}
static inline bool linux_graphics_recovery_snapshot_complete(const LinuxGpuState* g, const LinuxHardwareSnapshot* snapshot) {
    return (g->graphicsRecovery.covered & ~snapshot->graphicsPreimage.validMask) == 0;
}
static inline bool linux_graphics_apply_precheck(const LinuxGpuState* g, const DesiredSettings* d,
    bool graphicsWork, char* why, size_t size, bool handoffReviewed = false) {
    const gc_u32 requested = desired_named_graphics_surface(*d);
    const gc_u32 owned = graphics_scalar_recovery_bit(g->graphicsRecovery.ownedSurface);
    if ((requested && !(g->graphicsRecovery.prepared & graphics_scalar_recovery_bit(requested))) ||
        g->graphicsRecovery.uncertain || (g->graphicsRecovery.covered & ~owned) ||
        (owned && graphicsWork && requested != g->graphicsRecovery.ownedSurface && !handoffReviewed)) {
        if (why && size) snprintf(why, size, "Named graphics recovery or a control-surface handback requires explicit Reset");
        lb_log("graphics-offset: apply refused covered=%u uncertain=%u owned=%u requested=%u\n",
            g->graphicsRecovery.covered, g->graphicsRecovery.uncertain, g->graphicsRecovery.ownedSurface, requested);
        return false;
    }
    return true;
}

static inline bool linux_write_named_graphics(LinuxGpuState* g, gc_u32 surface, int targetMHz,
    ScalarCurveSubmissionReview* review = nullptr) {
    scalar_curve_note_write(review, false);
    auto& state = g->graphicsRecovery;
    const gc_u32 bit = graphics_scalar_recovery_bit(surface);
    if (!bit || !(state.prepared & bit)) {
        lb_log("graphics-offset: durable preparation missing surface=%u\n", surface);
        return false;
    }
    const auto before = state;
    state.covered |= bit; state.uncertain |= bit;
    // A named API may affect another covered API, including owned zero.
    state.attempted |= state.covered;
    const auto written = nvml_exact_clock_write(g->nvml, g->nvmlDevice, NVML_CLOCK_GRAPHICS, surface, targetMHz);
    if (!written.attempted) state = before;
    scalar_curve_note_write(review, written.attempted);
    if (written.verified) state.uncertain &= ~bit;
    lb_log("graphics-offset: precise surface=%u target=%d attempted=%d write=%d read=%d valid=%d actual=%d verified=%d\n",
        surface, targetMHz, written.attempted ? 1 : 0, (int)written.writeStatus,
        (int)written.after.status, written.after.valid ? 1 : 0, written.after.value, written.verified ? 1 : 0);
    return written.verified;
}

static inline bool linux_named_graphics_matches(LinuxGpuState* g, gc_u32 surface, int expectedMHz) {
    const auto read = nvml_exact_clock_read(g->nvml, g->nvmlDevice, NVML_CLOCK_GRAPHICS, surface);
    const bool matches = read.valid && read.value == expectedMHz;
    if (!matches)
        lb_log("graphics-offset: final recovery mismatch surface=%u expected=%d actual=%d readable=%d status=%d\n",
            surface, expectedMHz, read.value, read.valid ? 1 : 0, (int)read.status);
    return matches;
}

static inline bool linux_verify_graphics_recovery_zero(LinuxGpuState* g, bool retireCoverage) {
    const gc_u32 mask = g->graphicsRecovery.covered;
    if (!graphics_scalar_recovery_mask_valid(mask)) return false;
    bool ok = true;
    for (gc_u32 bit = 1; bit <= GRAPHICS_SCALAR_RECOVERY_GLOBAL; bit <<= 1) {
        if (!(mask & bit)) continue;
        const bool matches = linux_named_graphics_matches(g, graphics_scalar_recovery_surface(bit), 0);
        if (!matches) g->graphicsRecovery.uncertain |= bit;
        ok &= matches;
    }
    if (ok && retireCoverage) {
        g->graphicsRecovery.covered = g->graphicsRecovery.uncertain = 0;
        g->graphicsRecovery.ownedSurface = 0;
    }
    return ok;
}
template<class Unlock>
static inline bool linux_finalize_named_graphics_reset(LinuxGpuState* g, Unlock unlock) {
    if (!linux_verify_graphics_recovery_zero(g, false) || !unlock()) return false;
    g->graphicsRecovery.covered = g->graphicsRecovery.uncertain = g->graphicsRecovery.ownedSurface = 0;
    return true;
}
static inline bool linux_reset_named_graphics(LinuxGpuState* g, bool retireCoverage = true) {
    const gc_u32 mask = g->graphicsRecovery.covered;
    if (!mask || (mask & ~g->graphicsRecovery.prepared)) return false;
    bool ok = true;
    for (gc_u32 bit = 1; bit <= GRAPHICS_SCALAR_RECOVERY_GLOBAL; bit <<= 1)
        if (mask & bit) ok &= linux_write_named_graphics(g, graphics_scalar_recovery_surface(bit), 0);
    const bool verified = linux_verify_graphics_recovery_zero(g, ok && retireCoverage);
    return ok && verified;
}

static inline bool linux_authoring_scalar_transition_precheck(const LinuxGpuState* g, const DesiredSettings* d,
    const LinuxHardwareSnapshot& snapshot, ScalarCurveSubmissionReview& review, char* why, size_t size) {
    review = {};
    if (!d) return false;
    auto p = scalar_curve_submission_plan(*d, snapshot.gpuOffsetValid && snapshot.gpuOffsetExact &&
        snapshot.gpuOffsetSurface == desired_named_graphics_surface(*d),
        (long long)snapshot.gpuOffsetMHz * 1000000, g->freqOffsetsHz, g->vfOffsetsHzValid,
        g->nvml.setGpuLockedClocks && g->nvml.resetGpuLockedClocks);
    const gc_u32 owned = g->graphicsRecovery.ownedSurface;
    const gc_u32 incoming = desired_named_graphics_surface(*d);
    if (owned && ((d->hasGpuOffset && incoming != owned) || d->resetOcBeforeApply)) {
        const auto release = graphics_offset_release_plan(owned, g->nvml, g->nvmlDevice);
        const auto& before = snapshot.graphicsPreimage;
        const int value = owned == CLOCK_OFFSET_SURFACE_P0 ? before.p0MHz : before.globalMHz;
        const bool valid = graphics_offset_control_ready(release) &&
            (before.validMask & graphics_scalar_recovery_bit(owned)) && release.before.value == value &&
            (g->graphicsRecovery.prepared & graphics_scalar_recovery_bit(owned));
        const auto handoff = scalar_curve_handoff_plan(*d, valid, (long long)value * 1000000,
            g->nvml.setGpuLockedClocks && g->nvml.resetGpuLockedClocks);
        if (!scalar_curve_submission_allowed(handoff) ||
            (scalar_curve_submission_allowed(p) && handoff.status == SCALAR_CURVE_INSTALL_GUARD)) p = handoff;
        if (scalar_curve_submission_allowed(p)) {
            review.outgoingSurface = owned; review.outgoingBeforeHz = (long long)value * 1000000;
        }
    }
    if (p.status != SCALAR_CURVE_NO_SPLIT)
        lb_log("scalar-curve: preflight status=%d loweredCaptured=%d baselineDependent=%d\n",
            (int)p.status, p.loweredCaptured, p.baselineDependent ? 1 : 0);
    if (!scalar_curve_submission_allowed(p)) {
        if (why && size) snprintf(why, size, "Curve transition: %s. Nothing was changed.", scalar_curve_submission_reason(p.status));
        return false;
    }
    review.installedGuardRequired = p.status == SCALAR_CURVE_INSTALL_GUARD;
    review.beforeValid = (p.status != SCALAR_CURVE_NO_SPLIT || review.outgoingSurface) && incoming &&
        snapshot.gpuOffsetValid && snapshot.gpuOffsetExact && snapshot.gpuOffsetSurface == incoming;
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
    if (p.surface == CLOCK_OFFSET_SURFACE_P0) snapshot->graphicsPreimage.p0MHz = p.before.value;
    else snapshot->graphicsPreimage.globalMHz = p.before.value;
    snapshot->graphicsPreimage.validMask |= graphics_scalar_recovery_bit(p.surface);
    snapshot->availableMutationDomains |= SERVICE_MUTATION_DOMAIN_GPU_OFFSET;
    return true;
}

static inline bool linux_apply_named_graphics_offset(LinuxGpuState* g, const DesiredSettings* d,
    ScalarCurveSubmissionReview* review = nullptr) {
    scalar_curve_note_write(review, false);
    const auto p = graphics_offset_control_plan(*d, g->nvml, g->nvmlDevice);
    if (!graphics_offset_control_ready(p)) {
        lb_log("graphics-offset: fresh preflight refused status=%d surface=%u\n", (int)p.status, (unsigned)p.surface);
        return false;
    }
    if (review && review->beforeValid && (gc_i64)p.before.value * 1000000 != review->beforeHz) {
        scalar_curve_note_write(review, false);
        lb_log("graphics-offset: reviewed current changed surface=%u; refusing before scalar submission\n", (unsigned)p.surface);
        return false;
    }
    const gc_u32 bit = graphics_scalar_recovery_bit(p.surface);
    if (!(g->graphicsRecovery.prepared & bit)) {
        lb_log("graphics-offset: refusing unprepared named control surface=%u\n", (unsigned)p.surface);
        return false;
    }
    const bool ok = p.status == GRAPHICS_OFFSET_INERT ||
        linux_write_named_graphics(g, p.surface, p.targetMHz, review);
    if (ok) {
        g->graphicsRecovery.covered |= bit;
        g->graphicsRecovery.uncertain &= ~bit;
        g->graphicsRecovery.ownedSurface = p.surface;
    }
    return ok;
}

static inline bool linux_release_graphics_handoff(LinuxGpuState* g, const DesiredSettings* d,
    ScalarCurveSubmissionReview& review, bool guardInstalled) {
    if (!review.outgoingSurface || review.outgoingReleased) return true;
    scalar_curve_note_write(&review, false);
    if (review.installedGuardRequired && !guardInstalled) {
        lb_log("graphics-handoff: refusing release without installed protection\n"); return false;
    }
    const auto release = graphics_offset_release_plan(review.outgoingSurface, g->nvml, g->nvmlDevice);
    if (!graphics_offset_control_ready(release) ||
        (long long)release.before.value * 1000000 != review.outgoingBeforeHz) {
        lb_log("graphics-handoff: outgoing control changed before release surface=%u\n", review.outgoingSurface);
        return false;
    }
    if (review.beforeValid) {
        const auto incoming = graphics_offset_control_plan(*d, g->nvml, g->nvmlDevice);
        if (!graphics_offset_control_ready(incoming) ||
            (long long)incoming.before.value * 1000000 != review.beforeHz) {
            lb_log("graphics-handoff: incoming control changed before release\n"); return false;
        }
    }
    if (release.before.value && !linux_write_named_graphics(g, review.outgoingSurface, 0, &review)) return false;
    review.outgoingReleased = true;
    lb_log("graphics-handoff: released surface=%u previousHz=%lld attempted=%d protected=%d\n",
        review.outgoingSurface, review.outgoingBeforeHz, review.writeAttempted ? 1 : 0, guardInstalled ? 1 : 0);
    return true;
}

static inline bool linux_verify_graphics_handoff(LinuxGpuState* g, const DesiredSettings* d,
    const ScalarCurveSubmissionReview& review) {
    if (!review.outgoingSurface) return true;
    const gc_u32 incoming = desired_named_graphics_surface(*d);
    bool ok = review.outgoingReleased && linux_named_graphics_matches(g, review.outgoingSurface,
        incoming == review.outgoingSurface ? (int)(desired_graphics_hz(*d) / 1000000) : 0);
    if (incoming && incoming != review.outgoingSurface)
        ok &= linux_named_graphics_matches(g, incoming, (int)(desired_graphics_hz(*d) / 1000000));
    if (!ok) {
        g->graphicsRecovery.uncertain |= g->graphicsRecovery.attempted;
        lb_log("graphics-handoff: combined final readback failed; recovery retained\n");
    }
    return ok;
}
