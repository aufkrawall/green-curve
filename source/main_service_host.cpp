// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// SCM control handling, startup readiness, watchdog, and shutdown policy.

static DWORD WINAPI service_control_handler_ex(DWORD dwControl, DWORD dwEventType, LPVOID, LPVOID lpEventData) {
    if (dwControl == SERVICE_CONTROL_POWEREVENT) {
        if (dwEventType == PBT_APMSUSPEND) {
            service_lifecycle_post_suspend(dwEventType);
        } else if (dwEventType == PBT_APMRESUMEAUTOMATIC ||
                   dwEventType == PBT_APMRESUMESUSPEND ||
                   dwEventType == PBT_APMRESUMECRITICAL) {
            service_lifecycle_post_resume(dwEventType);
        }
        return NO_ERROR;
    }

    // DBT_DEVNODES_CHANGED is a global notification and normally carries null
    // event data.  It is diagnostic/readiness-only and can never create a
    // driver-recovery authorization by itself.
    if (dwControl == SERVICE_CONTROL_DEVICEEVENT) {
        if (dwEventType == DBT_DEVNODES_CHANGED) {
            service_lifecycle_post_devnodes_changed();
            return NO_ERROR;
        }

        // Interface-specific display-adapter notifications.  The handler only
        // coalesces state; it never allocates a thread or performs a GPU call.
        PDEV_BROADCAST_DEVICEINTERFACEW db_dev = (PDEV_BROADCAST_DEVICEINTERFACEW)lpEventData;
        if (!db_dev) return NO_ERROR;

        switch (dwEventType) {
            case DBT_DEVICEREMOVEPENDING:
            case DBT_DEVICEREMOVECOMPLETE: {
                // This class-wide interface notification may describe another
                // display adapter.  It is a read-only readiness cue only; the
                // Configuration Manager notification registered against the
                // exact selected device instance supplies recovery authority.
                service_lifecycle_post_prerequisite_signal(
                    "unscoped display-interface removal");
                return NO_ERROR;
            }

            case DBT_DEVICEARRIVAL: {
                service_lifecycle_post_prerequisite_signal(
                    "unscoped display-interface arrival");
                return NO_ERROR;
            }

            default:
                break;
        }
        return NO_ERROR;
    }

    if (dwControl == SERVICE_CONTROL_SESSIONCHANGE) {
        // Only WTS_SESSION_LOGON is an authorization event. Logoff cancels the
        // matching incarnation; connect/disconnect/unlock are readiness cues.
        // The lifecycle worker resolves and revalidates the exact session/SID/
        // authentication-LUID tuple before any write. This callback only
        // coalesces the notification and signals that worker.
        WTSSESSION_NOTIFICATION* sn = (WTSSESSION_NOTIFICATION*)lpEventData;
        if (sn) {
            DWORD sessionId = sn->dwSessionId;
            service_lifecycle_post_session_event(dwEventType, sessionId);
        }
        return NO_ERROR;
    }

    if (dwControl != SERVICE_CONTROL_STOP && dwControl != SERVICE_CONTROL_SHUTDOWN) return NO_ERROR;
    // This callback is reached only for an external SCM stop/shutdown.  Keep
    // the control path allocation/file/hardware-free: coalesce the intent and
    // wake service_main, which cancels any helper before it can emit the
    // dedicated controlled-recovery exit code.
    InterlockedExchange(&g_serviceExternalStopRequested, 1);
    g_serviceStatus.dwCurrentState = SERVICE_STOP_PENDING;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
    if (g_serviceStopEvent) SetEvent(g_serviceStopEvent);
    if (g_servicePipeWakeEvent) SetEvent(g_servicePipeWakeEvent);
    return NO_ERROR;
}

