// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#pragma once

enum ConfiguredGpuResolveResult {
    CONFIGURED_GPU_RESOLVE_LEGACY_ORDINAL = 0,
    CONFIGURED_GPU_RESOLVE_STABLE,
    CONFIGURED_GPU_RESOLVE_NOT_FOUND,
    CONFIGURED_GPU_RESOLVE_AMBIGUOUS,
};

static inline bool configured_gpu_base_identity_matches(
    const GpuAdapterInfo* expected, const GpuAdapterInfo* live) {
    return expected && live && expected->valid && live->valid &&
        expected->pciInfoValid && live->pciInfoValid &&
        expected->deviceId == live->deviceId &&
        expected->subSystemId == live->subSystemId &&
        expected->pciRevisionId == live->pciRevisionId &&
        expected->extDeviceId == live->extDeviceId;
}

static inline bool configured_gpu_identity_has_bdf(
    const GpuAdapterInfo* adapter) {
    return adapter && adapter->pciDomain <= 0xFFFFu &&
        adapter->pciBus <= 255u && adapter->pciDevice <= 31u &&
        adapter->pciFunction <= 7u &&
        (adapter->pciDomain != 0 || adapter->pciBus != 0 ||
         adapter->pciDevice != 0 || adapter->pciFunction != 0);
}

static inline ConfiguredGpuResolveResult resolve_configured_gpu_selection(
    const ConfiguredGpuSelection* configured,
    const GpuAdapterInfo* adapters, unsigned int adapterCount,
    unsigned int* resolvedIndexOut) {
    if (resolvedIndexOut) *resolvedIndexOut = 0;
    if (!configured || !configured->stableIdentityPresent) {
        if (resolvedIndexOut) *resolvedIndexOut = configured
            ? configured->legacyIndex : 0;
        return CONFIGURED_GPU_RESOLVE_LEGACY_ORDINAL;
    }

    const bool expectedHasBdf =
        configured_gpu_identity_has_bdf(&configured->identity);
    unsigned int matches = 0;
    unsigned int matchedIndex = 0;
    for (unsigned int i = 0; adapters && i < adapterCount; ++i) {
        const GpuAdapterInfo* live = &adapters[i];
        if (!configured_gpu_base_identity_matches(
                &configured->identity, live)) continue;
        if (expectedHasBdf) {
            if (!configured_gpu_identity_has_bdf(live) ||
                configured->identity.pciDomain != live->pciDomain ||
                configured->identity.pciBus != live->pciBus ||
                configured->identity.pciDevice != live->pciDevice ||
                configured->identity.pciFunction != live->pciFunction) {
                continue;
            }
        }
        matchedIndex = i;
        ++matches;
    }
    if (matches == 1) {
        if (resolvedIndexOut) *resolvedIndexOut = matchedIndex;
        return CONFIGURED_GPU_RESOLVE_STABLE;
    }
    return matches == 0
        ? CONFIGURED_GPU_RESOLVE_NOT_FOUND
        : CONFIGURED_GPU_RESOLVE_AMBIGUOUS;
}
