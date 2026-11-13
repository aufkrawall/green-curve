// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Normalize only the graphics VF domain at the driver boundary. Everything
// above it uses physical kHz for legacy controls and exact Hz for capture.

#ifndef GREEN_CURVE_VF_CURVE_UNITS_H
#define GREEN_CURVE_VF_CURVE_UNITS_H

// Exact cached control values take precedence over their legacy kHz projection.
// Unknown fractional normalization leaves the established legacy path available.
static inline bool vf_graphics_offset_readback_matches(int projectedKHz,
    bool exactValid, long long actualHz, int targetKHz) {
    return exactValid ? actualHz == (long long)targetKHz * 1000 : projectedKHz == targetKHz;
}

#include "gpu_core.h"
#include <limits.h>

static inline unsigned int vf_graphics_frequency_scale(const VfBackendSpec* b) {
    return b && b->graphicsFrequencyScale ? b->graphicsFrequencyScale : 1u;
}

static inline int vf_graphics_offset_grid_hz(const VfBackendSpec* b) {
    unsigned int a = vf_graphics_frequency_scale(b), divisor = 1000;
    while (divisor) { const unsigned int r = a % divisor; a = divisor; divisor = r; }
    return (int)(1000 / a);
}

static inline int vf_graphics_offset_khz(const VfBackendSpec* b, int raw) {
    return (int)((long long)raw / vf_graphics_frequency_scale(b));
}

static inline bool vf_graphics_frequency_hz(const VfBackendSpec* b, unsigned int raw, gc_i64* hz) {
    if (!hz) return false;
    const gc_i64 numerator = (gc_i64)raw * 1000;
    const gc_i64 scale = vf_graphics_frequency_scale(b);
    if (numerator % scale) return false;
    *hz = numerator / scale;
    return true;
}

static inline bool vf_graphics_offset_hz(const VfBackendSpec* b, int raw, gc_i64* hz) {
    if (!hz) return false;
    const gc_i64 numerator = (gc_i64)raw * 1000;
    const gc_i64 scale = vf_graphics_frequency_scale(b);
    if (numerator % scale) return false;
    *hz = numerator / scale;
    return true;
}

static inline bool vf_graphics_offset_driver_units_hz(
    const VfBackendSpec* b, gc_i64 hz, int* raw) {
    const gc_i64 scale = vf_graphics_frequency_scale(b);
    if (!raw || hz < LLONG_MIN / scale || hz > LLONG_MAX / scale) return false;
    const gc_i64 scaled = hz * scale;
    if (scaled % 1000 || scaled / 1000 < INT_MIN || scaled / 1000 > INT_MAX) return false;
    *raw = (int)(scaled / 1000);
    return true;
}

static inline bool vf_graphics_offset_driver_units(
    const VfBackendSpec* b, int physicalKHz, int* raw) {
    if (!raw) return false;
    const long long value = (long long)physicalKHz * vf_graphics_frequency_scale(b);
    if (value < INT_MIN || value > INT_MAX) return false;
    *raw = (int)value;
    return true;
}

// The status window concatenates clock domains. A populated point whose
// voltage drops starts a foreign domain; gaps do not end the graphics run.
static inline int vf_graphics_domain_length(const VFCurvePoint* curve, int count) {
    if (!curve || count <= 0) return 0;
    if (count > VF_NUM_POINTS) count = VF_NUM_POINTS;
    unsigned int previousVoltage = 0;
    for (int i = 0; i < count; ++i) {
        if (!curve[i].freq_kHz || !curve[i].volt_uV) continue;
        if (previousVoltage && curve[i].volt_uV < previousVoltage) return i;
        previousVoltage = curve[i].volt_uV;
    }
    return count;
}

// Return the leading graphics extent and normalize it exactly once. Keep the
// following domains raw for layout diagnostics; they have independent units.
// Optional frequencyHz storage has VF_NUM_POINTS entries, including cleared
// foreign/unpopulated entries; validity describes this exact status sample.
static inline int vf_normalize_graphics_status(
    const VfBackendSpec* b, VFCurvePoint* curve, int count,
    gc_i64* frequencyHz = nullptr, bool* frequencyHzValid = nullptr) {
    const int end = vf_graphics_domain_length(curve, count);
    const unsigned int scale = vf_graphics_frequency_scale(b);
    bool exact = frequencyHz != nullptr, populated = false;
    if (frequencyHz) for (int i = 0; i < VF_NUM_POINTS; ++i) frequencyHz[i] = 0;
    for (int i = 0; i < end; ++i) {
        if (!curve[i].freq_kHz && curve[i].volt_uV) exact = false;
        if (frequencyHz && curve[i].freq_kHz) {
            populated = true;
            if (!curve[i].volt_uV || !vf_graphics_frequency_hz(b, curve[i].freq_kHz, &frequencyHz[i])) exact = false;
        }
        curve[i].freq_kHz /= scale;
    }
    if (frequencyHzValid) *frequencyHzValid = exact && populated;
    return end;
}

#endif // GREEN_CURVE_VF_CURVE_UNITS_H
