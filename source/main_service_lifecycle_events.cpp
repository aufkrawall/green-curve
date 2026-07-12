// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// ============================================================================
// Lifecycle event inbox and readiness/config-watch posting
// ============================================================================
//
// SCM callbacks and other producers only coalesce compact state and signal the
// long-lived lifecycle worker. No hardware access occurs in this shard.

struct ServiceLifecycleInbox {
    bool sessionEventPending;
    DWORD sessionEventType;
    DWORD sessionEventSessionId;
    // Preserve the latest real LOGON independently from generic readiness or
    // LOGOFF traffic. Without this, LOGON(B) followed by LOGOFF(A) before the
    // worker drains the inbox loses B and defeats the WTS redundancy path.
    bool logonSessionEventPending;
    DWORD logonSessionEventSessionId;
    bool logoffPending;
    DWORD logoffSessionId;
    bool unresolvedLogonPending;
    DWORD unresolvedLogonSessionId;
    ServiceLifecycleTrigger unresolvedLogonTrigger;
    bool taskHandoffPending;
    ServiceLifecycleIdentity taskHandoffIdentity;
    bool prerequisiteSignalPending;
    bool devnodesChangedPending;
    bool selectedGpuRemovalPending;
    bool selectedGpuArrivalPending;
    bool suspendDiagnosticPending;
    bool resumeDiagnosticPending;
    DWORD suspendDiagnosticEventType;
    DWORD resumeDiagnosticEventType;
    gc_u64 suspendDiagnosticGeneration;
    gc_u64 resumeDiagnosticGeneration;
    bool resumeDiagnosticWake;
    bool resumeDiagnosticCoalesced;
    char prerequisiteReason[64];
};

static ServiceLifecycleState g_serviceLifecycleState = {};
static ServiceLifecycleInbox g_serviceLifecycleInbox = {};
static HANDLE g_serviceLifecycleWakeEvent = nullptr;
static HANDLE g_serviceLifecycleReadyEvent = nullptr;
static HANDLE g_serviceLifecycleThread = nullptr;
static HANDLE g_serviceUserConfigChange = INVALID_HANDLE_VALUE;
static HANDLE g_serviceMachineConfigChange = INVALID_HANDLE_VALUE;
static char g_serviceUserConfigWatchDir[MAX_PATH] = {};
static char g_serviceMachineConfigWatchDir[MAX_PATH] = {};
static char g_serviceUserConfigWatchPath[MAX_PATH] = {};
static char g_serviceMachineConfigWatchPath[MAX_PATH] = {};
struct ServiceConfigFileStamp {
    bool present;
    DWORD volumeSerial;
    DWORD fileIndexHigh;
    DWORD fileIndexLow;
    DWORD sizeHigh;
    DWORD sizeLow;
    FILETIME lastWrite;
};
static ServiceConfigFileStamp g_serviceUserConfigStamp = {};
static ServiceConfigFileStamp g_serviceMachineConfigStamp = {};
static volatile LONGLONG g_serviceLogoffEventGeneration = 0;
static volatile LONG g_serviceLastLogoffSessionId = -1;

static bool service_lifecycle_config_parent_directory(
    const char* path, char* directoryOut, size_t directoryOutSize) {
    if (!path || !path[0] || !directoryOut || directoryOutSize == 0 ||
        FAILED(StringCchCopyA(directoryOut, directoryOutSize, path))) {
        return false;
    }
    char* slash = strrchr(directoryOut, '\\');
    if (!slash || slash == directoryOut) return false;
    *slash = 0;
    // The account's Green Curve directory may not exist yet during early
    // profile mount.  Watch the nearest existing ancestor so creation/rename
    // becomes a real readiness signal; the next pass moves the watch inward.
    for (;;) {
        DWORD attributes = GetFileAttributesA(directoryOut);
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return true;
        }
        slash = strrchr(directoryOut, '\\');
        if (!slash || slash <= directoryOut + 2) return false;
        *slash = 0;
    }
}

