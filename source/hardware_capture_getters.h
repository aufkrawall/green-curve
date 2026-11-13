// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_HARDWARE_CAPTURE_GETTERS_H
#define GREEN_CURVE_HARDWARE_CAPTURE_GETTERS_H
#include "hardware_capture.h"
#include "nvml_clock_observation.h"
#include "vf_curve_units.h"
#include "vf_writable_point_policy.h"

// Only getters are resolved/called here. API setup, backend refresh, cache
// publication, ownership changes and recovery remain outside passive capture.
static inline HardwareCaptureScalar hardware_capture_nvml_clock(const NvmlApi& api,
    nvmlDevice_t device, unsigned int domain) {
    HardwareCaptureScalar result{};
    const auto observed = nvml_clock_observe(api, device, domain);
    const auto& r = observed.control;
    result.surface = observed.surface;
    result.status = (int32_t)r.status; result.rangeStatus = (int32_t)r.rangeStatus;
    result.valid = gc_bool8_from_bool(r.valid); result.rangeValid = gc_bool8_from_bool(r.rangeValid);
    result.writable = gc_bool8_from_bool(nvml_exact_clock_surface_available(api, domain, observed.surface));
    const gc_i64 unit = domain == NVML_CLOCK_MEM ? 500000 : 1000000;
    result.grid = unit;
    if (r.valid) result.value = (gc_i64)r.value * unit;
    if (r.rangeValid) { result.minimum = (gc_i64)r.minimum * unit; result.maximum = (gc_i64)r.maximum * unit; }
    return result;
}

static inline HardwareCapturePower hardware_capture_nvml_power(const NvmlApi& api, nvmlDevice_t device) {
    HardwareCapturePower out{};
    out.currentStatus = out.defaultStatus = out.rangeStatus = NVML_ERROR_NOT_SUPPORTED;
    unsigned int current = 0, baseline = 0, minimum = 0, maximum = 0;
    if (api.getPowerLimit) out.currentStatus = api.getPowerLimit(device, &current);
    if (api.getPowerDefaultLimit) out.defaultStatus = api.getPowerDefaultLimit(device, &baseline);
    if (api.getPowerConstraints) out.rangeStatus = api.getPowerConstraints(device, &minimum, &maximum);
    out.currentValid = gc_bool8_from_bool(out.currentStatus == NVML_SUCCESS && current > 0);
    out.defaultValid = gc_bool8_from_bool(out.defaultStatus == NVML_SUCCESS && baseline > 0);
    out.rangeValid = gc_bool8_from_bool(out.rangeStatus == NVML_SUCCESS && minimum > 0 && minimum <= maximum);
    out.writable = gc_bool8_from_bool(api.setPowerLimit != nullptr); out.gridMw = 1;
    if (out.currentValid) out.currentMw = current;
    if (out.defaultValid) out.defaultMw = baseline;
    if (out.rangeValid) { out.minimumMw = minimum; out.maximumMw = maximum; }
    return out;
}

static inline bool hardware_capture_vf_entry_fits(unsigned int base, unsigned int stride,
    unsigned int field, unsigned int size) {
    if (!stride || base > size || field > size - base || sizeof(int) > size - base - field) return false;
    return stride <= (size - base - field - sizeof(int)) / 127;
}

