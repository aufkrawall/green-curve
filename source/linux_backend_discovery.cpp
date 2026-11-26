// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included by linux_backend.cpp; do not compile separately.

static void linux_backend_publish_health(LinuxGpuState* g,
                                         unsigned int reason,
                                         int driverStatus,
                                         const char* detail) {
    if (!g) return;
    g->health.reason = reason;
    g->health.driverStatus = driverStatus;
    g->health.architectureSource = g->architectureSource;
    g->health.vfSnapshotFresh = g->vfSnapshotFresh;
    gc_strlcpy(g->health.detail, sizeof(g->health.detail), detail ? detail : "");
    bool transition = !g->healthLogged ||
        g->lastLoggedHealth.reason != g->health.reason ||
        g->lastLoggedHealth.driverStatus != g->health.driverStatus ||
        g->lastLoggedHealth.architectureSource != g->health.architectureSource ||
        g->lastLoggedHealth.vfSnapshotFresh != g->health.vfSnapshotFresh ||
        g->lastLoggedHealth.availableMutationDomains !=
            g->health.availableMutationDomains;
    if (transition) {
        lb_log("linux_backend: health=%s status=%d archSource=%s vfFresh=%d domains=0x%02x detail=%s\n",
            service_gpu_health_reason_name(reason), driverStatus,
            linux_gpu_architecture_source_name(g->architectureSource),
            g->vfSnapshotFresh ? 1 : 0,
            g->health.availableMutationDomains,
            g->health.detail[0] ? g->health.detail : "none");
        if (!g->vfSnapshotFresh)
            lb_log("linux_backend: vf layout %s\n",
                   linux_vf_layout_text(&g->vfLayout).text);
        g->lastLoggedHealth = g->health;
        g->healthLogged = true;
    }
}

static void linux_backend_clear_vf_freshness(LinuxGpuState* g,
                                             bool clearInfoCache) {
    if (!g) return;
    g->vfInfoFresh = false;
    g->vfStatusFresh = false;
    g->vfControlFresh = false;
    g->vfStructureValid = false;
    g->vfSnapshotFresh = false;
    g->vfFrequenciesHzValid = g->vfOffsetsHzValid = false;
    if (clearInfoCache) g->vfInfoCached = false;
    g->health.vfSnapshotFresh = false;
    g->health.availableMutationDomains &=
        ~SERVICE_MUTATION_DOMAIN_VF_CURVE;
}

static bool linux_backend_cached_architecture_matches(const LinuxGpuState* g,
                                                       const GpuAdapterInfo* gpu) {
    return g && gpu && g->cachedArchitectureValid &&
        linux_gpu_same_pci_nonconflicting(&g->cachedArchitectureGpu, gpu);
}

// The context's query interface, also after a refused NvAPI initialization:
// diagnostics such as the clock-domain dump keep using it, as they did when
// each state resolved its own copy.
static void linux_backend_adopt_nvapi_query(LinuxGpuState* g) {
    if (g->driver && g->driver->nvapiQi) g->nvapiQi = g->driver->nvapiQi;
}

