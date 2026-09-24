// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_FAN_FIXED_MAINTENANCE_POLICY_H
#define GREEN_CURVE_FAN_FIXED_MAINTENANCE_POLICY_H

// Whether the Linux daemon must re-assert a FIXED fan duty.
//
// The daemon's fan worker used to run for curve mode only.  A fixed duty was
// written once by the Apply and never looked at again, although the daemon's
// own startup comment said the worker kept "a curve or fixed duty" asserted:
// after a GPU reset or anything else that hands the fan back to the driver,
// the UI went on reporting "Fixed N%" while the driver ran its own policy.
// Windows re-asserts fixed duty on its own cadence; this is the Linux side.
//
// One fan's observation in, one decision out.  Pure, so it runs in the
// regression harness on either host.
struct FanFixedMaintenanceInputs {
    bool fixedModeActive;  // committed intent is FAN_MODE_FIXED
    int targetPercent;     // the effective duty the Apply wrote (driver-clamped)
    bool policyKnown;      // getFanControlPolicy answered
    bool policyManual;     // ... and it is the manual policy
    bool intentKnown;      // the driver exposes an intent getter and it answered
    int intentPercent;     // ... with this duty
};

struct FanFixedMaintenanceDecision {
    bool write;    // re-assert the fixed duty
    bool failure;  // the observation itself failed (counts toward escalation)
    const char* reason;
};

// A readback within this many percent is the same duty (driver rounding).
static const int FAN_FIXED_MAINTENANCE_TOLERANCE_PCT = 1;

static inline FanFixedMaintenanceDecision fan_fixed_maintenance_decide(
    FanFixedMaintenanceInputs in) {
    FanFixedMaintenanceDecision d = {false, false, "holding"};
    if (!in.fixedModeActive) {
        d.reason = "not in fixed mode";
        return d;
    }
    if (!in.policyKnown) {
        // Never write blind: an unreadable policy is a telemetry failure, and
        // repeated ones escalate like any other fan-runtime failure.
        d.failure = true;
        d.reason = "fan control policy unreadable";
        return d;
    }
    if (!in.policyManual) {
        d.write = true;
        d.reason = "driver took the fan back";
        return d;
    }
    int target = in.targetPercent < 0 ? 0 : (in.targetPercent > 100 ? 100 : in.targetPercent);
    if (in.intentKnown) {
        int delta = in.intentPercent - target;
        if (delta < 0) delta = -delta;
        if (delta > FAN_FIXED_MAINTENANCE_TOLERANCE_PCT) {
            d.write = true;
            d.reason = "duty drifted from the fixed target";
        }
    }
    return d;
}

#endif
