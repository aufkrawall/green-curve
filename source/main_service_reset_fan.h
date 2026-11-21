// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once

// Reset holds the service runtime lock, so the worker cannot reassert its
// duty during handoff. Keep it available if the handoff fails partway.
static bool service_reset_fan_to_driver(char* detail, size_t detailSize) {
    const bool running = g_app.fanCurveRuntimeActive || g_app.fanFixedRuntimeActive;
    if (!running && g_app.fanIsAuto && g_app.activeFanMode == FAN_MODE_AUTO)
        return true;
    if (!nvml_set_fan_auto(detail, detailSize) || !live_fan_policy_readback_complete()) {
        if (!detail[0]) set_message(detail, detailSize, "Driver Auto could not be verified");
        debug_log("reset fan: driver handoff failed, runtime retained=%d (%s)\n", running, detail);
        return false;
    }
    stop_fan_curve_runtime();
    g_app.activeFanMode = FAN_MODE_AUTO;
    g_app.activeFanFixedPercent = 0;
    debug_log("reset fan: driver handoff verified, previous runtime=%d\n", running);
    return true;
}

static void service_reset_retain_running_fan() {
    initialize_desired_settings_defaults(&g_serviceActiveDesired);
    g_serviceActiveDesired.lockCi = -1;
    g_serviceActiveDesired.hasFan = true;
    g_serviceActiveDesired.fanAuto = false;
    g_serviceActiveDesired.fanMode = g_app.fanCurveRuntimeActive ? FAN_MODE_CURVE : FAN_MODE_FIXED;
    g_serviceActiveDesired.fanPercent = g_app.activeFanFixedPercent;
    g_serviceActiveDesired.fanCurve = g_app.activeFanCurve;
    g_serviceHasActiveDesired = true;
    g_serviceActiveDesiredGpu = g_app.selectedGpu;
}
