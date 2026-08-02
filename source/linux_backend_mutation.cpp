// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// ===========================================================================
// Apply / reset  (ports apply_desired_settings_service ordering)
// ===========================================================================

static bool desired_has_curve_write(const DesiredSettings* d) {
    if (!d) return false;
    for (int i = 0; i < VF_NUM_POINTS; ++i)
        if (d->hasCurvePoint[i]) return true;
    return (d->hasLock && (d->lockMode == LOCK_MODE_FLATTEN ||
                           d->lockMode == LOCK_MODE_HARD)) ||
           (d->hasGpuOffset && d->gpuOffsetExcludeLowCount > 0);
}

static bool desired_gpu_offset_uses_curve(const DesiredSettings* d) {
    if (!d || !d->hasGpuOffset) return false;
    if (d->gpuOffsetExcludeLowCount > 0 || d->hasLock) return true;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (d->hasCurvePoint[i]) return true;
    }
    return false;
}

bool linux_backend_capture_snapshot(LinuxGpuState* g, LinuxHardwareSnapshot* snapshot,
                                    char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!g || !snapshot || !g->nvmlReady) {
        gc_strlcpy(err, errSize, "GPU backend is not ready for snapshot");
        return false;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    NvmlClockOffsetReadback gpuOffset =
        nvml_read_clock_offset(g, NVML_CLOCK_GRAPHICS);
    if (gpuOffset.offsetValid) {
        snapshot->gpuOffsetMHz = gpuOffset.offsetMHz;
        snapshot->gpuOffsetValid = true;
    }
    NvmlClockOffsetReadback memOffset =
        nvml_read_clock_offset(g, NVML_CLOCK_MEM);
    if (memOffset.offsetValid) {
        snapshot->memOffsetMHz = memOffset.offsetMHz;
        snapshot->memOffsetValid = true;
    }
    if (g->nvml.getPowerLimit &&
        g->nvml.getPowerLimit(g->nvmlDevice, &snapshot->powerLimitmW) == NVML_SUCCESS)
        snapshot->powerValid = true;
    if (g->gpuHandle && g->backend && g->backend->writeSupported &&
        linux_vf_snapshot_authoritative(
            g->vfInfoFresh, g->vfStatusFresh, g->vfControlFresh,
            g->vfStructureValid, g->vfSnapshotFresh) &&
        g->numPopulated > 0) {
        snapshot->curveValid = true;
        for (int i = 0; i < VF_NUM_POINTS; ++i) {
            snapshot->curveOffsets[i] = g->freqOffsets[i];
            snapshot->curveMask[i] = g->curve[i].freq_kHz != 0;
        }
    }
    unsigned int fans = 0;
    if (g->nvml.getNumFans && g->nvml.getNumFans(g->nvmlDevice, &fans) == NVML_SUCCESS) {
        if (fans > MAX_GPU_FANS) fans = MAX_GPU_FANS;
        snapshot->fanCount = fans;
        snapshot->fanValid = fans > 0;
        for (unsigned int i = 0; i < fans; ++i) {
            int intended = 0;
            if (nvml_read_fan_intent(g, i, &intended)) {
                snapshot->fanTargetPercent[i] = (unsigned int)(intended < 0 ? 0 : intended);
                snapshot->fanTargetKnown[i] = true;
            } else {
                // No intent getter: fall back to the measured duty so rollback
                // still has something to aim at, but remember it is a guess so
                // verification does not demand an exact match against telemetry.
                int measured = nvml_read_fan_measured(g, i);
                if (measured < 0) snapshot->fanValid = false;
                else snapshot->fanTargetPercent[i] = (unsigned int)measured;
                snapshot->fanTargetKnown[i] = false;
            }
            if (!g->nvml.getFanControlPolicy ||
                g->nvml.getFanControlPolicy(g->nvmlDevice, i, &snapshot->fanPolicy[i]) != NVML_SUCCESS)
                snapshot->fanValid = false;
        }
    }
    snapshot->valid = snapshot->gpuOffsetValid || snapshot->memOffsetValid ||
                       snapshot->powerValid || snapshot->curveValid || snapshot->fanValid;
    bool gpuOffsetWritable = g->nvml.setGpcClkVfOffset ||
        g->nvml.setClockOffsets;
    bool memOffsetWritable = g->nvml.setMemClkVfOffset ||
        g->nvml.setClockOffsets;
    bool fanAutoWritable = g->nvml.setDefaultFanSpeed ||
        g->nvml.setFanControlPolicy;
    if (g->writeIdentityResolved) {
        if (snapshot->gpuOffsetValid && gpuOffsetWritable)
            snapshot->availableMutationDomains |=
                SERVICE_MUTATION_DOMAIN_GPU_OFFSET;
        if (snapshot->memOffsetValid && memOffsetWritable)
            snapshot->availableMutationDomains |=
                SERVICE_MUTATION_DOMAIN_MEM_OFFSET;
        if (snapshot->powerValid && g->nvml.setPowerLimit)
            snapshot->availableMutationDomains |=
                SERVICE_MUTATION_DOMAIN_POWER;
        if (snapshot->curveValid)
            snapshot->availableMutationDomains |=
                SERVICE_MUTATION_DOMAIN_VF_CURVE;
        if (g->nvml.setGpuLockedClocks && g->nvml.resetGpuLockedClocks)
            snapshot->availableMutationDomains |=
                SERVICE_MUTATION_DOMAIN_LOCK;
        if (snapshot->fanValid && g->nvml.setFanSpeed && fanAutoWritable)
            snapshot->availableMutationDomains |=
                SERVICE_MUTATION_DOMAIN_FAN;
        if (snapshot->gpuOffsetValid && snapshot->memOffsetValid &&
            gpuOffsetWritable && memOffsetWritable &&
            g->nvml.resetGpuLockedClocks)
            snapshot->availableMutationDomains |=
                SERVICE_MUTATION_DOMAIN_RESET_BASELINE;
    }
    g->health.availableMutationDomains = snapshot->availableMutationDomains;

    // Mirror the same facts into the per-domain capability probe the Windows
    // side reports, deriving rather than re-probing so the two cannot diverge.
    //
    // The UNPROBED-vs-REFUSED split follows the same conservatism as
    // gpu_capability_classify(): a domain we could READ but cannot WRITE only
    // because the driver lacks the setter is an older driver, not absent
    // hardware, so it stays UNPROBED and raises no warning.  A domain whose
    // read itself failed is positive evidence and becomes REFUSED.
    {
        struct {
            gc_u32 mask;
            bool readValid;
            bool writable;
        } domains[] = {
            { SERVICE_MUTATION_DOMAIN_GPU_OFFSET, snapshot->gpuOffsetValid, gpuOffsetWritable },
            { SERVICE_MUTATION_DOMAIN_MEM_OFFSET, snapshot->memOffsetValid, memOffsetWritable },
            { SERVICE_MUTATION_DOMAIN_POWER, snapshot->powerValid, g->nvml.setPowerLimit != nullptr },
            { SERVICE_MUTATION_DOMAIN_VF_CURVE, snapshot->curveValid, true },
            { SERVICE_MUTATION_DOMAIN_LOCK, true,
              g->nvml.setGpuLockedClocks != nullptr && g->nvml.resetGpuLockedClocks != nullptr },
            { SERVICE_MUTATION_DOMAIN_FAN, snapshot->fanValid,
              g->nvml.setFanSpeed != nullptr && fanAutoWritable },
        };
        for (size_t i = 0; i < sizeof(domains) / sizeof(domains[0]); ++i) {
            gc_u32 cap;
            if (snapshot->availableMutationDomains & domains[i].mask) {
                cap = GPU_DOMAIN_CAP_AVAILABLE;
            } else if (!g->writeIdentityResolved || !domains[i].writable) {
                cap = GPU_DOMAIN_CAP_UNPROBED;
            } else {
                cap = domains[i].readValid ? GPU_DOMAIN_CAP_UNPROBED
                                           : GPU_DOMAIN_CAP_REFUSED;
            }
            gpu_capability_set(&g->capability, domains[i].mask, cap);
        }
        g->capability.memoryTopology = linux_platform_is_integrated_soc()
            ? GPU_MEMORY_TOPOLOGY_UNIFIED
            : GPU_MEMORY_TOPOLOGY_UNKNOWN;
    }
    g->health.capabilityMemoryTopology =
        (gc_u8)g->capability.memoryTopology;
    g->health.capabilityDomainsPacked =
        gpu_capability_pack_domains(&g->capability);
    linux_backend_publish_health(g, g->health.reason,
        g->health.driverStatus, g->health.detail);
    if (!snapshot->valid) gc_strlcpy(err, errSize, "no rollback-capable GPU state could be captured");
    return snapshot->valid;
}

