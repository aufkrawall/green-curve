// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_NVML_EXACT_CLOCK_CONTROL_H
#define GREEN_CURVE_NVML_EXACT_CLOCK_CONTROL_H

#include "gpu_core.h"

struct NvmlExactClockRead {
    bool valid, rangeValid;
    int value, minimum, maximum;
    nvmlReturn_t status, rangeStatus;
};

static inline bool nvml_exact_clock_surface_available(const NvmlApi& a,
    unsigned int domain, gc_u32 surface) {
    if (domain != NVML_CLOCK_MEM && domain != NVML_CLOCK_GRAPHICS) return false;
    if (surface == CLOCK_OFFSET_SURFACE_P0) return a.getClockOffsets && a.setClockOffsets;
    if (surface != CLOCK_OFFSET_SURFACE_GLOBAL) return false;
    return domain == NVML_CLOCK_MEM ? a.getMemClkVfOffset && a.setMemClkVfOffset
        : a.getGpcClkVfOffset && a.setGpcClkVfOffset;
}

// A named observation never falls back to another API. Capture can read a
// surface without having a setter; mutation separately requires both.
static inline NvmlExactClockRead nvml_exact_clock_read(const NvmlApi& a,
    nvmlDevice_t device, unsigned int domain, gc_u32 surface) {
    NvmlExactClockRead r{};
    r.status = NVML_ERROR_NOT_SUPPORTED;
    r.rangeStatus = NVML_ERROR_NOT_SUPPORTED;
    if (domain != NVML_CLOCK_MEM && domain != NVML_CLOCK_GRAPHICS) return r;
    if (surface == CLOCK_OFFSET_SURFACE_P0 && a.getClockOffsets) {
        nvmlClockOffset_t info{};
        info.version = nvmlClockOffset_v1; info.type = domain;
        info.pstate = nvml_configured_clock_offset_pstate();
        r.status = a.getClockOffsets(device, &info);
        r.rangeStatus = r.status;
        r.valid = r.status == NVML_SUCCESS && info.version == nvmlClockOffset_v1 &&
            info.type == domain && info.pstate == nvml_configured_clock_offset_pstate();
        r.value = info.clockOffsetMHz; r.minimum = info.minClockOffsetMHz;
        r.maximum = info.maxClockOffsetMHz;
        r.rangeValid = r.valid && r.minimum <= r.maximum;
    } else if (surface == CLOCK_OFFSET_SURFACE_GLOBAL) {
        auto get = domain == NVML_CLOCK_MEM ? a.getMemClkVfOffset : a.getGpcClkVfOffset;
        auto range = domain == NVML_CLOCK_MEM ? a.getMemClkMinMaxVfOffset : a.getGpcClkMinMaxVfOffset;
        if (get) r.status = get(device, &r.value);
        r.valid = r.status == NVML_SUCCESS;
        if (range) r.rangeStatus = range(device, &r.minimum, &r.maximum);
        r.rangeValid = r.rangeStatus == NVML_SUCCESS && r.minimum <= r.maximum;
    }
    return r;
}

struct NvmlExactClockWrite {
    bool attempted, verified;
    nvmlReturn_t writeStatus;
    NvmlExactClockRead after;
};

static inline NvmlExactClockWrite nvml_exact_clock_write(const NvmlApi& a,
    nvmlDevice_t device, unsigned int domain, gc_u32 surface, int value) {
    NvmlExactClockWrite r{};
    r.writeStatus = NVML_ERROR_NOT_SUPPORTED;
    r.after.status = NVML_ERROR_NOT_SUPPORTED;
    r.after.rangeStatus = NVML_ERROR_NOT_SUPPORTED;
    if (!nvml_exact_clock_surface_available(a, domain, surface)) return r;
    r.attempted = true;
    if (surface == CLOCK_OFFSET_SURFACE_P0) {
        nvmlClockOffset_t info{};
        info.version = nvmlClockOffset_v1; info.type = domain;
        info.pstate = nvml_configured_clock_offset_pstate(); info.clockOffsetMHz = value;
        r.writeStatus = a.setClockOffsets(device, &info);
    } else {
        auto set = domain == NVML_CLOCK_MEM ? a.setMemClkVfOffset : a.setGpcClkVfOffset;
        r.writeStatus = set(device, value);
    }
    // A setter may mutate and then fail. Do not try an alternative interface.
    if (r.writeStatus != NVML_SUCCESS) return r;
    r.after = nvml_exact_clock_read(a, device, domain, surface);
    r.verified = r.after.valid && r.after.value == value;
    return r;
}
#endif
