// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_LINUX_GPU_SELECTION_H
#define GREEN_CURVE_LINUX_GPU_SELECTION_H

#include "gpu_core.h"
#include "gpu_selection_policy.h"

static inline bool linux_gpu_bdf_valid(const GpuAdapterInfo* gpu) {
    return gpu && gpu->valid && gpu->pciDomain <= 0xFFFFu &&
           gpu->pciBus <= 0xFFu && gpu->pciDevice <= 0x1Fu &&
           gpu->pciFunction <= 7u &&
           (gpu->pciDomain || gpu->pciBus || gpu->pciDevice || gpu->pciFunction);
}

static inline bool linux_gpu_extract_nvidia_device_id(
    unsigned int encoded, unsigned int* deviceId) {
    const unsigned int nvidiaVendorId = 0x10DEu;
    if (!encoded || !deviceId) return false;
    if (encoded <= 0xFFFFu) {
        if (encoded == nvidiaVendorId) return false;
        *deviceId = encoded;
        return true;
    }
    unsigned int low = encoded & 0xFFFFu;
    unsigned int high = (encoded >> 16) & 0xFFFFu;
    // NvAPI and NVML releases have exposed the 16-bit vendor/device pair in
    // both word orders. Identify the device by the NVIDIA vendor word instead
    // of assuming that either the high or low word is always the device.
    if (low == nvidiaVendorId && high && high != nvidiaVendorId) {
        *deviceId = high;
        return true;
    }
    if (high == nvidiaVendorId && low && low != nvidiaVendorId) {
        *deviceId = low;
        return true;
    }
    return false;
}

static inline bool linux_gpu_device_id_matches(unsigned int left,
                                                unsigned int right) {
    if (!left || !right) return true;
    if (left == right) return true;
    unsigned int leftDevice = 0, rightDevice = 0;
    return linux_gpu_extract_nvidia_device_id(left, &leftDevice) &&
        linux_gpu_extract_nvidia_device_id(right, &rightDevice) &&
        leftDevice == rightDevice;
}

static inline bool linux_gpu_subsystem_id_matches(unsigned int left,
                                                   unsigned int right) {
    if (!left || !right) return true;
    if (left == right) return true;
    unsigned int leftLow = left & 0xFFFFu;
    unsigned int leftHigh = (left >> 16) & 0xFFFFu;
    unsigned int rightLow = right & 0xFFFFu;
    unsigned int rightHigh = (right >> 16) & 0xFFFFu;
    if (left > 0xFFFFu && right > 0xFFFFu)
        return leftLow == rightHigh && leftHigh == rightLow;
    if (left <= 0xFFFFu)
        return left == rightLow || left == rightHigh;
    return right == leftLow || right == leftHigh;
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
             !linux_gpu_subsystem_id_matches(requested->subSystemId,
                                             candidate->subSystemId)))
            return false;
    }
    return linux_gpu_bdf_valid(requested) || requested->pciInfoValid;
}

// Read-only recovery already owns a known PCI GPU and must tolerate a transient
// loss of optional device/subsystem metadata. Exact BDF remains mandatory when
// available, and any identifiers present on both sides must not conflict.
// Mutation attachment intentionally continues using the stricter helper above.
static inline bool linux_gpu_same_pci_nonconflicting(
    const GpuAdapterInfo* known, const GpuAdapterInfo* candidate) {
    if (!known || !candidate || !known->valid || !candidate->valid)
        return false;
    if (linux_gpu_bdf_valid(known) && linux_gpu_bdf_valid(candidate)) {
        if (known->pciDomain != candidate->pciDomain ||
            known->pciBus != candidate->pciBus ||
            known->pciDevice != candidate->pciDevice ||
            known->pciFunction != candidate->pciFunction) return false;
        return !(known->pciInfoValid && candidate->pciInfoValid) ||
            (linux_gpu_device_id_matches(known->deviceId,
                                         candidate->deviceId) &&
             (!known->subSystemId || !candidate->subSystemId ||
              linux_gpu_subsystem_id_matches(known->subSystemId,
                                              candidate->subSystemId)));
    }
    return linux_gpu_identity_matches(known, candidate);
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
