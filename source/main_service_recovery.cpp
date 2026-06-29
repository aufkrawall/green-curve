// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Recovery-reapply machinery split out of main_service_runtime.cpp (F-MAINT-1):
// chooses the desired profile to restore, the queued/threaded reapply workers,
// their health monitor, and the post-restart startup reapply. Compiled as a shard
// included after main_service_persist.cpp and before main_service_runtime.cpp
// (no behavior change; pure move).

static bool service_choose_recovery_reapply_desired(DesiredSettings* out, GpuAdapterInfo* targetOut, char* source, size_t sourceSize) {
    if (!out) return false;
    if (source && sourceSize > 0) source[0] = 0;
    if (targetOut) memset(targetOut, 0, sizeof(*targetOut));
    if (g_serviceHasActiveDesired) {
        *out = g_serviceActiveDesired;
        if (targetOut) *targetOut = g_serviceActiveDesiredGpu;
        if (source && sourceSize > 0) StringCchCopyA(source, sourceSize, "active desired");
        return true;
    }
    if (service_load_restart_reapply_snapshot(out, targetOut)) {
        if (source && sourceSize > 0) StringCchCopyA(source, sourceSize, "restart snapshot");
        return true;
    }
    return false;
}

static bool service_reapply_desired_preserving_intent(
    const DesiredSettings* desired,
    const char* context,
    char* result,
    size_t resultSize)
{
    if (!desired) {
        set_message(result, resultSize, "No recovery reapply settings available");
        return false;
    }

    DesiredSettings savedActive = g_serviceActiveDesired;
    GpuAdapterInfo savedActiveGpu = g_serviceActiveDesiredGpu;
    bool hadSavedActive = g_serviceHasActiveDesired;
    DesiredSettings request = *desired;
    request.resetOcBeforeApply = true;

    if (g_serviceActiveDesiredGpu.valid) {
        char targetErr[256] = {};
        if (!service_select_restart_reapply_gpu(&g_serviceActiveDesiredGpu, targetErr, sizeof(targetErr))) {
            set_message(result, resultSize, "Target GPU unavailable for recovery reapply: %s",
                targetErr[0] ? targetErr : "unknown");
            return false;
        }
    }

    bool ok = service_apply_desired_settings(&request, false, result, resultSize);
    if (!ok) {
        if (hadSavedActive) {
            g_serviceActiveDesired = savedActive;
            g_serviceActiveDesiredGpu = savedActiveGpu;
        } else {
            g_serviceActiveDesired = *desired;
            if (g_app.selectedGpu.valid) {
                g_serviceActiveDesiredGpu = g_app.selectedGpu;
            } else if (g_app.selectedGpuIndex < g_app.adapterCount && g_app.adapters[g_app.selectedGpuIndex].valid) {
                g_serviceActiveDesiredGpu = g_app.adapters[g_app.selectedGpuIndex];
            } else {
                memset(&g_serviceActiveDesiredGpu, 0, sizeof(g_serviceActiveDesiredGpu));
            }
        }
        g_serviceActiveDesired.resetOcBeforeApply = false;
        g_serviceHasActiveDesired = true;
        debug_log("recovery reapply: preserved desired settings after failed %s%s%s\n",
            context && context[0] ? context : "reapply",
            result && result[0] ? ": " : "",
            result && result[0] ? result : "");
    }
    // RC3 fix: always refresh the disk snapshot after a reapply attempt so
    // a service restart mid-recovery can still restore the previous
    // profile.  service_apply_desired_settings() also writes/clears the
    // snapshot internally; this call is a safety net that also covers the
    // case where the snapshot was just cleared by a successful apply that
    // immediately failed (e.g. a flaky reapply path that returns ok=true
    // but the snapshot was already gone).
    service_write_restart_reapply_snapshot();
    return ok;
}

static void service_queue_recovery_reapply(const char* reason, DWORD delayMs) {
    if (!g_serviceHasActiveDesired) {
        debug_log("recovery reapply: not queued%s%s (no active desired settings)\n",
            reason && reason[0] ? " after " : "",
            reason && reason[0] ? reason : "");
        return;
    }
    // RC8d: defense-in-depth crash loop detection — if the active desired
    // was cleared between the outer check (Phase D) and this call, bail.
    if (count_recent_driver_recoveries() >= MAX_RECOVERIES_BEFORE_BACKOFF) {
        debug_log("recovery reapply: crash loop detected (%u recoveries in %u ms), not queuing\n",
            count_recent_driver_recoveries(), (unsigned int)RECOVERY_LOOP_WINDOW_MS);
        return;
    }
    ULONGLONG now = GetTickCount64();
    // RC7: do NOT reset g_serviceRecoveryReapplyAttempts when pending was 0.
    // See below for why.
    InterlockedExchange(&g_serviceRecoveryReapplyPending, 1);
    g_serviceRecoveryReapplyNextTickMs = now + delayMs;
    service_write_restart_reapply_snapshot();
    debug_log("recovery reapply: queued%s%s nextInMs=%lu attempts=%lu\n",
        reason && reason[0] ? " after " : "",
        reason && reason[0] ? reason : "",
        (unsigned long)delayMs,
        (unsigned long)g_serviceRecoveryReapplyAttempts);
}

