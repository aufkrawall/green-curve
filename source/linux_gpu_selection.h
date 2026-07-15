// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_LINUX_GPU_SELECTION_H
#define GREEN_CURVE_LINUX_GPU_SELECTION_H

#include "gpu_core.h"

static inline bool linux_gpu_bdf_valid(const GpuAdapterInfo* gpu) {
    return gpu && gpu->valid && gpu->pciDomain <= 0xFFFFu &&
           gpu->pciBus <= 0xFFu && gpu->pciDevice <= 0x1Fu &&
           gpu->pciFunction <= 7u &&
           (gpu->pciDomain || gpu->pciBus || gpu->pciDevice || gpu->pciFunction);
}

static inline bool linux_gpu_device_id_matches(unsigned int nvapiId,
                                                unsigned int nvmlId) {
    if (!nvapiId || !nvmlId) return true;
    if (nvapiId == nvmlId) return true;
    // NVML commonly returns the PCI device/vendor pair (DDDD10DE), while
    // NvAPI variants may return only DDDD.  Never compare the low vendor word:
    // every NVIDIA adapter would otherwise appear to be the same device.
    unsigned int nvapiDevice = nvapiId > 0xFFFFu ? (nvapiId >> 16) : nvapiId;
    unsigned int nvmlDevice = nvmlId > 0xFFFFu ? (nvmlId >> 16) : nvmlId;
    return nvapiDevice == nvmlDevice;
}

static inline bool linux_gpu_identity_matches(const GpuAdapterInfo* requested,
                                               const GpuAdapterInfo* candidate) {
    if (!requested || !candidate || !requested->valid || !candidate->valid)
        return false;
    if (linux_gpu_bdf_valid(requested)) {
        if (!linux_gpu_bdf_valid(candidate) ||
            requested->pciDomain != candidate->pciDomain ||
            requested->pciBus != candidate->pciBus ||
            requested->pciDevice != candidate->pciDevice ||
            requested->pciFunction != candidate->pciFunction)
            return false;
    }
    if (requested->pciInfoValid) {
        if (!candidate->pciInfoValid ||
            !linux_gpu_device_id_matches(requested->deviceId, candidate->deviceId) ||
            (requested->subSystemId && candidate->subSystemId &&
             requested->subSystemId != candidate->subSystemId))
            return false;
    }
    return linux_gpu_bdf_valid(requested) || requested->pciInfoValid;
}

// The Linux daemon currently owns one selected backend and one active intent.
// Switching that backend while an intent belongs to another GPU would make the
// fan worker and state envelope apply/attribute the old intent to the new GPU.
// Until per-GPU runtime ownership exists, selection therefore fails closed.
static inline bool linux_gpu_switch_preserves_active_intent(
    bool hasActiveDesired, const GpuAdapterInfo* activeTarget,
    const GpuAdapterInfo* requestedTarget) {
    return !hasActiveDesired ||
        linux_gpu_identity_matches(activeTarget, requestedTarget);
}

// The snapshot index also names the adapter used for read-only telemetry when
// no multi-GPU write target has been selected. In that state, the first user
// selection must start at an endpoint instead of treating the telemetry index
// as an implicit choice.
static inline int linux_next_gpu_selection_index(
    bool hasSelectedTarget, unsigned int currentIndex,
    unsigned int adapterCount, int delta) {
    if (adapterCount == 0 || adapterCount > MAX_GPU_ADAPTERS) return -1;
    if (!hasSelectedTarget || currentIndex >= adapterCount)
        return delta < 0 ? (int)adapterCount - 1 : 0;
    int count = (int)adapterCount;
    int normalized = ((int)currentIndex + delta) % count;
    return normalized < 0 ? normalized + count : normalized;
}

// Returns the unique matching index, -1 for no match and -2 for ambiguity.
static inline int linux_resolve_gpu_identity(const GpuAdapterInfo* requested,
                                             const GpuAdapterInfo* adapters,
                                             unsigned int count) {
    if (!requested || !adapters) return -1;
    int match = -1;
    for (unsigned int i = 0; i < count; ++i) {
        if (!linux_gpu_identity_matches(requested, &adapters[i])) continue;
        if (match >= 0) return -2;
        match = (int)i;
    }
    return match;
}

#endif
