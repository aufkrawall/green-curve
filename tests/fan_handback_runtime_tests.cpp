// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Executes the production Windows fan runtime handback
// (main_fan_runtime_failure.cpp) and the service's fan fault record and owned
// intent update (main_service_fan_fault.cpp) against a scripted driver, cases
// 12856-12869.  No GPU, no sleeps.
#include "app_shared.h"
#include "fan_curve.h"
#include "fan_intent_policy.h"
#include "fan_runtime_policy.h"
#include "service_scoped_apply_policy.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <strsafe.h>

namespace fan_handback_fixture {
static AppData g_app = {};
static CRITICAL_SECTION g_appLock;
static bool g_serviceHasActiveDesired;
static DesiredSettings g_serviceActiveDesired;
static gc_log_steady::Tracker g_fanRuntimeWriteLog = {};
static const UINT FAN_FIXED_RUNTIME_INTERVAL_MS = 5000;
static const ULONGLONG FAN_RUNTIME_FAILURE_WINDOW_MS = 10000;
static bool serviceProcess, autoRefused, emergencyRefused;
static int autoWrites, manualWrites, lastManualPct, markerClears, runtimeStops;
static char lastErrorTitle[128], lastStatus[512];

static void debug_log(const char*, ...) {}
static void set_message(char* out, size_t size, const char* fmt, ...) {
    if (!out || !size) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(out, size, fmt, ap);
    va_end(ap);
}
static int clamp_percent(int value) { return value < 0 ? 0 : (value > 100 ? 100 : value); }
static const char* fan_mode_label(int mode) {
    return mode == FAN_MODE_CURVE ? "Curve" : (mode == FAN_MODE_FIXED ? "Fixed" : "Auto");
}
static bool app_is_service_process() { return serviceProcess; }
static bool nvml_set_fan_auto(char* detail, size_t size) {
    ++autoWrites;
    if (autoRefused) { set_message(detail, size, "refused"); return false; }
    return true;
}
static bool nvml_set_fan_manual(int pct, bool*, char* detail, size_t size, bool = false) {
    ++manualWrites;
    lastManualPct = pct;
    if (emergencyRefused) { set_message(detail, size, "refused"); return false; }
    return true;
}
static void stop_fan_curve_runtime() {
    if (g_app.fanCurveRuntimeActive || g_app.fanFixedRuntimeActive) ++runtimeStops;
    g_app.fanCurveRuntimeActive = false;
    g_app.fanFixedRuntimeActive = false;
    g_app.fanRuntimeConsecutiveFailures = 0;
    g_app.activeFanMode = FAN_MODE_AUTO;
}
static bool window_should_redraw_fan_controls() { return false; }
static void sync_fan_ui_from_cached_state(bool) {}
static void refresh_live_fan_telemetry(bool) {}
static bool write_error_report_log(const char* title, const char*, char*, size_t) {
    std::snprintf(lastErrorTitle, sizeof(lastErrorTitle), "%s", title ? title : "");
    return true;
}
static void set_profile_status_text(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(lastStatus, sizeof(lastStatus), fmt, ap);
    va_end(ap);
}
static void update_tray_icon() {}
static bool service_runtime_lock_held_by_current_thread() { return true; }
static bool service_ownership_marker_clear(const char*) { ++markerClears; return true; }
static void service_fan_runtime_handback_publish(const ServiceFanFault* fault);

#include "main_fan_runtime_failure.cpp"
#include "main_service_fan_fault.cpp"

static void reset(bool service) {
    g_app = {};
    g_app.fanSupported = true;
    g_app.fanCount = 2;
    g_app.activeFanCurve.pollIntervalMs = 1000;
    g_app.hProfileStatusLabel = service ? nullptr : (HWND)1;
    serviceProcess = service;
    autoRefused = emergencyRefused = false;
    autoWrites = manualWrites = lastManualPct = markerClears = runtimeStops = 0;
    lastErrorTitle[0] = lastStatus[0] = '\0';
    g_serviceHasActiveDesired = false;
    g_serviceActiveDesired = {};
    g_serviceFanFault = {};
}
static void own_running_curve(bool alsoPower) {
    g_serviceHasActiveDesired = true;
    g_serviceActiveDesired = {};
    g_serviceActiveDesired.lockCi = -1;
    g_serviceActiveDesired.hasFan = 1;
    g_serviceActiveDesired.fanMode = FAN_MODE_CURVE;
    fan_curve_set_default(&g_serviceActiveDesired.fanCurve);
    if (alsoPower) {
        g_serviceActiveDesired.hasPowerLimit = 1;
        g_serviceActiveDesired.powerLimitPct = 90;
    }
    g_app.fanCurveRuntimeActive = true;
    g_app.activeFanMode = FAN_MODE_CURVE;
}

static int run() {
    InitializeCriticalSection(&g_appLock);
    // Repeated failures in the service: the driver takes the fan back, the
    // owned intent stops claiming the curve, and the clients get a record.
    reset(true);
    own_running_curve(false);
    escalate_fan_runtime_failure("Fan curve runtime apply failed", "nvml error");
    if (autoWrites != 1 || manualWrites || runtimeStops != 1 || g_app.fanCurveRuntimeActive)
        return 12856;
    if (g_serviceActiveDesired.hasFan || g_serviceHasActiveDesired || markerClears != 1 ||
        fan_owned_intent_mode(g_serviceActiveDesired.hasFan != 0, g_serviceActiveDesired.fanMode,
            g_serviceActiveDesired.fanAuto != 0) != FAN_MODE_AUTO) return 12857;
    ServiceFanFault published = service_fan_fault_for_snapshot_locked();
    if (published.kind != SERVICE_FAN_FAULT_REPEATED_FAILURES ||
        published.handback != SERVICE_FAN_HANDBACK_DRIVER_AUTO || published.serial == 0 ||
        !std::strstr(published.detail, "Fan curve runtime apply failed") ||
        !service_fan_fault_wire_valid(&published) ||
        std::strcmp(lastErrorTitle, "Fan control runtime disabled after repeated failures") != 0)
        return 12858;
    // Other owned domains stay owned; the ownership record stays with them.
    reset(true);
    own_running_curve(true);
    escalate_fan_runtime_failure("Fan curve temperature poll failed", "");
    if (!g_serviceHasActiveDesired || markerClears || g_serviceActiveDesired.hasFan ||
        !g_serviceActiveDesired.hasPowerLimit) return 12859;
    // The driver refuses the fan: 100%, and the intent owns that emergency duty.
    reset(true);
    own_running_curve(false);
    autoRefused = true;
    escalate_fan_runtime_failure("Fan curve runtime apply failed", "nvml error");
    published = service_fan_fault_for_snapshot_locked();
    if (manualWrites != 1 || lastManualPct != FAN_RUNTIME_EMERGENCY_PERCENT ||
        published.handback != SERVICE_FAN_HANDBACK_EMERGENCY_MAX ||
        !g_serviceHasActiveDesired || !g_serviceActiveDesired.hasFan ||
        g_serviceActiveDesired.fanMode != FAN_MODE_FIXED ||
        g_serviceActiveDesired.fanPercent != FAN_RUNTIME_EMERGENCY_PERCENT || markerClears)
        return 12860;
    // Neither write confirmed: reported as such, still aiming at 100%.
    reset(true);
    own_running_curve(false);
    autoRefused = emergencyRefused = true;
    escalate_fan_runtime_failure("Fan curve runtime apply failed", "nvml error");
    published = service_fan_fault_for_snapshot_locked();
    if (published.handback != SERVICE_FAN_HANDBACK_FAILED ||
        g_serviceActiveDesired.fanPercent != FAN_RUNTIME_EMERGENCY_PERCENT) return 12861;
    // A GUI-run runtime has no service intent or record; it says so itself.
    reset(false);
    own_running_curve(false);
    escalate_fan_runtime_failure("Fan curve runtime apply failed", "nvml error");
    if (!g_serviceActiveDesired.hasFan || g_serviceFanFault.kind != SERVICE_FAN_FAULT_NONE ||
        !std::strstr(lastStatus, "repeated fan errors") ||
        !std::strstr(lastStatus, "See the Green Curve error log.")) return 12862;
    // Another path stopped the runtime first: nothing to hand back or report.
    reset(true);
    own_running_curve(false);
    g_app.fanCurveRuntimeActive = false;
    escalate_fan_runtime_failure("late", "");
    if (autoWrites || g_serviceFanFault.kind != SERVICE_FAN_FAULT_NONE ||
        !g_serviceActiveDesired.hasFan) return 12863;
    // A stall names the fan, the duty and the duration.
    reset(true);
    own_running_curve(false);
    FanRuntimeHandbackCause stall = {};
    stall.kind = SERVICE_FAN_FAULT_STALL;
    stall.summary = "Fan 1 did not turn at 40% for 20 s";
    stall.fanIndex = 0;
    stall.commandedPercent = 40;
    stall.stalledForMs = 20000;
    escalate_fan_runtime_handback(&stall);
    published = service_fan_fault_for_snapshot_locked();
    if (published.kind != SERVICE_FAN_FAULT_STALL || published.commandedPercent != 40 ||
        published.stalledForMs != 20000 || published.fanIndex != 0 ||
        !service_fan_fault_wire_valid(&published) ||
        std::strcmp(lastErrorTitle, "Fan stopped turning; custom fan control disabled") != 0)
        return 12864;
    // Each record is numbered; an Apply that sets the fan, or a Reset, clears it.
    const gc_u32 serial = published.serial;
    own_running_curve(false);
    escalate_fan_runtime_handback(&stall);
    if (service_fan_fault_for_snapshot_locked().serial != serial + 1) return 12865;
    service_fan_fault_clear("test");
    published = service_fan_fault_for_snapshot_locked();
    if (published.kind != SERVICE_FAN_FAULT_NONE || !service_fan_fault_wire_valid(&published))
        return 12866;
    // The failure ladder escalates exactly once, at its limit (fixed duty: 3).
    reset(true);
    g_serviceHasActiveDesired = true;
    g_serviceActiveDesired.hasFan = 1;
    g_serviceActiveDesired.fanMode = FAN_MODE_FIXED;
    g_serviceActiveDesired.fanPercent = 50;
    g_app.fanFixedRuntimeActive = true;
    report_fan_runtime_failure("Fixed fan runtime apply failed", "x");
    // A healthy tick in between starts the count over.
    mark_fan_runtime_success(1);
    report_fan_runtime_failure("Fixed fan runtime apply failed", "x");
    report_fan_runtime_failure("Fixed fan runtime apply failed", "x");
    if (autoWrites || g_serviceFanFault.kind != SERVICE_FAN_FAULT_NONE) return 12867;
    report_fan_runtime_failure("Fixed fan runtime apply failed", "x");
    if (autoWrites != 1 || g_serviceFanFault.kind != SERVICE_FAN_FAULT_REPEATED_FAILURES)
        return 12868;
    DeleteCriticalSection(&g_appLock);
    return 0;
}
} // namespace fan_handback_fixture
#endif

int run_fan_handback_runtime_tests() {
    int result = 0;
#if defined(_WIN32)
    result = fan_handback_fixture::run();
#endif
    if (result) std::fprintf(stderr, "fan handback regression failed: %d\n", result);
    return result;
}
