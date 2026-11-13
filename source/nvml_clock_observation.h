// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_NVML_CLOCK_OBSERVATION_H
#define GREEN_CURVE_NVML_CLOCK_OBSERVATION_H
#include "nvml_exact_clock_control.h"

struct NvmlClockObservation {
    NvmlExactClockRead control;
    gc_u32 surface;
};

// Observation can fall back, with provenance. Exact writes never use this
// resolver for verification: they keep the surface selected before mutation.
// A failed range query does not erase an independently valid current control.
static inline NvmlClockObservation nvml_clock_observe(const NvmlApi& a,
    nvmlDevice_t device, unsigned int domain, bool requireRange = false) {
    NvmlClockObservation r{};
    r.surface = CLOCK_OFFSET_SURFACE_P0;
    r.control = nvml_exact_clock_read(a, device, domain, r.surface);
    if (!r.control.valid || (requireRange && !r.control.rangeValid)) {
        r.surface = CLOCK_OFFSET_SURFACE_GLOBAL;
        r.control = nvml_exact_clock_read(a, device, domain, r.surface);
    }
    return r;
}
#endif
