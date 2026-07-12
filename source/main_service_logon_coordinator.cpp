// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// ============================================================================
// Long-lived Windows lifecycle worker and cleanup
// ============================================================================
//
// Exactly one worker is created before the service reports RUNNING. Pending
// prerequisites have no arbitrary deadline and are retried only on real
// readiness cues. Once a hardware write is authorized, its result is terminal.

static void service_lifecycle_process_unresolved_logon(
    DWORD sessionId,
    ServiceLifecycleTrigger trigger,
    const char* reason)
{
    DWORD activeSession = (DWORD)-1;
    if (!get_active_interactive_session_id(&activeSession) ||
        activeSession != sessionId) {
        debug_log("lifecycle logon: unresolved session %lu is not active; intent remains pending for a real identity/readiness signal\n",
            (unsigned long)sessionId);
        return;
    }
    ServiceLifecycleIdentity identity = {};
    char err[256] = {};
    if (!service_resolve_session_identity(sessionId, &identity,
            err, sizeof(err))) {
        service_lifecycle_worker_note_identity_not_ready(sessionId, trigger);
        debug_log("lifecycle logon: identity still not ready for session %lu: %s\n",
            (unsigned long)sessionId, err[0] ? err : "unknown");
        return;
    }
    EnterCriticalSection(&g_appLock);
    if (g_serviceLifecycleInbox.unresolvedLogonPending &&
        g_serviceLifecycleInbox.unresolvedLogonSessionId == sessionId) {
        g_serviceLifecycleInbox.unresolvedLogonPending = false;
    }
    LeaveCriticalSection(&g_appLock);
    service_lifecycle_worker_queue_logon(&identity, trigger, reason);
}

