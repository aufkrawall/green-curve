// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_HARDWARE_CAPTURE_H
#define GREEN_CURVE_HARDWARE_CAPTURE_H
#include "gpu_core.h"
#include "hardware_capture_types.h"
#include "hardware_capture_wire_validation.h"
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