// Binds the selected adapter to its NvAPI handle from an inventory; it never
// enumerates. Every failure is published as this GPU's health.
static bool linux_backend_match_nvapi(LinuxGpuState* g,
                                      const LinuxNvapiInventory* inventory,
                                      bool recovery, char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!g || !g->nvmlReady || g->selectedAdapterIndex >= g->adapterCount) {
        gc_strlcpy(err, errSize, "NVML GPU selection is unavailable");
        return false;
    }
    linux_backend_clear_vf_freshness(g, true);
    linux_xbar_clear_readback(g);
    // A new binding's first ClkDomains refusal is new evidence: log it in full.
    gc_log_steady::invalidate(&g->xbarReadLog);
    g->gpuHandle = nullptr;
    g->backend = nullptr;
    g->architecture = 0;
    g->family = GPU_FAMILY_UNKNOWN;
    g->architectureSource = SERVICE_GPU_ARCH_SOURCE_NONE;
    g->nvapiMatchMethod = LINUX_GPU_MATCH_NONE;
    for (unsigned int i = 0; i < g->adapterCount; ++i) {
        g->adapters[i].nvapiIndex = MAX_GPU_ADAPTERS;
        g->adapters[i].vfReadSupported = false;
        g->adapters[i].vfWriteSupported = false;
        g->adapters[i].vfBestGuess = false;
    }
    linux_backend_adopt_nvapi_query(g);
    g->nvapiReady = inventory && inventory->initialized;
    if (!inventory || !inventory->enumerated) {
        gc_strlcpy(err, errSize, inventory ? inventory->error : "NvAPI inventory is missing");
        linux_backend_publish_health(g,
            inventory ? inventory->healthReason : SERVICE_GPU_HEALTH_NVAPI_ENUM_FAILED,
            inventory ? inventory->status : LB_NVAPI_INVALID_DATA, err);
        return false;
    }
    const int handleCount = inventory->handleCount;
    const GPU_HANDLE* handles = inventory->handles;
    auto getArch = (nvapi_arch_t)g->nvapiQi(NVAPI_GPU_GET_ARCH_INFO_ID);
    bool nvapiAssigned[MAX_GPU_ADAPTERS] = {};
    unsigned int handleArchitectures[LINUX_NVAPI_MAX_HANDLES] = {};
    int handleArchStatuses[LINUX_NVAPI_MAX_HANDLES] = {};
    bool soleFallback = false;
    char soleBindingFailure[192] = {};

    for (int ni = 0; ni < handleCount; ++ni) {
        const unsigned int bus = inventory->bus[ni], slot = inventory->slot[ni];
        const int busStatus = inventory->busStatus[ni];
        const int slotStatus = inventory->slotStatus[ni];
        const unsigned int device = inventory->device[ni];
        const unsigned int subsystem = inventory->subsystem[ni];
        const unsigned int revision = inventory->revision[ni];
        const unsigned int extended = inventory->extended[ni];
        const int pciStatus = inventory->pciStatus[ni];
        LinuxNvapiIdentityObservation observation = {};
        observation.busValid = nvapi_ok(busStatus);
        observation.slotValid = nvapi_ok(slotStatus);
        observation.bus = bus;
        observation.slot = slot;
        observation.pciIdentityValid = nvapi_ok(pciStatus);
        observation.deviceId = device;
        observation.extDeviceId = extended;
        observation.subSystemId = subsystem;
        LinuxGpuBindingDecision binding = linux_gpu_binding_decide(
            g->adapters, g->adapterCount, (unsigned int)handleCount,
            &observation);
        int match = binding.adapterIndex;
        bool fallback = binding.method ==
            LINUX_GPU_MATCH_SOLE_NONCONFLICTING;
        if (match < 0) {
            const GpuAdapterInfo* candidate =
                binding.candidateIndex >= 0 &&
                (unsigned int)binding.candidateIndex < g->adapterCount
                    ? &g->adapters[binding.candidateIndex] : nullptr;
            lb_log("linux_backend: NvAPI handle %d unmatched decision=%d candidate=%d conflict=%d deviceConflict=%d subsystemConflict=%d busStatus=%d bus=%u slotStatus=%d slot=%u pciStatus=%d nvapiDevice=%08x nvapiExtDevice=%08x nvapiSubsystem=%08x nvmlDevice=%08x nvmlSubsystem=%08x\n",
                ni, match, binding.candidateIndex,
                binding.identifiersConflict ? 1 : 0,
                binding.deviceConflict ? 1 : 0,
                binding.subsystemConflict ? 1 : 0,
                busStatus, bus, slotStatus, slot, pciStatus, device,
                extended, subsystem, candidate ? candidate->deviceId : 0,
                candidate ? candidate->subSystemId : 0);
            if (g->adapterCount == 1 && handleCount == 1 && candidate &&
                binding.identifiersConflict) {
                gc_snprintf(soleBindingFailure, sizeof(soleBindingFailure),
                    "sole GPU identity conflict: NVML device=%08x subsystem=%08x; "
                    "NvAPI device=%08x ext=%08x subsystem=%08x (device=%d subsystem=%d)",
                    candidate->deviceId, candidate->subSystemId,
                    device, extended, subsystem,
                    binding.deviceConflict ? 1 : 0,
                    binding.subsystemConflict ? 1 : 0);
            }
            continue;
        }
        GpuAdapterInfo* adapter = &g->adapters[match];
        if (nvapiAssigned[match]) {
            lb_log("linux_backend: NvAPI handle %d creates an ambiguous duplicate mapping for NVML adapter %d; clearing the binding\n",
                ni, match);
            adapter->nvapiIndex = MAX_GPU_ADAPTERS;
            continue;
        }
        nvapiAssigned[match] = true;
        adapter->nvapiIndex = (unsigned int)ni;
        if (observation.pciIdentityValid) {
            // Keep the stable NVML representation when present. NvAPI may
            // return a compatible pair in the opposite 16-bit word order.
            if (!adapter->deviceId)
                adapter->deviceId = extended ? extended : device;
            if (!adapter->subSystemId) adapter->subSystemId = subsystem;
            adapter->pciInfoValid = adapter->deviceId != 0;
            adapter->pciRevisionId = revision;
            adapter->extDeviceId = extended;
        }
        handleArchStatuses[ni] = LB_NVAPI_FUNCTION_MISSING;
        if (getArch) {
            nvapiGpuArchInfo_t info = {};
            info.version = NVAPI_GPU_ARCH_INFO_VER2;
            handleArchStatuses[ni] = getArch(handles[ni], &info);
            if (nvapi_ok(handleArchStatuses[ni]))
                handleArchitectures[ni] = info.architecture;
        }
        if (fallback) {
            soleFallback = true;
            lb_log("linux_backend: accepted sole NVML/NvAPI handle despite bus/slot mismatch (busStatus=%d bus=%u slotStatus=%d slot=%u)\n",
                busStatus, bus, slotStatus, slot);
        }
    }

    GpuAdapterInfo* selected = &g->adapters[g->selectedAdapterIndex];
    if (selected->nvapiIndex >= (unsigned int)handleCount) {
        if (soleBindingFailure[0])
            gc_strlcpy(err, errSize, soleBindingFailure);
        else
            gc_snprintf(err, errSize,
                "NvAPI handle unresolved for selected PCI GPU (nvml=%u nvapi=%d)",
                g->adapterCount, handleCount);
        linux_backend_publish_health(g,
            SERVICE_GPU_HEALTH_NVAPI_HANDLE_UNRESOLVED,
            LB_NVAPI_INVALID_DATA, err);
        g->selectedGpu = *selected;
        return false;
    }

    g->nvapiIndex = selected->nvapiIndex;
    g->gpuHandle = handles[g->nvapiIndex];
    g->nvapiMatchMethod = soleFallback
        ? LINUX_GPU_MATCH_SOLE_NONCONFLICTING
        : LINUX_GPU_MATCH_EXACT_PCI;
    unsigned int architecture = handleArchitectures[g->nvapiIndex];
    int architectureStatus = handleArchStatuses[g->nvapiIndex];
    unsigned int nvmlArchitecture = NVML_DEVICE_ARCH_UNKNOWN;
    int nvmlStatus = NVML_ERROR_FUNCTION_NOT_FOUND;
    // Query NVML only when the single selected-handle NvAPI query failed. A
    // successful first result must never be discarded by a second call.
    if (!nvapi_ok(architectureStatus) || architecture == 0) {
        nvmlStatus = g->nvml.getArchitecture
            ? g->nvml.getArchitecture(g->nvmlDevice, &nvmlArchitecture)
            : NVML_ERROR_FUNCTION_NOT_FOUND;
    }
    bool cachedValid = linux_backend_cached_architecture_matches(g, selected);
    LinuxArchitectureDecision architectureDecision = linux_choose_architecture(
        architectureStatus, architecture, nvmlStatus, nvmlArchitecture,
        cachedValid, g->cachedArchitecture, g->cachedFamily);
    architecture = architectureDecision.architecture;
    GpuFamily family = architectureDecision.family;
    g->architectureSource = architectureDecision.source;
    const VfBackendSpec* backend = family != GPU_FAMILY_UNKNOWN
        ? vf_backend_for_family(family)
        : (architecture ? vf_backend_for_architecture(architecture, &family)
                        : &g_vfBackendFuture);
    if (g->architectureSource == SERVICE_GPU_ARCH_SOURCE_FUTURE_GUESS) {
        lb_log("linux_backend: architecture unavailable/unknown nvapiStatus=%d nvapiArch=0x%08x nvmlStatus=%d nvmlArch=%u; probing future backend read-only first\n",
            architectureStatus, handleArchitectures[g->nvapiIndex],
            nvmlStatus, nvmlArchitecture);
    }

    g->architecture = architecture;
    g->family = family;
    g->backend = backend;
    selected->family = family;
    g->selectedGpu = *selected;
    if (architecture || family != GPU_FAMILY_UNKNOWN) {
        g->cachedArchitecture = architecture;
        g->cachedFamily = family;
        g->cachedArchitectureGpu = *selected;
        g->cachedArchitectureValid = true;
    }
    lb_log("linux_backend: bound NvAPI handle=%u match=%s family=%d backend=%s arch=0x%08x source=%s recovery=%d vfGraphicsScale=%u\n",
        g->nvapiIndex, soleFallback ? "single-GPU-fallback" : "exact-BDF",
        (int)g->family, g->backend ? g->backend->name : "none",
        g->architecture, linux_gpu_architecture_source_name(g->architectureSource),
        recovery ? 1 : 0, vf_graphics_frequency_scale(g->backend));
    return true;
}

