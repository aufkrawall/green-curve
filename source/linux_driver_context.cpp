// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Process-wide NVML/NvAPI ownership (linux_driver_context.h). Amalgamated into
// linux_backend.cpp after nvml_resolve() and the NvAPI helpers it uses.

LinuxDriverResult linux_driver_open_nvml(LinuxDriverContext* driver,
                                         char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!driver) {
        gc_strlcpy(err, errSize, "Linux driver context is missing");
        return {false, SERVICE_GPU_HEALTH_NVML_UNAVAILABLE, NVML_ERROR_UNINITIALIZED};
    }
    if (driver->nvmlInitialized) return {true, SERVICE_GPU_HEALTH_NONE, NVML_SUCCESS};
    if (!driver->nvmlLib) {
        driver->nvmlLib = pl_open_driver_library(PL_DRIVER_NVML);
        if (!driver->nvmlLib) {
            gc_strlcpy(err, errSize, "libnvidia-ml.so.1 not found");
            return {false, SERVICE_GPU_HEALTH_NVML_UNAVAILABLE, NVML_ERROR_FUNCTION_NOT_FOUND};
        }
        nvml_resolve(&driver->nvml, driver->nvmlLib);
        driver->getUuid = sym<LinuxGetGpuUuid>(driver->nvmlLib, "nvmlDeviceGetUUID");
        driver->getName = sym<nvmlDeviceGetName_t>(driver->nvmlLib, "nvmlDeviceGetName");
        driver->getDriverVersion = sym<nvmlReturn_t (*)(char*, unsigned int)>(
            driver->nvmlLib, "nvmlSystemGetDriverVersion");
    }
    // A reference is only acquired when close can release it.
    if (!driver->nvml.init || !driver->nvml.shutdown) {
        gc_snprintf(err, errSize, "NVML lifetime entry points unavailable init=%d shutdown=%d",
            driver->nvml.init ? 1 : 0, driver->nvml.shutdown ? 1 : 0);
        return {false, SERVICE_GPU_HEALTH_NVML_UNAVAILABLE, NVML_ERROR_FUNCTION_NOT_FOUND};
    }
    const int initStatus = driver->nvml.init();
    ++driver->nvmlInitAttempts;
    if (initStatus != NVML_SUCCESS) {
        gc_snprintf(err, errSize, "nvmlInit_v2 failed status=%d", initStatus);
        return {false, SERVICE_GPU_HEALTH_NVML_UNAVAILABLE, initStatus};
    }
    driver->nvmlInitialized = true;
    ++driver->nvmlSessions;
    driver->nvmlRecoveryMisses = 0;
    lb_log("linux_driver: NVML initialized attempts=%u\n", driver->nvmlInitAttempts);
    return {true, SERVICE_GPU_HEALTH_NONE, NVML_SUCCESS};
}

LinuxDriverResult linux_driver_open_nvapi(LinuxDriverContext* driver,
                                          char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!driver) {
        gc_strlcpy(err, errSize, "Linux driver context is missing");
        return {false, SERVICE_GPU_HEALTH_NVAPI_LIBRARY_UNAVAILABLE, LB_NVAPI_FUNCTION_MISSING};
    }
    if (!driver->nvapiLib) driver->nvapiLib = pl_open_driver_library(PL_DRIVER_NVAPI);
    if (!driver->nvapiLib) {
        gc_strlcpy(err, errSize, "libnvidia-api.so.1 not found");
        return {false, SERVICE_GPU_HEALTH_NVAPI_LIBRARY_UNAVAILABLE, LB_NVAPI_FUNCTION_MISSING};
    }
    if (!driver->nvapiQi)
        driver->nvapiQi = (nvapi_qi_t)pl_lib_sym(driver->nvapiLib, "nvapi_QueryInterface");
    if (!driver->nvapiQi) {
        gc_strlcpy(err, errSize, "nvapi_QueryInterface unavailable");
        return {false, SERVICE_GPU_HEALTH_NVAPI_INIT_FAILED, LB_NVAPI_FUNCTION_MISSING};
    }
    if (driver->nvapiInitialized) return {true, SERVICE_GPU_HEALTH_NONE, 0};
    auto init = (nvapi_init_t)driver->nvapiQi(NVAPI_INIT_ID);
    if (!init) {
        gc_strlcpy(err, errSize, "NvAPI initialize entry point unavailable");
        return {false, SERVICE_GPU_HEALTH_NVAPI_INIT_FAILED, LB_NVAPI_FUNCTION_MISSING};
    }
    const int status = init();
    ++driver->nvapiInitAttempts;
    if (!nvapi_ok(status)) {
        gc_snprintf(err, errSize, "NvAPI initialize status=%d (%s)", status,
            nvapi_status_name(status));
        return {false, SERVICE_GPU_HEALTH_NVAPI_INIT_FAILED, status};
    }
    driver->nvapiInitialized = true;
    lb_log("linux_driver: NvAPI initialized attempts=%u\n", driver->nvapiInitAttempts);
    return {true, SERVICE_GPU_HEALTH_NONE, 0};
}

