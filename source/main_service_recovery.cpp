// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Fail-closed automatic-restore state transitions shared by the single
// lifecycle worker and the controlled-restart authorization path.

// Defined in shards the aggregator includes after this one.
static void service_clear_controlled_recovery_files();
static void debug_log_drain_pending_for_crash();

// Stop every autonomous restoration path without touching live hardware. The
// user's saved profile remains available for an explicit GUI/CLI apply, which
// is the only action permitted to acknowledge and clear this safety lockout.
// Callers serialize this with a real hardware write by holding the runtime
// lock; pre-write authorization failures may call it without that lock.
static void service_disable_automatic_restore(DWORD lockoutReason,
    const char* context) {
    service_latch_auto_restore_lockout(lockoutReason, context);
    service_clear_restart_reapply_snapshot();
    service_clear_oc_apply_stamp();
    InterlockedExchange(&g_serviceReapplyInProgress, 0);
    g_app.pendingDeviceRecovery = false;
    g_serviceHasActiveDesired = false;
    memset(&g_serviceActiveDesired, 0, sizeof(g_serviceActiveDesired));
    memset(&g_serviceActiveDesiredGpu, 0, sizeof(g_serviceActiveDesiredGpu));
    g_serviceActiveProfileSource = SERVICE_PROFILE_SOURCE_NONE;
    g_serviceActiveProfileSlot = 0;
    g_app.fanCurveRuntimeActive = false;
    g_app.fanFixedRuntimeActive = false;
    // When the caller already owns the runtime lock, the fan worker is either
    // outside a pulse or waiting for that same lock, and that wait also ends on
    // the stop event.  Signalling is therefore enough: the worker exits after
    // the atomic state transition is published, and this fail-closed path does
    // not block on it.  The worker handle is reaped by the next ensure/stop.
    if (service_runtime_lock_held_by_current_thread()) {
        if (g_serviceFanStopEvent) SetEvent(g_serviceFanStopEvent);
    } else {
        stop_service_fan_runtime_thread();
    }
    debug_log("automatic restore: disabled without a hardware write (%s)\n",
        context && context[0] ? context : "unspecified safety condition");
}

// Whether a settings snapshot for the controlled restart exists on disk.  Used
// by the poisoned-runtime exit, which may not consult the live intent it can no
// longer trust; file system only, no hardware.
static ServiceRestartSnapshotState service_restart_snapshot_state() {
    char path[MAX_PATH] = {};
    if (!service_active_desired_persist_path(path, sizeof(path)))
        return SERVICE_RESTART_SNAPSHOT_UNKNOWN;
    HANDLE h = gc_CreateFileUtf8(path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return SERVICE_RESTART_SNAPSHOT_PRESENT;
    }
    DWORD error = GetLastError();
    return (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
        ? SERVICE_RESTART_SNAPSHOT_ABSENT : SERVICE_RESTART_SNAPSHOT_UNKNOWN;
}

// A corroborated driver failure poisoned the runtime while no settings intent
// existed: restart plainly.  Nothing can be replayed, so automatic restore stays
// armed for the next logon/explicit apply.  Reporting STOPPED with an error lets
// the SCM's non-crash failure actions start a fresh process with fresh driver
// libraries; that start hands back anything a previous write still owned.
[[noreturn]] static void service_exit_poisoned_runtime_for_ordinary_restart(
    const char* reason) {
    service_clear_controlled_recovery_files();
    service_clear_oc_apply_stamp();
    debug_log("controlled recovery: poisoned runtime holds no settings intent (%s); ordinary restart, automatic restore stays armed\n",
        reason && reason[0] ? reason : "corroborated driver failure");
    debug_log_drain_pending_for_crash();
    g_serviceStatus.dwControlsAccepted = 0;
    g_serviceStatus.dwWin32ExitCode = ERROR_PROCESS_ABORTED;
    g_serviceStatus.dwServiceSpecificExitCode = 0;
    g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
    ExitProcess(ERROR_PROCESS_ABORTED);
}

// Decide how a poisoned runtime leaves (service_poisoned_runtime_exit_plan) and
// take the plain-restart exit here; the caller handles the other two plans.
static ServicePoisonedRuntimeExit service_poisoned_runtime_plan_or_exit(
    const char* reason, bool corroboratedDriverFailure) {
    const ServiceRestartSnapshotState snapshot = service_restart_snapshot_state();
    const bool liveIntent = g_serviceHasActiveDesired;
    const ServicePoisonedRuntimeExit plan = service_poisoned_runtime_exit_plan(
        corroboratedDriverFailure, snapshot, liveIntent);
    debug_log("controlled recovery: poisoned runtime exit plan=%d corroborated=%d snapshot=%d liveIntent=%d\n",
        (int)plan, corroboratedDriverFailure ? 1 : 0, (int)snapshot, liveIntent ? 1 : 0);
    if (plan == SERVICE_POISONED_EXIT_ORDINARY_RESTART) {
        service_exit_poisoned_runtime_for_ordinary_restart(reason);
    }
    return plan;
}
