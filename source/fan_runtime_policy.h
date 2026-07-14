// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_FAN_RUNTIME_POLICY_H
#define GREEN_CURVE_FAN_RUNTIME_POLICY_H

struct FanRuntimeState {
    bool initialized;
    int lastTemperatureC;
    int lastPercent;
};

struct FanRuntimeDecision {
    bool shouldWrite;
    int targetPercent;
    unsigned int nextPollMs;
};

static inline FanRuntimeDecision fan_runtime_next_action(
    FanRuntimeState* state, const FanCurveConfig* curve,
    int temperatureC, bool forceTargetRefresh) {
    FanRuntimeDecision decision = {};
    if (!state || !curve) return decision;
    int pollMs = curve->pollIntervalMs;
    if (pollMs < 250) pollMs = 250;
    decision.nextPollMs = (unsigned int)pollMs;
    int interpolated = fan_curve_interpolate_percent(curve, temperatureC);
    if (interpolated < 0) interpolated = 0;
    if (interpolated > 100) interpolated = 100;
    int hysteresis = curve->hysteresisC;
    if (hysteresis < 0) hysteresis = 0;
    if (hysteresis > 10) hysteresis = 10;
    bool temperatureRose = state->initialized &&
        temperatureC > state->lastTemperatureC;
    bool cooledPastHysteresis = state->initialized &&
        temperatureC <= state->lastTemperatureC - hysteresis;
    if (!state->initialized || forceTargetRefresh || temperatureRose ||
        cooledPastHysteresis) {
        state->lastTemperatureC = temperatureC;
        state->lastPercent = interpolated;
        state->initialized = true;
    }
    // Reassert the selected manual target each configured poll.  Hysteresis
    // controls target changes, not whether an external controller can steal it.
    decision.shouldWrite = true;
    decision.targetPercent = state->lastPercent;
    return decision;
}

#endif // GREEN_CURVE_FAN_RUNTIME_POLICY_H
