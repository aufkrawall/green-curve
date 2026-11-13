// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_HARDWARE_CAPTURE_WIRE_VALIDATION_H
#define GREEN_CURVE_HARDWARE_CAPTURE_WIRE_VALIDATION_H
#include "hardware_capture_types.h"
#include "profile_text_validation.h"
#include <limits.h>
// gpu_core.h includes the protocol before completing its declarations.
bool fan_curve_validate(const FanCurveConfig* config, char* error, size_t size);

static inline bool hardware_capture_bytes_zero(const void* value, size_t size) {
    const auto* bytes = (const unsigned char*)value;
    for (size_t i = 0; i < size; ++i) if (bytes[i]) return false;
    return true;
}
static inline bool hardware_capture_scalar_valid(const HardwareCaptureScalar& s) {
    return s.valid <= 1 && s.rangeValid <= 1 && s.writable <= 1 && !s.reserved && s.grid >= 0 &&
        (s.valid || !s.value) && (s.rangeValid ? s.minimum <= s.maximum : (!s.minimum && !s.maximum));
}
static inline bool hardware_capture_observation_valid(const HardwareCaptureObservation& o) {
    if (o.valid > 1) return false;
    if (!o.valid) return hardware_capture_bytes_zero(&o, sizeof(o));
    if (o.schemaVersion != 1 || o.topologyComplete > 1 || o.pinGetterValid > 1 || o.pinPresent > 1 ||
        !o.requestedDomains || (o.requestedDomains & ~511u) || o.pointCount > 128 || !o.gpuGeneration ||
        !hardware_capture_bytes_zero(o.reserved, sizeof(o.reserved)) ||
        !hardware_capture_bytes_zero(o.textReserved, sizeof(o.textReserved)) ||
        o.osFamily > 2u || o.backendFamily > INT_MAX ||
        o.composition > CAPTURE_COMPOSITION_ALIASED || o.alignmentReserved ||
        !profile_bounded_text_valid(o.driverVersion, sizeof(o.driverVersion), 64)) return false;
    if (!o.binding.valid || o.binding.valid > 1 || o.binding.pciInfoValid > 1 ||
        !hardware_capture_bytes_zero(o.binding.reserved, sizeof(o.binding.reserved)) ||
        o.binding.nvapiIndex >= MAX_GPU_ADAPTERS || o.binding.nvmlIndex >= MAX_GPU_ADAPTERS ||
        o.binding.pciDomain > 65535 || o.binding.pciBus > 255 || o.binding.pciDevice > 31 || o.binding.pciFunction > 7) return false;
    gc_u32 captured = 0, omitted = 0, unavailable = 0;
    for (int i = 0; i < 9; ++i) {
        const gc_u32 bit = 1u << i, status = o.domainStatus[i];
        if (status > CAPTURE_PARTIAL || ((o.requestedDomains & bit) ? status == CAPTURE_NOT_REQUESTED :
            (status != CAPTURE_NOT_REQUESTED || o.driverStatus[i]))) return false;
        if (status == CAPTURE_COMPLETE || status == CAPTURE_PARTIAL) captured |= bit;
        if ((o.requestedDomains & bit) && status != CAPTURE_COMPLETE) omitted |= bit;
        if (status == CAPTURE_UNSUPPORTED) unavailable |= bit;
    }
    if (captured != o.capturedDomains || omitted != o.omittedDomains || unavailable != o.unavailableDomains) return false;
    const HardwareCaptureScalar* scalars[] = {&o.graphics, &o.memory, &o.xbarClock, &o.xbarVoltage, &o.systemClock, &o.videoClock};
    for (const auto* scalar : scalars) if (!hardware_capture_scalar_valid(*scalar)) return false;
    if (o.graphics.surface > CLOCK_OFFSET_SURFACE_GLOBAL || o.memory.surface > CLOCK_OFFSET_SURFACE_GLOBAL) return false;
    const auto& p = o.power;
    if (p.currentValid > 1 || p.defaultValid > 1 || p.rangeValid > 1 || p.writable > 1 || p.gridMw < 0 ||
        (p.currentValid ? p.currentMw <= 0 : p.currentMw != 0) ||
        (p.defaultValid ? p.defaultMw <= 0 : p.defaultMw != 0) ||
        (p.rangeValid ? (p.minimumMw <= 0 || p.maximumMw < p.minimumMw) : (p.minimumMw || p.maximumMw))) return false;
    if ((!o.pinGetterValid && (o.pinPresent || o.pinMinimumHz || o.pinMaximumHz)) ||
        (o.pinGetterValid && (o.pinPresent ? (o.pinMinimumHz <= 0 || o.pinMaximumHz < o.pinMinimumHz) :
            (o.pinMinimumHz || o.pinMaximumHz)))) return false;
    bool indices[128]{};
    for (gc_u32 i = 0; i < 128; ++i) {
        const auto& point = o.points[i];
        if (i >= o.pointCount) { if (!hardware_capture_bytes_zero(&point, sizeof(point))) return false; continue; }
        if (point.index >= 128 || indices[point.index] || point.offsetValid > 1 || point.frequencyValid > 1 ||
            point.writableState > 2 || point.reserved || point.voltageUv <= 0 ||
            (point.offsetValid ? false : point.offsetHz != 0) ||
            (point.frequencyValid ? point.frequencyHz <= 0 : point.frequencyHz != 0) ||
            ((o.capturedDomains & PROFILE_DOMAIN_VF_CURVE) && point.writableState == 1 && !point.offsetValid)) return false;
        indices[point.index] = true;
    }
    if ((o.capturedDomains & PROFILE_DOMAIN_VF_CURVE) && (!o.topologyComplete || !o.pointCount)) return false;
    if ((o.capturedDomains & PROFILE_DOMAIN_GRAPHICS_OFFSET) && (!o.graphics.valid ||
        (o.composition != CAPTURE_COMPOSITION_INDEPENDENT &&
         !(o.composition == CAPTURE_COMPOSITION_ALIASED && (o.capturedDomains & PROFILE_DOMAIN_VF_CURVE))))) return false;
    if ((o.capturedDomains & PROFILE_DOMAIN_MEMORY) && (!o.memory.valid || !o.memory.surface)) return false;
    if ((o.capturedDomains & PROFILE_DOMAIN_POWER) && (!p.currentValid || !p.defaultValid)) return false;
    if ((o.capturedDomains & PROFILE_DOMAIN_XBAR) && !o.xbarClock.valid && !o.xbarVoltage.valid) return false;
    if ((o.capturedDomains & PROFILE_DOMAIN_SYS_CLK) && !o.systemClock.valid) return false;
    if ((o.capturedDomains & PROFILE_DOMAIN_VIDEO_CLK) && !o.videoClock.valid) return false;
    if ((o.capturedDomains & PROFILE_DOMAIN_LOCK) && !o.pinGetterValid) return false;
    if (o.fan.valid > 1 || o.fan.curvePresent > 1 || o.fan.zeroRpmEnabled > 1 || o.fan.reserved) return false;
    if (!o.fan.valid && !hardware_capture_bytes_zero(&o.fan, sizeof(o.fan))) return false;
    if ((o.capturedDomains & PROFILE_DOMAIN_FAN) && !o.fan.valid) return false;
    if (o.fan.valid) {
        FanCurveConfig curve{};
        curve.pollIntervalMs = (int)o.fan.pollIntervalMs; curve.hysteresisC = (int)o.fan.hysteresisC;
        curve.zeroRpmEnabled = o.fan.zeroRpmEnabled; curve.zeroRpmHysteresisC = (gc_u8)o.fan.zeroRpmHysteresisC;
        if (o.fan.mode > FAN_MODE_CURVE || o.fan.dutyPct > 100 ||
            o.fan.pollIntervalMs > INT_MAX || o.fan.hysteresisC > INT_MAX ||
            (o.fan.mode == FAN_MODE_CURVE && !o.fan.curvePresent) || o.fan.zeroRpmHysteresisC > 255) return false;
        for (int i = 0; i < 8; ++i) {
            const auto& point = o.fan.points[i];
            if (point.enabled > 1 || !hardware_capture_bytes_zero(point.reserved, sizeof(point.reserved))) return false;
            curve.points[i] = {point.enabled, point.temperatureC, point.dutyPct};
        }
        if (o.fan.curvePresent && !fan_curve_validate(&curve, nullptr, 0)) return false;
    }
    return true;
}

#endif
