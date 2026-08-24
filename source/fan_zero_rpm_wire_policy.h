// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_FAN_ZERO_RPM_WIRE_POLICY_H
#define GREEN_CURVE_FAN_ZERO_RPM_WIRE_POLICY_H

// Internal gpu_core.h shard, included after FanCurveConfig is complete. Keep
// wire canonicalization in the data model without making the runtime policy
// include back into gpu_core.h.
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
}

#endif // GREEN_CURVE_FAN_ZERO_RPM_WIRE_POLICY_H
