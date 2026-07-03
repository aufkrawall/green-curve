// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// ============================================================================
// Active-user session model (multi-user support)
// ============================================================================
//
// Green Curve controls a single GPU that may be shared by several Windows
// user accounts on the same machine.  Policy: only the currently-ACTIVE
// interactive session may drive the GPU, and its resolved profile is applied
// whenever a real Windows session-change event changes the active identity.
// This preserves the F-SEC-3 security boundary (a *different* local user cannot
// drive OC through the SYSTEM service) while keeping service install/repair/manual
// start non-mutating: starting the service while a user is already logged in is
// not treated as a synthetic logon.  Fast-User-Switching, RDP, and logon events
// still apply the now-active user's configured profile.
//
// This shard is a pure policy/router layer.  It delegates profile resolution
// to service_session_logon_resolve_and_load_profile() and hardware writes to
// service_apply_desired_settings(), both defined in main_service_runtime.cpp.
// All gating is event + identity comparison — no sleeps or timing bandaids.

// Last session we successfully applied a profile for in this process lifetime.
// Used to debounce session-change events: we only re-apply when the resolved
// active session identity actually changes, never on every event.  WTS numeric
// session IDs can be reused after logoff, so the identity is {sessionId, SID}.
static DWORD g_lastAppliedSessionId = (DWORD)-1;
static char g_lastAppliedSessionSid[184] = {};
static ULONGLONG g_lastAppliedUtcMs = 0;

// Auto-reset event the pipe server loop waits on.  Signaling it recycles the
// current listening pipe instance on an active-user change.  The pipe ACL is
// user-agnostic now (active-session enforcement lives server-side in
// service_caller_is_authorized), so this is defensive only: it drops any
// half-open ConnectNamedPipe from the previous active user and hands the next
// connect a clean instance.  Created in service_main.
static HANDLE g_servicePipeRecycleEvent = nullptr;

// Resolve the single "active controlling" session: the physical console
// session if one is active, otherwise the first WTSActive session (covers RDP
// and headless sessions).  Returns false when there is no active interactive
// session at all (e.g. no one logged in).  Thin wrapper so all session-change
// reasoning funnels through one definition of "active".
static bool service_get_active_session_for_control(DWORD* sessionIdOut) {
    return get_active_interactive_session_id(sessionIdOut);
}

static bool service_last_applied_session_matches(DWORD sessionId, const char* sid) {
    return sid && sid[0] &&
        g_lastAppliedSessionId == sessionId &&
        g_lastAppliedSessionSid[0] &&
        strcmp(g_lastAppliedSessionSid, sid) == 0;
}

