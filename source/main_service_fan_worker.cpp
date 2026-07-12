// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Read-only telemetry cache and the serialized service fan runtime worker.


static DWORD service_active_fan_runtime_interval_ms() {
    if (g_app.fanFixedRuntimeActive) return FAN_FIXED_RUNTIME_INTERVAL_MS;
    if (g_app.fanCurveRuntimeActive) {
        DWORD intervalMs = (DWORD)g_app.activeFanCurve.pollIntervalMs;
        return intervalMs < 250 ? 250 : intervalMs;
    }
    return 0;
}
static void mark_service_telemetry_cache_updated(const char* source) {
    if (!g_app.isServiceProcess) return;
    g_serviceTelemetryLastHardwarePollTickMs = GetTickCount64();
    if (source && source[0]) {
        StringCchCopyA(g_serviceTelemetryLastPollSource, ARRAY_COUNT(g_serviceTelemetryLastPollSource), source);
    } else {
        g_serviceTelemetryLastPollSource[0] = 0;
    }
}

static bool service_telemetry_cache_is_fresh(ULONGLONG now) {
    if (!g_serviceTelemetryLastHardwarePollTickMs) return false;
    ULONGLONG ageMs = now - g_serviceTelemetryLastHardwarePollTickMs;
    DWORD runtimeIntervalMs = service_active_fan_runtime_interval_ms();
    if (runtimeIntervalMs > 0) {
        return ageMs <= (ULONGLONG)runtimeIntervalMs + SERVICE_TELEMETRY_RUNTIME_STALE_GRACE_MS;
    }
    return ageMs < SERVICE_TELEMETRY_IDLE_REFRESH_INTERVAL_MS;
}

static bool service_refresh_idle_telemetry(char* detail, size_t detailSize) {
    char firstErr[128] = {};
    bool anyOk = false;

    char fanDetail[128] = {};
    if (nvml_read_fans(fanDetail, sizeof(fanDetail))) {
        anyOk = true;
    } else if (fanDetail[0]) {
        StringCchCopyA(firstErr, ARRAY_COUNT(firstErr), fanDetail);
    }

    char tempDetail[128] = {};
    int temperatureC = 0;
    if (nvml_read_temperature(&temperatureC, tempDetail, sizeof(tempDetail))) {
        anyOk = true;
    } else if (!firstErr[0] && tempDetail[0]) {
        StringCchCopyA(firstErr, ARRAY_COUNT(firstErr), tempDetail);
    }

    if (!anyOk) {
        set_message(detail, detailSize, "%s", firstErr[0] ? firstErr : "Telemetry refresh failed");
        return false;
    }

    populate_control_state(&g_serviceControlState);
    g_serviceControlStateValid = true;
    mark_service_telemetry_cache_updated("idle telemetry");
    return true;
}