void linux_driver_enumerate_nvapi(LinuxDriverContext* driver,
                                  LinuxNvapiInventory* inventory,
                                  bool initializeSession) {
    if (!inventory) return;
    *inventory = LinuxNvapiInventory{};
    // A binding initializes NvAPI before it enumerates, as each new GPU state
    // always did: a session a driver reset invalidated (even one that still
    // enumerates) is renewed. The recovery rebind keeps the current session.
    if (initializeSession && driver) driver->nvapiInitialized = false;
    const LinuxDriverResult opened = linux_driver_open_nvapi(
        driver, inventory->error, sizeof(inventory->error));
    if (!opened.ok) {
        inventory->healthReason = opened.healthReason;
        inventory->status = opened.status;
        return;
    }
    inventory->initialized = true;
    auto enumGpus = (nvapi_enum_t)driver->nvapiQi(NVAPI_ENUM_GPU_ID);
    if (!enumGpus) {
        gc_strlcpy(inventory->error, sizeof(inventory->error),
            "NvAPI GPU enumeration entry point unavailable");
        inventory->healthReason = SERVICE_GPU_HEALTH_NVAPI_ENUM_FAILED;
        inventory->status = LB_NVAPI_FUNCTION_MISSING;
        return;
    }
    int handleCount = 0;
    const int enumStatus = enumGpus(inventory->handles, &handleCount);
    // F-04-001: the same bound the probe path now asks for, stated once.
    if (!nvapi_ok(enumStatus) ||
        !linux_nvapi_enum_count_is_usable(handleCount, LINUX_NVAPI_MAX_HANDLES)) {
        gc_snprintf(inventory->error, sizeof(inventory->error),
            "NvAPI enumerate status=%d (%s), count=%d",
            enumStatus, nvapi_status_name(enumStatus), handleCount);
        // Initialize NvAPI again before the next enumeration.
        driver->nvapiInitialized = false;
        inventory->initialized = false;
        inventory->healthReason = SERVICE_GPU_HEALTH_NVAPI_ENUM_FAILED;
        inventory->status = enumStatus;
        return;
    }
    typedef int (*get_pci_t)(GPU_HANDLE, unsigned int*, unsigned int*,
                             unsigned int*, unsigned int*);
    typedef int (*bus_id_t)(GPU_HANDLE, unsigned int*);
    auto getPci = (get_pci_t)driver->nvapiQi(NVAPI_GPU_GET_PCI_IDENTIFIERS_ID);
    auto getBus = (bus_id_t)driver->nvapiQi(NVAPI_GPU_GET_BUS_ID_ID);
    auto getSlot = (bus_id_t)driver->nvapiQi(NVAPI_GPU_GET_BUS_SLOT_ID_ID);
    for (int ni = 0; ni < handleCount; ++ni) {
        GPU_HANDLE handle = inventory->handles[ni];
        inventory->busStatus[ni] = getBus ? getBus(handle, &inventory->bus[ni])
                                          : LB_NVAPI_FUNCTION_MISSING;
        inventory->slotStatus[ni] = getSlot ? getSlot(handle, &inventory->slot[ni])
                                            : LB_NVAPI_FUNCTION_MISSING;
        inventory->pciStatus[ni] = getPci
            ? getPci(handle, &inventory->device[ni], &inventory->subsystem[ni],
                     &inventory->revision[ni], &inventory->extended[ni])
            : LB_NVAPI_FUNCTION_MISSING;
    }
    inventory->handleCount = handleCount;
    inventory->enumerated = true;
}