static void service_lifecycle_close_config_watch(
    HANDLE* handle, char* watchedDirectory, char* watchedPath = nullptr,
    ServiceConfigFileStamp* stamp = nullptr) {
    if (handle && *handle != INVALID_HANDLE_VALUE) {
        FindCloseChangeNotification(*handle);
        *handle = INVALID_HANDLE_VALUE;
    }
    if (watchedDirectory) watchedDirectory[0] = 0;
    if (watchedPath) watchedPath[0] = 0;
    if (stamp) memset(stamp, 0, sizeof(*stamp));
}

static ServiceConfigFileStamp service_lifecycle_config_file_stamp(
    const char* path) {
    ServiceConfigFileStamp stamp = {};
    HANDLE file = CreateFileA(path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return stamp;
    BY_HANDLE_FILE_INFORMATION info = {};
    if (GetFileInformationByHandle(file, &info)) {
        stamp.present = true;
        stamp.volumeSerial = info.dwVolumeSerialNumber;
        stamp.fileIndexHigh = info.nFileIndexHigh;
        stamp.fileIndexLow = info.nFileIndexLow;
        stamp.sizeHigh = info.nFileSizeHigh;
        stamp.sizeLow = info.nFileSizeLow;
        stamp.lastWrite = info.ftLastWriteTime;
    }
    CloseHandle(file);
    return stamp;
}

static bool service_lifecycle_config_stamp_equal(
    const ServiceConfigFileStamp& a, const ServiceConfigFileStamp& b) {
    return a.present == b.present &&
        (!a.present || (a.volumeSerial == b.volumeSerial &&
         a.fileIndexHigh == b.fileIndexHigh && a.fileIndexLow == b.fileIndexLow &&
         a.sizeHigh == b.sizeHigh && a.sizeLow == b.sizeLow &&
         CompareFileTime(&a.lastWrite, &b.lastWrite) == 0));
}

static void service_lifecycle_update_config_watch(const char* configPath,
    HANDLE* handle, char* watchedDirectory, size_t watchedDirectorySize,
    char* watchedPath, size_t watchedPathSize,
    ServiceConfigFileStamp* stamp, const char* label) {
    char directory[MAX_PATH] = {};
    if (!service_lifecycle_config_parent_directory(configPath, directory,
            sizeof(directory))) {
        service_lifecycle_close_config_watch(handle, watchedDirectory,
            watchedPath, stamp);
        debug_log("lifecycle config readiness: cannot watch %s parent yet (%s)\n",
            label, configPath && configPath[0] ? configPath : "<unset>");
        return;
    }
    if (*handle != INVALID_HANDLE_VALUE &&
        _stricmp(watchedDirectory, directory) == 0 &&
        _stricmp(watchedPath, configPath) == 0) return;
    service_lifecycle_close_config_watch(handle, watchedDirectory,
        watchedPath, stamp);
    HANDLE change = FindFirstChangeNotificationA(directory, FALSE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE |
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SECURITY);
    if (change == INVALID_HANDLE_VALUE) {
        debug_log("lifecycle config readiness: FindFirstChangeNotification failed for %s directory %s (error=%lu)\n",
            label, directory, GetLastError());
        return;
    }
    *handle = change;
    StringCchCopyA(watchedDirectory, watchedDirectorySize, directory);
    StringCchCopyA(watchedPath, watchedPathSize, configPath);
    *stamp = service_lifecycle_config_file_stamp(configPath);
    debug_log("lifecycle config readiness: watching %s directory %s\n",
        label, directory);
}

static void service_lifecycle_watch_config_context(
    const ServiceSessionConfigContext* context) {
    if (!context) return;
    service_lifecycle_update_config_watch(context->userConfigPath,
        &g_serviceUserConfigChange, g_serviceUserConfigWatchDir,
        ARRAY_COUNT(g_serviceUserConfigWatchDir), g_serviceUserConfigWatchPath,
        ARRAY_COUNT(g_serviceUserConfigWatchPath), &g_serviceUserConfigStamp,
        "account config");
    if (context->machineConfigPath[0]) {
        service_lifecycle_update_config_watch(context->machineConfigPath,
            &g_serviceMachineConfigChange, g_serviceMachineConfigWatchDir,
            ARRAY_COUNT(g_serviceMachineConfigWatchDir),
            g_serviceMachineConfigWatchPath,
            ARRAY_COUNT(g_serviceMachineConfigWatchPath),
            &g_serviceMachineConfigStamp, "shared config");
    }
}

// Close the read-vs-watch lost-wakeup window.  The worker arms both directory
// notifications before resolving selectors, then samples them immediately
// after the mutex-held read.  If a non-cooperating editor changed either file
// during that interval, rearm the notification and keep the logon intent
// pending for the worker's next pass instead of accepting a stale NONE result.
static bool service_lifecycle_consume_config_change_if_signaled() {
    bool changed = false;
    auto consume = [&](HANDLE* handle, char* watchedDirectory,
                       char* watchedPath, ServiceConfigFileStamp* stamp,
                       const char* label) {
        if (!handle || *handle == INVALID_HANDLE_VALUE) return;
        DWORD wait = WaitForSingleObject(*handle, 0);
        if (wait != WAIT_OBJECT_0) return;
        ServiceConfigFileStamp current =
            service_lifecycle_config_file_stamp(watchedPath);
        bool exactConfigChanged =
            !service_lifecycle_config_stamp_equal(*stamp, current);
        // When the target directory did not exist, the watch was placed on the
        // nearest existing ancestor with subtree=false.  Directory creation can
        // signal before config.ini itself exists; absent->absent is then not an
        // exact-file stamp change, but it must move the watch inward or the later
        // file creation below that new child is lost forever.
        char nearestNow[MAX_PATH] = {};
        bool ancestorProgress =
            service_lifecycle_config_parent_directory(watchedPath,
                nearestNow, ARRAY_COUNT(nearestNow)) &&
            _stricmp(nearestNow, watchedDirectory) != 0;
        char targetPath[MAX_PATH] = {};
        StringCchCopyA(targetPath, ARRAY_COUNT(targetPath), watchedPath);
        *stamp = current;
        changed = changed || exactConfigChanged || ancestorProgress;
        if (!FindNextChangeNotification(*handle)) {
            debug_log("lifecycle config readiness: %s watch rearm failed after read (error=%lu)\n",
                label, GetLastError());
            service_lifecycle_close_config_watch(handle, watchedDirectory,
                watchedPath, stamp);
            service_lifecycle_update_config_watch(targetPath, handle,
                watchedDirectory, MAX_PATH, watchedPath, MAX_PATH, stamp,
                label);
        } else if (ancestorProgress) {
            debug_log("lifecycle config readiness: %s path materialized; moving watch inward from ancestor\n",
                label);
            service_lifecycle_update_config_watch(targetPath, handle,
                watchedDirectory, MAX_PATH, watchedPath, MAX_PATH, stamp,
                label);
        }
    };
    consume(&g_serviceUserConfigChange, g_serviceUserConfigWatchDir,
        g_serviceUserConfigWatchPath, &g_serviceUserConfigStamp, "account");
    consume(&g_serviceMachineConfigChange, g_serviceMachineConfigWatchDir,
        g_serviceMachineConfigWatchPath, &g_serviceMachineConfigStamp, "shared");
    return changed;
}

static void service_lifecycle_close_config_watches() {
    service_lifecycle_close_config_watch(&g_serviceUserConfigChange,
        g_serviceUserConfigWatchDir, g_serviceUserConfigWatchPath,
        &g_serviceUserConfigStamp);
    service_lifecycle_close_config_watch(&g_serviceMachineConfigChange,
        g_serviceMachineConfigWatchDir, g_serviceMachineConfigWatchPath,
        &g_serviceMachineConfigStamp);
}

static void service_lifecycle_set_result_locked(
    ServiceLifecycleTrigger trigger,
    ServiceLifecycleResult result)
{
    if (trigger != SERVICE_LIFECYCLE_TRIGGER_NONE) {
        g_serviceLastLifecycleTrigger = trigger;
    }
    if (result != SERVICE_LIFECYCLE_RESULT_NONE) {
        g_serviceLastLifecycleResult = result;
    }
}

static void service_lifecycle_signal() {
    if (g_serviceLifecycleWakeEvent) SetEvent(g_serviceLifecycleWakeEvent);
}

static bool service_lifecycle_worker_is_alive() {
    return g_serviceLifecycleThread &&
        WaitForSingleObject(g_serviceLifecycleThread, 0) == WAIT_TIMEOUT;
}

// Caller holds g_appLock.  Exact selected-device removal is recovery authority
// from the instant the CM callback posts it, not only after the worker's next
// inbox drain.  This closes the coincident standby/logon/apply window.
static bool service_lifecycle_selected_gpu_recovery_cue_pending_locked() {
    return g_serviceLifecycleInbox.selectedGpuRemovalPending ||
        InterlockedCompareExchange(&g_serviceSelectedGpuRemoved, 0, 0) != 0;
}

static ServiceLifecycleDecision service_lifecycle_reduce_locked(
    const ServiceLifecycleEvent* event)
{
    ServiceLifecycleDecision decision = service_lifecycle_reduce(
        &g_serviceLifecycleState, event);
    service_lifecycle_set_result_locked(decision.trigger, decision.result);
    return decision;
}

static bool service_lifecycle_post_session_event(DWORD eventType, DWORD sessionId) {
    bool accepted = false;
    EnterCriticalSection(&g_appLock);
    if (!g_serviceLifecycleState.stopped &&
        service_lifecycle_worker_is_alive()) {
        if (eventType == WTS_SESSION_LOGOFF) {
            InterlockedExchange(&g_serviceLastLogoffSessionId,
                (LONG)sessionId);
            InterlockedIncrement64(&g_serviceLogoffEventGeneration);
            g_serviceLifecycleInbox.logoffPending = true;
            g_serviceLifecycleInbox.logoffSessionId = sessionId;
        } else if (eventType == WTS_SESSION_LOGON) {
            g_serviceLifecycleInbox.logonSessionEventPending = true;
            g_serviceLifecycleInbox.logonSessionEventSessionId = sessionId;
        }
        g_serviceLifecycleInbox.sessionEventPending = true;
        g_serviceLifecycleInbox.sessionEventType = eventType;
        g_serviceLifecycleInbox.sessionEventSessionId = sessionId;
        accepted = true;
    }
    LeaveCriticalSection(&g_appLock);
    if (accepted) service_lifecycle_signal();
    return accepted;
}

static bool service_lifecycle_post_logon_handoff(
    const ServiceLifecycleIdentity* identity) {
    bool accepted = false;
    EnterCriticalSection(&g_appLock);
    if (!g_serviceLifecycleState.stopped &&
        service_lifecycle_worker_is_alive() && identity && identity->valid) {
        g_serviceLifecycleInbox.taskHandoffPending = true;
        g_serviceLifecycleInbox.taskHandoffIdentity = *identity;
        accepted = true;
    }
    LeaveCriticalSection(&g_appLock);
    if (accepted) service_lifecycle_signal();
    return accepted;
}

static void service_lifecycle_post_prerequisite_signal(const char* reason) {
    EnterCriticalSection(&g_appLock);
    if (!g_serviceLifecycleState.stopped) {
        g_serviceLifecycleInbox.prerequisiteSignalPending = true;
        StringCchCopyA(g_serviceLifecycleInbox.prerequisiteReason,
            ARRAY_COUNT(g_serviceLifecycleInbox.prerequisiteReason),
            reason && reason[0] ? reason : "readiness signal");
    }
    LeaveCriticalSection(&g_appLock);
    service_lifecycle_signal();
}

static void service_lifecycle_post_devnodes_changed() {
    EnterCriticalSection(&g_appLock);
    if (!g_serviceLifecycleState.stopped) {
        ServiceLifecycleEvent event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_DEVNODES_CHANGED;
        ServiceLifecycleDecision decision = service_lifecycle_reduce_locked(&event);
        g_serviceLifecycleInbox.devnodesChangedPending =
            decision.readOnlyReenumerate != 0;
        g_serviceLifecycleInbox.prerequisiteSignalPending = true;
        StringCchCopyA(g_serviceLifecycleInbox.prerequisiteReason,
            ARRAY_COUNT(g_serviceLifecycleInbox.prerequisiteReason),
            "global devnode re-enumeration");
    }
    LeaveCriticalSection(&g_appLock);
    service_lifecycle_signal();
}

static void service_lifecycle_post_selected_gpu_removal() {
    EnterCriticalSection(&g_appLock);
    if (!g_serviceLifecycleState.stopped) {
        g_serviceLifecycleInbox.selectedGpuRemovalPending = true;
    }
    LeaveCriticalSection(&g_appLock);
    service_lifecycle_signal();
}

static void service_lifecycle_post_selected_gpu_arrival() {
    EnterCriticalSection(&g_appLock);
    if (!g_serviceLifecycleState.stopped) {
        g_serviceLifecycleInbox.selectedGpuArrivalPending = true;
    }
    LeaveCriticalSection(&g_appLock);
    service_lifecycle_signal();
}

static void service_lifecycle_post_suspend(DWORD powerEventType) {
    EnterCriticalSection(&g_appLock);
    ServiceLifecycleEvent event = {};
    event.type = SERVICE_LIFECYCLE_EVENT_SUSPEND;
    service_lifecycle_reduce_locked(&event);
    g_serviceLifecycleInbox.suspendDiagnosticPending = true;
    g_serviceLifecycleInbox.suspendDiagnosticEventType = powerEventType;
    g_serviceLifecycleInbox.suspendDiagnosticGeneration =
        g_serviceLifecycleState.suspendGeneration;
    LeaveCriticalSection(&g_appLock);
    service_lifecycle_signal();
}

static void service_lifecycle_post_resume(DWORD powerEventType) {
    bool wake = false;
    EnterCriticalSection(&g_appLock);
    ServiceLifecycleEvent event = {};
    event.type = SERVICE_LIFECYCLE_EVENT_RESUME;
    ServiceLifecycleDecision decision = service_lifecycle_reduce_locked(&event);
    wake = decision.wakeWorker != 0;
    g_serviceLifecycleInbox.resumeDiagnosticPending = true;
    g_serviceLifecycleInbox.resumeDiagnosticEventType = powerEventType;
    g_serviceLifecycleInbox.resumeDiagnosticGeneration =
        g_serviceLifecycleState.resumedSuspendGeneration;
    g_serviceLifecycleInbox.resumeDiagnosticWake = wake;
    g_serviceLifecycleInbox.resumeDiagnosticCoalesced =
        decision.coalesced != 0;
    LeaveCriticalSection(&g_appLock);
    // Wake even when the reducer coalesced the event so file-backed diagnostics
    // remain in the lifecycle worker rather than the SCM control callback.
    service_lifecycle_signal();
}

static bool service_lifecycle_post_validated_driver_recovery() {
    DWORD lockoutReason = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
    ULONGLONG proofAgeMs = 0;
    if (!service_auto_restore_allowed_after_driver_event(
            &lockoutReason, &proofAgeMs)) {
        service_disable_automatic_restore(
            lockoutReason != SERVICE_AUTO_RESTORE_LOCKOUT_NONE
                ? lockoutReason
                : SERVICE_AUTO_RESTORE_LOCKOUT_UNSTABLE_APPLY,
            "controlled driver recovery did not satisfy the awake-time proof");
        EnterCriticalSection(&g_appLock);
        ServiceLifecycleEvent lockout = {};
        lockout.type = SERVICE_LIFECYCLE_EVENT_LOCKOUT;
        service_lifecycle_reduce_locked(&lockout);
        LeaveCriticalSection(&g_appLock);
        return false;
    }
    unsigned int recentRecoveries = service_count_recent_restarts();
    if (recentRecoveries >= SERVICE_RESTART_LOOP_THRESHOLD) {
        service_disable_automatic_restore(
            SERVICE_AUTO_RESTORE_LOCKOUT_TDR_SPAM,
            "persistent recovery ledger reached the spam threshold");
        EnterCriticalSection(&g_appLock);
        ServiceLifecycleEvent lockout = {};
        lockout.type = SERVICE_LIFECYCLE_EVENT_LOCKOUT;
        service_lifecycle_reduce_locked(&lockout);
        LeaveCriticalSection(&g_appLock);
        return false;
    }

    bool wake = false;
    EnterCriticalSection(&g_appLock);
    ServiceLifecycleEvent event = {};
    event.type = SERVICE_LIFECYCLE_EVENT_DRIVER_RECOVERY;
    event.driverProofReady = true;
    ServiceLifecycleDecision decision = service_lifecycle_reduce_locked(&event);
    wake = decision.wakeWorker != 0;
    LeaveCriticalSection(&g_appLock);
    debug_log("lifecycle driver recovery: validated proofAgeMs=%llu ledger=%u/%u wake=%d\n",
        (unsigned long long)proofAgeMs, recentRecoveries,
        (unsigned int)SERVICE_RESTART_LOOP_THRESHOLD, wake ? 1 : 0);
    if (wake) service_lifecycle_signal();
    return wake;
}

static void service_lifecycle_cancel_automatic_work(const char* reason) {
    EnterCriticalSection(&g_appLock);
    ServiceLifecycleEvent event = {};
    event.type = SERVICE_LIFECYCLE_EVENT_EXPLICIT_SUPERSEDE;
    service_lifecycle_reduce_locked(&event);
    memset(&g_serviceLifecycleInbox, 0, sizeof(g_serviceLifecycleInbox));
    LeaveCriticalSection(&g_appLock);
    debug_log("lifecycle: automatic work superseded: %s\n",
        reason && reason[0] ? reason : "explicit request");
    service_lifecycle_signal();
}

static void service_lifecycle_note_explicit_session_supersession(
    DWORD sessionId,
    const char* reason)
{
    ServiceLifecycleIdentity identity = {};
    char identityErr[128] = {};
    if (!service_resolve_session_identity(sessionId, &identity,
            identityErr, sizeof(identityErr))) {
        debug_log("lifecycle: explicit supersession could not cache session identity %lu: %s\n",
            (unsigned long)sessionId,
            identityErr[0] ? identityErr : "unknown");
        return;
    }
    EnterCriticalSection(&g_appLock);
    g_serviceLifecycleState.lastAppliedLogonIdentity = identity;
    service_lifecycle_set_result_locked(
        g_serviceLastLifecycleTrigger,
        SERVICE_LIFECYCLE_RESULT_SUPERSEDED);
    LeaveCriticalSection(&g_appLock);
    debug_log("lifecycle: current login %lu/%s auth=%llu satisfied by %s; later WTS/task duplicates will not auto-apply\n",
        (unsigned long)identity.sessionId, identity.sid,
        (unsigned long long)identity.authenticationId,
        reason && reason[0] ? reason : "explicit action");
}

static bool service_has_pending_logon_apply() {
    EnterCriticalSection(&g_appLock);
    bool pending = g_serviceLifecycleState.logonPending ||
        g_serviceLifecycleInbox.unresolvedLogonPending ||
        g_serviceLifecycleInbox.taskHandoffPending;
    LeaveCriticalSection(&g_appLock);
    return pending;
}

static bool service_logoff_identity_from_cached_state(
    DWORD sessionId,
    ServiceLifecycleIdentity* identityOut)
{
    if (!identityOut) return false;
    memset(identityOut, 0, sizeof(*identityOut));
    EnterCriticalSection(&g_appLock);
    if (g_serviceLifecycleState.logonPending &&
        g_serviceLifecycleState.pendingLogonIdentity.valid &&
        g_serviceLifecycleState.pendingLogonIdentity.sessionId == sessionId) {
        *identityOut = g_serviceLifecycleState.pendingLogonIdentity;
    } else if (g_serviceLifecycleState.lastAppliedLogonIdentity.valid &&
        g_serviceLifecycleState.lastAppliedLogonIdentity.sessionId == sessionId) {
        *identityOut = g_serviceLifecycleState.lastAppliedLogonIdentity;
    }
    LeaveCriticalSection(&g_appLock);
    return identityOut->valid != 0;
}

static void service_lifecycle_worker_reduce_logoff(
    const ServiceLifecycleIdentity* identity,
    DWORD sessionId)
{
    EnterCriticalSection(&g_appLock);
    ServiceLifecycleIdentity effective = {};
    if (identity && identity->valid) effective = *identity;
    if (!effective.valid) {
        if (g_serviceLifecycleState.logonPending &&
            g_serviceLifecycleState.pendingLogonIdentity.sessionId == sessionId) {
            effective = g_serviceLifecycleState.pendingLogonIdentity;
        } else if (g_serviceLifecycleState.lastAppliedLogonIdentity.valid &&
            g_serviceLifecycleState.lastAppliedLogonIdentity.sessionId == sessionId) {
            effective = g_serviceLifecycleState.lastAppliedLogonIdentity;
        }
    }
    if (effective.valid) {
        ServiceLifecycleEvent event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_LOGOFF;
        event.identity = effective;
        service_lifecycle_reduce_locked(&event);
    }
    if (g_serviceLifecycleInbox.unresolvedLogonPending &&
        g_serviceLifecycleInbox.unresolvedLogonSessionId == sessionId) {
        g_serviceLifecycleInbox.unresolvedLogonPending = false;
    }
    LeaveCriticalSection(&g_appLock);
}

static void service_lifecycle_worker_note_identity_not_ready(
    DWORD sessionId,
    ServiceLifecycleTrigger trigger)
{
    EnterCriticalSection(&g_appLock);
    g_serviceLifecycleInbox.unresolvedLogonPending = true;
    g_serviceLifecycleInbox.unresolvedLogonSessionId = sessionId;
    g_serviceLifecycleInbox.unresolvedLogonTrigger = trigger;
    service_lifecycle_set_result_locked(trigger,
        SERVICE_LIFECYCLE_RESULT_TRANSIENT_NOT_READY);
    LeaveCriticalSection(&g_appLock);
}

static void service_lifecycle_worker_queue_logon(
    const ServiceLifecycleIdentity* identity,
    ServiceLifecycleTrigger trigger,
    const char* reason)
{
    if (!identity || !identity->valid) return;
    ServiceLifecycleEvent event = {};
    event.type = trigger == SERVICE_LIFECYCLE_TRIGGER_TASK_HANDOFF
        ? SERVICE_LIFECYCLE_EVENT_TASK_HANDOFF
        : SERVICE_LIFECYCLE_EVENT_WTS_LOGON;
    event.identity = *identity;
    EnterCriticalSection(&g_appLock);
    ServiceLifecycleDecision decision = service_lifecycle_reduce_locked(&event);
    LeaveCriticalSection(&g_appLock);
    debug_log("lifecycle logon: %s session=%lu sid=%s auth=%llu pending=%d coalesced=%d result=%u\n",
        reason && reason[0] ? reason : "logon cue",
        (unsigned long)identity->sessionId, identity->sid,
        (unsigned long long)identity->authenticationId,
        decision.attemptLogonPrerequisites ? 1 : 0,
        decision.coalesced ? 1 : 0, (unsigned int)decision.result);
}
