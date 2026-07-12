// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#pragma once

// Pure helpers shared by the Windows Configuration Manager registration and
// deterministic tests.  PCI hardware IDs are matched before any DEVINST can be
// associated with the selected GPU.  The helpers intentionally reject partial
// identities: an ordinal/name match is not sufficient on a multi-GPU system.

struct SelectedGpuPciHardwareId {
    gc_bool8 valid;
    gc_u32 vendorId;
    gc_u32 deviceId;
    gc_u32 subsystemId;
    gc_u32 revisionId;
};

enum SelectedGpuPnpMatchResolution {
    SELECTED_GPU_PNP_MATCH_NONE = 0,
    SELECTED_GPU_PNP_MATCH_UNIQUE,
    SELECTED_GPU_PNP_MATCH_AMBIGUOUS,
};

static inline SelectedGpuPnpMatchResolution
selected_gpu_pnp_resolve_match_count(size_t matches) {
    if (matches == 0) return SELECTED_GPU_PNP_MATCH_NONE;
    if (matches == 1) return SELECTED_GPU_PNP_MATCH_UNIQUE;
    return SELECTED_GPU_PNP_MATCH_AMBIGUOUS;
}

static inline int selected_gpu_pnp_hex_value(wchar_t ch) {
    if (ch >= L'0' && ch <= L'9') return (int)(ch - L'0');
    if (ch >= L'a' && ch <= L'f') return (int)(ch - L'a') + 10;
    if (ch >= L'A' && ch <= L'F') return (int)(ch - L'A') + 10;
    return -1;
}

static inline wchar_t selected_gpu_pnp_ascii_upper(wchar_t ch) {
    return ch >= L'a' && ch <= L'z' ? (wchar_t)(ch - (L'a' - L'A')) : ch;
}

static inline bool selected_gpu_pnp_ascii_starts_with(
    const wchar_t* text, const wchar_t* expected)
{
    if (!text || !expected) return false;
    while (*expected) {
        if (!*text || selected_gpu_pnp_ascii_upper(*text) !=
                selected_gpu_pnp_ascii_upper(*expected)) return false;
        ++text;
        ++expected;
    }
    return true;
}

static inline const wchar_t* selected_gpu_pnp_find_ascii_token(
    const wchar_t* text, const wchar_t* token)
{
    if (!text || !token || !*token) return nullptr;
    for (const wchar_t* cursor = text; *cursor; ++cursor) {
        if (selected_gpu_pnp_ascii_starts_with(cursor, token)) return cursor;
    }
    return nullptr;
}

static inline bool selected_gpu_pnp_parse_fixed_hex_token(
    const wchar_t* text, const wchar_t* token, unsigned int digits,
    gc_u32* valueOut)
{
    if (!valueOut || digits == 0 || digits > 8) return false;
    const wchar_t* found = selected_gpu_pnp_find_ascii_token(text, token);
    if (!found) return false;
    while (*token) {
        ++found;
        ++token;
    }
    gc_u32 value = 0;
    for (unsigned int i = 0; i < digits; ++i) {
        int nibble = selected_gpu_pnp_hex_value(found[i]);
        if (nibble < 0) return false;
        value = (value << 4) | (gc_u32)nibble;
    }
    // Reject a longer hexadecimal value rather than silently truncating it.
    if (selected_gpu_pnp_hex_value(found[digits]) >= 0) return false;
    *valueOut = value;
    return true;
}

