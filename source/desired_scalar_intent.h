// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_DESIRED_SCALAR_INTENT_H
#define GREEN_CURVE_DESIRED_SCALAR_INTENT_H
// Included by gpu_core.h after the wire scalar aliases and control-surface enum.
#include "tuning_units.h"
#include "memory_offset_units.h"

template<class D> static inline TuningPowerRatio desired_power_ratio(const D& d) {
    return d.powerLimitRatio ? TuningPowerRatio{d.powerRatioNumerator, d.powerRatioDenominator}
        : tuning_power_ratio_from_percent(d.powerLimitPct);
}

static inline bool desired_power_ratio_preview(TuningPowerRatio r, int* out) {
    long long pct = 0; bool exact = false;
    if (!out || !tuning_power_ratio_valid(r) ||
        !tuning_mul_div_nonnegative(r.numerator, 100, r.denominator, &pct, &exact) ||
        pct > INT_MAX) return false;
    *out = (int)pct; // a labeled display projection, never write authority
    return true;
}

template<class D> static inline bool desired_memory_set_precise(D* d, long long hz, gc_u32 surface) {
    int effective = 0, khz = 0;
    if (!d || hz < -3000000000LL || hz > 3000000000LL || surface > CLOCK_OFFSET_SURFACE_GLOBAL ||
        !memory_offset_effective_mhz_from_hz(hz, &effective) ||
        !memory_offset_khz_from_effective_mhz(effective, &khz)) return false;
    d->hasMemOffset = d->memOffsetPrecise = 1;
    d->memOffsetHz = hz; d->memOffsetSurface = surface;
    d->memOffsetMHz = (int)(hz / 1000000);
    return true;
}

template<class D> static inline bool desired_power_set_ratio(D* d, TuningPowerRatio ratio) {
    ratio = tuning_power_ratio_reduce(ratio.numerator, ratio.denominator);
    int preview = 0;
    if (!d || !desired_power_ratio_preview(ratio, &preview)) return false;
    d->hasPowerLimit = d->powerLimitRatio = 1;
    d->powerRatioNumerator = ratio.numerator; d->powerRatioDenominator = ratio.denominator;
    d->powerLimitPct = preview;
    return true;
}

template<class D> static inline void desired_memory_precision_clear(D* d) {
    d->memOffsetPrecise = 0; d->memOffsetHz = 0; d->memOffsetSurface = 0;
}
template<class D> static inline void desired_power_precision_clear(D* d) {
    d->powerLimitRatio = 0; d->powerRatioNumerator = d->powerRatioDenominator = 0;
}
template<class D> static inline void desired_memory_intent_copy(D* out, const D* in) {
    out->hasMemOffset = in->hasMemOffset; out->memOffsetMHz = in->memOffsetMHz;
    out->memOffsetPrecise = in->memOffsetPrecise; out->memOffsetHz = in->memOffsetHz;
    out->memOffsetSurface = in->memOffsetSurface;
}
template<class D> static inline void desired_power_intent_copy(D* out, const D* in) {
    out->hasPowerLimit = in->hasPowerLimit; out->powerLimitPct = in->powerLimitPct;
    out->powerLimitRatio = in->powerLimitRatio; out->powerRatioNumerator = in->powerRatioNumerator;
    out->powerRatioDenominator = in->powerRatioDenominator;
}
template<class D> static inline bool desired_scalar_intent_valid(const D* d) {
    if (!d || d->memOffsetPrecise > 1 || d->powerLimitRatio > 1) return false;
    if (d->memOffsetPrecise) {
        int effective = 0, khz = 0;
        if (!d->hasMemOffset || d->memOffsetHz < -3000000000LL || d->memOffsetHz > 3000000000LL ||
            d->memOffsetSurface > CLOCK_OFFSET_SURFACE_GLOBAL ||
            !memory_offset_effective_mhz_from_hz(d->memOffsetHz, &effective) ||
            !memory_offset_khz_from_effective_mhz(effective, &khz) ||
            d->memOffsetHz / 1000000 != d->memOffsetMHz) return false;
    }
    if (d->powerLimitRatio) {
        const TuningPowerRatio r = desired_power_ratio(*d);
        int preview = 0;
        if (!d->hasPowerLimit || !desired_power_ratio_preview(r, &preview) ||
            tuning_gcd_i64(r.numerator, r.denominator) != 1 || preview != d->powerLimitPct) return false;
    }
    return true;
}
template<class D> static inline void desired_scalar_intent_canonicalize(D* d) {
    canonicalize_gc_bool8(&d->memOffsetPrecise); canonicalize_gc_bool8(&d->powerLimitRatio);
    if (!d->hasMemOffset || !d->memOffsetPrecise) desired_memory_precision_clear(d);
    if (!d->hasPowerLimit || !d->powerLimitRatio) desired_power_precision_clear(d);
}

// Profile ownership comparison: an unspecified surface may match the one
// selected at Apply, but a named source surface requires matching authority.
template<class D> static inline long long desired_memory_hz(const D& d) {
    return d.memOffsetPrecise ? d.memOffsetHz : (long long)d.memOffsetMHz * 1000000;
}
template<class D> static inline bool desired_memory_intent_matches(const D& profile, const D& active) {
    if (!!profile.hasMemOffset != !!active.hasMemOffset) return false;
    if (!profile.hasMemOffset) return true;
    return desired_memory_hz(profile) == desired_memory_hz(active) && (!profile.memOffsetPrecise || !profile.memOffsetSurface ||
        (active.memOffsetPrecise && profile.memOffsetSurface == active.memOffsetSurface));
}
template<class D> static inline bool desired_power_intent_matches(const D& profile, const D& active) {
    if (!!profile.hasPowerLimit != !!active.hasPowerLimit) return false;
    if (!profile.hasPowerLimit) return true;
    const TuningPowerRatio p = desired_power_ratio(profile), a = desired_power_ratio(active);
    if (!tuning_power_ratio_valid(p) || !tuning_power_ratio_valid(a)) return false;
    const TuningPowerRatio pr = tuning_power_ratio_reduce(p.numerator, p.denominator);
    const TuningPowerRatio ar = tuning_power_ratio_reduce(a.numerator, a.denominator);
    return pr.numerator == ar.numerator && pr.denominator == ar.denominator;
}
#endif