static DWORD WINAPI service_lifecycle_thread_proc(void*) {
    SetEvent(g_serviceLifecycleReadyEvent);
    for (;;) {
        HANDLE waits[5] = {
            g_serviceStopEvent,
            g_serviceLifecycleWakeEvent,
            nullptr,
            nullptr,
        };
        DWORD waitCount = 2;
        DWORD userConfigWait = MAXDWORD;
        DWORD machineConfigWait = MAXDWORD;
        DWORD dxgiAdapterWait = MAXDWORD;
        if (g_serviceDxgiAdaptersChangedEvent) {
            dxgiAdapterWait = waitCount;
            waits[waitCount++] = g_serviceDxgiAdaptersChangedEvent;
        }
        if (g_serviceUserConfigChange != INVALID_HANDLE_VALUE) {
            userConfigWait = waitCount;
            waits[waitCount++] = g_serviceUserConfigChange;
        }
        if (g_serviceMachineConfigChange != INVALID_HANDLE_VALUE) {
            machineConfigWait = waitCount;
            waits[waitCount++] = g_serviceMachineConfigChange;
        }
        DWORD wait = WaitForMultipleObjects(waitCount, waits, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) break;
        bool lifecycleWake = wait == WAIT_OBJECT_0 + 1;
        bool configReadinessSignal = false;
        bool dxgiAdapterReadinessSignal = dxgiAdapterWait != MAXDWORD &&
            wait == WAIT_OBJECT_0 + dxgiAdapterWait;
        if (userConfigWait != MAXDWORD &&
            wait == WAIT_OBJECT_0 + userConfigWait) {
            configReadinessSignal =
                service_lifecycle_consume_config_change_if_signaled();
        } else if (machineConfigWait != MAXDWORD &&
            wait == WAIT_OBJECT_0 + machineConfigWait) {
            configReadinessSignal =
                service_lifecycle_consume_config_change_if_signaled();
        } else if (!dxgiAdapterReadinessSignal && wait != WAIT_OBJECT_0 + 1) {
            debug_log("lifecycle worker: wait failed (result=%lu error=%lu)\n",
                wait, GetLastError());
            break;
        }

        ServiceLifecycleInbox inbox = {};
        EnterCriticalSection(&g_appLock);
        inbox = g_serviceLifecycleInbox;
        // An unresolved identity remains stored until resolution or logoff.
        bool preserveUnresolved = g_serviceLifecycleInbox.unresolvedLogonPending;
        DWORD unresolvedSession = g_serviceLifecycleInbox.unresolvedLogonSessionId;
        ServiceLifecycleTrigger unresolvedTrigger =
            g_serviceLifecycleInbox.unresolvedLogonTrigger;
        memset(&g_serviceLifecycleInbox, 0, sizeof(g_serviceLifecycleInbox));
        if (preserveUnresolved) {
            g_serviceLifecycleInbox.unresolvedLogonPending = true;
            g_serviceLifecycleInbox.unresolvedLogonSessionId = unresolvedSession;
            g_serviceLifecycleInbox.unresolvedLogonTrigger = unresolvedTrigger;
        }
        LeaveCriticalSection(&g_appLock);
        if (configReadinessSignal) {
            inbox.prerequisiteSignalPending = true;
            StringCchCopyA(inbox.prerequisiteReason,
                ARRAY_COUNT(inbox.prerequisiteReason),
                "watched config directory changed");
        }
        if (dxgiAdapterReadinessSignal) {
            inbox.prerequisiteSignalPending = true;
            StringCchCopyA(inbox.prerequisiteReason,
                ARRAY_COUNT(inbox.prerequisiteReason),
                "DXGI user-mode adapter set changed");
            debug_log("lifecycle DXGI readiness: adapter-set change signaled\n");
        }

        if (inbox.suspendDiagnosticPending) {
            debug_log("lifecycle power: armed suspend generation=%llu event=0x%08lx\n",
                (unsigned long long)inbox.suspendDiagnosticGeneration,
                (unsigned long)inbox.suspendDiagnosticEventType);
        }
        if (inbox.resumeDiagnosticPending) {
            debug_log("lifecycle power: resume generation=%llu event=0x%08lx wake=%d coalesced=%d\n",
                (unsigned long long)inbox.resumeDiagnosticGeneration,
                (unsigned long)inbox.resumeDiagnosticEventType,
                inbox.resumeDiagnosticWake ? 1 : 0,
                inbox.resumeDiagnosticCoalesced ? 1 : 0);
        }

        if (inbox.logoffPending &&
            (!inbox.sessionEventPending ||
             inbox.sessionEventType != WTS_SESSION_LOGOFF ||
             inbox.sessionEventSessionId != inbox.logoffSessionId)) {
            ServiceLifecycleIdentity identity = {};
            service_logoff_identity_from_cached_state(
                inbox.logoffSessionId, &identity);
            service_lifecycle_worker_reduce_logoff(
                &identity, inbox.logoffSessionId);
        }
        if (inbox.logonSessionEventPending &&
            (!inbox.sessionEventPending ||
             inbox.sessionEventType != WTS_SESSION_LOGON ||
             inbox.sessionEventSessionId !=
                inbox.logonSessionEventSessionId)) {
            service_handle_session_change(WTS_SESSION_LOGON,
                inbox.logonSessionEventSessionId);
        }
        if (inbox.sessionEventPending) {
            service_handle_session_change(inbox.sessionEventType,
                inbox.sessionEventSessionId);
        }
        if (inbox.taskHandoffPending) {
            // The pipe server captured SID + authentication LUID from the
            // authenticated task process before it exited. Queue that immutable
            // identity even if Windows has not marked the session ACTIVE yet;
            // final write authorization performs the active-session recheck.
            service_lifecycle_worker_queue_logon(
                &inbox.taskHandoffIdentity,
                SERVICE_LIFECYCLE_TRIGGER_TASK_HANDOFF,
                "authenticated scheduled-task handoff");
        }
        if (inbox.unresolvedLogonPending &&
            (inbox.prerequisiteSignalPending || inbox.taskHandoffPending ||
             inbox.sessionEventPending ||
             inbox.logonSessionEventPending)) {
            service_lifecycle_process_unresolved_logon(
                inbox.unresolvedLogonSessionId,
                inbox.unresolvedLogonTrigger,
                "identity readiness signal");
        }
        if (inbox.devnodesChangedPending) {
            debug_log("lifecycle PnP: global DEVNODES_CHANGED is a read-only re-enumeration cue; it cannot authorize recovery\n");
        }
        if (inbox.selectedGpuRemovalPending) {
            bool shouldRecord = false;
            lock_service_runtime();
            bool hasIntent = g_serviceHasActiveDesired;
            EnterCriticalSection(&g_appLock);
            if (!g_app.deviceRemoved) {
                g_app.deviceRemoved = true;
                g_app.deviceRemoveTimeMs = GetTickCount64();
                shouldRecord = true;
            }
            // A selected device can disappear while the service is merely
            // running with no owned settings. Preserve diagnostic evidence,
            // but do not create restore work (or a later lockout) without
            // in-memory intent from a successful apply in this process.
            if (hasIntent) {
                ServiceLifecycleEvent event = {};
                event.type = SERVICE_LIFECYCLE_EVENT_DRIVER_RECOVERY;
                event.driverProofReady = false;
                service_lifecycle_reduce_locked(&event);
            }
            LeaveCriticalSection(&g_appLock);
            if (shouldRecord) {
                record_driver_recovery();
                debug_log("lifecycle PnP: corroborated selected-GPU removal; recovery evidence recorded\n");
            }
            unlock_service_runtime();
        }
        if (inbox.selectedGpuArrivalPending) {
            bool wasRemoved = false;
            bool hasIntent = false;
            lock_service_runtime();
            EnterCriticalSection(&g_appLock);
            wasRemoved = g_app.deviceRemoved;
            g_app.deviceRemoved = false;
            hasIntent = g_serviceHasActiveDesired;
            g_app.pendingDeviceRecovery = wasRemoved && hasIntent;
            LeaveCriticalSection(&g_appLock);
            if (wasRemoved) {
                service_check_disk_version_on_device_arrival();
                debug_log("lifecycle PnP: selected GPU re-arrived (activeIntent=%d); confirmed recovery dominates standby\n",
                    hasIntent ? 1 : 0);
                if (hasIntent) {
                    launch_recovery_thread();
                } else {
                    // The exact removal remains counted in the persisted
                    // ledger, but a later, distinct cycle must receive a new
                    // evidence key even though this cycle had nothing to
                    // restore and therefore needed no process restart.
                    service_abandon_current_recovery_evidence();
                }
            } else {
                debug_log("lifecycle PnP: selected-GPU arrival without a corroborated removal; diagnostic/readiness only\n");
            }
            unlock_service_runtime();
        }
        if (inbox.prerequisiteSignalPending) {
            debug_log("lifecycle prerequisite signal: %s\n",
                inbox.prerequisiteReason[0]
                    ? inbox.prerequisiteReason : "unspecified");
        }

        // A directory notification is deliberately broad because the account
        // config can be atomically replaced. Ignore activity for sibling files
        // (most importantly our own debug log) after rearming the watch. Such
        // activity is not a lifecycle readiness event and must not spin the
        // pending-logon resolver.
        bool shouldAttemptLifecycle = lifecycleWake || configReadinessSignal ||
            dxgiAdapterReadinessSignal;
        if (shouldAttemptLifecycle &&
            InterlockedExchangeAdd(&g_serviceRestartRequested, 0) == 0 &&
            InterlockedExchangeAdd(&g_serviceRestartPreparing, 0) == 0 &&
            InterlockedExchangeAdd(&g_serviceExternalStopRequested, 0) == 0) {
            service_lifecycle_attempt_logon();
            service_lifecycle_attempt_standby_restore();
            service_lifecycle_attempt_driver_restore();
        } else if (shouldAttemptLifecycle) {
            debug_log("lifecycle worker: process stop/restart committed; suppressing further hardware authorization\n");
        }
        if (!service_has_pending_logon_apply()) {
            service_lifecycle_close_config_watches();
        }
    }

    EnterCriticalSection(&g_appLock);
    ServiceLifecycleEvent stop = {};
    stop.type = SERVICE_LIFECYCLE_EVENT_STOP;
    service_lifecycle_reduce_locked(&stop);
    LeaveCriticalSection(&g_appLock);
    service_lifecycle_close_config_watches();
    return 0;
}