// Enumerates NvAPI and matches the selected adapter's handle in one step, as
// a binding always did: the inventory is never reused by a later binding.
// initializeSession: a binding initializes NvAPI first, as each new GPU state
// always did; the recovery rebind keeps the current session.
static bool linux_backend_bind_nvapi(LinuxGpuState* g, bool recovery,
                                     char* err, size_t errSize,
                                     bool initializeSession = false) {
    LinuxNvapiInventory fresh;
    linux_driver_enumerate_nvapi(g ? g->driver : nullptr, &fresh, initializeSession);
    return linux_backend_match_nvapi(g, &fresh, recovery, err, errSize);
}

static unsigned int linux_backend_vf_failure_reason(const LinuxGpuState* g) {
    if (!g) return SERVICE_GPU_HEALTH_NVAPI_HANDLE_UNRESOLVED;
    if (!g->gpuHandle &&
        g->health.reason >= SERVICE_GPU_HEALTH_NVAPI_LIBRARY_UNAVAILABLE &&
        g->health.reason <= SERVICE_GPU_HEALTH_NVAPI_HANDLE_UNRESOLVED)
        return g->health.reason;
    if (!g->gpuHandle)
        return SERVICE_GPU_HEALTH_NVAPI_HANDLE_UNRESOLVED;
    if (g->vfInfoStatus == LB_NVAPI_INVALID_DATA ||
        g->vfStatusStatus == LB_NVAPI_INVALID_DATA ||
        g->vfControlStatus == LB_NVAPI_INVALID_DATA)
        return SERVICE_GPU_HEALTH_VF_STRUCTURE_INVALID;
    if (!g->vfInfoFresh) return SERVICE_GPU_HEALTH_VF_INFO_FAILED;
    if (!g->vfStatusFresh) return SERVICE_GPU_HEALTH_VF_STATUS_FAILED;
    if (!g->vfControlFresh) return SERVICE_GPU_HEALTH_VF_CONTROL_FAILED;
    return SERVICE_GPU_HEALTH_VF_STRUCTURE_INVALID;
}

