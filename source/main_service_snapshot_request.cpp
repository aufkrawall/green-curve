// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Perform a serialized full hardware refresh. READY is published only after
// curve, offset, global-control, and telemetry reads all succeed in one pass.
static void service_handle_snapshot_request(ServiceResponse* response) {
    char detail[256] = {};
    if (!try_lock_service_runtime(250)) {
        debug_log("service snapshot: runtime lock busy (recovery reapply in progress), serving cached globals\n");
        response->status = SERVICE_STATUS_OK;
        StringCchCopyA(response->message, ARRAY_COUNT(response->message),
            "snapshot cached");
        populate_service_snapshot(&response->snapshot);
        if (g_serviceControlStateValid)
            response->controlState = g_serviceControlState;
        return;
    }

    bool initialized = hardware_initialize(detail, sizeof(detail));
    bool authoritativeRefresh = initialized;
    if (!initialized) {
        debug_log("service snapshot: hardware initialize unavailable: %s\n",
            detail[0] ? detail : "unknown");
    } else {
        bool offsetsOk = false;
        if (!read_live_curve_snapshot_settled(3, 20, &offsetsOk)) {
            authoritativeRefresh = false;
            debug_log("service snapshot: live curve refresh failed; cached curve is not authoritative\n");
        } else if (!offsetsOk) {
            authoritativeRefresh = false;
            debug_log("service snapshot: curve refresh lacked offset readback confirmation; state remains non-READY\n");
        }
        // NVML can access-violate on stale handles during device reconnect.
        // The recovery worker owns reinitialization; this read path publishes a
        // non-READY envelope until a complete refresh is safe and successful.
        if (nvml_crash_recovery_active()) {
            authoritativeRefresh = false;
            debug_log("service snapshot: NVML crash recovery in progress, using cached globals\n");
        } else if (!refresh_global_state(detail, sizeof(detail))) {
            authoritativeRefresh = false;
            debug_log("service snapshot: full state refresh failed; cached globals are invalid%s%s\n",
                detail[0] ? ": " : "", detail[0] ? detail : "");
        }
        if (authoritativeRefresh) {
            populate_control_state(&g_serviceControlState);
            g_serviceControlStateValid = true;
        } else {
            g_serviceControlStateValid = false;
            memset(&g_serviceControlState, 0, sizeof(g_serviceControlState));
        }
    }

    ServiceGpuPhase currentPhase = (ServiceGpuPhase)InterlockedCompareExchange(
        &g_serviceGpuPhase, 0, 0);
    ServiceGpuPhase publishedPhase = authoritativeRefresh
        ? SERVICE_GPU_PHASE_READY
        : currentPhase == SERVICE_GPU_PHASE_DEVICE_MISSING
            ? SERVICE_GPU_PHASE_DEVICE_MISSING
        : (currentPhase == SERVICE_GPU_PHASE_RECOVERING ||
           currentPhase == SERVICE_GPU_PHASE_REAPPLYING ||
           nvml_crash_recovery_active())
            ? SERVICE_GPU_PHASE_RECOVERING : SERVICE_GPU_PHASE_DEGRADED;
    bool lostReadyAuthority = !authoritativeRefresh &&
        currentPhase == SERVICE_GPU_PHASE_READY;
    service_publish_gpu_phase(publishedPhase, lostReadyAuthority,
        authoritativeRefresh
        ? "full snapshot confirmed current hardware"
        : "full snapshot could not confirm every live section");
    response->status = SERVICE_STATUS_OK;
    StringCchCopyA(response->message, ARRAY_COUNT(response->message),
        authoritativeRefresh ? "snapshot ready" :
        (detail[0] ? detail : "snapshot unavailable"));
    populate_service_snapshot(&response->snapshot);
    if (g_serviceControlStateValid)
        response->controlState = g_serviceControlState;
    unlock_service_runtime();
    if (authoritativeRefresh) {
        service_lifecycle_post_prerequisite_signal(
            "serialized snapshot probe confirmed GPU readiness");
    }
}
