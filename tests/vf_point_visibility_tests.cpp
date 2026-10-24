// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Which VF points the editor shows (vf_point_visibility_policy.h), assertion
// codes 7380-7399.  The grid's layout and a profile's `pointN_visible` flags
// follow this set, so it must come out the same for every read of one GPU,
// whatever the driver's temperature compensation does to the stock curve.

#include <stdio.h>
#include <string.h>

#include "vf_point_visibility_policy.h"

namespace {

const int kPoints = 128;

// An RTX 5070 VF table: voltages are fixed per index.  0-126 rise in the
// driver's 5/10 mV pattern from 450 mV to 1240 mV; 127 is the out-of-order
// idle placeholder (540 mV).
unsigned int table_volt_mv(int i) {
    if (i == 127) return 540;
    unsigned int mv = 450;
    for (int k = 1; k <= i; ++k) mv += (k % 4 == 1) ? 10 : 5;
    return mv;
}

// Stock frequencies: a flat 180 MHz floor, a ~85 MHz/point ramp, then the
// boost region.  `rampStart` is the first point above the floor; the driver
// moves it about two points to the right when the GPU is cold.
void stock_curve(int rampStart, unsigned int freq_kHz[kPoints],
                 unsigned int volt_uV[kPoints]) {
    for (int i = 0; i < kPoints; ++i) {
        volt_uV[i] = table_volt_mv(i) * 1000u;
        unsigned int mhz = 180;
        if (i >= rampStart) {
            mhz = 202 + (unsigned int)(i - rampStart) * 85u;
            if (mhz > 2400) mhz = 2400;
        }
        freq_kHz[i] = mhz * 1000u;
    }
    freq_kHz[127] = 405000;
}

int visible_set(const unsigned int freq_kHz[kPoints], const unsigned int volt_uV[kPoints],
                bool out[kPoints]) {
    int count = 0;
    for (int i = 0; i < kPoints; ++i) {
        out[i] = vf_point_visible_in_editor(freq_kHz[i], volt_uV[i]);
        if (out[i]) ++count;
    }
    return count;
}

// The rule this replaced: voltage AND a stock frequency of at least 500 MHz.
int legacy_visible_count(const unsigned int freq_kHz[kPoints],
                         const unsigned int volt_uV[kPoints]) {
    int count = 0;
    for (int i = 0; i < kPoints; ++i) {
        if (freq_kHz[i] == 0) continue;
        if (volt_uV[i] / 1000u >= 700u && freq_kHz[i] / 1000u >= 500u) ++count;
    }
    return count;
}

}  // namespace

int run_vf_point_visibility_tests() {
    unsigned int warmFreq[kPoints], warmVolt[kPoints];
    unsigned int coldFreq[kPoints], coldVolt[kPoints];
    stock_curve(45, warmFreq, warmVolt);  // 30 C read
    stock_curve(47, coldFreq, coldVolt);  // 22 C logon read
    bool warm[kPoints], cold[kPoints];

    // 7380: the fixture reproduces the report: the old rule showed two points
    // fewer at the cold logon read than after a warm Refresh.
    if (legacy_visible_count(warmFreq, warmVolt) !=
        legacy_visible_count(coldFreq, coldVolt) + 2) return 7380;

    // 7381: the same table yields the same visible set cold and warm.
    int warmCount = visible_set(warmFreq, warmVolt, warm);
    int coldCount = visible_set(coldFreq, coldVolt, cold);
    if (warmCount != coldCount) return 7381;
    if (memcmp(warm, cold, sizeof(warm)) != 0) return 7382;

    // 7383: every point from 700 mV up to the top is visible, including the
    // 180 MHz floor entries at or above 700 mV.
    for (int i = 0; i < 127; ++i) {
        bool expected = table_volt_mv(i) >= 700u;
        if (warm[i] != expected) return 7383;
    }
    if (!warm[44] || warmFreq[44] != 180000) return 7384;

    // 7385: the out-of-order 540 mV placeholder stays hidden.
    if (warm[127]) return 7385;

    // 7386: an unpopulated entry is never visible, whatever its voltage.
    if (vf_point_visible_in_editor(0, 900000)) return 7386;

    // 7387: the threshold is inclusive at exactly MIN_VISIBLE_VOLT_mV, and a
    // sub-millivolt remainder does not round a point into view.
    if (!vf_point_visible_in_editor(180000, MIN_VISIBLE_VOLT_mV * 1000u)) return 7387;
    if (vf_point_visible_in_editor(2000000, MIN_VISIBLE_VOLT_mV * 1000u - 1u)) return 7388;

    // 7389: frequency never decides: a live readback anywhere from the floor
    // to a fully boosted value leaves a 760 mV point's answer unchanged.
    const unsigned int freqs[] = {1000u, 180000u, 360000u, 499999u, 500000u, 547000u, 3000000u};
    for (unsigned int f : freqs) {
        if (!vf_point_visible_in_editor(f, 760000u)) return 7389;
    }
    // 7390-7392: profile projection uses the grid rule, clears all of a
    // hidden point's saved intent, and preserves even the low-frequency floor.
    VFCurvePoint live[VF_NUM_POINTS] = {};
    DesiredSettings saved = {};
    for (int i = 0; i < 4; ++i) {
        saved.hasCurvePoint[i] = true;
        saved.curvePointMHz[i] = 2000;
        saved.curvePointFromGpuOffset[i] = true;
    }
    live[0].freq_kHz = 0;       live[0].volt_uV = 900000; // unpopulated
    live[1].freq_kHz = 2000000; live[1].volt_uV = 699999; // below threshold
    live[2].freq_kHz = 180000;  live[2].volt_uV = 700000; // visible floor
    live[3].freq_kHz = 2500000; live[3].volt_uV = 1000000;
    if (vf_filter_hidden_editor_curve_points(&saved, live) != 2) return 7390;
    for (int i = 0; i < 2; ++i)
        if (saved.hasCurvePoint[i] || saved.curvePointMHz[i] ||
            saved.curvePointFromGpuOffset[i]) return 7391;
    for (int i = 2; i < 4; ++i)
        if (!saved.hasCurvePoint[i] || saved.curvePointMHz[i] != 2000 ||
            !saved.curvePointFromGpuOffset[i]) return 7392;
    if (vf_filter_hidden_editor_curve_points(&saved, live) != 0) return 7392;
    return 0;
}
