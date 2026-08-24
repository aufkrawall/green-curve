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

static bool linux_backend_ensure_nvapi(LinuxGpuState* g, char* err,
                                       size_t errSize) {
    if (!g->nvapiLib) g->nvapiLib = pl_open_driver_library(PL_DRIVER_NVAPI);
    if (!g->nvapiLib) {
        gc_strlcpy(err, errSize, "libnvidia-api.so.1 not found");
        linux_backend_publish_health(g,
            SERVICE_GPU_HEALTH_NVAPI_LIBRARY_UNAVAILABLE,
            LB_NVAPI_FUNCTION_MISSING, err);
        return false;
    }
    if (!g->nvapiQi)
        g->nvapiQi = (nvapi_qi_t)pl_lib_sym(g->nvapiLib,
            "nvapi_QueryInterface");
    if (!g->nvapiQi) {
        gc_strlcpy(err, errSize, "nvapi_QueryInterface unavailable");
        linux_backend_publish_health(g, SERVICE_GPU_HEALTH_NVAPI_INIT_FAILED,
            LB_NVAPI_FUNCTION_MISSING, err);
        return false;
    }
    if (g->nvapiInitialized) return true;
    auto init = (nvapi_init_t)g->nvapiQi(NVAPI_INIT_ID);
    if (!init) {
        gc_strlcpy(err, errSize, "NvAPI initialize entry point unavailable");
        linux_backend_publish_health(g, SERVICE_GPU_HEALTH_NVAPI_INIT_FAILED,
            LB_NVAPI_FUNCTION_MISSING, err);
        return false;
    }
    int status = init();
    if (!nvapi_ok(status)) {
        gc_snprintf(err, errSize, "NvAPI initialize status=%d (%s)", status,
            nvapi_status_name(status));
        linux_backend_publish_health(g, SERVICE_GPU_HEALTH_NVAPI_INIT_FAILED,
            status, err);
        return false;
    }
    g->nvapiInitialized = true;
    return true;
}

