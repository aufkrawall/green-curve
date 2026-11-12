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
        (kind == DESIRED_CURVE_ABSOLUTE && d->curvePointMHz[ci] > 0);
}

template <class Settings>
static inline bool desired_curve_point_offset_khz(const Settings* d, int ci,
    long long liveBaseKHz, int gpuComponentKHz, long long* out) {
    if (!out) return false;
    const DesiredCurvePointKind kind = desired_curve_point_kind(d, ci);
    if (kind == DESIRED_CURVE_INVALID || kind == DESIRED_CURVE_ABSENT) return false;
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
static inline bool desired_curve_point_semantically_equal(
    const Settings* a, const Settings* b, int ci) {
    const DesiredCurvePointKind kind = desired_curve_point_kind(a, ci);
    if (kind == DESIRED_CURVE_INVALID || kind != desired_curve_point_kind(b, ci)) return false;
    if (kind == DESIRED_CURVE_CAPTURED)
        return a->curvePointCapturedOffsetHz[ci] == b->curvePointCapturedOffsetHz[ci];
    if (kind == DESIRED_CURVE_ABSOLUTE) return a->curvePointMHz[ci] == b->curvePointMHz[ci];
    return true; // projected MHz is an observation, absent payload is inactive
}

template <class Settings>
static inline void desired_curve_point_clear(Settings* d, int ci) {
    d->hasCurvePoint[ci] = 0;
    d->curvePointMHz[ci] = 0;
    d->curvePointFromGpuOffset[ci] = 0;
    d->curvePointCaptured[ci] = 0;
    d->curvePointCapturedOffsetHz[ci] = 0;
}

template <class Settings>
static inline void desired_curve_point_copy(Settings* dst, const Settings* src, int ci) {
    dst->hasCurvePoint[ci] = src->hasCurvePoint[ci];
    dst->curvePointMHz[ci] = src->curvePointMHz[ci];
    dst->curvePointFromGpuOffset[ci] = src->curvePointFromGpuOffset[ci];
    dst->curvePointCaptured[ci] = src->curvePointCaptured[ci];
    dst->curvePointCapturedOffsetHz[ci] = src->curvePointCapturedOffsetHz[ci];
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