// Dedicated thread for recovery reapply.  Runs on its own thread instead of the
// main service loop so that a VEH crash (NVML/NvAPI access-violation on a still-
// transitional driver) kills only this thread, not the service-main loop.
static DWORD WINAPI service_reapply_thread_proc(void*) {
    DWORD myTid = GetCurrentThreadId();
    InterlockedExchange((volatile LONG*)&g_serviceReapplyThreadId, (LONG)myTid);
    debug_log("reapply thread: started (tid=%lu)\n", myTid);

    // Clear the reapply-pending flag immediately — it means "a reapply is needed",
    // not "a reapply is currently running".  The per-attempt accounting below
    // prevents endless loops.
    InterlockedExchange(&g_serviceRecoveryReapplyPending, 0);

    // Set in-progress flag so the GUI shows "reapplying..." instead of stale state.
    InterlockedExchange(&g_serviceReapplyInProgress, 1);

    debug_log("reapply thread: acquiring runtime lock\n");
    lock_service_runtime();
    debug_log("reapply thread: runtime lock acquired\n");

    if (g_app.deviceRemoved) {
        debug_log("reapply thread: device removed, aborting\n");
        unlock_service_runtime();
        InterlockedExchange(&g_serviceInitInProgress, 0);
        InterlockedExchange(&g_serviceReapplyInProgress, 0);
        InterlockedExchange((volatile LONG*)&g_serviceReapplyThreadId, 0);
        return 1;
    }

    DesiredSettings desired = {};
    GpuAdapterInfo targetGpu = {};
    char source[64] = {};
    if (!service_choose_recovery_reapply_desired(&desired, &targetGpu, source, sizeof(source))) {
        debug_log("reapply thread: no desired settings found, aborting\n");
        g_serviceRecoveryReapplyAttempts = 0;
        g_serviceRecoveryReapplyNextTickMs = 0;
        unlock_service_runtime();
        InterlockedExchange(&g_serviceInitInProgress, 0);
        InterlockedExchange(&g_serviceReapplyInProgress, 0);
        InterlockedExchange((volatile LONG*)&g_serviceReapplyThreadId, 0);
        return 1;
    }

    debug_log("reapply thread: attempting to reapply settings from %s\n"
        "  intent: gpuOffset=%d memOffset=%d powerLimit=%d%% fanMode=%d lockCi=%d lockMHz=%u\n",
        source[0] ? source : "active desired",
        desired.hasGpuOffset ? desired.gpuOffsetMHz : -1,
        desired.hasMemOffset ? desired.memOffsetMHz : -1,
        desired.hasPowerLimit ? desired.powerLimitPct : -1,
        desired.hasFan ? desired.fanMode : -1,
        desired.hasLock ? desired.lockCi : -1,
        desired.hasLock ? desired.lockMHz : 0u);

    // RC7: set g_serviceInitInProgress so nvml_ensure_ready() bypasses the
    // nvml_crash_recovery_active() guard.  Without this, the reapply of a
    // previous recovery cycle runs during a NEWER recovery's 15s safety window
    // and gets blocked with "NVML not ready" at nvml_ensure_ready line 762.
    InterlockedExchange(&g_serviceInitInProgress, 1);

    // F-REL-2c: GUARANTEE the configured OC/fan are reapplied once ANY driver is
    // active — never give up just because no driver is present yet.  Gate the
    // apply on real driver readiness: "driver not ready yet" (a clean
    // hardware_initialize failure, e.g. NvAPI not initialized after a prolonged
    // GPU disable, or no driver installed at all) is NOT an apply failure, so we
    // retry it INDEFINITELY at a modest interval WITHOUT consuming the bounded
    // attempt budget.  The bounded budget below is reserved for the dangerous
    // case (driver IS ready but the apply repeatedly fails/crashes), which the
    // restart-loop dormant breaker also guards.  Logging is throttled so an
    // indefinite no-driver wait cannot flood the log.
    {
        char hwDetail[160] = {};
        bool driverReady = hardware_initialize(hwDetail, sizeof(hwDetail)) && g_app.numPopulated > 0;
        if (!driverReady) {
            InterlockedExchange(&g_serviceInitInProgress, 0);
            unlock_service_runtime();
            InterlockedExchange(&g_serviceRecoveryReapplyPending, 1); // stay queued
            g_serviceRecoveryReapplyNextTickMs = GetTickCount64() + SERVICE_RECOVERY_REAPPLY_RETRY_INTERVAL_MS;
            static unsigned int s_notReadyPolls = 0;
            if ((s_notReadyPolls++ % 12u) == 0u) {
                debug_log("reapply thread: GPU driver not ready yet (%s) — waiting (NOT counted against the retry budget); the configured OC/fan WILL be reapplied once a driver is active\n",
                    hwDetail[0] ? hwDetail : "hardware init failed");
            }
            InterlockedExchange(&g_serviceReapplyInProgress, 0);
            InterlockedExchange((volatile LONG*)&g_serviceReapplyThreadId, 0);
            return 0;
        }
    }

    if (!targetGpu.valid) {
        debug_log("reapply thread: no target GPU identity for %s; skipping recovery reapply\n",
            source[0] ? source : "active desired");
        service_clear_restart_reapply_snapshot();
        g_serviceHasActiveDesired = false;
        memset(&g_serviceActiveDesired, 0, sizeof(g_serviceActiveDesired));
        memset(&g_serviceActiveDesiredGpu, 0, sizeof(g_serviceActiveDesiredGpu));
        unlock_service_runtime();
        InterlockedExchange(&g_serviceInitInProgress, 0);
        InterlockedExchange(&g_serviceReapplyInProgress, 0);
        InterlockedExchange((volatile LONG*)&g_serviceReapplyThreadId, 0);
        return 0;
    }
    {
        char targetErr[256] = {};
        if (!service_select_restart_reapply_gpu(&targetGpu, targetErr, sizeof(targetErr))) {
            debug_log("reapply thread: target GPU unavailable for %s; NOT reapplying: %s\n",
                source[0] ? source : "active desired",
                targetErr[0] ? targetErr : "unknown");
            service_clear_restart_reapply_snapshot();
            g_serviceHasActiveDesired = false;
            memset(&g_serviceActiveDesired, 0, sizeof(g_serviceActiveDesired));
            memset(&g_serviceActiveDesiredGpu, 0, sizeof(g_serviceActiveDesiredGpu));
            unlock_service_runtime();
            InterlockedExchange(&g_serviceInitInProgress, 0);
            InterlockedExchange(&g_serviceReapplyInProgress, 0);
            InterlockedExchange((volatile LONG*)&g_serviceReapplyThreadId, 0);
            return 0;
        }
    }

    char result[256] = {};
    bool ok = service_reapply_desired_preserving_intent(&desired, "recovery reapply", result, sizeof(result));
    InterlockedExchange(&g_serviceInitInProgress, 0);

    if (ok) {
        debug_log("reapply thread: apply SUCCEEDED: %s\n", result[0] ? result : "(no detail)");
        g_serviceRecoveryReapplyAttempts = 0;
        g_serviceRecoveryReapplyNextTickMs = 0;
        service_clear_restart_reapply_snapshot();
        InterlockedExchange(&g_serviceReapplyInProgress, 0);
    } else {
        debug_log("reapply thread: apply FAILED: %s\n", result[0] ? result : "(no detail)");
        // Log per-component rollup for quick diagnosis.  result contains
        // semicolon-separated failures like "Memory offset ... was not
        // accepted by the driver; Fan control change failed: ..."
        {
            char summary[512] = {};
            bool hasMemFail = strstr(result, "Memory offset") != nullptr;
            bool hasFanFail = strstr(result, "Fan control") != nullptr;
            bool hasGpuFail = strstr(result, "GPU offset") != nullptr;
            bool hasPowerFail = strstr(result, "Power limit") != nullptr;
            bool hasVfFail = strstr(result, "VF curve") != nullptr;
            bool hasLockFail = strstr(result, "Lock restore") != nullptr;
            bool hasBaselineFail = strstr(result, "reset OC baseline") != nullptr;
            StringCchPrintfA(summary, sizeof(summary),
                "  reapply components: vfCurve=%s memOffset=%s fan=%s gpuOffset=%s powerLimit=%s lock=%s baselineReset=%s",
                desired.hasLock && !hasVfFail ? "ok" : (hasVfFail ? "FAIL" : "-"),
                desired.hasMemOffset && !hasMemFail ? "ok" : (hasMemFail ? "FAIL" : "-"),
                desired.hasFan && !hasFanFail ? "ok" : (hasFanFail ? "FAIL" : "-"),
                desired.hasGpuOffset && !hasGpuFail ? "ok" : (hasGpuFail ? "FAIL" : "-"),
                desired.hasPowerLimit && !hasPowerFail ? "ok" : (hasPowerFail ? "FAIL" : "-"),
                desired.hasLock && !hasLockFail ? "ok" : (hasLockFail ? "FAIL" : "-"),
                !hasBaselineFail ? "ok" : "FAIL");
            debug_log("reapply thread: per-component result:\n%s\n", summary);
        }
        // Keep the disk snapshot as a safety net — do NOT clear it.
        // Don't clear g_serviceReapplyInProgress — the monitor will retry.
    }

    debug_log("reapply thread: releasing runtime lock\n");
    unlock_service_runtime();
    debug_log("reapply thread: runtime lock released\n");

    if (!ok) {
        // Schedule the next retry via the pending flag + next-tick timestamp.
        // The main-loop monitor (service_check_reapply_thread_health) will
        // launch a new reapply thread when the time is right.
        InterlockedExchange(&g_serviceRecoveryReapplyPending, 1);
        g_serviceRecoveryReapplyAttempts++;
        if (g_serviceRecoveryReapplyAttempts < SERVICE_RECOVERY_REAPPLY_MAX_ATTEMPTS) {
            // Use adaptive delay based on the type of failure.
            // nvml_getTemperature succeeds early (basic NVML path), but
            // NvAPI memory offset writes and NVML fan control writes require
            // the driver's clock/fan control subsystems — these take longer to
            // initialize after a device reconnect.  Check the result string for
            // telltale failure patterns and use a much longer delay (30 s) when
            // the driver is operational but still rejecting advanced writes.
            DWORD nextDelay = SERVICE_RECOVERY_REAPPLY_RETRY_INTERVAL_MS;
            if (strstr(result, "was not accepted by the driver") ||
                strstr(result, "Failed to verify the initial")) {
                // Driver control subsystems not ready yet — wait 15 s so
                // the driver has time to initialize its clock/fan paths.
                const DWORD SUBSYSTEM_WEDGE_TIMEOUT_MS = 15000UL;
                nextDelay = SUBSYSTEM_WEDGE_TIMEOUT_MS;
                debug_log("reapply thread: driver control subsystem rejected write, using %lu ms delay\n",
                    (unsigned long)nextDelay);
            } else {
                // Poll basic driver readiness via NVML temperature read.
                // NOTE: we intentionally do NOT SEH-protect this call because
                // the reapply thread runs on its own stack — if it crashes
                // (AV on a transitional driver), the VEH kills only this
                // thread (ExitThread), not the service main loop.  The
                // main-loop monitor detects the VEH-kill via exit code + the
                // in-progress flag and retries.  This is acceptable because
                // the reapply thread is expendable and gets recreated.
                bool driverResponsive = false;
                if (nvml_ensure_ready() && g_nvml_api.getTemperature) {
                    unsigned int temp = 0;
                    if (g_nvml_api.getTemperature(g_app.nvmlDevice, NVML_TEMPERATURE_GPU, &temp) == NVML_SUCCESS) {
                        driverResponsive = true;
                    }
                }
                if (driverResponsive) {
                    nextDelay = 1000UL;
                }
            }
            g_serviceRecoveryReapplyNextTickMs = GetTickCount64() + nextDelay;
            debug_log("reapply thread: retry attempt %lu scheduled in %lu ms\n",
                (unsigned long)g_serviceRecoveryReapplyAttempts,
                (unsigned long)nextDelay);
        } else {
            debug_log("reapply thread: giving up after %lu attempts; clearing active desired, disk snapshot preserved for manual retry\n",
                (unsigned long)g_serviceRecoveryReapplyAttempts);
            InterlockedExchange(&g_serviceRecoveryReapplyPending, 0);
            g_serviceRecoveryReapplyNextTickMs = 0;
            // RC7: clear active desired so the GUI does not falsely report
            // settings as active when the reapply permanently failed.
            InterlockedExchange(&g_serviceReapplyInProgress, 0);
            g_serviceHasActiveDesired = false;
            memset(&g_serviceActiveDesired, 0, sizeof(g_serviceActiveDesired));
            memset(&g_serviceActiveDesiredGpu, 0, sizeof(g_serviceActiveDesiredGpu));
            // Keep the disk snapshot for a future service restart or manual reapply.
        }
    }

    debug_log("reapply thread: exiting (tid=%lu, ok=%d)\n", myTid, ok ? 1 : 0);
    InterlockedExchange((volatile LONG*)&g_serviceReapplyThreadId, 0);
    return ok ? 0 : 1;
}

