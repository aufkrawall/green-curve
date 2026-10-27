// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#pragma once

#include <stdio.h>
#include <string.h>

// NvAPI and NVML have reported the PCI device ID bare (0x2684) and packed with
// the vendor ID in either word (0x268410DE or 0x10DE2684). Locate the NVIDIA
// vendor word rather than assuming an order; never compare vendor words alone.
static inline bool gpu_pci_extract_nvidia_device_id(unsigned int encoded,
                                                    unsigned int* deviceId) {
    const unsigned int nvidiaVendorId = 0x10DEu;
    if (!encoded || !deviceId) return false;
    if (encoded <= 0xFFFFu) {
        if (encoded == nvidiaVendorId) return false;
        *deviceId = encoded;
        return true;
    }
    const unsigned int low = encoded & 0xFFFFu;
    const unsigned int high = (encoded >> 16) & 0xFFFFu;
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

// Both IDs known and naming the same NVIDIA device, in any reported shape.
static inline bool gpu_pci_nvidia_device_ids_match(unsigned int left,
                                                   unsigned int right) {
    if (!left || !right) return false;
    if (left == right) return true;
    unsigned int leftDevice = 0, rightDevice = 0;
    return gpu_pci_extract_nvidia_device_id(left, &leftDevice) &&
        gpu_pci_extract_nvidia_device_id(right, &rightDevice) &&
        leftDevice == rightDevice;
}

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

static inline bool configured_gpu_location_equal(
    const GpuAdapterInfo* left, const GpuAdapterInfo* right) {
    return left && right && left->pciDomain == right->pciDomain &&
        left->pciBus == right->pciBus && left->pciDevice == right->pciDevice &&
        left->pciFunction == right->pciFunction;
}

static inline const char* configured_gpu_location_role_name(
    GpuLocationRole role) {
    switch (role) {
    case GPU_LOCATION_ROLE_UNRECORDED: return "unrecorded";
    case GPU_LOCATION_ROLE_ADVISORY: return "advisory";
    case GPU_LOCATION_ROLE_REQUIRED: return "required";
    }
    return "invalid";
}

// Reads the persisted spelling of a role, case-insensitively. Empty text is a
// record written before the role existed; any other spelling is malformed and
// returns false rather than silently weakening or strengthening the record.
static inline bool configured_gpu_location_role_parse(const char* text,
                                                      GpuLocationRole* roleOut) {
    if (!text || !roleOut) return false;
    if (!text[0]) {
        *roleOut = GPU_LOCATION_ROLE_UNRECORDED;
        return true;
    }
    const GpuLocationRole written[] = {
        GPU_LOCATION_ROLE_ADVISORY, GPU_LOCATION_ROLE_REQUIRED};
    for (GpuLocationRole role : written) {
        const char* name = configured_gpu_location_role_name(role);
        size_t i = 0;
        for (; name[i] && text[i]; ++i) {
            char c = text[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != name[i]) break;
        }
        if (!name[i] && !text[i]) {
            *roleOut = role;
            return true;
        }
    }
    return false;
}

// The role a selection of `target` records: advisory when no other enumerated
// adapter shares its model identity, required when one does. A target without
// a PCI location records nothing, because there is no location to weigh.
static inline GpuLocationRole configured_gpu_location_role_for_selection(
    const GpuAdapterInfo* target, const GpuAdapterInfo* adapters,
    unsigned int adapterCount) {
    if (!target || !configured_gpu_identity_has_bdf(target))
        return GPU_LOCATION_ROLE_UNRECORDED;
    unsigned int models = 0;
    for (unsigned int i = 0; adapters && i < adapterCount; ++i) {
        if (configured_gpu_base_identity_matches(target, &adapters[i]))
            ++models;
    }
    return models == 1 ? GPU_LOCATION_ROLE_ADVISORY : GPU_LOCATION_ROLE_REQUIRED;
}

// Whether the selected model may be followed away from a stored location that
// no longer holds it. A required location IS the identity, so it never moves.
// A record that predates the role moves only while exactly one adapter is
// present, the same tolerance a legacy ordinal-only selection gets.
static inline bool configured_gpu_location_may_move(
    GpuLocationRole role, unsigned int adapterCount) {
    switch (role) {
    case GPU_LOCATION_ROLE_ADVISORY: return true;
    case GPU_LOCATION_ROLE_UNRECORDED: return adapterCount == 1;
    case GPU_LOCATION_ROLE_REQUIRED: return false;
    }
    return false;
}

// Resolves a durable selection against live enumeration. The model identity
// (device, subsystem, revision, extended device) must always match. A stored
// PCI location picks between identical adapters; when it no longer holds the
// model, the model is followed to its one live adapter only if the location
// role allows it. Everything else fails closed as NOT_FOUND or AMBIGUOUS.
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
    unsigned int modelMatches = 0, modelIndex = 0;
    unsigned int locationMatches = 0, locationIndex = 0;
    for (unsigned int i = 0; adapters && i < adapterCount; ++i) {
        const GpuAdapterInfo* live = &adapters[i];
        if (!configured_gpu_base_identity_matches(
                &configured->identity, live)) continue;
        modelIndex = i;
        ++modelMatches;
        if (expectedHasBdf && configured_gpu_identity_has_bdf(live) &&
            configured_gpu_location_equal(&configured->identity, live)) {
            locationIndex = i;
            ++locationMatches;
        }
    }
    if (!expectedHasBdf) {
        if (modelMatches == 1) {
            if (resolvedIndexOut) *resolvedIndexOut = modelIndex;
            return CONFIGURED_GPU_RESOLVE_STABLE;
        }
        return modelMatches == 0
            ? CONFIGURED_GPU_RESOLVE_NOT_FOUND
            : CONFIGURED_GPU_RESOLVE_AMBIGUOUS;
    }
    if (locationMatches == 1) {
        if (resolvedIndexOut) *resolvedIndexOut = locationIndex;
        return CONFIGURED_GPU_RESOLVE_STABLE;
    }
    if (locationMatches > 1) return CONFIGURED_GPU_RESOLVE_AMBIGUOUS;
    if (modelMatches == 0 ||
        !configured_gpu_location_may_move(configured->locationRole,
            adapterCount)) {
        return CONFIGURED_GPU_RESOLVE_NOT_FOUND;
    }
    if (modelMatches > 1) return CONFIGURED_GPU_RESOLVE_AMBIGUOUS;
    if (resolvedIndexOut) *resolvedIndexOut = modelIndex;
    return CONFIGURED_GPU_RESOLVE_STABLE;
}

