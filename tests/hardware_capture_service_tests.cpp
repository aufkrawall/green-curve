// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Execute the actual daemon capture handler without a driver or daemon process.
#include "linux_backend.h"
#include "linux_gpu_controller.h"
#include "hardware_capture_runtime.h"

namespace {
// The daemon's per-GPU state, passed explicitly like the daemon does.
static LinuxGpuController fixture{};
static int calls = 0, logs = 0;
static bool accepted = true, argumentsValid = true;
static gc_u32 expectedDomains = PROFILE_DOMAIN_MEMORY;
static gc_i64 memoryValue = -500000;
static void dlog(const char*, ...) { ++logs; }
static bool fake_capture(const LinuxGpuState* gpu, gc_u64 generation, gc_u32 domains,
    const DesiredSettings* owned, HardwareCaptureObservation* output, char* why, size_t size) {
    ++calls;
    argumentsValid &= gpu == &fixture.gpu && generation == fixture.gpuGeneration && domains == expectedDomains &&
        owned == (fixture.hasActiveDesired ? &fixture.activeDesired : nullptr);
    if (!accepted) { snprintf(why, size, "%s", "fixture control changed"); return false; }
    *output = {}; output->valid = 1; output->schemaVersion = HARDWARE_CAPTURE_SCHEMA_CURRENT;
    output->gpuGeneration = generation; output->requestedDomains = domains;
    output->binding = hardware_capture_binding(gpu->selectedGpu);
    for (int i = 0; i < 9; ++i) hardware_capture_domain(*output, 1 << i, CAPTURE_UNSUPPORTED);
    output->memory.valid = 1; output->memory.surface = CLOCK_OFFSET_SURFACE_P0; output->memory.value = memoryValue;
    hardware_capture_domain(*output, PROFILE_DOMAIN_MEMORY, CAPTURE_COMPLETE);
    output->fingerprint = hardware_capture_fingerprint(*output);
    return true;
}
#define linux_backend_capture_current fake_capture
#include "linux_daemon_capture.h"
#undef linux_backend_capture_current
}

int run_hardware_capture_service_tests() {
    linux_gpu_controller_init(&fixture);
    fixture.gpuGeneration = 9;
    ServiceRequest req{}; req.command = SERVICE_CMD_CAPTURE_CURRENT; req.captureDomains = PROFILE_DOMAIN_MEMORY;
    auto response = std::make_unique<ServiceResponse>();
    fixture.gpu.selectedGpu.valid = fixture.gpu.selectedGpu.pciInfoValid = 1;
    fixture.gpu.selectedGpu.deviceId = 0x1234; fixture.gpu.selectedGpu.pciBus = 1;
    req.targetGpu = fixture.gpu.selectedGpu;
    daemon_handle_capture(fixture, &req, response.get());
    if (calls || response->capture.valid || response->status != SERVICE_STATUS_ERROR) return 8520;
    fixture.gpuReady = true; fixture.stateUncertain = true;
    daemon_handle_capture(fixture, &req, response.get());
    if (calls || response->capture.valid || response->status != SERVICE_STATUS_ERROR) return 8521;
    fixture.stateUncertain = false; fixture.fanOwnershipHandbackDeferred = true;
    daemon_handle_capture(fixture, &req, response.get());
    if (calls || response->capture.valid || response->status != SERVICE_STATUS_ERROR) return 8522;
    fixture.fanOwnershipHandbackDeferred = false; req.targetGpu.pciBus = 2;
    daemon_handle_capture(fixture, &req, response.get());
    if (calls || response->capture.valid || response->status != SERVICE_STATUS_ERROR) return 8523;
    req.targetGpu = fixture.gpu.selectedGpu;
    daemon_handle_capture(fixture, &req, response.get());
    if (calls != 1 || !response->capture.valid || response->status != SERVICE_STATUS_OK || !argumentsValid ||
        !hardware_capture_observation_valid(response->capture) || fixture.hasActiveDesired ||
        fixture.gpu.selectedGpu.pciBus != 1 || !strstr(response->message, "pin=unknown")) return 8524;
    *response = {}; fixture.hasActiveDesired = true; fixture.activeDesired.hasMemOffset = 1;
    daemon_handle_capture(fixture, &req, response.get());
    if (calls != 2 || response->status != SERVICE_STATUS_OK || !argumentsValid || !fixture.hasActiveDesired ||
        !fixture.activeDesired.hasMemOffset) return 8525;
    accepted = false; *response = {};
    daemon_handle_capture(fixture, &req, response.get());
    if (calls != 3 || response->capture.valid || response->status != SERVICE_STATUS_ERROR ||
        strcmp(response->message, "fixture control changed") || logs != 7) return 8526;
    accepted = true; expectedDomains = req.captureDomains = 511; *response = {};
    daemon_handle_capture(fixture, &req, response.get());
    const auto reviewed = response->capture;
    if (calls != 4 || response->status != SERVICE_STATUS_OK || !argumentsValid ||
        !reviewed.revision || reviewed.revision == reviewed.fingerprint) return 8905;
    req.command = SERVICE_CMD_APPLY; req.captureDomains = 0;
    req.expectedServiceInstanceId = 1; req.expectedGpuGeneration = fixture.gpuGeneration;
    req.operationId = 1; req.applyOrigin = SERVICE_APPLY_ORIGIN_CLI; req.flags = SERVICE_REQUEST_FLAG_INTERACTIVE;
    req.desired.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED;
    req.expectedControlFingerprint = reviewed.revision; req.expectedControlDomains = 511;
    if (!daemon_control_preconditions_match(fixture, &req, response.get()) || calls != 5 || !argumentsValid) return 8906;
    fixture.activeDesired.curvePointCaptured[2] = 1;
    if (daemon_control_preconditions_match(fixture, &req, response.get()) || calls != 6 ||
        response->status != SERVICE_STATUS_STALE_STATE || !fixture.activeDesired.curvePointCaptured[2]) return 8907;
    fixture.activeDesired.curvePointCaptured[2] = 0; memoryValue = 0;
    if (daemon_control_preconditions_match(fixture, &req, response.get()) || calls != 7) return 8908;
    memoryValue = -500000; fixture.stateUncertain = true;
    if (daemon_control_preconditions_match(fixture, &req, response.get()) || calls != 7 || !fixture.stateUncertain) return 8909;
    fixture.stateUncertain = false; fixture.fanOwnershipHandbackDeferred = true;
    if (daemon_control_preconditions_match(fixture, &req, response.get()) || calls != 7 || !fixture.fanOwnershipHandbackDeferred) return 8910;
    fixture.fanOwnershipHandbackDeferred = false; ++req.targetGpu.pciBus;
    if (daemon_control_preconditions_match(fixture, &req, response.get()) || calls != 7 || fixture.gpu.selectedGpu.pciBus != 1) return 8911;
    --req.targetGpu.pciBus; req.expectedControlFingerprint = req.expectedControlDomains = 0;
    if (!daemon_control_preconditions_match(fixture, &req, response.get()) || calls != 7) return 8912;
    return 0;
}
