// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "graphics_offset_control.h"

static bool nvapi_write_named_gpu_offset(int targetMHz, gc_u32 surface) {
    g_applyScalarShiftEvidence.clear();
    g_app.gpuOffsetUncertainSurface = surface;
    g_app.readback.gpuOffset = false;
    set_last_apply_phase("GPU offset exact NVML write");
    const auto written = nvml_exact_clock_write(g_nvml_api, g_app.nvmlDevice,
        NVML_CLOCK_GRAPHICS, surface, targetMHz);
    if (written.verified) {
        g_app.gpuClockOffsetkHz = targetMHz * 1000;
        g_app.readback.gpuOffset = true;
        g_app.gpuOffsetOwnedSurface = targetMHz ? surface : 0;
        g_app.gpuOffsetUncertainSurface = 0;
    }
    debug_log("graphics-offset: precise surface=%u target=%d attempted=%d write=%d read=%d actual=%d verified=%d uncertain=%u\n",
        (unsigned)surface, targetMHz, written.attempted ? 1 : 0, (int)written.writeStatus,
        (int)written.after.status, written.after.value, written.verified ? 1 : 0,
        (unsigned)g_app.gpuOffsetUncertainSurface);
    return written.verified;
}

static bool nvapi_set_gpu_offset_precise(gc_i64 offsetHz, gc_u32 surface, bool forceWrite) {
    if (offsetHz < -1000000000LL || offsetHz > 1000000000LL) return false;
    DesiredSettings request{}; request.hasGpuOffset = 1;
    request.gpuOffsetMHz = (int)(offsetHz / 1000000);
    request.authoring.graphicsPrecise = 1; request.authoring.graphicsOffsetHz = offsetHz;
    request.authoring.graphicsSurface = surface == CLOCK_OFFSET_SURFACE_P0 ? DESIRED_GRAPHICS_P0
        : surface == CLOCK_OFFSET_SURFACE_GLOBAL ? DESIRED_GRAPHICS_GLOBAL : DESIRED_GRAPHICS_AUTO;
    const auto plan = graphics_offset_control_plan(request, g_nvml_api, g_app.nvmlDevice);
    if (!graphics_offset_control_ready(plan) ||
        (g_app.gpuOffsetUncertainSurface && g_app.gpuOffsetUncertainSurface != surface)) {
        debug_log("graphics-offset: precise preflight refused status=%d surface=%u targetHz=%lld uncertain=%u\n",
            (int)plan.status, (unsigned)surface, offsetHz, (unsigned)g_app.gpuOffsetUncertainSurface);
        return false;
    }
    const gc_u32 previous = g_app.gpuOffsetOwnedSurface;
    if (previous && previous != surface) {
        if (!nvml_exact_clock_surface_available(g_nvml_api, NVML_CLOCK_GRAPHICS, previous) ||
            !nvml_exact_clock_read(g_nvml_api, g_app.nvmlDevice, NVML_CLOCK_GRAPHICS, previous).valid) {
            debug_log("graphics-offset: owned surface unavailable for handback; surface=%u\n", (unsigned)previous);
            return false;
        }
        if (!nvapi_write_named_gpu_offset(0, previous)) return false;
    }
    if (!forceWrite && !g_app.gpuOffsetUncertainSurface && plan.status == GRAPHICS_OFFSET_INERT &&
        (!previous || previous == surface)) {
        g_app.gpuOffsetOwnedSurface = plan.targetMHz ? surface : 0;
        g_app.gpuClockOffsetkHz = plan.targetMHz * 1000; g_app.readback.gpuOffset = true;
        return true;
    }
    return nvapi_write_named_gpu_offset(plan.targetMHz, surface);
}

// Reset/rollback repair the actual named surface before any VF baseline can
// remove its protection. A VF-global backend must not subtract a flatten floor
// as though it were a separate scalar; its next phase resets the curve itself.
static bool nvapi_reset_graphics_scalar_controls() {
    const gc_u32 surface = g_app.gpuOffsetUncertainSurface
        ? g_app.gpuOffsetUncertainSurface : g_app.gpuOffsetOwnedSurface;
    if (surface && !nvapi_set_gpu_offset_precise(0, surface, true)) return false;
    return vf_curve_global_gpu_offset_supported() || nvapi_set_gpu_offset(0, true);
}
