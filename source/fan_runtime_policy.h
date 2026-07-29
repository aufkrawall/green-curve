// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_FAN_RUNTIME_POLICY_H
#define GREEN_CURVE_FAN_RUNTIME_POLICY_H

struct FanRuntimeState {
    bool initialized;
    int lastTemperatureC;
    int lastPercent;
};

// ---------------------------------------------------------------------------
// Manual fan write verification (platform-neutral)
// ---------------------------------------------------------------------------
//
// There are two different "fan percent" readbacks and confusing them silently
// breaks manual fan control:
//
//   * the *measured* duty (`nvmlDeviceGetFanSpeed_v2`) is telemetry.  It lags
//     spin-up by seconds, overshoots the request while the fan accelerates, and
//     reads 0 for as long as a zero-RPM fan stop is in effect.  It is NOT a
//     readback of the write.
//   * the *intended* duty (`nvmlDeviceGetTargetFanSpeed`) is the driver echoing
//     the duty it accepted.  That is the readback.
//
// The Linux backend used to gate manual fan writes on `measured == requested`,
// so every fixed/curve apply on an idle GPU failed and rolled back even though
// the driver had accepted the write.  Verify against the intent, and only fall
// back to a tolerance window on the measured value when the driver exposes no
// intent getter.
static const int FAN_MANUAL_READBACK_TOLERANCE_PERCENT = 2;

// The driver silently clamps a manual duty into the range reported by
// `nvmlDeviceGetMinMaxFanSpeed` (an RTX 5070 reports 30..100 and answers a
// write of 10% with an intent of 30%).  Clamp before writing so the verified
// value is the value the hardware will actually hold — the same "the driver
// snapped our request, that is not a failure" contract the memory-offset grid
// already uses.
static inline int fan_manual_effective_percent(int requestedPercent,
                                               int hardwareMinPercent,
                                               int hardwareMaxPercent,
                                               bool rangeKnown) {
    int pct = requestedPercent;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (!rangeKnown) return pct;
    if (hardwareMinPercent < 0) hardwareMinPercent = 0;
    if (hardwareMaxPercent > 100) hardwareMaxPercent = 100;
    if (hardwareMaxPercent < hardwareMinPercent) return pct;
    if (pct < hardwareMinPercent) pct = hardwareMinPercent;
    if (pct > hardwareMaxPercent) pct = hardwareMaxPercent;
    return pct;
}

static inline bool fan_percent_within_readback_tolerance(int wantPercent,
                                                         int gotPercent) {
    int delta = gotPercent - wantPercent;
    if (delta < 0) delta = -delta;
    return delta <= FAN_MANUAL_READBACK_TOLERANCE_PERCENT;
}

// `intendedKnown` is false when the driver exposes no intent getter at all, not
// merely when the intent happens to be 0 — a genuine 0 intent still confirms a
// 0 write.
static inline bool fan_manual_write_confirmed(int wantPercent,
                                              int measuredPercent,
                                              int intendedPercent,
                                              bool intendedKnown) {
    if (intendedKnown &&
        fan_percent_within_readback_tolerance(wantPercent, intendedPercent))
        return true;
    // No intent readback: a stopped fan reads 0 under driver auto too, so a
    // 0% request cannot be confirmed by a tolerance window around 0.
    if (wantPercent == 0) return measuredPercent == 0;
    return fan_percent_within_readback_tolerance(wantPercent, measuredPercent);
}

// Why a shared reducer: the runtime holds the fan at a manual duty.  Every path
// that stops tracking temperature — a refused write *or* a refused temperature
// read — leaves that duty pinned, so both must escalate identically.  Windows
// already counted both; the Linux daemon silently ignored telemetry loss.
enum FanRuntimeOutcome {
    FAN_RUNTIME_OUTCOME_SUCCESS = 0,
    FAN_RUNTIME_OUTCOME_WRITE_FAILED = 1,
    FAN_RUNTIME_OUTCOME_TELEMETRY_FAILED = 2,
};

enum FanRuntimeEscalation {
    FAN_RUNTIME_ESCALATION_NONE = 0,
    FAN_RUNTIME_ESCALATION_RESTORE_AUTO = 1,
    FAN_RUNTIME_ESCALATION_EMERGENCY_MAX = 2,
};

struct FanRuntimeFailureDecision {
    unsigned int failureCount;
    FanRuntimeEscalation escalation;
    bool shouldLogFailure;
};

static const unsigned int FAN_RUNTIME_DEFAULT_FAILURE_LIMIT = 3;
static const int FAN_RUNTIME_EMERGENCY_PERCENT = 100;

static inline FanRuntimeFailureDecision fan_runtime_observe_result(
    unsigned int previousFailureCount, FanRuntimeOutcome outcome,
    unsigned int failureLimit) {
    FanRuntimeFailureDecision decision = {};
    if (failureLimit == 0) failureLimit = FAN_RUNTIME_DEFAULT_FAILURE_LIMIT;
    if (outcome == FAN_RUNTIME_OUTCOME_SUCCESS) {
        decision.failureCount = 0;
        decision.escalation = FAN_RUNTIME_ESCALATION_NONE;
        decision.shouldLogFailure = false;
        return decision;
    }
    unsigned int next = previousFailureCount;
    if (next < 0xFFFFFFFFu) next++;   // saturate rather than wrap back to zero
    decision.failureCount = next;
    decision.shouldLogFailure = true;
    decision.escalation = next >= failureLimit
        ? FAN_RUNTIME_ESCALATION_RESTORE_AUTO
        : FAN_RUNTIME_ESCALATION_NONE;
    return decision;
}

// The driver refused to take the fan back.  A stale manual duty is the one
// outcome that can overheat a GPU unattended, so force maximum cooling.
static inline FanRuntimeEscalation fan_runtime_escalation_after_auto_restore(
    bool autoRestoreSucceeded) {
    return autoRestoreSucceeded ? FAN_RUNTIME_ESCALATION_NONE
                                : FAN_RUNTIME_ESCALATION_EMERGENCY_MAX;
}

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