template <class Query>
static inline bool hardware_capture_read_vf(Query query, void* handle, const VfBackendSpec* backend,
    HardwareCaptureObservation& out, int& driverStatus, const VfFloorTopology* boundFloor = nullptr) {
    using Function = int (*)(void*, void*);
    driverStatus = -100001;
    if (!handle || !backend || !backend->readSupported) return false;
    auto getInfo = (Function)query(backend->getInfoId);
    auto getStatus = (Function)query(backend->getStatusId);
    auto getControl = (Function)query(backend->getControlId);
    if (!getInfo || !getStatus || !getControl) return false;
    const unsigned int sizes[] = {backend->infoBufferSize, backend->statusBufferSize, backend->controlBufferSize};
    for (unsigned int size : sizes) if (size < 36 || size > 0x4000) { driverStatus = -100002; return false; }
    if (backend->infoMaskOffset > sizes[0] - 32 || backend->statusMaskOffset > sizes[1] - 32 ||
        backend->controlMaskOffset > sizes[2] - 32 || backend->statusEntryStride < 8 ||
        backend->controlEntryStride < 4 ||
        !hardware_capture_vf_entry_fits(backend->statusEntriesOffset, backend->statusEntryStride, 4, sizes[1]) ||
        !hardware_capture_vf_entry_fits(backend->controlEntryBaseOffset, backend->controlEntryStride,
            backend->controlEntryDeltaOffset, sizes[2])) { driverStatus = -100002; return false; }
    auto info = std::make_unique<unsigned char[]>(sizes[0]);
    auto status = std::make_unique<unsigned char[]>(sizes[1]);
    auto control = std::make_unique<unsigned char[]>(sizes[2]);
    unsigned char mask[32]{};
    const auto initialize = [](unsigned char* buffer, unsigned int size, unsigned int version) {
        memset(buffer, 0, size); const unsigned int word = (version << 16) | size; memcpy(buffer, &word, 4);
    };
    const auto returned = [](const unsigned char* buffer, unsigned int size, unsigned int version) {
        unsigned int word = 0; memcpy(&word, buffer, 4); return word == ((version << 16) | size);
    };
    initialize(info.get(), sizes[0], backend->infoVersion);
    memset(info.get() + backend->infoMaskOffset, 0xff, sizeof(mask));
    driverStatus = getInfo(handle, info.get());
    if (driverStatus) return false;
    if (!returned(info.get(), sizes[0], backend->infoVersion)) { driverStatus = -100002; return false; }
    memcpy(mask, info.get() + backend->infoMaskOffset, sizeof(mask));
    initialize(status.get(), sizes[1], backend->statusVersion);
    memcpy(status.get() + backend->statusMaskOffset, mask, sizeof(mask));
    driverStatus = getStatus(handle, status.get());
    if (driverStatus) return false;
    if (!returned(status.get(), sizes[1], backend->statusVersion) ||
        memcmp(mask, status.get() + backend->statusMaskOffset, sizeof(mask))) { driverStatus = -100002; return false; }
    initialize(control.get(), sizes[2], backend->controlVersion);
    memcpy(control.get() + backend->controlMaskOffset, mask, sizeof(mask));
    driverStatus = getControl(handle, control.get());
    if (driverStatus) return false;
    if (!returned(control.get(), sizes[2], backend->controlVersion) ||
        memcmp(mask, control.get() + backend->controlMaskOffset, sizeof(mask))) { driverStatus = -100002; return false; }
    VFCurvePoint curve[VF_NUM_POINTS]{};
    int offsets[VF_NUM_POINTS]{};
    gc_i64 exactOffset[VF_NUM_POINTS]{}, exactFrequency[VF_NUM_POINTS]{};
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        const unsigned int entry = backend->statusEntriesOffset + (unsigned int)i * backend->statusEntryStride;
        memcpy(&curve[i].freq_kHz, status.get() + entry, 4); memcpy(&curve[i].volt_uV, status.get() + entry + 4, 4);
    }
    const int end = vf_graphics_domain_length(curve, VF_NUM_POINTS);
    const gc_i64 scale = vf_graphics_frequency_scale(backend);
    int populated = 0;
    for (int i = 0; i < end; ++i) {
        if (!curve[i].freq_kHz) { if (curve[i].volt_uV) { driverStatus = -100002; return false; } continue; }
        if (!curve[i].volt_uV) { driverStatus = -100002; return false; }
        const gc_i64 numerator = (gc_i64)curve[i].freq_kHz * 1000;
        if (numerator % scale) { driverStatus = -100002; return false; }
        exactFrequency[i] = numerator / scale;
        int raw = 0;
        const unsigned int at = backend->controlEntryBaseOffset + (unsigned int)i * backend->controlEntryStride + backend->controlEntryDeltaOffset;
        memcpy(&raw, control.get() + at, 4);
        if (!vf_graphics_offset_hz(backend, raw, &exactOffset[i])) { driverStatus = -100002; return false; }
        offsets[i] = vf_graphics_offset_khz(backend, raw); curve[i].freq_kHz /= (unsigned int)scale; ++populated;
    }
    if (!populated) { driverStatus = -100002; return false; }
    for (int i = end; i < VF_NUM_POINTS; ++i) curve[i] = {};
    const VfFloorTopology floor = boundFloor && boundFloor->valid ? *boundFloor :
        vf_floor_topology_compute(curve, offsets, VF_NUM_POINTS);
    HardwareCapturePoint points[128]{};
    gc_u32 count = 0;
    for (int i = 0; i < end; ++i) if (curve[i].freq_kHz) {
        auto& p = points[count++];
        p.index = (gc_u32)i; p.voltageUv = curve[i].volt_uV;
        p.offsetValid = p.frequencyValid = 1; p.offsetHz = exactOffset[i]; p.frequencyHz = exactFrequency[i];
        const bool writable = (mask[i / 8] & (1u << (i % 8))) &&
            vf_point_target_status(floor, curve, i) == VF_POINT_TARGET_OK;
        p.writableState = writable ? 1 : 2;
    }
    memcpy(out.points, points, sizeof(points)); out.pointCount = count; out.topologyComplete = 1;
    return true;
}

