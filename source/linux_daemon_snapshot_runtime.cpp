// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Included daemon shard: atomic Linux snapshot/control publication,
// selected-GPU generation tracking, and mutation precondition validation.

static void populate_snapshot(ServiceSnapshot* s, ControlState* control) {
    memset(s, 0, sizeof(*s));
    if (control) memset(control, 0, sizeof(*control));
    s->adapterCount = g_gpu.adapterCount;
    s->selectedAdapterIndex = g_gpu.selectedAdapterIndex;
    s->selectedAdapterOrdinalFallback = g_gpu.adapterCount == 1 &&
                                        !linux_gpu_bdf_valid(&g_gpu.selectedGpu);
    for (unsigned int i = 0; i < g_gpu.adapterCount && i < MAX_GPU_ADAPTERS; ++i)
        s->adapters[i] = g_gpu.adapters[i];
    // The guard is the authority here, not g_stateUncertain.  Deriving this
    // field from the uncertain flag alone published LOCKOUT_NONE for the whole
    // crash-loop arm -- three refused start-time replays latch the guard
    // without ever reaching a hardware write, so the daemon reported a healthy
    // GPU while having permanently stopped restoring settings at boot, with the
    // debug log as the only evidence.  Windows has always fed the same field
    // from its real lockout state (service_auto_restore_is_locked_out).
    s->autoRestoreLockoutReason = linux_auto_restore_published_lockout_reason(
        &g_autoRestoreGuard, g_stateUncertain);
    s->health = g_gpu.health;
    if (g_stateUncertain) {
        s->health.reason = SERVICE_GPU_HEALTH_STATE_UNCERTAIN;
        s->health.driverStatus = 0;
        gc_strlcpy(s->health.detail, sizeof(s->health.detail),
            "previous daemon state or rollback is uncertain; explicit Apply/Reset is required");
    }
    if (!g_gpuReady) return;
    s->initialized = true;
    s->loaded = g_gpu.vfSnapshotFresh && g_gpu.numPopulated > 0;
    s->gpuFamily = g_gpu.family;
    s->numPopulated = s->loaded ? g_gpu.numPopulated : 0;
    s->vfReadSupported = s->loaded && g_gpu.gpuHandle && g_gpu.backend &&
        g_gpu.backend->readSupported;
    s->vfWriteSupported = s->loaded && g_gpu.gpuHandle && g_gpu.backend &&
        g_gpu.backend->writeSupported;
    s->vfBestGuess = s->loaded && g_gpu.backend &&
        g_gpu.backend->bestGuessOnly;
    s->gpuClockOffsetMinMHz = g_gpu.gpuOffsetMinMHz;
    s->gpuClockOffsetMaxMHz = g_gpu.gpuOffsetMaxMHz;
    s->memOffsetMinMHz = g_gpu.memOffsetMinMHz;
    s->memOffsetMaxMHz = g_gpu.memOffsetMaxMHz;
    s->curveOffsetMinkHz = g_gpu.curveOffsetMinKHz;
    s->curveOffsetMaxkHz = g_gpu.curveOffsetMaxKHz;
    s->curveOffsetRangeKnown = g_gpu.curveOffsetRangeKnown;
    s->powerLimitMinmW = g_gpu.powerLimitMinmW;
    s->powerLimitMaxmW = g_gpu.powerLimitMaxmW;
    s->powerLimitDefaultmW = g_gpu.powerLimitDefaultmW;
    if (s->loaded) {
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            s->curve[i] = g_gpu.curve[i];
            s->freqOffsets[i] = g_gpu.freqOffsets[i];
        }
    }
    // XBAR clock domain state (schema-gated on Windows).
    s->xbarSupported = g_gpu.xbarProbeValid;
    s->xbarOffsetReadbackValid = g_gpu.xbarFreqReadbackValid;
    s->xbarOffsetKhz = g_gpu.xbarFreqOffsetKhz;
    s->xbarMsvddOffsetReadbackValid = g_gpu.xbarMsvddReadbackValid;
    s->xbarMsvddOffsetUv = g_gpu.xbarMsvddOffsetUv;
    s->xbarMeasuredClockKhz = g_gpu.xbarMeasuredClockKhz;
    gc_strlcpy(s->gpuName, sizeof(s->gpuName), g_gpu.gpuName[0] ? g_gpu.gpuName : "NVIDIA GPU");

    // Preserve the backend's complete adapter list and exact selected index.
    // The old Linux snapshot replaced it with one fabricated name-only entry,
    // which made multi-GPU selection and reconnect-safe identity checks
    // impossible even though enumeration had succeeded.
    if (s->selectedAdapterIndex < s->adapterCount) {
        s->adapters[s->selectedAdapterIndex] = g_gpu.selectedGpu;
        if (!s->adapters[s->selectedAdapterIndex].name[0])
            gc_strlcpy(s->adapters[s->selectedAdapterIndex].name,
                       sizeof(s->adapters[s->selectedAdapterIndex].name),
                       s->gpuName);
    }

    LinuxHardwareSnapshot hardware = {};
    char hardwareErr[160] = {};
    bool hardwareAvailable = linux_backend_capture_snapshot(
        &g_gpu, &hardware, hardwareErr, sizeof(hardwareErr));
    s->health = g_gpu.health;
    if (g_stateUncertain) {
        s->health.reason = SERVICE_GPU_HEALTH_STATE_UNCERTAIN;
        s->health.driverStatus = 0;
        gc_strlcpy(s->health.detail, sizeof(s->health.detail),
            "previous daemon state or rollback is uncertain; explicit Apply/Reset is required");
    }
    // Backend health transition logging already records the first loss/recovery
    // of control domains. Do not repeat the same failure on every telemetry read.
    if (hardware.gpuOffsetValid) {
        s->gpuClockOffsetkHz = hardware.gpuOffsetMHz * 1000;
        s->gpuOffsetRangeKnown = true;
    }
    if (hardware.memOffsetValid) {
        s->memClockOffsetkHz = hardware.memOffsetMHz * 1000;
        s->memOffsetRangeKnown = true;
    }
    if (hardware.powerValid) {
        s->powerLimitCurrentmW = (int)hardware.powerLimitmW;
        if (s->powerLimitDefaultmW > 0) {
            s->powerLimitPct = (int)(((long long)hardware.powerLimitmW * 100LL +
                                      s->powerLimitDefaultmW / 2) /
                                     s->powerLimitDefaultmW);
        }
    }
    if (g_gpu.nvml.getTemperature) {
        unsigned int t = 0;
        if (g_gpu.nvml.getTemperature(g_gpu.nvmlDevice, NVML_TEMPERATURE_GPU, &t) == NVML_SUCCESS) {
            s->gpuTemperatureC = (int)t;
            s->gpuTemperatureValid = true;
        }
    }
    if (g_gpu.nvml.getPowerLimit) {
        unsigned int p = 0;
        if (g_gpu.nvml.getPowerLimit(g_gpu.nvmlDevice, &p) == NVML_SUCCESS) s->powerLimitCurrentmW = (int)p;
    }
    if (g_gpu.nvml.getNumFans) {
        unsigned int fans = 0;
        if (g_gpu.nvml.getNumFans(g_gpu.nvmlDevice, &fans) == NVML_SUCCESS) {
            s->fanCount = fans;
            s->fanSupported = (fans > 0);
            // Publish the range the driver actually honors.  This used to be
            // hardcoded 0..100, which let the UI offer duties the driver
            // silently clamped (an RTX 5070 floors manual control at 30%).
            // Still gate only on fan presence: fanRangeKnown enables the fan
            // controls, and a driver without the range getter can still be
            // driven manually over the full 0..100 span.
            s->fanRangeKnown = (fans > 0);
            s->fanMinPct = g_gpu.fanRangeKnown ? (unsigned int)g_gpu.fanMinPct : 0u;
            s->fanMaxPct = g_gpu.fanRangeKnown ? (unsigned int)g_gpu.fanMaxPct : 100u;
            for (unsigned int f = 0; f < fans && f < MAX_GPU_FANS; f++) {
                // fanPercent is measured telemetry; fanTargetPercent is intent.
                unsigned int pct = 0;
                if (g_gpu.nvml.getFanSpeed && g_gpu.nvml.getFanSpeed(g_gpu.nvmlDevice, f, &pct) == NVML_SUCCESS)
                    s->fanPercent[f] = pct;
                if (f < hardware.fanCount) {
                    s->fanPolicy[f] = hardware.fanPolicy[f];
                    s->fanTargetPercent[f] = hardware.fanTargetPercent[f];
                }
            }
        }
    }

    if (g_hasActiveDesired) {
        s->hasLock = g_activeDesired.hasLock;
        s->lockCi = g_activeDesired.lockCi;
        s->lockMHz = g_activeDesired.lockMHz;
        s->lockMode = g_activeDesired.lockMode;
        s->lockTracksAnchor = g_activeDesired.lockTracksAnchor;
        s->appliedGpuOffsetMHz = g_activeDesired.gpuOffsetMHz;
        s->appliedGpuOffsetExcludeLowCount =
            g_activeDesired.gpuOffsetExcludeLowCount;
        s->lastApplyUsedGpuOffset = g_activeDesired.hasGpuOffset;
        s->activeFanMode = g_activeDesired.fanMode;
        s->activeFanFixedPercent = g_activeDesired.fanPercent;
        s->activeFanCurve = g_activeDesired.fanCurve;
        s->fanCurveRuntimeActive = g_activeDesired.hasFan &&
                                   g_activeDesired.fanMode == FAN_MODE_CURVE;
        s->fanFixedRuntimeActive = g_activeDesired.hasFan &&
                                   g_activeDesired.fanMode == FAN_MODE_FIXED;
    }

    if (!control) return;
    control->valid = hardwareAvailable || g_hasActiveDesired;
    control->hasGpuOffset = hardware.gpuOffsetValid ||
                            (g_hasActiveDesired && g_activeDesired.hasGpuOffset);
    control->gpuOffsetReadbackValid = hardware.gpuOffsetValid;
    control->gpuOffsetMHz = hardware.gpuOffsetValid
        ? hardware.gpuOffsetMHz
        : g_activeDesired.gpuOffsetMHz;
    control->gpuOffsetExcludeLowCount = g_hasActiveDesired
        ? g_activeDesired.gpuOffsetExcludeLowCount : 0;
    control->hasMemOffset = hardware.memOffsetValid ||
                            (g_hasActiveDesired && g_activeDesired.hasMemOffset);
    control->memOffsetReadbackValid = hardware.memOffsetValid;
    control->memOffsetMHz = hardware.memOffsetValid
        ? hardware.memOffsetMHz
        : g_activeDesired.memOffsetMHz;
    control->hasPowerLimit = hardware.powerValid ||
                             (g_hasActiveDesired && g_activeDesired.hasPowerLimit);
    control->powerLimitReadbackValid = hardware.powerValid;
    control->powerLimitPct = s->powerLimitPct > 0
        ? s->powerLimitPct
        : (g_hasActiveDesired ? g_activeDesired.powerLimitPct : 100);
    control->hasFan = s->fanSupported ||
                      (g_hasActiveDesired && g_activeDesired.hasFan);
    control->fanPolicyReadbackValid = hardware.fanValid &&
        hardware.fanCount > 0;
    bool allFanTargetsKnown = hardware.fanValid && hardware.fanCount > 0;
    for (unsigned int i = 0; i < hardware.fanCount; ++i)
        if (!hardware.fanTargetKnown[i]) allFanTargetsKnown = false;
    control->fanTargetReadbackValid = allFanTargetsKnown;
    control->fanCurrentPercent = s->fanCount > 0 ? (int)s->fanPercent[0] : 0;
    control->fanCurrentTemperatureC = s->gpuTemperatureValid
        ? s->gpuTemperatureC : 0;
    if (g_hasActiveDesired && g_activeDesired.hasFan) {
        control->fanMode = g_activeDesired.fanMode;
        control->fanFixedPercent = g_activeDesired.fanPercent;
        control->fanCurve = g_activeDesired.fanCurve;
    } else {
        bool automatic = hardware.fanValid && hardware.fanCount > 0;
        for (unsigned int i = 0; i < hardware.fanCount; ++i) {
            if (hardware.fanPolicy[i] !=
                NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW) automatic = false;
        }
        control->fanMode = automatic ? FAN_MODE_AUTO : FAN_MODE_FIXED;
        control->fanFixedPercent = control->fanCurrentPercent;
        fan_curve_set_default(&control->fanCurve);
    }
}

