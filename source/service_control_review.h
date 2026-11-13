// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_SERVICE_CONTROL_REVIEW_H
#define GREEN_CURVE_SERVICE_CONTROL_REVIEW_H
#include "hardware_capture_runtime.h"

// The physical fingerprint remains portable observation metadata. The service
// revision additionally binds the outgoing owned intent, including owned zero.
// Hash fields rather than object representation: padding is not state.
static inline gc_u64 service_control_review_revision(
    gc_u64 physicalFingerprint, const DesiredSettings* owned) {
    gc_u64 hash = 14695981039346656037ull;
    const auto feed = [&](gc_u64 value) {
        for (int i = 0; i < 8; ++i) { hash ^= value & 255u; hash *= 1099511628211ull; value >>= 8; }
    };
    feed(physicalFingerprint); feed(owned != nullptr);
    if (!owned) return hash ? hash : 1;
    const auto& d = *owned;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        feed(d.hasCurvePoint[i]); feed(d.curvePointMHz[i]); feed(d.curvePointFromGpuOffset[i]);
        feed(d.curvePointCaptured[i]); feed(d.curvePointCapturedOffsetHz[i]);
        feed(d.authoring.pointFrequencyPrecise[i]); feed(d.authoring.pointFrequencyHz[i]);
        feed(d.authoring.voltageUv[i]); feed(d.authoring.graphicsExcluded[i]);
    }
    feed(d.hasLock); feed(d.lockCi); feed(d.lockMHz); feed(d.lockMode); feed(d.lockTracksAnchor);
    feed(d.hasGpuOffset); feed(d.gpuOffsetMHz); feed(d.gpuOffsetExcludeLowCount);
    feed(d.hasMemOffset); feed(d.memOffsetMHz); feed(d.hasPowerLimit); feed(d.powerLimitPct);
    feed(d.hasFan); feed(d.fanAuto); feed(d.fanMode); feed(d.fanPercent);
    feed(d.fanCurve.pollIntervalMs); feed(d.fanCurve.hysteresisC);
    feed(d.fanCurve.zeroRpmEnabled); feed(d.fanCurve.zeroRpmHysteresisC);
    for (const auto& p : d.fanCurve.points) { feed(p.enabled); feed(p.temperatureC); feed(p.fanPercent); }
    feed(d.resetOcBeforeApply); feed(d.hasXbarOffsetKhz); feed(d.xbarOffsetKhz);
    feed(d.hasXbarMsvddOffsetUv); feed(d.xbarMsvddOffsetUv);
    feed(d.hasSysClkOffsetKhz); feed(d.sysClkOffsetKhz); feed(d.hasVideoClkOffsetKhz); feed(d.videoClkOffsetKhz);
    feed(d.applicationScope); feed(d.memOffsetPrecise); feed(d.memOffsetSurface); feed(d.memOffsetHz);
    feed(d.powerLimitRatio); feed(d.powerRatioNumerator); feed(d.powerRatioDenominator);
    const auto& a = d.authoring;
    feed(a.graphicsPrecise); feed(a.lockPrecise); feed(a.identityExclusions); feed(a.topologyBound);
    feed(a.graphicsSurface); feed(a.graphicsOffsetHz); feed(a.lockFrequencyHz); feed(a.topologyCount); feed(a.reserved);
    return hash ? hash : 1;
}
static inline void service_control_review_stamp(HardwareCaptureObservation& o, const DesiredSettings* owned) {
    o.revision = service_control_review_revision(o.fingerprint, owned);
}

// Read must use an already bound, usable session and the bounded passive
// collector. It must never initialize, refresh recovery, select, or write.
template <class Read>
static inline bool service_control_review_matches(const ServiceRequest& request,
    const DesiredSettings* owned, Read read, char* why, size_t size) {
    if (!request.expectedControlFingerprint && !request.expectedControlDomains) return true;
    const char* reason = service_control_preconditions_reject_reason(request);
    if (reason) { if (why && size) snprintf(why, size, "%s", reason); return false; }
    auto observation = std::make_unique<HardwareCaptureObservation>();
    if (!read(*observation, why, size)) return false;
    const auto& o = *observation;
    const auto binding = hardware_capture_binding(request.targetGpu);
    if (!o.valid || !hardware_capture_observation_valid(o) || !o.fingerprint ||
        o.fingerprint != hardware_capture_fingerprint(o) ||
        o.requestedDomains != request.expectedControlDomains ||
        o.gpuGeneration != request.expectedGpuGeneration ||
        memcmp(&o.binding, &binding, sizeof(binding)) ||
        request.expectedControlFingerprint != service_control_review_revision(o.fingerprint, owned)) {
        if (why && size) snprintf(why, size, "%s",
            "Reviewed GPU controls, ownership, or binding changed; capture and review again before Apply");
        return false;
    }
    return true;
}
#endif
