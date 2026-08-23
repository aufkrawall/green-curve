// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Read-only Windows NvAPI probe for the XBAR offset domain.
//
// Family is deliberately NOT a gate here.  The private ClkDomains interface
// predates XBAR by roughly a decade, reports its own schema version in the
// response's version word, and XBAR has been an independently controllable
// domain since Volta — so availability is decided by the driver's reported
// schema (validated inside gpu_backend_xbar.h), never by an architecture
// whitelist.  A pre-Blackwell adapter either answers with a pinned schema
// row or is refused with actionable diagnostics; both are safe because this
// probe only reads.

#ifndef GREEN_CURVE_GPU_CAPABILITY_XBAR_PROBE_H
#define GREEN_CURVE_GPU_CAPABILITY_XBAR_PROBE_H

#include "gpu_backend_xbar.h"

static void probe_xbar_control_surface(GpuCapabilityProbe* probe) {
    if (!probe) return;
    GpuDomainObservation obs = {};
    // Reset previous proof scalars first: hardware_initialize() re-runs this
    // probe on selection change / reconnect / driver swap, so values from a
    // previous surface must never survive into the new session.
    g_app.xbarProbeValid = false;
    g_app.xbarFreqReadbackValid = false;
    g_app.xbarMsvddReadbackValid = false;
    obs.entryPointPresent = g_app.gpuHandle != nullptr;
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
        debug_log("gpu capability probe: xbar schema version word=0x%08X"
                  " layout base=0x%03X stride=0x%03X domain=%u offset=%d kHz"
                  " msvdd=%d uV measured=%u kHz\n",
                  snap.versionWord, snap.entryBase, snap.entryStride,
                  snap.domainIndex, snap.freqOffsetKhz, snap.msvddOffsetUv,
                  snap.measuredKhz);
    } else if (snap.schemaStatus == XBAR_SCHEMA_STATUS_UNKNOWN_VERSION) {
        // The driver speaks a ClkDomains schema this build has not pinned.
        // That is incomplete tooling knowledge, NOT positive evidence that the
        // domain was refused — so classify UNPROBED (no limited-surface
        // warning) and rely on the loud schema diagnostics already logged.
        obs.entryPointPresent = false;
        debug_log("gpu capability probe: xbar ClkDomains schema unknown to this"
                  " build; domain left unprobed until its layout is validated"
                  " and pinned\n");
    } else {
        debug_log("gpu capability probe: xbar ClkDomains read refused or layout"
                  " validation failed\n");
    }
    gpu_capability_set(probe, SERVICE_MUTATION_DOMAIN_XBAR,
                       gpu_capability_classify(&obs));
}
#endif
