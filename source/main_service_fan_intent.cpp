// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// The service's fan transaction-boundary invariant (fan_intent_policy.h).
//
// After a successful Apply the fan runtime must drive exactly what the
// published intent owns: a Fixed/Curve intent has its runtime running with the
// intent's duty or curve, and an Auto or unclaimed fan has none.  An Apply once
// released a running curve to driver Auto while the intent and the GUI went on
// showing that curve, and nothing noticed until the user saw the fan standing
// still.  This check makes such a divergence impossible to leave behind
// silently: it is logged, the owned state is re-established through the same
// fan step the Apply uses, and the outcome is raised to a warning (repaired)
// or an error (could not be repaired) so the user is told.
//
// Called with the service runtime lock held and WITHOUT g_appLock, like the
// fan step inside the apply.

enum ServiceFanIntentEnforcement {
    SERVICE_FAN_INTENT_HELD = 0,
    SERVICE_FAN_INTENT_REPAIRED,
    SERVICE_FAN_INTENT_REPAIR_FAILED,
};

static int service_owned_fan_mode() {
    return fan_owned_intent_mode(g_serviceHasActiveDesired && g_serviceActiveDesired.hasFan,
        g_serviceActiveDesired.fanMode, g_serviceActiveDesired.fanAuto != 0);
}

static FanRuntimeIntentVerdict service_fan_runtime_intent_verdict(int ownedMode) {
    bool valuesMatch = true;
    if (ownedMode == FAN_MODE_CURVE) {
        // The same normalization and range clamp apply_fan_settings() stores.
        FanCurveConfig wanted = g_serviceActiveDesired.fanCurve;
        fan_curve_normalize(&wanted);
        if (g_app.fanRangeKnown)
            fan_curve_clamp_percentages(&wanted, (int)g_app.fanMinPct, (int)g_app.fanMaxPct);
        valuesMatch = fan_curve_equals(&wanted, &g_app.activeFanCurve);
    } else if (ownedMode == FAN_MODE_FIXED) {
        valuesMatch = clamp_percent(g_serviceActiveDesired.fanPercent) ==
            clamp_percent(g_app.activeFanFixedPercent);
    }
    return fan_runtime_intent_verdict(ownedMode, g_app.fanCurveRuntimeActive != 0,
        g_app.fanFixedRuntimeActive != 0, valuesMatch);
}

static ServiceFanIntentEnforcement service_enforce_fan_runtime_matches_intent(
    const char* origin, char* note, size_t noteSize) {
    if (note && noteSize) note[0] = '\0';
    const int ownedMode = service_owned_fan_mode();
    const FanRuntimeIntentVerdict before = service_fan_runtime_intent_verdict(ownedMode);
    if (before == FAN_RUNTIME_MATCHES_INTENT) {
        debug_log_on_change("fan intent invariant: %s holds (owned=%s runtime=%s)\n",
            origin ? origin : "?", fan_mode_label(ownedMode),
            fan_mode_label(fan_runtime_driven_mode(g_app.fanCurveRuntimeActive,
                g_app.fanFixedRuntimeActive)));
        return SERVICE_FAN_INTENT_HELD;
    }
    debug_log("FAN INVARIANT VIOLATION: origin=%s verdict=%s owned=%s runtime=%s"
              " activeFanMode=%s fanIsAuto=%d; re-establishing the owned fan state\n",
        origin ? origin : "?", fan_runtime_intent_verdict_name(before),
        fan_mode_label(ownedMode),
        fan_mode_label(fan_runtime_driven_mode(g_app.fanCurveRuntimeActive,
            g_app.fanFixedRuntimeActive)),
        fan_mode_label(g_app.activeFanMode), g_app.fanIsAuto ? 1 : 0);

    DesiredSettings repair = {};
    initialize_desired_settings_defaults(&repair);
    repair.hasFan = true;
    repair.fanMode = ownedMode;
    repair.fanAuto = ownedMode == FAN_MODE_AUTO;
    repair.fanPercent = ownedMode == FAN_MODE_FIXED ? g_serviceActiveDesired.fanPercent : 0;
    repair.fanCurve = ownedMode == FAN_MODE_CURVE
        ? g_serviceActiveDesired.fanCurve : g_app.activeFanCurve;
    char failure[512] = {};
    char message[256] = {};
    int successCount = 0, failCount = 0;
    bool fanChanged = false;
    if (validate_desired_fan_settings_for_apply(&repair, failure, sizeof(failure))) {
        apply_fan_settings(&repair, failure, sizeof(failure), successCount, failCount,
            message, sizeof(message), fanChanged);
    } else {
        failCount = 1;
    }
    const FanRuntimeIntentVerdict after = service_fan_runtime_intent_verdict(ownedMode);
    const bool repaired = failCount == 0 && after == FAN_RUNTIME_MATCHES_INTENT;
    debug_log("fan intent invariant: %s repair %s (verdict %s -> %s changed=%d ok=%d failed=%d%s%s)\n",
        origin ? origin : "?", repaired ? "SUCCEEDED" : "FAILED",
        fan_runtime_intent_verdict_name(before), fan_runtime_intent_verdict_name(after),
        fanChanged ? 1 : 0, successCount, failCount,
        failure[0] ? " detail=" : "", failure);
    if (repaired) {
        set_message(note, noteSize,
            "Fan control had drifted from the applied profile and was re-established (%s).",
            fan_mode_label(ownedMode));
        return SERVICE_FAN_INTENT_REPAIRED;
    }
    set_message(note, noteSize,
        "Fan control does not match the applied profile (%s requested, %s running)%s%s",
        fan_mode_label(ownedMode),
        fan_mode_label(fan_runtime_driven_mode(g_app.fanCurveRuntimeActive,
            g_app.fanFixedRuntimeActive)),
        failure[0] ? ": " : "", failure);
    return SERVICE_FAN_INTENT_REPAIR_FAILED;
}
