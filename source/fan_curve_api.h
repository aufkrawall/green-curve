// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_FAN_CURVE_API_H
#define GREEN_CURVE_FAN_CURVE_API_H
#include <stddef.h>
#include "gpu_core.h"

void fan_curve_set_default(FanCurveConfig* config);
void fan_curve_normalize(FanCurveConfig* config);
// The IPC variant preserves an incomplete curve for boundary validation;
// normalization must never invent a curve the client did not send.
void fan_curve_normalize_for_ipc(FanCurveConfig* config);
void fan_curve_clamp_percentages(FanCurveConfig* config, int minPct, int maxPct);
bool fan_curve_validate(const FanCurveConfig* config, char* err, size_t errSize);
int fan_curve_active_count(const FanCurveConfig* config);
int fan_curve_interpolate_percent(const FanCurveConfig* config, int temperatureC);
void fan_curve_format_summary(const FanCurveConfig* config, char* buffer, size_t bufferSize);
bool fan_curve_equals(const FanCurveConfig* lhs, const FanCurveConfig* rhs);
bool fan_curve_has_high_temp_low_fan_warning(const FanCurveConfig* config);
#endif
