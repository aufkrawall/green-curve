// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Pure PCI identity predicates for Windows GpuAdapterInfo records. Shared by
// the GUI/service unity build (gpu_backend.cpp and its callers) and the
// regression harness, so the rules that decide "is this the same GPU" are
// executed by tests instead of only being read.

#pragma once

#include "gpu_core.h"

static inline bool gpu_adapter_has_valid_pci_location(
    const GpuAdapterInfo* adapter) {
    return adapter && adapter->pciDomain <= 0xFFFFu &&
        adapter->pciBus <= 255u && adapter->pciDevice <= 31u &&
        adapter->pciFunction <= 7u &&
        (adapter->pciDomain != 0 || adapter->pciBus != 0 ||
         adapter->pciDevice != 0 || adapter->pciFunction != 0);
}

static inline bool gpu_adapter_has_same_pci_base_identity(
    const GpuAdapterInfo* a, const GpuAdapterInfo* b) {
    return a && b && a->valid && b->valid && a->pciInfoValid &&
        b->pciInfoValid && a->deviceId == b->deviceId &&
        a->subSystemId == b->subSystemId &&
        a->pciRevisionId == b->pciRevisionId &&
        a->extDeviceId == b->extDeviceId;
}

static inline bool gpu_adapter_has_same_pci_identity(
    const GpuAdapterInfo* a, const GpuAdapterInfo* b) {
    if (!gpu_adapter_has_same_pci_base_identity(a, b)) return false;
    bool aHasBdf = gpu_adapter_has_valid_pci_location(a);
    bool bHasBdf = gpu_adapter_has_valid_pci_location(b);
    // BDF is a strengthening field, not a wire-compatibility requirement.
    // Older/current client snapshots can carry the stable PCI IDs without the
    // service-side NVML/NVAPI location enrichment.  Reject a different BDF
    // when both sides know it, but do not make ordinary explicit Apply fail
    // merely because exactly one side has already been enriched.  Durable
    // recovery still counts all base-ID matches and rejects duplicates.
    if (!aHasBdf || !bHasBdf) return true;
    return a->pciDomain == b->pciDomain && a->pciBus == b->pciBus &&
        a->pciDevice == b->pciDevice &&
        a->pciFunction == b->pciFunction;
}

static inline bool gpu_adapter_pci_base_identity_is_unique(
    const GpuAdapterInfo* target, const GpuAdapterInfo* adapters,
    unsigned int adapterCount) {
    if (!target || !adapters || !target->pciInfoValid) return false;
    unsigned int matches = 0;
    for (unsigned int i = 0; i < adapterCount; ++i) {
        if (adapters[i].valid &&
            gpu_adapter_has_same_pci_base_identity(target, &adapters[i])) {
            ++matches;
        }
    }
    return matches == 1;
}

// Whether `current` is provably the GPU `expected` names, as a hardware write
// target. PCI identity decides when both sides carry it; a weak ordinal
// identity is accepted only on an unambiguous one-adapter system.
static inline bool gpu_adapter_is_same_write_target(
    const GpuAdapterInfo* expected, const GpuAdapterInfo* current,
    unsigned int adapterCount) {
    if (!expected || !current || !expected->valid || !current->valid)
        return false;
    if (expected->pciInfoValid && current->pciInfoValid)
        return gpu_adapter_has_same_pci_identity(expected, current);
    return adapterCount == 1 && expected->nvapiIndex == current->nvapiIndex;
}
