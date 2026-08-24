// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_FAN_ZERO_RPM_POLICY_H
#define GREEN_CURVE_FAN_ZERO_RPM_POLICY_H

#include "gpu_core.h"

static inline int fan_curve_first_enabled_temperature(
    const FanCurveConfig* config) {
    if (!config) return 0;
    int first = 101;
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; ++i) {
        if (config->points[i].enabled &&
            config->points[i].temperatureC < first)
            first = config->points[i].temperatureC;
    }
    if (first > 100) return 0;
    return first < 0 ? 0 : first;
}

static inline int fan_curve_zero_rpm_hysteresis(
    const FanCurveConfig* config) {
    int hysteresis = config ? (int)config->zeroRpmHysteresisC
                            : FAN_ZERO_RPM_DEFAULT_HYSTERESIS_C;
    if (hysteresis < FAN_ZERO_RPM_MIN_HYSTERESIS_C)
        hysteresis = FAN_ZERO_RPM_MIN_HYSTERESIS_C;
    if (hysteresis > FAN_ZERO_RPM_MAX_HYSTERESIS_C)
        hysteresis = FAN_ZERO_RPM_MAX_HYSTERESIS_C;
    return hysteresis;
}

#endif // GREEN_CURVE_FAN_ZERO_RPM_POLICY_H
