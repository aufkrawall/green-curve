// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Fan-runtime failure counting and escalation, included by main_fan_runtime.cpp.
//
// Split in two on purpose.  Counting needs g_appLock; escalating must NOT hold
// it, for two reasons:
//
//  * It writes to NVML (driver auto, or 100% when that fails).  A driver
//    restart can make that write fault, and the VEH ends the faulting thread
//    with ExitThread -- which would orphan g_appLock and hang every later
//    reader of application state.  apply_fan_curve_tick() already keeps NVML
//    outside the lock for exactly this reason; the escalation was the one path
//    that did not.
//  * It stops the fan runtime.  In the service that stops the fan worker, and
//    the worker takes g_appLock at the top of every loop.  Escalating under the
//    lock, from the worker itself, is what deadlocked the service against the
//    telemetry request (fan_worker_lifecycle_policy.h).

#include "fan_fault_notice_policy.h"

static unsigned int fan_runtime_failure_limit() {
    UINT intervalMs = g_app.fanFixedRuntimeActive
        ? FAN_FIXED_RUNTIME_INTERVAL_MS
        : (UINT)g_app.activeFanCurve.pollIntervalMs;
    if (intervalMs < 250) intervalMs = 250;
    unsigned int limit = (unsigned int)((FAN_RUNTIME_FAILURE_WINDOW_MS + intervalMs - 1) / intervalMs);
    if (limit < 3) limit = 3;
    if (limit > 10) limit = 10;
    return limit;
}
static void mark_fan_runtime_success(ULONGLONG now) {
    g_app.fanRuntimeConsecutiveFailures = 0;
    g_app.fanRuntimeLastApplyTickMs = now;
}

// Records one failed tick.  Caller holds g_appLock.  Returns true when the
// failure limit has been reached; the caller must then RELEASE g_appLock and
// call escalate_fan_runtime_failure().
static bool note_fan_runtime_failure_locked(const char* action, const char* detail) {
    if (!g_app.fanCurveRuntimeActive && !g_app.fanFixedRuntimeActive) return false;
    g_app.fanRuntimeLastApplyTickMs = 0;
    g_app.fanRuntimeConsecutiveFailures++;
    unsigned int limit = fan_runtime_failure_limit();
    // Suppress repetitive identical failure logs within a 30-second window.
    // The statics are guarded by g_appLock, which the caller holds.
    static ULONGLONG s_lastFailureLogTickMs = 0;
    static char s_lastFailureAction[128] = {};
    static char s_lastFailureDetail[128] = {};
    ULONGLONG now = GetTickCount64();
    bool sameAction = (action && action[0]) ? strcmp(action, s_lastFailureAction) == 0 : s_lastFailureAction[0] == 0;
    bool sameDetail = (detail && detail[0]) ? strcmp(detail, s_lastFailureDetail) == 0 : s_lastFailureDetail[0] == 0;
    bool suppress = sameAction && sameDetail && (now - s_lastFailureLogTickMs < 30000);
    if (!suppress) {
        s_lastFailureLogTickMs = now;
        StringCchCopyA(s_lastFailureAction, ARRAY_COUNT(s_lastFailureAction), action ? action : "");
        StringCchCopyA(s_lastFailureDetail, ARRAY_COUNT(s_lastFailureDetail), detail ? detail : "");
        debug_log("fan runtime failure %u/%u: %s%s%s\n",
            g_app.fanRuntimeConsecutiveFailures,
            limit,
            action ? action : "fan runtime failure",
            (detail && detail[0]) ? " - " : "",
            (detail && detail[0]) ? detail : "");
    }
    return g_app.fanRuntimeConsecutiveFailures >= limit;
}

// Why the runtime gives a fan up.  `kind` (ServiceFanFaultKind) selects the
// wording; the stall fields stay zero for repeated failures.
struct FanRuntimeHandbackCause {
    gc_u32 kind;
    const char* summary;
    unsigned int fanIndex;
    int commandedPercent;
    unsigned long long stalledForMs;
};