// Shared per-session apply: resolve the user's effective profile (per-user
// logon_slot, else machine default), wait for the GPU driver to be ready, and
// apply under the service runtime lock.  Captures owner identity on success.
// `reason` is a short label for logging only.  Returns true on a successful
// apply; returns false (without applying) when there is nothing to apply,
// the driver is not ready, or NVML crash recovery is in flight.
static bool service_apply_profile_for_session(DWORD sessionId, const char* reason) {
    if (sessionId == (DWORD)-1) return false;

    // Wait briefly for the GPU driver to be ready for this session.  A fresh
    // logon or a service restart can race with driver setup.  Bounded retry,
    // not a timing bandaid: each attempt actually probes hardware_initialize.
    bool driverReady = false;
    for (int attempt = 0; attempt < 20; attempt++) {
        char detail[256] = {};
        if (hardware_initialize(detail, sizeof(detail)) && g_app.numPopulated > 0 && g_app.loaded) {
            driverReady = true;
            debug_log("session apply%s%s: driver ready on attempt %d for session %lu\n",
                reason && reason[0] ? " (" : "",
                reason && reason[0] ? reason : "",
                attempt + 1, (unsigned long)sessionId);
            break;
        }
        Sleep(500);
    }
    if (!driverReady) {
        debug_log("session apply%s%s: GPU driver not ready after waiting; skipping apply for session %lu\n",
            reason && reason[0] ? " (" : "",
            reason && reason[0] ? reason : "",
            (unsigned long)sessionId);
        return false;
    }

    DesiredSettings desired = {};
    int slot = 0;
    bool usedMachineDefault = false;
    if (!service_session_logon_resolve_and_load_profile(sessionId, &desired, &slot, &usedMachineDefault)) {
        return false;
    }

    lock_service_runtime();
    if (nvml_crash_recovery_active()) {
        debug_log("session apply%s%s: NVML crash recovery active, deferring apply for session %lu\n",
            reason && reason[0] ? " (" : "",
            reason && reason[0] ? reason : "",
            (unsigned long)sessionId);
        unlock_service_runtime();
        return false;
    }

    desired.resetOcBeforeApply = true;
    char result[512] = {};
    debug_log("session apply%s%s: applying %s slot %d for session %lu (gpu=%d mem=%d power=%d fanMode=%d)\n",
        reason && reason[0] ? " (" : "",
        reason && reason[0] ? reason : "",
        usedMachineDefault ? "machine-wide default" : "per-user",
        slot, (unsigned long)sessionId,
        desired.hasGpuOffset ? desired.gpuOffsetMHz : 0,
        desired.hasMemOffset ? desired.memOffsetMHz : 0,
        desired.hasPowerLimit ? desired.powerLimitPct : 0,
        desired.hasFan ? desired.fanMode : -1);
    bool ok = service_apply_desired_settings(&desired, false, result, sizeof(result));
    if (ok) {
        service_capture_owner_identity(usedMachineDefault ? "logon task" : "logon task", sessionId);
        debug_log("session apply%s%s: applied %s slot %d for session %lu: %s\n",
            reason && reason[0] ? " (" : "",
            reason && reason[0] ? reason : "",
            usedMachineDefault ? "machine-wide default" : "per-user",
            slot, (unsigned long)sessionId, result[0] ? result : "ok");
        g_lastAppliedSessionId = sessionId;
        if (g_serviceUserPathsResolved &&
            g_serviceUserPathsSessionId == sessionId &&
            g_serviceUserPathsSid[0]) {
            StringCchCopyA(g_lastAppliedSessionSid, ARRAY_COUNT(g_lastAppliedSessionSid), g_serviceUserPathsSid);
        } else {
            char sidErr[128] = {};
            if (!service_resolve_session_user_sid(sessionId, g_lastAppliedSessionSid, sizeof(g_lastAppliedSessionSid), sidErr, sizeof(sidErr))) {
                g_lastAppliedSessionSid[0] = 0;
                debug_log("session apply%s%s: applied session %lu but could not record user SID for debounce: %s\n",
                    reason && reason[0] ? " (" : "",
                    reason && reason[0] ? reason : "",
                    (unsigned long)sessionId,
                    sidErr[0] ? sidErr : "unknown");
            }
        }
        FILETIME ft = {};
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER uli = {};
        uli.LowPart = ft.dwLowDateTime;
        uli.HighPart = ft.dwHighDateTime;
        g_lastAppliedUtcMs = uli.QuadPart / 10000ULL;
    } else {
        debug_log("session apply%s%s: failed to apply %s slot %d for session %lu: %s\n",
            reason && reason[0] ? " (" : "",
            reason && reason[0] ? reason : "",
            usedMachineDefault ? "machine-wide default" : "per-user",
            slot, (unsigned long)sessionId, result[0] ? result : "unknown");
    }
    unlock_service_runtime();
    return ok;
}

