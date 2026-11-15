// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_capture_proposals.h"
namespace profile_capture_proposal_fixture {
static inline HardwareCaptureObservation curve() {
    HardwareCaptureObservation o{};
    o.valid = o.binding.valid = o.topologyComplete = 1;
    o.schemaVersion = HARDWARE_CAPTURE_SCHEMA_CURRENT;
    o.gpuGeneration = 7;
    o.requestedDomains = PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_GRAPHICS_OFFSET | PROFILE_DOMAIN_LOCK;
    o.osFamily = PROFILE_SOURCE_OS_WINDOWS;
    o.composition = CAPTURE_COMPOSITION_INDEPENDENT;
    hardware_capture_domain(o, PROFILE_DOMAIN_VF_CURVE, CAPTURE_COMPLETE);
    hardware_capture_domain(o, PROFILE_DOMAIN_GRAPHICS_OFFSET, CAPTURE_COMPLETE);
    hardware_capture_domain(o, PROFILE_DOMAIN_LOCK, CAPTURE_UNSUPPORTED, -100001);
    o.graphics.valid = o.graphics.writable = 1;
    o.graphics.grid = 1000000;
    o.graphics.surface = CLOCK_OFFSET_SURFACE_P0;
    o.vfLimits = {1, 1, 1, 1, VF_OFFSET_RANGE_SOURCE_NVML_GRAPHICS, -1000000000, 1000000000, 500};
    o.pointCount = 6;
    const gc_i64 frequencies[] = {900000000, 1000000500, 999999500, 1000000400, 1000000000, 1000000000};
    for (int i = 0; i < 6; ++i) {
        auto &p = o.points[i];
        p.index = 21 + i;
        p.voltageUv = 650000 + i * 12000;
        p.writableState = p.offsetValid = p.frequencyValid = 1;
        p.frequencyHz = frequencies[i];
        p.offsetHz = i ? -((gc_i64)i * 100000000 + 500) : 0;
    }
    o.fingerprint = hardware_capture_fingerprint(o);
    o.revision = service_control_review_revision(o.fingerprint, nullptr);
    return o;
}
} // namespace profile_capture_proposal_fixture
