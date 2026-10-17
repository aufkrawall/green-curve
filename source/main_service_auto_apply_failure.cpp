// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The service's v29 automatic-apply failure record (ServiceAutoApplyFailure):
// what the GUI is told when a logon, standby or driver-recovery write failed
// and latched the sticky AUTOMATIC_APPLY_FAILED lockout.  Included before the
// snapshot shard, which publishes it.
//
// In memory only, on purpose: the lockout itself is persisted, but after a
// service restart this instance did not see the failure and does not claim to
// know its cause -- the GUI then says only that automation is paused.

// Guarded by g_appLock.
static ServiceAutoApplyFailure g_serviceAutoApplyFailure = {};

// `detail` is the apply's full result; whether its core recovery verified is
// decided here, before the text is truncated to the wire field.
static void service_auto_apply_failure_record(ServiceLifecycleTrigger trigger,
                                              const char* detail) {
    ServiceAutoApplyFailure record = {};
    record.trigger = (gc_u32)trigger;
    record.coreRecovered = gc_apply_result_reports_core_recovery(detail) ? 1 : 0;
    StringCchCopyA(record.detail, ARRAY_COUNT(record.detail),
                   detail && detail[0] ? detail : "no failure detail recorded");
    EnterCriticalSection(&g_appLock);
    g_serviceAutoApplyFailure = record;
    LeaveCriticalSection(&g_appLock);
    debug_log("automatic apply failure: recorded for the GUI trigger=%u coreRecovered=%d detailChars=%u truncated=%d\n",
        (unsigned int)record.trigger, record.coreRecovered ? 1 : 0,
        (unsigned int)strlen(record.detail),
        detail && strlen(detail) >= ARRAY_COUNT(record.detail) ? 1 : 0);
}

// An explicit successful Apply is about to re-arm automation: the cause is
// history.  Called BEFORE the lockout is cleared, so the record can never
// outlive its lockout into one latched for another reason.
static void service_auto_apply_failure_clear() {
    EnterCriticalSection(&g_appLock);
    const gc_u32 hadTrigger = g_serviceAutoApplyFailure.trigger;
    g_serviceAutoApplyFailure = ServiceAutoApplyFailure{};
    LeaveCriticalSection(&g_appLock);
    if (hadTrigger != SERVICE_LIFECYCLE_TRIGGER_NONE)
        debug_log("automatic apply failure: record (trigger=%u) cleared by explicit successful apply\n",
                  (unsigned int)hadTrigger);
}

// Caller holds g_appLock.  Published only while the lockout it explains is the
// one in force, so a later lockout with another cause never inherits it.
static ServiceAutoApplyFailure service_auto_apply_failure_for_snapshot_locked(
    DWORD lockoutReason) {
    if (lockoutReason != SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED)
        return ServiceAutoApplyFailure{};
    return g_serviceAutoApplyFailure;
}
