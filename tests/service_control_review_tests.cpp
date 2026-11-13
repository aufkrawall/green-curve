// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "service_control_review.h"
#include <stdio.h>

int run_service_control_review_tests() {
    ServiceRequest r{};
    r.magic = SERVICE_PROTOCOL_MAGIC; r.version = SERVICE_PROTOCOL_VERSION; r.callerPid = 1;
    r.command = SERVICE_CMD_APPLY; r.flags = SERVICE_REQUEST_FLAG_INTERACTIVE;
    r.applyOrigin = SERVICE_APPLY_ORIGIN_CLI; r.operationId = 3;
    r.expectedServiceInstanceId = 4; r.expectedGpuGeneration = 5;
    r.targetGpu.valid = r.targetGpu.pciInfoValid = 1; r.targetGpu.pciBus = 1; r.targetGpu.deviceId = 0x1234;
    r.desired.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED;
    r.desired.hasMemOffset = 1; r.desired.memOffsetMHz = -1;
    HardwareCaptureObservation current{};
    current.valid = 1; current.schemaVersion = HARDWARE_CAPTURE_SCHEMA_CURRENT;
    current.gpuGeneration = r.expectedGpuGeneration; current.binding = hardware_capture_binding(r.targetGpu);
    current.requestedDomains = 511;
    for (int i = 0; i < 9; ++i) hardware_capture_domain(current, 1 << i, CAPTURE_UNSUPPORTED);
    hardware_capture_domain(current, PROFILE_DOMAIN_MEMORY, CAPTURE_COMPLETE);
    current.memory.valid = 1; current.memory.surface = CLOCK_OFFSET_SURFACE_P0; current.memory.value = -500000;
    current.topologyComplete = 1; current.pointCount = 1;
    current.points[0].voltageUv = 600000; current.points[0].frequencyValid = 1;
    current.points[0].frequencyHz = 1000000000;
    current.fingerprint = hardware_capture_fingerprint(current);
    service_control_review_stamp(current, nullptr);
    if (!hardware_capture_observation_valid(current)) return 8884;
    char why[256]{}; int reads = 0;
    const auto read = [&](HardwareCaptureObservation& out, char*, size_t) { ++reads; out = current; return true; };
    if (!service_control_review_matches(r, nullptr, read, why, sizeof(why)) || reads) return 8885;
    r.expectedControlFingerprint = current.revision;
    if (!service_request_reject_reason(&r) || service_control_review_matches(r, nullptr, read, why, sizeof(why)) || reads) return 8886;
    r.expectedControlDomains = 511;
    if (service_request_reject_reason(&r) || !service_control_review_matches(r, nullptr, read, why, sizeof(why)) || reads != 1) return 8887;
    current.points[0].frequencyHz += 30000000;
    if (!service_control_review_matches(r, nullptr, read, why, sizeof(why))) return 8888;
    current.memory.value += 500000; current.fingerprint = hardware_capture_fingerprint(current);
    if (service_control_review_matches(r, nullptr, read, why, sizeof(why))) return 8889;
    current.memory.value -= 500000; current.fingerprint = hardware_capture_fingerprint(current);
    DesiredSettings owned{}; owned.curvePointCaptured[1] = 1;
    if (service_control_review_matches(r, &owned, read, why, sizeof(why))) return 8890;
    service_control_review_stamp(current, &owned); r.expectedControlFingerprint = current.revision;
    if (!service_control_review_matches(r, &owned, read, why, sizeof(why))) return 8891;
    owned.curvePointCapturedOffsetHz[1] = 500;
    if (service_control_review_matches(r, &owned, read, why, sizeof(why))) return 8892;
    owned.curvePointCapturedOffsetHz[1] = 0;
    auto changed = r; changed.targetGpu.pciBus = 2;
    if (service_control_review_matches(changed, &owned, read, why, sizeof(why))) return 8893;
    changed = r; ++changed.expectedGpuGeneration;
    if (service_control_review_matches(changed, &owned, read, why, sizeof(why))) return 8894;
    current.fingerprint ^= 1;
    if (service_control_review_matches(r, &owned, read, why, sizeof(why))) return 8895;
    current.fingerprint ^= 1; current.memory.valid = 0;
    if (service_control_review_matches(r, &owned, read, why, sizeof(why))) return 8896;
    current.memory.valid = 1;
    changed = r; changed.expectedControlDomains = PROFILE_DOMAIN_MEMORY;
    const int before = reads;
    if (service_control_review_matches(changed, &owned, read, why, sizeof(why)) || reads != before) return 8897;
    changed = r; changed.command = SERVICE_CMD_CAPTURE_CURRENT;
    if (!service_request_reject_reason(&changed)) return 8898;
    changed = r; changed.applyOrigin = SERVICE_APPLY_ORIGIN_APP_LAUNCH;
    if (!service_request_reject_reason(&changed)) return 8899;
    changed = r; changed.flags = 0;
    if (!service_request_reject_reason(&changed)) return 8900;
    changed = r; changed.desired.applicationScope = DESIRED_SCOPE_LEGACY;
    if (!service_request_reject_reason(&changed)) return 8901;
    changed = r; changed.desired.authoring.graphicsPrecise = 1;
    if (!service_request_reject_reason(&changed)) return 8902;
    const auto fail = [&](HardwareCaptureObservation&, char* text, size_t size) {
        ++reads; snprintf(text, size, "%s", "fixture: passive session unavailable"); return false;
    };
    if (service_control_review_matches(r, &owned, fail, why, sizeof(why)) ||
        strcmp(why, "fixture: passive session unavailable")) return 8903;
    // Distinct physical and owned-state tokens, stable across value copies.
    DesiredSettings copied = owned;
    if (service_control_review_revision(current.fingerprint, &copied) != r.expectedControlFingerprint ||
        service_control_review_revision(current.fingerprint, nullptr) == r.expectedControlFingerprint) return 8904;
    static_assert(offsetof(DesiredSettings, lockCi) > offsetof(DesiredSettings, hasLock) + 1, "fixture needs padding");
    ((unsigned char*)&copied)[offsetof(DesiredSettings, lockCi) - 1] = 0xa5;
    if (service_control_review_revision(current.fingerprint, &copied) != r.expectedControlFingerprint) return 8913;
    copied.curvePointCaptured[1] = 0;
    if (service_control_review_revision(current.fingerprint, &copied) == r.expectedControlFingerprint) return 8914;
    return 0;
}
#ifdef GC_CONTROL_REVIEW_FIXTURE_MAIN
int main() { const int result = run_service_control_review_tests(); if (result) fprintf(stderr, "control review: %d\n", result); return result; }
#endif
