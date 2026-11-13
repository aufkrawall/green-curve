// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
// Included after CurvePlanInput; exact arithmetic stays separate from display.
#include "desired_curve_intent.h"

static inline long long curve_plan_absolute_hz(const DesiredSettings* d, int ci) {
    return d->authoring.pointFrequencyPrecise[ci] ? d->authoring.pointFrequencyHz[ci]
        : (long long)d->curvePointMHz[ci] * 1000000;
}
static inline long long curve_plan_lock_hz(const DesiredSettings* d) {
    return d->authoring.lockPrecise ? d->authoring.lockFrequencyHz : (long long)d->lockMHz * 1000000;
}
static inline bool curve_plan_uses_precise_values(const DesiredSettings* d) {
    if (d->authoring.graphicsPrecise || d->authoring.lockPrecise) return true;
    for (int i = 0; i < VF_NUM_POINTS; ++i)
        if (d->authoring.pointFrequencyPrecise[i] || d->curvePointCaptured[i]) return true;
    return false;
}
static inline bool curve_plan_exact_samples_valid(const CurvePlanInput& in) {
    if (!!in.frequencyHz != !!in.programmedOffsetsHz) return false;
    if (in.desired->authoring.graphicsPrecise && !in.gpuComponentHz) return false;
    if (in.desired->authoring.lockPrecise && in.lockMode == LOCK_MODE_HARD && in.lockGridHz <= 0) return false;
    if (in.gpuComponentHz) for (int i = 0; i < VF_NUM_POINTS; ++i)
        if (in.gpuComponentHz[i] < -2147483648000LL || in.gpuComponentHz[i] > 2147483647000LL) return false;
    if (!in.frequencyHz) {
        if (in.desired->authoring.lockPrecise) return false;
        for (int i = 0; i < VF_NUM_POINTS; ++i)
            if (in.desired->authoring.pointFrequencyPrecise[i]) return false;
        return true;
    }
    for (int i = 0; i < VF_NUM_POINTS && i < in.floor.graphicsEnd; ++i) {
        if (!in.curve[i].freq_kHz) continue;
        if (in.frequencyHz[i] <= 0 || in.frequencyHz[i] > (long long)UINT_MAX * 1000 ||
            in.frequencyHz[i] / 1000 != in.curve[i].freq_kHz ||
            in.programmedOffsetsHz[i] < -2147483648000LL || in.programmedOffsetsHz[i] > 2147483647000LL ||
            in.programmedOffsetsHz[i] / 1000 != in.programmedOffsetsKHz[i]) return false;
    }
    return true;
}
static inline long long curve_plan_base_hz(const CurvePlanInput& in, int ci) {
    const long long base = in.frequencyHz
        ? in.frequencyHz[ci] - in.programmedOffsetsHz[ci]
        : ((long long)in.curve[ci].freq_kHz - in.programmedOffsetsKHz[ci]) * 1000;
    return base > 0 ? base : 0;
}
static inline long long curve_plan_component_hz(const CurvePlanInput& in, int ci) {
    return in.gpuComponentHz ? in.gpuComponentHz[ci]
        : in.gpuComponentKHz ? (long long)in.gpuComponentKHz[ci] * 1000 : 0;
}
static inline unsigned int curve_plan_display_hz(long long hz) {
    if (hz < 0) hz = 0;
    if (hz > (long long)INT_MAX * 1000) hz = (long long)INT_MAX * 1000;
    return (unsigned int)((hz + 500000) / 1000000);
}

// Unit-independent spacing; callers supply the fallback in the same units.
static inline long long curve_plan_neighbour_step(const long long* base,
    const VFCurvePoint* curve, int ci, int end, long long fallback) {
    long long best = 0;
    for (int dir = -1; dir <= 1; dir += 2) {
        for (int j = ci + dir; j >= 0 && j < end; j += dir) {
            if (!curve[j].freq_kHz) continue;
            long long delta = base[j] - base[ci];
            if (delta < 0) delta = -delta;
            if (!delta) continue;
            if (!best || delta < best) best = delta;
            break;
        }
    }
    return best > 0 ? best : fallback;
}