static void service_maybe_launch_recovery_reapply_thread() {
    // Check if a reapply is needed (pending flag set and not already running)
    if (InterlockedExchangeAdd(&g_serviceRecoveryReapplyPending, 0) == 0) return;

    // Don't launch while GPU recovery is in progress
    if (InterlockedExchangeAdd(&g_serviceGpuRecovering, 0) != 0) return;

    // Don't launch during crash recovery window
    if (nvml_crash_recovery_active()) return;

    // Check timing
    ULONGLONG now = GetTickCount64();
    if (g_serviceRecoveryReapplyNextTickMs != 0 && now < g_serviceRecoveryReapplyNextTickMs) return;

    // Don't stack multiple reapply threads
    if (InterlockedExchangeAdd(&g_serviceReapplyThreadId, 0) != 0) {
        debug_log("recovery reapply: previous reapply thread still active (tid=%lu), skipping\n",
            (unsigned long)g_serviceReapplyThreadId);
        return;
    }

    // Clean up stale handle from previous reapply thread
    HANDLE prevHandle = (HANDLE)InterlockedExchangePointer(
        (PVOID volatile*)&g_serviceReapplyThread, nullptr);
    if (prevHandle) {
        DWORD prevResult = WaitForSingleObject(prevHandle, 0);
        if (prevResult == WAIT_OBJECT_0 || prevResult == WAIT_FAILED) {
            CloseHandle(prevHandle);
        } else {
            // Thread still alive — put handle back and return
            InterlockedExchangePointer((PVOID volatile*)&g_serviceReapplyThread, prevHandle);
            debug_log("recovery reapply: previous thread handle still active, skipping\n");
            return;
        }
    }

    // Check attempt limit here too (in case the pending flag was set from an
    // earlier retry and the thread died before incrementing attempts)
    if (g_serviceRecoveryReapplyAttempts >= SERVICE_RECOVERY_REAPPLY_MAX_ATTEMPTS) {
        debug_log("recovery reapply: max attempts (%lu) reached, clearing active desired (settings could not be reapplied)\n",
            (unsigned long)g_serviceRecoveryReapplyAttempts);
        InterlockedExchange(&g_serviceRecoveryReapplyPending, 0);
        g_serviceRecoveryReapplyNextTickMs = 0;
        InterlockedExchange(&g_serviceReapplyInProgress, 0);
        // RC7: clear the active desired so the GUI does not falsely report
        // settings as active after the reapply gave up.  The profile on disk
        // is still available for manual retry.
        g_serviceHasActiveDesired = false;
        memset(&g_serviceActiveDesired, 0, sizeof(g_serviceActiveDesired));
        memset(&g_serviceActiveDesiredGpu, 0, sizeof(g_serviceActiveDesiredGpu));
        return;
    }

    // RC7: set g_serviceReapplyInProgress BEFORE CreateThread so the fan
    // pulse can check it and extend the cooldown window instead of touching
    // NVML and crashing.  The thread proc also sets it (harmless duplicate),
    // but by then the race window is already closed.
    InterlockedExchange(&g_serviceReapplyInProgress, 1);
    debug_log("recovery reapply: launching dedicated reapply thread (attempt %lu)\n",
        (unsigned long)(g_serviceRecoveryReapplyAttempts + 1));
    DWORD tid = 0;
    HANDLE h = CreateThread(nullptr, 0, service_reapply_thread_proc, nullptr, 0, &tid);
    if (h) {
        g_serviceReapplyThread = h;
        InterlockedExchange((volatile LONG*)&g_serviceReapplyThreadId, (LONG)tid);
        debug_log("recovery reapply: dedicated thread launched (tid=%lu)\n", tid);
    } else {
        debug_log("recovery reapply: CreateThread FAILED (error=%lu)\n", GetLastError());
        InterlockedExchange(&g_serviceReapplyInProgress, 0);
    }
}