static int linux_backend_vf_failure_status(const LinuxGpuState* g) {
    if (!g) return LB_NVAPI_INVALID_DATA;
    if (!g->gpuHandle) return g->health.driverStatus
        ? g->health.driverStatus : LB_NVAPI_INVALID_DATA;
    if (g->vfInfoStatus == LB_NVAPI_INVALID_DATA)
        return g->vfInfoStatus;
    if (g->vfStatusStatus == LB_NVAPI_INVALID_DATA)
        return g->vfStatusStatus;
    if (g->vfControlStatus == LB_NVAPI_INVALID_DATA)
        return g->vfControlStatus;
    if (!g->vfInfoFresh) return g->vfInfoStatus;
    if (!g->vfStatusFresh) return g->vfStatusStatus;
    if (!g->vfControlFresh) return g->vfControlStatus;
    return LB_NVAPI_INVALID_DATA;
}

static bool linux_backend_reenumerate_same_nvml_gpu(
    LinuxGpuState* g, char* err, size_t errSize,
    bool* nvmlReinitializedOut = nullptr) {
    if (err && errSize) err[0] = 0;
    if (nvmlReinitializedOut) *nvmlReinitializedOut = false;
    if (!g || !g->nvmlReady || !g->nvml.getCount ||
        !g->nvml.getHandleByIndex || !g->nvml.getPciInfo ||
        (!linux_gpu_bdf_valid(&g->selectedGpu) &&
         !g->selectedGpu.pciInfoValid)) {
        gc_strlcpy(err, errSize,
            "cannot prove the selected PCI GPU during read-only recovery");
        return false;
    }
    unsigned int count = 0;
    int countStatus = g->nvml.getCount(&count);
    if (countStatus == NVML_ERROR_UNINITIALIZED && g->driver &&
        g->driver->nvmlLib) {
        // Same rule as discovery: clear any stale init and attempt one
        // re-initialization through the context so a live bound GPU can
        // recover in place instead of staying stuck on a dead session.
        const bool wasInitialized = g->driver->nvmlInitialized;
        g->driver->nvmlInitialized = false;
        char openErr[192] = {};
        const LinuxDriverResult reopened = linux_driver_open_nvml(
            g->driver, openErr, sizeof(openErr));
        lb_log("linux_backend: NVML reports uninitialized during recovery "
               "(wasInitialized=%d); re-init ok=%d status=%d%s%s\n",
               wasInitialized ? 1 : 0, reopened.ok ? 1 : 0, reopened.status,
               (!reopened.ok && openErr[0]) ? " detail=" : "",
               (!reopened.ok && openErr[0]) ? openErr : "");
        if (reopened.ok) {
            if (nvmlReinitializedOut) *nvmlReinitializedOut = true;
            count = 0;
            countStatus = g->nvml.getCount(&count);
            if (countStatus == NVML_ERROR_UNINITIALIZED) {
                g->driver->nvmlInitialized = false;
                lb_log("linux_backend: NVML still uninitialized after recovery re-init\n");
            }
        }
    }
    if (countStatus != NVML_SUCCESS || count == 0) {
        gc_snprintf(err, errSize,
            "NVML recovery enumeration failed status=%d count=%u",
            countStatus, count);
        return false;
    }
    int match = -1;
    nvmlDevice_t matchedDevice = nullptr;
    GpuAdapterInfo matched = {};
    unsigned int enumerate = count > MAX_GPU_ADAPTERS
        ? MAX_GPU_ADAPTERS : count;
    for (unsigned int index = 0; index < enumerate; ++index) {
        nvmlDevice_t device = nullptr;
        if (g->nvml.getHandleByIndex(index, &device) != NVML_SUCCESS ||
            !device) continue;
        nvmlPciInfo_t pci = {};
        if (g->nvml.getPciInfo(device, &pci) != NVML_SUCCESS) continue;
        GpuAdapterInfo candidate = {};
        candidate.valid = true;
        candidate.pciInfoValid = pci.pciDeviceId != 0;
        candidate.deviceId = pci.pciDeviceId;
        candidate.subSystemId = pci.pciSubSystemId;
        candidate.pciDomain = pci.domain;
        candidate.pciBus = pci.bus;
        candidate.pciDevice = pci.device;
        unsigned int domain = 0, bus = 0, slot = 0, function = 0;
        if (parse_pci_bdf_string(nvml_pci_bus_id_text(&pci), &domain, &bus,
                   &slot, &function)) {
            candidate.pciDomain = domain;
            candidate.pciBus = bus;
            candidate.pciDevice = slot;
            candidate.pciFunction = function;
        }
        if (!linux_gpu_same_pci_nonconflicting(&g->selectedGpu, &candidate))
            continue;
        const auto observed = linux_gpu_read_uuid(g->getUuid, device);
        if (!linux_gpu_uuid_compatible(linux_backend_selected_uuid(g), &observed)) {
            gc_strlcpy(err, errSize,
                "different physical GPU at the selected PCI location during recovery");
            return false;
        }
        if (match >= 0) {
            gc_strlcpy(err, errSize,
                "selected PCI GPU became ambiguous during recovery");
            return false;
        }
        match = (int)index;
        matchedDevice = device;
        matched = candidate;
    }
    if (match < 0 || !matchedDevice) {
        gc_strlcpy(err, errSize,
            "selected PCI GPU disappeared during recovery; refusing ordinal fallback");
        return false;
    }
    g->nvmlDevice = matchedDevice;
    g->nvmlIndex = (unsigned int)match;
    if (g->selectedAdapterIndex < g->adapterCount) {
        GpuAdapterInfo* selected = &g->adapters[g->selectedAdapterIndex];
        selected->nvmlIndex = (unsigned int)match;
        selected->pciInfoValid = matched.pciInfoValid;
        selected->deviceId = matched.deviceId;
        selected->subSystemId = matched.subSystemId;
        selected->pciDomain = matched.pciDomain;
        selected->pciBus = matched.pciBus;
        selected->pciDevice = matched.pciDevice;
        selected->pciFunction = matched.pciFunction;
        g->selectedGpu = *selected;
    }
    lb_log("linux_backend: recovery re-enumerated the same PCI GPU at NVML index %d\n",
           match);
    return true;
}

