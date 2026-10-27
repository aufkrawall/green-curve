// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_GPU_BACKEND_MEMORY_OFFSET_H
#define GREEN_CURVE_GPU_BACKEND_MEMORY_OFFSET_H

// Included at the backend's original definition site. Recovery can force a
// write even when cached stock is the last value from before a failed setter.
static bool nvapi_set_mem_offset(int offsetkHz, bool forceWrite) {
    if (!forceWrite && !g_app.memOffsetWriteUncertain && g_app.readback.memOffset &&
        g_app.memClockOffsetkHz == offsetkHz) return true;
    int nvmlValueMHz = (offsetkHz / 1000) * 2;
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
    detect_clock_offsets();
    const bool verified = exact || freshMemMatch ||
        (freshPstate && g_app.nvmlMemMaxMHz > 0 && g_app.memClockOffsetkHz == offsetkHz);
    if (verified) {
        g_app.memClockOffsetkHz = offsetkHz;
        g_app.readback.memOffset = true;
        g_app.memOffsetWriteUncertain = false;
    } else {
        g_app.readback.memOffset = false;
    }
    debug_log("nvapi_set_mem_offset: readback_driver_kHz=%d verified=%d nvmlMatch=%d freshMemMatch=%d freshPstate=%d uncertain=%d\n",
        g_app.memClockOffsetkHz, verified ? 1 : 0, exact ? 1 : 0,
        freshMemMatch ? 1 : 0, freshPstate ? 1 : 0, g_app.memOffsetWriteUncertain ? 1 : 0);
    return verified;
}
#endif
