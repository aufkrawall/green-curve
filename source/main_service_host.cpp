// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// SCM control handling, startup readiness, watchdog, and shutdown policy.

#include "service_wedge_watchdog_policy.h"

// How long the SCM should expect each pre-RUNNING step to take.  Generous on
// purpose: a wait hint is an upper bound the SCM measures progress against, not
// a delay, and the steps behind it include DACL rewrites and a SAM round trip
// on a domain-joined machine.
static const DWORD SERVICE_START_WAIT_HINT_MS = 20000;
static const DWORD SERVICE_STOP_WAIT_HINT_MS = 20000;

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
    // Same omission as the start path: without a hint the SCM has no stated
    // budget for a shutdown that tears down NVML, the pipe pool and the fan
    // runtime, and a caller cannot tell "stopping" from "stuck".
    g_serviceStatus.dwCheckPoint++;
    g_serviceStatus.dwWaitHint = SERVICE_STOP_WAIT_HINT_MS;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
    if (g_serviceStopEvent) SetEvent(g_serviceStopEvent);
    return NO_ERROR;
}

// Publish "still starting, here is how far" to the SCM.
//
// THE DEFECT THIS EXISTS FOR (audit 2026-09-22): dwWaitHint and dwCheckPoint
// were assigned NOWHERE in the tree.  g_serviceStatus is zero-initialized, so
// START_PENDING went out once with waitHint 0 and checkpoint 0 and nothing
// moved until RUNNING, ~200 lines and several filesystem/DACL/SAM operations
// later.  A wait hint of 0 satisfies the SCM's hung-service heuristic
// immediately, so NOTHING -- not the SCM, not `sc`, not Services.msc, not our
// own installer's state wait -- could tell a service that was merely slow from
// one that had hung.  "The service did not respond to the start or control
// request in a timely fashion" is the single most recognizable Windows service
// failure, and we were emitting the condition for it by omission.
//
// Each call bumps the checkpoint, which is the part that actually says
// "progress happened"; the hint only says how long to allow before the next one.
static void service_report_start_progress(const char* stage) {
    if (!g_serviceStatusHandle) return;
    g_serviceStatus.dwCurrentState = SERVICE_START_PENDING;
    g_serviceStatus.dwControlsAccepted = 0;
    g_serviceStatus.dwCheckPoint++;
    g_serviceStatus.dwWaitHint = SERVICE_START_WAIT_HINT_MS;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
    debug_log("service_main: start progress checkpoint=%lu stage=%s\n",
        (unsigned long)g_serviceStatus.dwCheckPoint, stage ? stage : "(unnamed)");
}