// Hands the fan back to the driver, forcing maximum cooling when the driver
// refuses it, stops the runtime and reports what happened: the error log, the
// GUI status line in a GUI-run runtime, and in the service the v39 record plus
// an owned intent that matches what Green Curve now holds
// (main_service_fan_fault.cpp).  Called WITHOUT g_appLock (see the file header).
static void escalate_fan_runtime_handback(const FanRuntimeHandbackCause* cause) {
    EnterCriticalSection(&g_appLock);
    bool stillActive = g_app.fanCurveRuntimeActive || g_app.fanFixedRuntimeActive;
    unsigned int failures = g_app.fanRuntimeConsecutiveFailures;
    LeaveCriticalSection(&g_appLock);
    if (!stillActive) {
        debug_log("fan runtime escalation: runtime already stopped by another path; nothing to hand back\n");
        return;
    }
    const gc_u32 kind = cause ? cause->kind : (gc_u32)SERVICE_FAN_FAULT_REPEATED_FAILURES;
    const char* summary = cause && cause->summary && cause->summary[0]
        ? cause->summary : "Custom fan runtime failed repeatedly";
    debug_log("fan runtime escalation: cause=%s failures=%u; handing the fan back to the driver"
              " (outside g_appLock, thread=%lu): %s\n",
        gc_fan_fault_kind_name(kind), failures, GetCurrentThreadId(), summary);
    char autoDetail[128] = {};
    bool autoRestored = nvml_set_fan_auto(autoDetail, sizeof(autoDetail));
    bool emergencyOk = false;
    char emergencyDetail[128] = {};
    if (!autoRestored) {
        emergencyOk = nvml_set_fan_manual(FAN_RUNTIME_EMERGENCY_PERCENT, nullptr,
            emergencyDetail, sizeof(emergencyDetail));
        if (emergencyOk) {
            debug_log("fan runtime failure emergency: set fan to %d%% after auto-restore failed\n",
                FAN_RUNTIME_EMERGENCY_PERCENT);
        } else {
            debug_log("fan runtime failure emergency: could not set fan to %d%% after auto-restore failed: %s\n",
                FAN_RUNTIME_EMERGENCY_PERCENT, emergencyDetail);
        }
    }
    // In the service this may be the fan worker stopping itself; that is a
    // signal, not a join (stop_service_fan_runtime_thread).
    stop_fan_curve_runtime();
    if (autoRestored) {
        EnterCriticalSection(&g_appLock);
        g_app.activeFanMode = FAN_MODE_AUTO;
        sync_fan_ui_from_cached_state(window_should_redraw_fan_controls());
        LeaveCriticalSection(&g_appLock);
    } else if (g_app.hMainWnd) {
        refresh_live_fan_telemetry(window_should_redraw_fan_controls());
    }

    ServiceFanFault fault = {};
    fault.kind = kind;
    fault.handback = autoRestored ? SERVICE_FAN_HANDBACK_DRIVER_AUTO
        : emergencyOk ? SERVICE_FAN_HANDBACK_EMERGENCY_MAX : SERVICE_FAN_HANDBACK_FAILED;
    if (kind == SERVICE_FAN_FAULT_STALL && cause) {
        fault.fanIndex = cause->fanIndex < MAX_GPU_FANS ? cause->fanIndex : 0;
        fault.commandedPercent = (gc_u32)clamp_percent(cause->commandedPercent);
        fault.stalledForMs = cause->stalledForMs > 0xFFFFFFFFull
            ? 0xFFFFFFFFu : (gc_u32)cause->stalledForMs;
    }
    StringCchCopyA(fault.detail, ARRAY_COUNT(fault.detail), summary);
    fault.serial = 1;  // the service numbers its own records
    if (app_is_service_process()) service_fan_runtime_handback_publish(&fault);

    char reportDetails[768] = {};
    if (autoRestored) {
        set_message(reportDetails, sizeof(reportDetails), "%s. Driver auto fan restored%s%s%s.",
            summary, autoDetail[0] ? " (" : "", autoDetail, autoDetail[0] ? ")" : "");
    } else {
        set_message(reportDetails, sizeof(reportDetails),
            "%s. Attempt to restore driver auto fan failed%s%s. %s",
            summary, autoDetail[0] ? ": " : "", autoDetail,
            emergencyOk ? "Fan forced to 100%." : "Forcing the fan to 100% also failed.");
    }
    char logErr[256] = {};
    if (!write_error_report_log(
            kind == SERVICE_FAN_FAULT_STALL
                ? "Fan stopped turning; custom fan control disabled"
                : "Fan control runtime disabled after repeated failures",
            reportDetails,
            logErr,
            sizeof(logErr)) &&
        logErr[0]) {
        debug_log("fan runtime error log failed: %s\n", logErr);
    }
    if (g_app.hProfileStatusLabel) {
        char status[256] = {}, title[64] = {}, body[256] = {};
        if (gc_fan_fault_compose(&fault, status, sizeof(status), title, sizeof(title),
                body, sizeof(body)))
            set_profile_status_text("%s See the Green Curve error log.", status);
    }
    update_tray_icon();
}

// Repeated write or telemetry failures reached the limit.
static void escalate_fan_runtime_failure(const char* action, const char* detail) {
    char summary[512] = {};
    if (action && action[0] && detail && detail[0]) {
        set_message(summary, sizeof(summary), "%s: %s", action, detail);
    } else if (action && action[0]) {
        set_message(summary, sizeof(summary), "%s", action);
    } else if (detail && detail[0]) {
        set_message(summary, sizeof(summary), "%s", detail);
    } else {
        set_message(summary, sizeof(summary), "Custom fan runtime failed repeatedly");
    }
    FanRuntimeHandbackCause cause = {};
    cause.kind = SERVICE_FAN_FAULT_REPEATED_FAILURES;
    cause.summary = summary;
    escalate_fan_runtime_handback(&cause);
}

// The one entry point for a failed tick.  Must be called WITHOUT g_appLock.
static void report_fan_runtime_failure(const char* action, const char* detail) {
    gc_log_steady::invalidate(&g_fanRuntimeWriteLog);
    EnterCriticalSection(&g_appLock);
    bool escalate = note_fan_runtime_failure_locked(action, detail);
    LeaveCriticalSection(&g_appLock);
    if (escalate) escalate_fan_runtime_failure(action, detail);
}
