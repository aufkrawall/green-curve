// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Windows fan request handling for an apply: the "does this selection already
// hold?" predicate, pre-write validation, and the fan step itself.  Included by
// main_fan_runtime.cpp.
#ifndef GREEN_CURVE_MAIN_FAN_APPLY_H
#define GREEN_CURVE_MAIN_FAN_APPLY_H

static bool fan_setting_matches_current(int wantMode, int wantPct, const FanCurveConfig* wantCurve) {
    if (!g_app.fanSupported) return false;
    if (wantMode != g_app.activeFanMode) return false;
    if (wantMode == FAN_MODE_AUTO) return g_app.fanIsAuto;
    if (wantMode == FAN_MODE_CURVE) {
        return g_app.fanCurveRuntimeActive && wantCurve &&
            fan_curve_equals(wantCurve, &g_app.activeFanCurve);
    }
    if (g_app.fanIsAuto || g_app.fanCount == 0) return false;
    for (unsigned int fan = 0; fan < g_app.fanCount; fan++) {
        int gotPct = (int)g_app.fanPercent[fan];
        if (!manual_fan_readback_matches_target(wantPct, gotPct,
                g_app.fanTargetPercent[fan], g_app.readback.fan.target[fan])) return false;
    }
    return true;
}

static bool validate_desired_fan_settings_for_apply(const DesiredSettings* desired, char* result, size_t resultSize) {
    if (!desired->hasFan) return true;
    int desiredFanMode = desired->fanMode;
    if (desiredFanMode < FAN_MODE_AUTO || desiredFanMode > FAN_MODE_CURVE) {
        desiredFanMode = desired->fanAuto ? FAN_MODE_AUTO : FAN_MODE_FIXED;
    }
    FanCurveConfig desiredCurve = desired->fanCurve;
    fan_curve_normalize(&desiredCurve);
    if (g_app.fanRangeKnown) {
        fan_curve_clamp_percentages(&desiredCurve, (int)g_app.fanMinPct, (int)g_app.fanMaxPct);
    }
    if (desiredFanMode == FAN_MODE_CURVE) {
        char validationErr[256] = {};
        if (!fan_curve_validate(&desiredCurve, validationErr, sizeof(validationErr))) {
            set_message(result, resultSize, "%s", validationErr);
            return false;
        }
        if (!validate_fan_curve_for_runtime(&desiredCurve, validationErr, sizeof(validationErr))) {
            set_message(result, resultSize, "%s", validationErr);
            return false;
        }
    } else if (desiredFanMode == FAN_MODE_FIXED) {
        char detail[128] = {};
        if (!validate_manual_fan_percent_for_runtime(desired->fanPercent, detail, sizeof(detail))) {
            set_message(result, resultSize, "%s", detail);
            return false;
        }
    }
    return true;
}