static bool service_refresh_telemetry_for_request(char* detail, size_t detailSize) {
    // If within the NVML crash recovery window, skip hardware_initialize()
    // entirely.  NVML reads (VF curve, temperature, etc.) access-violate on
    // the transitional driver after a device reconnect, killing this pipe
    // server thread and abandoning g_serviceRuntimeLock.  Return true without
    // refreshing — the caller populates the snapshot from cached state.
    if (nvml_crash_recovery_active()) {
        debug_log("service telemetry: crash recovery active, skipping hardware_initialize\n");
        if (detail && detailSize > 0) detail[0] = 0;
        return true;
    }

    // A visible GUI asks for telemetry once per second. Re-running the full
    // adapter/NVAPI initialization probe for every request creates substantial
    // device I/O even though the fan worker already owns a fresh telemetry
    // cache. Only bootstrap through hardware_initialize when the service has
    // not produced an initialized snapshot yet. Device arrival and explicit
    // commands have their own serialized reinitialization paths.
    if (!g_app.gpuHandle || !g_app.loaded) {
        if (!hardware_initialize(detail, detailSize)) return false;
        debug_log("service telemetry: initialized unavailable hardware while bootstrapping the first snapshot\n");
    }

    bool runtimeActive = g_app.fanCurveRuntimeActive || g_app.fanFixedRuntimeActive;
    if (runtimeActive) {
        bool needRuntimeThread = g_serviceFanThread == nullptr;
        if (g_serviceFanThread) {
            DWORD waitResult = WaitForSingleObject(g_serviceFanThread, 0);
            needRuntimeThread = waitResult != WAIT_TIMEOUT;
        }
        if (needRuntimeThread) {
            ensure_service_fan_runtime_thread();
        }
    }

    ULONGLONG now = GetTickCount64();
    if (runtimeActive) {
        if (!service_telemetry_cache_is_fresh(now)) {
            bool noRuntimeThread = !g_serviceFanThread;
            bool noCache = g_serviceTelemetryLastHardwarePollTickMs == 0;
            // Don't call service_runtime_pulse() from the pipe server
            // telemetry handler while recovering from a recent NVML crash —
            // doing so triggers the recovery-reapply path, which calls
            // NVML/NVAPI writes that can access-violate when the GPU kernel
            // driver is still in a transitional state after a device
            // reconnect.  The VEH kills the pipe server thread, breaking the
            // GUI connection with ERROR_BROKEN_PIPE (109).  The fan runtime
            // thread handles recovery independently.  Uses the shared recovery
            // window so the guard matches the fan thread's crash back-off.
            bool recentCrash = nvml_crash_recovery_active();
            if ((noRuntimeThread || noCache) && !recentCrash) {
                service_runtime_pulse();
            } else {
                debug_log("service telemetry: using stale runtime cache ageMs=%llu source=%s\n",
                    now - g_serviceTelemetryLastHardwarePollTickMs,
                    g_serviceTelemetryLastPollSource[0] ? g_serviceTelemetryLastPollSource : "<none>");
            }
        }
    } else if (!service_telemetry_cache_is_fresh(now)) {
        char telemetryDetail[128] = {};
        if (!service_refresh_idle_telemetry(telemetryDetail, sizeof(telemetryDetail))) {
            debug_log("service telemetry: lightweight refresh failed: %s\n",
                telemetryDetail[0] ? telemetryDetail : "unknown");
            mark_service_telemetry_cache_updated("idle telemetry failed");
        }
    }

    if (!g_serviceControlStateValid) {
        populate_control_state(&g_serviceControlState);
        g_serviceControlStateValid = true;
    }
    if (detail && detailSize > 0) detail[0] = 0;
    return true;
}

