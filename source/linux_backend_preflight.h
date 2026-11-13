// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Read-only transaction preflight, included after backend dependencies.
#ifndef GREEN_CURVE_LINUX_BACKEND_PREFLIGHT_H
#define GREEN_CURVE_LINUX_BACKEND_PREFLIGHT_H

#include "power_ratio_control.h"
#include "nvml_exact_clock_control.h"
#include "linux_memory_curve_policy.h"

static bool linux_backend_preflight(LinuxGpuState* g, const DesiredSettings* d,
                                    const LinuxHardwareSnapshot* snapshot,
                                    char* err, size_t errSize, int graphicsRouteOverride = 0) {
    if (!g || !d || !g->nvmlReady || !g->writeIdentityResolved) {
        gc_strlcpy(err, errSize, "GPU write target is unavailable or not uniquely resolved");
        return false;
    }
    if (!desired_scalar_intent_valid(d)) {
        gc_strlcpy(err, errSize, "invalid precise scalar intent"); return false;
    }
    if (!desired_authoring_valid(d) || !desired_authoring_runtime_ready(d)) {
        gc_strlcpy(err, errSize, "bound authoring replay requires destination review");
        lb_log("apply-authoring: refused before writes (destination review required)\n"); return false;
    }
    gc_u32 requestedDomains = service_desired_mutation_domains(d);
    const bool independentGpu = graphicsRouteOverride == 1 && d->hasGpuOffset;
    if (independentGpu) requestedDomains |= SERVICE_MUTATION_DOMAIN_GPU_OFFSET;
    gc_u32 unavailableDomains = service_unavailable_mutation_domains(
        SERVICE_CMD_APPLY, d, snapshot->availableMutationDomains);
    if (independentGpu) unavailableDomains |= SERVICE_MUTATION_DOMAIN_GPU_OFFSET & ~snapshot->availableMutationDomains;
    bool powerInert = linux_power_request_is_inert(d, snapshot, g);
    if (powerInert && (unavailableDomains & SERVICE_MUTATION_DOMAIN_POWER)) {
        unavailableDomains &= ~(gc_u32)SERVICE_MUTATION_DOMAIN_POWER;
        lb_log("power: request is the board default (%d%%) and this board exposes no "
               "power control surface; not treating it as an unavailable domain\n",
               d->powerLimitPct);
    }
    if (unavailableDomains != 0) {
        gc_snprintf(err, errSize,
            "requested GPU domains are unavailable (requested=0x%02x available=0x%02x missing=0x%02x)",
            (unsigned)requestedDomains, (unsigned)snapshot->availableMutationDomains,
            (unsigned)unavailableDomains);
        return false;
    }
    bool hardLock = d->hasLock && d->lockMode == LOCK_MODE_HARD && d->lockMHz > 0;
    if (d->resetOcBeforeApply && !g->nvml.resetGpuLockedClocks &&
        !linux_clock_control_proven_absent(g)) {
        gc_strlcpy(err, errSize, "OC baseline reset cannot release locked clocks safely"); return false;
    }
    const bool gpuUsesCurve = graphicsRouteOverride == 0 ? desired_gpu_offset_uses_curve(d) : graphicsRouteOverride == 2;
    if (((d->hasGpuOffset && !gpuUsesCurve) ||
         d->resetOcBeforeApply) &&
        (!snapshot->gpuOffsetValid || (!g->nvml.setGpcClkVfOffset && !g->nvml.setClockOffsets))) {
        gc_strlcpy(err, errSize, "GPU offset cannot be snapshotted and written safely"); return false;
    }
    if (d->hasMemOffset &&
        (!snapshot->memOffsetValid || (!g->nvml.setMemClkVfOffset && !g->nvml.setClockOffsets))) {
        gc_strlcpy(err, errSize, "memory offset cannot be snapshotted and written safely"); return false;
    }
    if (d->memOffsetPrecise && (!snapshot->memOffsetExact ||
        (d->memOffsetSurface && d->memOffsetSurface != snapshot->memOffsetSurface) ||
        !nvml_exact_clock_surface_available(g->nvml, NVML_CLOCK_MEM, snapshot->memOffsetSurface))) {
        gc_strlcpy(err, errSize, "exact memory control surface cannot be snapshotted and written safely"); return false;
    }
    if (!linux_memory_curve_precheck(g, d, snapshot, err, errSize)) return false;
    if (d->hasPowerLimit && !powerInert &&
        (!snapshot->powerValid || !g->nvml.setPowerLimit)) {
        gc_strlcpy(err, errSize, "power limit cannot be snapshotted and written safely"); return false;
    }
    if (d->hasPowerLimit && !powerInert) {
        const PowerRatioControlPlan power = power_ratio_control_plan(
            desired_power_ratio(*d),
            power_ratio_read_nvml(g->nvml, g->nvmlDevice));
        if (power.status != POWER_RATIO_READY) {
            gc_snprintf(err, errSize, "power limit preflight: %s", power_ratio_control_status_text(power.status));
            return false;
        }
    }
    if (desired_has_curve_write(d)) {
        char why[160] = {};
        if (!snapshot->curveValid || !g->backend || !g->backend->writeSupported ||
            !linux_backend_curve_plausible(g, why, sizeof(why))) {
            gc_snprintf(err, errSize, "VF curve write preflight failed: %s", why[0] ? why : "unsupported");
            return false;
        }
    }
    if (hardLock && !g->nvml.setGpuLockedClocks) {
        gc_strlcpy(err, errSize, "hard clock locking is unavailable"); return false;
    }
    if (d->hasFan && !snapshot->fanValid) {
        gc_strlcpy(err, errSize, "fan state cannot be snapshotted safely"); return false;
    }
    if ((d->hasXbarOffsetKhz || d->hasXbarMsvddOffsetUv) &&
        !snapshot->xbarValid) {
        gc_strlcpy(err, errSize,
            "XBAR state cannot be snapshotted safely"); return false;
    }
    if (d->hasSysClkOffsetKhz && !snapshot->sysClkValid) {
        gc_strlcpy(err, errSize,
            "SYS clock state cannot be snapshotted safely"); return false;
    }
    if (d->hasVideoClkOffsetKhz && !snapshot->videoClkValid) {
        gc_strlcpy(err, errSize,
            "video clock state cannot be snapshotted safely"); return false;
    }
    return true;
}

#endif
