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
    if (!xbar_read_control(getControl, g_app.gpuHandle, &snap)) return false;
    xbar_measure_clock(measure, g_app.gpuHandle, &snap.measuredKhz);
    bool changed = g_app.xbarFreqOffsetKhz != snap.freqOffsetKhz ||
        g_app.xbarMsvddOffsetUv != snap.msvddOffsetUv ||
        g_app.xbarMeasuredClockKhz != snap.measuredKhz;
    g_app.xbarFreqOffsetKhz = snap.freqOffsetKhz;
    g_app.xbarMsvddOffsetUv = snap.msvddOffsetUv;
    g_app.xbarMeasuredClockKhz = snap.measuredKhz;
    if (changed) {
        debug_log("xbar refresh: offset=%d kHz msvdd=%d uV measured=%u kHz\n",
                  snap.freqOffsetKhz, snap.msvddOffsetUv, snap.measuredKhz);
    }
    return true;
}
#endif