static bool linux_backend_bind_nvapi(LinuxGpuState* g, bool recovery,
                                     char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!g || !g->nvmlReady || g->selectedAdapterIndex >= g->adapterCount) {
        gc_strlcpy(err, errSize, "NVML GPU selection is unavailable");
        return false;
    }
    linux_backend_clear_vf_freshness(g, true);
    linux_xbar_clear_readback(g);
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
    if (!linux_backend_ensure_nvapi(g, err, errSize)) return false;

    auto enumGpus = (nvapi_enum_t)g->nvapiQi(NVAPI_ENUM_GPU_ID);
    if (!enumGpus) {
        gc_strlcpy(err, errSize, "NvAPI GPU enumeration entry point unavailable");
        linux_backend_publish_health(g, SERVICE_GPU_HEALTH_NVAPI_ENUM_FAILED,
            LB_NVAPI_FUNCTION_MISSING, err);
        return false;
    }
    GPU_HANDLE handles[64] = {};
    int handleCount = 0;
    int enumStatus = enumGpus(handles, &handleCount);
    if (!nvapi_ok(enumStatus) || handleCount <= 0 || handleCount > 64) {
        gc_snprintf(err, errSize, "NvAPI enumerate status=%d (%s), count=%d",
            enumStatus, nvapi_status_name(enumStatus), handleCount);
        g->nvapiInitialized = false;
        linux_backend_publish_health(g, SERVICE_GPU_HEALTH_NVAPI_ENUM_FAILED,
            enumStatus, err);
        return false;
    }

    typedef int (*get_pci_t)(GPU_HANDLE, unsigned int*, unsigned int*,
                             unsigned int*, unsigned int*);
    typedef int (*bus_id_t)(GPU_HANDLE, unsigned int*);
    auto getPci = (get_pci_t)g->nvapiQi(NVAPI_GPU_GET_PCI_IDENTIFIERS_ID);
    auto getBus = (bus_id_t)g->nvapiQi(NVAPI_GPU_GET_BUS_ID_ID);
    auto getSlot = (bus_id_t)g->nvapiQi(NVAPI_GPU_GET_BUS_SLOT_ID_ID);
    auto getArch = (nvapi_arch_t)g->nvapiQi(NVAPI_GPU_GET_ARCH_INFO_ID);
    bool nvapiAssigned[MAX_GPU_ADAPTERS] = {};
    unsigned int handleArchitectures[64] = {};
    int handleArchStatuses[64] = {};
    bool soleFallback = false;
    char soleBindingFailure[192] = {};

    for (int ni = 0; ni < handleCount; ++ni) {
        unsigned int bus = 0, slot = 0;
        int busStatus = getBus ? getBus(handles[ni], &bus) :
            LB_NVAPI_FUNCTION_MISSING;
        int slotStatus = getSlot ? getSlot(handles[ni], &slot) :
            LB_NVAPI_FUNCTION_MISSING;
        unsigned int device = 0, subsystem = 0, revision = 0, extended = 0;
        int pciStatus = getPci ? getPci(handles[ni], &device, &subsystem,
            &revision, &extended) : LB_NVAPI_FUNCTION_MISSING;
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
    lb_log("linux_backend: bound NvAPI handle=%u match=%s family=%d backend=%s arch=0x%08x source=%s recovery=%d\n",
        g->nvapiIndex, soleFallback ? "single-GPU-fallback" : "exact-BDF",
        (int)g->family, g->backend ? g->backend->name : "none",
        g->architecture, linux_gpu_architecture_source_name(g->architectureSource),
        recovery ? 1 : 0);
    return true;
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
    LinuxGpuState* g, char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
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
        if (sscanf(nvml_pci_bus_id_text(&pci), "%x:%x:%x.%x", &domain, &bus,
                   &slot, &function) == 4) {
            candidate.pciDomain = domain;
            candidate.pciBus = bus;
            candidate.pciDevice = slot;
            candidate.pciFunction = function;
        }
        if (!linux_gpu_same_pci_nonconflicting(&g->selectedGpu, &candidate))
            continue;
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

bool linux_backend_init(LinuxGpuState* g, const GpuAdapterInfo* target,
                        char* err, size_t errSize) {
    if (!g) {
        gc_strlcpy(err, errSize, "Linux GPU state is missing");
        return false;
    }
    memset(g, 0, sizeof(*g));
    if (err && errSize) err[0] = 0;
    g->health.reason = SERVICE_GPU_HEALTH_NVML_UNAVAILABLE;
    gc_strlcpy(g->health.detail, sizeof(g->health.detail),
        "NVML backend has not initialized");

    g->nvmlLib = pl_open_driver_library(PL_DRIVER_NVML);
    if (!g->nvmlLib) {
        gc_strlcpy(err, errSize, "libnvidia-ml.so.1 not found");
        linux_backend_publish_health(g, SERVICE_GPU_HEALTH_NVML_UNAVAILABLE,
            NVML_ERROR_FUNCTION_NOT_FOUND, err);
        return false;
    }
    nvml_resolve(g);
    int initStatus = g->nvml.init
        ? g->nvml.init() : NVML_ERROR_FUNCTION_NOT_FOUND;
    if (initStatus != NVML_SUCCESS) {
        gc_snprintf(err, errSize, "nvmlInit_v2 failed status=%d", initStatus);
        linux_backend_publish_health(g, SERVICE_GPU_HEALTH_NVML_UNAVAILABLE,
            initStatus, err);
        return false;
    }
    unsigned int count = 0;
    int countStatus = g->nvml.getCount
        ? g->nvml.getCount(&count) : NVML_ERROR_FUNCTION_NOT_FOUND;
    if (countStatus != NVML_SUCCESS || count == 0) {
        gc_snprintf(err, errSize,
            "NVML device enumeration failed status=%d count=%u",
            countStatus, count);
        linux_backend_publish_health(g, SERVICE_GPU_HEALTH_NVML_UNAVAILABLE,
            countStatus, err);
        return false;
    }
    if (!g->nvml.getHandleByIndex) {
        gc_strlcpy(err, errSize,
            "nvmlDeviceGetHandleByIndex_v2 unavailable");
        linux_backend_publish_health(g, SERVICE_GPU_HEALTH_NVML_UNAVAILABLE,
            NVML_ERROR_FUNCTION_NOT_FOUND, err);
        return false;
    }
    nvmlDeviceGetName_t getName = sym<nvmlDeviceGetName_t>(g->nvmlLib,
        "nvmlDeviceGetName");
    nvmlDevice_t devices[MAX_GPU_ADAPTERS] = {};
    unsigned int enumerate = count > MAX_GPU_ADAPTERS
        ? MAX_GPU_ADAPTERS : count;
    for (unsigned int i = 0; i < enumerate; ++i) {
        if (g->nvml.getHandleByIndex(i, &devices[i]) != NVML_SUCCESS)
            continue;
        GpuAdapterInfo* adapter = &g->adapters[g->adapterCount++];
        adapter->valid = true;
        adapter->nvmlIndex = i;
        adapter->nvapiIndex = MAX_GPU_ADAPTERS;
        if (getName) getName(devices[i], adapter->name,
            sizeof(adapter->name));
        if (!adapter->name[0])
            gc_strlcpy(adapter->name, sizeof(adapter->name), "NVIDIA GPU");
        if (g->nvml.getPciInfo) {
            nvmlPciInfo_t pci = {};
            if (g->nvml.getPciInfo(devices[i], &pci) == NVML_SUCCESS) {
                adapter->pciInfoValid = pci.pciDeviceId != 0;
                adapter->deviceId = pci.pciDeviceId;
                adapter->subSystemId = pci.pciSubSystemId;
                adapter->pciDomain = pci.domain;
                adapter->pciBus = pci.bus;
                adapter->pciDevice = pci.device;
                unsigned int domain = 0, bus = 0, device = 0, function = 0;
                if (sscanf(nvml_pci_bus_id_text(&pci), "%x:%x:%x.%x", &domain, &bus,
                        &device, &function) == 4) {
                    adapter->pciDomain = domain;
                    adapter->pciBus = bus;
                    adapter->pciDevice = device;
                    adapter->pciFunction = function;
                }
            }
        }
    }
    if (g->adapterCount == 0) {
        gc_strlcpy(err, errSize, "no accessible NVML devices");
        linux_backend_publish_health(g, SERVICE_GPU_HEALTH_NVML_UNAVAILABLE,
            NVML_ERROR_NOT_FOUND, err);
        return false;
    }

    int selectedIndex = -1;
    if (target && target->valid)
        selectedIndex = linux_resolve_gpu_identity(target, g->adapters,
            g->adapterCount);
    else if (g->adapterCount == 1)
        selectedIndex = 0;
    if (selectedIndex < 0) {
        if (selectedIndex == -2)
            gc_strlcpy(err, errSize,
                "selected GPU PCI identity is ambiguous");
        else if (g->adapterCount > 1)
            gc_strlcpy(err, errSize,
                "multiple GPUs detected; select one by PCI BDF with --gpu");
        selectedIndex = 0; // telemetry-only until an exact request selects one
    }
    g->selectedAdapterIndex = (unsigned int)selectedIndex;
    g->selectedGpu = g->adapters[selectedIndex];
    g->nvmlIndex = g->selectedGpu.nvmlIndex;
    g->nvmlDevice = devices[g->nvmlIndex];
    g->nvmlReady = g->nvmlDevice != nullptr;
    g->writeIdentityResolved = g->adapterCount == 1 ||
        (target && target->valid &&
         linux_gpu_identity_matches(target, &g->selectedGpu));
    gc_strlcpy(g->gpuName, sizeof(g->gpuName), g->selectedGpu.name);
    nvml_query_ranges(g);
    g->health.reason = SERVICE_GPU_HEALTH_ARCHITECTURE_UNAVAILABLE;
    gc_strlcpy(g->health.detail, sizeof(g->health.detail),
        "NvAPI binding has not completed");
    linux_backend_bind_nvapi(g, false, err, errSize);
    linux_backend_refresh(g);
    return g->nvmlReady;
}

bool linux_backend_select_target(LinuxGpuState* g,
                                 const GpuAdapterInfo* target,
                                 char* err, size_t errSize) {
    if (!g || !target || !target->valid) {
        gc_strlcpy(err, errSize, "a stable GPU target is required");
        return false;
    }
    if (linux_gpu_identity_matches(target, &g->selectedGpu) &&
        g->writeIdentityResolved) return true;
    LinuxGpuState replacement = {};
    if (!linux_backend_init(&replacement, target, err, errSize) ||
        !replacement.writeIdentityResolved) {
        linux_backend_shutdown(&replacement);
        if (err && !err[0])
            gc_strlcpy(err, errSize,
                "selected GPU could not be resolved uniquely");
        return false;
    }
    linux_backend_shutdown(g);
    *g = replacement;
    return true;
}

void linux_backend_shutdown(LinuxGpuState* g) {
    if (!g) return;
    if (g->nvml.shutdown) g->nvml.shutdown();
    if (g->nvmlLib) pl_lib_close(g->nvmlLib);
    if (g->nvapiLib) pl_lib_close(g->nvapiLib);
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
        bool nvmlRebound = linux_backend_reenumerate_same_nvml_gpu(
            g, bindError, sizeof(bindError));
        bool rebound = nvmlRebound && linux_backend_bind_nvapi(
            g, true, bindError, sizeof(bindError));
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