LinuxDriverResult linux_driver_discover(LinuxDriverContext* driver,
                                        char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!driver || !driver->nvmlInitialized) {
        gc_strlcpy(err, errSize, "NVML is not initialized");
        return {false, SERVICE_GPU_HEALTH_NVML_UNAVAILABLE, NVML_ERROR_UNINITIALIZED};
    }
    driver->discoveryValid = false;
    ++driver->discoveries;
    LinuxGpuDiscovery* discovery = &driver->discovery;
    *discovery = LinuxGpuDiscovery{};
    const NvmlApi& nvml = driver->nvml;
    unsigned int count = 0;
    const int countStatus = nvml.getCount
        ? nvml.getCount(&count) : NVML_ERROR_FUNCTION_NOT_FOUND;
    if (countStatus == NVML_ERROR_UNINITIALIZED) {
        // The library no longer counts our reference (driver reset/reload):
        // nothing remains to release, and the next open must initialize again.
        driver->nvmlInitialized = false;
        lb_log("linux_driver: NVML reports uninitialized during discovery; "
               "the next open initializes again\n");
    }
    if (countStatus != NVML_SUCCESS || count == 0) {
        gc_snprintf(err, errSize,
            "NVML device enumeration failed status=%d count=%u",
            countStatus, count);
        return {false, SERVICE_GPU_HEALTH_NVML_UNAVAILABLE, countStatus};
    }
    if (!nvml.getHandleByIndex) {
        gc_strlcpy(err, errSize, "nvmlDeviceGetHandleByIndex_v2 unavailable");
        return {false, SERVICE_GPU_HEALTH_NVML_UNAVAILABLE, NVML_ERROR_FUNCTION_NOT_FOUND};
    }
    const auto getName = driver->getName;
    discovery->reportedCount = count;
    const unsigned int enumerate = count > MAX_GPU_ADAPTERS
        ? MAX_GPU_ADAPTERS : count;
    for (unsigned int i = 0; i < enumerate; ++i) {
        if (nvml.getHandleByIndex(i, &discovery->devices[i]) != NVML_SUCCESS)
            continue;
        discovery->adapterUuids[discovery->adapterCount] =
            linux_gpu_read_uuid(driver->getUuid, discovery->devices[i]);
        GpuAdapterInfo* adapter = &discovery->adapters[discovery->adapterCount++];
        adapter->valid = true;
        adapter->nvmlIndex = i;
        adapter->nvapiIndex = MAX_GPU_ADAPTERS;
        if (getName) getName(discovery->devices[i], adapter->name,
            sizeof(adapter->name));
        if (!adapter->name[0])
            gc_strlcpy(adapter->name, sizeof(adapter->name), "NVIDIA GPU");
        if (nvml.getPciInfo) {
            nvmlPciInfo_t pci = {};
            if (nvml.getPciInfo(discovery->devices[i], &pci) == NVML_SUCCESS) {
                adapter->pciInfoValid = pci.pciDeviceId != 0;
                adapter->deviceId = pci.pciDeviceId;
                adapter->subSystemId = pci.pciSubSystemId;
                adapter->pciDomain = pci.domain;
                adapter->pciBus = pci.bus;
                adapter->pciDevice = pci.device;
                unsigned int domain = 0, bus = 0, device = 0, function = 0;
                if (parse_pci_bdf_string(nvml_pci_bus_id_text(&pci), &domain, &bus,
                        &device, &function)) {
                    adapter->pciDomain = domain;
                    adapter->pciBus = bus;
                    adapter->pciDevice = device;
                    adapter->pciFunction = function;
                }
            }
        }
    }
    if (discovery->adapterCount == 0) {
        gc_strlcpy(err, errSize, "no accessible NVML devices");
        return {false, SERVICE_GPU_HEALTH_NVML_UNAVAILABLE, NVML_ERROR_NOT_FOUND};
    }
    driver->discoveryValid = true;
    lb_log("linux_driver: discovery %u adapters=%u reported=%u\n",
        driver->discoveries, discovery->adapterCount, count);
    return {true, SERVICE_GPU_HEALTH_NONE, NVML_SUCCESS};
}

void linux_driver_release_nvml(LinuxDriverContext* driver) {
    if (!driver || !driver->nvmlInitialized) return;
    if (driver->nvml.shutdown) {
        const int status = (int)driver->nvml.shutdown();
        if (status != NVML_SUCCESS)
            lb_log("linux_driver: nvmlShutdown during recovery reset status=%d\n", status);
    }
    driver->nvmlInitialized = false;
}

void linux_driver_close(LinuxDriverContext* driver) {
    if (!driver) return;
    if (driver->nvmlInitialized && driver->nvml.shutdown) {
        const int status = (int)driver->nvml.shutdown();
        if (status != NVML_SUCCESS)
            lb_log("linux_driver: nvmlShutdown status=%d\n", status);
    } else if (driver->nvmlLib) {
        lb_log("linux_driver: close skips nvmlShutdown (init not acquired) attempts=%u\n",
            driver->nvmlInitAttempts);
    }
    if (driver->nvmlLib) pl_lib_close(driver->nvmlLib);
    if (driver->nvapiLib) pl_lib_close(driver->nvapiLib);
    *driver = LinuxDriverContext{};
}
