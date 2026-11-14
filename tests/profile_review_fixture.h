// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_editor_review_session.h"
namespace profile_review_fixture {
static inline void live(ServiceResponse& response) {
    response = {}; response.status = SERVICE_STATUS_OK; auto& s = response.state;
    s.serviceInstanceId = 4; s.gpuGeneration = 7; s.topologySignature = 21;
    s.gpuPhase = SERVICE_GPU_PHASE_READY; s.validSections = SERVICE_STATE_SECTION_READY_REQUIRED;
    response.snapshot.adapterCount = 1; response.snapshot.health.availableMutationDomains = 511;
    auto& target = response.snapshot.adapters[0]; target.valid = target.pciInfoValid = 1; target.pciBus = 6; target.deviceId = 42;
}
static inline ProfileDocument source() {
    auto d = profile_document_make_empty(); d.domains = PROFILE_DOMAIN_MEMORY;
    d.hasMemoryOffset = true; d.memoryOffsetHz = -123500000; d.memorySurface = CLOCK_OFFSET_SURFACE_P0;
    return d;
}
static inline void capture(ServiceResponse& response, const ProfileDocument& source) {
    auto& o = response.capture; o = {};
    o.valid = 1; o.schemaVersion = HARDWARE_CAPTURE_SCHEMA_CURRENT; o.gpuGeneration = response.state.gpuGeneration;
    o.binding = hardware_capture_binding(response.snapshot.adapters[0]); o.requestedDomains = profile_editor_review_domains(source);
    for (int i = 0; i < 9; ++i) if (o.requestedDomains & (1u << i)) hardware_capture_domain(o, 1 << i, CAPTURE_UNSUPPORTED, -100001);
    o.topologyComplete = 1; o.pointCount = 1; o.points[0].index = 22; o.points[0].voltageUv = 662000;
    o.points[0].writableState = o.points[0].frequencyValid = o.points[0].offsetValid = 1; o.points[0].frequencyHz = 1500000000;
    hardware_capture_domain(o, PROFILE_DOMAIN_VF_CURVE, CAPTURE_COMPLETE);
    o.composition = CAPTURE_COMPOSITION_INDEPENDENT;
    o.graphics.valid = o.graphics.writable = 1; o.graphics.surface = CLOCK_OFFSET_SURFACE_P0; o.graphics.grid = 1000000;
    hardware_capture_domain(o, PROFILE_DOMAIN_GRAPHICS_OFFSET, CAPTURE_COMPLETE);
    o.memory.valid = o.memory.writable = o.memory.rangeValid = 1; o.memory.surface = CLOCK_OFFSET_SURFACE_P0;
    o.memory.grid = 500000; o.memory.minimum = -3000000000LL; o.memory.maximum = 3000000000LL;
    hardware_capture_domain(o, PROFILE_DOMAIN_MEMORY, CAPTURE_COMPLETE);
    o.fingerprint = hardware_capture_fingerprint(o);
    o.revision = service_control_review_revision(o.fingerprint, response.state.activeDesiredValid ? &response.desired : nullptr);
}
}
