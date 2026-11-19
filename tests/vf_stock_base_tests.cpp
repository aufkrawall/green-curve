// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Stock-base VF curve estimation for GUI/TUI previews
// (source/vf_stock_base_policy.h), assertion codes 12520-12542.
//
// Reported failure: after applying a flatten profile at point 76 (925 mV ->
// 2962 MHz, +480 MHz offset; 0..75 at 0 offset; 77..126 at -1000 MHz), driver
// neighbor cross-talk pulled untouched point 75 (920 mV, stock ~2457 MHz) up to
// 2572 MHz while freqOffsets[75] stayed 0. Loading a +475 MHz / exclude-70
// profile previewed point 75 at 2572 + 475 = 3047 MHz (above the 2957 MHz lock
// at point 76) instead of ~2932 MHz.

#include <stdlib.h>
#include <string.h>

#include "curve_plan_fixtures.h"
#include "gui_draft_desired_policy.h"
#include "vf_stock_base_policy.h"

namespace {

// Exact RTX 5070 curve around points 69..77 (stock 2295..2507 MHz, floor 180 on
// 0..44, rising to ~3172 MHz at ci=126).
void rtx5070_stock_curve(Table* t) {
    memset(t, 0, sizeof(*t));
    for (int i = 0; i < 127; ++i) {
        t->curve[i].volt_uV = rtx_volt_mv(i) * 1000u;
        unsigned int mhz = 180u;
        if (i >= 45 && i < 69) {
            mhz = 202u + (unsigned int)((i - 45) * (2295 - 202) / (69 - 45));
        } else if (i > 77) {
            mhz = 2507u + (unsigned int)((i - 77) * (3172 - 2507) / (126 - 77));
        }
        t->curve[i].freq_kHz = mhz * 1000u;
    }
    const unsigned int knownVoltMv[] = {885, 890, 895, 900, 910, 915, 920, 925, 935};
    const unsigned int knownFreqMHz[] = {2295, 2332, 2362, 2385, 2407, 2430, 2457, 2482, 2507};
    for (int k = 0; k < 9; ++k) {
        const int ci = 69 + k;
        t->curve[ci].volt_uV = knownVoltMv[k] * 1000u;
        t->curve[ci].freq_kHz = knownFreqMHz[k] * 1000u;
    }
}

void apply_profile5_with_crosstalk(Table* t, unsigned int ci75LiveMHz = 2572u) {
    t->curve[75].freq_kHz = ci75LiveMHz * 1000u;
    t->offsets[75] = 0;
    t->offsets[76] = 480000;
    t->curve[76].freq_kHz = 2962000u;
    for (int i = 77; i < 127; ++i) {
        t->offsets[i] = -1000000;
        t->curve[i].freq_kHz = 2962000u;
    }
}

VfFloorTopology floor_for(const Table& t) {
    return vf_floor_topology_compute(t.curve, t.offsets, VF_NUM_POINTS);
}

int run_reported_crosstalk_cases() {
    // 12520-12524: exact reported Profile 5 -> Profile 3 preview WITHOUT a
    // cached unshaped stock snapshot (e.g. cold GUI launch while Profile 5 is
    // already applied). Point 75 must be interpolated to 2456 MHz (+475 -> 2931
    // MHz, NOT 3047 MHz), clean points 70..74 and 76 stay exact, and clamped
    // tail points 77..126 must never fabricate 3962 MHz.
    {
        Table t;
        rtx5070_stock_curve(&t);
        apply_profile5_with_crosstalk(&t, 2572u);
        const VfFloorTopology floor = floor_for(t);
        const VfStockBaseEstimate est =
            vf_estimate_stock_base(t.curve, t.offsets, floor, nullptr, 0);
        if (!est.valid || !est.shaped || est.usedCachedStock) return 12520;
        if (est.crosstalkCount != 1 || est.firstCrosstalkCi != 75 ||
            est.firstCrosstalkRawKHz != 2572000LL)
            return 12521;
        // Voltage interpolation between ci=74 (915 mV, 2430) and ci=76 (925 mV,
        // 2482) at ci=75 (920 mV) = 2456 MHz (+475 -> 2931 MHz).
        if (est.baseKHz[75] != 2456000LL || est.baseKHz[75] + 475000LL != 2931000LL)
            return 12522;
        if (est.baseKHz[70] != 2332000LL || est.baseKHz[74] != 2430000LL ||
            est.baseKHz[76] != 2482000LL)
            return 12523;
        if (est.clampedTailCount != 50 || est.firstClampedTailCi != 77 ||
            est.baseKHz[77] != 2962000LL || est.baseKHz[126] != 2962000LL)
            return 12524;
    }

    // 12525-12528: exact reported Profile 5 -> Profile 3 preview WITH a cached
    // unshaped stock snapshot captured earlier, including -15 MHz thermal boost
    // drift between stock capture and the live shaped snapshot. Point 75 must
    // recover exact 2457 - 15 = 2442 MHz (+475 -> 2917 MHz) and tail 77..126
    // recover exact thermally aligned stock frequencies.
    {
        Table stock;
        rtx5070_stock_curve(&stock);
        const VfFloorTopology stockFloor = floor_for(stock);
        VfStockCurveCache cache = {};
        if (!vf_stock_curve_cache_observe(&cache, stock.curve, stock.offsets,
                stockFloor, 0, false) || !cache.valid)
            return 12525;

        Table live;
        rtx5070_stock_curve(&live);
        for (int i = 45; i < 127; ++i) live.curve[i].freq_kHz -= 15000u;
        // Cross-talked ci=75 pulled up +115 MHz above its thermally drifted base.
        live.curve[75].freq_kHz = (2457u - 15u + 115u) * 1000u;
        live.offsets[75] = 0;
        live.offsets[76] = 480000;
        live.curve[76].freq_kHz = (2482u - 15u + 480u) * 1000u;
        for (int i = 77; i < 127; ++i) {
            live.offsets[i] = -1000000;
            live.curve[i].freq_kHz = live.curve[76].freq_kHz;
        }
        const VfFloorTopology liveFloor = floor_for(live);
        const VfStockBaseEstimate est =
            vf_estimate_stock_base(live.curve, live.offsets, liveFloor, &cache, 0);
        if (!est.valid || !est.usedCachedStock || !est.thermalAligned ||
            est.thermalDeltaKHz != -15000LL || est.referenceCount < 20)
            return 12526;
        if (est.baseKHz[75] != 2442000LL || est.baseKHz[74] != 2415000LL ||
            est.baseKHz[76] != 2467000LL)
            return 12527;
        // Tail points recover exact stock minus thermal drift (2507-15=2492,
        // 3172-15=3157), never 3947 MHz.
        if (est.baseKHz[77] != 2492000LL || est.baseKHz[126] != 3157000LL)
            return 12528;
    }

    return 0;
}

int run_crosstalk_and_cache_edge_cases() {
    // 12529-12530: multi-point cross-talk (both ci=74 and ci=75 pulled above
    // rawBase[76]=2482) without stock cache -> both interpolated from ci=73
    // (910 mV, 2407) to ci=76 (925 mV, 2482).
    {
        Table t;
        rtx5070_stock_curve(&t);
        t.curve[74].freq_kHz = 2495000u;
        t.curve[75].freq_kHz = 2572000u;
        t.offsets[76] = 480000;
        t.curve[76].freq_kHz = 2962000u;
        for (int i = 77; i < 127; ++i) {
            t.offsets[i] = -1000000;
            t.curve[i].freq_kHz = 2962000u;
        }
        const VfStockBaseEstimate est =
            vf_estimate_stock_base(t.curve, t.offsets, floor_for(t), nullptr, 0);
        if (est.crosstalkCount != 2 || est.firstCrosstalkCi != 74) return 12529;
        // 2407 + 75 * (5/15) = 2432; 2407 + 75 * (10/15) = 2457.
        if (est.baseKHz[74] != 2432000LL || est.baseKHz[75] != 2457000LL) return 12530;
    }

    // 12531-12532: sub-monotonic upward cross-talk (+28 MHz at ci=75 -> 2518,
    // still strictly below rawBase[76]=2520 yet > 25 MHz above the 6 MHz/mV
    // linear chord 2490) is caught by the step-window chord rule and
    // interpolated between ci=74 (2460 @ 915 mV) and ci=76 (2520 @ 925 mV).
    {
        Table t;
        rtx5070_stock_curve(&t);
        t.curve[72].freq_kHz = 2370000u; // 900 mV (6 MHz/mV linear stock)
        t.curve[73].freq_kHz = 2430000u; // 910 mV
        t.curve[74].freq_kHz = 2460000u; // 915 mV
        t.curve[75].freq_kHz = 2518000u; // 920 mV: strictly < 2520, +28 above 2490 chord
        t.offsets[76] = 480000;
        t.curve[76].freq_kHz = 3000000u; // 925 mV: rawBase = 2520
        for (int i = 77; i < 127; ++i) {
            t.offsets[i] = -1000000;
            t.curve[i].freq_kHz = 3000000u;
        }
        const VfStockBaseEstimate est =
            vf_estimate_stock_base(t.curve, t.offsets, floor_for(t), nullptr, 0);
        if (est.crosstalkCount != 1 || est.firstCrosstalkCi != 75) return 12531;
        // Interpolated between ci=74 (2460) and ci=76 (2520) at 920 mV -> 2490.
        if (est.baseKHz[75] != 2490000LL) return 12532;
    }

    // 12533-12534: small (+18 MHz) cross-talk at ci=75 (still strictly below
    // rawBase[76] and below the 25 MHz uncached chord threshold) IS corrected
    // when an unshaped stock cache exists.
    {
        Table stock;
        rtx5070_stock_curve(&stock);
        VfStockCurveCache cache = {};
        if (!vf_stock_curve_cache_observe(&cache, stock.curve, stock.offsets,
                floor_for(stock), 0, false))
            return 12533;
        Table live = stock;
        apply_profile5_with_crosstalk(&live, 2457u + 18u);
        const VfStockBaseEstimate est =
            vf_estimate_stock_base(live.curve, live.offsets, floor_for(live), &cache, 0);
        if (!est.usedCachedStock || est.crosstalkCount != 1 ||
            est.firstCrosstalkCi != 75 || est.baseKHz[75] != 2457000LL)
            return 12534;
    }

    // 12535-12537: unshaped stock curve and uniform offset curve are never
    // altered (zero crosstalk/clamped counts, exact rawBase everywhere).
    {
        Table stock;
        rtx5070_stock_curve(&stock);
        // Stock level top on 118..126 must not be mistaken for a clamped tail.
        for (int i = 118; i < 127; ++i) stock.curve[i].freq_kHz = 3172000u;
        const VfStockBaseEstimate estStock =
            vf_estimate_stock_base(stock.curve, stock.offsets, floor_for(stock), nullptr, 0);
        if (!estStock.valid || estStock.shaped || estStock.crosstalkCount != 0 ||
            estStock.clampedTailCount != 0 || estStock.baseKHz[75] != 2457000LL)
            return 12535;

        Table uniform = stock;
        for (int i = 45; i < 127; ++i) {
            uniform.offsets[i] = 200000;
            uniform.curve[i].freq_kHz += 200000u;
        }
        const VfStockBaseEstimate estUni =
            vf_estimate_stock_base(uniform.curve, uniform.offsets, floor_for(uniform), nullptr, 0);
        if (!estUni.valid || !estUni.shaped || estUni.crosstalkCount != 0 ||
            estUni.clampedTailCount != 0 || estUni.baseKHz[75] != 2457000LL ||
            estUni.baseKHz[126] != 3172000LL)
            return 12536;

        // Clean selective offset with zero cross-talk stays untouched.
        Table cleanSel = stock;
        for (int i = 70; i < 127; ++i) {
            cleanSel.offsets[i] = 475000;
            cleanSel.curve[i].freq_kHz += 475000u;
        }
        const VfStockBaseEstimate estSel =
            vf_estimate_stock_base(cleanSel.curve, cleanSel.offsets, floor_for(cleanSel), nullptr, 0);
        if (estSel.crosstalkCount != 0 || estSel.clampedTailCount != 0 ||
            estSel.baseKHz[69] != 2295000LL || estSel.baseKHz[75] != 2457000LL)
            return 12537;
    }

    // 12538-12541: stock cache safety — shaped, hard-pinned, or nonzero uniform
    // scalar snapshots must not capture into cache; a topology change clears
    // any previous GPU's cache so it cannot leak across GPUs.
    {
        Table t;
        rtx5070_stock_curve(&t);
        VfStockCurveCache cache = {};
        if (vf_stock_curve_cache_observe(&cache, t.curve, t.offsets, floor_for(t), 100000, false) ||
            cache.valid)
            return 12538;
        if (vf_stock_curve_cache_observe(&cache, t.curve, t.offsets, floor_for(t), 0, true) ||
            cache.valid)
            return 12539;
        apply_profile5_with_crosstalk(&t, 2572u);
        if (vf_stock_curve_cache_observe(&cache, t.curve, t.offsets, floor_for(t), 0, false) ||
            cache.valid)
            return 12540;

        // Capture on GPU A, then observe a shaped GPU B with different voltages:
        // cache must be cleared rather than applied to GPU B.
        Table gpuA;
        rtx5070_stock_curve(&gpuA);
        if (!vf_stock_curve_cache_observe(&cache, gpuA.curve, gpuA.offsets, floor_for(gpuA), 0, false) ||
            !cache.valid)
            return 12541;
        Table gpuB = gpuA;
        for (int i = 45; i < 127; ++i) gpuB.curve[i].volt_uV += 5000u;
        apply_profile5_with_crosstalk(&gpuB, 2572u);
        if (vf_stock_curve_cache_observe(&cache, gpuB.curve, gpuB.offsets, floor_for(gpuB), 0, false) ||
            cache.valid)
            return 12542;
    }

    return 0;
}

int run_projected_draft_point_cases() {
    // 12544-12550: loading an indexed portable profile whose projected points
    // omit observed_frequency_hz (hasCurvePoint=1, curvePointFromGpuOffset=1,
    // curvePointMHz=0, e.g. Profile 3) must resolve pre-tail points from the
    // crosstalk-safe stock base + GPU offset component and locked-tail points
    // from lockMHz, never writing 0 / invalid into the editor draft.
    Table t;
    rtx5070_stock_curve(&t);
    apply_profile5_with_crosstalk(&t, 2572u);
    const VfStockBaseEstimate est =
        vf_estimate_stock_base(t.curve, t.offsets, floor_for(t), nullptr, 0);

    DesiredSettings profile3 = {};
    profile3.hasGpuOffset = 1;
    profile3.gpuOffsetMHz = 475;
    profile3.gpuOffsetExcludeLowCount = 70;
    profile3.hasLock = 1;
    profile3.lockCi = 76;
    profile3.lockMHz = 2957;
    profile3.lockMode = LOCK_MODE_FLATTEN;
    profile3.lockTracksAnchor = 0;
    for (int ci = 70; ci <= 126; ++ci) {
        profile3.hasCurvePoint[ci] = 1;
        profile3.curvePointFromGpuOffset[ci] = 1;
        profile3.curvePointMHz[ci] = 0;  // no observed_frequency_hz in document
    }

    bool fromLock = false, fromProj = false;
    // 12544: pre-tail projected point 70 -> 2332 + 475 = 2807 MHz.
    if (gui_draft_desired_point_mhz(profile3, 70, &fromLock, est.baseKHz[70], 475, &fromProj) != 2807u ||
        fromLock || !fromProj)
        return 12544;
    // 12545: cross-talked pre-tail projected point 75 -> 2456 + 475 = 2931 MHz
    // (NOT 0, and NOT 2572 + 475 = 3047 MHz!).
    if (gui_draft_desired_point_mhz(profile3, 75, &fromLock, est.baseKHz[75], 475, &fromProj) != 2931u ||
        fromLock || !fromProj)
        return 12545;
    // 12546: lock anchor point 76 -> lockMHz = 2957 MHz (fromLock=true).
    if (gui_draft_desired_point_mhz(profile3, 76, &fromLock, est.baseKHz[76], 475, &fromProj) != 2957u ||
        !fromLock || fromProj)
        return 12546;
    // 12547: zero-MHz projected tail point 77 -> lockMHz = 2957 MHz (fromLock=true).
    if (gui_draft_desired_point_mhz(profile3, 77, &fromLock, est.baseKHz[77], 475, &fromProj) != 2957u ||
        !fromLock || fromProj)
        return 12547;
    // 12548: offline / unknown stock base (0 kHz) -> returns 0 cleanly so the
    // caller skips writing an invalid "0" into the draft.
    if (gui_draft_desired_point_mhz(profile3, 70, &fromLock, 0, 475, &fromProj) != 0u ||
        fromLock || fromProj)
        return 12548;
    // 12549: explicit non-zero point MHz always wins over projection fallback.
    profile3.curvePointMHz[70] = 2815u;
    if (gui_draft_desired_point_mhz(profile3, 70, &fromLock, est.baseKHz[70], 475, &fromProj) != 2815u ||
        fromLock || fromProj)
        return 12549;
    // 12550: non-projected point with MHz==0 (invalid/absent) never synthesizes
    // a frequency from stockBaseKHz.
    DesiredSettings nonProj = {};
    nonProj.hasGpuOffset = 1;
    nonProj.gpuOffsetMHz = 475;
    nonProj.hasCurvePoint[70] = 1;
    nonProj.curvePointFromGpuOffset[70] = 0;
    nonProj.curvePointMHz[70] = 0;
    if (gui_draft_desired_point_mhz(nonProj, 70, &fromLock, est.baseKHz[70], 475, &fromProj) != 0u)
        return 12550;

    return 0;
}

}  // namespace

int run_vf_stock_base_tests() {
    if (int rc = run_reported_crosstalk_cases()) return rc;
    if (int rc = run_crosstalk_and_cache_edge_cases()) return rc;
    if (int rc = run_projected_draft_point_cases()) return rc;
    return 0;
}
