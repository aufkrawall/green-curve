// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "nvml_exact_clock_control.h"
#include "desired_graphics_surface.h"

enum GraphicsOffsetControlStatus {
    GRAPHICS_OFFSET_LEGACY, GRAPHICS_OFFSET_VF_COMPONENT, GRAPHICS_OFFSET_READY,
    GRAPHICS_OFFSET_INERT, GRAPHICS_OFFSET_INVALID, GRAPHICS_OFFSET_UNAVAILABLE,
    GRAPHICS_OFFSET_OFF_GRID, GRAPHICS_OFFSET_OUT_OF_RANGE,
};
struct GraphicsOffsetControlPlan {
    GraphicsOffsetControlStatus status = GRAPHICS_OFFSET_INVALID;
    gc_u32 surface = CLOCK_OFFSET_SURFACE_UNSPECIFIED;
    int targetMHz = 0;
    NvmlExactClockRead before{};
};

static inline bool graphics_offset_control_ready(const GraphicsOffsetControlPlan& p) {
    return p.status == GRAPHICS_OFFSET_READY || p.status == GRAPHICS_OFFSET_INERT;
}

// An explicitly named scalar never changes surfaces. Valid current reads do
// not require reported bounds; absent bounds retain application policy. VF
// component grids and writable membership belong to the exact curve compiler.
static inline GraphicsOffsetControlPlan graphics_offset_control_plan(const DesiredSettings& d,
    const NvmlApi& api, nvmlDevice_t device) {
    GraphicsOffsetControlPlan p;
    if (!desired_authoring_valid(&d)) return p;
    if (!d.authoring.graphicsPrecise) { p.status = GRAPHICS_OFFSET_LEGACY; return p; }
    if (d.authoring.graphicsSurface == DESIRED_GRAPHICS_VF_COMPONENT) {
        p.status = GRAPHICS_OFFSET_VF_COMPONENT; return p;
    }
    p.surface = desired_named_graphics_surface(d);
    if (!p.surface) return p;
    const gc_i64 hz = desired_graphics_hz(d);
    if (hz % 1000000) { p.status = GRAPHICS_OFFSET_OFF_GRID; return p; }
    p.targetMHz = (int)(hz / 1000000);
    p.before = nvml_exact_clock_read(api, device, NVML_CLOCK_GRAPHICS, p.surface);
    if (!p.before.valid || !nvml_exact_clock_surface_available(api, NVML_CLOCK_GRAPHICS, p.surface)) {
        p.status = GRAPHICS_OFFSET_UNAVAILABLE; return p;
    }
    if (p.before.rangeValid && (p.targetMHz < p.before.minimum || p.targetMHz > p.before.maximum)) {
        p.status = GRAPHICS_OFFSET_OUT_OF_RANGE; return p;
    }
    p.status = p.targetMHz == p.before.value ? GRAPHICS_OFFSET_INERT : GRAPHICS_OFFSET_READY;
    return p;
}
