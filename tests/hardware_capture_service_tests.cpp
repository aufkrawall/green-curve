// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Execute the actual daemon capture handler without a driver or daemon process.
#include "linux_backend.h"
#include "hardware_capture_runtime.h"

namespace {
static LinuxGpuState g_gpu{};
static bool g_gpuReady = false, g_stateUncertain = false, g_fanOwnershipHandbackDeferred = false;
static bool g_hasActiveDesired = false;
static DesiredSettings g_activeDesired{};
static gc_u64 g_daemonGpuGeneration = 9;
static int calls = 0, logs = 0;
static bool accepted = true, argumentsValid = true;
static gc_u32 expectedDomains = PROFILE_DOMAIN_MEMORY;
static gc_i64 memoryValue = -500000;
static void dlog(const char*, ...) { ++logs; }
static bool fake_capture(const LinuxGpuState* gpu, gc_u64 generation, gc_u32 domains,
    const DesiredSettings* owned, HardwareCaptureObservation* output, char* why, size_t size) {
    ++calls;
    argumentsValid &= gpu == &g_gpu && generation == g_daemonGpuGeneration && domains == expectedDomains &&
        owned == (g_hasActiveDesired ? &g_activeDesired : nullptr);
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
    ServiceRequest req{}; req.command = SERVICE_CMD_CAPTURE_CURRENT; req.captureDomains = PROFILE_DOMAIN_MEMORY;
    auto response = std::make_unique<ServiceResponse>();
    g_gpu.selectedGpu.valid = g_gpu.selectedGpu.pciInfoValid = 1;
    g_gpu.selectedGpu.deviceId = 0x1234; g_gpu.selectedGpu.pciBus = 1;
    req.targetGpu = g_gpu.selectedGpu;
    daemon_handle_capture(&req, response.get());
    if (calls || response->capture.valid || response->status != SERVICE_STATUS_ERROR) return 8520;
    g_gpuReady = true; g_stateUncertain = true;
    daemon_handle_capture(&req, response.get());
    if (calls || response->capture.valid || response->status != SERVICE_STATUS_ERROR) return 8521;
    g_stateUncertain = false; g_fanOwnershipHandbackDeferred = true;
    daemon_handle_capture(&req, response.get());
    if (calls || response->capture.valid || response->status != SERVICE_STATUS_ERROR) return 8522;
    g_fanOwnershipHandbackDeferred = false; req.targetGpu.pciBus = 2;
    daemon_handle_capture(&req, response.get());
    if (calls || response->capture.valid || response->status != SERVICE_STATUS_ERROR) return 8523;
    req.targetGpu = g_gpu.selectedGpu;
    daemon_handle_capture(&req, response.get());
    if (calls != 1 || !response->capture.valid || response->status != SERVICE_STATUS_OK || !argumentsValid ||
        !hardware_capture_observation_valid(response->capture) || g_hasActiveDesired ||
        g_gpu.selectedGpu.pciBus != 1 || !strstr(response->message, "pin=unknown")) return 8524;
    *response = {}; g_hasActiveDesired = true; g_activeDesired.hasMemOffset = 1;
    daemon_handle_capture(&req, response.get());
    if (calls != 2 || response->status != SERVICE_STATUS_OK || !argumentsValid || !g_hasActiveDesired ||
        !g_activeDesired.hasMemOffset) return 8525;
    accepted = false; *response = {};
    daemon_handle_capture(&req, response.get());
    if (calls != 3 || response->capture.valid || response->status != SERVICE_STATUS_ERROR ||
        strcmp(response->message, "fixture control changed") || logs != 7) return 8526;
    accepted = true; expectedDomains = req.captureDomains = 511; *response = {};
    daemon_handle_capture(&req, response.get());
    const auto reviewed = response->capture;
    if (calls != 4 || response->status != SERVICE_STATUS_OK || !argumentsValid ||
        !reviewed.revision || reviewed.revision == reviewed.fingerprint) return 8905;
    req.command = SERVICE_CMD_APPLY; req.captureDomains = 0;
    req.expectedServiceInstanceId = 1; req.expectedGpuGeneration = g_daemonGpuGeneration;
    req.operationId = 1; req.applyOrigin = SERVICE_APPLY_ORIGIN_CLI; req.flags = SERVICE_REQUEST_FLAG_INTERACTIVE;
    req.desired.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED;
    req.expectedControlFingerprint = reviewed.revision; req.expectedControlDomains = 511;
    if (!daemon_control_preconditions_match(&req, response.get()) || calls != 5 || !argumentsValid) return 8906;
    g_activeDesired.curvePointCaptured[2] = 1;
    if (daemon_control_preconditions_match(&req, response.get()) || calls != 6 ||
        response->status != SERVICE_STATUS_STALE_STATE || !g_activeDesired.curvePointCaptured[2]) return 8907;
    g_activeDesired.curvePointCaptured[2] = 0; memoryValue = 0;
    if (daemon_control_preconditions_match(&req, response.get()) || calls != 7) return 8908;
    memoryValue = -500000; g_stateUncertain = true;
    if (daemon_control_preconditions_match(&req, response.get()) || calls != 7 || !g_stateUncertain) return 8909;
    g_stateUncertain = false; g_fanOwnershipHandbackDeferred = true;
    if (daemon_control_preconditions_match(&req, response.get()) || calls != 7 || !g_fanOwnershipHandbackDeferred) return 8910;
    g_fanOwnershipHandbackDeferred = false; ++req.targetGpu.pciBus;
    if (daemon_control_preconditions_match(&req, response.get()) || calls != 7 || g_gpu.selectedGpu.pciBus != 1) return 8911;
    --req.targetGpu.pciBus; req.expectedControlFingerprint = req.expectedControlDomains = 0;
    if (!daemon_control_preconditions_match(&req, response.get()) || calls != 7) return 8912;
    return 0;
}
