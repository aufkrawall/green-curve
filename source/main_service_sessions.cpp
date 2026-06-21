// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// ============================================================================
// Active-user session model (multi-user support)
// ============================================================================
//
// Green Curve controls a single GPU that may be shared by several Windows
// user accounts on the same machine.  Policy: only the currently-ACTIVE
// interactive session may drive the GPU, and its resolved profile is applied
// whenever the active session identity changes.  This preserves the F-SEC-3
// security boundary (a *different* local user cannot drive OC through the
// SYSTEM service) while making Fast-User-Switching, RDP, and service restart
// behave predictably: the active user's profile is applied on logon, on
// service (re)start when a user is already logged in, and restored when the
// user switches back to a session.
//
// This shard is a pure policy/router layer.  It delegates profile resolution
// to service_session_logon_resolve_and_load_profile() and hardware writes to
// service_apply_desired_settings(), both defined in main_service_runtime.cpp.
// All gating is event + identity comparison — no sleeps or timing bandaids.

// Last session we successfully applied a profile for in this process lifetime.
// Used to debounce session-change events: we only re-apply when the resolved
// active session identity actually changes, never on every event.
static DWORD g_lastAppliedSessionId = (DWORD)-1;
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
    debug_log("session change: %s for session %lu; resolved active session = %s%lu, last applied = %lu\n",
        evtName, (unsigned long)eventSessionId,
        hasActive ? "" : "<none> ",
        hasActive ? (unsigned long)activeSessionId : 0UL,
        (unsigned long)g_lastAppliedSessionId);

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
    if (activeSessionId == g_lastAppliedSessionId) {
        debug_log("session change: active session %lu unchanged since last apply, skipping re-apply\n",
            (unsigned long)activeSessionId);
        return;
    }

    service_apply_profile_for_session(activeSessionId, evtName);
}

// Service-start reconciliation: if a user is already logged into an active
// session when the service (re)starts — e.g. SCM failure-restart, driver
// recovery restart, or a late auto-start after login — the WTS_SESSION_LOGON
// for that session already fired before we were accepting controls.  Apply
// the active session's resolved profile so the GPU reflects the active user's
// intent without requiring a logout/login.  Called from service_main after
// the service reports RUNNING and the pipe thread is up.
static void service_reconcile_active_session_apply() {
    DWORD activeSessionId = (DWORD)-1;
    if (!service_get_active_session_for_control(&activeSessionId)) {
        debug_log("session reconcile: no active interactive session at service start, nothing to apply\n");
        return;
    }
    if (g_lastAppliedSessionId == activeSessionId) {
        debug_log("session reconcile: active session %lu already applied this process lifetime, skipping\n",
            (unsigned long)activeSessionId);
        return;
    }
    debug_log("session reconcile: service started with active session %lu already logged in; applying its profile\n",
        (unsigned long)activeSessionId);
    service_apply_profile_for_session(activeSessionId, "service start reconcile");
}