static void WINAPI service_main(DWORD argc, LPWSTR* argv) {
    g_app.isServiceProcess = true;
    g_serviceMainThreadId = GetCurrentThreadId();
    InterlockedExchange(&g_serviceRuntimeLockPoisoned, 0);
    InterlockedExchange(&g_serviceRuntimePoisonCorroborated, 0);
    InterlockedExchange(&g_serviceClientRequestsReady, 0);
    bool lifecycleWorkerFailed = false;
    // All ordinary SCM starts are non-mutating, regardless of whether they are
    // boot, install, repair, demand, or failure-action starts. The legacy
    // --manual argument is accepted but intentionally has no policy effect; a
    // validated nonce-bound controlled recovery is the sole startup exception.
    SetUnhandledExceptionFilter(green_curve_unhandled_exception_filter);
    // Vectored handler catches nvml.dll access violations (driver restart without
    // device removal notification) and lets the fan runtime thread survive.
    AddVectoredExceptionHandler(1, green_curve_vectored_handler);

    // Suppress all debug logging until the user's config is read.
    // This guarantees zero file I/O when the user has opted out.
    bool envExplicitlyEnabled = false;
    {
        char debugEnvBuf[16] = {};
        DWORD debugEnvLen = GetEnvironmentVariableA(APP_DEBUG_ENV, debugEnvBuf, ARRAY_COUNT(debugEnvBuf));
        if (debugEnvLen > 0 && !(debugEnvBuf[0] == '0' && debugEnvBuf[1] == '\0')) {
            envExplicitlyEnabled = true;
        }
    }
    g_debug_logging = envExplicitlyEnabled;

    g_serviceStatusHandle = RegisterServiceCtrlHandlerExW(L"GreenCurveService", service_control_handler_ex, nullptr);
    if (!g_serviceStatusHandle) return;

    g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    // Accept no controls while START_PENDING.  The stop event, lifecycle inbox,
    // worker and selected-device subscriptions do not exist yet, so publishing
    // STOP/SESSION/POWER/DEVICE here would acknowledge notifications that can
    // only be dropped.  The complete mask is published atomically with RUNNING.
    g_serviceStatus.dwControlsAccepted = 0;
    g_serviceStatus.dwCurrentState = SERVICE_START_PENDING;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);

    service_resolve_active_user_paths_for_startup("service_main startup");
    if (!envExplicitlyEnabled) {
        refresh_service_debug_logging_from_config();
    }

    if (g_debug_logging) {
        g_debugSessionStartTickMs = GetTickCount64();
        debug_log_session_marker("BEGIN", "service", "service_main bootstrap");
        debug_log_session_marker("BEGIN", "service", "service_main startup");
    }

    // F-SEC-5 / policy: SENSITIVE program artifacts (crash dumps, service logs,
    // restart/reapply state) live only under SYSTEM %LOCALAPPDATA%\Green Curve.
    // Clear any such legacy artifacts older builds left world-readable in
    // %ProgramData%\Green Curve (the deliberately-shared shared-profiles.ini is
    // preserved — see service_cleanup_legacy_programdata).
    service_cleanup_legacy_programdata();
    service_cleanup_obsolete_recovery_artifacts();
    // This is the sole persisted-replay gate and runs synchronously while the
    // service is START_PENDING. Ordinary/failure-action/Task-Manager restarts
    // clear stale state and remain non-mutating.
    service_prepare_controlled_recovery_startup(argc, argv);
    // One-time migration of the shared profile bank from the legacy
    // machine.ini-next-to-binary location to %ProgramData%\Green Curve.  Runs as
    // LocalSystem so it can write %ProgramData% and apply the protected DACL.
    migrate_legacy_machine_config();
    // Harden the %ProgramData% shared bank at boot (before any interactive login)
    // so a standard user cannot pre-create and squat the directory/file.
    secure_shared_bank_at_startup();
    // F-REL-2: bound the on-disk VEH minidumps so a restart loop cannot fill the
    // disk (runs once per fresh process = once per restart cycle).
    service_rotate_minidumps(10);

    if (!ensure_service_runtime_lock()) {
        DWORD lockError = GetLastError();
        debug_log("service_main: FATAL failed to create runtime serialization mutex (error=%lu)\n",
            (unsigned long)lockError);
        g_serviceStatus.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
        g_serviceStatus.dwServiceSpecificExitCode =
            lockError ? lockError : ERROR_NOT_ENOUGH_MEMORY;
        g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
        return;
    }
    g_serviceStopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    g_servicePipeWakeEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    // Auto-reset: each SetEvent forces exactly one pipe-instance recycle so the
    // ACL is rebuilt for the new active user (see service_handle_session_change).
    g_servicePipeRecycleEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    g_servicePipeReadyEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!g_serviceStopEvent || !g_servicePipeWakeEvent ||
        !g_servicePipeRecycleEvent || !g_servicePipeReadyEvent) {
        debug_log("service_main: FATAL failed to create required service events (error=%lu)\n",
            GetLastError());
        g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
        return;
    }

    char lifecycleErr[256] = {};
    if (!service_start_lifecycle_worker(lifecycleErr, sizeof(lifecycleErr))) {
        debug_log("service_main: FATAL lifecycle worker startup failed: %s\n",
            lifecycleErr[0] ? lifecycleErr : "unknown");
        if (g_serviceStopEvent) SetEvent(g_serviceStopEvent);
        service_shutdown_logon_apply_coordinator();
        g_serviceStatus.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
        g_serviceStatus.dwServiceSpecificExitCode = ERROR_NOT_ENOUGH_MEMORY;
        g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
        return;
    }
    if (g_app.fanCurveRuntimeActive || g_app.fanFixedRuntimeActive) {
        ensure_service_fan_runtime_thread();
    }

    DWORD threadId = 0;
    g_servicePipeThread = CreateThread(nullptr, 1024 * 1024, service_pipe_server_thread_proc, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, &threadId);
    if (!g_servicePipeThread) {
        debug_log("service_main: FATAL failed to create pipe server thread (error %lu)\n", GetLastError());
        if (g_serviceStopEvent) SetEvent(g_serviceStopEvent);
        service_shutdown_logon_apply_coordinator();
        stop_service_fan_runtime_thread();
        if (g_servicePipeWakeEvent) {
            CloseHandle(g_servicePipeWakeEvent);
            g_servicePipeWakeEvent = nullptr;
        }
        if (g_servicePipeRecycleEvent) {
            CloseHandle(g_servicePipeRecycleEvent);
            g_servicePipeRecycleEvent = nullptr;
        }
        if (g_serviceFanStopEvent) {
            CloseHandle(g_serviceFanStopEvent);
            g_serviceFanStopEvent = nullptr;
        }
        if (g_serviceStopEvent) {
            CloseHandle(g_serviceStopEvent);
            g_serviceStopEvent = nullptr;
        }
        if (g_serviceRuntimeLock) {
            CloseHandle(g_serviceRuntimeLock);
            g_serviceRuntimeLock = nullptr;
        }
        g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
        return;
    }

    HANDLE pipeReadyOrExited[2] = { g_servicePipeReadyEvent, g_servicePipeThread };
    DWORD pipeReadyWait = WaitForMultipleObjects(2, pipeReadyOrExited, FALSE, INFINITE);
    LONG pipeStartupError = InterlockedExchangeAdd(&g_servicePipeStartupError, 0);
    if (pipeReadyWait != WAIT_OBJECT_0 || pipeStartupError != ERROR_SUCCESS) {
        debug_log("service_main: FATAL pipe listener failed before readiness (wait=%lu error=%ld)\n",
            pipeReadyWait, (long)pipeStartupError);
        if (g_serviceStopEvent) SetEvent(g_serviceStopEvent);
        service_shutdown_logon_apply_coordinator();
        if (g_servicePipeThread) {
            WaitForSingleObject(g_servicePipeThread, INFINITE);
            CloseHandle(g_servicePipeThread);
            g_servicePipeThread = nullptr;
        }
        g_serviceStatus.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
        g_serviceStatus.dwServiceSpecificExitCode =
            pipeStartupError == ERROR_SUCCESS ? ERROR_PIPE_NOT_CONNECTED : (DWORD)pipeStartupError;
        g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
        return;
    }

    // Keep SCM failure actions as an unexpected-crash availability net. They
    // never authorize settings replay; controlled recovery uses its helper.
    service_ensure_failure_actions_configured();
    // F-REL-1: log whether the SCM auto-restart net is actually armed, so a
    // "service never came back after a driver event" report is diagnosable.
    service_verify_restart_safety_net();

    // Register before RUNNING so no selected-display readiness transition can
    // be missed between SCM readiness and lifecycle readiness.
    {
        DEV_BROADCAST_DEVICEINTERFACEW db_dev = {};
        db_dev.dbcc_size = sizeof(db_dev);
        db_dev.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        db_dev.dbcc_reserved = 0;
        db_dev.dbcc_classGuid = GUID_DISPLAY_ADAPTER_DEVINTERFACE;
        g_serviceDeviceNotifyHandle = RegisterDeviceNotificationW(
            g_serviceStatusHandle,
            &db_dev,
            DEVICE_NOTIFY_SERVICE_HANDLE
        );
        if (g_serviceDeviceNotifyHandle) {
            debug_log("device notify: registered for GUID_DEVINTERFACE_DISPLAY_ADAPTER\n");
        } else {
            DWORD notifyError = GetLastError();
            debug_log("service_main: FATAL device notification registration failed (error=%lu)\n",
                notifyError);
            if (g_serviceStopEvent) SetEvent(g_serviceStopEvent);
            service_shutdown_logon_apply_coordinator();
            if (g_servicePipeWakeEvent) SetEvent(g_servicePipeWakeEvent);
            if (g_servicePipeThread) {
                WaitForSingleObject(g_servicePipeThread, INFINITE);
                CloseHandle(g_servicePipeThread);
                g_servicePipeThread = nullptr;
            }
            g_serviceStatus.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
            g_serviceStatus.dwServiceSpecificExitCode = notifyError;
            g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
            SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
            return;
        }
    }

    service_prepare_selected_gpu_notification_before_running();

    // Consume the validated recovery capability into the reducer before any
    // client can observe RUNNING. This closes the startup race where an
    // explicit Apply could be overwritten by a later arm operation.
    service_arm_validated_controlled_recovery();
    InterlockedExchange(&g_serviceClientRequestsReady, 1);
    // SERVICE_ACCEPT_DEVICE_EVENTS is required for display-adapter readiness
    // cues; exact selected-GPU authority comes from the CM notification that is
    // already registered above.
    g_serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP |
        SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_POWEREVENT |
        SERVICE_ACCEPT_DEVICE_EVENTS | SERVICE_ACCEPT_SESSIONCHANGE;
    g_serviceStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);

    debug_log("service_main: running; hardware writes only on explicit client request, authenticated/WTS logon, standby resume, or validated controlled recovery\n");

    // No startup inference or persisted replay occurs here. Fast Startup and
    // autologon are authorized only by a real authenticated scheduled-task
    // handoff (coalesced with WTS logon). Controlled recovery validation, when
    // present, is completed synchronously before this RUNNING transition.
    // Main service loop: wait for stop event, but periodically check if the
    // fan runtime thread or pipe server thread needs restarting (e.g. after a
    // driver-upgrade crash handled by the VEH which calls ExitThread).