static void service_runtime_pulse() {
    EnterCriticalSection(&g_appLock);
    bool curveActive = g_app.fanCurveRuntimeActive;
    bool fixedActive = g_app.fanFixedRuntimeActive;
    LeaveCriticalSection(&g_appLock);
    if (!curveActive && !fixedActive) return;
    ULONGLONG now = GetTickCount64();
    bool logPulse = g_serviceRuntimeLastPulseLogTickMs == 0 ||
        now - g_serviceRuntimeLastPulseLogTickMs >= SERVICE_RUNTIME_NOISY_LOG_INTERVAL_MS;
    if (logPulse) {
        g_serviceRuntimeLastPulseLogTickMs = now;
        EnterCriticalSection(&g_appLock);
        debug_log("service_runtime_pulse: curve=%d fixed=%d lastApplyMs=%llu mode=%d fixedPct=%d\n",
            g_app.fanCurveRuntimeActive ? 1 : 0,
            g_app.fanFixedRuntimeActive ? 1 : 0,
            g_app.fanRuntimeLastApplyTickMs,
            g_app.activeFanMode,
            g_app.activeFanFixedPercent);
        LeaveCriticalSection(&g_appLock);
    }
    // GPU driver restart recovery (restart64.exe / TDR / driver upgrade).
    // A stale NVIDIA user-mode DLL is never reloaded in this process. Any
    // corroborated VEH/device cue requests the nonce-bound clean process
    // restart; the old process performs no recovery write.
    LONG crashCount = g_nvmlCrashCount;
    bool deviceWasRemoved = false;
    bool pendingRecovery = false;
    EnterCriticalSection(&g_appLock);
    if (g_app.deviceRemoved) deviceWasRemoved = true;
    if (g_app.pendingDeviceRecovery) pendingRecovery = true;
    LeaveCriticalSection(&g_appLock);

    if (deviceWasRemoved) {
        // Device physically gone — NVML is unsafe; wait for arrival.  Skip the
        // fan tick entirely (it would only read stale/zero data).
        if (logPulse) {
            debug_log("service_runtime_pulse: device removed, awaiting arrival (NVML idle)\n");
        }
        return;
    }

    if (crashCount > 0 || pendingRecovery) {
        if (logPulse || crashCount > 0) {
            debug_log("service_runtime_pulse: driver recovery cue detected; requesting controlled process restart (crashCount=%ld pending=%d)\n",
                (long)crashCount, pendingRecovery ? 1 : 0);
        }
        launch_recovery_thread();
        return;
    }

    // While the lifecycle worker owns a controlled-recovery continuation, skip
    // the fan tick so it cannot touch a transitional driver before the sole
    // authorized restore finishes.
    if (InterlockedExchangeAdd(&g_serviceReapplyInProgress, 0) != 0) {
        if (logPulse) {
            debug_log("service_runtime_pulse: reapply active, skipping fan tick\n");
        }
        return;
    }

    // Healthy path: drive the fan curve / fixed runtime.  If the GPU driver
    // restarts during this call, nvml_read_temperature() access-violates, the
    // VEH bumps g_nvmlCrashCount and ExitThreads us; the watchdog recreates the
    // thread and the next pulse takes the restart branch above.
    apply_fan_curve_tick();

    mark_service_telemetry_cache_updated("fan runtime");
    if (logPulse) {
        EnterCriticalSection(&g_appLock);
        debug_log("service_runtime_pulse_done: fanMode=%d currentPct=%d temp=%d runtimeLastApply=%llu failures=%u\n",
            g_app.activeFanMode,
            g_app.activeFanFixedPercent,
            g_app.gpuTemperatureValid ? g_app.gpuTemperatureC : 0,
            g_app.fanRuntimeLastApplyTickMs,
            g_app.fanRuntimeConsecutiveFailures);
        LeaveCriticalSection(&g_appLock);
    }
}