static void linux_backend_reset_state(LinuxGpuState* g, LinuxDriverContext* driver) {
    memset(g, 0, sizeof(*g));
    g->driver = driver;
    g->health.reason = SERVICE_GPU_HEALTH_NVML_UNAVAILABLE;
    gc_strlcpy(g->health.detail, sizeof(g->health.detail),
        "NVML backend has not initialized");
    if (driver) {
        // Copies of the resolved entry points, also after a failed init.
        g->nvml = driver->nvml;
        g->getUuid = driver->getUuid;
    }
}

static bool linux_backend_bind_discovered(LinuxGpuState* g, LinuxDriverContext* driver,
                                          const GpuAdapterInfo* target,
                                          char* err, size_t errSize, GpuLocationRole role,
                                          const LinuxGpuUuid* uuid);

bool linux_backend_init(LinuxGpuState* g, LinuxDriverContext* driver,
                        const GpuAdapterInfo* target,
                        char* err, size_t errSize, GpuLocationRole role,
                        const LinuxGpuUuid* uuid) {
    if (!g) {
        gc_strlcpy(err, errSize, "Linux GPU state is missing");
        return false;
    }
    if (err && errSize) err[0] = 0;
    // Only a cached session can have been lost: a cold start that fails keeps
    // its single attempt, as before.
    const bool reusedSession = driver && driver->nvmlInitialized;
    LinuxDriverResult step = linux_driver_open_nvml(driver, err, errSize);
    if (step.ok) step = linux_driver_discover(driver, err, errSize);
    if (!step.ok && reusedSession && step.status == NVML_ERROR_UNINITIALIZED &&
        driver->nvmlLib && !driver->nvmlInitialized) {
        // The driver dropped NVML since the last discovery (reset/reload) and
        // discovery just cleared the stale init. Initialize again in this same
        // call, as a fresh per-state init did before the shared context, so a
        // one-shot caller (read retry, resume) recovers on its first attempt.
        lb_log("linux_backend: NVML was reset; initializing again before binding\n");
        step = linux_driver_open_nvml(driver, err, errSize);
        if (step.ok) step = linux_driver_discover(driver, err, errSize);
    }
    if (!step.ok) {
        linux_backend_reset_state(g, driver);
        linux_backend_publish_health(g, step.healthReason, step.status, err);
        return false;
    }
    // The bind resets the state itself before using the discovery.
    return linux_backend_bind_discovered(g, driver, target, err, errSize, role, uuid);
}

