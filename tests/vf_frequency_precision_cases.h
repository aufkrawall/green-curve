// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
static int vf_frequency_precision_cases() {
    gc_i64 hz = 17;
    if (!vf_graphics_frequency_hz(&g_vfBackendPascal, 1200001, &hz) || hz != 600000500 ||
        vf_graphics_frequency_hz(&g_vfBackendPascal, 1, nullptr)) return 8740;
    if (!vf_graphics_frequency_hz(nullptr, UINT_MAX, &hz) || hz != (gc_i64)UINT_MAX * 1000) return 8741;
    VFCurvePoint curve[VF_NUM_POINTS]{};
    curve[0] = {1200001, 650000}; curve[1] = {1400001, 660000};
    curve[2] = {18000000, 500000}; // foreign domain remains raw in the legacy table
    gc_i64 exact[VF_NUM_POINTS];
    for (auto& value : exact) value = 29;
    bool valid = false;
    if (vf_normalize_graphics_status(&g_vfBackendPascal, curve, VF_NUM_POINTS, exact, &valid) != 2 || !valid ||
        curve[0].freq_kHz != 600000 || exact[0] != 600000500 || exact[1] != 700000500 ||
        curve[2].freq_kHz != 18000000 || exact[2] || exact[127]) return 8742;
    VfBackendSpec fractional = g_vfBackendPascal; fractional.graphicsFrequencyScale = 3;
    hz = 17;
    if (vf_graphics_frequency_hz(&fractional, 1, &hz) || hz != 17) return 8743;
    curve[0] = {1, 650000}; curve[1] = {}; curve[2] = {};
    vf_normalize_graphics_status(&fractional, curve, VF_NUM_POINTS, exact, &valid);
    if (valid || exact[0]) return 8744;
    vf_normalize_graphics_status(nullptr, nullptr, 0, exact, &valid);
    if (valid || exact[0]) return 8745;
    curve[0] = {1200001, 650000}; curve[1] = {0, 660000};
    vf_normalize_graphics_status(&g_vfBackendPascal, curve, VF_NUM_POINTS, exact, &valid);
    if (valid || exact[0] != 600000500 || exact[1]) return 8759;
    return 0;
}