bool linux_backend_restore_snapshot(LinuxGpuState* g, const LinuxHardwareSnapshot* snapshot,
                                    unsigned int phaseMask, char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!g || !snapshot || !snapshot->valid) {
        gc_strlcpy(err, errSize, "rollback snapshot is invalid");
        return false;
    }
    bool ok = true;
    bool baseline = (phaseMask & LINUX_MUTATION_RESET_BASELINE) != 0;
    if ((baseline || (phaseMask & LINUX_MUTATION_GPU_OFFSET)) && snapshot->gpuOffsetValid)
        ok &= nvml_set_clock_offset(g, NVML_CLOCK_GRAPHICS, snapshot->gpuOffsetMHz);
    if ((baseline || (phaseMask & LINUX_MUTATION_MEM_OFFSET)) && snapshot->memOffsetValid)
        ok &= nvml_set_clock_offset(g, NVML_CLOCK_MEM, snapshot->memOffsetMHz);
    if ((phaseMask & LINUX_MUTATION_POWER) && snapshot->powerValid && g->nvml.setPowerLimit) {
        bool powerOk = g->nvml.setPowerLimit(g->nvmlDevice, snapshot->powerLimitmW) == NVML_SUCCESS;
        if (powerOk && g->nvml.getPowerLimit) {
            unsigned int verify = 0;
            powerOk = g->nvml.getPowerLimit(g->nvmlDevice, &verify) == NVML_SUCCESS &&
                      verify == snapshot->powerLimitmW;
        }
        ok &= powerOk;
    }
    if ((phaseMask & LINUX_MUTATION_CURVE) && snapshot->curveValid)
        ok &= apply_curve_offsets_verified(g, snapshot->curveOffsets, snapshot->curveMask, 25);
    if ((phaseMask & LINUX_MUTATION_FAN) && snapshot->fanValid) {
        for (unsigned int i = 0; i < snapshot->fanCount; ++i) {
            bool fanOk = false;
            if (snapshot->fanPolicy[i] == NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW) {
                if (g->nvml.setDefaultFanSpeed)
                    fanOk = g->nvml.setDefaultFanSpeed(g->nvmlDevice, i) == NVML_SUCCESS;
                else if (g->nvml.setFanControlPolicy)
                    fanOk = g->nvml.setFanControlPolicy(g->nvmlDevice, i,
                        NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW) == NVML_SUCCESS;
            } else if (g->nvml.setFanSpeed) {
                fanOk = g->nvml.setFanSpeed(g->nvmlDevice, i, snapshot->fanTargetPercent[i]) == NVML_SUCCESS;
            }
            if (fanOk && snapshot->fanPolicy[i] == NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW) {
                unsigned int verifyPolicy = 0;
                fanOk = g->nvml.getFanControlPolicy &&
                        g->nvml.getFanControlPolicy(g->nvmlDevice, i, &verifyPolicy) == NVML_SUCCESS &&
                        verifyPolicy == NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW;
            } else if (fanOk) {
                // Same trap as the forward write: the measured duty is not a
                // readback.  Confirm the restored *intent* instead.
                int intended = 0;
                bool intendedKnown = nvml_read_fan_intent(g, i, &intended);
                int measured = nvml_read_fan_measured(g, i);
                fanOk = fan_manual_write_confirmed((int)snapshot->fanTargetPercent[i],
                    measured < 0 ? 0 : measured, intended, intendedKnown);
                if (!fanOk) {
                    lb_log("fan: rollback readback mismatch for fan %u "
                           "(want=%u intent=%s%d measured=%d)\n",
                           i, snapshot->fanTargetPercent[i],
                           intendedKnown ? "" : "unknown:", intended, measured);
                }
            }
            if (!fanOk) lb_log("fan: rollback verification failed for fan %u\n", i);
            ok &= fanOk;
        }
    }
    if (baseline || (phaseMask & LINUX_MUTATION_LOCK)) {
        // NVML exposes no getter for the configured locked-clock range.  Release
        // a lock written by this transaction, but never claim that the unknown
        // pre-transaction lock policy was restored exactly.
        if (g->nvml.resetGpuLockedClocks)
            g->nvml.resetGpuLockedClocks(g->nvmlDevice);
        ok = false;
    }
    if (!ok) gc_strlcpy(err, errSize, "one or more GPU rollback phases failed");
    linux_backend_refresh(g);
    return ok;
}