static DWORD WINAPI service_fan_runtime_thread_proc(void*) {
    HANDLE waitHandles[1] = { g_serviceFanStopEvent };
    debug_log("service_fan_runtime_thread: started\n");
    while (true) {
        DWORD waitMs = INFINITE;
        EnterCriticalSection(&g_appLock);
        bool fanFixedRuntimeActive = g_app.fanFixedRuntimeActive;
        bool fanCurveRuntimeActive = g_app.fanCurveRuntimeActive;
        DWORD pollIntervalMs = fanCurveRuntimeActive ? (DWORD)g_app.activeFanCurve.pollIntervalMs : 0;
        LeaveCriticalSection(&g_appLock);
        if (fanFixedRuntimeActive) waitMs = FAN_FIXED_RUNTIME_INTERVAL_MS;
        else if (fanCurveRuntimeActive) {
            waitMs = pollIntervalMs;
            if (waitMs < 250) waitMs = 250;
        }
        bool curveActive = fanCurveRuntimeActive;
        bool fixedActive = fanFixedRuntimeActive;
        ULONGLONG now = GetTickCount64();
        if (g_serviceFanThreadLastWaitLogTickMs == 0 ||
            now - g_serviceFanThreadLastWaitLogTickMs >= SERVICE_RUNTIME_NOISY_LOG_INTERVAL_MS ||
            waitMs != g_serviceFanThreadLastWaitMs ||
            curveActive != g_serviceFanThreadLastWaitCurve ||
            fixedActive != g_serviceFanThreadLastWaitFixed) {
            g_serviceFanThreadLastWaitLogTickMs = now;
            g_serviceFanThreadLastWaitMs = waitMs;
            g_serviceFanThreadLastWaitCurve = curveActive;
            g_serviceFanThreadLastWaitFixed = fixedActive;
            debug_log("service_fan_runtime_thread: waiting %lu ms curve=%d fixed=%d\n",
                waitMs,
                curveActive ? 1 : 0,
                fixedActive ? 1 : 0);
        }
        DWORD waitResult = WaitForMultipleObjects(1, waitHandles, FALSE, waitMs);
        if (waitResult == WAIT_OBJECT_0) break;
        if (waitResult == WAIT_TIMEOUT) {
            // Heartbeat: stamp BEFORE acquiring the lock / touching NVML so the
            // main-loop watchdog can detect a wedge inside nvml.dll (a hang the
            // VEH cannot catch) and request a controlled process restart.
            g_serviceFanPulseHeartbeatMs = GetTickCount64();
            InterlockedExchange(&g_serviceFanPulseInFlight, 1);
            lock_service_runtime();
            service_runtime_pulse();
            unlock_service_runtime();
            InterlockedExchange(&g_serviceFanPulseInFlight, 0);
            g_serviceFanPulseHeartbeatMs = GetTickCount64();
        } else if (waitResult == WAIT_FAILED) {
            debug_log("service_fan_runtime_thread: wait failed error=%lu\n", GetLastError());
            break;
        }
    }
    debug_log("service_fan_runtime_thread: exiting\n");
    return 0;
}

static bool ensure_service_fan_runtime_thread() {
    if (g_serviceFanThread) {
        DWORD waitResult = WaitForSingleObject(g_serviceFanThread, 0);
        if (waitResult == WAIT_TIMEOUT) {
            debug_log("ensure_service_fan_runtime_thread: already running\n");
            return true;
        }
        // Thread has exited; close stale handle and recreate.
        CloseHandle(g_serviceFanThread);
        g_serviceFanThread = nullptr;
        debug_log("ensure_service_fan_runtime_thread: stale handle detected, recreating\n");
    }
    if (!g_serviceFanStopEvent) {
        g_serviceFanStopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!g_serviceFanStopEvent) return false;
    }
    ResetEvent(g_serviceFanStopEvent);
    DWORD threadId = 0;
    g_serviceFanThread = CreateThread(nullptr, 64 * 1024, service_fan_runtime_thread_proc, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, &threadId);
    g_fanRuntimeThreadId = threadId;
    debug_log("ensure_service_fan_runtime_thread: created=%d threadId=%lu\n", g_serviceFanThread ? 1 : 0, threadId);
    return g_serviceFanThread != nullptr;
}

static void stop_service_fan_runtime_thread() {
    if (!g_serviceFanThread) return;
    if (g_serviceFanStopEvent) SetEvent(g_serviceFanStopEvent);
    bool lockHeld = service_runtime_lock_held_by_current_thread();
    if (lockHeld) {
        unlock_service_runtime();
    }
    DWORD waitResult = WaitForSingleObject(g_serviceFanThread, SERVICE_FAN_THREAD_STOP_TIMEOUT_MS);
    if (waitResult != WAIT_OBJECT_0) {
        debug_log("stop_service_fan_runtime_thread: timed out waiting for fan thread (result=%lu); thread handle preserved to prevent replacement\n", waitResult);
        if (lockHeld) {
            lock_service_runtime();
        }
        // Keep g_serviceFanThread handle alive to prevent a new thread from starting
        // while the original may still reference shared events or runtime state.
        return;
    }
    if (lockHeld) {
        lock_service_runtime();
    }
    CloseHandle(g_serviceFanThread);
    g_serviceFanThread = nullptr;
}
