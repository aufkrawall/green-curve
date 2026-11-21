// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Executes the production Windows fan runtime handback
// (main_fan_runtime_failure.cpp), the service's fan fault record and owned
// intent update (main_service_fan_fault.cpp), cases 12856-12869, and the stall
// check after every runtime tick (main_fan_stall.cpp), 12870-12879 and
// 12887-12888, against a
// scripted driver and clock.  No GPU, no sleeps.
#include "app_shared.h"
#include "fan_curve.h"
#include "desired_settings_ui_policy.h"
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
static GpuAdapterInfo g_serviceActiveDesiredGpu;
static ServiceProfileSource g_serviceActiveProfileSource;
static int g_serviceActiveProfileSlot;
static gc_log_steady::Tracker g_fanRuntimeWriteLog = {};
static const UINT FAN_FIXED_RUNTIME_INTERVAL_MS = 5000;
static const ULONGLONG FAN_RUNTIME_FAILURE_WINDOW_MS = 10000;
static bool serviceProcess, autoRefused, emergencyRefused;
static int autoWrites, manualWrites, lastManualPct, markerClears, runtimeStops;
static int restartSnapshotWrites, restartSnapshotClears;
static DesiredSettings lastRestartSnapshotDesired;
static ServiceProfileSource lastRestartSnapshotSource;
static int lastRestartSnapshotSlot;
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
static void service_write_restart_reapply_snapshot() {
    ++restartSnapshotWrites;
    lastRestartSnapshotDesired = g_serviceActiveDesired;
    lastRestartSnapshotSource = g_serviceActiveProfileSource;
    lastRestartSnapshotSlot = g_serviceActiveProfileSlot;
}
static void service_clear_restart_reapply_snapshot() {
    ++restartSnapshotClears;
    lastRestartSnapshotDesired = {};
    lastRestartSnapshotSource = SERVICE_PROFILE_SOURCE_NONE;
    lastRestartSnapshotSlot = 0;
}
static void service_fan_runtime_handback_publish(const ServiceFanFault* fault);

// Scripted fan readings (nvml_read_fans) and clock (GetTickCount64).
struct ScriptFan {
    bool manual;
    int intent;
    bool rpmKnown;
    unsigned int rpm;
    bool measuredKnown;
    int measured;
};
static ScriptFan script[2];
static bool readFails;
static int fanReads;
static unsigned long long fixtureNowMs;
static unsigned long long fixture_tick_ms() { return fixtureNowMs; }
static bool nvml_read_fans(char* detail, size_t size) {
    ++fanReads;
    if (readFails) { set_message(detail, size, "scripted failure"); return false; }
    std::memset(&g_app.readback.fan, 0, sizeof(g_app.readback.fan));
    for (unsigned int fan = 0; fan < 2; ++fan) {
        const ScriptFan& f = script[fan];
        g_app.readback.fan.policy[fan] = true;
        g_app.fanPolicy[fan] = f.manual ? NVML_FAN_POLICY_MANUAL
                                        : NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW;
        g_app.readback.fan.target[fan] = f.manual;
        g_app.fanTargetPercent[fan] = f.manual ? (unsigned int)f.intent : 0u;
        g_app.readback.fan.rpm[fan] = f.rpmKnown;
        g_app.fanRpm[fan] = f.rpmKnown ? f.rpm : 0u;
        g_app.readback.fan.measured[fan] = f.measuredKnown;
        g_app.fanPercent[fan] = f.measuredKnown ? (unsigned int)f.measured : 0u;
    }
    g_app.fanReadSerial++;
    return true;
}

#define GetTickCount64 fixture_tick_ms
#include "main_fan_runtime_failure.cpp"
#include "main_fan_stall.cpp"
#undef GetTickCount64
#include "main_service_fan_fault.cpp"
static bool resetFanPolicyComplete;
static bool live_fan_policy_readback_complete() { return resetFanPolicyComplete; }
#include "main_service_reset_fan.h"

