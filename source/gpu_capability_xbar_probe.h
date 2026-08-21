// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Read-only Windows NvAPI probe for the Blackwell XBAR offset domain.

#ifndef GREEN_CURVE_GPU_CAPABILITY_XBAR_PROBE_H
#define GREEN_CURVE_GPU_CAPABILITY_XBAR_PROBE_H

#include "gpu_backend_xbar.h"

static void probe_xbar_control_surface(GpuCapabilityProbe* probe) {
    if (!probe) return;
    GpuDomainObservation obs = {};
    obs.entryPointPresent = g_app.gpuFamily == GPU_FAMILY_BLACKWELL &&
        g_app.gpuHandle != nullptr;
    if (!obs.entryPointPresent) {
        gpu_capability_set(probe, SERVICE_MUTATION_DOMAIN_XBAR,
                           gpu_capability_classify(&obs));
        return;
    }
    auto getControl = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_DOMAINS_GET_CONTROL);
    auto measure = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_MEASURE);
    obs.entryPointPresent = getControl != nullptr && measure != nullptr;
    if (!obs.entryPointPresent) {
        gpu_capability_set(probe, SERVICE_MUTATION_DOMAIN_XBAR,
                           gpu_capability_classify(&obs));
        return;
    }
    XbarControlSnapshot snap{};
    obs.readSucceeded = xbar_probe(getControl, measure, g_app.gpuHandle, &snap);
    if (obs.readSucceeded) {
        g_app.xbarProbeValid = true;
        g_app.xbarFreqReadbackValid = true;
        g_app.xbarMsvddReadbackValid = true;
        g_app.xbarFreqOffsetKhz = snap.freqOffsetKhz;
        g_app.xbarMsvddOffsetUv = snap.msvddOffsetUv;
        g_app.xbarMeasuredClockKhz = snap.measuredKhz;
        debug_log("gpu capability probe: xbar layout base=0x%03X stride=0x%03X domain=%u offset=%d kHz msvdd=%d uV measured=%u kHz\n",
                  snap.entryBase, snap.entryStride, snap.domainIndex,
                  snap.freqOffsetKhz, snap.msvddOffsetUv, snap.measuredKhz);
    } else {
        debug_log("gpu capability probe: xbar ClkDomains read refused or layout validation failed\n");
    }
    gpu_capability_set(probe, SERVICE_MUTATION_DOMAIN_XBAR,
                       gpu_capability_classify(&obs));
}
#endif
