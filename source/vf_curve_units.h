// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Normalize only the graphics VF domain at the driver boundary. Everything
// above it (targets, verification, snapshots and persistence) uses physical kHz.

#ifndef GREEN_CURVE_VF_CURVE_UNITS_H
#define GREEN_CURVE_VF_CURVE_UNITS_H

#include "gpu_core.h"
#include <limits.h>

static inline unsigned int vf_graphics_frequency_scale(const VfBackendSpec* b) {
    return b && b->graphicsFrequencyScale ? b->graphicsFrequencyScale : 1u;
}

static inline int vf_graphics_offset_khz(const VfBackendSpec* b, int raw) {
    return (int)((long long)raw / vf_graphics_frequency_scale(b));
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
static inline int vf_normalize_graphics_status(
    const VfBackendSpec* b, VFCurvePoint* curve, int count) {
    const int end = vf_graphics_domain_length(curve, count);
    const unsigned int scale = vf_graphics_frequency_scale(b);
    for (int i = 0; i < end; ++i) curve[i].freq_kHz /= scale;
    return end;
}

#endif // GREEN_CURVE_VF_CURVE_UNITS_H
