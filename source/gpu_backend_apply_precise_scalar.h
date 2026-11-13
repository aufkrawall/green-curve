// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_GPU_BACKEND_APPLY_PRECISE_SCALAR_H
#define GREEN_CURVE_GPU_BACKEND_APPLY_PRECISE_SCALAR_H
#include "power_ratio_control.h"
#include "nvml_exact_clock_control.h"
#include "graphics_offset_control.h"

static bool nvapi_set_power_limit_ratio(TuningPowerRatio, bool, unsigned int);
static PowerRatioSurface power_limit_read_ratio_surface();
static bool nvapi_set_mem_offset_precise(int, bool, gc_u32);

static bool apply_precise_scalar_precheck(const DesiredSettings* d,
    unsigned int* powerDefaultMw, gc_u32* memorySurface, char* why, size_t whySize) {
    if (!desired_scalar_intent_valid(d)) {
        set_message(why, whySize, "Invalid precise scalar intent. Nothing was changed.");
        return false;
    }
    const auto graphics = graphics_offset_control_plan(*d, g_nvml_api, g_app.nvmlDevice);
    if (d->authoring.graphicsPrecise && graphics.status != GRAPHICS_OFFSET_VF_COMPONENT &&
        !graphics_offset_control_ready(graphics)) {
        set_message(why, whySize, "Graphics offset: %s. Nothing was changed.", graphics_offset_control_status_text(graphics.status));
        debug_log("graphics-offset: preflight refused status=%d surface=%u targetHz=%lld\n",
            (int)graphics.status, (unsigned)graphics.surface, desired_graphics_hz(*d));
        return false;
    }
    const gc_u32 previousGraphics = g_app.gpuOffsetOwnedSurface;
    if (d->authoring.graphicsPrecise && previousGraphics && previousGraphics != graphics.surface &&
        (!nvml_exact_clock_surface_available(g_nvml_api, NVML_CLOCK_GRAPHICS, previousGraphics) ||
         !nvml_exact_clock_read(g_nvml_api, g_app.nvmlDevice, NVML_CLOCK_GRAPHICS, previousGraphics).valid)) {
        set_message(why, whySize, "The previously owned graphics control cannot be released. Nothing was changed.");
        debug_log("graphics-offset: handback preflight refused; surface=%u\n", (unsigned)previousGraphics);
        return false;
    }
    if (d->memOffsetPrecise) {
        nvml_read_clock_offsets(why, whySize);
        const gc_u32 surface = g_app.memOffsetSurface;
        const NvmlExactClockRead read = nvml_exact_clock_read(g_nvml_api,
            g_app.nvmlDevice, NVML_CLOCK_MEM, surface);
        if (!g_app.readback.memOffset || !read.valid ||
            (d->memOffsetSurface && d->memOffsetSurface != surface) ||
            !nvml_exact_clock_surface_available(g_nvml_api, NVML_CLOCK_MEM, surface)) {
            set_message(why, whySize, "Exact memory control is unavailable or uses a different surface. Nothing was changed.");
            return false;
        }
        *memorySurface = surface;
    }
    if (d->powerLimitRatio) {
        const PowerRatioControlPlan p = power_ratio_control_plan(desired_power_ratio(*d),
            power_limit_read_ratio_surface(), g_app.powerLimitWriteUncertain);
        if (p.status != POWER_RATIO_READY && p.status != POWER_RATIO_INERT) {
            set_message(why, whySize, "Exact power target: %s. Nothing was changed.", power_ratio_control_status_text(p.status));
            return false;
        }
        *powerDefaultMw = p.defaultMw;
    }
    return true;
}
#endif
