// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "nvml_adapter_uuid_policy.h"

// Every adapter's physical UUID, for the v40 adapter summary. Rebuilt only
// once NVML is matched by PCI identity (never on an ordinal fallback), from
// NVML devices that each match exactly one adapter. An absent getter or a
// failed query leaves an adapter without one; it is never a prerequisite.
static void nvml_refresh_adapter_uuids() {
    NvmlUuidCandidate candidates[64] = {};
    unsigned int nvmlCount = 0, read = 0, unidentified = 0;
    if (g_nvml_api.getUuid && g_nvml_api.getPciInfo && g_nvml_api.getCount &&
        g_nvml_api.getCount(&nvmlCount) == NVML_SUCCESS) {
        for (unsigned int i = 0; i < nvmlCount && i < 64; ++i) {
            nvmlDevice_t device = nullptr;
            nvmlPciInfo_t pci = {};
            if (g_nvml_api.getHandleByIndex(i, &device) != NVML_SUCCESS || !device ||
                g_nvml_api.getPciInfo(device, &pci) != NVML_SUCCESS || pci.pciDeviceId == 0) {
                ++unidentified;
                continue;
            }
            // Every PCI-identified device takes part, with or without a UUID:
            // dropping one could turn an ambiguous match into a wrong one.
            NvmlUuidCandidate& candidate = candidates[read++];
            candidate.pciValid = pci.pciDeviceId != 0;
            candidate.deviceId = pci.pciDeviceId;
            candidate.subSystemId = pci.pciSubSystemId;
            candidate.bus = pci.bus;
            candidate.device = pci.device;
            char text[96] = {};
            if (g_nvml_api.getUuid(device, text, sizeof(text)) == NVML_SUCCESS && memchr(text, 0, sizeof(text)))
                service_gpu_uuid_parse(text, &candidate.uuid);
        }
    }
    // A device whose identity could not be read could be any adapter's.
    if (unidentified || nvmlCount > 64) read = 0;
    EnterCriticalSection(&g_appLock);
    const unsigned int count = g_app.adapterCount < MAX_GPU_ADAPTERS ? g_app.adapterCount : MAX_GPU_ADAPTERS;
    ServiceGpuUuid uuids[MAX_GPU_ADAPTERS] = {};
    const unsigned int assigned = nvml_adapter_uuid_assign(g_app.adapters, count, candidates, read, uuids);
    memset(g_app.adapterNvmlUuid, 0, sizeof(g_app.adapterNvmlUuid));
    memset(g_app.adapterNvmlUuidIdentity, 0, sizeof(g_app.adapterNvmlUuidIdentity));
    for (unsigned int i = 0; i < count; ++i) {
        if (!service_gpu_uuid_present(uuids[i])) continue;
        memcpy(g_app.adapterNvmlUuid[i], uuids[i].bytes, sizeof(uuids[i].bytes));
        g_app.adapterNvmlUuidIdentity[i] = g_app.adapters[i];
    }
    LeaveCriticalSection(&g_appLock);
    debug_log("nvml_select_device: adapter UUIDs assigned=%u of %u adapters (nvml identified=%u unidentified=%u reported=%u%s)\n",
        assigned, count, read, unidentified, nvmlCount, g_nvml_api.getUuid ? "" : ", getter absent");
}

