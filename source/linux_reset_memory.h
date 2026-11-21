// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once

#include "linux_backend.h"
#include "nvml_exact_clock_control.h"

// Global first: a driver may update the P0 value as a side effect. The
// final read validates both controls even when they are coupled.
static constexpr gc_u32 LINUX_RESET_MEMORY_SURFACES[] = {
    CLOCK_OFFSET_SURFACE_GLOBAL, CLOCK_OFFSET_SURFACE_P0
};

static inline unsigned int linux_reset_memory_bit(gc_u32 surface) {
    return 1u << surface;
}

static inline int& linux_reset_memory_value(LinuxResetMemoryPreimage& preimage, gc_u32 surface) {
    return surface == CLOCK_OFFSET_SURFACE_GLOBAL ? preimage.globalMHz : preimage.p0MHz;
}

static inline bool linux_capture_reset_memory(LinuxGpuState* g, gc_u32 ownedSurface,
    LinuxResetMemoryPreimage* preimage) {
    *preimage = {};
    for (gc_u32 surface : LINUX_RESET_MEMORY_SURFACES) {
        const auto read = nvml_exact_clock_read(g->nvml, g->nvmlDevice, NVML_CLOCK_MEM, surface);
        if (!read.valid && read.status == NVML_ERROR_NOT_SUPPORTED && surface != ownedSurface)
            continue;
        if (!read.valid || (read.value != 0 &&
            !nvml_exact_clock_surface_available(g->nvml, NVML_CLOCK_MEM, surface))) {
            lb_log("reset memory: preimage unavailable surface=%u status=%d valid=%d\n",
                   surface, (int)read.status, read.valid ? 1 : 0);
            return false;
        }
        preimage->validMask |= linux_reset_memory_bit(surface);
        linux_reset_memory_value(*preimage, surface) = read.value;
    }
    return preimage->validMask != 0;
}

static inline bool linux_reset_memory_matches(LinuxGpuState* g, LinuxResetMemoryPreimage targets) {
    bool ok = true;
    for (gc_u32 surface : LINUX_RESET_MEMORY_SURFACES) {
        if (!(targets.validMask & linux_reset_memory_bit(surface))) continue;
        const auto final = nvml_exact_clock_read(g->nvml, g->nvmlDevice, NVML_CLOCK_MEM, surface);
        const int target = linux_reset_memory_value(targets, surface);
        if (!final.valid || final.value != target) {
            lb_log("reset memory: final mismatch surface=%u target=%d actual=%d status=%d\n",
                   surface, target, final.value, (int)final.status);
            ok = false;
        }
    }
    return ok;
}

static inline bool linux_write_reset_memory(LinuxGpuState* g, LinuxResetMemoryPreimage targets) {
    bool ok = true;
    for (gc_u32 surface : LINUX_RESET_MEMORY_SURFACES) {
        if (!(targets.validMask & linux_reset_memory_bit(surface))) continue;
        const auto current = nvml_exact_clock_read(g->nvml, g->nvmlDevice, NVML_CLOCK_MEM, surface);
        const int target = linux_reset_memory_value(targets, surface);
        if (current.valid && current.value == target) continue;
        const auto write = nvml_exact_clock_write(g->nvml, g->nvmlDevice, NVML_CLOCK_MEM, surface, target);
        ok &= write.verified;
        lb_log("reset memory: surface=%u target=%d write=%d verified=%d\n",
               surface, target, (int)write.writeStatus, write.verified ? 1 : 0);
    }
    return linux_reset_memory_matches(g, targets) && ok;
}
