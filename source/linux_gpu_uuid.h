// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_LINUX_GPU_UUID_H
#define GREEN_CURVE_LINUX_GPU_UUID_H

#include "linux_gpu_selection.h"

// Optional physical-device identity. Kept outside GpuAdapterInfo so adding it
// does not change the client wire or any frozen record embedding that struct.
struct LinuxGpuUuid { char text[96]; };

static inline bool linux_gpu_uuid_valid(const LinuxGpuUuid* uuid) {
    if (!uuid || memcmp(uuid->text, "GPU-", 4) != 0) return false;
    for (unsigned int i = 4; i < 40; ++i) {
        char c = uuid->text[i];
        if (i == 12 || i == 17 || i == 22 || i == 27) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return uuid->text[40] == 0;
}

static inline LinuxGpuUuid linux_gpu_uuid_parse(const char* text) {
    LinuxGpuUuid uuid = {};
    if (!text) return uuid;
    for (unsigned int i = 0; i < sizeof(uuid.text) - 1 && text[i]; ++i) {
        char c = text[i];
        uuid.text[i] = i >= 4 && c >= 'A' && c <= 'F' ? (char)(c + 'a' - 'A') : c;
    }
    return linux_gpu_uuid_valid(&uuid) ? uuid : LinuxGpuUuid{};
}

using LinuxGetGpuUuid = nvmlReturn_t (*)(nvmlDevice_t, char*, unsigned int);
static inline bool linux_gpu_uuid_compatible(const LinuxGpuUuid* stored,
                                              const LinuxGpuUuid* observed) {
    return !linux_gpu_uuid_valid(stored) || !linux_gpu_uuid_valid(observed) ||
        strcmp(stored->text, observed->text) == 0;
}

// Startup policy binding may name an adapter other than the telemetry GPU.
// Never attach the selected adapter's UUID to a different requested target.
static inline const LinuxGpuUuid* linux_gpu_uuid_for_target(
    const GpuAdapterInfo* target, const GpuAdapterInfo* adapters,
    unsigned int count, const LinuxGpuUuid* uuids) {
    const LinuxGpuUuid* found = nullptr;
    for (unsigned int i = 0; adapters && uuids && i < count && i < MAX_GPU_ADAPTERS; ++i) {
        if (!linux_gpu_identity_matches(target, &adapters[i])) continue;
        if (found) return nullptr;
        found = &uuids[i];
    }
    return found;
}
static inline LinuxGpuUuid linux_gpu_read_uuid(LinuxGetGpuUuid query,
                                               nvmlDevice_t device) {
    char text[sizeof(LinuxGpuUuid::text)] = {};
    if (!query || query(device, text, sizeof(text)) != NVML_SUCCESS) return {};
    // A successful driver response must terminate inside its declared buffer.
    if (!memchr(text, 0, sizeof(text))) return {};
    return linux_gpu_uuid_parse(text);
}

enum LinuxGpuUuidVerdict {
    LINUX_GPU_UUID_FALLBACK, LINUX_GPU_UUID_MATCHED,
    LINUX_GPU_UUID_MISMATCH, LINUX_GPU_UUID_AMBIGUOUS,
};
static inline const char* linux_gpu_uuid_verdict_name(LinuxGpuUuidVerdict verdict) {
    switch (verdict) {
        case LINUX_GPU_UUID_MATCHED: return "matched";
        case LINUX_GPU_UUID_MISMATCH: return "mismatch";
        case LINUX_GPU_UUID_AMBIGUOUS: return "ambiguous";
        default: return "fallback";
    }
}

// No new prerequisite: absent/unreadable UUIDs preserve the previous resolver.
// Positive evidence overrides PCI location, including a required location:
// the physical GPU can move, but an identical replacement is a different GPU.
static inline LinuxGpuInitSelection linux_gpu_restore_selection(
    const GpuAdapterInfo* target, GpuLocationRole role,
    const GpuAdapterInfo* adapters, unsigned int count,
    const LinuxGpuUuid* stored, const LinuxGpuUuid* live,
    LinuxGpuUuidVerdict* verdict) {
    if (verdict) *verdict = LINUX_GPU_UUID_FALLBACK;
    LinuxGpuInitSelection fallback = linux_gpu_init_selection(target, adapters, count, role);
    if (!linux_gpu_uuid_valid(stored) || !live || !target || !target->valid || !adapters)
        return fallback;
    int match = -1;
    for (unsigned int i = 0; i < count && i < MAX_GPU_ADAPTERS; ++i) {
        if (!adapters[i].valid || !linux_gpu_uuid_valid(&live[i]) ||
            strcmp(stored->text, live[i].text) != 0) continue;
        if (match >= 0) {
            if (verdict) *verdict = LINUX_GPU_UUID_AMBIGUOUS;
            return {-2, false, false};
        }
        match = (int)i;
    }
    if (match >= 0 && (!linux_gpu_model_known(target) ||
        linux_gpu_model_matches(target, &adapters[match]))) {
        if (verdict) *verdict = LINUX_GPU_UUID_MATCHED;
        return {match, true, !linux_gpu_bdf_equal(target, &adapters[match])};
    }
    if (match >= 0 || (fallback.index >= 0 &&
        linux_gpu_uuid_valid(&live[fallback.index]))) {
        if (verdict) *verdict = LINUX_GPU_UUID_MISMATCH;
        return {-1, false, false};
    }
    return fallback;
}

// A transient query failure must not erase a previously learned binding.
static inline LinuxGpuUuid linux_gpu_uuid_retain(const LinuxGpuUuid* observed,
                                                 const LinuxGpuUuid* previous) {
    if (observed && linux_gpu_uuid_valid(observed)) return *observed;
    if (previous && linux_gpu_uuid_valid(previous)) return *previous;
    return {};
}
#endif
