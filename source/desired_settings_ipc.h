// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_DESIRED_SETTINGS_IPC_H
#define GREEN_CURVE_DESIRED_SETTINGS_IPC_H

// Split out of gpu_core.h, which sits on its size ratchet.  Included from
// gpu_core.h at the point this function used to occupy, so it still sees
// DesiredSettings and every clamp helper it calls, and every consumer still
// gets it by including gpu_core.h.  Pure move plus the one new flag.

// Sanitize a DesiredSettings struct received over IPC.  This is the single
// trust boundary between an unprivileged caller and the privileged service:
// every numeric field that can reach an array index, a hardware write, or a
// runtime loop MUST be range-checked here.  Downstream code also guards the
// index fields, but completing the clamps at the boundary is defense in depth
// (CWE-20) so a malformed or hostile request can never drive out-of-range
// behavior even if a future downstream guard is dropped.
static inline void validate_desired_settings_for_ipc(DesiredSettings* d) {
    if (!d) return;
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        canonicalize_gc_bool8(&d->hasCurvePoint[ci]);
    }
    canonicalize_gc_bool8(&d->hasLock);
    canonicalize_gc_bool8(&d->lockTracksAnchor);
    canonicalize_gc_bool8(&d->hasGpuOffset);
    canonicalize_gc_bool8(&d->hasMemOffset);
    canonicalize_gc_bool8(&d->hasPowerLimit);
    canonicalize_gc_bool8(&d->hasFan);
    canonicalize_gc_bool8(&d->fanAuto);
    canonicalize_gc_bool8(&d->resetOcBeforeApply);
    canonicalize_gc_bool8(&d->hasXbarOffsetKhz);
    canonicalize_gc_bool8(&d->hasXbarMsvddOffsetUv);
    canonicalize_gc_bool8(&d->hasSysClkOffsetKhz);
    canonicalize_gc_bool8(&d->hasVideoClkOffsetKhz);
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        canonicalize_gc_bool8(&d->curvePointFromGpuOffset[ci]);
    }
    if (d->hasXbarOffsetKhz && (d->xbarOffsetKhz < -1000000 || d->xbarOffsetKhz > 1000000)) {
        d->xbarOffsetKhz = d->xbarOffsetKhz < -1000000 ? -1000000 : 1000000;
    }
    if (d->hasXbarMsvddOffsetUv && (d->xbarMsvddOffsetUv < -100000 || d->xbarMsvddOffsetUv > 100000)) {
        d->xbarMsvddOffsetUv = d->xbarMsvddOffsetUv < -100000 ? -100000 : 100000;
    }
    if (d->hasSysClkOffsetKhz && (d->sysClkOffsetKhz < -1000000 || d->sysClkOffsetKhz > 1000000)) {
        d->sysClkOffsetKhz = d->sysClkOffsetKhz < -1000000 ? -1000000 : 1000000;
    }
    if (d->hasVideoClkOffsetKhz && (d->videoClkOffsetKhz < -1000000 || d->videoClkOffsetKhz > 1000000)) {
        d->videoClkOffsetKhz = d->videoClkOffsetKhz < -1000000 ? -1000000 : 1000000;
    }
    validate_fan_curve_flags_for_ipc(&d->fanCurve);
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (d->curvePointMHz[ci] > 5000u) d->curvePointMHz[ci] = 5000u;
    }
    // The unknown/unset sentinel is normalized to the board default BEFORE the
    // range clamp, exactly as normalize_desired_settings_for_ui() does it.  A
    // bare clamp promotes 0 to the 50% floor, which silently turns "this board
    // never told us its power target" into a genuine request to halve it.
    if (d->hasPowerLimit) {
        d->powerLimitPct = clamp_power_limit_pct(
            d->powerLimitPct == 0 ? POWER_LIMIT_DEFAULT_PCT : d->powerLimitPct);
    }
    if (d->hasGpuOffset && (d->gpuOffsetMHz < -1000 || d->gpuOffsetMHz > 1000)) {
        d->gpuOffsetMHz = d->gpuOffsetMHz < -1000 ? -1000 : 1000;
    }
    if (d->hasMemOffset && (d->memOffsetMHz < -3000 || d->memOffsetMHz > 3000)) {
        d->memOffsetMHz = d->memOffsetMHz < -3000 ? -3000 : 3000;
    }
    // lockCi indexes VF_NUM_POINTS-sized arrays downstream.  Preserve the -1
    // "no explicit lock" sentinel but neutralize any out-of-bounds index.
    if (d->lockCi < -1) d->lockCi = -1;
    if (d->lockCi >= VF_NUM_POINTS) d->lockCi = VF_NUM_POINTS - 1;
    if (d->lockMode < LOCK_MODE_NONE) d->lockMode = LOCK_MODE_NONE;
    if (d->lockMode > LOCK_MODE_HARD) d->lockMode = LOCK_MODE_HARD;
    // lockMHz feeds NVML locked-clocks and flatten-tail targets; cap it like
    // the curve points (0 stays 0 = "no target").
    if (d->lockMHz > 5000u) d->lockMHz = 5000u;
    // Selective-offset exclude count gates per-point GPU offset application.
    if (d->gpuOffsetExcludeLowCount < 0) d->gpuOffsetExcludeLowCount = 0;
    if (d->gpuOffsetExcludeLowCount > VF_NUM_POINTS) d->gpuOffsetExcludeLowCount = VF_NUM_POINTS;
    if (d->hasFan) {
        if (d->fanPercent < 0) d->fanPercent = 0;
        if (d->fanPercent > 100) d->fanPercent = 100;
        // fanMode selects the runtime policy (auto/fixed/curve); an unknown
        // value would fall through every branch with undefined effect.
        if (d->fanMode < FAN_MODE_AUTO) d->fanMode = FAN_MODE_AUTO;
        if (d->fanMode > FAN_MODE_CURVE) d->fanMode = FAN_MODE_CURVE;
        // The embedded fan curve feeds fan-speed writes and interpolation.
        for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
            if (d->fanCurve.points[i].fanPercent < 0) d->fanCurve.points[i].fanPercent = 0;
            if (d->fanCurve.points[i].fanPercent > 100) d->fanCurve.points[i].fanPercent = 100;
            if (d->fanCurve.points[i].temperatureC < 0) d->fanCurve.points[i].temperatureC = 0;
            if (d->fanCurve.points[i].temperatureC > 150) d->fanCurve.points[i].temperatureC = 150;
        }
        if (d->fanCurve.hysteresisC < 0) d->fanCurve.hysteresisC = 0;
        if (d->fanCurve.hysteresisC > FAN_CURVE_MAX_HYSTERESIS_C) d->fanCurve.hysteresisC = FAN_CURVE_MAX_HYSTERESIS_C;
        if (d->fanCurve.zeroRpmHysteresisC < FAN_ZERO_RPM_MIN_HYSTERESIS_C) d->fanCurve.zeroRpmHysteresisC = FAN_ZERO_RPM_MIN_HYSTERESIS_C;
        if (d->fanCurve.zeroRpmHysteresisC > FAN_ZERO_RPM_MAX_HYSTERESIS_C) d->fanCurve.zeroRpmHysteresisC = FAN_ZERO_RPM_MAX_HYSTERESIS_C;
        // F-01-002: the clamps above bound every NUMBER, but the curve's own
        // validity rule lived only in fan_curve_validate(), on the GUI side of
        // this boundary -- so a non-GUI caller could install a curve the UI
        // rejects. Normalizing here (rather than rejecting) matches every other
        // rule in this function, and subsumes the 1 ms poll floor and the 150 C
        // temperature cap this block used to apply by hand, neither of which
        // any other rule in the product accepts. See fan_curve.h for why this
        // is the _for_ipc variant.
        fan_curve_normalize_for_ipc(&d->fanCurve);
    }
}

#endif
