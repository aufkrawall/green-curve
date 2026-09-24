// Forward declaration: xbar_refresh_live_state() is defined later in the
// shell (xbar_telemetry.h inside the NVML shard); the fan worker's telemetry
// refresh calls it under the runtime lock.
static bool xbar_refresh_live_state();

// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Read-only telemetry cache and the serialized service fan runtime worker.

#include "fan_worker_lifecycle_policy.h"
// The cancellable wait itself, shared with the native regression fixture.
#include "fan_worker_lock_wait_win32.h"


static DWORD service_active_fan_runtime_interval_ms() {
    if (g_app.fanFixedRuntimeActive) return FAN_FIXED_RUNTIME_INTERVAL_MS;
    if (g_app.fanCurveRuntimeActive) {
        DWORD intervalMs = (DWORD)g_app.activeFanCurve.pollIntervalMs;
        return intervalMs < 250 ? 250 : intervalMs;
    }
    return 0;
}
static void mark_service_telemetry_cache_updated(const char* source) {
    if (!app_is_service_process()) return;
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

    // Live ClkDomains scalars (XBAR clock/MSVDD, SYS, VIDEO): the Advanced
    // dialog renders these once per second, so refresh them here under the
    // runtime lock with a >= 1 s throttle.  xbar_refresh_live_state() is a
    // cheap read (one GET_CONTROL + one CLK_MEASURE) and self-guards on
    // probe validity.
    {
        static ULONGLONG lastClkDomainsReadTickMs = 0;
        ULONGLONG clkNow = GetTickCount64();
        if (lastClkDomainsReadTickMs == 0 ||
            clkNow - lastClkDomainsReadTickMs >= 1000) {
            if (xbar_refresh_live_state()) {
                lastClkDomainsReadTickMs = clkNow;
            }
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

// Blocking acquisition that a stop request can cancel.  Returns false, with the
// lock NOT held, when `cancelEvent` is signaled first.  Only the fan worker uses
// it: because its wait now ends on the stop event too, whoever stops it can keep
// the runtime mutex for the whole join instead of releasing it mid-transaction
// (fan_worker_lifecycle_policy.h).  Poison and abandonment are handled exactly
// as lock_service_runtime() handles them.
static bool lock_service_runtime_unless_signaled(HANDLE cancelEvent) {
    if (!cancelEvent) {
        lock_service_runtime();
        return true;
    }
    if (InterlockedExchangeAdd(&g_serviceRuntimeLockPoisoned, 0) != 0) {
        service_runtime_reject_poisoned_acquisition("cancellable acquisition", false);
    }
    if (!ensure_service_runtime_lock()) {
        service_runtime_lock_fail_closed("mutex creation", GetLastError());
    }
    DWORD waitResult = 0;
    switch (fan_worker_wait_for_runtime_lock(cancelEvent, g_serviceRuntimeLock, &waitResult)) {
        case FAN_WORKER_LOCK_STOP_REQUESTED:
            return false;
        case FAN_WORKER_LOCK_ABANDONED:
            service_runtime_mutex_abandoned("cancellable wait");
        case FAN_WORKER_LOCK_FAILED:
            service_runtime_lock_fail_closed("cancellable mutex wait",
                waitResult == WAIT_FAILED ? GetLastError() : waitResult);
        case FAN_WORKER_LOCK_ACQUIRED:
            break;
    }
    if (InterlockedExchangeAdd(&g_serviceRuntimeLockPoisoned, 0) != 0) {
        service_runtime_reject_poisoned_acquisition(
            "cancellable acquisition", true);
    }
    DWORD currentThreadId = GetCurrentThreadId();
    if (g_serviceRuntimeLockOwnerThreadId == currentThreadId) {
        g_serviceRuntimeLockDepth++;
    } else {
        g_serviceRuntimeLockOwnerThreadId = currentThreadId;
        g_serviceRuntimeLockDepth = 1;
    }
    return true;
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
            // Cancellable: a stop requested while this pulse queues behind an
            // Apply/Reset reaches it here, so the stopper never has to release
            // the runtime mutex to be able to join this thread.
            if (!lock_service_runtime_unless_signaled(g_serviceFanStopEvent)) {
                InterlockedExchange(&g_serviceFanPulseInFlight, 0);
                debug_log("service_fan_runtime_thread: stop requested while waiting for the runtime lock\n");
                break;
            }
            // Queuing on the gate is not evidence of anything, so the wedge
            // window starts HERE, once this thread owns the gate and is about
            // to enter nvml.dll.  Before this line the pulse may simply have
            // been waiting behind a long apply, which is healthy.
            service_note_hardware_progress();
            if (service_update_install_release_and_block()) {
                InterlockedExchange(&g_serviceFanPulseInFlight, 0);
                continue;
            }
            service_runtime_pulse();
            service_note_hardware_progress();
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

// Whether the calling thread is the fan worker.  The id is only trusted while
// the worker's handle is open: an open handle keeps the thread object alive, so
// its id cannot have been reused by another thread.
static bool service_fan_worker_is_current_thread() {
    return g_serviceFanThread && g_fanRuntimeThreadId != 0 &&
        g_fanRuntimeThreadId == GetCurrentThreadId();
}

static void service_fan_worker_release_handle() {
    CloseHandle(g_serviceFanThread);
    g_serviceFanThread = nullptr;
    g_fanRuntimeThreadId = 0;
}

// The worker handle is shared by the pipe workers, the lifecycle worker, the
// updater and the service main thread.  The runtime mutex is what serializes
// them; a caller without it is a handle-ownership race (double CloseHandle, or
// two workers), so it is logged rather than silently tolerated.
static void service_fan_worker_note_unserialized(const char* operation) {
    if (service_runtime_lock_held_by_current_thread()) return;
    debug_log("%s: called WITHOUT the runtime lock; fan worker handle ownership"
              " is unserialized here\n", operation);
}

static bool ensure_service_fan_runtime_thread() {
    service_fan_worker_note_unserialized("ensure_service_fan_runtime_thread");
    bool present = g_serviceFanThread != nullptr;
    bool alive = present && WaitForSingleObject(g_serviceFanThread, 0) == WAIT_TIMEOUT;
    bool stopSignaled = g_serviceFanStopEvent &&
        WaitForSingleObject(g_serviceFanStopEvent, 0) == WAIT_OBJECT_0;
    FanWorkerEnsurePlan plan = fan_worker_ensure_plan(present, alive,
        stopSignaled, service_fan_worker_is_current_thread());
    switch (plan) {
        case FAN_WORKER_ENSURE_ALREADY_RUNNING:
            debug_log("ensure_service_fan_runtime_thread: already running\n");
            return true;
        case FAN_WORKER_ENSURE_REFUSE_SELF:
            debug_log("ensure_service_fan_runtime_thread: refused -- the retiring fan"
                      " worker cannot replace itself\n");
            return false;
        case FAN_WORKER_ENSURE_JOIN_RETIRING_THEN_CREATE: {
            // It was told to stop and is leaving; its lock wait is cancellable,
            // so this join cannot depend on the runtime mutex we may hold.
            DWORD joined = WaitForSingleObject(g_serviceFanThread,
                SERVICE_FAN_THREAD_STOP_TIMEOUT_MS);
            if (joined != WAIT_OBJECT_0) {
                debug_log("ensure_service_fan_runtime_thread: retiring fan worker did not"
                          " exit (result=%lu); thread handle preserved, not replaced\n",
                    joined);
                return false;
            }
            service_fan_worker_release_handle();
            debug_log("ensure_service_fan_runtime_thread: joined a retiring worker, recreating\n");
            break;
        }
        case FAN_WORKER_ENSURE_REAP_THEN_CREATE:
            service_fan_worker_release_handle();
            debug_log("ensure_service_fan_runtime_thread: stale handle detected, recreating\n");
            break;
        case FAN_WORKER_ENSURE_CREATE:
            break;
    }
    if (!g_serviceFanStopEvent) {
        g_serviceFanStopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!g_serviceFanStopEvent) return false;
    }
    ResetEvent(g_serviceFanStopEvent);
    DWORD threadId = 0;
    g_serviceFanThread = CreateThread(nullptr, (SIZE_T)64 * 1024, service_fan_runtime_thread_proc, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, &threadId);
    g_fanRuntimeThreadId = g_serviceFanThread ? threadId : 0;
    debug_log("ensure_service_fan_runtime_thread: created=%d threadId=%lu plan=%s\n",
        g_serviceFanThread ? 1 : 0, threadId, fan_worker_ensure_plan_name(plan));
    return g_serviceFanThread != nullptr;
}

static void stop_service_fan_runtime_thread() {
    FanWorkerStopPlan plan = fan_worker_stop_plan(g_serviceFanThread != nullptr,
        service_fan_worker_is_current_thread());
    if (plan == FAN_WORKER_STOP_NOTHING) return;
    if (g_serviceFanStopEvent) SetEvent(g_serviceFanStopEvent);
    if (plan == FAN_WORKER_STOP_SIGNAL_ONLY) {
        // Joining yourself can only time out, and it used to do so with the
        // runtime mutex released and g_appLock still held by the failure
        // escalation that got us here -- an ABBA deadlock with any telemetry
        // request.  The loop sees the event and exits after this pulse.
        debug_log("stop_service_fan_runtime_thread: requested by the fan worker itself;"
                  " signalled, it exits after the current pulse\n");
        return;
    }
    service_fan_worker_note_unserialized("stop_service_fan_runtime_thread");
    // The runtime mutex is deliberately NOT released here.  The worker's lock
    // wait also ends on the stop event, so it can leave without it, and keeping
    // it closes the window in which a lifecycle restore or the updater could
    // run a hardware transaction in the middle of the caller's Apply/Reset.
    DWORD waitResult = WaitForSingleObject(g_serviceFanThread, SERVICE_FAN_THREAD_STOP_TIMEOUT_MS);
    if (waitResult != WAIT_OBJECT_0) {
        debug_log("stop_service_fan_runtime_thread: timed out waiting for fan thread (result=%lu); thread handle preserved to prevent replacement\n", waitResult);
        // Keep g_serviceFanThread handle alive to prevent a new thread from starting
        // while the original may still reference shared events or runtime state.
        return;
    }
    service_fan_worker_release_handle();
    debug_log("stop_service_fan_runtime_thread: fan worker joined (plan=%s)\n",
        fan_worker_stop_plan_name(plan));
}