static bool linux_backend_preflight(LinuxGpuState* g, const DesiredSettings* d,
                                    const LinuxHardwareSnapshot* snapshot,
                                    char* err, size_t errSize) {
    if (!g || !d || !g->nvmlReady || !g->writeIdentityResolved) {
        gc_strlcpy(err, errSize, "GPU write target is unavailable or not uniquely resolved");
        return false;
    }
    gc_u32 requestedDomains = service_desired_mutation_domains(d);
    gc_u32 unavailableDomains = service_unavailable_mutation_domains(
        SERVICE_CMD_APPLY, d, snapshot->availableMutationDomains);
    if (unavailableDomains != 0) {
        gc_snprintf(err, errSize,
            "requested GPU domains are unavailable (requested=0x%02x available=0x%02x missing=0x%02x)",
            requestedDomains, snapshot->availableMutationDomains,
            unavailableDomains);
        return false;
    }
    bool hardLock = d->hasLock && d->lockMode == LOCK_MODE_HARD && d->lockMHz > 0;
    if (d->resetOcBeforeApply && !g->nvml.resetGpuLockedClocks) {
        gc_strlcpy(err, errSize, "OC baseline reset cannot release locked clocks safely"); return false;
    }
    if (((d->hasGpuOffset && !desired_gpu_offset_uses_curve(d)) ||
         d->resetOcBeforeApply) &&
        (!snapshot->gpuOffsetValid || (!g->nvml.setGpcClkVfOffset && !g->nvml.setClockOffsets))) {
        gc_strlcpy(err, errSize, "GPU offset cannot be snapshotted and written safely"); return false;
    }
    if ((d->hasMemOffset || d->resetOcBeforeApply) &&
        (!snapshot->memOffsetValid || (!g->nvml.setMemClkVfOffset && !g->nvml.setClockOffsets))) {
        gc_strlcpy(err, errSize, "memory offset cannot be snapshotted and written safely"); return false;
    }
    if (d->hasPowerLimit && (!snapshot->powerValid || !g->nvml.setPowerLimit)) {
        gc_strlcpy(err, errSize, "power limit cannot be snapshotted and written safely"); return false;
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
    return true;
}

// Build per-point absolute-target curve offsets from a DesiredSettings request.
// offset_kHz = targetMHz*1000 - stockFreq_kHz  (mirrors the Windows correction
// loop's `targetMHz*1000 - liveFreq`).  Handles FLATTEN lock by applying a
// uniform floor offset (range minimum) to tail points beyond the lock index
// (the Build-109 uniform-tail-floor approach).
static LinuxCurveTargetBuildResult build_curve_targets(LinuxGpuState* g,
    const DesiredSettings* d, int* targetOffsets, bool* pointMask) {
    int mn = 0, mx = 0;
    curve_offset_range_khz(g, &mn, &mx);
    (void)mx;
    return linux_build_curve_targets(g->curve, g->freqOffsets, d, mn,
        targetOffsets, pointMask);
}

static LinuxCurveTargetBuildResult build_curve_transition_targets(
    LinuxGpuState* g, const DesiredSettings* previousIntent,
    const DesiredSettings* committedIntent, int* targetOffsets,
    bool* pointMask, int* cleanupPointCountOut) {
    int mn = 0, mx = 0;
    curve_offset_range_khz(g, &mn, &mx);
    (void)mx;
    return linux_build_curve_transition_targets(
        g->curve, g->freqOffsets, previousIntent, committedIntent, mn,
        targetOffsets, pointMask, cleanupPointCountOut);
}

struct LinuxApplyTransactionContext {
    LinuxGpuState* gpu;
    const DesiredSettings* desired;
    const LinuxHardwareSnapshot* snapshot;
    int curveTargets[VF_NUM_POINTS];
    bool curveMask[VF_NUM_POINTS];
    int fanTargetPercent;
};

static bool linux_apply_transaction_step(void* opaque, unsigned int phase) {
    LinuxApplyTransactionContext* context = (LinuxApplyTransactionContext*)opaque;
    LinuxGpuState* g = context->gpu;
    const DesiredSettings* d = context->desired;
    switch (phase) {
        case LINUX_MUTATION_RESET_BASELINE:
            return nvml_set_clock_offset(g, NVML_CLOCK_GRAPHICS, 0) &&
                   nvml_set_clock_offset(g, NVML_CLOCK_MEM, 0) &&
                   g->nvml.resetGpuLockedClocks &&
                   g->nvml.resetGpuLockedClocks(g->nvmlDevice) == NVML_SUCCESS;
        case LINUX_MUTATION_GPU_OFFSET:
            return nvml_set_clock_offset(g, NVML_CLOCK_GRAPHICS, d->gpuOffsetMHz);
        case LINUX_MUTATION_MEM_OFFSET:
            return nvml_set_clock_offset(g, NVML_CLOCK_MEM, d->memOffsetMHz);
        case LINUX_MUTATION_POWER:
            return nvml_set_power_limit_pct(g, d->powerLimitPct);
        case LINUX_MUTATION_CURVE:
            return apply_curve_offsets_verified(g, context->curveTargets,
                                                context->curveMask, 25);
        case LINUX_MUTATION_LOCK:
            if (d->lockMode == LOCK_MODE_HARD && d->lockMHz > 0)
                return g->nvml.setGpuLockedClocks &&
                       g->nvml.setGpuLockedClocks(g->nvmlDevice, d->lockMHz,
                                                  d->lockMHz) == NVML_SUCCESS;
            return g->nvml.resetGpuLockedClocks &&
                   g->nvml.resetGpuLockedClocks(g->nvmlDevice) == NVML_SUCCESS;
        case LINUX_MUTATION_FAN:
            return nvml_set_fan(g, d->fanMode, d->fanAuto,
                context->fanTargetPercent);
        default:
            return false;
    }
}

static bool linux_apply_transaction_rollback(void* opaque, unsigned int phases) {
    LinuxApplyTransactionContext* context = (LinuxApplyTransactionContext*)opaque;
    char rollback[256] = {};
    return linux_backend_restore_snapshot(context->gpu, context->snapshot, phases,
                                          rollback, sizeof(rollback));
}

static int phase_count(unsigned int phases) {
    int count = 0;
    while (phases) { count += (int)(phases & 1u); phases >>= 1; }
    return count;
}

LinuxMutationResult linux_backend_apply(LinuxGpuState* g, const DesiredSettings* d,
                                        const DesiredSettings* previousIntent,
                                        const DesiredSettings* committedIntent,
                                        char* result, size_t resultSize) {
    LinuxHardwareSnapshot snapshot = {};
    char preflight[256] = {};
    linux_backend_refresh(g);
    if (!linux_backend_capture_snapshot(g, &snapshot, preflight, sizeof(preflight)) ||
        !linux_backend_preflight(g, d, &snapshot, preflight, sizeof(preflight))) {
        LinuxMutationResult mutation = {};
        if (result) gc_strlcpy(result, resultSize, preflight[0] ? preflight : "Apply preflight failed");
        return mutation;
    }
    bool hardLock = d->hasLock && d->lockMode == LOCK_MODE_HARD && d->lockMHz > 0;
    LinuxApplyTransactionContext context = {g, d, &snapshot, {}, {},
        d->fanPercent};
    gc_u32 requestedDomains = service_desired_mutation_domains(d);
    int cleanupPointCount = 0;
    LinuxCurveTargetBuildResult curveBuild = {};
    if ((requestedDomains & SERVICE_MUTATION_DOMAIN_VF_CURVE) != 0 &&
        committedIntent) {
        curveBuild = build_curve_transition_targets(g, previousIntent,
            committedIntent, context.curveTargets, context.curveMask,
            &cleanupPointCount);
    } else {
        curveBuild = build_curve_targets(g, d, context.curveTargets,
            context.curveMask);
    }
    if (cleanupPointCount > 0) {
        lb_log("apply: added %d zero-offset cleanup point(s) for replaced "
               "VF intent\n", cleanupPointCount);
    }
    if (d->hasFan && d->fanMode == FAN_MODE_CURVE) {
        FanCurveConfig normalized = d->fanCurve;
        fan_curve_normalize(&normalized);
        char fanErr[128] = {};
        unsigned int temperature = 0;
        if (!fan_curve_validate(&normalized, fanErr, sizeof(fanErr)) ||
            !g->nvml.getTemperature ||
            g->nvml.getTemperature(g->nvmlDevice, 0, &temperature) != NVML_SUCCESS) {
            LinuxMutationResult mutation = {};
            if (result) gc_snprintf(result, resultSize,
                "Fan curve initial target preflight failed: %s",
                fanErr[0] ? fanErr : "temperature unavailable");
            return mutation;
        }
        context.fanTargetPercent = fan_curve_interpolate_percent(&normalized,
            (int)temperature);
    }
    unsigned int requested = 0;
    if (d->resetOcBeforeApply) requested |= LINUX_MUTATION_RESET_BASELINE;
    if (d->hasGpuOffset && !curveBuild.composedGpuOffset)
        requested |= LINUX_MUTATION_GPU_OFFSET;
    if (d->hasMemOffset) requested |= LINUX_MUTATION_MEM_OFFSET;
    if (d->hasPowerLimit) requested |= LINUX_MUTATION_POWER;
    if (curveBuild.pointCount > 0)
        requested |= LINUX_MUTATION_CURVE;
    if (d->hasLock) requested |= LINUX_MUTATION_LOCK;
    if (d->hasFan) requested |= LINUX_MUTATION_FAN;
    LinuxMutationResult mutation = linux_execute_transaction(
        requested, linux_apply_transaction_step, linux_apply_transaction_rollback, &context);
    char msg[512] = {};
    gc_snprintf(msg, sizeof(msg), "%s: %d phase(s), %d failed%s%s",
                mutation.success ? "Applied" : "Apply failed",
                phase_count(mutation.attemptedPhases), phase_count(mutation.failedPhases),
                (!mutation.success && mutation.rollbackSucceeded) ? " (rolled back)" :
                (!mutation.success && mutation.rollbackAttempted) ? " (rollback uncertain)" : "",
                hardLock ? " (hard clock pin)" : "");
    if (result) gc_strlcpy(result, resultSize, msg);
    lb_log("apply: %s\n", msg);
    linux_backend_refresh(g);
    return mutation;
}

struct LinuxResetTransactionContext {
    LinuxGpuState* gpu;
    const LinuxHardwareSnapshot* snapshot;
};

static bool linux_reset_transaction_step(void* opaque, unsigned int phase) {
    LinuxResetTransactionContext* context = (LinuxResetTransactionContext*)opaque;
    LinuxGpuState* g = context->gpu;
    switch (phase) {
        case LINUX_MUTATION_LOCK:
            return g->nvml.resetGpuLockedClocks(g->nvmlDevice) == NVML_SUCCESS;
        case LINUX_MUTATION_GPU_OFFSET:
            return nvml_set_clock_offset(g, NVML_CLOCK_GRAPHICS, 0);
        case LINUX_MUTATION_MEM_OFFSET:
            return nvml_set_clock_offset(g, NVML_CLOCK_MEM, 0);
        case LINUX_MUTATION_POWER: {
            bool ok = g->nvml.setPowerLimit(g->nvmlDevice,
                (unsigned int)g->powerLimitDefaultmW) == NVML_SUCCESS;
            unsigned int verify = 0;
            return ok && g->nvml.getPowerLimit &&
                   g->nvml.getPowerLimit(g->nvmlDevice, &verify) == NVML_SUCCESS &&
                   verify == (unsigned int)g->powerLimitDefaultmW;
        }
        case LINUX_MUTATION_CURVE: {
            int targets[VF_NUM_POINTS] = {};
            bool mask[VF_NUM_POINTS];
            for (int i = 0; i < VF_NUM_POINTS; ++i)
                mask[i] = g->curve[i].freq_kHz != 0;
            return apply_curve_offsets_verified(g, targets, mask, 25);
        }
        case LINUX_MUTATION_FAN:
            return nvml_set_fan(g, FAN_MODE_AUTO, true, 0);
        default:
            return false;
    }
}

static bool linux_reset_transaction_rollback(void* opaque, unsigned int phases) {
    LinuxResetTransactionContext* context = (LinuxResetTransactionContext*)opaque;
    char rollback[256] = {};
    return linux_backend_restore_snapshot(context->gpu, context->snapshot, phases,
                                          rollback, sizeof(rollback));
}

LinuxMutationResult linux_backend_reset(LinuxGpuState* g, char* result, size_t resultSize) {
    LinuxMutationResult mutation = {};
    LinuxHardwareSnapshot snapshot = {};
    char detail[256] = {};
    linux_backend_refresh(g);
    if (!linux_backend_capture_snapshot(g, &snapshot, detail, sizeof(detail)) ||
        !g->writeIdentityResolved) {
        if (result) gc_strlcpy(result, resultSize, detail[0] ? detail : "Reset preflight failed");
        return mutation;
    }
    const gc_u32 resetDomains = SERVICE_MUTATION_DOMAIN_ALL;
    if ((snapshot.availableMutationDomains & resetDomains) != resetDomains ||
        g->powerLimitDefaultmW <= 0 || !g->backend ||
        !g->backend->writeSupported) {
        gc_strlcpy(detail, sizeof(detail),
                   "Reset preflight failed: every mutable domain must support snapshot and restore");
        if (result) gc_strlcpy(result, resultSize, detail);
        return mutation;
    }
    LinuxResetTransactionContext context = {g, &snapshot};
    const unsigned int requested = LINUX_MUTATION_LOCK | LINUX_MUTATION_GPU_OFFSET |
        LINUX_MUTATION_MEM_OFFSET | LINUX_MUTATION_POWER | LINUX_MUTATION_CURVE |
        LINUX_MUTATION_FAN;
    mutation = linux_execute_transaction(requested, linux_reset_transaction_step,
                                         linux_reset_transaction_rollback, &context);
    if (result) gc_strlcpy(result, resultSize, mutation.success ? "Reset to defaults" :
        mutation.rollbackSucceeded ? "Reset failed and was rolled back" : "Reset failed; rollback uncertain");
    linux_backend_refresh(g);
    return mutation;
}

bool linux_backend_set_curve_fan_percent(LinuxGpuState* g, unsigned int percent) {
    if (!g || percent > 100) return false;
    return nvml_set_fan(g, FAN_MODE_FIXED, false, (int)percent);
}

bool linux_backend_set_fan_auto(LinuxGpuState* g) {
    return g && nvml_set_fan(g, FAN_MODE_AUTO, true, 0);
}