// Called from the main service loop each iteration to check if the reapply
// thread has completed and needs cleanup, or if a new one needs launching.
static void service_check_reapply_thread_health() {
    // First: check if we need to launch a new reapply thread (no thread
    // currently running, pending flag is set, timing is right).
    if (g_serviceReapplyThread == nullptr) {
        service_maybe_launch_recovery_reapply_thread();
        return;
    }

    // A thread handle exists.  Check whether it's still alive.
    // Use non-destructive check: snapshot the handle under volatile read
    // (we own this slot, only we write to it and the VEH writes to it
    // when killing the thread; a stale read just delays one cycle).
    HANDLE threadHandle = g_serviceReapplyThread;
    if (!threadHandle) {
        service_maybe_launch_recovery_reapply_thread();
        return;
    }

    DWORD waitResult = WaitForSingleObject(threadHandle, 0);
    if (waitResult == WAIT_OBJECT_0) {
        // Thread completed.  Atomically claim and close the handle.
        HANDLE toClose = (HANDLE)InterlockedExchangePointer(
            (PVOID volatile*)&g_serviceReapplyThread, nullptr);
        if (toClose) {
            DWORD exitCode = 0;
            GetExitCodeThread(toClose, &exitCode);
            CloseHandle(toClose);
            debug_log("recovery reapply: reapply thread exited (exitCode=%lu)\n", exitCode);
            // RC8c: exitCode == 0 can be either a successful apply OR a
            // VEH-killed thread (the VEH redirects to ExitThread(0) which
            // passes exit code 0).  Distinguish them by checking whether
            // g_serviceReapplyInProgress is still set — the success path
            // (service_reapply_thread_proc) clears it explicitly at
            // line 251; a VEH-killed thread is killed before reaching
            // that line, leaving it set.  Treat a VEH-killed thread
            // identically to a failed thread: re-queue the reapply.
            bool wasVehKilled = false;
            if (exitCode == 0) {
                if (InterlockedExchangeAdd(&g_serviceReapplyInProgress, 0)) {
                    wasVehKilled = true;
                    debug_log("recovery reapply: reapply thread was VEH-killed (exitCode=0 but inProgress still set), re-queuing\n");
                } else {
                    InterlockedExchange(&g_serviceReapplyInProgress, 0);
                }
            }
            if ((exitCode != 0 || wasVehKilled) &&
                g_serviceRecoveryReapplyAttempts < SERVICE_RECOVERY_REAPPLY_MAX_ATTEMPTS) {
                // Thread failed or was VEH-killed before it could schedule a retry.
                // If the pending flag is 0 and in-progress is still set, re-queue.
                bool shouldRetry = wasVehKilled ||
                    (InterlockedExchangeAdd(&g_serviceReapplyInProgress, 0) &&
                     InterlockedExchangeAdd(&g_serviceRecoveryReapplyPending, 0) == 0);
                if (shouldRetry) {
                    debug_log("recovery reapply: reapply thread failed/VEH-killed without scheduling retry, re-queuing (attempt %lu)\n",
                        (unsigned long)(g_serviceRecoveryReapplyAttempts + 1));
                    g_serviceRecoveryReapplyAttempts++;
                    if (g_serviceRecoveryReapplyAttempts < SERVICE_RECOVERY_REAPPLY_MAX_ATTEMPTS) {
                        InterlockedExchange(&g_serviceRecoveryReapplyPending, 1);
                        g_serviceRecoveryReapplyNextTickMs = GetTickCount64() + SERVICE_RECOVERY_REAPPLY_RETRY_INTERVAL_MS;
                    } else {
                        debug_log("recovery reapply: max attempts (%lu) reached after thread exit, clearing active desired\n",
                            (unsigned long)g_serviceRecoveryReapplyAttempts);
                        InterlockedExchange(&g_serviceReapplyInProgress, 0);
                        g_serviceHasActiveDesired = false;
                        memset(&g_serviceActiveDesired, 0, sizeof(g_serviceActiveDesired));
                        memset(&g_serviceActiveDesiredGpu, 0, sizeof(g_serviceActiveDesiredGpu));
                    }
                }
            }
        }
        InterlockedExchange((volatile LONG*)&g_serviceReapplyThreadId, 0);
        // Check for retry via next launch
        service_maybe_launch_recovery_reapply_thread();
    } else if (waitResult == WAIT_TIMEOUT) {
        // Thread still running — nothing to do on this iteration.
    } else {
        // WAIT_FAILED — treat as dead.  Atomically claim and close.
        debug_log("recovery reapply: thread handle wait failed (result=%lu), cleaning up\n", waitResult);
        HANDLE toClose = (HANDLE)InterlockedExchangePointer(
            (PVOID volatile*)&g_serviceReapplyThread, nullptr);
        if (toClose) CloseHandle(toClose);
        InterlockedExchange((volatile LONG*)&g_serviceReapplyThreadId, 0);
        InterlockedExchange(&g_serviceReapplyInProgress, 0);
        if (InterlockedExchangeAdd(&g_serviceRecoveryReapplyPending, 0) != 0) {
            g_serviceRecoveryReapplyAttempts++;
        }
        service_maybe_launch_recovery_reapply_thread();
    }
}

