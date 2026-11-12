// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_MEMORY_OFFSET_UNITS_H
#define GREEN_CURVE_MEMORY_OFFSET_UNITS_H
#include <limits.h>

// NVML effective MHz is exactly half a physical/display MHz. These conversions
// describe API units, not a claim that every board accepts every grid value.
static inline bool memory_offset_effective_mhz_from_khz(int khz, int* out) {
    if (!out || khz % 500) return false;
    *out = khz / 500;
    return true;
}
static inline bool memory_offset_khz_from_effective_mhz(int effective, int* out) {
    const long long khz = (long long)effective * 500;
    if (!out || khz < INT_MIN || khz > INT_MAX) return false;
    *out = (int)khz;
    return true;
}
static inline long long memory_offset_hz_from_effective_mhz(int effective) {
    return (long long)effective * 500000;
}
static inline bool memory_offset_effective_mhz_from_hz(long long hz, int* out) {
    if (!out || hz % 500000 || hz / 500000 < INT_MIN || hz / 500000 > INT_MAX) return false;
    *out = (int)(hz / 500000);
    return true;
}
#endif
