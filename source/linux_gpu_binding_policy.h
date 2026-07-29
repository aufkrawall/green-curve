// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Pure NvAPI-to-NVML matching policy for deterministic Linux discovery tests.

#ifndef GREEN_CURVE_LINUX_GPU_BINDING_POLICY_H
#define GREEN_CURVE_LINUX_GPU_BINDING_POLICY_H

#include "linux_gpu_selection.h"

enum LinuxGpuMatchMethod {
    LINUX_GPU_MATCH_NONE = 0,
    LINUX_GPU_MATCH_EXACT_PCI = 1,
    LINUX_GPU_MATCH_SOLE_NONCONFLICTING = 2,
};

static inline const char* linux_gpu_match_method_name(
    LinuxGpuMatchMethod method) {
    switch (method) {
        case LINUX_GPU_MATCH_EXACT_PCI: return "exact-pci";
        case LINUX_GPU_MATCH_SOLE_NONCONFLICTING:
            return "sole-nonconflicting";
        default: return "none";
    }
}

struct LinuxNvapiIdentityObservation {
    bool busValid;
    bool slotValid;
    unsigned int bus;
    unsigned int slot;
    bool pciIdentityValid;
    unsigned int deviceId;
    unsigned int extDeviceId;
    unsigned int subSystemId;
};

struct LinuxGpuBindingDecision {
    int adapterIndex; // -1 no match, -2 ambiguous
    int candidateIndex; // unique location/fallback candidate before ID checks
    LinuxGpuMatchMethod method;
    bool identifiersConflict;
    bool deviceConflict;
    bool subsystemConflict;
};

static inline LinuxGpuBindingDecision linux_gpu_binding_decide(
    const GpuAdapterInfo* adapters, unsigned int adapterCount,
    unsigned int nvapiHandleCount,
    const LinuxNvapiIdentityObservation* observation) {
    LinuxGpuBindingDecision decision = {};
    decision.adapterIndex = -1;
    decision.candidateIndex = -1;
    if (!adapters || !observation || adapterCount == 0 ||
        adapterCount > MAX_GPU_ADAPTERS || nvapiHandleCount == 0)
        return decision;

    if (observation->busValid && observation->slotValid) {
        for (unsigned int index = 0; index < adapterCount; ++index) {
            if (!adapters[index].valid ||
                adapters[index].pciBus != observation->bus ||
                adapters[index].pciDevice != observation->slot) continue;
            if (decision.adapterIndex >= 0) {
                decision.adapterIndex = -2;
                decision.candidateIndex = -2;
                return decision;
            }
            decision.adapterIndex = (int)index;
        }
    }

    if (decision.adapterIndex == -1 && adapterCount == 1 &&
        nvapiHandleCount == 1) {
        decision.adapterIndex = 0;
        decision.method = LINUX_GPU_MATCH_SOLE_NONCONFLICTING;
    } else if (decision.adapterIndex >= 0) {
        decision.method = LINUX_GPU_MATCH_EXACT_PCI;
    }
    if (decision.adapterIndex < 0) return decision;

    decision.candidateIndex = decision.adapterIndex;
    const GpuAdapterInfo* adapter = &adapters[decision.adapterIndex];
    if (observation->pciIdentityValid && adapter->pciInfoValid) {
        bool haveNvapiDevice = observation->deviceId != 0 ||
            observation->extDeviceId != 0;
        bool deviceMatches = observation->deviceId &&
            linux_gpu_device_id_matches(observation->deviceId,
                                        adapter->deviceId);
        if (!deviceMatches && observation->extDeviceId)
            deviceMatches = linux_gpu_device_id_matches(
                observation->extDeviceId, adapter->deviceId);
        decision.deviceConflict = haveNvapiDevice && !deviceMatches;
        decision.subsystemConflict = observation->subSystemId &&
            adapter->subSystemId && !linux_gpu_subsystem_id_matches(
                observation->subSystemId, adapter->subSystemId);
        decision.identifiersConflict = decision.deviceConflict ||
            decision.subsystemConflict;
    }
    // Multiple GPUs require a positive device/subsystem identity read in
    // addition to a unique bus/device pair. The sole-device fallback accepts
    // unavailable identifiers, but never contradictory ones.
    if (decision.identifiersConflict ||
        (adapterCount > 1 &&
         (!observation->pciIdentityValid ||
          (!observation->deviceId && !observation->extDeviceId)))) {
        decision.adapterIndex = -1;
        decision.method = LINUX_GPU_MATCH_NONE;
    }
    return decision;
}

#endif // GREEN_CURVE_LINUX_GPU_BINDING_POLICY_H
