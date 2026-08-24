// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_LINUX_CLI_FAN_OVERRIDE_POLICY_H
#define GREEN_CURVE_LINUX_CLI_FAN_OVERRIDE_POLICY_H

#include "gpu_core.h"

#include <stddef.h>
#include <string.h>

void fan_curve_normalize(FanCurveConfig* config);
bool fan_curve_validate(const FanCurveConfig* config, char* err,
                        size_t errSize);

// DesiredSettings intentionally describes a complete fan control.  Command-line
// overrides are different: `--fan-zero-rpm 1`, for example, must not replace a
// stored custom curve with the parser's default curve.  Keep an explicit field
// mask so zero is a value rather than "not supplied" and merge only the options
// the user actually wrote.
struct LinuxFanCliOverrideMask {
    bool mode;
    bool fixedPercent;
    bool pollInterval;
    bool hysteresis;
    bool zeroRpm;
    bool pointEnabled[FAN_CURVE_MAX_POINTS];
    bool pointTemperature[FAN_CURVE_MAX_POINTS];
    bool pointPercent[FAN_CURVE_MAX_POINTS];
};

static inline void merge_linux_cli_fan_overrides(
    DesiredSettings* base,
    const DesiredSettings* incoming,
    const LinuxFanCliOverrideMask* mask) {
    if (!base || !incoming || !mask || !incoming->hasFan) return;

    base->hasFan = true;
    if (mask->mode) {
        base->fanMode = incoming->fanMode;
        base->fanAuto = incoming->fanMode == FAN_MODE_AUTO;
    }
    if (mask->fixedPercent) base->fanPercent = incoming->fanPercent;
    if (mask->pollInterval)
        base->fanCurve.pollIntervalMs = incoming->fanCurve.pollIntervalMs;
    if (mask->hysteresis)
        base->fanCurve.hysteresisC = incoming->fanCurve.hysteresisC;
    if (mask->zeroRpm) {
        base->fanCurve.zeroRpmEnabled = incoming->fanCurve.zeroRpmEnabled;
        memset(base->fanCurve.zeroRpmReserved, 0,
               sizeof(base->fanCurve.zeroRpmReserved));
    }
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; ++i) {
        if (mask->pointEnabled[i])
            base->fanCurve.points[i].enabled =
                incoming->fanCurve.points[i].enabled;
        if (mask->pointTemperature[i])
            base->fanCurve.points[i].temperatureC =
                incoming->fanCurve.points[i].temperatureC;
        if (mask->pointPercent[i])
            base->fanCurve.points[i].fanPercent =
                incoming->fanCurve.points[i].fanPercent;
    }
}

// Apply and validate transactionally.  This turns an invalid combination such
// as two enabled points at the same temperature into a useful CLI error instead
// of letting presentation normalization silently replace the whole curve with
// defaults.
static inline bool apply_linux_cli_fan_overrides(
    DesiredSettings* base,
    const DesiredSettings* incoming,
    const LinuxFanCliOverrideMask* mask,
    char* err,
    size_t errSize) {
    if (!base || !incoming || !mask) return false;
    if (!incoming->hasFan) return true;

    DesiredSettings candidate = *base;
    merge_linux_cli_fan_overrides(&candidate, incoming, mask);
    fan_curve_normalize(&candidate.fanCurve);
    if (!fan_curve_validate(&candidate.fanCurve, err, errSize)) return false;
    *base = candidate;
    return true;
}

#endif // GREEN_CURVE_LINUX_CLI_FAN_OVERRIDE_POLICY_H
