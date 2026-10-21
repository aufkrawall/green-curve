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

// A validated controlled recovery that will not run must not leave its
// provisional intent behind: g_serviceReapplyInProgress keeps the published GPU
// phase at REAPPLYING ("GPU reconnecting") until something clears it, and
// without a write nothing would.  The lockout itself is left as it is.
static void service_drop_provisional_recovery_intent(const char* reason) {
    lock_service_runtime();
    const bool wasReapplying = InterlockedExchange(&g_serviceReapplyInProgress, 0) != 0;
    g_serviceHasActiveDesired = false;
    memset(&g_serviceActiveDesired, 0, sizeof(g_serviceActiveDesired));
    memset(&g_serviceActiveDesiredGpu, 0, sizeof(g_serviceActiveDesiredGpu));
    g_serviceActiveProfileSource = SERVICE_PROFILE_SOURCE_NONE;
    g_serviceActiveProfileSlot = 0;
    unlock_service_runtime();
    debug_log("controlled recovery: provisional intent dropped (%s; wasReapplying=%d)\n",
        reason && reason[0] ? reason : "recovery will not run", wasReapplying ? 1 : 0);
}

// ---- An SCM failure-action start yields to a pending controlled recovery ----
//
// The controlled exit leaves the service in STOP_PENDING, which the SCM counts
// as a failure: 2 s later its failure action starts an ordinary instance.  The
// nonce-bound helper normally starts the recovery instance long before that,
// but when it is late the ordinary instance used to win, delete the
// authorization, hand the GPU back to stock and leave the profile unrestored.
// Now that instance steps aside (service_ordinary_start_yields_to_recovery_helper)
// and the helper waits through it (service_classify_controlled_recovery_scm_stop_state).

#ifndef SERVICE_START_REASON_RESTART_ON_FAILURE
#define SERVICE_START_REASON_RESTART_ON_FAILURE 0x00000008
#endif

static bool service_query_scm_start_reason(DWORD* reasonOut);

// Whether the process (pid, creation time) is still running.
static bool service_process_identity_alive(DWORD pid, ULONGLONG creationTime100ns) {
    if (pid == 0 || creationTime100ns == 0) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!process) return false;
    ULONGLONG created = 0;
    const bool alive = service_query_process_creation_time_100ns(process, &created) &&
        created == creationTime100ns && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
}

// A helper-validated, fresh, same-boot authorization, read without its nonce
// and without consuming it.
static bool service_peek_pending_controlled_recovery(
    ServiceControlledRecoveryAuthorization* out) {
    char path[MAX_PATH] = {};
    if (!out || !service_controlled_recovery_authorization_path(path, sizeof(path))) return false;
    HANDLE file = gc_CreateFileUtf8(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD read = 0, trailingRead = 0;
    BYTE trailing = 0;
    bool ok = ReadFile(file, out, sizeof(*out), &read, nullptr) && read == sizeof(*out) &&
        ReadFile(file, &trailing, sizeof(trailing), &trailingRead, nullptr) && trailingRead == 0;
    CloseHandle(file);
    ServiceBootIdentity boot = {};
    const ULONGLONG now = GetTickCount64();
    return ok && out->magic == SERVICE_CONTROLLED_RECOVERY_MAGIC &&
        out->version == SERVICE_CONTROLLED_RECOVERY_VERSION && out->size == sizeof(*out) &&
        out->helperValidationMagic == SERVICE_CONTROLLED_RECOVERY_HELPER_VALIDATION_MAGIC &&
        service_query_boot_identity(&boot) && service_boot_identity_equal(out->bootIdentity, boot) &&
        out->createdUptimeMs != 0 && out->createdUptimeMs <= now &&
        now - out->createdUptimeMs < SERVICE_CONTROLLED_RECOVERY_MAX_AGE_MS;
}

// Called before the startup gate clears recovery state.  Returns for every
// start except a failure-action start racing a live helper, which it ends.
static void service_yield_to_controlled_recovery_helper_if_pending(DWORD argc, LPWSTR* argv) {
    for (DWORD i = 1; argv && i < argc; ++i) {
        if (argv[i] && _wcsicmp(argv[i], L"--controlled-recovery") == 0) return;
    }
    DWORD startReason = 0;
    const bool restartOnFailure = service_query_scm_start_reason(&startReason) &&
        (startReason & SERVICE_START_REASON_RESTART_ON_FAILURE) != 0;
    if (!restartOnFailure) return;
    ServiceControlledRecoveryAuthorization pending = {};
    const bool authorizationPending = service_peek_pending_controlled_recovery(&pending);
    const bool previousGone = authorizationPending && !service_process_identity_alive(
        pending.previousProcessId, pending.previousProcessCreationTime100ns);
    const bool helperAlive = authorizationPending && service_process_identity_alive(
        pending.helperProcessId, pending.helperProcessCreationTime100ns);
    SecureZeroMemory(&pending, sizeof(pending));
    const bool yield = service_ordinary_start_yields_to_recovery_helper(
        restartOnFailure, authorizationPending, previousGone, helperAlive);
    debug_log("controlled recovery: failure-action start (reason=0x%08lX) pending=%d previousGone=%d helperAlive=%d -> %s\n",
        (unsigned long)startReason, authorizationPending ? 1 : 0, previousGone ? 1 : 0,
        helperAlive ? 1 : 0, yield ? "yielding to the recovery helper" : "ordinary start");
    if (!yield) return;
    debug_log_drain_pending_for_crash();
    g_serviceStatus.dwControlsAccepted = 0;
    g_serviceStatus.dwWin32ExitCode = ERROR_RETRY;
    g_serviceStatus.dwServiceSpecificExitCode = 0;
    g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
    ExitProcess(ERROR_RETRY);
}
