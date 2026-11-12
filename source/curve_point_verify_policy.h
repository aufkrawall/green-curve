// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Pure curve-verification math shared by the Windows and Linux apply paths.
// Windows keeps its existing displayed_curve_mhz()/curve_point_verify_tolerance_mhz()
// wrappers (main_runtime_gpu.cpp) over g_app.curve; they delegate here so the
// two platforms cannot drift onto different tolerance rules for the same
// quantization. Linux calls these directly over LinuxGpuState::curve, with no
// Windows dependency.
#ifndef GREEN_CURVE_CURVE_POINT_VERIFY_POLICY_H
#define GREEN_CURVE_CURVE_POINT_VERIFY_POLICY_H

static inline unsigned int curve_display_mhz_from_khz(unsigned int rawFreqKHz) {
    long long v = (long long)rawFreqKHz;
    if (v > INT_MAX) v = INT_MAX;
    return (unsigned int)((v + 500) / 1000);
}

// Tolerance accounts for driver quantization: half the distance to the
// nearest distinct neighbor's displayed MHz, clamped to [1, 8]. A point whose
// visible neighbors all share its frequency (deep inside a flat plateau,
// or an isolated point) gets the 8 MHz ceiling. `freqKHzAt(ci)` returns 0 for
// an unpopulated point. Same rule Windows has always applied (F-03), so a
// flatten verified on Linux cannot be held to a tighter or looser bin than
// the identical shape would get on Windows.
template <typename FreqAt>
static inline unsigned int curve_point_verify_tolerance_mhz_over(
    FreqAt freqKHzAt, int pointIndex, int numPoints) {
    if (pointIndex < 0 || pointIndex >= numPoints || freqKHzAt(pointIndex) == 0)
        return 1;
    unsigned int actualMHz = curve_display_mhz_from_khz(freqKHzAt(pointIndex));
    auto nearest_distinct_neighbor_distance_mhz = [&](int startIndex, int step) -> unsigned int {
        for (int ci = startIndex; ci >= 0 && ci < numPoints; ci += step) {
            unsigned int f = freqKHzAt(ci);
            if (f == 0) continue;
            unsigned int neighborMHz = curve_display_mhz_from_khz(f);
            if (neighborMHz == actualMHz) continue;
            return actualMHz > neighborMHz ? (actualMHz - neighborMHz) : (neighborMHz - actualMHz);
        }
        return 0;
    };
    unsigned int leftDistanceMHz = nearest_distinct_neighbor_distance_mhz(pointIndex - 1, -1);
    unsigned int rightDistanceMHz = nearest_distinct_neighbor_distance_mhz(pointIndex + 1, 1);
    unsigned int minDistanceMHz;
    if (leftDistanceMHz && rightDistanceMHz)
        minDistanceMHz = leftDistanceMHz < rightDistanceMHz ? leftDistanceMHz : rightDistanceMHz;
    else
        minDistanceMHz = leftDistanceMHz ? leftDistanceMHz : rightDistanceMHz;
    if (minDistanceMHz == 0) return 8;
    unsigned int toleranceMHz = (minDistanceMHz + 1) / 2;
    if (toleranceMHz < 1) toleranceMHz = 1;
    if (toleranceMHz > 8) toleranceMHz = 8;
    return toleranceMHz;
}

// True when a point's readback MHz (derived the same way as the tolerance
// above) is within tolerance of an absolute target, whether that target is
// its own explicit frequency or a flatten/hard-lock tail's shared plateau.
template <typename FreqAt>
static inline bool curve_point_absolute_target_verified_over(
    FreqAt freqKHzAt, int pointIndex, int numPoints, unsigned int targetMHz) {
    unsigned int freqKHz = freqKHzAt(pointIndex);
    if (freqKHz == 0) return false;
    unsigned int actualMHz = curve_display_mhz_from_khz(freqKHz);
    unsigned int toleranceMHz =
        curve_point_verify_tolerance_mhz_over(freqKHzAt, pointIndex, numPoints);
    unsigned int delta = actualMHz > targetMHz ? actualMHz - targetMHz : targetMHz - actualMHz;
    return delta <= toleranceMHz;
}

#endif // GREEN_CURVE_CURVE_POINT_VERIFY_POLICY_H
