// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_FAN_CURVE_H
#define GREEN_CURVE_FAN_CURVE_H

#include "app_shared.h"
#include "fan_zero_rpm_policy.h"
#include "fan_zero_rpm_profile_policy.h"

void fan_curve_set_default(FanCurveConfig* config);
void fan_curve_normalize(FanCurveConfig* config);
// F-01-002: the IPC trust boundary's variant. Identical, except that a curve
// with fewer than two enabled points is left alone rather than replaced with
// the built-in default -- the boundary may make a request coherent, but it may
// not invent a fan curve the client never sent.
void fan_curve_normalize_for_ipc(FanCurveConfig* config);
void fan_curve_clamp_percentages(FanCurveConfig* config, int minPct, int maxPct);
bool fan_curve_validate(const FanCurveConfig* config, char* err, size_t errSize);
int fan_curve_active_count(const FanCurveConfig* config);
int fan_curve_interpolate_percent(const FanCurveConfig* config, int temperatureC);
void fan_curve_format_summary(const FanCurveConfig* config, char* buffer, size_t bufferSize);
bool fan_curve_equals(const FanCurveConfig* lhs, const FanCurveConfig* rhs);
bool fan_curve_has_high_temp_low_fan_warning(const FanCurveConfig* config);

#endif
