// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Included daemon shard: atomic Linux snapshot/control publication,
// selected-GPU generation tracking, and mutation precondition validation.

static void populate_snapshot(LinuxGpuController& ctl, ServiceSnapshot* s, ControlState* control) {
    memset(s, 0, sizeof(*s));
    if (control) memset(control, 0, sizeof(*control));
    s->adapterCount = ctl.gpu.adapterCount;
    s->selectedAdapterIndex = ctl.gpu.selectedAdapterIndex;
    s->selectedAdapterOrdinalFallback = ctl.gpu.adapterCount == 1 &&
                                        !linux_gpu_bdf_valid(&ctl.gpu.selectedGpu);
    for (unsigned int i = 0; i < ctl.gpu.adapterCount && i < MAX_GPU_ADAPTERS; ++i)
        s->adapters[i] = ctl.gpu.adapters[i];
    // The guard is the authority here, not ctl.stateUncertain.  Deriving this
    // field from the uncertain flag alone published LOCKOUT_NONE for the whole
    // crash-loop arm -- three refused start-time replays latch the guard
    // without ever reaching a hardware write, so the daemon reported a healthy
    // GPU while having permanently stopped restoring settings at boot, with the
    // debug log as the only evidence.  Windows has always fed the same field
    // from its real lockout state (service_auto_restore_is_locked_out).
    s->autoRestoreLockoutReason = linux_auto_restore_published_lockout_reason(
        &ctl.autoRestoreGuard, ctl.stateUncertain);
    // Published only with the lockout it explains (protocol v29).
    if (s->autoRestoreLockoutReason ==
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED)
        s->autoApplyFailure = ctl.autoApplyFailure;
    s->fanFault = ctl.fanFault;  // v39
    s->health = ctl.gpu.health;
    if (ctl.stateUncertain) {
        s->health.reason = SERVICE_GPU_HEALTH_STATE_UNCERTAIN;
        s->health.driverStatus = 0;
        gc_strlcpy(s->health.detail, sizeof(s->health.detail),
            "previous daemon state or rollback is uncertain; explicit Apply/Reset is required");
    }
    if (!ctl.gpuReady) return;
    s->initialized = true;
    s->loaded = ctl.gpu.vfSnapshotFresh && ctl.gpu.numPopulated > 0;
    s->gpuFamily = ctl.gpu.family;
    s->numPopulated = s->loaded ? ctl.gpu.numPopulated : 0;
    s->vfReadSupported = s->loaded && ctl.gpu.gpuHandle && ctl.gpu.backend &&
        ctl.gpu.backend->readSupported;
    s->vfWriteSupported = s->loaded && ctl.gpu.gpuHandle && ctl.gpu.backend &&
        ctl.gpu.backend->writeSupported;
    s->vfBestGuess = s->loaded && ctl.gpu.backend &&
        ctl.gpu.backend->bestGuessOnly;
    s->gpuClockOffsetMinMHz = ctl.gpu.gpuOffsetMinMHz;
    s->gpuClockOffsetMaxMHz = ctl.gpu.gpuOffsetMaxMHz;
    s->memOffsetMinMHz = ctl.gpu.memOffsetMinMHz;
    s->memOffsetMaxMHz = ctl.gpu.memOffsetMaxMHz;
    s->curveOffsetMinkHz = ctl.gpu.curveOffsetMinKHz;
    s->curveOffsetMaxkHz = ctl.gpu.curveOffsetMaxKHz;
    s->curveOffsetRangeKnown = ctl.gpu.curveOffsetRangeKnown;
    s->powerLimitMinmW = ctl.gpu.powerLimitMinmW;
    s->powerLimitMaxmW = ctl.gpu.powerLimitMaxmW;
    s->powerLimitDefaultmW = ctl.gpu.powerLimitDefaultmW;
    if (s->loaded) {
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            s->curve[i] = ctl.gpu.curve[i];
            s->freqOffsets[i] = ctl.gpu.freqOffsets[i];
        }
    }
    gc_strlcpy(s->gpuName, sizeof(s->gpuName), ctl.gpu.gpuName[0] ? ctl.gpu.gpuName : "NVIDIA GPU");

    // Preserve the backend's complete adapter list and exact selected index.
    // The old Linux snapshot replaced it with one fabricated name-only entry,
    // which made multi-GPU selection and reconnect-safe identity checks
    // impossible even though enumeration had succeeded.
    if (s->selectedAdapterIndex < s->adapterCount) {
        s->adapters[s->selectedAdapterIndex] = ctl.gpu.selectedGpu;
        if (!s->adapters[s->selectedAdapterIndex].name[0])
            gc_strlcpy(s->adapters[s->selectedAdapterIndex].name,
                       sizeof(s->adapters[s->selectedAdapterIndex].name),
                       s->gpuName);
    }

    LinuxHardwareSnapshot hardware = {};
    char hardwareErr[160] = {};
    bool hardwareAvailable = linux_backend_capture_snapshot(
        &ctl.gpu, &hardware, hardwareErr, sizeof(hardwareErr));
    // capture_snapshot performs the fresh shared ClkDomains read; publish only
    // after it so this envelope never lags its own availability proof.
    s->xbarSupported = ctl.gpu.xbarProbeValid;
    s->xbarOffsetReadbackValid = ctl.gpu.xbarFreqReadbackValid;
    s->xbarOffsetKhz = ctl.gpu.xbarFreqOffsetKhz;
    s->xbarMsvddOffsetReadbackValid = ctl.gpu.xbarMsvddReadbackValid;
    s->xbarMsvddOffsetUv = ctl.gpu.xbarMsvddOffsetUv;
    s->xbarMeasuredClockKhz = ctl.gpu.xbarMeasuredClockKhz;
    s->xbarMeasuredVoltageUv = ctl.gpu.xbarMeasuredVoltageUv;
    s->sysClkSupported = ctl.gpu.sysClkProbeValid;
    s->sysClkOffsetReadbackValid = ctl.gpu.sysClkFreqReadbackValid;
    s->sysClkOffsetKhz = ctl.gpu.sysClkFreqOffsetKhz;
    s->sysClkMeasuredClockKhz = ctl.gpu.sysClkMeasuredClockKhz;
    s->videoClkSupported = ctl.gpu.videoClkProbeValid;
    s->videoClkOffsetReadbackValid = ctl.gpu.videoClkFreqReadbackValid;
    s->videoClkOffsetKhz = ctl.gpu.videoClkFreqOffsetKhz;
    s->videoClkMeasuredClockKhz = ctl.gpu.videoClkMeasuredClockKhz;
    s->health = ctl.gpu.health;
    if (ctl.stateUncertain) {
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
        // Hardware snapshot is display MHz, so this is display kHz, matching
        // Windows (main_state_sync.cpp: memClockOffsetkHz from halved NVML).
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
    if (ctl.gpu.nvml.getTemperature) {
        unsigned int t = 0;
        if (ctl.gpu.nvml.getTemperature(ctl.gpu.nvmlDevice, NVML_TEMPERATURE_GPU, &t) == NVML_SUCCESS) {
            s->gpuTemperatureC = (int)t;
            s->gpuTemperatureValid = true;
        }
    }
    if (ctl.gpu.nvml.getPowerLimit) {
        unsigned int p = 0;
        if (ctl.gpu.nvml.getPowerLimit(ctl.gpu.nvmlDevice, &p) == NVML_SUCCESS) s->powerLimitCurrentmW = (int)p;
    }
    if (ctl.gpu.nvml.getNumFans) {
        unsigned int fans = 0;
        if (ctl.gpu.nvml.getNumFans(ctl.gpu.nvmlDevice, &fans) == NVML_SUCCESS) {
            s->fanCount = fans;
            s->fanSupported = (fans > 0);
            // Publish the range the driver actually honors.  This used to be
            // hardcoded 0..100, which let the UI offer duties the driver
            // silently clamped (an RTX 5070 floors manual control at 30%).
            // Still gate only on fan presence: fanRangeKnown enables the fan
            // controls, and a driver without the range getter can still be
            // driven manually over the full 0..100 span.
            s->fanRangeKnown = (fans > 0);
            s->fanMinPct = ctl.gpu.fanRangeKnown ? (unsigned int)ctl.gpu.fanMinPct : 0u;
            s->fanMaxPct = ctl.gpu.fanRangeKnown ? (unsigned int)ctl.gpu.fanMaxPct : 100u;
            for (unsigned int f = 0; f < fans && f < MAX_GPU_FANS; f++) {
                // fanPercent is measured telemetry; fanTargetPercent is intent.
                unsigned int pct = 0;
                if (ctl.gpu.nvml.getFanSpeed && ctl.gpu.nvml.getFanSpeed(ctl.gpu.nvmlDevice, f, &pct) == NVML_SUCCESS)
                    s->fanPercent[f] = pct;
                if (f < hardware.fanCount) {
                    s->fanPolicy[f] = hardware.fanPolicy[f];
                    s->fanTargetPercent[f] = hardware.fanTargetPercent[f];
                }
            }
        }
    }

    const bool intentMatchesSelected = ctl.hasActiveDesired &&
        linux_gpu_identity_matches(&ctl.activeTarget, &ctl.gpu.selectedGpu) && daemon_active_intent_on_served(ctl);
    const bool fanRuntimeActive = linux_daemon_fan_runtime_active(ctl.gpuReady,
        ctl.hasActiveDesired, ctl.stateUncertain, intentMatchesSelected,
        ctl.activeDesired.hasFan != 0, ctl.activeDesired.fanMode,
        ctl.activeDesired.fanAuto != 0);
    const bool intentOwnsSelectedFan = intentMatchesSelected &&
        !ctl.stateUncertain && ctl.activeDesired.hasFan != 0;
    if (ctl.hasActiveDesired) {
        s->hasLock = ctl.activeDesired.hasLock;
        s->lockCi = ctl.activeDesired.lockCi;
        s->lockMHz = ctl.activeDesired.lockMHz;
        s->lockMode = ctl.activeDesired.lockMode;
        s->lockTracksAnchor = ctl.activeDesired.lockTracksAnchor;
        s->appliedGpuOffsetMHz = ctl.activeDesired.gpuOffsetMHz;
        s->appliedGpuOffsetExcludeLowCount =
            ctl.activeDesired.gpuOffsetExcludeLowCount;
        s->lastApplyUsedGpuOffset = ctl.activeDesired.hasGpuOffset;
        // An unclaimed, uncertain, or other-GPU fan is not driven by the
        // worker (fan_intent_policy.h); the runtime flags agree with the worker.
        s->activeFanMode = fan_owned_intent_mode(intentOwnsSelectedFan,
            ctl.activeDesired.fanMode, ctl.activeDesired.fanAuto != 0);
        s->activeFanFixedPercent = intentOwnsSelectedFan
            ? ctl.activeDesired.fanPercent : 0;
        s->activeFanCurve = ctl.activeDesired.fanCurve;
        s->fanCurveRuntimeActive = fanRuntimeActive &&
                                   s->activeFanMode == FAN_MODE_CURVE;
        s->fanFixedRuntimeActive = fanRuntimeActive &&
                                   s->activeFanMode == FAN_MODE_FIXED;
    }

    if (!control) return;
    control->valid = hardwareAvailable || ctl.hasActiveDesired;
    control->hasGpuOffset = hardware.gpuOffsetValid ||
                            (ctl.hasActiveDesired && ctl.activeDesired.hasGpuOffset);
    control->gpuOffsetReadbackValid = hardware.gpuOffsetValid;
    control->gpuOffsetMHz = hardware.gpuOffsetValid
        ? hardware.gpuOffsetMHz
        : ctl.activeDesired.gpuOffsetMHz;
    control->gpuOffsetExcludeLowCount = ctl.hasActiveDesired
        ? ctl.activeDesired.gpuOffsetExcludeLowCount : 0;
    control->hasMemOffset = hardware.memOffsetValid ||
                            (ctl.hasActiveDesired && ctl.activeDesired.hasMemOffset);
    control->memOffsetReadbackValid = hardware.memOffsetValid;
    control->memOffsetMHz = hardware.memOffsetValid
        ? hardware.memOffsetMHz
        : ctl.activeDesired.memOffsetMHz;
    control->hasPowerLimit = hardware.powerValid ||
                             (ctl.hasActiveDesired && ctl.activeDesired.hasPowerLimit);
    control->powerLimitReadbackValid = hardware.powerValid;
    control->powerLimitPct = s->powerLimitPct > 0
        ? s->powerLimitPct
        : (ctl.hasActiveDesired ? ctl.activeDesired.powerLimitPct : 100);
    control->hasFan = s->fanSupported ||
                      (ctl.hasActiveDesired && ctl.activeDesired.hasFan);
    control->fanPolicyReadbackValid = hardware.fanValid &&
        hardware.fanCount > 0;
    bool allFanTargetsKnown = hardware.fanValid && hardware.fanCount > 0;
    for (unsigned int i = 0; i < hardware.fanCount; ++i)
        if (!hardware.fanTargetKnown[i]) allFanTargetsKnown = false;
    control->fanTargetReadbackValid = allFanTargetsKnown;
    control->fanCurrentPercent = s->fanCount > 0 ? (int)s->fanPercent[0] : 0;
    control->fanCurrentTemperatureC = s->gpuTemperatureValid
        ? s->gpuTemperatureC : 0;
    if (intentOwnsSelectedFan) {
        control->fanMode = s->activeFanMode;
        control->fanFixedPercent = ctl.activeDesired.fanPercent;
        control->fanCurve = ctl.activeDesired.fanCurve;
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
    control->hasXbarOffset = hardware.xbarValid ||
        (ctl.hasActiveDesired && ctl.activeDesired.hasXbarOffsetKhz);
    control->xbarOffsetReadbackValid = hardware.xbarValid;
    control->xbarOffsetKhz = hardware.xbarValid
        ? hardware.xbarOffsetKhz : ctl.activeDesired.xbarOffsetKhz;
    control->hasXbarMsvddOffset = hardware.xbarValid ||
        (ctl.hasActiveDesired && ctl.activeDesired.hasXbarMsvddOffsetUv);
    control->xbarMsvddOffsetReadbackValid = hardware.xbarValid;
    control->xbarMsvddOffsetUv = hardware.xbarValid
        ? hardware.xbarMsvddOffsetUv : ctl.activeDesired.xbarMsvddOffsetUv;
    control->hasSysClkOffset = hardware.sysClkValid ||
        (ctl.hasActiveDesired && ctl.activeDesired.hasSysClkOffsetKhz);
    control->sysClkOffsetReadbackValid = hardware.sysClkValid;
    control->sysClkOffsetKhz = hardware.sysClkValid
        ? hardware.sysClkOffsetKhz : ctl.activeDesired.sysClkOffsetKhz;
    control->hasVideoClkOffset = hardware.videoClkValid ||
        (ctl.hasActiveDesired && ctl.activeDesired.hasVideoClkOffsetKhz);
    control->videoClkOffsetReadbackValid = hardware.videoClkValid;
    control->videoClkOffsetKhz = hardware.videoClkValid
        ? hardware.videoClkOffsetKhz : ctl.activeDesired.videoClkOffsetKhz;
}

#include "linux_daemon_identity.cpp"
#include "linux_fan_runtime.h"


// A one-shot read may safely retry backend discovery without selecting write
// authority. An automatic tick gets one attempt; an explicit user refresh asks
// for another through the existing interactive request flag.
static bool daemon_retry_backend_for_read(LinuxGpuController& ctl, bool explicitRefresh) {
    if (ctl.gpuReady) return true;
    if (!linux_backend_read_init_needed(
            ctl.gpuReady, ctl.backendReadInitAttempted, explicitRefresh))
        return false;
    ctl.backendReadInitAttempted = true;
    LinuxGpuState replacement = {};
    char err[256] = {};
    // Discovery runs inside this read's deadline, like a targeted GPU switch.
    // The timing makes a slow driver start visible against that budget.
    const unsigned long long initStartMs = monotonic_ms();
    const bool initOk = linux_backend_init(&replacement, &g_driver, nullptr, err, sizeof(err));
    const unsigned long long initMs = monotonic_ms() - initStartMs;
    if (initMs > GC_DAEMON_STATE_REFRESH_BUDGET_MS)
        dlog("daemon backend retry: discovery took %llu ms, over the %u ms state"
             " refresh budget; the client may time out this read\n",
             initMs, (unsigned int)GC_DAEMON_STATE_REFRESH_BUDGET_MS);
    if (!initOk) {
        linux_backend_shutdown(&replacement);
        dlog("daemon backend retry: untargeted discovery failed after %llu ms: %s\n",
             initMs, err[0] ? err : "unknown error");
        return false;
    }
    const GpuAdapterInfo previous = ctl.gpu.selectedGpu;
    linux_backend_preserve_same_gpu_log_state(&ctl.gpu, &replacement);
    linux_backend_shutdown(&ctl.gpu);
    ctl.gpu = replacement;
    ctl.gpuReady = true;
    if (previous.valid &&
        !linux_gpu_identity_matches(&previous, &ctl.gpu.selectedGpu))
        ++ctl.gpuGeneration;
    dlog("daemon backend retry: untargeted discovery online adapters=%u"
         " writeIdentityResolved=%d explicit=%d initMs=%llu\n",
         ctl.gpu.adapterCount, ctl.gpu.writeIdentityResolved ? 1 : 0,
         explicitRefresh ? 1 : 0, initMs);
    // The worker performs any deferred fan handback outside this read request.
    wake_fan_runtime(ctl);
    return true;
}

static bool select_request_gpu(LinuxGpuController& ctl, const ServiceRequest* req, char* err, size_t errSize) {
    if (req && req->expectedControlFingerprint) {
        if (ctl.gpuReady && ctl.gpu.writeIdentityResolved && req->targetGpu.valid &&
            req->targetGpu.nvapiIndex == ctl.gpu.selectedGpu.nvapiIndex &&
            req->targetGpu.nvmlIndex == ctl.gpu.selectedGpu.nvmlIndex &&
            linux_gpu_identity_matches(&req->targetGpu, &ctl.gpu.selectedGpu)) return true;
        gc_strlcpy(err, errSize, "Reviewed GPU session changed; capture and review again before Apply");
        return false;
    }
    if (ctl.gpu.adapterCount == 1 && (!req || !req->targetGpu.valid)) {
        if (!ctl.gpu.writeIdentityResolved) {
            gc_strlcpy(err, errSize, "single GPU identity is not safe for writes");
            return false;
        }
        return true;
    }
    if (!req || !req->targetGpu.valid) {
        gc_strlcpy(err, errSize, "multiple GPUs detected; select an exact PCI BDF with --gpu");
        return false;
    }
    // A request built from a config.ini written before a PCI bus renumbering
    // names the right GPU at its old location. Compare live adapters, not the
    // stale location, or the active GPU looks like "another GPU" and the
    // request is refused with advice to reset it.
    GpuAdapterInfo requested = req->targetGpu;
    bool moved = false;
    int live = linux_resolve_stored_gpu_target(&req->targetGpu,
        GPU_LOCATION_ROLE_UNRECORDED, ctl.gpu.adapters, ctl.gpu.adapterCount,
        &moved);
    if (live >= 0) {
        requested = ctl.gpu.adapters[live];
        if (moved) {
            static unsigned long long movedRequests = 0;
            ++movedRequests;
            if (movedRequests == 1 || movedRequests % 100 == 0)
                dlog("daemon state: request target moved to %04x:%02x:%02x.%u "
                     "(count=%llu); the client's config names an old location\n",
                     requested.pciDomain, requested.pciBus,
                     requested.pciDevice, requested.pciFunction,
                     movedRequests);
        }
    }
    // The card reading the intent's UUID (after a renumbering) may be selected
    // to Reset it; any other card stays refused, and so does a model match.
    const bool keepsIntent = linux_gpu_switch_preserves_active_intent(
        ctl.hasActiveDesired, &ctl.activeTarget, &requested);
    const bool provenOwner = !keepsIntent && live >= 0 && ctl.hasActiveDesired &&
        daemon_intent_uuid_read_on(ctl, (unsigned int)live);
    if (!keepsIntent && !provenOwner) {
        char activeBdf[32] = {}, requestedBdf[32] = {};
        gc_snprintf(activeBdf, sizeof(activeBdf), "%04x:%02x:%02x.%u",
            ctl.activeTarget.pciDomain, ctl.activeTarget.pciBus,
            ctl.activeTarget.pciDevice, ctl.activeTarget.pciFunction);
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
    GpuAdapterInfo previous = ctl.gpu.selectedGpu;
    if (!linux_backend_select_target(&ctl.gpu, &requested, err, errSize)) return false;
    // The intent follows its card to its new location while the new binding
    // still reads its UUID, so every check by PCI identity -- the fan worker,
    // Reset's previous intent, the snapshot -- agrees with the proof; if it
    // does not, an owned manual fan is reported, never silently left.
    if (provenOwner) {
        if (ctl.gpu.writeIdentityResolved && daemon_intent_uuid_read_on(ctl, ctl.gpu.selectedAdapterIndex))
            ctl.activeTarget = ctl.gpu.selectedGpu;
        else
            daemon_fan_report_unproven(ctl);
        wake_fan_runtime(ctl);
    }
    if (!ctl.gpuReady) {
        // This read-only request brought the backend up. Deferred fan recovery
        // runs in the fan worker, outside the state-read deadline.
        ctl.gpuReady = true;
        wake_fan_runtime(ctl);
    }
    if (!linux_gpu_identity_matches(&previous, &ctl.gpu.selectedGpu)) {
        ++ctl.gpuGeneration;
        dlog("daemon state: selected GPU changed; generation=%llu\n",
             (unsigned long long)ctl.gpuGeneration);
    }
    ctl.gpuReady = true;
    return true;
}

// The adapter a request route must address: the published selected adapter.
// The read-only telemetry fallback of a multi-GPU host is not served.
static int daemon_served_adapter_index(const LinuxGpuController& ctl) {
    return ctl.gpu.writeIdentityResolved &&
        ctl.gpu.selectedAdapterIndex < ctl.gpu.adapterCount
        ? (int)ctl.gpu.selectedAdapterIndex : -1;
}

// One route rule for every command (service_gpu_route.h): a routed command
// answers for the served GPU, a state read may move the daemon (guarded by
// select_request_gpu), and Apply/Reset bind their target through their
// preconditions. Anything else naming another GPU is refused, never answered
// for the wrong card.
static bool daemon_route_admits(LinuxGpuController& ctl, const ServiceRequest* req,
                                ServiceResponse* resp) {
    const int served = daemon_served_adapter_index(ctl);
    const ServiceGpuRouteDisposition disposition = service_gpu_route_disposition(
        req->command, req->targetGpu.valid != 0,
        service_gpu_route_addresses(req->targetGpu, ctl.gpu.adapters,
                                    ctl.gpu.adapterCount, served));
    if (disposition != SERVICE_GPU_ROUTE_NOT_SERVED) return true;
    resp->status = SERVICE_STATUS_STALE_STATE;
    gc_strlcpy(resp->message, sizeof(resp->message), SERVICE_GPU_ROUTE_NOT_SERVED_MESSAGE);
    dlog("daemon route: refused command=%u route=%s target=%04x:%02x:%02x.%u "
         "resolved=%d served=%d adapters=%u\n", (unsigned int)req->command,
         service_gpu_route_name(service_command_gpu_route(req->command)),
         req->targetGpu.pciDomain, req->targetGpu.pciBus, req->targetGpu.pciDevice,
         req->targetGpu.pciFunction,
         gpu_adapter_route_index(&req->targetGpu, ctl.gpu.adapters, ctl.gpu.adapterCount),
         served, ctl.gpu.adapterCount);
    return false;
}

static bool mutation_preconditions_match(LinuxGpuController& ctl, const ServiceRequest* req,
                                         ServiceResponse* resp) {
    if (!req || !resp) return false;
    if (!req->expectedServiceInstanceId && !req->expectedGpuGeneration &&
        !req->expectedTopologySignature) return true;

    if (ctl.gpuReady && !req->expectedControlFingerprint) linux_backend_refresh(&ctl.gpu);
    ServiceSnapshot current = {};
    ControlState controls = {};
    populate_snapshot(ctl, &current, &controls);
    gc_u64 topology = current.loaded && current.numPopulated > 0
        ? service_snapshot_topology_signature(&current) : 0;
    gc_u32 requestedDomains = service_requested_mutation_domains(
        req->command, &req->desired);
    // Reset restores every domain this Linux driver can prove and roll back.
    // Optional advanced domains must not make Reset impossible on a GPU whose
    // ClkDomains schema is unknown; when present, they are included and reset.
    if (req->command == SERVICE_CMD_RESET)
        requestedDomains = current.health.availableMutationDomains;
    // service_inert_request_domains.h: an inert request (board-default power
    // on a board without a power surface, Auto fan without a controllable fan)
    // owns its domain without needing the surface.  The TUI's own gates use
    // the same helper on the same snapshot.
    gc_u32 unavailableDomains = req->command == SERVICE_CMD_APPLY
        ? service_snapshot_missing_mutation_domains(&req->desired, &current)
        : 0;
    const FanRequestDisposition fanDisposition =
        req->command == SERVICE_CMD_APPLY
            ? service_snapshot_fan_request_disposition(&req->desired, &current)
            : FAN_REQUEST_NOT_OWNED;
    if (fanDisposition == FAN_REQUEST_INERT_NO_FAN) {
        dlog("daemon mutation gate: fan Auto on a GPU without a controllable fan"
             " (count=%u) is inert; fan domain not required\n", current.fanCount);
    }
    if (req->command == SERVICE_CMD_APPLY &&
        service_snapshot_power_request_is_inert(&req->desired, &current)) {
        dlog("daemon mutation gate: board-default power on a board without a power"
             " surface is inert; power domain not required\n");
    }
    if (fanDisposition == FAN_REQUEST_REFUSED_NO_FAN) {
        resp->status = SERVICE_STATUS_ERROR;
        gc_snprintf(resp->message, sizeof(resp->message),
            "fan mode %s cannot be applied: %s",
            fan_request_mode_name(fan_request_effective_mode(
                req->desired.fanMode, req->desired.fanAuto != 0)),
            fan_request_refusal_text());
        dlog("daemon mutation rejected before write: fan mode %d on a GPU without a"
             " controllable fan (count=%u)\n", req->desired.fanMode, current.fanCount);
        return false;
    }
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
        req, daemon_service_instance_id(), ctl.gpuGeneration,
        topology, currentGpu, requestedDomains);
    if (matches) return true;

    resp->status = SERVICE_STATUS_STALE_STATE;
    gc_snprintf(resp->message, sizeof(resp->message),
        "stale Linux GUI state; refresh before applying "
        "(instance=%llu generation=%llu topology=%llu)",
        (unsigned long long)daemon_service_instance_id(),
        (unsigned long long)ctl.gpuGeneration,
        (unsigned long long)topology);
    dlog("daemon mutation rejected: expected instance=%llu generation=%llu "
         "topology=%llu identity=%d; current instance=%llu generation=%llu "
         "topology=%llu\n",
         (unsigned long long)req->expectedServiceInstanceId,
         (unsigned long long)req->expectedGpuGeneration,
         (unsigned long long)req->expectedTopologySignature,
         identityMatches ? 1 : 0,
         (unsigned long long)daemon_service_instance_id(),
         (unsigned long long)ctl.gpuGeneration,
         (unsigned long long)topology);
    return false;
}
