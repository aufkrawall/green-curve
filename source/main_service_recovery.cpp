// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Fail-closed automatic-restore state transitions shared by the single
// lifecycle worker and the controlled-restart authorization path.

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
    // outside a pulse or blocked on that same lock. Signal it and let it exit
    // after the atomic state transition is published; synchronously joining it
    // would temporarily drop the lock and let an explicit Apply interleave
    // halfway through this fail-closed transition.
    if (service_runtime_lock_held_by_current_thread()) {
        if (g_serviceFanStopEvent) SetEvent(g_serviceFanStopEvent);
    } else {
        stop_service_fan_runtime_thread();
    }
    debug_log("automatic restore: disabled without a hardware write (%s)\n",
        context && context[0] ? context : "unspecified safety condition");
}