// Compare every candidate to the same NvAPI identity. Enrich the selection
// only after a unique match has been established.
static bool nvml_select_device_for_selected_gpu(char* detail, size_t detailSize) {
    unsigned int count = 0;
    const bool haveCount = g_nvml_api.getCount &&
        g_nvml_api.getCount(&count) == NVML_SUCCESS && count > 0;
    if (!haveCount) count = g_app.adapterCount > 0 ? g_app.adapterCount : 1;
    const GpuAdapterInfo selected = g_app.selectedGpu;
    const bool haveSelectedIdentity = selected.valid && selected.pciInfoValid;
    const bool haveLocation = gpu_adapter_has_valid_pci_location(&selected);
    const bool multiple = count > 1 || g_app.adapterCount > 1;
    if (g_nvml_api.getPciInfo && haveSelectedIdentity) {
        nvmlDevice_t matchedDevice = nullptr;
        unsigned int matchedIndex = 0;
        GpuAdapterInfo matchedIdentity = selected;
        unsigned int observed = 0;
        for (unsigned int i = 0; i < count && i < 64; i++) {
            nvmlDevice_t candidate = nullptr;
            if (g_nvml_api.getHandleByIndex(i, &candidate) != NVML_SUCCESS || !candidate) continue;
            nvmlPciInfo_t pci = {};
            if (g_nvml_api.getPciInfo(candidate, &pci) != NVML_SUCCESS) continue;
            ++observed;
            unsigned int domain = 0, bus = 0, device = 0, function = 0;
            const bool haveFunction = parse_pci_bdf_string(nvml_pci_bus_id_text(&pci),
                &domain, &bus, &device, &function);
            if (!gpu_pci_nvidia_device_ids_match(selected.deviceId, pci.pciDeviceId) ||
                (selected.subSystemId && pci.pciSubSystemId &&
                 selected.subSystemId != pci.pciSubSystemId) ||
                (haveLocation && (selected.pciDomain != pci.domain ||
                 selected.pciBus != pci.bus || selected.pciDevice != pci.device ||
                 (haveFunction ? selected.pciFunction != function : selected.pciFunction != 0)))) {
                debug_log("nvml_select_device: PCI identity mismatch at NVML index %u\n", i);
                continue;
            }
            if (matchedDevice) {
                set_message(detail, detailSize, "Multiple NVML devices match selected GPU PCI identity");
                return false;
            }
            matchedDevice = candidate;
            matchedIndex = i;
            matchedIdentity.pciDomain = pci.domain;
            matchedIdentity.pciBus = pci.bus;
            matchedIdentity.pciDevice = pci.device;
            matchedIdentity.pciFunction = function;
        }
        bool selectedModelUnique = true;
        if (!haveLocation && g_app.adapterCount > 1) {
            for (unsigned int i = 0; i < g_app.adapterCount; ++i) {
                if (i == g_app.selectedGpuIndex) continue;
                const GpuAdapterInfo& other = g_app.adapters[i];
                const bool sameDevice = gpu_pci_nvidia_device_ids_match(selected.deviceId, other.deviceId);
                const bool compatibleSubsystem = selected.subSystemId == 0 || other.subSystemId == 0 ||
                    selected.subSystemId == other.subSystemId;
                if (!other.pciInfoValid || (sameDevice && compatibleSubsystem)) {
                    selectedModelUnique = false;
                    break;
                }
            }
        }
        // Without a location, another unread candidate could be the selected
        // GPU. An exact location match needs no inference from unread entries.
        if (matchedDevice && (haveLocation || (selectedModelUnique && observed == count &&
                (haveCount || !multiple)))) {
            g_app.nvmlDevice = matchedDevice;
            g_app.selectedNvmlIndex = matchedIndex;
            g_app.selectedGpu = matchedIdentity;
            if (g_app.selectedGpuIndex < g_app.adapterCount) {
                GpuAdapterInfo& adapter = g_app.adapters[g_app.selectedGpuIndex];
                adapter.pciDomain = matchedIdentity.pciDomain;
                adapter.pciBus = matchedIdentity.pciBus;
                adapter.pciDevice = matchedIdentity.pciDevice;
                adapter.pciFunction = matchedIdentity.pciFunction;
            }
            g_app.selectedGpuOrdinalFallback = false;
            nvml_refresh_adapter_uuids();
            debug_log("nvml_select_device: matched NVAPI index %u to NVML index %u by PCI identity\n",
                g_app.selectedGpuIndex, matchedIndex);
            return true;
        }
        if (observed || multiple) {
            set_message(detail, detailSize, "No unique NVML device matched selected GPU PCI identity");
            return false;
        }
    }
    if (haveSelectedIdentity && multiple) {
        set_message(detail, detailSize, "NVML PCI identity unavailable on a multi-GPU system");
        return false;
    }
    if (count == 1 && g_app.adapterCount > 1) {
        set_message(detail, detailSize, "Cannot map the only NVML device to the selected NvAPI GPU");
        return false;
    }
    unsigned int fallback = g_app.selectedGpuIndex;
    if (fallback >= count) fallback = 0;
    nvmlDevice_t candidate = nullptr;
    nvmlReturn_t r = g_nvml_api.getHandleByIndex(fallback, &candidate);
    if (r != NVML_SUCCESS || !candidate) {
        set_message(detail, detailSize, "NVML device index %u failed: %s", fallback, nvml_err_name(r));
        return false;
    }
    g_app.nvmlDevice = candidate;
    g_app.selectedNvmlIndex = fallback;
    g_app.selectedGpuOrdinalFallback = multiple;
    if (multiple) {
        debug_log("nvml_select_device: WARNING using ordinal fallback index %u on %u-device system (no PCI identity)\n",
            fallback, count);
    }
    return true;
}
