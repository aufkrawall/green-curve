// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_CURVE_PRESERVATION_PRECISION_H
#define GREEN_CURVE_CURVE_PRESERVATION_PRECISION_H
#include "gpu_core.h"
#include "vf_offset_range_policy.h"
#include "vf_curve_units.h"

// A whole-kHz projection can be zero while the programmed control is not.
static inline bool curve_control_nonzero(int khz, bool exactValid, gc_i64 hz) {
    return exactValid ? hz != 0 : khz != 0;
}
static inline bool curve_preservation_targets(const DesiredSettings& desired, const bool* mask,
    const bool* lockTail, const int* targetsKhz, const int* originalKhz, const gc_i64* originalHz,
    bool originalExact, gc_i64* targetsHz) {
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        targetsHz[i] = (gc_i64)targetsKhz[i] * 1000;
        if (!mask[i]) continue;
        if (desired.curvePointCaptured[i]) targetsHz[i] = desired.curvePointCapturedOffsetHz[i];
        else if (!desired_curve_point_present(&desired, i) && !lockTail[i]) {
            if (!originalExact || originalHz[i] / 1000 != originalKhz[i]) return false;
            targetsHz[i] += originalHz[i] - (gc_i64)originalKhz[i] * 1000;
        }
    }
    return true;
}
static inline bool curve_exact_targets_representable(const VfBackendSpec* backend, const VfOffsetRange& range,
    const bool* mask, const gc_i64* hz, int* firstBad) {
    if (!backend || !backend->writeSupported) return false;
    for (int i = 0; i < VF_NUM_POINTS; ++i) if (mask[i]) {
        int raw = 0;
        if (hz[i] < (gc_i64)range.minKHz * 1000 || hz[i] > (gc_i64)range.maxKHz * 1000 ||
            !vf_graphics_offset_driver_units_hz(backend, hz[i], &raw)) {
            if (firstBad) *firstBad = i;
            return false;
        }
    }
    return true;
}
#endif
