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
    s->autoRestoreLockoutReason = g_stateUncertain
        ? SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED
        : SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
    if (!g_gpuReady) return;
    s->initialized = true;
    s->loaded = (g_gpu.numPopulated > 0);
    s->gpuFamily = g_gpu.family;
    s->numPopulated = g_gpu.numPopulated;
    s->vfReadSupported = g_gpu.backend && g_gpu.backend->readSupported;
    s->vfWriteSupported = g_gpu.backend && g_gpu.backend->writeSupported;
    s->vfBestGuess = g_gpu.backend && g_gpu.backend->bestGuessOnly;
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
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        s->curve[i] = g_gpu.curve[i];
        s->freqOffsets[i] = g_gpu.freqOffsets[i];
    }
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
    if (!hardwareAvailable) {
        dlog("daemon snapshot: partial live controls: %s\n",
             hardwareErr[0] ? hardwareErr : "no readable control domain");
    }
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
            s->fanRangeKnown = (fans > 0);
            s->fanMinPct = 0;
            s->fanMaxPct = 100;
            for (unsigned int f = 0; f < fans && f < MAX_GPU_FANS; f++) {
                unsigned int pct = 0;
                if (g_gpu.nvml.getFanSpeed && g_gpu.nvml.getFanSpeed(g_gpu.nvmlDevice, f, &pct) == NVML_SUCCESS)
                    s->fanPercent[f] = pct;
                if (f < hardware.fanCount) {
                    s->fanPolicy[f] = hardware.fanPolicy[f];
                    s->fanTargetPercent[f] = hardware.fanPercent[f];
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
    control->gpuOffsetMHz = hardware.gpuOffsetValid
        ? hardware.gpuOffsetMHz
        : g_activeDesired.gpuOffsetMHz;
    control->gpuOffsetExcludeLowCount = g_hasActiveDesired
        ? g_activeDesired.gpuOffsetExcludeLowCount : 0;
    control->hasMemOffset = hardware.memOffsetValid ||
                            (g_hasActiveDesired && g_activeDesired.hasMemOffset);
    control->memOffsetMHz = hardware.memOffsetValid
        ? hardware.memOffsetMHz
        : g_activeDesired.memOffsetMHz;
    control->hasPowerLimit = hardware.powerValid ||
                             (g_hasActiveDesired && g_activeDesired.hasPowerLimit);
    control->powerLimitPct = s->powerLimitPct > 0
        ? s->powerLimitPct
        : (g_hasActiveDesired ? g_activeDesired.powerLimitPct : 100);
    control->hasFan = s->fanSupported ||
                      (g_hasActiveDesired && g_activeDesired.hasFan);
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
    bool identityMatches = current.selectedAdapterIndex < current.adapterCount &&
        linux_gpu_identity_matches(&req->targetGpu,
            &current.adapters[current.selectedAdapterIndex]);
    bool matches = req->expectedServiceInstanceId ==
                       daemon_service_instance_id() &&
                   req->expectedGpuGeneration == g_daemonGpuGeneration &&
                   req->expectedTopologySignature == topology &&
                   identityMatches;
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
