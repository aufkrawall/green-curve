// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "hardware_capture_runtime.h"
#include "hardware_capture_aux.h"
static void* nvapi_qi_passive(unsigned int id);

static void service_handle_capture_request(const ServiceRequest* request, ServiceResponse* response) {
    ULONGLONG waitMs = 0;
    response->status = SERVICE_STATUS_ERROR;
    if (!service_state_read_lock_runtime("capture", &waitMs)) {
        StringCchCopyA(response->message, ARRAY_COUNT(response->message), "capture: GPU runtime is busy");
        return;
    }
    const auto read = [&](HardwareCaptureObservation& out, char* why, size_t size) {
        HardwareCaptureContext c{};
        EnterCriticalSection(&g_appLock);
        const ServiceGpuPhase phase = service_effective_gpu_phase_locked();
        c.usable = !g_app.deviceRemoved && !g_app.pendingDeviceRecovery &&
            !nvml_crash_recovery_active() && phase != SERVICE_GPU_PHASE_STARTING &&
            phase != SERVICE_GPU_PHASE_DEVICE_MISSING && phase != SERVICE_GPU_PHASE_RECOVERING &&
            phase != SERVICE_GPU_PHASE_REAPPLYING &&
            hardware_capture_target_matches(request->targetGpu, g_app.selectedGpu);
        c.gpuHandle = g_app.gpuHandle; c.query = nvapi_qi_passive; c.backend = g_app.vfBackend;
        c.nvmlUsable = g_app.nvmlReady; c.nvml = &g_nvml_api; c.nvmlDevice = g_app.nvmlDevice;
        c.binding = hardware_capture_binding(g_app.selectedGpu);
        c.gpuGeneration = (gc_u64)InterlockedCompareExchange64(&g_serviceGpuGeneration, 0, 0);
        c.osFamily = PROFILE_SOURCE_OS_WINDOWS; c.backendFamily = (gc_u32)g_app.gpuFamily;
        const auto driver = service_loaded_driver_label();
        memcpy(c.driverVersion, driver.text, sizeof(driver.text));
        c.ownedFanIntent = g_serviceHasActiveDesired ? &g_serviceActiveDesired : nullptr;
        LeaveCriticalSection(&g_appLock);
        return hardware_capture_collect(c, request->captureDomains, hardware_capture_aux, out, why, size);
    };
    const bool ok = hardware_capture_consistent(read, response->capture, response->message, sizeof(response->message));
    if (ok) {
        response->status = SERVICE_STATUS_OK;
        hardware_capture_summary(response->capture, response->message, sizeof(response->message));
    }
    debug_log("%s\n", response->message);
    unlock_service_runtime();
}

static void service_capture_check_published_generation(ServiceResponse* response) {
    if (!response->capture.valid || response->capture.gpuGeneration == response->state.gpuGeneration) return;
    memset(&response->capture, 0, sizeof(response->capture));
    response->status = SERVICE_STATUS_STALE_STATE;
    response->outcomeSeverity = service_response_resolve_outcome_severity(response->status, 0);
    StringCchCopyA(response->message, ARRAY_COUNT(response->message), "capture: GPU generation changed before publication");
    debug_log("%s\n", response->message);
}
