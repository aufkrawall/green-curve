// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_FAN_ZERO_RPM_POLICY_H
#define GREEN_CURVE_FAN_ZERO_RPM_POLICY_H

#include "gpu_core.h"

static const int FAN_ZERO_RPM_MIN_HYSTERESIS_C = 2;

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
    int hysteresis = config ? config->hysteresisC
                            : FAN_ZERO_RPM_MIN_HYSTERESIS_C;
    if (hysteresis < FAN_ZERO_RPM_MIN_HYSTERESIS_C)
        hysteresis = FAN_ZERO_RPM_MIN_HYSTERESIS_C;
    if (hysteresis > FAN_CURVE_MAX_HYSTERESIS_C)
        hysteresis = FAN_CURVE_MAX_HYSTERESIS_C;
    return hysteresis;
}

static inline bool fan_curve_wire_flags_valid(
    const FanCurveConfig* config) {
    if (!config || config->zeroRpmEnabled > 1) return false;
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; ++i)
        if (config->points[i].enabled > 1) return false;
    for (unsigned int i = 0; i < sizeof(config->zeroRpmReserved); ++i)
        if (config->zeroRpmReserved[i] != 0) return false;
    return true;
}

static inline void validate_fan_curve_flags_for_ipc(FanCurveConfig* config) {
    if (!config) return;
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; ++i)
        canonicalize_gc_bool8(&config->points[i].enabled);
    canonicalize_gc_bool8(&config->zeroRpmEnabled);
    config->zeroRpmReserved[0] = 0;
    config->zeroRpmReserved[1] = 0;
    config->zeroRpmReserved[2] = 0;
}

#endif // GREEN_CURVE_FAN_ZERO_RPM_POLICY_H
