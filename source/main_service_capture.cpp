// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "service_control_review.h"
#include "hardware_capture_aux.h"
static void* nvapi_qi_passive(unsigned int id);

// Called under the runtime lock by every passive capture read.
static void service_log_client_locks(const NvapiClientLocks& locks) {
    static NvapiClientLocksLogGate gate{};
    if (!nvapi_client_locks_log_changed(gate, locks)) return;
    char line[192];
    nvapi_client_locks_describe(locks, line, sizeof(line));
    debug_log("%s\n", line);
}

// Called under the runtime lock by every passive capture read that includes
// memory: what the offset getter said, what the running clock said, and which
// one the capture reports. Logged once per changed reading.
static void service_log_memory_clock(const MemoryClockReading& reading, gc_i64 getterHz,
                                     const HardwareCaptureObservation& out) {
    static char last[256]{};
    char line[256];
    snprintf(line, sizeof(line),
        "capture memory: getter=%lldHz (valid=%d surface=%s) running=%u MHz (pstates=%d) board=%u MHz (nvml=%d)"
        " -> %lldHz evidence=%s",
        (long long)getterHz, out.memory.valid ? 1 : 0, hardware_capture_clock_surface_name(out.memory.surface),
        reading.runningMaxMHz, reading.pstatesStatus, reading.boardMaxMHz, reading.boardStatus,
        (long long)out.memory.value, hardware_capture_memory_evidence_name(out.memoryEvidence));
    if (!strcmp(line, last)) return;
    StringCchCopyA(last, ARRAY_COUNT(last), line);
    debug_log("%s\n", line);
}

// Caller holds the runtime lock. Reuse the passive collector at the mutation
// boundary without acquiring that lock recursively or waking recovery.
static bool service_collect_capture_locked(const GpuAdapterInfo& target, gc_u32 domains,
    HardwareCaptureObservation& output, char* why, size_t size) {
    const auto read = [&](HardwareCaptureObservation& out, char* why, size_t size) {
        HardwareCaptureContext c{};
        EnterCriticalSection(&g_appLock);
        const ServiceGpuPhase phase = service_effective_gpu_phase_locked();
        c.usable = !g_app.deviceRemoved && !g_app.pendingDeviceRecovery &&
            !nvml_crash_recovery_active() && phase != SERVICE_GPU_PHASE_STARTING &&
            phase != SERVICE_GPU_PHASE_DEVICE_MISSING && phase != SERVICE_GPU_PHASE_RECOVERING &&
            phase != SERVICE_GPU_PHASE_REAPPLYING &&
            hardware_capture_target_matches(target, g_app.selectedGpu);
        c.gpuHandle = g_app.gpuHandle; c.query = nvapi_qi_passive; c.backend = g_app.vfBackend;
        c.nvmlUsable = g_app.nvmlReady; c.nvml = &g_nvml_api; c.nvmlDevice = g_app.nvmlDevice;
        c.binding = hardware_capture_binding(g_app.selectedGpu);
        c.gpuGeneration = (gc_u64)InterlockedCompareExchange64(&g_serviceGpuGeneration, 0, 0);
        c.osFamily = PROFILE_SOURCE_OS_WINDOWS; c.backendFamily = (gc_u32)g_app.gpuFamily;
        const auto driver = service_loaded_driver_label();
        memcpy(c.driverVersion, driver.text, sizeof(driver.text));
        c.ownedFanIntent = g_serviceHasActiveDesired ? &g_serviceActiveDesired : nullptr;
        c.lockObserved = service_log_client_locks;
        // The live state reads memory from the running clock; capture must
        // report the same value (memory_clock_evidence.h).
        c.memoryClockReader = hardware_capture_memory_clock;
        c.memoryObserved = service_log_memory_clock;
        LeaveCriticalSection(&g_appLock);
        return hardware_capture_collect(c, domains, hardware_capture_aux, out, why, size);
    };
    if (!hardware_capture_consistent(read, output, why, size)) return false;
    service_control_review_stamp(output, g_serviceHasActiveDesired ? &g_serviceActiveDesired : nullptr);
    return true;
}

static bool service_control_preconditions_match_locked(const ServiceRequest* request, char* why, size_t size) {
    const auto read = [&](HardwareCaptureObservation& out, char* detail, size_t count) {
        return service_collect_capture_locked(request->targetGpu, request->expectedControlDomains, out, detail, count);
    };
    const bool ok = service_control_review_matches(*request,
        g_serviceHasActiveDesired ? &g_serviceActiveDesired : nullptr, read, why, size);
    if (!ok) debug_log("service APPLY control review refused: %s\n", why);
    return ok;
}

static void service_handle_capture_request(const ServiceRequest* request, ServiceResponse* response) {
    ULONGLONG waitMs = 0;
    response->status = SERVICE_STATUS_ERROR;
    if (!service_state_read_lock_runtime("capture", &waitMs)) {
        StringCchCopyA(response->message, ARRAY_COUNT(response->message), "capture: GPU runtime is busy");
        return;
    }
    const bool ok = service_collect_capture_locked(request->targetGpu, request->captureDomains,
        response->capture, response->message, sizeof(response->message));
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
