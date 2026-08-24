// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Returns true when native zero-RPM owns this tick (including a handled
// failure).  False leaves the ordinary manual-curve path in charge.
static bool apply_fan_curve_zero_rpm_tick(const FanCurveConfig* curve,
    int currentTempC, bool hasLastAppliedTemp, int lastAppliedTempC,
    int lastAppliedPercent, bool driverAutoActive, ULONGLONG lastApplyTickMs,
    int consecutiveFailures, ULONGLONG now, char* detail, size_t detailSize) {
    if (!curve || !curve->zeroRpmEnabled) return false;
    int startC = fan_curve_first_enabled_temperature(curve);
    int stopC = startC - fan_curve_zero_rpm_hysteresis(curve);
    if (stopC < 0) stopC = 0;
    FanRuntimeState runtime = {};
    runtime.initialized = hasLastAppliedTemp;
    runtime.lastTemperatureC = lastAppliedTempC;
    runtime.lastPercent = lastAppliedPercent;
    runtime.driverAutoActive = driverAutoActive;
    FanRuntimeDecision decision = fan_runtime_next_action(
        &runtime, curve, currentTempC, false);
    if (!decision.useDriverAuto) return false;

    ULONGLONG reapplyMs = FAN_RUNTIME_REAPPLY_INTERVAL_MS;
    if (consecutiveFailures > 0) {
        unsigned int factor = 1u <<
            (consecutiveFailures > 4 ? 4 : consecutiveFailures);
        reapplyMs *= factor;
        if (reapplyMs > 120000) reapplyMs = 120000;
    }
    bool timerExpired = lastApplyTickMs == 0 ||
        now - lastApplyTickMs >= reapplyMs;
    if (!decision.shouldWrite && !timerExpired) return true;

    bool alreadyAuto = nvml_read_fans(detail, detailSize) && g_app.fanIsAuto;
    if (!alreadyAuto && !nvml_set_fan_auto(detail, detailSize)) {
        EnterCriticalSection(&g_appLock);
        handle_fan_runtime_failure(
            "Fan curve native zero-RPM handback failed", detail);
        LeaveCriticalSection(&g_appLock);
        return true;
    }

    EnterCriticalSection(&g_appLock);
    if (!g_app.fanCurveRuntimeActive) {
        LeaveCriticalSection(&g_appLock);
        return true;
    }
    bool transitioned = !g_app.fanCurveDriverAutoActive;
    g_app.activeFanMode = FAN_MODE_CURVE;
    g_app.activeFanFixedPercent = 0;
    g_app.fanCurveLastAppliedPercent = 0;
    g_app.fanCurveLastAppliedTempC = currentTempC;
    g_app.fanCurveHasLastAppliedTemp = true;
    g_app.fanCurveDriverAutoActive = true;
    mark_fan_runtime_success(now);
    if (transitioned) {
        debug_log("fan curve transition: temp=%dC "
            "control=driver-auto/native-zero-RPM zeroRpmGap=%dC "
            "start=%dC stop=%dC\n",
            currentTempC, fan_curve_zero_rpm_hysteresis(curve),
            startC, stopC);
    }
    if (g_app.isServiceProcess) {
        populate_control_state(&g_serviceControlState);
        g_serviceControlStateValid = true;
    }
    sync_fan_ui_from_cached_state(window_should_redraw_fan_controls());
    LeaveCriticalSection(&g_appLock);
    return true;
}