// Binds `g` from the discovery this same init call just took, exactly as a
// fresh init always did. Binding several GPUs from one shared discovery (with
// proof that cached handles still address their cards) belongs to the
// multi-GPU step.
static bool linux_backend_bind_discovered(LinuxGpuState* g, LinuxDriverContext* driver,
                                          const GpuAdapterInfo* target,
                                          char* err, size_t errSize, GpuLocationRole role,
                                          const LinuxGpuUuid* uuid) {
    if (!g) {
        gc_strlcpy(err, errSize, "Linux GPU state is missing");
        return false;
    }
    if (err && errSize) err[0] = 0;
    linux_backend_reset_state(g, driver);
    if (!driver || !driver->discoveryValid) {
        gc_strlcpy(err, errSize, "GPU discovery is unavailable");
        linux_backend_publish_health(g, SERVICE_GPU_HEALTH_NVML_UNAVAILABLE,
            NVML_ERROR_UNINITIALIZED, err);
        return false;
    }
    const LinuxGpuDiscovery& discovery = driver->discovery;
    g->adapterCount = discovery.adapterCount;
    memcpy(g->adapters, discovery.adapters, sizeof(g->adapters));
    memcpy(g->adapterUuids, discovery.adapterUuids, sizeof(g->adapterUuids));
    const nvmlDevice_t* devices = discovery.devices;

    // UUID evidence is optional; its fallback is the existing location resolver.
    const bool targeted = target && target->valid;
    const LinuxGpuInitSelection selection =
        linux_gpu_restore_selection(target, role, g->adapters, g->adapterCount,
            uuid, g->adapterUuids, &g->uuidVerdict);
    if (targeted)
        lb_log("linux_backend: restore identity uuid=%s stored=%d\n",
            linux_gpu_uuid_verdict_name(g->uuidVerdict), linux_gpu_uuid_valid(uuid) ? 1 : 0);
    int selectedIndex = selection.index;
    char selectionErr[192] = {};
    if (targeted && (selection.index < 0 || selection.moved)) {
        char detail[256] = {};
        linux_format_stored_gpu_detail(target, role,
            g->adapters, g->adapterCount, detail, sizeof(detail));
        lb_log("linux_backend: stored GPU target %s (%s)\n",
               selection.index >= 0
                   ? "followed its model to a new PCI location"
                   : selection.index == -2 ? "is ambiguous" : "is not present",
               detail);
    }
    if (selectedIndex < 0) {
        if (g->uuidVerdict == LINUX_GPU_UUID_MISMATCH)
            gc_strlcpy(selectionErr, sizeof(selectionErr),
                "the available GPU has a different physical identity; select the intended GPU explicitly");
        else if (selectedIndex == -2)
            gc_strlcpy(selectionErr, sizeof(selectionErr),
                "selected GPU PCI identity is ambiguous");
        else if (targeted)
            gc_strlcpy(selectionErr, sizeof(selectionErr),
                "the stored GPU is not present (a different model, or moved "
                "among several GPUs); select one by PCI BDF with --gpu");
        else if (g->adapterCount > 1)
            gc_strlcpy(selectionErr, sizeof(selectionErr),
                "multiple GPUs detected; select one by PCI BDF with --gpu");
        selectedIndex = 0; // telemetry-only until an exact request selects one
    }
    g->selectedAdapterIndex = (unsigned int)selectedIndex;
    g->selectedGpu = g->adapters[selectedIndex];
    g->nvmlIndex = g->selectedGpu.nvmlIndex;
    g->nvmlDevice = devices[g->nvmlIndex];
    g->nvmlReady = g->nvmlDevice != nullptr;
    g->writeIdentityResolved = selection.writeResolved;
    gc_strlcpy(g->gpuName, sizeof(g->gpuName), g->selectedGpu.name);
    nvml_query_ranges(g);
    g->health.reason = SERVICE_GPU_HEALTH_ARCHITECTURE_UNAVAILABLE;
    gc_strlcpy(g->health.detail, sizeof(g->health.detail),
        "NvAPI binding has not completed");
    linux_backend_bind_nvapi(g, false, err, errSize, true);
    linux_backend_refresh(g);
    // The selection reason outranks a later binding detail: it is what a
    // caller that needs a write target has to show.
    if (selectionErr[0]) gc_strlcpy(err, errSize, selectionErr);
    return g->nvmlReady;
}

