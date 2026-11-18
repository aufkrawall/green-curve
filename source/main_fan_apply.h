// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Windows fan request handling for an apply: the "does this selection already
// hold?" predicate, pre-write validation, and the fan step itself.  Included by
// main_fan_runtime.cpp; tests/fan_request_apply_tests.cpp executes this shard
// against a fixture driver.  The rule it implements for GPUs without a
// controllable fan lives in fan_request_policy.h.
#ifndef GREEN_CURVE_MAIN_FAN_APPLY_H
#define GREEN_CURVE_MAIN_FAN_APPLY_H

// fan_request_policy.h: positive evidence that this GPU has no fan Green Curve
// can drive (notebook EC cooling, passive boards).  In the service both inputs
// come from its own NVML reads; in the GUI both come from the service snapshot
// (fanSupported and the packed capability probe), so the two processes decide
// from the same facts.
static bool app_fan_control_known_absent() {
    return fan_control_known_absent(g_app.fanSupported != 0,
        gpu_capability_domain_is_missing(gpu_capability_get(
            &g_app.gpuCapability, SERVICE_MUTATION_DOMAIN_FAN)));
}
static FanRequestDisposition app_fan_request_disposition(const DesiredSettings* desired) {
    if (!desired) return FAN_REQUEST_NOT_OWNED;
    return fan_request_disposition(desired->hasFan != 0, desired->fanMode,
        desired->fanAuto != 0, g_app.fanSupported != 0,
        gpu_capability_domain_is_missing(gpu_capability_get(
            &g_app.gpuCapability, SERVICE_MUTATION_DOMAIN_FAN)));
}
static bool fan_setting_matches_current(int wantMode, int wantPct, const FanCurveConfig* wantCurve) {
    // No fan Green Curve can drive: Auto is the platform's own state and needs
    // no write, anything else can never hold.  Answering "never matches" here
    // made every GUI Apply on a notebook carry a fan-auto write that could only
    // fail, and that failure rolled the whole Apply back (fan_request_policy.h).
    if (!g_app.fanSupported) return fan_selection_holds_without_fans(wantMode);
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
    if (!g_app.fanSupported) {
        // Read-only: give a fan that has not been reported yet one chance to
        // answer before the request is judged against "no fan reported".
        char refreshDetail[128] = {};
        bool refreshed = nvml_read_fans(refreshDetail, sizeof(refreshDetail));
        debug_log("apply fan prevalidation: no fan reported; refresh %s (%s) fanSupported=%d\n",
            refreshed ? "ok" : "failed", refreshDetail[0] ? refreshDetail : "-",
            g_app.fanSupported ? 1 : 0);
    }
    const FanRequestDisposition disposition = app_fan_request_disposition(desired);
    const int effectiveMode = fan_request_effective_mode(desired->fanMode, desired->fanAuto != 0);
    if (disposition == FAN_REQUEST_INERT_NO_FAN) {
        // Owned and already satisfied; apply_fan_settings() writes nothing.
        debug_log("apply fan prevalidation: Auto on a GPU without a controllable fan"
                  " (fanSupported=0 capability=%s) is inert; no fan write\n",
            gpu_capability_name(gpu_capability_get(&g_app.gpuCapability, SERVICE_MUTATION_DOMAIN_FAN)));
        return true;
    }
    if (disposition == FAN_REQUEST_REFUSED_NO_FAN) {
        set_message(result, resultSize, "Fan mode %s cannot be applied: %s",
            fan_request_mode_name(effectiveMode), fan_request_refusal_text());
        debug_log("apply fan prevalidation: REFUSED before any write -- fan mode %s on a GPU"
                  " without a controllable fan (capability=%s)\n",
            fan_request_mode_name(effectiveMode),
            gpu_capability_name(gpu_capability_get(&g_app.gpuCapability, SERVICE_MUTATION_DOMAIN_FAN)));
        return false;
    }
    if (!g_app.fanSupported) {
        // No fan was reported and nothing proves the GPU has none (the probe
        // never answered).  Every fan write below needs a reported fan, so the
        // request can only fail; refuse it now instead of after the VF writes.
        set_message(result, resultSize,
            "Fan mode %s cannot be applied: the driver has not reported this GPU's fans."
            " Nothing was changed.", fan_request_mode_name(effectiveMode));
        debug_log("apply fan prevalidation: REFUSED before any write -- no fan reported and"
                  " fan capability %s (mode %s)\n",
            gpu_capability_name(gpu_capability_get(&g_app.gpuCapability, SERVICE_MUTATION_DOMAIN_FAN)),
            fan_request_mode_name(effectiveMode));
        return false;
    }
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
    if (app_fan_request_disposition(desired) == FAN_REQUEST_INERT_NO_FAN) {
        // fan_request_policy.h: the platform owns cooling and Auto already
        // holds.  No write, no success/failure count, no rollback participation.
        debug_log("apply fan: Auto on a GPU without a controllable fan; nothing to write\n");
        return;
    }
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