struct HardwareCaptureContext {
    bool usable = false, nvmlUsable = false;
    void* gpuHandle = nullptr;
    void* (*query)(unsigned int) = nullptr;
    const VfBackendSpec* backend = nullptr;
    const NvmlApi* nvml = nullptr;
    nvmlDevice_t nvmlDevice = nullptr;
    HardwareCaptureBinding binding{};
    gc_u64 gpuGeneration = 0;
    gc_u32 osFamily = 0, backendFamily = 0;
    HardwareCaptureComposition composition = CAPTURE_COMPOSITION_UNKNOWN;
    const char* driverVersion = nullptr;
    const DesiredSettings* ownedFanIntent = nullptr;
    // Optional, validated read-only pin getter. No production backend currently
    // supplies one; operating clocks and active intent never substitute for it.
    bool (*pinGetter)(void*, bool&, gc_i64&, gc_i64&, int&) = nullptr;
};

static inline void hardware_capture_owned_fan(const DesiredSettings* intent, HardwareCaptureFan& fan) {
    if (!intent || !intent->hasFan) return;
    fan.valid = fan.curvePresent = 1;
    fan.mode = (gc_u32)intent->fanMode; fan.dutyPct = (gc_u32)intent->fanPercent;
    fan.pollIntervalMs = (gc_u32)intent->fanCurve.pollIntervalMs; fan.hysteresisC = (gc_u32)intent->fanCurve.hysteresisC;
    fan.zeroRpmEnabled = intent->fanCurve.zeroRpmEnabled; fan.zeroRpmHysteresisC = intent->fanCurve.zeroRpmHysteresisC;
    for (int i = 0; i < 8; ++i) {
        fan.points[i].enabled = intent->fanCurve.points[i].enabled;
        fan.points[i].temperatureC = intent->fanCurve.points[i].temperatureC;
        fan.points[i].dutyPct = intent->fanCurve.points[i].fanPercent;
    }
}