static void reset(bool service) {
    g_app = {};
    g_app.fanSupported = true;
    g_app.fanCount = 2;
    g_app.activeFanCurve.pollIntervalMs = 1000;
    g_app.hProfileStatusLabel = service ? nullptr : (HWND)1;
    g_app.selectedGpu.valid = true;
    g_app.selectedGpu.pciInfoValid = true;
    g_app.selectedGpu.nvapiIndex = 0;
    g_app.selectedGpu.deviceId = 0x2F0410DEu;
    g_app.selectedGpu.pciBus = 1;
    g_app.adapterCount = 1;
    g_app.adapters[0] = g_app.selectedGpu;
    serviceProcess = service;
    autoRefused = emergencyRefused = false;
    autoWrites = manualWrites = lastManualPct = markerClears = runtimeStops = 0;
    restartSnapshotWrites = restartSnapshotClears = 0;
    lastRestartSnapshotDesired = {};
    lastRestartSnapshotSource = SERVICE_PROFILE_SOURCE_NONE;
    lastRestartSnapshotSlot = 0;
    lastErrorTitle[0] = lastStatus[0] = '\0';
    g_serviceHasActiveDesired = false;
    g_serviceActiveDesired = {};
    g_serviceActiveDesiredGpu = {};
    g_serviceActiveProfileSource = SERVICE_PROFILE_SOURCE_NONE;
    g_serviceActiveProfileSlot = 0;
    g_serviceFanFault = {};
}
static void own_running_curve(bool alsoPower) {
    g_serviceHasActiveDesired = true;
    g_serviceActiveDesired = {};
    g_serviceActiveDesired.lockCi = -1;
    g_serviceActiveDesired.powerLimitPct = POWER_LIMIT_DEFAULT_PCT;
    g_serviceActiveDesired.hasFan = 1;
    g_serviceActiveDesired.fanMode = FAN_MODE_CURVE;
    fan_curve_set_default(&g_serviceActiveDesired.fanCurve);
    if (alsoPower) {
        g_serviceActiveDesired.hasPowerLimit = 1;
        g_serviceActiveDesired.powerLimitPct = 90;
    }
    g_serviceActiveDesiredGpu = g_app.selectedGpu;
    g_serviceActiveProfileSource = SERVICE_PROFILE_SOURCE_USER_SLOT;
    g_serviceActiveProfileSlot = 3;
    g_app.fanCurveRuntimeActive = true;
    g_app.activeFanMode = FAN_MODE_CURVE;
}

static ScriptFan spinning_fan() { return {true, 40, true, 1100u, true, 41}; }
static ScriptFan stopped_fan() { return {true, 40, true, 0u, true, 0}; }

static void start_fixed_runtime(bool service) {
    reset(service);
    g_serviceHasActiveDesired = true;
    g_serviceActiveDesired.lockCi = -1;
    g_serviceActiveDesired.hasFan = 1;
    g_serviceActiveDesired.fanMode = FAN_MODE_FIXED;
    g_serviceActiveDesired.fanPercent = 40;
    g_app.fanFixedRuntimeActive = true;
    g_app.activeFanMode = FAN_MODE_FIXED;
    g_app.activeFanFixedPercent = 40;
    g_app.fanRangeKnown = true;
    g_app.fanMinPct = 30;
    g_app.fanMaxPct = 100;
    g_app.nvmlDevice = (nvmlDevice_t)(uintptr_t)0x1000;
    g_app.fanRuntimeGeneration = 1;
    g_fanStallTracker = {};
    g_fanStallLogLimiter = {};
    script[0] = script[1] = spinning_fan();
    readFails = false;
    fanReads = 0;
    fixtureNowMs = 100000;
}

// One runtime tick every `stepMs` until `untilMs`; the tick itself reads
// nothing, so the stall check samples on its own cadence.
static void tick_until(unsigned long long untilMs, unsigned long long stepMs) {
    while (fixtureNowMs < untilMs) {
        fixtureNowMs += stepMs;
        fan_stall_check_after_tick(g_app.fanReadSerial);
    }
}