static inline bool selected_gpu_pnp_parse_hardware_id(
    const wchar_t* hardwareId, SelectedGpuPciHardwareId* out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!hardwareId || !selected_gpu_pnp_ascii_starts_with(hardwareId, L"PCI\\")) {
        return false;
    }
    gc_u32 vendor = 0;
    gc_u32 device = 0;
    gc_u32 subsystem = 0;
    gc_u32 revision = 0;
    if (!selected_gpu_pnp_parse_fixed_hex_token(hardwareId, L"VEN_", 4, &vendor) ||
        !selected_gpu_pnp_parse_fixed_hex_token(hardwareId, L"DEV_", 4, &device) ||
        !selected_gpu_pnp_parse_fixed_hex_token(hardwareId, L"SUBSYS_", 8, &subsystem) ||
        !selected_gpu_pnp_parse_fixed_hex_token(hardwareId, L"REV_", 2, &revision)) {
        return false;
    }
    out->valid = 1;
    out->vendorId = vendor;
    out->deviceId = device;
    out->subsystemId = subsystem;
    out->revisionId = revision;
    return true;
}

static inline bool selected_gpu_pnp_device_id_matches(
    gc_u32 nvapiDeviceId, gc_u32 vendorId, gc_u32 deviceId)
{
    if (!nvapiDeviceId || !vendorId || !deviceId) return false;
    gc_u32 low = nvapiDeviceId & 0xFFFFu;
    gc_u32 high = (nvapiDeviceId >> 16) & 0xFFFFu;
    // NvAPI has returned both the combined DEV/VEN representation and a bare
    // 16-bit device ID across driver generations.  Accept either byte-order of
    // the combined value, but never relax the vendor match.
    if (low == vendorId && high == deviceId) return true;
    if (high == vendorId && low == deviceId) return true;
    return high == 0 && vendorId == 0x10DEu && low == deviceId;
}

static inline bool selected_gpu_pnp_hardware_id_matches_target(
    const GpuAdapterInfo* target, const SelectedGpuPciHardwareId* hardware)
{
    if (!target || !hardware || !target->valid || !target->pciInfoValid ||
        !hardware->valid || target->subSystemId == 0 ||
        target->subSystemId <= 0xFFFFu) return false;
    if (!selected_gpu_pnp_device_id_matches(target->deviceId,
            hardware->vendorId, hardware->deviceId)) return false;
    if (target->subSystemId != hardware->subsystemId) return false;
    return (target->pciRevisionId & 0xFFu) == hardware->revisionId;
}

static inline bool selected_gpu_pnp_hardware_id_list_matches_target(
    const GpuAdapterInfo* target, const wchar_t* hardwareIds,
    size_t hardwareIdChars)
{
    if (!target || !hardwareIds || hardwareIdChars < 2) return false;
    size_t offset = 0;
    while (offset < hardwareIdChars && hardwareIds[offset]) {
        size_t length = 0;
        while (offset + length < hardwareIdChars && hardwareIds[offset + length]) {
            ++length;
        }
        if (offset + length >= hardwareIdChars) return false;
        SelectedGpuPciHardwareId parsed = {};
        if (selected_gpu_pnp_parse_hardware_id(hardwareIds + offset, &parsed) &&
            selected_gpu_pnp_hardware_id_matches_target(target, &parsed)) {
            return true;
        }
        offset += length + 1;
    }
    return false;
}

static inline bool selected_gpu_pnp_target_has_bdf(const GpuAdapterInfo* target) {
    if (!target || !target->valid || target->pciDomain != 0 ||
        target->pciBus > 255u || target->pciDevice > 31u ||
        target->pciFunction > 7u) return false;
    // GpuAdapterInfo predates an explicit BDF-valid bit.  NVML populates these
    // fields; all-zero therefore remains "unknown" (and is resolved only by a
    // unique full hardware-ID match) rather than assuming 0000:00:00.0.
    return target->pciBus != 0 || target->pciDevice != 0 ||
           target->pciFunction != 0;
}

static inline bool selected_gpu_pnp_bdf_matches_target(
    const GpuAdapterInfo* target, bool candidateBdfValid,
    gc_u32 bus, gc_u32 device, gc_u32 function)
{
    if (!target) return false;
    if (!selected_gpu_pnp_target_has_bdf(target)) return true;
    return candidateBdfValid && target->pciBus == bus &&
           target->pciDevice == device && target->pciFunction == function;
}
