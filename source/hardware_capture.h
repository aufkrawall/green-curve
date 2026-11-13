// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_HARDWARE_CAPTURE_H
#define GREEN_CURVE_HARDWARE_CAPTURE_H
#include "gpu_core.h"
#include "hardware_capture_types.h"
#include "profile_document.h"
#include <memory>

static_assert(VF_NUM_POINTS == 128 && FAN_CURVE_MAX_POINTS == 8, "capture wire limits require versioning");

static inline void hardware_capture_domain(HardwareCaptureObservation& out, int domain,
    HardwareCaptureStatus status, int driverStatus = 0) {
    int index = 0;
    while (index < 9 && (1 << index) != domain) ++index;
    if (index == 9 || !(out.requestedDomains & (gc_u32)domain)) return;
    out.domainStatus[index] = status; out.driverStatus[index] = driverStatus;
    out.capturedDomains &= ~(gc_u32)domain; out.omittedDomains &= ~(gc_u32)domain;
    out.unavailableDomains &= ~(gc_u32)domain;
    if (status == CAPTURE_COMPLETE || status == CAPTURE_PARTIAL) out.capturedDomains |= (gc_u32)domain;
    if (status != CAPTURE_COMPLETE) out.omittedDomains |= (gc_u32)domain;
    if (status == CAPTURE_UNSUPPORTED) out.unavailableDomains |= (gc_u32)domain;
}

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
        o.osFamily > PROFILE_SOURCE_OS_LINUX || o.backendFamily > INT_MAX ||
        o.composition > CAPTURE_COMPOSITION_ALIASED || o.alignmentReserved ||
        !profile_bounded_text_valid(o.driverVersion, sizeof(o.driverVersion), 64)) return false;
    if (!o.binding.valid || o.binding.valid > 1 || o.binding.pciInfoValid > 1 ||
        !hardware_capture_bytes_zero(o.binding.reserved, sizeof(o.binding.reserved)) ||
        o.binding.nvapiIndex >= MAX_GPU_ADAPTERS || o.binding.nvmlIndex >= MAX_GPU_ADAPTERS ||
        o.binding.pciDomain > 65535 || o.binding.pciBus > 255 || o.binding.pciDevice > 31 || o.binding.pciFunction > 7) return false;
    gc_u32 captured = 0, omitted = 0, unavailable = 0;
    for (int i = 0; i < 9; ++i) {
        const gc_u32 bit = 1u << i, status = o.domainStatus[i];
        if (status > CAPTURE_PARTIAL || ((o.requestedDomains & bit) ? status == CAPTURE_NOT_REQUESTED : status != CAPTURE_NOT_REQUESTED)) return false;
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
    if ((o.capturedDomains & PROFILE_DOMAIN_MEMORY) && !o.memory.valid) return false;
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

// Moving observed frequencies are deliberately excluded; control validity,
// topology, exact offsets, ranges, power denominators and binding are included.
static inline void hardware_capture_control_image(HardwareCaptureObservation& o) {
    o.revision = o.fingerprint = 0; o.readAttempts = 0;
    for (auto& point : o.points) point.frequencyHz = 0;
}
static inline gc_u64 hardware_capture_fingerprint(const HardwareCaptureObservation& observation) {
    auto copy = std::make_unique<HardwareCaptureObservation>(observation);
    hardware_capture_control_image(*copy);
    gc_u64 hash = 14695981039346656037ull;
    const auto* bytes = (const unsigned char*)copy.get();
    for (size_t i = 0; i < sizeof(*copy); ++i) { hash ^= bytes[i]; hash *= 1099511628211ull; }
    return hash ? hash : 1;
}
template <class Read>
static inline bool hardware_capture_consistent(Read read, HardwareCaptureObservation& output,
    char* why, size_t whySize) {
    auto first = std::make_unique<HardwareCaptureObservation>();
    auto second = std::make_unique<HardwareCaptureObservation>();
    for (unsigned int attempt = 1; attempt <= 3; ++attempt) {
        memset(first.get(), 0, sizeof(*first)); memset(second.get(), 0, sizeof(*second));
        if (!read(*first, why, whySize) || !read(*second, why, whySize)) return false;
        if (!hardware_capture_observation_valid(*first) || !first->valid ||
            !hardware_capture_observation_valid(*second) || !second->valid) {
            if (why && whySize) snprintf(why, whySize, "%s", "capture getters returned an invalid observation");
            return false;
        }
        auto comparison = std::make_unique<HardwareCaptureObservation>(*second);
        hardware_capture_control_image(*first); hardware_capture_control_image(*comparison);
        if (memcmp(first.get(), comparison.get(), sizeof(*first)) != 0) continue;
        second->readAttempts = attempt;
        second->fingerprint = hardware_capture_fingerprint(*second);
        second->revision = second->fingerprint;
        output = *second;
        return true;
    }
    if (why && whySize) snprintf(why, whySize, "%s", "programmable controls or topology changed during all three capture attempts");
    return false;
}

static inline bool hardware_capture_to_document(const HardwareCaptureObservation& o, ProfileDocument& output,
    char* why, size_t whySize) {
    if (!o.valid || !hardware_capture_observation_valid(o)) {
        if (why && whySize) snprintf(why, whySize, "%s", "invalid hardware capture");
        return false;
    }
    ProfileDocument d = profile_document_make_empty();
    d.pointSetPolicy = PROFILE_POINTS_REPLACE;
    d.sourceTopologyComplete = o.topologyComplete != 0; d.topologyCount = (int)o.pointCount;
    d.source.osFamily = (int)o.osFamily; d.source.backendFamily = (int)o.backendFamily;
    d.source.deviceId = o.binding.deviceId; d.source.subSystemId = o.binding.subSystemId;
    memcpy(d.source.driverVersion, o.driverVersion, sizeof(d.source.driverVersion));
    d.source.captureComplete = !o.omittedDomains;
    d.source.capturedDomains = o.capturedDomains; d.source.omittedDomains = o.omittedDomains;
    d.source.unavailableDomains = o.unavailableDomains;
    d.source.pinKnowledge = !o.pinGetterValid ? PROFILE_PIN_UNKNOWN : o.pinPresent ? PROFILE_PIN_PRESENT : PROFILE_PIN_ABSENT;
    for (int i = 0; i < d.topologyCount; ++i) {
        const auto& p = o.points[i];
        d.topology[i] = {(int)p.index, (int)p.index, p.voltageUv, p.writableState};
        auto& point = d.points[i];
        point.hasObservedFrequency = p.frequencyValid != 0; point.observedFrequencyHz = p.frequencyHz;
        point.hasObservedOffset = p.offsetValid != 0; point.observedOffsetHz = p.offsetHz;
        if ((o.capturedDomains & PROFILE_DOMAIN_VF_CURVE) && p.offsetValid && p.writableState == 1) {
            point.kind = PROFILE_POINT_CAPTURED_OFFSET; point.capturedOffsetHz = p.offsetHz;
            d.domains |= PROFILE_DOMAIN_VF_CURVE;
        }
    }
    if ((o.capturedDomains & PROFILE_DOMAIN_VF_CURVE) && !d.domains) d.domains |= PROFILE_DOMAIN_VF_CURVE;
    if (o.graphics.valid) {
        d.graphicsOffset.hasObservedOffset = true; d.graphicsOffset.observedOffsetHz = o.graphics.value;
        d.graphicsOffset.observedSurface = o.graphics.surface; d.graphicsOffset.observedComposition = (int)o.composition;
    }
    if ((o.capturedDomains & PROFILE_DOMAIN_GRAPHICS_OFFSET) && o.composition == CAPTURE_COMPOSITION_INDEPENDENT) {
        d.domains |= PROFILE_DOMAIN_GRAPHICS_OFFSET; d.graphicsOffset.present = true; d.graphicsOffset.offsetHz = o.graphics.value;
    }
    if (o.capturedDomains & PROFILE_DOMAIN_MEMORY) {
        d.domains |= PROFILE_DOMAIN_MEMORY; d.hasMemoryOffset = true; d.memoryOffsetHz = o.memory.value; d.memorySurface = o.memory.surface;
    }
    d.power.hasObservedDefaultMw = o.power.defaultValid != 0; d.power.observedDefaultMw = o.power.defaultMw;
    d.power.hasObservedCurrentMw = o.power.currentValid != 0; d.power.observedCurrentMw = o.power.currentMw;
    if (o.capturedDomains & PROFILE_DOMAIN_POWER) {
        d.domains |= PROFILE_DOMAIN_POWER; d.power.present = true;
        d.power.ratio = tuning_power_ratio_from_mw(o.power.currentMw, o.power.defaultMw);
    }
    if (o.pinGetterValid && o.pinPresent) {
        d.lock.hasObservedPinRange = true; d.lock.observedPinMinimumHz = o.pinMinimumHz;
        d.lock.observedPinMaximumHz = o.pinMaximumHz; d.lock.provenance = PROFILE_LOCK_PROVENANCE_DRIVER_GETTER;
    }
    if (o.capturedDomains & PROFILE_DOMAIN_FAN) {
        d.domains |= PROFILE_DOMAIN_FAN; d.fan.present = true; d.fan.fanMode = (int)o.fan.mode;
        d.fan.fanAuto = o.fan.mode == FAN_MODE_AUTO; d.fan.fanPercent = (int)o.fan.dutyPct;
        d.fan.hasCurveConfig = o.fan.curvePresent != 0;
        d.fan.fanCurve.pollIntervalMs = (int)o.fan.pollIntervalMs; d.fan.fanCurve.hysteresisC = (int)o.fan.hysteresisC;
        d.fan.fanCurve.zeroRpmEnabled = o.fan.zeroRpmEnabled; d.fan.fanCurve.zeroRpmHysteresisC = (gc_u8)o.fan.zeroRpmHysteresisC;
        for (int i = 0; i < 8; ++i) d.fan.fanCurve.points[i] = {o.fan.points[i].enabled, o.fan.points[i].temperatureC, o.fan.points[i].dutyPct};
    }
    if (o.capturedDomains & PROFILE_DOMAIN_XBAR) {
        d.domains |= PROFILE_DOMAIN_XBAR; d.hasXbarOffsetHz = o.xbarClock.valid != 0; d.xbarOffsetHz = o.xbarClock.value;
        d.hasXbarMsvddOffsetUv = o.xbarVoltage.valid != 0; d.xbarMsvddOffsetUv = o.xbarVoltage.value;
    }
    if (o.capturedDomains & PROFILE_DOMAIN_SYS_CLK) { d.domains |= PROFILE_DOMAIN_SYS_CLK; d.hasSysClkOffsetHz = true; d.sysClkOffsetHz = o.systemClock.value; }
    if (o.capturedDomains & PROFILE_DOMAIN_VIDEO_CLK) { d.domains |= PROFILE_DOMAIN_VIDEO_CLK; d.hasVideoClkOffsetHz = true; d.videoClkOffsetHz = o.videoClock.value; }
    if (!profile_document_structurally_valid(d, why, whySize)) return false;
    output = d;
    return true;
}
#endif
