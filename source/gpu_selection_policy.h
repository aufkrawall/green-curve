// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#pragma once

#include <string.h>

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

static inline bool gpu_adapter_info_equal(const GpuAdapterInfo* left,
                                          const GpuAdapterInfo* right) {
    if (!left || !right) return left == right;
    return left->valid == right->valid &&
        left->pciInfoValid == right->pciInfoValid &&
        left->vfReadSupported == right->vfReadSupported &&
        left->vfWriteSupported == right->vfWriteSupported &&
        left->vfBestGuess == right->vfBestGuess &&
        left->nvapiIndex == right->nvapiIndex &&
        left->nvmlIndex == right->nvmlIndex &&
        left->deviceId == right->deviceId &&
        left->subSystemId == right->subSystemId &&
        left->pciRevisionId == right->pciRevisionId &&
        left->extDeviceId == right->extDeviceId &&
        left->pciDomain == right->pciDomain &&
        left->pciBus == right->pciBus &&
        left->pciDevice == right->pciDevice &&
        left->pciFunction == right->pciFunction &&
        left->family == right->family &&
        strncmp(left->name, right->name, sizeof(left->name)) == 0;
}

static inline bool configured_gpu_selection_equal(
    const ConfiguredGpuSelection* left, const ConfiguredGpuSelection* right) {
    if (!left || !right) return left == right;
    if (left->stableIdentityPresent != right->stableIdentityPresent ||
        left->legacyIndex != right->legacyIndex) return false;
    if (!left->stableIdentityPresent) return true;
    return gpu_adapter_info_equal(&left->identity, &right->identity);
}

static inline bool parse_hex_u32_field(const char** text, unsigned int* out,
                                       unsigned int maxVal) {
    if (!text || !*text || !out) return false;
    const char* p = *text;
    if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')))
        return false;
    unsigned long val = 0;
    int digits = 0;
    while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
        if (++digits > 8) return false;
        unsigned int digit = (*p >= '0' && *p <= '9') ? (unsigned int)(*p - '0') :
                             (*p >= 'a' && *p <= 'f') ? (unsigned int)(*p - 'a' + 10) :
                             (unsigned int)(*p - 'A' + 10);
        val = (val << 4) | digit;
        p++;
    }
    if (val > maxVal) return false;
    *out = (unsigned int)val;
    *text = p;
    return true;
}

static inline bool parse_pci_bdf_string(const char* text, unsigned int* domain,
                                        unsigned int* bus, unsigned int* device,
                                        unsigned int* function) {
    if (!text || !domain || !bus || !device || !function) return false;
    const char* p = text;
    while (*p == ' ' || *p == '\t') p++;
    if (!parse_hex_u32_field(&p, domain, 0xFFFFu) || *p != ':') return false;
    p++;
    if (!parse_hex_u32_field(&p, bus, 0xFFu) || *p != ':') return false;
    p++;
    if (!parse_hex_u32_field(&p, device, 0x1Fu) || *p != '.') return false;
    p++;
    if (!parse_hex_u32_field(&p, function, 0x7u)) return false;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return *p == '\0';
}

