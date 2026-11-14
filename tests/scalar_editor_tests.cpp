// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "desired_scalar_editor.h"
#include <initializer_list>

int run_scalar_editor_tests() {
    DesiredSettings d{};
    if (!desired_memory_set_precise(&d, -123500000, CLOCK_OFFSET_SURFACE_GLOBAL) ||
        !desired_power_set_ratio(&d, {2, 3})) return 9286;
    d.curvePointCaptured[22] = 1; d.curvePointCapturedOffsetHz[22] = -100000500;
    d.hasFan = 1; d.fanPercent = 41;
    const DesiredSettings original = d;
    // Retyping the displayed integer is an explicit edit of the precise value.
    if (!desired_memory_edit_physical_mhz(&d, d.memOffsetMHz) || desired_memory_hz(d) != -123000000 ||
        !d.memOffsetPrecise || d.memOffsetSurface != CLOCK_OFFSET_SURFACE_GLOBAL || !desired_scalar_intent_valid(&d) ||
        desired_memory_intent_matches(original, d) || !desired_power_intent_matches(original, d) ||
        !d.powerLimitRatio || d.powerRatioNumerator != 2 || d.powerRatioDenominator != 3 ||
        !d.curvePointCaptured[22] || d.curvePointCapturedOffsetHz[22] != -100000500) return 9287;
    if (!desired_power_edit_percent(&d, d.powerLimitPct) || d.powerRatioNumerator != 33 || d.powerRatioDenominator != 50 ||
        desired_power_intent_matches(original, d) ||
        !desired_scalar_intent_valid(&d) || desired_memory_hz(d) != -123000000 || d.fanPercent != 41) return 9288;
    if (!desired_memory_edit_physical_mhz(&d, 0) || !d.hasMemOffset || !d.memOffsetPrecise ||
        desired_memory_hz(d) || d.memOffsetSurface != CLOCK_OFFSET_SURFACE_GLOBAL) return 9289;
    if (desired_memory_edit_physical_mhz(&d, 3001) || desired_memory_edit_physical_mhz(&d, -3001) ||
        desired_power_edit_percent(&d, POWER_LIMIT_MIN_PCT - 1) || desired_memory_hz(d) || d.powerRatioNumerator != 33) return 9290;
    d.memOffsetSurface = 99;
    if (desired_memory_edit_physical_mhz(&d, 100) || desired_memory_hz(d) || d.memOffsetSurface != 99) return 9291;
    d = {};
    d.memOffsetHz = -500; d.memOffsetSurface = 99;
    d.powerRatioNumerator = d.powerRatioDenominator = 7;
    if (!desired_memory_edit_physical_mhz(&d, -123) || !desired_power_edit_percent(&d, 150) ||
        !d.hasMemOffset || d.memOffsetPrecise || d.memOffsetHz || d.memOffsetSurface || d.memOffsetMHz != -123 ||
        !d.hasPowerLimit || d.powerLimitRatio || d.powerRatioNumerator || d.powerRatioDenominator || d.powerLimitPct != 150 ||
        !desired_scalar_intent_valid(&d)) return 9292;
    if (!desired_power_set_ratio(&d, {1, 3}) || !desired_power_edit_percent(&d, 150) ||
        d.powerRatioNumerator != 3 || d.powerRatioDenominator != 2) return 9293;
    for (const auto surface : {CLOCK_OFFSET_SURFACE_UNSPECIFIED, CLOCK_OFFSET_SURFACE_P0}) {
        if (!desired_memory_set_precise(&d, 500000, surface) || !desired_memory_edit_physical_mhz(&d, 1) ||
            desired_memory_hz(d) != 1000000 || d.memOffsetSurface != (gc_u32)surface) return 9294;
    }
    return 0;
}