// Forward decl: defined below alongside the on-disk version helpers.
static bool service_get_module_version(HMODULE mod, DWORD* outMs, DWORD* outLs);

static void service_log_startup_coordinator_state(const char* phase) {
    EnterCriticalSection(&g_appLock);
    debug_log("service startup coordinator: %s final activeDesired=%d gpu=%d exclude=%d mem=%d power=%d fanMode=%d lockCi=%d lockMHz=%u reapplyPending=%ld reapplyInProgress=%ld\n",
        phase && phase[0] ? phase : "state",
        g_serviceHasActiveDesired ? 1 : 0,
        (g_serviceHasActiveDesired && g_serviceActiveDesired.hasGpuOffset) ? g_serviceActiveDesired.gpuOffsetMHz : 0,
        (g_serviceHasActiveDesired && g_serviceActiveDesired.hasGpuOffset) ? g_serviceActiveDesired.gpuOffsetExcludeLowCount : 0,
        (g_serviceHasActiveDesired && g_serviceActiveDesired.hasMemOffset) ? g_serviceActiveDesired.memOffsetMHz : 0,
        (g_serviceHasActiveDesired && g_serviceActiveDesired.hasPowerLimit) ? g_serviceActiveDesired.powerLimitPct : 0,
        (g_serviceHasActiveDesired && g_serviceActiveDesired.hasFan) ? g_serviceActiveDesired.fanMode : -1,
        (g_serviceHasActiveDesired && g_serviceActiveDesired.hasLock) ? g_serviceActiveDesired.lockCi : -1,
        (g_serviceHasActiveDesired && g_serviceActiveDesired.hasLock) ? g_serviceActiveDesired.lockMHz : 0u,
        (long)InterlockedExchangeAdd(&g_serviceRecoveryReapplyPending, 0),
        (long)InterlockedExchangeAdd(&g_serviceReapplyInProgress, 0));
    LeaveCriticalSection(&g_appLock);
}

