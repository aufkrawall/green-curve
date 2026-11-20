// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Which fan mode Green Curve owns, which one its runtime drives, and whether
// the two agree.  Shared by the Windows service and the Linux daemon.
//
// The service used to fall back to a mirrored `activeFanMode` when its owned
// intent did not claim the fan.  A post-write state refresh re-derives that
// mirror from the intent that is still published -- the OUTGOING one during
// an Apply -- so an Apply that released a fan curve to driver Auto went on
// reporting "Curve" with no runtime running: the GUI showed the profile and its
// curve as active while the fan sat in driver control.  The reported mode now
// comes from the owned intent and the runtime alone, never from that mirror,
// and every Apply ends by checking that the runtime matches the intent.

#ifndef GREEN_CURVE_FAN_INTENT_POLICY_H
#define GREEN_CURVE_FAN_INTENT_POLICY_H

#include "fan_request_policy.h"

// Green Curve's fan mode for one intent: Auto unless the intent claims the
// fan.  An unclaimed fan is not Green Curve's; its stale `fanMode` field (a
// replacement request may carry one) means nothing.
static inline int fan_owned_intent_mode(bool hasFan, int fanMode, bool fanAuto) {
    return hasFan ? fan_request_effective_mode(fanMode, fanAuto) : FAN_MODE_AUTO;
}

// The mode Green Curve's own fan runtime drives right now.  A native zero-RPM
// curve in its driver-automatic phase is still the curve runtime.
static inline int fan_runtime_driven_mode(bool curveRuntimeActive,
                                          bool fixedRuntimeActive) {
    if (curveRuntimeActive) return FAN_MODE_CURVE;
    if (fixedRuntimeActive) return FAN_MODE_FIXED;
    return FAN_MODE_AUTO;
}

// What the service reports as Green Curve's fan mode: the owned intent when
// it claims the fan, otherwise whatever the runtime itself is driving.
static inline int service_reported_fan_intent_mode(bool intentOwnsFan,
    int intentFanMode, bool intentFanAuto, bool curveRuntimeActive,
    bool fixedRuntimeActive) {
    if (intentOwnsFan) return fan_owned_intent_mode(true, intentFanMode, intentFanAuto);
    return fan_runtime_driven_mode(curveRuntimeActive, fixedRuntimeActive);
}

// Whether the Linux daemon fan worker is actively maintaining a custom fan
// mode (Curve or Fixed) on the currently selected GPU. Shared by the worker
// loop and snapshot publication so the two cannot disagree after lockout,
// uncertainty, or an adapter mismatch.
static inline bool linux_daemon_fan_runtime_active(bool gpuReady,
    bool hasActiveDesired, bool stateUncertain, bool targetMatchesSelected,
    bool hasFan, int fanMode, bool fanAuto) {
    if (!gpuReady || !hasActiveDesired || stateUncertain ||
        !targetMatchesSelected || !hasFan)
        return false;
    const int mode = fan_owned_intent_mode(true, fanMode, fanAuto);
    return mode == FAN_MODE_CURVE || mode == FAN_MODE_FIXED;
}

enum FanRuntimeIntentVerdict {
    FAN_RUNTIME_MATCHES_INTENT = 0,
    // The intent owns Fixed/Curve but no runtime drives it.
    FAN_RUNTIME_MISSING,
    // A runtime runs, but in another mode or with other values.
    FAN_RUNTIME_STALE,
    // A runtime runs although the intent leaves the fan to the driver.
    FAN_RUNTIME_UNOWNED,
};

// Transaction-boundary invariant: after a successful Apply the runtime drives
// exactly what the published intent owns.  `valuesMatch` is the caller's
// comparison of the running curve or duty against the intent's (after the same
// normalization and range clamp the apply uses); it is ignored for Auto.
static inline FanRuntimeIntentVerdict fan_runtime_intent_verdict(int ownedMode,
    bool curveRuntimeActive, bool fixedRuntimeActive, bool valuesMatch) {
    const int driven = fan_runtime_driven_mode(curveRuntimeActive, fixedRuntimeActive);
    if (ownedMode == FAN_MODE_AUTO)
        return driven == FAN_MODE_AUTO ? FAN_RUNTIME_MATCHES_INTENT : FAN_RUNTIME_UNOWNED;
    if (driven == FAN_MODE_AUTO) return FAN_RUNTIME_MISSING;
    if (driven != ownedMode || !valuesMatch) return FAN_RUNTIME_STALE;
    return FAN_RUNTIME_MATCHES_INTENT;
}

static inline const char* fan_runtime_intent_verdict_name(FanRuntimeIntentVerdict verdict) {
    switch (verdict) {
        case FAN_RUNTIME_MATCHES_INTENT: return "matches";
        case FAN_RUNTIME_MISSING: return "runtime-missing";
        case FAN_RUNTIME_STALE: return "runtime-stale";
        case FAN_RUNTIME_UNOWNED: return "runtime-unowned";
    }
    return "unknown";
}

#endif  // GREEN_CURVE_FAN_INTENT_POLICY_H
