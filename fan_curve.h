#ifndef GREEN_CURVE_FAN_CURVE_H
#define GREEN_CURVE_FAN_CURVE_H

#include "app_shared.h"

void fan_curve_set_default(FanCurveConfig* config);
void fan_curve_normalize(FanCurveConfig* config);
bool fan_curve_validate(const FanCurveConfig* config, char* err, size_t errSize);
int fan_curve_active_count(const FanCurveConfig* config);
int fan_curve_interpolate_percent(const FanCurveConfig* config, int temperatureC);
void fan_curve_format_summary(const FanCurveConfig* config, char* buffer, size_t bufferSize);
bool fan_curve_equals(const FanCurveConfig* lhs, const FanCurveConfig* rhs);

#endif