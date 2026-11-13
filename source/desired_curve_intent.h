// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_DESIRED_CURVE_INTENT_H
#define GREEN_CURVE_DESIRED_CURVE_INTENT_H

#include <limits.h>
#include "curve_point_offset_policy.h"

// Shared interpretation of the runtime point fields. Captured offsets never
// acquire authority from a sampled frequency or from the graphics scalar.
enum DesiredCurvePointKind {
    DESIRED_CURVE_ABSENT, DESIRED_CURVE_ABSOLUTE, DESIRED_CURVE_PROJECTED,
    DESIRED_CURVE_CAPTURED, DESIRED_CURVE_INVALID,
};

template <class Settings>
static inline DesiredCurvePointKind desired_curve_point_kind(const Settings* d, int ci) {
    if (!d || ci < 0 || ci >= VF_NUM_POINTS) return DESIRED_CURVE_INVALID;
    if (d->hasCurvePoint[ci] > 1 || d->curvePointFromGpuOffset[ci] > 1 ||
        d->curvePointCaptured[ci] > 1) return DESIRED_CURVE_INVALID;
    if (d->curvePointCaptured[ci]) {
        if (d->hasCurvePoint[ci] || d->curvePointFromGpuOffset[ci] || d->curvePointMHz[ci])
            return DESIRED_CURVE_INVALID;
        return DESIRED_CURVE_CAPTURED;
    }
    if (!d->hasCurvePoint[ci]) return DESIRED_CURVE_ABSENT;
    return d->curvePointFromGpuOffset[ci] ? DESIRED_CURVE_PROJECTED : DESIRED_CURVE_ABSOLUTE;
}

template <class Settings>
static inline bool desired_curve_point_present(const Settings* d, int ci) {
    return d && ci >= 0 && ci < VF_NUM_POINTS &&
        (d->hasCurvePoint[ci] || d->curvePointCaptured[ci]);
}

template <class Settings>
static inline bool desired_curve_point_has_target(const Settings* d, int ci) {
    const DesiredCurvePointKind kind = desired_curve_point_kind(d, ci);
    return kind == DESIRED_CURVE_CAPTURED || kind == DESIRED_CURVE_PROJECTED ||
        (kind == DESIRED_CURVE_ABSOLUTE &&
            (d->authoring.pointFrequencyPrecise[ci] ? d->authoring.pointFrequencyHz[ci] > 0 : d->curvePointMHz[ci] > 0));
}

template <class Settings>
static inline bool desired_curve_point_offset_khz(const Settings* d, int ci,
    long long liveBaseKHz, int gpuComponentKHz, long long* out) {
    if (!out) return false;
    const DesiredCurvePointKind kind = desired_curve_point_kind(d, ci);
    if (kind == DESIRED_CURVE_INVALID || kind == DESIRED_CURVE_ABSENT) return false;
    // This adapter has only a kHz baseline. Exact absolute authority needs an
    // exact baseline and must never be silently projected through whole MHz.
    if (kind == DESIRED_CURVE_ABSOLUTE && d->authoring.pointFrequencyPrecise[ci]) return false;
    if (kind == DESIRED_CURVE_CAPTURED) {
        const long long hz = d->curvePointCapturedOffsetHz[ci];
        if (hz % 1000 || hz / 1000 < INT_MIN || hz / 1000 > INT_MAX) return false;
        *out = hz / 1000;
        return true;
    }
    CurvePointOffsetRequest request{};
    request.fromGpuOffset = kind == DESIRED_CURVE_PROJECTED;
    request.gpuOffsetComponentKHz = gpuComponentKHz;
    request.absoluteMHz = d->curvePointMHz[ci];
    request.liveBaseKHz = liveBaseKHz;
    *out = curve_point_target_offset_khz(&request);
    return true;
}

template <class Settings>
static inline bool desired_curve_point_offset_from_base_hz(const Settings* d, int ci,
    long long baseHz, long long gpuComponentHz, long long* out) {
    if (!out) return false;
    const DesiredCurvePointKind kind = desired_curve_point_kind(d, ci);
    long long value = 0;
    if (kind == DESIRED_CURVE_CAPTURED) value = d->curvePointCapturedOffsetHz[ci];
    else if (kind == DESIRED_CURVE_PROJECTED) value = gpuComponentHz;
    else if (kind == DESIRED_CURVE_ABSOLUTE) {
        const long long target = d->authoring.pointFrequencyPrecise[ci]
            ? d->authoring.pointFrequencyHz[ci] : (long long)d->curvePointMHz[ci] * 1000000;
        if (baseHz < 0 || target <= 0) return false;
        // Both operands are nonnegative signed values, so subtraction is safe.
        value = target - baseHz;
    } else return false;
    // Absolute offsets are derived math, possibly awaiting a fresh baseline;
    // their destination range/raw conversion is checked by the target planner.
    if (kind != DESIRED_CURVE_ABSOLUTE && (value < -2147483648000LL || value > 2147483647000LL)) return false;
    *out = value;
    return true;
}

