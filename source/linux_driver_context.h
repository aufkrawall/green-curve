// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_driver_context.h -- process-wide NVIDIA driver libraries for the Linux
// backend.
//
// One context per process owns the NVML and NvAPI library handles, the
// process's single nvmlInit_v2 reference and the NvAPI initialization.  A GPU
// state (LinuxGpuState) is bound against a context and keeps only per-GPU
// device handles plus a copy of the resolved entry points: it never loads,
// initializes or releases a library.  Its owner closes the context after the
// last GPU state bound to it was shut down.
//
// Discovery is a separate step (linux_driver_discover): one NVML enumeration
// stored in the context.  linux_backend_init() is open + discover + bind, and
// the bind enumerates and matches NvAPI itself, so one GPU sees the same
// sequence as before.  Binding several GPUs from one shared discovery (with
// proof that cached handles still address their cards) is a later multi-GPU
// step; until then every binding uses the discovery its own init just took.

#ifndef GREEN_CURVE_LINUX_DRIVER_CONTEXT_H
#define GREEN_CURVE_LINUX_DRIVER_CONTEXT_H

#include "gpu_core.h"
#include "linux_gpu_uuid.h"
#include "platform.h"

// One NvAPI enumeration with the per-handle PCI evidence a binding matches on,
// taken by that binding. A failure is recorded, not fatal: the binding
// publishes it as its health.
#define LINUX_NVAPI_MAX_HANDLES 64
struct LinuxNvapiInventory {
    bool enumerated;
    // NvAPI was initialized when this inventory was taken (an enumeration
    // failure clears it, a missing enumeration entry point does not).
    bool initialized;
    unsigned int healthReason;
    int status;
    char error[192];
    int handleCount;
    GPU_HANDLE handles[LINUX_NVAPI_MAX_HANDLES];
    int busStatus[LINUX_NVAPI_MAX_HANDLES];
    int slotStatus[LINUX_NVAPI_MAX_HANDLES];
    int pciStatus[LINUX_NVAPI_MAX_HANDLES];
    unsigned int bus[LINUX_NVAPI_MAX_HANDLES];
    unsigned int slot[LINUX_NVAPI_MAX_HANDLES];
    unsigned int device[LINUX_NVAPI_MAX_HANDLES];
    unsigned int subsystem[LINUX_NVAPI_MAX_HANDLES];
    unsigned int revision[LINUX_NVAPI_MAX_HANDLES];
    unsigned int extended[LINUX_NVAPI_MAX_HANDLES];
};

// One NVML enumeration (adapters compacted; devices[] indexed by the NVML
// index each adapter records).
struct LinuxGpuDiscovery {
    unsigned int adapterCount;
    GpuAdapterInfo adapters[MAX_GPU_ADAPTERS];
    LinuxGpuUuid adapterUuids[MAX_GPU_ADAPTERS];
    nvmlDevice_t devices[MAX_GPU_ADAPTERS];
};

struct LinuxDriverContext {
    // NVML: resolved once per load; nvmlInitialized records the one
    // nvmlInit_v2 reference this context owns and releases on close.
    PlLib nvmlLib;
    NvmlApi nvml;
    LinuxGetGpuUuid getUuid;
    // Optional queries, resolved once with the library.
    nvmlReturn_t (*getName)(nvmlDevice_t, char*, unsigned int);
    nvmlReturn_t (*getDriverVersion)(char*, unsigned int);
    bool nvmlInitialized;
    unsigned int nvmlInitAttempts;
    // NvAPI: every binding initializes it before enumerating; a recovery
    // rebind reuses the session unless an enumeration failure cleared this.
    PlLib nvapiLib;
    void* (*nvapiQi)(unsigned int);
    bool nvapiInitialized;
    unsigned int nvapiInitAttempts;
    // The current discovery the binding in the same init reads; discoveries
    // counts them.
    LinuxGpuDiscovery discovery;
    bool discoveryValid;
    unsigned int discoveries;
};

// A failed step names the health reason and driver status the caller
// publishes on the GPU state it was working for.
struct LinuxDriverResult {
    bool ok;
    unsigned int healthReason;
    int status;
};

LinuxDriverResult linux_driver_open_nvml(LinuxDriverContext* driver,
                                         char* err, size_t errSize);
LinuxDriverResult linux_driver_open_nvapi(LinuxDriverContext* driver,
                                          char* err, size_t errSize);
// Enumerates NVML into driver->discovery.
// NVML reporting itself uninitialized clears the context's init so the next
// open initializes again, as a fresh init did before this context existed.
LinuxDriverResult linux_driver_discover(LinuxDriverContext* driver,
                                        char* err, size_t errSize);
// One NvAPI enumeration, taken by a binding (and by its recovery rebind).
// With initializeSession (a binding), NvAPI is initialized again first, as
// every new GPU state did before the shared context; a recovery rebind keeps
// the current session.
void linux_driver_enumerate_nvapi(LinuxDriverContext* driver,
                                  LinuxNvapiInventory* inventory,
                                  bool initializeSession = false);
// Releases the NVML reference (only if this context acquired it) and both
// library handles, then clears the context. Safe to call repeatedly.
void linux_driver_close(LinuxDriverContext* driver);

#endif