bool linux_backend_select_target(LinuxGpuState* g,
                                 const GpuAdapterInfo* target,
                                 char* err, size_t errSize, GpuLocationRole role,
                                 const LinuxGpuUuid* uuid, LinuxGpuUuidVerdict* uuidVerdict) {
    if (uuidVerdict) *uuidVerdict = LINUX_GPU_UUID_FALLBACK;
    if (!g || !target || !target->valid) {
        gc_strlcpy(err, errSize, "a stable GPU target is required");
        return false;
    }
    // A stored target whose location moved still names the adapter already
    // selected; re-initializing the backend for it on every request would
    // only repeat the same resolution.
    int current = linux_resolve_stored_gpu_target(target,
        role, g->adapters, g->adapterCount, nullptr);
    if (g->graphicsRecovery.covered) {
        const auto recoveryTarget = linux_gpu_restore_selection(target, role, g->adapters, g->adapterCount,
            uuid, g->adapterUuids, nullptr);
        if (recoveryTarget.index < 0 || (unsigned int)recoveryTarget.index != g->selectedAdapterIndex) {
            gc_strlcpy(err, errSize, "Reset named graphics controls on their selected GPU before changing targets");
            return false;
        }
    }
    if (g->writeIdentityResolved && current >= 0 &&
        (unsigned int)current == g->selectedAdapterIndex) {
        const LinuxGpuUuid observed = linux_gpu_read_uuid(g->getUuid, g->nvmlDevice);
        if (linux_gpu_uuid_compatible(uuid, &observed)) {
            // A fresh query proves a match. If the optional query fails, keep
            // the old fast path rather than requiring a new NVML init to work.
            g->adapterUuids[current] = observed;
            if (uuidVerdict && linux_gpu_uuid_valid(uuid) && linux_gpu_uuid_valid(&observed))
                *uuidVerdict = LINUX_GPU_UUID_MATCHED;
            lb_log("linux_backend: restore identity uuid=%s\n",
                linux_gpu_uuid_valid(uuid) && linux_gpu_uuid_valid(&observed)
                    ? "matched" : "fallback");
            return true;
        }
    }
    LinuxGpuState replacement = {};
    if (!linux_backend_init(&replacement, g->driver, target, err, errSize, role, uuid) ||
        !replacement.writeIdentityResolved) {
        if (uuidVerdict) *uuidVerdict = replacement.uuidVerdict;
        linux_backend_shutdown(&replacement);
        if (err && !err[0])
            gc_strlcpy(err, errSize,
                "selected GPU could not be resolved uniquely");
        return false;
    }
    if (g->graphicsRecovery.covered) {
        if (!linux_backend_recovery_binding_matches(g, &replacement)) {
            linux_backend_shutdown(&replacement);
            gc_strlcpy(err, errSize, "Backend replacement cannot discard named graphics recovery");
            return false;
        }
        replacement.graphicsRecovery = g->graphicsRecovery;
    }
    linux_backend_shutdown(g);
    if (uuidVerdict) *uuidVerdict = replacement.uuidVerdict;
    *g = replacement;
    return true;
}

