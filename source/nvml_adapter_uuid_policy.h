// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// nvml_adapter_uuid_policy.h -- which NVML device's UUID belongs to which
// NvAPI adapter, for the v40 adapter summary on Windows.
//
// NvAPI and NVML enumerate in their own orders, so an ordinal says nothing.
// An adapter gets a UUID only from the one NVML device that matches it by PCI
// identity (device ID in either reported shape, compatible subsystem, bus and
// device), and only if that device matches no other adapter. Anything less
// leaves the adapter without a UUID, which is never a prerequisite.

#ifndef GREEN_CURVE_NVML_ADAPTER_UUID_POLICY_H
#define GREEN_CURVE_NVML_ADAPTER_UUID_POLICY_H

#include "gpu_selection_policy.h"  // after gpu_core.h, which defines ServiceGpuUuid

struct NvmlUuidCandidate {
    bool pciValid;
    unsigned int deviceId, subSystemId, bus, device;
    ServiceGpuUuid uuid;
};

static inline bool nvml_uuid_candidate_matches(const GpuAdapterInfo& adapter,
                                               const NvmlUuidCandidate& candidate) {
    return adapter.valid && adapter.pciInfoValid && candidate.pciValid &&
        gpu_pci_nvidia_device_ids_match(adapter.deviceId, candidate.deviceId) &&
        (!adapter.subSystemId || !candidate.subSystemId ||
         adapter.subSystemId == candidate.subSystemId) &&
        adapter.pciBus == candidate.bus && adapter.pciDevice == candidate.device;
}

// out[i] receives adapter i's UUID or stays all zero. Returns how many were assigned.
static inline unsigned int nvml_adapter_uuid_assign(const GpuAdapterInfo* adapters,
    unsigned int count, const NvmlUuidCandidate* candidates, unsigned int candidateCount,
    ServiceGpuUuid* out) {
    unsigned int assigned = 0;
    for (unsigned int i = 0; i < count; ++i) {
        out[i] = ServiceGpuUuid{};
        int match = -1;
        for (unsigned int c = 0; c < candidateCount; ++c) {
            if (!nvml_uuid_candidate_matches(adapters[i], candidates[c])) continue;
            match = match == -1 ? (int)c : -2;
        }
        if (match < 0 || !service_gpu_uuid_present(candidates[match].uuid)) continue;
        bool shared = false;
        for (unsigned int j = 0; j < count; ++j)
            if (j != i && nvml_uuid_candidate_matches(adapters[j], candidates[match])) shared = true;
        if (shared) continue;
        out[i] = candidates[match].uuid;
        ++assigned;
    }
    return assigned;
}

// The identity an inventory entry was matched against; publication requires
// the adapter in that slot to still be the same card.
static inline bool nvml_adapter_uuid_identity_equal(const GpuAdapterInfo& a, const GpuAdapterInfo& b) {
    return a.valid && b.valid && a.pciInfoValid && b.pciInfoValid && a.deviceId == b.deviceId &&
        a.subSystemId == b.subSystemId && a.pciBus == b.pciBus && a.pciDevice == b.pciDevice;
}

#endif  // GREEN_CURVE_NVML_ADAPTER_UUID_POLICY_H
