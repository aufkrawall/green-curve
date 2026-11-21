// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once

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