static bool service_start_lifecycle_worker(char* err, size_t errSize) {
    g_serviceLifecycleWakeEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    g_serviceLifecycleReadyEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!g_serviceLifecycleWakeEvent || !g_serviceLifecycleReadyEvent) {
        set_message(err, errSize, "Could not create lifecycle worker events (error %lu)",
            GetLastError());
        return false;
    }
    memset(&g_serviceLifecycleState, 0, sizeof(g_serviceLifecycleState));
    memset(&g_serviceLifecycleInbox, 0, sizeof(g_serviceLifecycleInbox));
    char dxgiErr[256] = {};
    if (!service_start_dxgi_adapter_readiness(dxgiErr, sizeof(dxgiErr))) {
        debug_log("lifecycle DXGI readiness: unavailable; PnP/client readiness remains active (%s)\n",
            dxgiErr[0] ? dxgiErr : "unknown error");
    }
    g_serviceLifecycleThread = CreateThread(nullptr, 0,
        service_lifecycle_thread_proc, nullptr, 0, nullptr);
    if (!g_serviceLifecycleThread) {
        set_message(err, errSize, "Could not create lifecycle worker (error %lu)",
            GetLastError());
        return false;
    }
    HANDLE readyOrExited[2] = {
        g_serviceLifecycleReadyEvent, g_serviceLifecycleThread
    };
    DWORD wait = WaitForMultipleObjects(2, readyOrExited, FALSE, INFINITE);
    if (wait != WAIT_OBJECT_0) {
        set_message(err, errSize,
            "Lifecycle worker exited before becoming ready (wait=%lu error=%lu)",
            wait, wait == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS);
        return false;
    }
    debug_log("lifecycle worker: ready\n");
    return true;
}

// Historical name retained for cleanup call sites.  This now joins the one
// long-lived lifecycle worker rather than a per-logon retry thread.
static void service_shutdown_logon_apply_coordinator() {
    if (g_serviceStopEvent) SetEvent(g_serviceStopEvent);
    service_lifecycle_signal();
    if (g_serviceLifecycleThread) {
        WaitForSingleObject(g_serviceLifecycleThread, INFINITE);
        CloseHandle(g_serviceLifecycleThread);
        g_serviceLifecycleThread = nullptr;
    }
    if (g_serviceLifecycleWakeEvent) {
        CloseHandle(g_serviceLifecycleWakeEvent);
        g_serviceLifecycleWakeEvent = nullptr;
    }
    if (g_serviceLifecycleReadyEvent) {
        CloseHandle(g_serviceLifecycleReadyEvent);
        g_serviceLifecycleReadyEvent = nullptr;
    }
    service_stop_dxgi_adapter_readiness();
}

// Compatibility wrappers used by readiness/config/PnP call sites.
static void service_signal_pending_logon_retry() {
    service_lifecycle_post_prerequisite_signal("legacy readiness cue");
}

static void service_cancel_pending_logon_apply(const char* reason) {
    service_lifecycle_cancel_automatic_work(reason);
}