#include "linux_daemon_identity.cpp"
#include "linux_fan_runtime.h"
static bool select_request_gpu(const ServiceRequest* req, char* err, size_t errSize) {
    if (g_gpu.adapterCount == 1 && (!req || !req->targetGpu.valid)) {
        if (!g_gpu.writeIdentityResolved) {
            gc_strlcpy(err, errSize, "single GPU identity is not safe for writes");
            return false;
        }
        return true;
    }
    if (!req || !req->targetGpu.valid) {
        gc_strlcpy(err, errSize, "multiple GPUs detected; select an exact PCI BDF with --gpu");
        return false;
    }
    if (!linux_gpu_switch_preserves_active_intent(
            g_hasActiveDesired, &g_activeTarget, &req->targetGpu)) {
        char activeBdf[32] = {}, requestedBdf[32] = {};
        gc_snprintf(activeBdf, sizeof(activeBdf), "%04x:%02x:%02x.%u",
            g_activeTarget.pciDomain, g_activeTarget.pciBus,
            g_activeTarget.pciDevice, g_activeTarget.pciFunction);
        gc_snprintf(requestedBdf, sizeof(requestedBdf), "%04x:%02x:%02x.%u",
            req->targetGpu.pciDomain, req->targetGpu.pciBus,
            req->targetGpu.pciDevice, req->targetGpu.pciFunction);
        gc_snprintf(err, errSize,
            "GPU %s has active Green Curve settings; Reset it before selecting %s",
            activeBdf, requestedBdf);
        dlog("daemon state: rejected GPU switch active=%s requested=%s; "
             "single active-intent ownership preserved\n",
             activeBdf, requestedBdf);
        return false;
    }
    GpuAdapterInfo previous = g_gpu.selectedGpu;
    if (!linux_backend_select_target(&g_gpu, &req->targetGpu, err, errSize)) return false;
    if (!linux_gpu_identity_matches(&previous, &g_gpu.selectedGpu)) {
        ++g_daemonGpuGeneration;
        dlog("daemon state: selected GPU changed; generation=%llu\n",
             (unsigned long long)g_daemonGpuGeneration);
    }
    g_gpuReady = true;
    return true;
}

