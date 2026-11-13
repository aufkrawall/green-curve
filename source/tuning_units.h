// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Checked physical-unit conversions and exact power ratios for the new
// authoring document (ProfileDocument, profile_document.h). Pure and
// GPU-free: every function here takes plain numbers and returns a result,
// never reads global state or a driver. Canonical units match §5.4 of the
// authoring plan: frequency and signed frequency adjustments in Hz (64-bit),
// voltage in microvolts, absolute power observations in mW, and power
// INTENT as an exact reduced ratio of board default rather than mW, so a
// captured 249000/250000 mW limit stays 249/250 instead of rounding to 100%.
//
// Existing runtime code stays physical-kHz based (vf_curve_units.h,
// gpu_core.h) where an exact conversion from Hz is always possible; these
// helpers exist so a precise document can be read, compared and re-exported
// without ever being silently narrowed, even before anything resolves it
// against a destination's driver surface.
#ifndef GREEN_CURVE_TUNING_UNITS_H
#define GREEN_CURVE_TUNING_UNITS_H

#include <climits>

// Every helper below assumes non-negative inputs unless the comment says
// otherwise; the domains involved (mW, Hz magnitudes, ratio terms) are never
// negative, so the overflow checks skip the LLONG_MIN edge case on purpose.
static inline bool tuning_mul_overflows_i64(long long a, long long b) {
    if (a < 0 || b < 0) return true; // out of the documented domain
    if (a == 0 || b == 0) return false;
    return a > (LLONG_MAX / b);
}

// Hz -> kHz, reporting whether a nonzero sub-kHz remainder was discarded.
// This is the exact case §5.4 calls out for Pascal's scale-2 raw VF values:
// a half-kHz physical adjustment (synthetic 15000500 Hz) must not silently
// become 15000 kHz without the caller knowing a remainder was dropped.
struct TuningHzToKHzResult {
    long long kHz;
    bool hadRemainder;
};
static inline TuningHzToKHzResult tuning_hz_to_khz(long long hz) {
    TuningHzToKHzResult r{};
    r.kHz = hz / 1000;
    r.hadRemainder = (hz % 1000) != 0;
    return r;
}

// Signed scaling includes LLONG_MIN; the factor must be positive.
static inline bool tuning_scale_i64(long long value, long long scale, long long* out) {
    if (!out || scale <= 0 || value > LLONG_MAX / scale || value < LLONG_MIN / scale)
        return false;
    *out = value * scale;
    return true;
}
// Zero is the invalid absolute-frequency sentinel for the legacy convenience
// callers. Adapters for signed offsets use the checked function directly.
static inline long long tuning_khz_to_hz(long long khz) {
    long long out = 0;
    return tuning_scale_i64(khz, 1000, &out) ? out : 0;
}
static inline long long tuning_mhz_to_hz(long long mhz) {
    long long out = 0;
    return tuning_scale_i64(mhz, 1000000, &out) ? out : 0;
}

// An exact reduced ratio of board default power, denominator always
// positive. {0, 0} is the explicit invalid sentinel (never a valid 0/0
// construction site; every constructor below returns it on bad input rather
// than inventing a ratio).
struct TuningPowerRatio {
    long long numerator;
    long long denominator;
};

static inline long long tuning_gcd_i64(long long a, long long b) {
    if (a < 0 || b < 0) return 0;
    while (b != 0) {
        long long t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static inline bool tuning_power_ratio_valid(TuningPowerRatio r) {
    return r.numerator > 0 && r.denominator > 0;
}

static inline TuningPowerRatio tuning_power_ratio_reduce(long long numerator,
                                                          long long denominator) {
    if (numerator <= 0 || denominator <= 0) return TuningPowerRatio{0, 0};
    long long g = tuning_gcd_i64(numerator, denominator);
    if (g <= 0) return TuningPowerRatio{0, 0};
    return TuningPowerRatio{numerator / g, denominator / g};
}

// A captured limit against the board's own default, reduced canonically.
// Retain currentMw/defaultMw separately as provenance; this ratio alone
// cannot reconstruct them once reduced (249/250 could come from many pairs).
static inline TuningPowerRatio tuning_power_ratio_from_mw(long long currentMw,
                                                           long long defaultMw) {
    if (currentMw <= 0 || defaultMw <= 0) return TuningPowerRatio{0, 0};
    return tuning_power_ratio_reduce(currentMw, defaultMw);
}

// A user-entered integer percentage (100 -> 1/1, 50 -> 1/2).
static inline TuningPowerRatio tuning_power_ratio_from_percent(long long percent) {
    if (percent <= 0) return TuningPowerRatio{0, 0};
    return tuning_power_ratio_reduce(percent, 100);
}

struct TuningPowerRatioResolution {
    bool exact;
    long long resolvedMw;    // valid only when exact
    long long requestedMw;   // the ideal, possibly fractional-rounded, target
    bool overflowed;         // checked arithmetic could not represent it at all
};

// Floor(a*b/denominator) and its exactness without an overflowing product or
// compiler-specific wide integer. Each partial value is a quotient/remainder
// pair; remainder addition and doubling use subtraction before addition.
static inline bool tuning_mul_div_nonnegative(long long a, long long b,
    long long denominator, long long* quotient, bool* exact) {
    if (a < 0 || b < 0 || denominator <= 0 || !quotient || !exact) return false;
    long long q = a / denominator, r = a % denominator;
    long long totalQ = 0, totalR = 0;
    while (b) {
        if (b & 1) {
            if (q > LLONG_MAX - totalQ) return false;
            totalQ += q;
            if (totalR >= denominator - r) {
                totalR -= denominator - r;
                if (totalQ == LLONG_MAX) return false;
                ++totalQ;
            } else totalR += r;
        }
        b /= 2;
        if (!b) break;
        const long long carry = r >= denominator - r ? 1 : 0;
        if (q > (LLONG_MAX - carry) / 2) return false;
        q = q * 2 + carry;
        r = carry ? r - (denominator - r) : r + r;
    }
    *quotient = totalQ;
    *exact = totalR == 0;
    return true;
}

// Resolve a ratio against a FRESH destination default, with checked 64-bit
// arithmetic (contract: reject overflow before narrowing). Exact only when
// numerator*destDefaultMw is evenly divisible by denominator; otherwise the
// caller must stage an explicit batch conversion (§5.4) rather than round.
// This performs no destination-granularity check -- the driver-side grid is
// a separate, platform-specific question answered where the destination is
// actually known (gpu_backend / linux_backend adapters).
static inline TuningPowerRatioResolution tuning_power_ratio_resolve(
    TuningPowerRatio ratio, long long destDefaultMw) {
    TuningPowerRatioResolution out{};
    if (!tuning_power_ratio_valid(ratio) || destDefaultMw <= 0) {
        out.overflowed = true;
        return out;
    }
    if (!tuning_mul_div_nonnegative(ratio.numerator, destDefaultMw,
            ratio.denominator, &out.requestedMw, &out.exact)) {
        out.overflowed = true;
        return out;
    }
    out.resolvedMw = out.exact ? out.requestedMw : 0;
    return out;
}

static inline bool tuning_power_ratio_equal(TuningPowerRatio a, TuningPowerRatio b) {
    if (!tuning_power_ratio_valid(a) || !tuning_power_ratio_valid(b)) return false;
    a = tuning_power_ratio_reduce(a.numerator, a.denominator);
    b = tuning_power_ratio_reduce(b.numerator, b.denominator);
    return a.numerator == b.numerator && a.denominator == b.denominator;
}

#endif // GREEN_CURVE_TUNING_UNITS_H
