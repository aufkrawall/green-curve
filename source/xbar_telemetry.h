// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Service-side XBAR telemetry refresh.  Kept out of the already-oversized NVML
// runtime shard; the caller has NvAPI initialized and owns hardware locking.
#ifndef GREEN_CURVE_XBAR_TELEMETRY_H
#define GREEN_CURVE_XBAR_TELEMETRY_H

#include "gpu_backend_xbar.h"

// Public clock ids used here: gpu_core.h carries GRAPHICS/MEMORY for the VF
// path; VIDEO is the one this file needs.
enum {
    NVAPI_GPU_PUBLIC_CLOCK_VIDEO = 8,
};

// The video engine has no CLK_MEASURE domain, but the DOCUMENTED public
// frequency query reports its current clock.  This is the proper in-app
// source for the live VIDEO value - no external tool involved.
static bool clk_read_public_video_clock(void* gpuHandle,
                                        unsigned int* khzOut) {
    if (!gpuHandle || !khzOut) return false;
    auto getAllClocks = (NvApiFunc)nvapi_qi(0xDCB616C3u);
    if (!getAllClocks) return false;
    struct {
        unsigned int version;
        unsigned int clockType;  // 0 = CURRENT_FREQ
        struct { unsigned int present; unsigned int frequency; } domain[32];
    } freqs;
    memset(&freqs, 0, sizeof(freqs));
    freqs.version = (3u << 16) | (unsigned int)sizeof(freqs);
    if (getAllClocks(gpuHandle, &freqs) != 0) return false;
    if (!freqs.domain[NVAPI_GPU_PUBLIC_CLOCK_VIDEO].present) return false;
    *khzOut = freqs.domain[NVAPI_GPU_PUBLIC_CLOCK_VIDEO].frequency;
    return *khzOut != 0;
}

static bool xbar_refresh_live_state() {
    if (!g_app.xbarProbeValid || !g_app.gpuHandle) return false;
    auto getControl = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_DOMAINS_GET_CONTROL);
    auto measure = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_MEASURE);
    if (!getControl || !measure) return false;
    XbarControlSnapshot snap{};
    if (!xbar_read_control(getControl, g_app.gpuHandle, &snap)) {
        // Keep the last-known scalar for the editor, but stop claiming proof.
        g_app.xbarFreqReadbackValid = false;
        g_app.xbarMsvddReadbackValid = false;
        g_app.sysClkFreqReadbackValid = false;
        g_app.videoClkFreqReadbackValid = false;
        g_app.videoClkMeasuredClockKhz = 0;
        return false;
    }
    xbar_measure_clock(measure, g_app.gpuHandle, XBAR_MEASURE_DOMAIN_XBAR,
                       &snap.measuredKhz);
    unsigned int sysMeasuredKhz = 0;
    xbar_measure_clock(measure, g_app.gpuHandle, XBAR_MEASURE_DOMAIN_SYS,
                       &sysMeasuredKhz);
    g_app.xbarFreqReadbackValid = true;
    g_app.xbarMsvddReadbackValid = true;
    bool changed = g_app.xbarFreqOffsetKhz != snap.freqOffsetKhz ||
        g_app.xbarMsvddOffsetUv != snap.msvddOffsetUv ||
        g_app.xbarMeasuredClockKhz != snap.measuredKhz;
    g_app.xbarFreqOffsetKhz = snap.freqOffsetKhz;
    g_app.xbarMsvddOffsetUv = snap.msvddOffsetUv;
    g_app.xbarMeasuredClockKhz = snap.measuredKhz;
    // The same validated block carries the SYS entry — refresh it too.
    int sysOffset = 0;
    if (xbar_read_entry_freq(&snap, XBAR_PINNED_SYS_ENTRY_INDEX,
                             &sysOffset)) {
        changed = changed || g_app.sysClkFreqOffsetKhz != sysOffset;
        g_app.sysClkFreqReadbackValid = true;
        g_app.sysClkFreqOffsetKhz = sysOffset;
        changed = changed || g_app.sysClkMeasuredClockKhz != sysMeasuredKhz;
        g_app.sysClkMeasuredClockKhz = sysMeasuredKhz;
    }
    int videoOffset = 0;
    if (xbar_read_entry_freq(&snap, XBAR_PINNED_VIDEO_ENTRY_INDEX,
                             &videoOffset)) {
        changed = changed || g_app.videoClkFreqOffsetKhz != videoOffset;
        g_app.videoClkFreqReadbackValid = true;
        g_app.videoClkFreqOffsetKhz = videoOffset;
    }
    unsigned int videoMeasuredKhz = 0;
    clk_read_public_video_clock(g_app.gpuHandle, &videoMeasuredKhz);
    changed = changed ||
        g_app.videoClkMeasuredClockKhz != videoMeasuredKhz;
    g_app.videoClkMeasuredClockKhz = videoMeasuredKhz;
    if (changed) {
        debug_log("xbar refresh: offset=%d kHz msvdd=%d uV measured=%u kHz\n",
                  snap.freqOffsetKhz, snap.msvddOffsetUv, snap.measuredKhz);
    }
    return true;
}
#endif