// True when a STABLE resolution followed the model away from its stored
// location (or to an adapter whose location could not be read), so the
// record no longer says where the GPU is.
static inline bool configured_gpu_selection_location_moved(
    const ConfiguredGpuSelection* configured, const GpuAdapterInfo* live) {
    return configured && live && configured->stableIdentityPresent &&
        configured_gpu_identity_has_bdf(&configured->identity) &&
        (!configured_gpu_identity_has_bdf(live) ||
         !configured_gpu_location_equal(&configured->identity, live));
}

// One bounded diagnostic line, shared by both platforms: the stored model,
// location and role, then where the live adapters are and whether each carries
// the stored model (first four, then an ellipsis).  Windows and Linux differ
// only in what counts as a known location and a matching model, so they pass
// those rules in; the stored ids are printed as given (zero when unknown).
typedef bool (*GpuDetailLocationKnown)(const GpuAdapterInfo* gpu);
typedef bool (*GpuDetailModelMatches)(const GpuAdapterInfo* stored,
                                      const GpuAdapterInfo* live);

static inline void format_gpu_resolution_detail(
    const GpuAdapterInfo* stored, unsigned int storedDeviceId,
    unsigned int storedSubSystemId, GpuLocationRole role,
    const GpuAdapterInfo* adapters, unsigned int adapterCount,
    GpuDetailLocationKnown locationKnown, GpuDetailModelMatches modelMatches,
    bool uppercaseLocation, char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = 0;
    if (!stored || !locationKnown || !modelMatches) return;
    char location[24] = "unknown";
    if (locationKnown(stored))
        snprintf(location, sizeof(location), uppercaseLocation ? "%04X:%02X:%02X.%u" : "%04x:%02x:%02x.%u",
                 stored->pciDomain, stored->pciBus, stored->pciDevice,
                 stored->pciFunction);
    int written = snprintf(out, outSize,
        "stored=%08X/%08X@%s role=%s adapters=%u live=[", storedDeviceId,
        storedSubSystemId, location, configured_gpu_location_role_name(role),
        adapterCount);
    if (written < 0 || (size_t)written >= outSize) return;
    size_t used = (size_t)written;
    const unsigned int shown = adapterCount < 4u ? adapterCount : 4u;
    for (unsigned int i = 0; adapters && i < shown; ++i) {
        const GpuAdapterInfo* live = &adapters[i];
        const char* kind = modelMatches(stored, live) ? "model" : "other";
        written = locationKnown(live)
            ? snprintf(out + used, outSize - used,
                uppercaseLocation ? "%s%u:%s@%04X:%02X:%02X.%u" : "%s%u:%s@%04x:%02x:%02x.%u", i ? " " : "", i, kind,
                live->pciDomain, live->pciBus, live->pciDevice,
                live->pciFunction)
            : snprintf(out + used, outSize - used, "%s%u:%s@unknown",
                i ? " " : "", i, kind);
        if (written < 0 || (size_t)written >= outSize - used) return;
        used += (size_t)written;
    }
    snprintf(out + used, outSize - used, "%s]",
        adapterCount > shown ? " ..." : "");
}

static inline void format_configured_gpu_resolution_detail(
    const ConfiguredGpuSelection* configured,
    const GpuAdapterInfo* adapters, unsigned int adapterCount,
    char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = 0;
    if (!configured || !configured->stableIdentityPresent) {
        snprintf(out, outSize, "stored=legacy-ordinal:%u adapters=%u",
            configured ? configured->legacyIndex : 0u, adapterCount);
        return;
    }
    const GpuAdapterInfo* stored = &configured->identity;
    format_gpu_resolution_detail(stored, stored->deviceId, stored->subSystemId,
        configured->locationRole, adapters, adapterCount,
        configured_gpu_identity_has_bdf, configured_gpu_base_identity_matches,
        true, out, outSize);
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
    return left->locationRole == right->locationRole &&
        gpu_adapter_info_equal(&left->identity, &right->identity);
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