service_watchdog_loop:
    if (g_serviceStopEvent) {
        while (true) {
            DWORD wr = WaitForSingleObject(g_serviceStopEvent, SERVICE_FAN_WATCHDOG_INTERVAL_MS);
            if (wr == WAIT_OBJECT_0) break; // stop event signaled

            DWORD lifecycleState = g_serviceLifecycleThread
                ? WaitForSingleObject(g_serviceLifecycleThread, 0)
                : WAIT_OBJECT_0;
            if (lifecycleState != WAIT_TIMEOUT) {
                lifecycleWorkerFailed = true;
                debug_log("service_main: FATAL lifecycle worker died unexpectedly (wait=%lu error=%lu); latching automatic restoration off and stopping for an ordinary non-replaying SCM restart\n",
                    lifecycleState,
                    lifecycleState == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS);
                lock_service_runtime();
                service_disable_automatic_restore(
                    SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED,
                    "long-lived lifecycle worker terminated unexpectedly");
                unlock_service_runtime();
                if (g_serviceStopEvent) SetEvent(g_serviceStopEvent);
                continue;
            }

            // Wedge watchdog: if a fan pulse has been in-flight far longer than
            // any healthy pulse (a driver restart can HANG a read inside nvml.dll
            // — a hang the VEH cannot catch), the fan thread is stuck inside the
            // stale nvml.dll module.  Recover by restarting the process: a fresh
            // process maps clean driver DLLs, and ExitProcess tears down the
            // wedged thread.  Do NOT TerminateThread / close NVML here — racy and
            // unnecessary right before the process exits.
            if (g_serviceFanPulseInFlight && g_serviceFanPulseHeartbeatMs != 0) {
                ULONGLONG stuckMs = GetTickCount64() - g_serviceFanPulseHeartbeatMs;
                if (stuckMs > SERVICE_FAN_PULSE_WEDGE_TIMEOUT_MS) {
                    debug_log("service_main: fan pulse wedged for %llu ms — closing the hardware gate and using durable controlled recovery\n", stuckMs);
                    service_emergency_restart_from_poisoned_runtime(
                        "fan pulse wedged inside nvml.dll", true);
                }
            }

            // If a pipe-server request was the first thread to touch stale
            // NVML/NvAPI after a driver restart, the VEH kills that pipe
            // thread rather than the fan thread.  The main loop must still
            // launch recovery so reset/apply works again even when no fan
            // runtime is active.
            service_maybe_launch_recovery_from_main_loop("main loop");

            // NOTE: there is deliberately NO continuous VF-drift monitor / auto-reapply
            // here (removed in 0.18). NVIDIA's VF curve legitimately shifts a few MHz
            // with temperature/boost; actively "correcting" it meant re-applying the
            // whole OC (reset-to-stock spike + aggressive rewrite) over and over under
            // game load — a TDR risk — and it looped forever whenever the flatten target
            // was below the driver's reachable floor (e.g. 2957 vs a floored 2962). We
            // now LIVE WITH the drift. Settings are re-applied only on real events that
            // actually wipe the OC: resume-from-standby, driver/TDR recovery restart,
            // and session logon (the event-driven reapply worker below/elsewhere).

            // Check fan runtime thread health
            if (g_app.fanCurveRuntimeActive || g_app.fanFixedRuntimeActive) {
                ensure_service_fan_runtime_thread();
            }

            // Check pipe server thread health
            if (g_servicePipeThread && WaitForSingleObject(g_servicePipeThread, 0) == WAIT_OBJECT_0) {
                debug_log("service_main: pipe server thread died, recreating\n");
                // Reclaim the orphaned pipe handle atomically: take the slot to
                // INVALID and close only if we won the real handle, so we never
                // double-close one the dead thread already released (which would
                // hard-crash the process under Strict Handle Checks).
                HANDLE orphanPipe = (HANDLE)InterlockedExchangePointer(
                    (PVOID volatile*)&g_servicePipeHandle, INVALID_HANDLE_VALUE);
                if (orphanPipe != INVALID_HANDLE_VALUE) {
                    CloseHandle(orphanPipe);
                }
                CloseHandle(g_servicePipeThread);
                g_servicePipeThread = nullptr;
                DWORD pipeThreadId = 0;
                g_servicePipeThread = CreateThread(nullptr, 1024 * 1024,
                    service_pipe_server_thread_proc, nullptr,
                    STACK_SIZE_PARAM_IS_A_RESERVATION, &pipeThreadId);
                debug_log("service_main: pipe server thread recreated=%d\n",
                    g_servicePipeThread ? 1 : 0);
            }
        }
    }

    if (InterlockedExchangeAdd(&g_serviceRuntimeLockPoisoned, 0) != 0) {
        service_emergency_restart_from_poisoned_runtime(
            InterlockedExchangeAdd(&g_serviceRuntimePoisonCorroborated, 0) != 0
                ? "VEH-corroborated runtime mutex abandonment"
                : "unexplained runtime mutex abandonment",
            InterlockedExchangeAdd(
                &g_serviceRuntimePoisonCorroborated, 0) != 0);
    }

    // An explicit Apply/Reset may abort a helper after its internal wake was
    // signaled. If it won runtime serialization before the main thread claimed
    // the helper, resume service operation instead of treating that stale event
    // as an external stop.
    if (!lifecycleWorkerFailed &&
        InterlockedExchangeAdd(&g_serviceExternalStopRequested, 0) == 0 &&
        InterlockedExchangeAdd(&g_serviceRestartRequested, 0) == 0) {
        debug_log("service_main: internal restart wake was superseded; resuming watchdog loop\n");
        ResetEvent(g_serviceStopEvent);
        goto service_watchdog_loop;
    }

    InterlockedExchange(&g_serviceClientRequestsReady, 0);

    // Serialize the final helper claim/abort with explicit Apply/Reset. Once
    // this thread owns the runtime lock and claims a live helper, process exit
    // is committed; if an explicit request won first it has already cleared
    // the flags and the watchdog loop above resumes instead.
    lock_service_runtime();
    if (InterlockedExchangeAdd(&g_serviceExternalStopRequested, 0) != 0 &&
        (InterlockedExchangeAdd(&g_serviceRestartRequested, 0) != 0 ||
         InterlockedExchangeAdd(&g_serviceRestartPreparing, 0) != 0 ||
         InterlockedCompareExchangePointer(
             (PVOID volatile*)&g_serviceRestartHelperProcess,
             nullptr, nullptr) != nullptr)) {
        service_abort_controlled_restart(
            "external SCM stop/shutdown superseded controlled recovery");
    }

    // Controlled driver-recovery restart. The helper must still be alive and
    // waiting on our inherited process handle. Report STOP_PENDING and let SCM
    // publish STOPPED only after this dispatcher generation has actually
    // disconnected; the helper subscribes to that authoritative transition
    // before its sole StartServiceW call. Skip NVML teardown, which can hang
    // against the dead/transitional driver.
    if (InterlockedExchangeAdd(&g_serviceRestartRequested, 0) != 0 &&
        InterlockedExchangeAdd(&g_serviceExternalStopRequested, 0) == 0) {
        // Atomically claim the process handle so a racing external stop cannot
        // close it underneath WaitForSingleObject with strict-handle checking.
        HANDLE helper = (HANDLE)InterlockedExchangePointer(
            (PVOID volatile*)&g_serviceRestartHelperProcess, nullptr);
        if (!helper || WaitForSingleObject(helper, 0) != WAIT_TIMEOUT) {
            if (helper) CloseHandle(helper);
            service_abort_controlled_restart(
                "validated helper exited before the service committed its restart");
            g_serviceStatus.dwControlsAccepted = 0;
            g_serviceStatus.dwWin32ExitCode = NO_ERROR;
            g_serviceStatus.dwServiceSpecificExitCode = 0;
            g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
            SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
            ExitProcess(0); // fail closed; do not touch the transitional driver
        } else {
            debug_log("service_main: reporting STOP_PENDING and exiting with dedicated controlled-recovery code\n");
            service_stop_selected_gpu_notification_best_effort(
                "controlled recovery restart");
            if (g_serviceDeviceNotifyHandle) {
                UnregisterDeviceNotification(g_serviceDeviceNotifyHandle);
                g_serviceDeviceNotifyHandle = nullptr;
            }
            if (g_debug_logging) {
                debug_log_session_marker("END", "service",
                    "nonce-bound controlled GPU recovery restart");
            }
            g_serviceStatus.dwControlsAccepted = 0;
            g_serviceStatus.dwWin32ExitCode = NO_ERROR;
            g_serviceStatus.dwServiceSpecificExitCode = 0;
            g_serviceStatus.dwCurrentState = SERVICE_STOP_PENDING;
            SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
            CloseHandle(helper);
            ExitProcess(SERVICE_CONTROLLED_RECOVERY_EXIT_CODE);
        }
    }
    unlock_service_runtime();

    // Ordinary or externally requested shutdown. The long-lived lifecycle
    // worker is the sole owner of pending automatic restoration work.
    service_stop_selected_gpu_notification_best_effort(
        "graceful service shutdown");
    service_shutdown_logon_apply_coordinator();
    lock_service_runtime();
    bool hadOwnedIntentForShutdown = g_serviceHasActiveDesired;
    unlock_service_runtime();
    stop_service_fan_runtime_thread();
    if (g_servicePipeThread) {
        if (g_servicePipeWakeEvent) SetEvent(g_servicePipeWakeEvent);
        if (g_servicePipeRecycleEvent) SetEvent(g_servicePipeRecycleEvent);
        WaitForSingleObject(g_servicePipeThread, INFINITE);
        CloseHandle(g_servicePipeThread);
        g_servicePipeThread = nullptr;
    }
    if (g_servicePipeWakeEvent) {
        CloseHandle(g_servicePipeWakeEvent);
        g_servicePipeWakeEvent = nullptr;
    }
    if (g_servicePipeRecycleEvent) {
        CloseHandle(g_servicePipeRecycleEvent);
        g_servicePipeRecycleEvent = nullptr;
    }
    if (g_servicePipeReadyEvent) {
        CloseHandle(g_servicePipeReadyEvent);
        g_servicePipeReadyEvent = nullptr;
    }
    if (g_serviceFanStopEvent) {
        CloseHandle(g_serviceFanStopEvent);
        g_serviceFanStopEvent = nullptr;
    }
    if (g_serviceStopEvent) {
        CloseHandle(g_serviceStopEvent);
        g_serviceStopEvent = nullptr;
    }
    if (g_serviceRuntimeLock) {
        CloseHandle(g_serviceRuntimeLock);
        g_serviceRuntimeLock = nullptr;
    }
    // Cleanup device notification handle
    if (g_serviceDeviceNotifyHandle) {
        UnregisterDeviceNotification(g_serviceDeviceNotifyHandle);
        g_serviceDeviceNotifyHandle = nullptr;
    }
    // A service that was merely installed/started/repaired owns nothing and
    // must remain completely non-mutating even when that ordinary instance is
    // later stopped.  If this process successfully applied settings, retain
    // the established graceful-stop behavior of returning them to defaults.
    if (hadOwnedIntentForShutdown) {
        char resetDetail[256] = {};
        bool resetWriteAttempted = false;
        bool resetOk = service_reset_all(resetDetail, sizeof(resetDetail),
            &resetWriteAttempted);
        if (!resetOk && resetWriteAttempted) {
            service_latch_auto_restore_lockout(
                SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED,
                "graceful shutdown reset hardware write did not complete");
        }
    } else {
        debug_log("service_main: graceful shutdown has no owned intent; skipping all GPU reset writes\n");
    }
    if (!lifecycleWorkerFailed) {
        close_nvml();
        if (g_app.hNvApi) {
            FreeLibrary(g_app.hNvApi);
            g_app.hNvApi = nullptr;
        }
    } else {
        debug_log("service_main: lifecycle worker failure may involve stale driver DLL state; process exit will reclaim modules without teardown calls\n");
    }
    // External/graceful stops report success. An unexpected lifecycle-worker
    // death reports a service error so the SCM availability net may restart the
    // process, but the sticky lockout and lack of a nonce keep that restart
    // entirely non-mutating.
    g_serviceStatus.dwWin32ExitCode = lifecycleWorkerFailed
        ? ERROR_SERVICE_SPECIFIC_ERROR : NO_ERROR;
    g_serviceStatus.dwServiceSpecificExitCode = lifecycleWorkerFailed
        ? ERROR_PROCESS_ABORTED : 0;
    g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
    if (g_debug_logging) {
        ULONGLONG elapsedMs = g_debugSessionStartTickMs ? (GetTickCount64() - g_debugSessionStartTickMs) : 0;
        char extra[128] = {};
        StringCchPrintfA(extra, ARRAY_COUNT(extra), "service_main shutdown uptimeMs=%llu", elapsedMs);
        debug_log_session_marker("END", "service", extra);
    }
    close_debug_log_file();
    DeleteCriticalSection(&g_debugLogLock);
}