template <class ReadAux>
static inline bool hardware_capture_collect(const HardwareCaptureContext& context, gc_u32 requested,
    ReadAux readAux, HardwareCaptureObservation& out, char* why, size_t whySize) {
    if (!context.usable || !context.binding.valid || !context.gpuGeneration || !requested || (requested & ~511u)) {
        if (why && whySize) snprintf(why, whySize, "%s", "passive GPU session is unavailable or needs recovery; capture did not initialize it");
        return false;
    }
    memset(&out, 0, sizeof(out));
    out.valid = 1; out.schemaVersion = 1; out.requestedDomains = requested;
    out.binding = context.binding; out.gpuGeneration = context.gpuGeneration;
    out.osFamily = context.osFamily; out.backendFamily = context.backendFamily; out.composition = context.composition;
    if (context.driverVersion) snprintf(out.driverVersion, sizeof(out.driverVersion), "%s", context.driverVersion);
    for (int i = 0; i < 9; ++i) if (requested & (1u << i)) hardware_capture_domain(out, 1 << i, CAPTURE_READ_FAILED);
    if (requested & (PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_GRAPHICS_OFFSET | PROFILE_DOMAIN_LOCK)) {
        int status = -100001;
        const bool ok = context.query && hardware_capture_read_vf(context.query, context.gpuHandle, context.backend, out, status);
        HardwareCaptureStatus state = ok ? CAPTURE_COMPLETE : status == -100001 ? CAPTURE_UNSUPPORTED : CAPTURE_READ_FAILED;
        if (ok) for (gc_u32 i = 0; i < out.pointCount; ++i)
            if (out.points[i].writableState == 2 && out.points[i].offsetHz) state = CAPTURE_PARTIAL;
        hardware_capture_domain(out, PROFILE_DOMAIN_VF_CURVE, state, status);
    }
    if (context.nvmlUsable && context.nvml && context.nvmlDevice) {
        if (requested & (PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_GRAPHICS_OFFSET))
            out.graphics = hardware_capture_nvml_clock(*context.nvml, context.nvmlDevice, NVML_CLOCK_GRAPHICS);
        if (requested & PROFILE_DOMAIN_MEMORY) out.memory = hardware_capture_nvml_clock(*context.nvml, context.nvmlDevice, NVML_CLOCK_MEM);
        if (requested & PROFILE_DOMAIN_POWER) out.power = hardware_capture_nvml_power(*context.nvml, context.nvmlDevice);
    }
    if (requested & PROFILE_DOMAIN_GRAPHICS_OFFSET) {
        const bool represented = out.graphics.valid && (out.composition == CAPTURE_COMPOSITION_INDEPENDENT ||
            (out.composition == CAPTURE_COMPOSITION_ALIASED && (out.capturedDomains & PROFILE_DOMAIN_VF_CURVE)));
        hardware_capture_domain(out, PROFILE_DOMAIN_GRAPHICS_OFFSET, represented ? CAPTURE_COMPLETE :
            out.graphics.valid ? CAPTURE_COMPOSITION_UNRESOLVED : CAPTURE_READ_FAILED, out.graphics.status);
    }
    if (requested & PROFILE_DOMAIN_MEMORY) hardware_capture_domain(out, PROFILE_DOMAIN_MEMORY,
        out.memory.valid ? CAPTURE_COMPLETE : CAPTURE_READ_FAILED, out.memory.status);
    if (requested & PROFILE_DOMAIN_POWER) hardware_capture_domain(out, PROFILE_DOMAIN_POWER,
        out.power.currentValid && out.power.defaultValid ? CAPTURE_COMPLETE : CAPTURE_READ_FAILED,
        out.power.currentValid ? out.power.defaultStatus : out.power.currentStatus);
    if (requested & PROFILE_DOMAIN_LOCK) {
        bool present = false; gc_i64 minimum = 0, maximum = 0; int status = -100001;
        const bool known = context.pinGetter && context.pinGetter(context.gpuHandle, present, minimum, maximum, status);
        out.pinStatus = status; out.pinGetterValid = gc_bool8_from_bool(known);
        if (known) { out.pinPresent = gc_bool8_from_bool(present); out.pinMinimumHz = minimum; out.pinMaximumHz = maximum; }
        hardware_capture_domain(out, PROFILE_DOMAIN_LOCK, known ? CAPTURE_COMPLETE : context.pinGetter ? CAPTURE_READ_FAILED : CAPTURE_UNSUPPORTED, status);
    }
    if (requested & PROFILE_DOMAIN_FAN) {
        hardware_capture_owned_fan(context.ownedFanIntent, out.fan);
        hardware_capture_domain(out, PROFILE_DOMAIN_FAN, out.fan.valid ? CAPTURE_COMPLETE : CAPTURE_UNSUPPORTED);
    }
    if (requested & (PROFILE_DOMAIN_XBAR | PROFILE_DOMAIN_SYS_CLK | PROFILE_DOMAIN_VIDEO_CLK)) readAux(context, out);
    return true;
}
#endif