// Startup coordinator: if a restart-reapply snapshot exists (because a GPU device
// reconnect / driver upgrade / TDR restarted the service process), wait for the
// GPU driver to be ready, re-apply the saved OC/fan settings, then delete the
// snapshot.  This is the restore half of the restart-based recovery: a fresh
// process maps clean driver DLLs and re-applies the profile.  If no snapshot
// exists, startup is intentionally non-mutating: installing, repairing, or
// manually starting the background service must not apply a saved logon profile
// or reset hardware behind the user's back.  Real logon/session-change events
// still apply the active user's configured profile, and explicit GUI/CLI
// apply/reset commands still write immediately.
static DWORD WINAPI service_startup_coordinator_thread_proc(void*) {
    DesiredSettings desired = {};
    GpuAdapterInfo targetGpu = {};
    bool haveSnapshot = service_load_restart_reapply_snapshot(&desired, &targetGpu);
    unsigned int recentRestarts = service_count_recent_restarts();
    debug_log("service startup coordinator: boot check - recoveryRestart=%d recentRestarts=%u/%u within %llu ms\n",
        haveSnapshot ? 1 : 0, recentRestarts, (unsigned int)SERVICE_RESTART_LOOP_THRESHOLD,
        (unsigned long long)SERVICE_RESTART_LOOP_WINDOW_MS);
    if (!haveSnapshot) {
        // No restart snapshot means this is either a clean boot, a controlled
        // stop+start, or an unexpected termination (Task Manager kill).  In the
        // unexpected-kill case, the GPU may still have OC settings applied from
        // the previous process that was killed before it could reset them.
        // Detect this by reading live GPU state; if stale OC settings are
        // present, reset to driver defaults.
        debug_log("service startup coordinator: no restart snapshot; checking for stale GPU OC settings\n");
        bool needsCleanup = false;
        lock_service_runtime();
        char hwDetail[160] = {};
        bool hwOk = hardware_initialize(hwDetail, sizeof(hwDetail)) && g_app.numPopulated > 0;
        if (hwOk) {
            if (g_app.gpuClockOffsetkHz != 0 || g_app.memClockOffsetkHz != 0 ||
                g_app.powerLimitPct != 100 || !g_app.fanIsAuto) {
                needsCleanup = true;
            }
        }
        if (needsCleanup) {
            char resetDetail[256] = {};
            bool resetOk = service_reset_all(resetDetail, sizeof(resetDetail));
            debug_log("service startup coordinator: stale GPU OC settings detected, %s: %s\n",
                resetOk ? "reset complete" : "reset FAILED",
                resetDetail[0] ? resetDetail : "(no detail)");
        } else {
            debug_log("service startup coordinator: no stale GPU OC settings found%s%s\n",
                hwOk ? "" : " (hardware init: ",
                hwOk ? "" : (hwDetail[0] ? hwDetail : "failed"));
        }
        unlock_service_runtime();
        service_log_startup_coordinator_state("no-snapshot idle");
        return 0;
    }
    // OC stabilization window: if the user applied OC settings less than
    // SERVICE_OC_STABILIZATION_WINDOW_MS ago and the service has now crash-restarted,
    // the just-applied settings are very likely UNSTABLE (they did not survive their
    // proving period).  Do NOT auto-reapply them — drop them so the GPU stays at
    // stock and the user can reconfigure.  This catches the FIRST crash (faster than
    // the 5-in-5-min dormant breaker), and by clearing BOTH the disk snapshot and the
    // in-memory active desired it also stops the in-process (standby-resume) reapply
    // path from restoring the suspect profile.  A stable OC that already ran past the
    // window is unaffected (its stamp is older than the window), so a later driver
    // event still reapplies it.  Saved profile slots on disk are untouched.
    if (service_oc_within_stabilization_window()) {
        debug_log("service startup reapply: service restarted within the %llu ms OC stabilization window — "
            "treating the just-applied settings as UNSTABLE; NOT reapplying. Dropping the active profile; "
            "re-apply from the GUI after verifying/adjusting.\n",
            (unsigned long long)SERVICE_OC_STABILIZATION_WINDOW_MS);
        service_clear_restart_reapply_snapshot();
        service_clear_restart_history();
        service_clear_oc_apply_stamp();
        InterlockedExchange(&g_serviceRecoveryReapplyPending, 0);
        g_serviceRecoveryReapplyNextTickMs = 0;
        g_serviceHasActiveDesired = false;
        memset(&g_serviceActiveDesired, 0, sizeof(g_serviceActiveDesired));
        memset(&g_serviceActiveDesiredGpu, 0, sizeof(g_serviceActiveDesiredGpu));
        service_log_startup_coordinator_state("unstable snapshot dropped");
        return 0;
    }
    // F-REL-2: restart-loop breaker — go DORMANT instead of giving up.
    // Re-applying is what crashes on a transitional/broken driver, so when too
    // many restarts pile up in the window we simply do NOT reapply this round.
    // Crucially we RETAIN the snapshot (the user's profile is not discarded) and
    // we do NOT clear the restart history, so the loop stays broken until either
    //   (a) the 5-min window naturally ages the recent restarts out (the driver
    //       settled and the dormant process stopped crashing), or
    //   (b) a genuine device arrival / driver (re)install clears the history to
    //       re-arm reapply on the next fresh process (see DBT_DEVICEARRIVAL).
    // This avoids both the old infinite snapshot->apply->crash->restart loop and
    // the old behavior of permanently discarding the user's profile.
    if (recentRestarts >= SERVICE_RESTART_LOOP_THRESHOLD) {
        debug_log("service startup reapply: RESTART LOOP detected (%u restarts within %llu ms) — "
            "going DORMANT (snapshot retained, not reapplied). Auto-reapply resumes once the restart "
            "window clears or a driver install/arrival re-arms it; you can also apply manually.\n",
            recentRestarts, (unsigned long long)SERVICE_RESTART_LOOP_WINDOW_MS);
        service_log_startup_coordinator_state("restart-loop dormant");
        return 0;
    }
    debug_log("service startup reapply: restart snapshot found, waiting for GPU driver readiness\n");
    bool ready = false;
    // Wait for the driver to finish initializing.  A quick TDR/reconnect
    // (e.g. restart64.exe) is ready almost immediately, but re-enabling a GPU in
    // Device Manager after a prolonged disable can take well over a minute before
    // NvAPI/NVML accept calls (observed ~90 s, nvapi_init=-6 throughout).  This is
    // a wait-for-resource poll (no readiness event exists), not a race bandaid;
    // the lock is released between polls so the fan runtime / pipe server run.
    const int kReadyMaxAttempts = 240; // up to ~120 s (240 * 500 ms)
    for (int attempt = 0; attempt < kReadyMaxAttempts; attempt++) {
        char detail[160] = {};
        lock_service_runtime();
        bool hw = hardware_initialize(detail, sizeof(detail));
        bool populated = g_app.numPopulated > 0;
        unlock_service_runtime();
        if (hw && populated) {
            ready = true;
            debug_log("service startup reapply: GPU driver ready on attempt %d (populated=%d)\n",
                attempt + 1, g_app.numPopulated);
            break;
        }
        Sleep(500);
    }
    if (!ready) {
        // Do NOT abandon the profile.  Hand off to the main-loop reapply worker so
        // the running service keeps retrying as the driver finishes coming up
        // (covers a driver-ready time beyond the initial wait).  The worker bails
        // on !g_serviceHasActiveDesired, so promote the loaded disk snapshot to the
        // in-memory active desired first (mirrors the recovery-thread hand-off).
        debug_log("service startup reapply: GPU driver not ready within the initial wait; deferring to the "
            "main-loop reapply worker (snapshot retained) so it reapplies once the driver becomes ready\n");
        lock_service_runtime();
        if (!g_serviceHasActiveDesired) {
            g_serviceActiveDesired = desired;
            g_serviceActiveDesiredGpu = targetGpu;
            g_serviceHasActiveDesired = true;
        }
        unlock_service_runtime();
        service_queue_recovery_reapply("startup reapply: driver not ready in initial wait",
            SERVICE_RECOVERY_REAPPLY_RETRY_INTERVAL_MS);
        service_log_startup_coordinator_state("snapshot deferred");
        return 0;
    }
    // Log the freshly loaded driver DLL versions for post-mortem correlation
    // with the version delta recorded at the recovery trigger.
    {
        DWORD ms = 0, ls = 0;
        if (g_nvml && service_get_module_version(g_nvml, &ms, &ls)) {
            debug_log("service startup reapply: loaded NVML version %u.%u.%u.%u\n",
                (unsigned)(ms >> 16), (unsigned)(ms & 0xFFFFu),
                (unsigned)(ls >> 16), (unsigned)(ls & 0xFFFFu));
        }
        ms = 0; ls = 0;
        if (g_app.hNvApi && service_get_module_version(g_app.hNvApi, &ms, &ls)) {
            debug_log("service startup reapply: loaded NvAPI version %u.%u.%u.%u\n",
                (unsigned)(ms >> 16), (unsigned)(ms & 0xFFFFu),
                (unsigned)(ls >> 16), (unsigned)(ls & 0xFFFFu));
        }
    }
    // Settle briefly so the driver is fully past its transient post-restart state.
    Sleep(500);
    lock_service_runtime();
    {
        char targetErr[256] = {};
        if (!service_select_restart_reapply_gpu(&targetGpu, targetErr, sizeof(targetErr))) {
            debug_log("service startup reapply: target GPU unavailable; NOT reapplying snapshot: %s\n",
                targetErr[0] ? targetErr : "unknown");
            service_clear_restart_reapply_snapshot();
            service_clear_restart_history();
            g_serviceHasActiveDesired = false;
            memset(&g_serviceActiveDesired, 0, sizeof(g_serviceActiveDesired));
            memset(&g_serviceActiveDesiredGpu, 0, sizeof(g_serviceActiveDesiredGpu));
            unlock_service_runtime();
            service_log_startup_coordinator_state("snapshot target unavailable");
            return 0;
        }
    }
    desired.resetOcBeforeApply = true;
    char result[256] = {};
    bool ok = service_reapply_desired_preserving_intent(&desired, "startup reapply", result, sizeof(result));
    unlock_service_runtime();
    debug_log("service startup reapply: %s%s%s\n",
        ok ? "applied successfully" : "FAILED",
        result[0] ? ": " : "",
        result[0] ? result : "");
    if (ok) {
        service_clear_restart_reapply_snapshot();
        // Driver is stable again; clear the restart-loop history so unrelated
        // future events start from a clean count.
        service_clear_restart_history();
    } else {
        service_queue_recovery_reapply("startup reapply failure", SERVICE_RECOVERY_REAPPLY_RETRY_INTERVAL_MS);
    }
    service_log_startup_coordinator_state(ok ? "snapshot applied" : "snapshot apply queued");
    return 0;
}

static void service_launch_startup_coordinator() {
    HANDLE h = CreateThread(nullptr, 0, service_startup_coordinator_thread_proc, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    else debug_log("service startup coordinator CreateThread FAILED (error=%lu)\n", GetLastError());
}
