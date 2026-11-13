// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_CONVERSION_GRID_H
#define GREEN_CURVE_PROFILE_CONVERSION_GRID_H
#include "tuning_units.h"

struct ProfileConversionGrid {
    bool known = false;
    long long step = 0, origin = 0;
};
struct ProfileGridProjection {
    bool valid = false, exact = false;
    long long lower = 0;
    unsigned long long error = 0;
};

// Floor onto an arbitrary-origin grid, including negative offsets and signed
// minima, without overflowing value-origin or silently truncating toward zero.
static inline ProfileGridProjection profile_grid_lower(long long value, ProfileConversionGrid grid) {
    ProfileGridProjection result;
    if (!grid.known || grid.step <= 0) return result;
    long long valueRemainder = value % grid.step, originRemainder = grid.origin % grid.step;
    if (valueRemainder < 0) valueRemainder += grid.step;
    if (originRemainder < 0) originRemainder += grid.step;
    long long remainder = valueRemainder - originRemainder;
    if (remainder < 0) remainder += grid.step;
    if (value < LLONG_MIN + remainder) return result;
    result.valid = true; result.exact = remainder == 0;
    result.lower = value - remainder; result.error = (unsigned long long)remainder;
    return result;
}

struct ProfilePowerConversion {
    bool valid = false, exact = false;
    TuningPowerRatio derivedRatio{};
    long long lowerMw = 0;
    unsigned long long maximumErrorMw = 0;
};

static inline ProfilePowerConversion profile_power_lower(TuningPowerRatio source, long long destinationDefaultMw,
    ProfileConversionGrid grid) {
    ProfilePowerConversion result;
    const auto ideal = tuning_power_ratio_resolve(source, destinationDefaultMw);
    if (ideal.overflowed) return result;
    const auto lower = profile_grid_lower(ideal.requestedMw, grid);
    if (!lower.valid || lower.lower <= 0) return result;
    result.valid = true; result.exact = ideal.exact && lower.exact;
    result.lowerMw = lower.lower;
    result.derivedRatio = tuning_power_ratio_reduce(lower.lower, destinationDefaultMw);
    // Ceiling of the exact error bounds a fractional-mW ideal without rounding
    // the requested limit up. It is a report only, never the applied target.
    result.maximumErrorMw = lower.error + (ideal.exact ? 0ull : 1ull);
    return result;
}
#endif