// The stop-side twin.  STOP_PENDING used to go out ONCE, from the control
// handler, and nothing moved until STOPPED -- through pipe-pool join, fan
// runtime shutdown, a possible GPU reset and NVML teardown.  A waiter that
// follows the checkpoint protocol (service_scm_wait_policy.h: our own install
// path now does) could not tell that apart from a hang.  Each teardown stage
// bumps the checkpoint.  Controls are withdrawn: nothing may be accepted while
// the process is going away, which also keeps the control handler from
// writing g_serviceStatus concurrently with this thread.
static void service_report_stop_progress(const char* stage) {
    if (!g_serviceStatusHandle) return;
    g_serviceStatus.dwCurrentState = SERVICE_STOP_PENDING;
    g_serviceStatus.dwControlsAccepted = 0;
    g_serviceStatus.dwCheckPoint++;
    g_serviceStatus.dwWaitHint = SERVICE_STOP_WAIT_HINT_MS;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
    debug_log("service_main: stop progress checkpoint=%lu stage=%s\n",
        (unsigned long)g_serviceStatus.dwCheckPoint, stage ? stage : "(unnamed)");
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
    // Unhandled-exception filter + fast-fail reporter, plus the vectored handler
    // that catches nvml.dll access violations (driver restart without device
    // removal notification) and lets the fan runtime thread survive.
    install_crash_handlers(true);

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
    g_serviceStatus.dwCheckPoint = 1;
    g_serviceStatus.dwWaitHint = SERVICE_START_WAIT_HINT_MS;
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
    if (!service_initialize_state_identity()) {
        debug_log("service_main: FATAL could not generate protocol-v12 service instance identity\n");
        // Terminal state: drop any START_PENDING progress bookkeeping so a
        // failed start is not advertised as still making progress.
        g_serviceStatus.dwCheckPoint = 0;
        g_serviceStatus.dwWaitHint = 0;
        g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
        g_serviceStatus.dwWin32ExitCode = ERROR_GEN_FAILURE;
        SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
        return;
    }
    debug_log("service state: initialized instance=%llu gpuGeneration=1 phase=STARTING\n",
        (unsigned long long)g_serviceInstanceId);

    // F-SEC-5 / policy: SENSITIVE program artifacts (crash dumps, service logs,
    // restart/reapply state) live only under SYSTEM %LOCALAPPDATA%\Green Curve.
    // Clear any such legacy artifacts older builds left world-readable in
    // %ProgramData%\Green Curve (the deliberately-shared shared-profiles.ini is
    // preserved — see service_cleanup_legacy_programdata).
    service_report_start_progress("legacy artifact cleanup");
    service_cleanup_legacy_programdata();
    service_cleanup_obsolete_recovery_artifacts();
    // This is the sole persisted-replay gate and runs synchronously while the
    // service is START_PENDING. Ordinary/failure-action/Task-Manager restarts
    // clear stale state and remain non-mutating.
    service_prepare_controlled_recovery_startup(argc, argv);
    // Startup never replays; it only decides here (file I/O, no hardware)
    // whether a previous instance died owning GPU state that must be handed
    // back once.  The lifecycle worker performs it after RUNNING.
    service_report_start_progress("ownership handback decision");
    service_ownership_handback_prepare_at_startup(
        g_serviceControlledRecoveryValidated);
    // One-time migration of the shared profile bank from the legacy
    // machine.ini-next-to-binary location to %ProgramData%\Green Curve.  Runs as
    // LocalSystem so it can write %ProgramData% and apply the protected DACL.
    service_report_start_progress("machine config migration");
    migrate_legacy_machine_config();
    // Harden the %ProgramData% shared bank at boot (before any interactive login)
    // so a standard user cannot pre-create and squat the directory/file.
    secure_shared_bank_at_startup();
    // The service directory's protection verdict used to be gathered HERE, on
    // the critical path to RUNNING, for a log line nothing reads synchronously.
    // Its first call builds the admin trust set: a LoadLibrary plus a
    // NetLocalGroupGetMembers round trip through SAM. Purely diagnostic work
    // must not be able to delay -- or on a wedged SAM, prevent -- the service
    // reaching RUNNING, so it now runs after RUNNING is published.
    // Read the update policy from that same protected machine-scope file.  It
    // is loaded AFTER the bank is hardened, so the value that decides whether
    // this service makes outbound requests is never read from a file a standard
    // user could still have been able to write.
    service_update_load_settings();
    // And restore what the LAST check found, by re-verifying the manifest it
    // cached rather than by trusting a stored conclusion.  Without this a
    // restart forgets that an update exists while `last_check` survives, so the
    // next automatic check stays up to a full interval away and every UI
    // surface goes quiet in the meantime -- for an update that may already be
    // downloaded, verified and sitting in the staging directory.
    service_update_restore_from_cache();
    // F-REL-2: bound the on-disk crash artifacts so a restart loop cannot fill
    // the disk (runs once per fresh process = once per restart cycle).  Sweeps
    // the terminal crash dumps and the append-only breadcrumb as well as the VEH
    // dumps, each against its own budget.
    rotate_crash_artifacts_for_process();
    service_report_start_progress("crash artifact rotation");

    if (!ensure_service_runtime_lock()) {
        DWORD lockError = GetLastError();
        debug_log("service_main: FATAL failed to create runtime serialization mutex (error=%lu)\n",
            (unsigned long)lockError);
        g_serviceStatus.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
        g_serviceStatus.dwServiceSpecificExitCode =
            lockError ? lockError : ERROR_NOT_ENOUGH_MEMORY;
        // Terminal state: drop any START_PENDING progress bookkeeping so a
        // failed start is not advertised as still making progress.
        g_serviceStatus.dwCheckPoint = 0;
        g_serviceStatus.dwWaitHint = 0;
        g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
        return;
    }
    g_serviceStopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    // The retired pre-read experiment's wake/recycle events are gone: the
    // broad transition-safe ACL is never rebuilt, and the stop event alone
    // wakes every pipe worker's connect wait.
    g_servicePipeReadyEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!g_serviceStopEvent || !g_servicePipeReadyEvent) {
        debug_log("service_main: FATAL failed to create required service events (error=%lu)\n",
            GetLastError());
        // Terminal state: drop any START_PENDING progress bookkeeping so a
        // failed start is not advertised as still making progress.
        g_serviceStatus.dwCheckPoint = 0;
        g_serviceStatus.dwWaitHint = 0;
        g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
        return;
    }

    service_report_start_progress("lifecycle worker startup");
    char lifecycleErr[256] = {};
    if (!service_start_lifecycle_worker(lifecycleErr, sizeof(lifecycleErr))) {
        debug_log("service_main: FATAL lifecycle worker startup failed: %s\n",
            lifecycleErr[0] ? lifecycleErr : "unknown");
        if (g_serviceStopEvent) SetEvent(g_serviceStopEvent);
        service_shutdown_logon_apply_coordinator();
        g_serviceStatus.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
        g_serviceStatus.dwServiceSpecificExitCode = ERROR_NOT_ENOUGH_MEMORY;
        // Terminal state: drop any START_PENDING progress bookkeeping so a
        // failed start is not advertised as still making progress.
        g_serviceStatus.dwCheckPoint = 0;
        g_serviceStatus.dwWaitHint = 0;
        g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
        return;
    }
    if (g_app.fanCurveRuntimeActive || g_app.fanFixedRuntimeActive) {
        ensure_service_fan_runtime_thread();
    }

    service_report_start_progress("pipe listener startup");
    DWORD threadId = 0;
    (void)threadId;
    if (!service_pipe_listener_start()) {
        debug_log("service_main: FATAL failed to create pipe listener pool\n");
        if (g_serviceStopEvent) SetEvent(g_serviceStopEvent);
        service_shutdown_logon_apply_coordinator();
        stop_service_fan_runtime_thread();
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
        // Terminal state: drop any START_PENDING progress bookkeeping so a
        // failed start is not advertised as still making progress.
        g_serviceStatus.dwCheckPoint = 0;
        g_serviceStatus.dwWaitHint = 0;
        g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
        return;
    }

    HANDLE pipeReadyOrPrimary[2] = { g_servicePipeReadyEvent,
        gc_pipe_listener_primary_thread() };
    DWORD pipeReadyWait = WaitForMultipleObjects(2, pipeReadyOrPrimary, FALSE, INFINITE);
    LONG pipeStartupError = InterlockedExchangeAdd(&g_servicePipeStartupError, 0);
    if (pipeReadyWait != WAIT_OBJECT_0 || pipeStartupError != ERROR_SUCCESS) {
        debug_log("service_main: FATAL pipe listener failed before readiness (wait=%lu error=%ld)\n",
            pipeReadyWait, (long)pipeStartupError);
        if (g_serviceStopEvent) SetEvent(g_serviceStopEvent);
        service_shutdown_logon_apply_coordinator();
        service_pipe_listener_stop_and_join();
        g_serviceStatus.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
        g_serviceStatus.dwServiceSpecificExitCode =
            pipeStartupError == ERROR_SUCCESS ? ERROR_PIPE_NOT_CONNECTED : (DWORD)pipeStartupError;
        // Terminal state: drop any START_PENDING progress bookkeeping so a
        // failed start is not advertised as still making progress.
        g_serviceStatus.dwCheckPoint = 0;
        g_serviceStatus.dwWaitHint = 0;
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
            service_pipe_listener_stop_and_join();
            g_serviceStatus.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
            g_serviceStatus.dwServiceSpecificExitCode = notifyError;
            // Terminal state: drop any START_PENDING progress bookkeeping so a
            // failed start is not advertised as still making progress.
            g_serviceStatus.dwCheckPoint = 0;
            g_serviceStatus.dwWaitHint = 0;
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
    // A running service reports no pending progress; leaving the last
    // START_PENDING checkpoint/hint behind would misreport it as still moving.
    g_serviceStatus.dwCheckPoint = 0;
    g_serviceStatus.dwWaitHint = 0;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);

    // Diagnostic only, and deliberately AFTER RUNNING (see the note at
    // secure_shared_bank_at_startup above): a chain someone loosened since the
    // install must be visible in the support log, but gathering that fact
    // costs a SAM round trip and must never sit between the SCM and RUNNING.
    service_log_path_protection_at_startup();

    // The one startup write: returning what a crashed previous instance still
    // owned.  The worker runs it as soon as the GPU is ready.
    if (InterlockedExchangeAdd(&g_serviceHandbackPending, 0) != 0) {
        service_lifecycle_signal();
    }

    debug_log("service_main: running; hardware writes only on explicit client request, authenticated/WTS logon, standby resume, validated controlled recovery, or the once-per-crash ownership handback\n");

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

            // Automatic update check.  This is a no-op unless the user turned
            // auto-check on AND the interval (or its failure backoff) elapsed,
            // both of which are decided by the pure policy in
            // update_schedule_policy.h against a real clock reading.  Riding the
            // existing watchdog tick rather than owning a timer keeps the
            // service's thread inventory unchanged; the tick's period has no
            // bearing on the check interval, which is measured in wall time.
            service_update_maybe_auto_check();

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
            // Any other hardware work -- an Apply, a Reset, the crash handback --
            // that stops making progress is the same wedge, fan curve or not.
            // Windows cannot see it (a hung thread in a running service is
            // healthy to the SCM), so a fresh process with fresh driver DLLs is
            // requested exactly as for a wedged fan pulse.
            if (InterlockedExchangeAdd(&g_serviceHardwareWorkDepth, 0) > 0) {
                ULONGLONG workProgressAgeMs = service_progress_age_ms(
                    GetTickCount64(), g_serviceHardwareProgressMs);
                if (service_hardware_work_is_wedged(true, workProgressAgeMs)) {
                    const char* label = g_serviceHardwareWorkLabel;
                    debug_log("service_main: hardware work '%s' made no progress for %llu ms"
                              " (limit %llu ms, phase=%s) -- treating it as wedged inside the"
                              " driver and restarting into a fresh process\n",
                        label ? label : "unknown", workProgressAgeMs,
                        SERVICE_HARDWARE_WORK_WEDGE_TIMEOUT_MS, g_lastApplyPhase);
                    service_emergency_restart_from_poisoned_runtime(
                        "hardware work wedged inside the driver", true);
                }
            }
            if (g_serviceFanPulseInFlight && g_serviceFanPulseHeartbeatMs != 0) {
                ULONGLONG nowTickMs = GetTickCount64();
                ULONGLONG stuckMs = nowTickMs - g_serviceFanPulseHeartbeatMs;
                // The pulse stamps its heartbeat BEFORE queuing on the runtime
                // lock, so its age alone cannot tell a hang from a wait.  Require
                // that the hardware gate itself has also stopped moving: a wedge
                // inside nvml.dll stops every stamp, while an apply that is merely
                // slow keeps advancing phases.  Without this second test a normal
                // under-load profile switch was destroying a healthy driver
                // session (2026-09-17).
                ULONGLONG progressAgeMs = g_serviceHardwareProgressMs != 0
                    ? nowTickMs - g_serviceHardwareProgressMs
                    : stuckMs;
                if (stuckMs > SERVICE_FAN_PULSE_WEDGE_TIMEOUT_MS &&
                    progressAgeMs > SERVICE_FAN_PULSE_WEDGE_TIMEOUT_MS) {
                    debug_log("service_main: fan pulse wedged for %llu ms and the hardware gate has not moved for %llu ms — closing the hardware gate and using durable controlled recovery\n",
                        stuckMs, progressAgeMs);
                    service_emergency_restart_from_poisoned_runtime(
                        "fan pulse wedged inside nvml.dll", true);
                } else if (stuckMs > SERVICE_FAN_PULSE_WEDGE_TIMEOUT_MS) {
                    debug_log("service_main: fan pulse waiting %llu ms, but the hardware gate moved %llu ms ago — the gate holder is working, not wedged; not recovering\n",
                        stuckMs, progressAgeMs);
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

            // Check pipe worker health. A VEH-killed worker (stuck in NVML on
            // a transitional driver) has its instance handle reclaimed via the
            // same CAS-slot contract as before, and the slot is respawned.
            service_pipe_listener_reap_and_respawn();
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
            // Terminal state: drop any START_PENDING progress bookkeeping so a
            // finished stop is not advertised as still making progress.
            g_serviceStatus.dwCheckPoint = 0;
            g_serviceStatus.dwWaitHint = 0;
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
    service_report_stop_progress("selected GPU notification and logon coordinator");
    service_stop_selected_gpu_notification_best_effort(
        "graceful service shutdown");
    service_shutdown_logon_apply_coordinator();
    lock_service_runtime();
    bool hadOwnedIntentForShutdown = g_serviceHasActiveDesired;
    unlock_service_runtime();
    service_report_stop_progress("fan runtime shutdown");
    stop_service_fan_runtime_thread();
    service_report_stop_progress("pipe listener shutdown");
    service_pipe_listener_stop_and_join();
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
        service_report_stop_progress("GPU reset of owned intent");
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
        service_report_stop_progress("driver library teardown");
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
    // A terminal state carries no pending progress. Leaving the STOP_PENDING
    // checkpoint and hint behind would advertise a stopped service as still
    // working through a shutdown it already finished.
    g_serviceStatus.dwCheckPoint = 0;
    g_serviceStatus.dwWaitHint = 0;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
    if (g_debug_logging) {
        ULONGLONG elapsedMs = g_debugSessionStartTickMs ? (GetTickCount64() - g_debugSessionStartTickMs) : 0;
        char extra[128] = {};
        StringCchPrintfA(extra, ARRAY_COUNT(extra), "service_main shutdown uptimeMs=%llu", elapsedMs);
        debug_log_session_marker("END", "service", extra);
    }
    debug_log_writer_stop();
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
