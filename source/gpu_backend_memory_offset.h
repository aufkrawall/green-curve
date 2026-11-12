// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_GPU_BACKEND_MEMORY_OFFSET_H
#define GREEN_CURVE_GPU_BACKEND_MEMORY_OFFSET_H
#include "memory_offset_units.h"

// Included at the backend's original definition site. Recovery can force a
// write even when cached stock is the last value from before a failed setter.
static bool nvapi_set_mem_offset_impl(int offsetkHz, bool forceWrite, bool strictReadback) {
    int nvmlValueMHz = 0;
    if (!memory_offset_effective_mhz_from_khz(offsetkHz, &nvmlValueMHz)) {
        debug_log("memory offset: %d physical kHz is not representable by the NVML surface\n", offsetkHz);
        return false;
    }
    strictReadback = strictReadback || offsetkHz % 1000 != 0;
    if (strictReadback) {
        char readDetail[128] = {};
        g_app.readback.memOffset = false;
        nvml_read_clock_offsets(readDetail, sizeof(readDetail));
        if (!g_app.readback.memOffset) {
            debug_log("memory offset: exact programmed-control readback unavailable; no write\n");
            return false;
        }
    }
    if (!forceWrite && !g_app.memOffsetWriteUncertain && g_app.readback.memOffset &&
        g_app.memClockOffsetkHz == offsetkHz) return true;
    debug_log("nvapi_set_mem_offset: target_driver_kHz=%d nvml_value_MHz=%d current_driver_kHz=%d force=%d\n",
        offsetkHz, nvmlValueMHz, g_app.memClockOffsetkHz, forceWrite ? 1 : 0);
    bool exact = false;
    char detail[128] = {};
    g_app.memOffsetWriteUncertain = true;
    g_app.readback.memOffset = false;
    set_last_apply_phase("Memory offset NVML write");
    bool ok = nvml_set_clock_offset_domain(NVML_CLOCK_MEM, nvmlValueMHz, &exact, detail, sizeof(detail));
    if (!ok) {
        debug_log("nvapi_set_mem_offset: write outcome uncertain (nvml=%d MHz) detail=%s\n",
            nvmlValueMHz, detail[0] ? detail : "unknown");
        return false;
    }
    nvml_read_clock_offsets(detail, sizeof(detail));
    const bool freshMemMatch = g_app.readback.memOffset &&
        g_app.memClockOffsetkHz == offsetkHz;
    const bool freshPstate = nvapi_read_pstates();
    // Pstates may answer with graphics clocks only. Cached memory equality
    // is not evidence unless this response supplied a usable memory clock.
    const bool freshPstateMem = freshPstate && g_app.pstateMemMaxMHz > 0;
    detect_clock_offsets();
    const bool verified = freshMemMatch || (!strictReadback && (exact ||
        (freshPstateMem && g_app.nvmlMemMaxMHz > 0 && g_app.memClockOffsetkHz == offsetkHz)));
    if (verified) {
        g_app.memClockOffsetkHz = offsetkHz;
        g_app.readback.memOffset = true;
        g_app.memOffsetWriteUncertain = false;
    } else {
        g_app.readback.memOffset = false;
    }
    debug_log("nvapi_set_mem_offset: readback_driver_kHz=%d verified=%d nvmlMatch=%d freshMemMatch=%d freshPstate=%d pstateMemMHz=%u maxMemMHz=%u uncertain=%d\n",
        g_app.memClockOffsetkHz, verified ? 1 : 0, exact ? 1 : 0,
        freshMemMatch ? 1 : 0, freshPstate ? 1 : 0, g_app.pstateMemMaxMHz,
        g_app.nvmlMemMaxMHz, g_app.memOffsetWriteUncertain ? 1 : 0);
    return verified;
}

static bool nvapi_set_mem_offset(int offsetkHz, bool forceWrite) {
    return nvapi_set_mem_offset_impl(offsetkHz, forceWrite, false);
}

static bool nvapi_set_mem_offset_precise(int offsetkHz, bool forceWrite) {
    return nvapi_set_mem_offset_impl(offsetkHz, forceWrite, true);
}
#endif