static int run_stall_cases() {
    // A proven fan stops under a fixed 40% duty: handed back after 20 s of
    // stopped readings, not one sample earlier, and reported as a stall.
    start_fixed_runtime(true);
    fan_stall_check_after_tick(g_app.fanReadSerial);
    script[1] = stopped_fan();
    script[1].measured = 45;
    tick_until(121000, 1000);
    if (autoWrites || !g_app.fanFixedRuntimeActive) return 12870;
    tick_until(122000, 1000);
    ServiceFanFault published = service_fan_fault_for_snapshot_locked();
    if (autoWrites != 1 || g_app.fanFixedRuntimeActive || published.kind != SERVICE_FAN_FAULT_STALL ||
        published.fanIndex != 1 || published.commandedPercent != 40 ||
        published.stalledForMs != 20000 || g_serviceActiveDesired.hasFan ||
        !std::strstr(published.detail, "Fan 2 did not turn at 40%") ||
        !service_fan_fault_wire_valid(&published)) return 12871;
    // Readings are taken every 2 s, not on every tick.
    if (fanReads != 12) return 12872;
    // A tick that read the fans supplies the sample; nothing is read again.
    start_fixed_runtime(true);
    fan_stall_check_after_tick(g_app.fanReadSerial);
    fixtureNowMs += 2000;
    g_app.fanReadSerial++;
    fan_stall_check_after_tick(g_app.fanReadSerial - 1);
    if (fanReads != 1) return 12873;
    // A zero-RPM curve in its driver-automatic phase stands still by design.
    start_fixed_runtime(true);
    fan_stall_check_after_tick(g_app.fanReadSerial);
    g_app.fanFixedRuntimeActive = false;
    g_app.fanCurveRuntimeActive = true;
    g_app.fanCurveDriverAutoActive = true;
    g_app.fanCurveHasLastAppliedTemp = true;
    g_app.fanCurveLastAppliedPercent = 0;
    g_app.fanRuntimeGeneration++;
    script[0] = script[1] = stopped_fan();
    script[0].manual = script[1].manual = false;
    tick_until(200000, 1000);
    if (autoWrites || !g_app.fanCurveRuntimeActive) return 12874;
    // Each gate holds on its own.  The runtime's zero-RPM phase alone: the
    // driver still reports the last manual duty while it takes the fan back.
    start_fixed_runtime(true);
    fan_stall_check_after_tick(g_app.fanReadSerial);
    g_app.fanFixedRuntimeActive = false;
    g_app.fanCurveRuntimeActive = true;
    g_app.fanCurveDriverAutoActive = true;
    g_app.fanCurveHasLastAppliedTemp = true;
    g_app.fanCurveLastAppliedPercent = 40;
    script[0] = script[1] = stopped_fan();
    tick_until(200000, 1000);
    if (autoWrites) return 12887;
    // The driver's policy alone: the runtime commands 40%, the driver reports
    // automatic control (the runtime's own re-assert handles that).
    start_fixed_runtime(true);
    fan_stall_check_after_tick(g_app.fanReadSerial);
    script[0] = script[1] = stopped_fan();
    script[0].manual = script[1].manual = false;
    tick_until(200000, 1000);
    if (autoWrites) return 12888;
    // No RPM channel: never judged.
    start_fixed_runtime(true);
    script[0] = script[1] = stopped_fan();
    script[0].rpmKnown = script[1].rpmKnown = false;
    script[0].measuredKnown = script[1].measuredKnown = false;
    tick_until(200000, 1000);
    if (autoWrites) return 12875;
    // Sensors that read 0 from the start prove nothing.
    start_fixed_runtime(true);
    script[0] = script[1] = stopped_fan();
    tick_until(200000, 1000);
    if (autoWrites) return 12876;
    // Spin-up after a new Apply: a few stopped seconds, then motion.
    start_fixed_runtime(true);
    fan_stall_check_after_tick(g_app.fanReadSerial);
    g_app.fanRuntimeGeneration++;
    script[1] = stopped_fan();
    tick_until(106000, 1000);
    script[1] = spinning_fan();
    tick_until(200000, 1000);
    if (autoWrites) return 12877;
    // A failed read closes the window; 20 s of fresh stopped readings follow.
    start_fixed_runtime(true);
    fan_stall_check_after_tick(g_app.fanReadSerial);
    script[1] = stopped_fan();
    tick_until(116000, 1000);
    readFails = true;
    tick_until(118000, 1000);
    readFails = false;
    tick_until(137000, 1000);
    if (autoWrites) return 12878;
    tick_until(140000, 1000);
    if (autoWrites != 1) return 12878;
    // A GUI-run runtime tells the user itself.
    start_fixed_runtime(false);
    fan_stall_check_after_tick(g_app.fanReadSerial);
    script[1] = stopped_fan();
    tick_until(122000, 1000);
    if (autoWrites != 1 || !std::strstr(lastStatus, "Fan 2 stopped turning at 40%") ||
        std::strcmp(lastErrorTitle, "Fan stopped turning; custom fan control disabled") != 0)
        return 12879;
    // No runtime: no readings at all.
    start_fixed_runtime(true);
    g_app.fanFixedRuntimeActive = false;
    tick_until(200000, 1000);
    if (fanReads) return 12879;
    return 0;
}

static int run_reset_handoff_cases() {
    char detail[128] = {};
    start_fixed_runtime(true);
    autoRefused = true;
    if (service_reset_fan_to_driver(detail, sizeof(detail)) || !g_app.fanFixedRuntimeActive ||
        runtimeStops || autoWrites != 1 || !detail[0]) return 12889;
    autoRefused = false;
    resetFanPolicyComplete = false;
    detail[0] = 0;
    if (service_reset_fan_to_driver(detail, sizeof(detail)) || !g_app.fanFixedRuntimeActive ||
        runtimeStops || autoWrites != 2 || !detail[0]) return 12900;
    g_serviceActiveDesired.hasGpuOffset = true;
    g_app.selectedGpu.valid = true;
    service_reset_retain_running_fan();
    if (!g_serviceHasActiveDesired || !g_serviceActiveDesiredGpu.valid ||
        g_serviceActiveDesired.hasGpuOffset || !g_serviceActiveDesired.hasFan ||
        g_serviceActiveDesired.fanMode != FAN_MODE_FIXED ||
        g_serviceActiveDesired.fanPercent != 40 || g_serviceActiveDesired.lockCi != -1) return 12901;
    resetFanPolicyComplete = true;
    if (!service_reset_fan_to_driver(detail, sizeof(detail)) || g_app.fanFixedRuntimeActive ||
        runtimeStops != 1 || autoWrites != 3 || g_app.activeFanMode != FAN_MODE_AUTO) return 12902;
    return 0;
}

