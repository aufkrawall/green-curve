// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Service-side XBAR telemetry refresh.  Kept out of the already-oversized NVML
// runtime shard; the caller has NvAPI initialized and owns hardware locking.
#ifndef GREEN_CURVE_XBAR_TELEMETRY_H
#define GREEN_CURVE_XBAR_TELEMETRY_H

#include "gpu_backend_xbar.h"

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
    unsigned long long sysField =
        (unsigned long long)snap.entryBase +
        XBAR_PINNED_SYS_ENTRY_INDEX * snap.entryStride +
        g_xbarSchemas[0].freqOffsetField;
    if (sysField + sizeof(unsigned int) <= XBAR_CONTROL_BUF_SIZE) {
        int sysOffset = (int)xbar_get_u32(snap.buf, (unsigned int)sysField);
        changed = changed || g_app.sysClkFreqOffsetKhz != sysOffset;
        g_app.sysClkFreqReadbackValid = true;
        g_app.sysClkFreqOffsetKhz = sysOffset;
        changed = changed || g_app.sysClkMeasuredClockKhz != sysMeasuredKhz;
        g_app.sysClkMeasuredClockKhz = sysMeasuredKhz;
    }
    const int videoEntry = XBAR_PINNED_VIDEO_ENTRY_INDEX;
    if (videoEntry >= 0) {
        unsigned long long videoField =
            (unsigned long long)snap.entryBase +
            (unsigned long long)videoEntry * snap.entryStride +
            g_xbarSchemas[0].freqOffsetField;
        if (videoField + sizeof(unsigned int) <= XBAR_CONTROL_BUF_SIZE) {
            int videoOffset = (int)xbar_get_u32(snap.buf, (unsigned int)videoField);
            changed = changed || g_app.videoClkFreqOffsetKhz != videoOffset;
            g_app.videoClkFreqReadbackValid = true;
            g_app.videoClkFreqOffsetKhz = videoOffset;
        }
    }
    if (changed) {
        debug_log("xbar refresh: offset=%d kHz msvdd=%d uV measured=%u kHz\n",
                  snap.freqOffsetKhz, snap.msvddOffsetUv, snap.measuredKhz);
    }
    return true;
}
#endif