// Pure policy helper (unit-testable): does a given WTS session-change event
// type participate in re-resolving the active controlling session?  Logon,
// logoff, and console connect/disconnect all change who the active user may
// be.  Lock/unlock and remote connect/disconnect of a non-console session do
// NOT change the active controlling user (screen lock is not a user switch;
// remote-control is a session-internal redirect).
static bool session_event_relevant_for_active_user(DWORD eventType) {
    return eventType == WTS_SESSION_LOGON ||
           eventType == WTS_SESSION_LOGOFF ||
           eventType == WTS_CONSOLE_CONNECT ||
           eventType == WTS_CONSOLE_DISCONNECT;
}

// Policy router invoked from the SCM SERVICE_CONTROL_SESSIONCHANGE handler.
// Re-resolves the active session; if its identity differs from the last
// session we applied for, applies that session's profile (or does nothing if
// no active interactive session remains).  Also forces a pipe-instance
// recycle so the ACL is rebuilt for the new active user.  Debounced: events
// that do not change the resolved active session are ignored to avoid apply
// storms.  Runs on a short-lived worker thread spawned by the handler.
static void service_handle_session_change(DWORD eventType, DWORD eventSessionId) {
    const char* evtName = "unknown";
    switch (eventType) {
        case WTS_SESSION_LOGON: evtName = "SESSION_LOGON"; break;
        case WTS_SESSION_LOGOFF: evtName = "SESSION_LOGOFF"; break;
        case WTS_CONSOLE_CONNECT: evtName = "CONSOLE_CONNECT"; break;
        case WTS_CONSOLE_DISCONNECT: evtName = "CONSOLE_DISCONNECT"; break;
        default: evtName = "other"; break;
    }

    if (!session_event_relevant_for_active_user(eventType)) {
        debug_log("session change: %s for session %lu is not relevant for active-user policy, ignoring\n",
            evtName, (unsigned long)eventSessionId);
        return;
    }

    DWORD activeSessionId = (DWORD)-1;
    bool hasActive = service_get_active_session_for_control(&activeSessionId);
    char activeSid[184] = {};
    char sidErr[128] = {};
    bool haveActiveSid = false;
    if (hasActive) {
        haveActiveSid = service_resolve_session_user_sid(activeSessionId, activeSid, sizeof(activeSid), sidErr, sizeof(sidErr));
        if (!haveActiveSid) {
            debug_log("session change: %s for session %lu; active session %lu SID unavailable: %s\n",
                evtName,
                (unsigned long)eventSessionId,
                (unsigned long)activeSessionId,
                sidErr[0] ? sidErr : "unknown");
        }
    }
    debug_log("session change: %s for session %lu; resolved active identity = %s%lu/%s, last applied = %lu/%s\n",
        evtName, (unsigned long)eventSessionId,
        hasActive ? "" : "<none> ",
        hasActive ? (unsigned long)activeSessionId : 0UL,
        (hasActive && haveActiveSid) ? activeSid : "<unknown>",
        (unsigned long)g_lastAppliedSessionId,
        g_lastAppliedSessionSid[0] ? g_lastAppliedSessionSid : "<none>");

    // Recycle the listening pipe instance on the active-user change (defensive:
    // the ACL is user-agnostic, so this only hands the next connect a clean
    // instance; active-session enforcement is server-side).
    if (g_servicePipeRecycleEvent) {
        SetEvent(g_servicePipeRecycleEvent);
    }

    // Debounce: only apply when the resolved active session identity changed.
    if (!hasActive) {
        // No active interactive session remains (e.g. everyone logged off).
        // Do not reset GPU state here — leave the last applied settings in
        // place; a fresh logon will re-resolve and apply.
        return;
    }
    if (!haveActiveSid) {
        return;
    }
    if (service_last_applied_session_matches(activeSessionId, activeSid)) {
        debug_log("session change: active identity %lu/%s unchanged since last apply, skipping re-apply\n",
            (unsigned long)activeSessionId, activeSid);
        return;
    }

    service_apply_profile_for_session(activeSessionId, evtName);
}