static bool should_suppress_startup_ui() {
    return g_app.launchedFromLogon || g_app.startHiddenToTray;
}

static const char* nvml_err_name(nvmlReturn_t r) {
    switch (r) {
        case NVML_SUCCESS: return "NVML_SUCCESS";
        case NVML_ERROR_UNINITIALIZED: return "NVML_ERROR_UNINITIALIZED";
        case NVML_ERROR_INVALID_ARGUMENT: return "NVML_ERROR_INVALID_ARGUMENT";
        case NVML_ERROR_NOT_SUPPORTED: return "NVML_ERROR_NOT_SUPPORTED";
        case NVML_ERROR_NO_PERMISSION: return "NVML_ERROR_NO_PERMISSION";
        case NVML_ERROR_ALREADY_INITIALIZED: return "NVML_ERROR_ALREADY_INITIALIZED";
        case NVML_ERROR_NOT_FOUND: return "NVML_ERROR_NOT_FOUND";
        case NVML_ERROR_INSUFFICIENT_SIZE: return "NVML_ERROR_INSUFFICIENT_SIZE";
        case NVML_ERROR_FUNCTION_NOT_FOUND: return "NVML_ERROR_FUNCTION_NOT_FOUND";
        case NVML_ERROR_GPU_IS_LOST: return "NVML_ERROR_GPU_IS_LOST";
        case NVML_ERROR_ARG_VERSION_MISMATCH: return "NVML_ERROR_ARGUMENT_VERSION_MISMATCH";
        default: return "NVML_ERROR_OTHER";
    }
}
