// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included by linux_backend.cpp; do not compile separately.

// Linux loads the same NvAPI query interface as Windows.  Keep one shared
// schema/transaction implementation and route its diagnostics to the daemon
// logger.
#define XBAR_LOG lb_log
#include "gpu_backend_xbar.h"
#undef XBAR_LOG

namespace {

struct LinuxXbarApi {
    NvApiFunc getControl;
    NvApiFunc setControl;
    NvApiFunc measure;
    NvApiFunc getAllClocks;
};

static LinuxXbarApi linux_xbar_api(const LinuxGpuState* g) {
    LinuxXbarApi api = {};
    if (!g || !g->nvapiQi) return api;
    api.getControl = (NvApiFunc)g->nvapiQi(
        XBAR_NVAPI_CLK_DOMAINS_GET_CONTROL);
    api.setControl = (NvApiFunc)g->nvapiQi(
        XBAR_NVAPI_CLK_DOMAINS_SET_CONTROL);
    api.measure = (NvApiFunc)g->nvapiQi(XBAR_NVAPI_CLK_MEASURE);
    api.getAllClocks = (NvApiFunc)g->nvapiQi(0xDCB616C3u);
    return api;
}

static bool linux_xbar_public_video_clock(NvApiFunc getAllClocks,
                                           void* gpuHandle,
                                           unsigned int* khzOut) {
    if (!getAllClocks || !gpuHandle || !khzOut) return false;
    enum { PUBLIC_CLOCK_VIDEO = 8 };
    struct {
        unsigned int version;
        unsigned int clockType;
        struct { unsigned int present; unsigned int frequency; } domain[32];
    } freqs = {};
    freqs.version = (3u << 16) | (unsigned int)sizeof(freqs);
    if (getAllClocks(gpuHandle, &freqs) != 0 ||
        !freqs.domain[PUBLIC_CLOCK_VIDEO].present) return false;
    *khzOut = freqs.domain[PUBLIC_CLOCK_VIDEO].frequency;
    return *khzOut != 0;
}

static void linux_xbar_clear_readback(LinuxGpuState* g) {
    if (!g) return;
    g->xbarProbeValid = false;
    g->xbarSchemaStatus = XBAR_SCHEMA_STATUS_UNAVAILABLE;
    g->xbarFreqReadbackValid = false;
    g->xbarMsvddReadbackValid = false;
    g->sysClkProbeValid = false;
    g->sysClkFreqReadbackValid = false;
    g->videoClkProbeValid = false;
    g->videoClkFreqReadbackValid = false;
    g->xbarMeasuredClockKhz = 0;
    g->sysClkMeasuredClockKhz = 0;
    g->videoClkMeasuredClockKhz = 0;
}

static bool linux_xbar_refresh(LinuxGpuState* g) {
    linux_xbar_clear_readback(g);
    LinuxXbarApi api = linux_xbar_api(g);
    if (!g || !g->gpuHandle || !api.getControl) return false;
    XbarControlSnapshot* snap =
        (XbarControlSnapshot*)calloc(1, sizeof(XbarControlSnapshot));
    if (!snap) return false;
    bool ok = xbar_read_control(api.getControl, g->gpuHandle, snap);
    g->xbarSchemaStatus = snap->schemaStatus;
    if (ok) {
        g->xbarProbeValid = true;
        g->xbarFreqReadbackValid = true;
        g->xbarMsvddReadbackValid = true;
        g->xbarFreqOffsetKhz = snap->freqOffsetKhz;
        g->xbarMsvddOffsetUv = snap->msvddOffsetUv;
        xbar_measure_clock(api.measure, g->gpuHandle,
                           XBAR_MEASURE_DOMAIN_XBAR,
                           &g->xbarMeasuredClockKhz);

        int value = 0;
        if (xbar_read_entry_freq(snap, XBAR_PINNED_SYS_ENTRY_INDEX, &value)) {
            g->sysClkProbeValid = true;
            g->sysClkFreqReadbackValid = true;
            g->sysClkFreqOffsetKhz = value;
            xbar_measure_clock(api.measure, g->gpuHandle,
                               XBAR_MEASURE_DOMAIN_SYS,
                               &g->sysClkMeasuredClockKhz);
        }
        if (xbar_read_entry_freq(snap, XBAR_PINNED_VIDEO_ENTRY_INDEX,
                                 &value)) {
            g->videoClkProbeValid = true;
            g->videoClkFreqReadbackValid = true;
            g->videoClkFreqOffsetKhz = value;
            linux_xbar_public_video_clock(api.getAllClocks, g->gpuHandle,
                                          &g->videoClkMeasuredClockKhz);
        }
    }
    free(snap);
    return ok;
}

static bool linux_xbar_write_owned(LinuxGpuState* g, int freqKhz,
                                   int msvddUv, bool writeFreq,
                                   bool writeMsvdd) {
    LinuxXbarApi api = linux_xbar_api(g);
    if (!g || !api.getControl || !api.setControl) return false;
    XbarControlSnapshot* snap =
        (XbarControlSnapshot*)calloc(1, sizeof(XbarControlSnapshot));
    if (!snap) return false;
    bool ok = xbar_write(api.getControl, api.setControl, api.measure,
                         g->gpuHandle, snap, freqKhz, msvddUv,
                         writeFreq, writeMsvdd);
    free(snap);
    linux_xbar_refresh(g);
    return ok;
}

static bool linux_xbar_write_entry(LinuxGpuState* g,
                                   unsigned int entryIndex,
                                   int freqKhz) {
    LinuxXbarApi api = linux_xbar_api(g);
    if (!g || !api.getControl || !api.setControl) return false;
    XbarControlSnapshot* snap =
        (XbarControlSnapshot*)calloc(1, sizeof(XbarControlSnapshot));
    if (!snap) return false;
    bool ok = xbar_write_entry_freq(api.getControl, api.setControl,
                                    g->gpuHandle, snap, entryIndex, freqKhz);
    free(snap);
    linux_xbar_refresh(g);
    return ok;
}

}  // namespace
