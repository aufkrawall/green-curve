// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Measure the runtime-lock wait the read handlers pay. It is the one component
// of their turnaround that nothing used to record, so a client-side deadline
// expiry could not be attributed to lock contention versus hardware I/O.
static bool service_state_read_lock_runtime(const char* command,
        ULONGLONG* waitMsOut) {
    ULONGLONG started = GetTickCount64();
    bool acquired = try_lock_service_runtime(
        SERVICE_STATE_READ_RUNTIME_LOCK_WAIT_MS);
    ULONGLONG waited = GetTickCount64() - started;
    if (waitMsOut) *waitMsOut = waited;
    // Anything past a few milliseconds means the fan runtime thread or a
    // lifecycle write held the lock; that is what pushes a read toward its
    // deadline, so record it rather than inferring it later from a gap.
    if (waited >= 5) {
        debug_log("service state read: %s waited %llu ms for the runtime lock (budget %u ms, acquired=%d)\n",
            command, (unsigned long long)waited,
            (unsigned int)SERVICE_STATE_READ_RUNTIME_LOCK_WAIT_MS,
            acquired ? 1 : 0);
    }
    return acquired;
}

// A read that overruns its own budget is the condition that makes a client
// deadline fire. Log it from the side that knows why, so the next occurrence is
// diagnosable from the service half instead of only as a client-side timeout.
static void service_state_read_report_turnaround(const char* command,
        bool fullSync, ULONGLONG handlerStartMs, ULONGLONG lockWaitMs) {
    ULONGLONG elapsed = GetTickCount64() - handlerStartMs;
    unsigned long budget = service_state_read_handler_budget_ms(fullSync);
    if (elapsed < budget) return;
    debug_log("service state read: %s OVERRAN its handler budget elapsedMs=%llu budgetMs=%lu lockWaitMs=%llu clientDeadlineMs=%lu\n",
        command, (unsigned long long)elapsed, budget,
        (unsigned long long)lockWaitMs,
        service_state_read_response_timeout_ms(fullSync));
}

// Perform a serialized full hardware refresh. READY is published only after
// curve, offset, global-control, and telemetry reads all succeed in one pass.
static void service_handle_snapshot_request(ServiceResponse* response) {
    char detail[256] = {};
    ULONGLONG lockWaitMs = 0;
    ULONGLONG handlerStartMs = GetTickCount64();
    if (!service_state_read_lock_runtime("snapshot", &lockWaitMs)) {
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
    service_state_read_report_turnaround("snapshot", true, handlerStartMs,
        lockWaitMs);
    if (authoritativeRefresh) {
        service_lifecycle_post_prerequisite_signal(
            "serialized snapshot probe confirmed GPU readiness");
    }
}

// Serve cached-or-refreshed telemetry for SERVICE_CMD_GET_TELEMETRY.
//
// Moved out of main_service_pipe.cpp (2026-08-14) when the v19 updater
// commands arrived and that file was already at its size ratchet.  It sits
// beside service_handle_snapshot_request() because the two are the same shape:
// take the runtime lock briefly, refresh, publish, and fall back to the cached
// snapshot rather than blocking when a recovery reapply holds the lock.
static void service_handle_telemetry_request(ServiceResponse* response) {
    char detail[256] = {};
    ULONGLONG lockWaitMs = 0;
    ULONGLONG handlerStartMs = GetTickCount64();
    bool lockAcquired = service_state_read_lock_runtime("telemetry", &lockWaitMs);
    if (!lockAcquired) {
        debug_log("service telemetry: runtime lock busy (recovery reapply in progress), serving cached telemetry\n");
        response->status = SERVICE_STATUS_OK;
        StringCchCopyA(response->message, ARRAY_COUNT(response->message), "telemetry cached");
        populate_service_snapshot(&response->snapshot);
        if (g_serviceControlStateValid) response->controlState = g_serviceControlState;
        return;
    }
    bool telemetryReady = service_refresh_telemetry_for_request(
        detail, sizeof(detail));
    if (!telemetryReady) {
        debug_log("service telemetry: hardware initialize unavailable: %s\n", detail[0] ? detail : "unknown");
    }
    response->status = SERVICE_STATUS_OK;
    StringCchCopyA(response->message, ARRAY_COUNT(response->message), detail[0] ? detail : "telemetry ready");
    populate_service_snapshot(&response->snapshot);
    if (g_serviceControlStateValid) response->controlState = g_serviceControlState;
    unlock_service_runtime();
    service_state_read_report_turnaround("telemetry", false, handlerStartMs,
        lockWaitMs);
    // Routine telemetry is a cached observation, not lifecycle
    // readiness authority. The bootstrap hardware probe above
    // is reached only before the first initialized snapshot;
    // normal snapshot/PnP/config events own prerequisite wakes.
}
