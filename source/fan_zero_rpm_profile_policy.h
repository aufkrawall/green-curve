// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_FAN_ZERO_RPM_PROFILE_POLICY_H
#define GREEN_CURVE_FAN_ZERO_RPM_PROFILE_POLICY_H

#include "fan_zero_rpm_policy.h"

// v23 profile files stored one shared hysteresis. Returning true lets each
// platform log the one-time compatibility migration before its next save
// writes the dedicated v24 key.
static inline bool fan_curve_migrate_legacy_zero_rpm_hysteresis(
    FanCurveConfig* config, bool hasExplicitZeroRpmHysteresis) {
    if (!config || !config->zeroRpmEnabled ||
        hasExplicitZeroRpmHysteresis) return false;
    int gap = config->hysteresisC;
    if (gap < FAN_ZERO_RPM_MIN_HYSTERESIS_C)
        gap = FAN_ZERO_RPM_MIN_HYSTERESIS_C;
    if (gap > FAN_ZERO_RPM_MAX_HYSTERESIS_C)
        gap = FAN_ZERO_RPM_MAX_HYSTERESIS_C;
    config->zeroRpmHysteresisC = (gc_u8)gap;
    return true;
}

#endif // GREEN_CURVE_FAN_ZERO_RPM_PROFILE_POLICY_H