// Boot-time safety net for the no-snapshot startup coordinator.  On a boot
// auto-start where a user is ALREADY logged in when the service comes up, the
// live WTS_SESSION_LOGON that normally drives the apply was never delivered to
// us (it fired before service_control_handler_ex registered, or the console
// session was resumed already-active under Windows Fast Startup).  Without this,
// the GPU stays at driver defaults until the user opens the GUI (which applies
// on launch).  Here we resolve+apply that session's logon profile ONCE, reusing
// service_apply_profile_for_session() so behavior is byte-for-byte identical to a
// real logon (including the {session,SID} debounce, so a racing real logon event
// does not double-apply).  Gating is delegated to the pure, unit-tested
// should_reconcile_active_session_at_boot() so manual (--manual) starts stay
// non-mutating and an SCM crash-restart within the same boot cannot re-drive it.
static void service_maybe_reconcile_active_session_at_boot(const char* reason) {
    const char* label = (reason && reason[0]) ? reason : "boot-active-session";

    DWORD activeSessionId = (DWORD)-1;
    bool hasActive = service_get_active_session_for_control(&activeSessionId);

    // Enable the user's debug logging as early as possible so this boot decision
    // and the ensuing apply are captured in the user's log.  Honors the [debug]
    // enabled opt-out (stays silent if the user disabled logging); before this,
    // boot-phase service logging is off because no user config has been read yet.
    if (hasActive) {
        char pathErr[256] = {};
        if (resolve_service_user_data_paths(activeSessionId, pathErr, sizeof(pathErr))) {
            refresh_service_debug_logging_from_config();
        }
    }

    bool manual = g_serviceManualStart;
    bool alreadyDone = service_boot_reconcile_already_done();
    unsigned int recentRestarts = service_count_recent_restarts();
    bool inLoop = recentRestarts >= SERVICE_RESTART_LOOP_THRESHOLD;

    // Best-effort: did the live logon router already apply for this exact
    // identity (it won the race)?  Avoids a redundant second apply.
    bool alreadyApplied = false;
    if (hasActive) {
        char activeSid[184] = {};
        char sidErr[128] = {};
        if (service_resolve_session_user_sid(activeSessionId, activeSid, sizeof(activeSid), sidErr, sizeof(sidErr))) {
            alreadyApplied = service_last_applied_session_matches(activeSessionId, activeSid);
        }
    }

    bool doReconcile = should_reconcile_active_session_at_boot(
        manual, alreadyDone, inLoop, hasActive, alreadyApplied);
    debug_log("boot reconcile decision (%s): activeSession=%s%lu manual=%d alreadyThisBoot=%d restartLoop=%d(recent=%u) alreadyApplied=%d -> %s\n",
        label,
        hasActive ? "" : "<none> ",
        hasActive ? (unsigned long)activeSessionId : 0UL,
        manual ? 1 : 0, alreadyDone ? 1 : 0, inLoop ? 1 : 0, recentRestarts,
        alreadyApplied ? 1 : 0, doReconcile ? "reconcile" : "skip");

    if (!doReconcile) {
        // If we skipped because the live logon router already applied for this
        // identity, stamp the marker so a later same-boot restart does not
        // reconsider.  Other skip reasons (manual / restart-loop / no active
        // session) intentionally leave the marker unset so the safety net can
        // still arm later this boot (e.g. once a loop clears or a user logs in
        // and the live logon event drives the apply).
        if (hasActive && alreadyApplied) service_mark_boot_reconcile_done();
        return;
    }

    bool applied = service_apply_profile_for_session(activeSessionId, label);
    // Stamp regardless of apply success: at-most-once per boot so an SCM
    // crash-restart loop cannot re-drive a failing/unstable apply within the same
    // boot.  A real later session change can still apply, and the next boot
    // re-arms the safety net (the marker is keyed to this boot's identity).
    service_mark_boot_reconcile_done();
    debug_log("boot reconcile: applied=%d for session %lu (marker stamped for this boot)\n",
        applied ? 1 : 0, (unsigned long)activeSessionId);
}