void linux_backend_shutdown(LinuxGpuState* g) {
    if (!g) return;
    // Library handles and the NVML reference belong to the driver context;
    // a GPU state, failed or live, never releases them.
    memset(g, 0, sizeof(*g));
}

LinuxBackendRefreshResult linux_backend_refresh(LinuxGpuState* g) {
    LinuxBackendRefreshResult result = {};
    if (!g || !g->nvmlReady) {
        if (g) linux_backend_publish_health(g,
            SERVICE_GPU_HEALTH_NVML_UNAVAILABLE, NVML_ERROR_UNINITIALIZED,
            "NVML GPU is unavailable");
        if (g) result.health = g->health;
        return result;
    }
    result.nvmlReady = true;
    nvml_query_ranges(g);
    g->health.recoveryAttempted = false;
    g->health.recoverySucceeded = false;
    bool recoveryAttempted = false;

    char detail[192] = {};
    bool fresh = g->gpuHandle && g->backend &&
        nvapi_read_vf_snapshot(g, detail, sizeof(detail));
    if (!fresh) {
        unsigned int reason = linux_backend_vf_failure_reason(g);
        int status = linux_backend_vf_failure_status(g);
        if (!detail[0])
            gc_strlcpy(detail, sizeof(detail),
                g->health.detail[0] ? g->health.detail :
                "VF snapshot is unavailable");
        linux_backend_publish_health(g, reason, status, detail);
        recoveryAttempted = true;
        g->health.recoveryAttempted = true;
        char bindError[192] = {};
        bool nvmlReinitialized = false;
        bool nvmlRebound = linux_backend_reenumerate_same_nvml_gpu(
            g, bindError, sizeof(bindError), &nvmlReinitialized);
        if (nvmlRebound && nvmlReinitialized) nvml_query_ranges(g);
        bool rebound = nvmlRebound && linux_backend_bind_nvapi(
            g, true, bindError, sizeof(bindError), nvmlReinitialized);
        detail[0] = 0;
        fresh = rebound && nvapi_read_vf_snapshot(g, detail, sizeof(detail));
        if (!rebound && bindError[0])
            gc_strlcpy(detail, sizeof(detail), bindError);
        g->health.recoverySucceeded = fresh;
    }

    GpuAdapterInfo* selected = g->selectedAdapterIndex < g->adapterCount
        ? &g->adapters[g->selectedAdapterIndex] : nullptr;
    if (fresh) {
        if (selected) {
            selected->family = g->family;
            selected->vfReadSupported = g->backend &&
                g->backend->readSupported;
            selected->vfWriteSupported = g->backend &&
                g->backend->writeSupported;
            selected->vfBestGuess = g->backend &&
                g->backend->bestGuessOnly;
            g->selectedGpu = *selected;
        }
        gc_snprintf(detail, sizeof(detail),
            "fresh VF snapshot (%d points, backend=%s, match=%s, architecture=%s)",
            g->numPopulated, g->backend ? g->backend->name : "none",
            linux_gpu_match_method_name(g->nvapiMatchMethod),
            linux_gpu_architecture_source_name(g->architectureSource));
        linux_backend_publish_health(g, SERVICE_GPU_HEALTH_NONE, 0, detail);
    } else {
        if (selected) {
            selected->vfReadSupported = false;
            selected->vfWriteSupported = false;
            selected->vfBestGuess = false;
            g->selectedGpu = *selected;
        }
        unsigned int reason = linux_backend_vf_failure_reason(g);
        int status = linux_backend_vf_failure_status(g);
        if (!detail[0]) gc_strlcpy(detail, sizeof(detail),
            g->health.detail[0] ? g->health.detail :
            "VF recovery did not produce a complete snapshot");
        linux_backend_publish_health(g, reason, status, detail);
    }
    g->health.recoveryAttempted = recoveryAttempted;
    g->health.recoverySucceeded = fresh && recoveryAttempted;
    g->health.vfSnapshotFresh = fresh;
    result.vfFresh = fresh;
    result.health = g->health;
    return result;
}