static int run() {
    if (int failure = run_reset_handoff_cases()) return failure;
    InitializeCriticalSection(&g_appLock);
    // Repeated failures in the service: the driver takes the fan back, the
    // owned intent stops claiming the curve, and the clients get a record.
    reset(true);
    own_running_curve(false);
    escalate_fan_runtime_failure("Fan curve runtime apply failed", "nvml error");
    if (autoWrites != 1 || manualWrites || runtimeStops != 1 || g_app.fanCurveRuntimeActive)
        return 12856;
    if (g_serviceActiveDesired.hasFan || g_serviceHasActiveDesired || markerClears != 1 ||
        restartSnapshotClears != 1 || restartSnapshotWrites != 0 ||
        g_serviceActiveDesiredGpu.valid ||
        g_serviceActiveProfileSource != SERVICE_PROFILE_SOURCE_NONE ||
        g_serviceActiveProfileSlot != 0 ||
        fan_owned_intent_mode(g_serviceActiveDesired.hasFan != 0, g_serviceActiveDesired.fanMode,
            g_serviceActiveDesired.fanAuto != 0) != FAN_MODE_AUTO) return 12857;
    ServiceFanFault published = service_fan_fault_for_snapshot_locked();
    if (published.kind != SERVICE_FAN_FAULT_REPEATED_FAILURES ||
        published.handback != SERVICE_FAN_HANDBACK_DRIVER_AUTO || published.serial == 0 ||
        !std::strstr(published.detail, "Fan curve runtime apply failed") ||
        !service_fan_fault_wire_valid(&published) ||
        std::strcmp(lastErrorTitle, "Fan control runtime disabled after repeated failures") != 0)
        return 12858;
    // Other owned domains stay owned; the ownership record stays with them,
    // the saved slot is demoted to AD_HOC, and the restart snapshot is updated.
    reset(true);
    own_running_curve(true);
    escalate_fan_runtime_failure("Fan curve temperature poll failed", "");
    if (!g_serviceHasActiveDesired || markerClears || g_serviceActiveDesired.hasFan ||
        !g_serviceActiveDesired.hasPowerLimit || !g_serviceActiveDesiredGpu.valid ||
        g_serviceActiveProfileSource != SERVICE_PROFILE_SOURCE_AD_HOC ||
        g_serviceActiveProfileSlot != 0 ||
        restartSnapshotWrites != 1 || restartSnapshotClears != 0 ||
        lastRestartSnapshotDesired.hasFan || !lastRestartSnapshotDesired.hasPowerLimit ||
        lastRestartSnapshotSource != SERVICE_PROFILE_SOURCE_AD_HOC ||
        lastRestartSnapshotSlot != 0) return 12859;
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
        g_serviceActiveDesired.fanPercent != FAN_RUNTIME_EMERGENCY_PERCENT || markerClears ||
        !g_serviceActiveDesiredGpu.valid ||
        g_serviceActiveProfileSource != SERVICE_PROFILE_SOURCE_AD_HOC ||
        g_serviceActiveProfileSlot != 0 ||
        restartSnapshotWrites != 1 || restartSnapshotClears != 0 ||
        lastRestartSnapshotDesired.fanMode != FAN_MODE_FIXED ||
        lastRestartSnapshotDesired.fanPercent != FAN_RUNTIME_EMERGENCY_PERCENT)
        return 12860;
    // Emergency handback when hadIntent == false initializes canonical defaults,
    // records the owner GPU from g_app.selectedGpu, and persists the snapshot.
    reset(true);
    g_app.fanCurveRuntimeActive = true;
    autoRefused = true;
    escalate_fan_runtime_failure("Fan runtime initial start failed", "nvml error");
    if (!g_serviceHasActiveDesired || !g_serviceActiveDesired.hasFan ||
        g_serviceActiveDesired.lockCi != -1 ||
        g_serviceActiveDesired.powerLimitPct != POWER_LIMIT_DEFAULT_PCT ||
        !g_serviceActiveDesiredGpu.valid ||
        g_serviceActiveDesiredGpu.deviceId != g_app.selectedGpu.deviceId ||
        g_serviceActiveProfileSource != SERVICE_PROFILE_SOURCE_AD_HOC ||
        g_serviceActiveProfileSlot != 0 || restartSnapshotWrites != 1)
        return 12869;
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
    if (int stall = run_stall_cases()) return stall;
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