template <class Settings>
static inline bool desired_curve_point_offset_hz(const Settings* d, int ci,
    long long liveBaseKHz, int gpuComponentKHz, long long* out) {
    if (!out) return false;
    if (desired_curve_point_kind(d, ci) == DESIRED_CURVE_CAPTURED) {
        const long long hz = d->curvePointCapturedOffsetHz[ci];
        if (hz < -2147483648000LL || hz > 2147483647000LL) return false;
        *out = hz;
        return true;
    }
    long long khz = 0;
    if (!desired_curve_point_offset_khz(d, ci, liveBaseKHz, gpuComponentKHz, &khz) ||
        khz < LLONG_MIN / 1000 || khz > LLONG_MAX / 1000) return false;
    *out = khz * 1000;
    return true;
}

template <class Settings>
static inline bool desired_curve_point_semantically_equal(
    const Settings* a, const Settings* b, int ci) {
    const DesiredCurvePointKind kind = desired_curve_point_kind(a, ci);
    if (kind == DESIRED_CURVE_INVALID || kind != desired_curve_point_kind(b, ci)) return false;
    if (kind == DESIRED_CURVE_CAPTURED)
        return a->curvePointCapturedOffsetHz[ci] == b->curvePointCapturedOffsetHz[ci];
    if (kind == DESIRED_CURVE_ABSOLUTE)
        return (a->authoring.pointFrequencyPrecise[ci] ? a->authoring.pointFrequencyHz[ci] : (long long)a->curvePointMHz[ci] * 1000000) ==
            (b->authoring.pointFrequencyPrecise[ci] ? b->authoring.pointFrequencyHz[ci] : (long long)b->curvePointMHz[ci] * 1000000);
    return true; // projected MHz is an observation, absent payload is inactive
}

template <class Settings>
static inline void desired_curve_point_clear(Settings* d, int ci) {
    d->hasCurvePoint[ci] = 0;
    d->curvePointMHz[ci] = 0;
    d->curvePointFromGpuOffset[ci] = 0;
    d->curvePointCaptured[ci] = 0;
    d->curvePointCapturedOffsetHz[ci] = 0;
    d->authoring.pointFrequencyPrecise[ci] = 0;
    d->authoring.pointFrequencyHz[ci] = 0;
}

template <class Settings>
static inline void desired_curve_point_copy_to(Settings* dst, int to, const Settings* src, int from) {
    dst->hasCurvePoint[to] = src->hasCurvePoint[from];
    dst->curvePointMHz[to] = src->curvePointMHz[from];
    dst->curvePointFromGpuOffset[to] = src->curvePointFromGpuOffset[from];
    dst->curvePointCaptured[to] = src->curvePointCaptured[from];
    dst->curvePointCapturedOffsetHz[to] = src->curvePointCapturedOffsetHz[from];
    dst->authoring.pointFrequencyPrecise[to] = src->authoring.pointFrequencyPrecise[from];
    dst->authoring.pointFrequencyHz[to] = src->authoring.pointFrequencyHz[from];
}
template <class Settings>
static inline void desired_curve_point_copy(Settings* dst, const Settings* src, int ci) {
    desired_curve_point_copy_to(dst, ci, src, ci);
}

// Structural checks are independent of destination feasibility and grid.
template <class Settings>
static inline bool desired_curve_intent_valid(const Settings* d, int* badPoint = nullptr) {
    if (!d) return false;
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        if (desired_curve_point_kind(d, ci) == DESIRED_CURVE_INVALID ||
            (d->curvePointCaptured[ci] && d->hasLock && d->lockCi >= 0 && ci >= d->lockCi) ||
            d->curvePointCapturedOffsetHz[ci] < -2147483648000LL ||
            d->curvePointCapturedOffsetHz[ci] > 2147483647000LL) {
            if (badPoint) *badPoint = ci;
            return false;
        }
    }
    return true;
}

#endif
