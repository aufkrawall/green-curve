// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_HARDWARE_CAPTURE_RUNTIME_H
#define GREEN_CURVE_HARDWARE_CAPTURE_RUNTIME_H
#include "hardware_capture_getters.h"
#include "gpu_selection_policy.h"

static inline HardwareCaptureBinding hardware_capture_binding(const GpuAdapterInfo& gpu) {
    HardwareCaptureBinding b{};
    b.valid = gpu.valid; b.pciInfoValid = gpu.pciInfoValid;
    b.deviceId = gpu.deviceId; b.subSystemId = gpu.subSystemId;
    b.pciDomain = gpu.pciDomain; b.pciBus = gpu.pciBus; b.pciDevice = gpu.pciDevice; b.pciFunction = gpu.pciFunction;
    b.nvapiIndex = gpu.nvapiIndex; b.nvmlIndex = gpu.nvmlIndex;
    return b;
}
static inline bool hardware_capture_target_matches(const GpuAdapterInfo& requested, const GpuAdapterInfo& selected) {
    if (!selected.valid) return false;
    if (!requested.valid) return true; // Current bound session only; never select another GPU.
    return requested.pciInfoValid && selected.pciInfoValid ?
        configured_gpu_base_identity_matches(&requested, &selected) && configured_gpu_location_equal(&requested, &selected) :
        gpu_adapter_info_equal(&requested, &selected);
}
static inline void hardware_capture_summary(const HardwareCaptureObservation& o, char* text, size_t size) {
    if (!text || !size) return;
    snprintf(text, size, "capture: requested=0x%03x captured=0x%03x omitted=0x%03x unavailable=0x%03x points=%u attempts=%u composition=%u pin=%s",
        (unsigned)o.requestedDomains, (unsigned)o.capturedDomains, (unsigned)o.omittedDomains,
        (unsigned)o.unavailableDomains, (unsigned)o.pointCount, (unsigned)o.readAttempts, (unsigned)o.composition,
        !o.pinGetterValid ? "unknown" : o.pinPresent ? "present" : "absent");
    const size_t used = strlen(text);
    if (o.vfLimits.valid && used < size) snprintf(text + used, size - used,
        "; VF offsets=%lld..%lld Hz (%s), grid=%lld Hz%s",
        o.vfLimits.minimumHz, o.vfLimits.maximumHz,
        o.vfLimits.rangeReported ? "reported graphics bounds" : "application policy",
        o.vfLimits.gridHz, o.vfLimits.gridValid ? "" : " (unknown)");
}
#endif
