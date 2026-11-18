// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Fan requests on a GPU without a controllable fan (fan_request_policy.h).
// Pins the shared pure rule, then executes the production Windows fan step
// (main_fan_apply.h) against a scripted driver.  The reported failure: on a
// notebook the GUI's default Auto selection reached the service as a fan-auto
// write, which cannot succeed without a fan, and its failure rolled back the
// whole Apply -- VF undervolt included.
#include "app_shared.h"
#include "fan_curve.h"
#include "fan_request_policy.h"

#include <cstdio>
#include <cstring>

namespace fan_request_policy_cases {
static int run() {
    // Positive evidence only: a reported fan always means hardware.
    if (fan_control_known_absent(true, true)) return 12100;
    if (fan_control_known_absent(false, false)) return 12101;
    if (!fan_control_known_absent(false, true)) return 12102;
    if (fan_request_disposition(false, FAN_MODE_CURVE, false, false, true) !=
            FAN_REQUEST_NOT_OWNED) return 12103;
    if (fan_request_disposition(true, FAN_MODE_AUTO, true, true, true) !=
            FAN_REQUEST_HARDWARE) return 12104;
    // Unprobed (no evidence either way) keeps the ordinary path and its refusals.
    if (fan_request_disposition(true, FAN_MODE_AUTO, true, false, false) !=
            FAN_REQUEST_HARDWARE) return 12105;
    if (fan_request_disposition(true, FAN_MODE_AUTO, true, false, true) !=
            FAN_REQUEST_INERT_NO_FAN) return 12106;
    if (fan_request_disposition(true, FAN_MODE_FIXED, false, false, true) !=
            FAN_REQUEST_REFUSED_NO_FAN) return 12107;
    if (fan_request_disposition(true, FAN_MODE_CURVE, false, false, true) !=
            FAN_REQUEST_REFUSED_NO_FAN) return 12108;
    // Legacy requests carry only the auto flag.
    if (fan_request_disposition(true, -1, true, false, true) !=
            FAN_REQUEST_INERT_NO_FAN) return 12109;
    if (fan_request_disposition(true, 7, false, false, true) !=
            FAN_REQUEST_REFUSED_NO_FAN) return 12110;
    if (fan_request_needs_fan_surface(FAN_REQUEST_INERT_NO_FAN) ||
        fan_request_needs_fan_surface(FAN_REQUEST_NOT_OWNED) ||
        fan_request_needs_fan_surface(FAN_REQUEST_REFUSED_NO_FAN) ||
        !fan_request_needs_fan_surface(FAN_REQUEST_HARDWARE)) return 12111;
    if (!fan_selection_holds_without_fans(FAN_MODE_AUTO) ||
        fan_selection_holds_without_fans(FAN_MODE_FIXED) ||
        fan_selection_holds_without_fans(FAN_MODE_CURVE)) return 12112;
    if (fan_editor_adoptable_mode(FAN_MODE_CURVE, true) != FAN_MODE_AUTO ||
        fan_editor_adoptable_mode(FAN_MODE_CURVE, false) != FAN_MODE_CURVE ||
        fan_editor_adoptable_mode(FAN_MODE_FIXED, true) != FAN_MODE_AUTO) return 12113;
    if (std::strcmp(fan_request_disposition_name(FAN_REQUEST_INERT_NO_FAN), "inert-no-fan") ||
        !std::strstr(fan_request_refusal_text(), "only fan mode Auto")) return 12114;
    return 0;
}
} // namespace fan_request_policy_cases

