// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Pure normalization of a DesiredSettings block for presentation/editing.
//
// This lived inside linux_port.cpp, which the pure regression harness does not
// compile, so nothing pinned its clamps.  That is how the power limit ended up
// running through the generic 0..100 percent clamp: a 105% target was rewritten
// to 100% on load, on save, and on every TUI refresh after an apply, while the
// daemon happily held the 105% the user asked for.  Pure, header-only, and
// tested.

#ifndef GREEN_CURVE_DESIRED_SETTINGS_UI_POLICY_H
#define GREEN_CURVE_DESIRED_SETTINGS_UI_POLICY_H

// gpu_core.h — not fan_curve.h — is the shared data model here: fan_curve.h
// pulls in the Win32-coupled app_shared.h, which the Linux client deliberately
// keeps out of its include graph.  The three curve helpers are declared the
// same way linux_port.h declares them, and resolve to fan_curve.cpp on Windows
// and in the test harness, and to linux_port.cpp in the Linux binary.
#include "gpu_core.h"

#include <stddef.h>
#include <string.h>

void fan_curve_set_default(FanCurveConfig* config);
void fan_curve_normalize(FanCurveConfig* config);
bool fan_curve_validate(const FanCurveConfig* config, char* err, size_t errSize);

static inline int desired_ui_clamp_fan_percent(int value) {
    if (value < 0) return 0;
    if (value > 100) return 100;
    return value;
}

static inline void initialize_desired_settings_defaults(DesiredSettings* desired) {
    if (!desired) return;
    memset(desired, 0, sizeof(*desired));
    desired->lockTracksAnchor = true;
    desired->fanAuto = true;
    desired->fanMode = FAN_MODE_AUTO;
    desired->powerLimitPct = 100;
    fan_curve_set_default(&desired->fanCurve);
}

static inline void normalize_desired_settings_for_ui(DesiredSettings* desired) {
    if (!desired) return;
    desired->hasGpuOffset = true;
    desired->hasMemOffset = true;
    desired->hasPowerLimit = true;
    desired->hasFan = true;
    if (desired->gpuOffsetMHz == 0) desired->gpuOffsetExcludeLowCount = 0;
    // Deliberately NOT a 0..100 percent clamp: the power limit is a percentage
    // *of the board default*, and every board worth overclocking allows more
    // than 100% (an RTX 5070 defaults to 250 W with a 300 W ceiling).
    desired->powerLimitPct = clamp_power_limit_pct(
        desired->powerLimitPct == 0 ? 100 : desired->powerLimitPct);
    // A fan duty, unlike the power limit, really is 0..100.
    desired->fanPercent = desired_ui_clamp_fan_percent(
        desired->fanPercent <= 0 ? 50 : desired->fanPercent);
    if (desired->fanMode < FAN_MODE_AUTO || desired->fanMode > FAN_MODE_CURVE)
        desired->fanMode = FAN_MODE_AUTO;
    desired->fanAuto = desired->fanMode == FAN_MODE_AUTO;
    fan_curve_normalize(&desired->fanCurve);
    char err[128] = {};
    if (!fan_curve_validate(&desired->fanCurve, err, sizeof(err))) {
        fan_curve_set_default(&desired->fanCurve);
    }
}

// Field-wise comparison avoids treating struct padding as state. Besides being
// portable across the Windows/Linux ABIs, this keeps editor dirty detection and
// regression checks from depending on indeterminate object representation.
static inline bool desired_settings_equal(const DesiredSettings* left,
                                          const DesiredSettings* right) {
    if (!left || !right) return left == right;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        if (left->hasCurvePoint[i] != right->hasCurvePoint[i] ||
            left->curvePointMHz[i] != right->curvePointMHz[i]) return false;
    }
    if (left->hasLock != right->hasLock ||
        left->lockCi != right->lockCi ||
        left->lockMHz != right->lockMHz ||
        left->lockMode != right->lockMode ||
        left->lockTracksAnchor != right->lockTracksAnchor ||
        left->hasGpuOffset != right->hasGpuOffset ||
        left->gpuOffsetMHz != right->gpuOffsetMHz ||
        left->gpuOffsetExcludeLowCount != right->gpuOffsetExcludeLowCount ||
        left->hasMemOffset != right->hasMemOffset ||
        left->memOffsetMHz != right->memOffsetMHz ||
        left->hasPowerLimit != right->hasPowerLimit ||
        left->powerLimitPct != right->powerLimitPct ||
        left->hasFan != right->hasFan ||
        left->fanAuto != right->fanAuto ||
        left->fanMode != right->fanMode ||
        left->fanPercent != right->fanPercent ||
        left->fanCurve.pollIntervalMs != right->fanCurve.pollIntervalMs ||
        left->fanCurve.hysteresisC != right->fanCurve.hysteresisC ||
        left->fanCurve.zeroRpmEnabled !=
            right->fanCurve.zeroRpmEnabled ||
        left->fanCurve.zeroRpmHysteresisC !=
            right->fanCurve.zeroRpmHysteresisC ||
        left->resetOcBeforeApply != right->resetOcBeforeApply) return false;
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; ++i) {
        const FanCurvePoint* a = &left->fanCurve.points[i];
        const FanCurvePoint* b = &right->fanCurve.points[i];
        if (a->enabled != b->enabled ||
            a->temperatureC != b->temperatureC ||
            a->fanPercent != b->fanPercent) return false;
    }
    return true;
}

#endif // GREEN_CURVE_DESIRED_SETTINGS_UI_POLICY_H