static bool mutation_preconditions_match(const ServiceRequest* req,
                                         ServiceResponse* resp) {
    if (!req || !resp) return false;
    if (!req->expectedServiceInstanceId && !req->expectedGpuGeneration &&
        !req->expectedTopologySignature) return true;

    if (g_gpuReady) linux_backend_refresh(&g_gpu);
    ServiceSnapshot current = {};
    ControlState controls = {};
    populate_snapshot(&current, &controls);
    gc_u64 topology = current.loaded && current.numPopulated > 0
        ? service_snapshot_topology_signature(&current) : 0;
    gc_u32 requestedDomains = service_requested_mutation_domains(
        req->command, &req->desired);
    gc_u32 unavailableDomains = service_unavailable_mutation_domains(
        req->command, &req->desired,
        current.health.availableMutationDomains);
    if (unavailableDomains != 0) {
        resp->status = SERVICE_STATUS_ERROR;
        gc_snprintf(resp->message, sizeof(resp->message),
            "requested GPU domains unavailable (requested=0x%02x available=0x%02x missing=0x%02x): %s",
            requestedDomains, current.health.availableMutationDomains,
            unavailableDomains,
            current.health.detail[0] ? current.health.detail :
            service_gpu_health_reason_name(current.health.reason));
        dlog("daemon mutation rejected before write: requested=0x%02x available=0x%02x missing=0x%02x reason=%u status=%d\n",
            requestedDomains, current.health.availableMutationDomains,
            unavailableDomains, current.health.reason,
            current.health.driverStatus);
        return false;
    }
    const GpuAdapterInfo* currentGpu = current.selectedAdapterIndex <
            current.adapterCount
        ? &current.adapters[current.selectedAdapterIndex] : nullptr;
    bool identityMatches = currentGpu && linux_gpu_identity_matches(
        &req->targetGpu, currentGpu);
    bool matches = linux_mutation_authority_matches(
        req, daemon_service_instance_id(), g_daemonGpuGeneration,
        topology, currentGpu, requestedDomains);
    if (matches) return true;

    resp->status = SERVICE_STATUS_STALE_STATE;
    gc_snprintf(resp->message, sizeof(resp->message),
        "stale Linux GUI state; refresh before applying "
        "(instance=%llu generation=%llu topology=%llu)",
        (unsigned long long)daemon_service_instance_id(),
        (unsigned long long)g_daemonGpuGeneration,
        (unsigned long long)topology);
    dlog("daemon mutation rejected: expected instance=%llu generation=%llu "
         "topology=%llu identity=%d; current instance=%llu generation=%llu "
         "topology=%llu\n",
         (unsigned long long)req->expectedServiceInstanceId,
         (unsigned long long)req->expectedGpuGeneration,
         (unsigned long long)req->expectedTopologySignature,
         identityMatches ? 1 : 0,
         (unsigned long long)daemon_service_instance_id(),
         (unsigned long long)g_daemonGpuGeneration,
         (unsigned long long)topology);
    return false;
}