#if defined(_WIN32)
namespace fan_request_apply_fixture {
static AppData g_app = {};
static int autoWrites, manualWrites, fanReads, curveStarts;
static bool driverReportsFans;

static void debug_log(const char*, ...) {}
static void set_last_apply_phase(const char*) {}
static bool app_is_service_process() { return true; }
static int clamp_percent(int value) { return value < 0 ? 0 : (value > 100 ? 100 : value); }
static void copy_fan_curve(FanCurveConfig* destination, const FanCurveConfig* source) {
    if (destination && source) std::memcpy(destination, source, sizeof(*destination));
}
static bool manual_fan_readback_matches_target(int wantPct, int actualPct, unsigned int, bool) {
    return wantPct == actualPct;
}
// The production read: a fan appears only when the driver reports one.
static bool nvml_read_fans(char* detail, size_t detailSize) {
    ++fanReads;
    if (!driverReportsFans) {
        set_message(detail, detailSize, "fixture: no fan reported");
        return false;
    }
    g_app.fanSupported = true;
    g_app.fanCount = 1;
    return true;
}
// Same gate as the production setter: no reported fan, no write.
static bool nvml_set_fan_auto(char* detail, size_t detailSize) {
    if (!g_app.fanSupported) {
        set_message(detail, detailSize, "Fan auto unsupported");
        return false;
    }
    ++autoWrites;
    g_app.fanIsAuto = true;
    return true;
}
static bool nvml_set_fan_manual(int, bool*, char*, size_t) { ++manualWrites; return g_app.fanSupported; }
static bool validate_manual_fan_percent_for_runtime(int, char* detail, size_t detailSize) {
    if (g_app.fanSupported) return true;
    set_message(detail, detailSize, "Manual fan control unsupported on this GPU");
    return false;
}
static bool validate_fan_curve_for_runtime(const FanCurveConfig*, char*, size_t) { return true; }
static void stop_fan_curve_runtime() {}
static void start_fixed_fan_runtime() { ++manualWrites; }
static void start_fan_curve_runtime() { ++curveStarts; }

#include "main_fan_apply.h"

static void notebook(gc_u32 fanCapability) {
    g_app = {};
    g_app.fanIsAuto = true;
    g_app.activeFanMode = FAN_MODE_AUTO;
    gpu_capability_set(&g_app.gpuCapability, SERVICE_MUTATION_DOMAIN_FAN, fanCapability);
    autoWrites = manualWrites = fanReads = curveStarts = 0;
    driverReportsFans = false;
}
static DesiredSettings fan_request(int mode) {
    DesiredSettings d = {};
    d.hasFan = true;
    d.fanMode = mode;
    d.fanAuto = mode == FAN_MODE_AUTO;
    d.fanPercent = mode == FAN_MODE_FIXED ? 60 : 0;
    fan_curve_set_default(&d.fanCurve);
    return d;
}
static int apply_step(const DesiredSettings& d, int* successes, int* failures, bool* changed) {
    char details[256] = {}, result[256] = {};
    *successes = *failures = 0;
    *changed = false;
    apply_fan_settings(&d, details, sizeof(details), *successes, *failures,
                       result, sizeof(result), *changed);
    return 0;
}

static int run() {
    int successes = 0, failures = 0;
    bool changed = false;
    char message[256] = {};

    // The reported case: notebook, zero fans, GUI selection Auto.
    notebook(GPU_DOMAIN_CAP_ABSENT);
    if (!fan_setting_matches_current(FAN_MODE_AUTO, 0, nullptr)) return 12120;
    if (fan_setting_matches_current(FAN_MODE_CURVE, 0, nullptr) ||
        fan_setting_matches_current(FAN_MODE_FIXED, 60, nullptr)) return 12121;
    DesiredSettings autoRequest = fan_request(FAN_MODE_AUTO);
    if (!validate_desired_fan_settings_for_apply(&autoRequest, message, sizeof(message)))
        return 12122;
    apply_step(autoRequest, &successes, &failures, &changed);
    // Before the fix: one doomed fan-auto attempt, failures == 1, whole Apply rolled back.
    if (failures != 0 || successes != 0 || changed || autoWrites != 0) return 12123;
    if (!app_fan_control_known_absent()) return 12124;

    // A driver that refuses the fan-count query is the same evidence.
    notebook(GPU_DOMAIN_CAP_REFUSED);
    if (!validate_desired_fan_settings_for_apply(&autoRequest, message, sizeof(message)))
        return 12125;
    apply_step(autoRequest, &successes, &failures, &changed);
    if (failures != 0 || autoWrites != 0) return 12126;

    // Fixed and Curve cannot exist without a fan: refused by name before any write.
    notebook(GPU_DOMAIN_CAP_ABSENT);
    DesiredSettings fixedRequest = fan_request(FAN_MODE_FIXED);
    message[0] = 0;
    if (validate_desired_fan_settings_for_apply(&fixedRequest, message, sizeof(message)))
        return 12127;
    if (!std::strstr(message, "Fan mode Fixed") || !std::strstr(message, "Nothing was changed"))
        return 12128;
    DesiredSettings curveRequest = fan_request(FAN_MODE_CURVE);
    message[0] = 0;
    if (validate_desired_fan_settings_for_apply(&curveRequest, message, sizeof(message)) ||
        !std::strstr(message, "Fan mode Curve") || manualWrites || curveStarts) return 12129;

    // No evidence either way (probe never answered): refused before writes,
    // after one read-only refresh, never silently skipped.
    notebook(GPU_DOMAIN_CAP_UNPROBED);
    message[0] = 0;
    if (validate_desired_fan_settings_for_apply(&autoRequest, message, sizeof(message)) ||
        fanReads != 1 || !std::strstr(message, "has not reported")) return 12130;

    // The refresh finds a fan that had not been reported yet: ordinary path.
    notebook(GPU_DOMAIN_CAP_UNPROBED);
    driverReportsFans = true;
    if (!validate_desired_fan_settings_for_apply(&autoRequest, message, sizeof(message)) ||
        fanReads != 1 || !g_app.fanSupported) return 12131;

    // A GPU with fans keeps the hardware path: a manual fan is returned to auto.
    notebook(GPU_DOMAIN_CAP_AVAILABLE);
    g_app.fanSupported = true;
    g_app.fanCount = 1;
    g_app.fanIsAuto = false;
    g_app.activeFanMode = FAN_MODE_FIXED;
    if (fan_setting_matches_current(FAN_MODE_AUTO, 0, nullptr)) return 12132;
    if (!validate_desired_fan_settings_for_apply(&autoRequest, message, sizeof(message)) ||
        fanReads != 0) return 12133;
    apply_step(autoRequest, &successes, &failures, &changed);
    if (autoWrites != 1 || successes != 1 || failures != 0 || !changed) return 12134;
    // Even with a stale "missing" capability, a reported fan is never inert.
    notebook(GPU_DOMAIN_CAP_REFUSED);
    g_app.fanSupported = true;
    g_app.fanCount = 1;
    g_app.fanIsAuto = false;
    g_app.activeFanMode = FAN_MODE_FIXED;
    if (app_fan_request_disposition(&autoRequest) != FAN_REQUEST_HARDWARE) return 12135;
    apply_step(autoRequest, &successes, &failures, &changed);
    if (autoWrites != 1 || successes != 1) return 12136;
    return 0;
}
} // namespace fan_request_apply_fixture
#endif

int run_fan_request_apply_tests() {
    int result = fan_request_policy_cases::run();
#if defined(_WIN32)
    if (!result) result = fan_request_apply_fixture::run();
#endif
    if (result) std::fprintf(stderr, "regression assertion failed: %d\n", result);
    return result;
}
#ifdef GC_FAN_REQUEST_APPLY_FIXTURE_MAIN
int main() { return run_fan_request_apply_tests(); }
#endif