// Prevalidation above owns request validity.  Once this helper is entered, a
// fan hardware attempt reports its outcome only through successCount/failCount;
// it always returns control to the caller so the core transaction can make one
// mixed-result rollback decision across VF/lock/memory/power/fan.
static void apply_fan_settings(const DesiredSettings* desired, char* failureDetails, size_t failureDetailsSize, int& successCount, int& failCount, char* result, size_t resultSize, bool& outFanChanged) {
    (void)result;
    (void)resultSize;
    outFanChanged = false;
    if (!desired->hasFan) return;
    set_last_apply_phase("apply: fan settings");
    auto append_failure = [&](const char* fmt, ...) {
        char part[256] = {};
        va_list ap;
        va_start(ap, fmt);
        StringCchVPrintfA(part, ARRAY_COUNT(part), fmt, ap);
        va_end(ap);
        if (!part[0]) return;
        if (failureDetails[0]) {
            StringCchCatA(failureDetails, (int)failureDetailsSize, "; ");
        }
        StringCchCatA(failureDetails, (int)failureDetailsSize, part);
        debug_log("apply failure: %s\n", part);
    };
    int desiredFanMode = desired->fanMode;
    if (desiredFanMode < FAN_MODE_AUTO || desiredFanMode > FAN_MODE_CURVE) {
        desiredFanMode = desired->fanAuto ? FAN_MODE_AUTO : FAN_MODE_FIXED;
    }
    FanCurveConfig desiredCurve = desired->fanCurve;
    fan_curve_normalize(&desiredCurve);
    if (g_app.fanRangeKnown) {
        fan_curve_clamp_percentages(&desiredCurve, (int)g_app.fanMinPct, (int)g_app.fanMaxPct);
    }
    bool fanChanged = false;
    if (!fan_setting_matches_current(desiredFanMode, desired->fanPercent, &desiredCurve)) {
        fanChanged = true;
        int previousMode = g_app.activeFanMode;
        int previousFixed = g_app.activeFanFixedPercent;
        FanCurveConfig previousCurve = g_app.activeFanCurve;
        bool previousCurveRuntime = g_app.fanCurveRuntimeActive;
        bool previousFixedRuntime = g_app.fanFixedRuntimeActive;
        bool exact = false;
        char detail[128] = {};
        bool ok = false;
        if (desiredFanMode == FAN_MODE_AUTO) {
            set_last_apply_phase("apply: fan auto write");
            ok = nvml_set_fan_auto(detail, sizeof(detail));
            if (ok) {
                stop_fan_curve_runtime();
                g_app.activeFanMode = FAN_MODE_AUTO;
                debug_log("apply fan auto: restored driver auto, runtime stopped\n");
            }
        } else if (desiredFanMode == FAN_MODE_FIXED) {
            if (validate_manual_fan_percent_for_runtime(desired->fanPercent, detail, sizeof(detail))) {
                stop_fan_curve_runtime();
                if (g_app.hMainWnd || app_is_service_process()) {
                    set_last_apply_phase("apply: fixed fan runtime start");
                    g_app.activeFanFixedPercent = clamp_percent(desired->fanPercent);
                    start_fixed_fan_runtime();
                    ok = g_app.fanFixedRuntimeActive && g_app.fanRuntimeLastApplyTickMs != 0;
                    if (!ok) {
                        set_message(detail, sizeof(detail),
                            g_app.fanRuntimeConsecutiveFailures > 0
                                ? "Failed to verify the initial fixed fan apply"
                                : "Failed to start fixed fan maintenance");
                    }
                } else {
                    set_last_apply_phase("apply: fixed fan write");
                    ok = nvml_set_fan_manual(desired->fanPercent, &exact, detail, sizeof(detail));
                }
            }
            if (ok) {
                g_app.activeFanMode = FAN_MODE_FIXED;
                g_app.activeFanFixedPercent = clamp_percent(desired->fanPercent);
            }
        } else {
            if (!validate_fan_curve_for_runtime(&desiredCurve, detail, sizeof(detail))) {
                ok = false;
            } else if (g_app.hMainWnd || app_is_service_process()) {
                stop_fan_curve_runtime();
                copy_fan_curve(&g_app.activeFanCurve, &desiredCurve);
                debug_log("apply fan curve: pollMs=%d curveDownshift=%dC "
                    "zeroRpm=%d zeroRpmGap=%dC start=%dC "
                    "firstEnabledPct=%d serviceProcess=%d\n",
                    g_app.activeFanCurve.pollIntervalMs,
                    g_app.activeFanCurve.hysteresisC,
                    g_app.activeFanCurve.zeroRpmEnabled ? 1 : 0,
                    fan_curve_zero_rpm_hysteresis(&g_app.activeFanCurve),
                    fan_curve_first_enabled_temperature(&g_app.activeFanCurve),
                    g_app.activeFanCurve.points[0].enabled ? g_app.activeFanCurve.points[0].fanPercent : 0,
                    app_is_service_process() ? 1 : 0);
                set_last_apply_phase("apply: fan curve runtime start");
                start_fan_curve_runtime();
                ok = g_app.fanCurveRuntimeActive && g_app.fanRuntimeLastApplyTickMs != 0;
                debug_log("apply fan curve runtime start: active=%d lastApply=%llu failures=%u\n",
                    g_app.fanCurveRuntimeActive ? 1 : 0,
                    g_app.fanRuntimeLastApplyTickMs,
                    g_app.fanRuntimeConsecutiveFailures);
                if (!ok) {
                    set_message(detail, sizeof(detail),
                        g_app.fanRuntimeConsecutiveFailures > 0
                            ? "Failed to verify the initial fan curve apply"
                            : "Failed to start fan curve maintenance");
                }
            } else {
                set_message(detail, sizeof(detail),
                    "Fan curve mode requires the resident Green Curve tray app. Start the program hidden to tray for long-running fan control.");
            }
        }
        if (ok) successCount++;
        else {
            g_app.activeFanMode = previousMode;
            g_app.activeFanFixedPercent = previousFixed;
            copy_fan_curve(&g_app.activeFanCurve, &previousCurve);
            if (previousCurveRuntime) {
                start_fan_curve_runtime();
            } else if (previousFixedRuntime) {
                start_fixed_fan_runtime();
            }
            failCount++;
            append_failure("Fan control change failed%s%s",
                detail[0] ? ": " : "",
                detail[0] ? detail : "");
        }
    }
    outFanChanged = fanChanged;
}

#endif  // GREEN_CURVE_MAIN_FAN_APPLY_H
