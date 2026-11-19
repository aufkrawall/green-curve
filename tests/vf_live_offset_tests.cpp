// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Pure live VF-curve GPU-offset detection (source/vf_live_offset_policy.h),
// assertion codes 12450-12474.
//
// Reported failure: an external curve editor flattened the tail at point 76
// (925 mV -> 2962 MHz) by writing -1927 MHz to every tail point (77..126)
// without touching lower points or setting a global GPU offset. Because the
// service leaves `lockedCi == -1` for unowned external flattens, live offset
// detection scanned the tail and reported `GPU Offset = -1927 MHz` with
// `Exclude first 76 VF points` (and a lower-voltage flatten below the midpoint
// also fooled uniform offset detection into reporting `-1927 MHz`).

#include <stdlib.h>
#include <string.h>

#include "curve_plan_fixtures.h"
#include "vf_live_offset_policy.h"

namespace {

// Realistic rising RTX 5070-shaped curve: 180 MHz floor on 0..44, rising to
// ~2482 MHz at ci=76 (925 mV) and ~3200 MHz at ci=126 (1240 mV).
void rtx_rising_stock(Table* t) {
    memset(t, 0, sizeof(*t));
    for (int i = 0; i < 127; ++i) {
        t->curve[i].volt_uV = rtx_volt_mv(i) * 1000u;
        unsigned int mhz = 180;
        if (i >= 45 && i <= 76) {
            mhz = 202u + (unsigned int)((i - 45) * (2482 - 202) / (76 - 45));
        } else if (i > 76) {
            mhz = 2482u + (unsigned int)((i - 76) * (3200 - 2482) / (126 - 76));
        }
        t->curve[i].freq_kHz = mhz * 1000u;
    }
}

VfFloorTopology floor_for(const Table& t) {
    return vf_floor_topology_compute(t.curve, t.offsets, VF_NUM_POINTS);
}

int run_external_flatten_cases() {
    // 12450: exact reported scenario — external curve editor raised ci=75
    // (+112 MHz) and ci=76 (+480 MHz to 2962 MHz) and flattened 77..126 via
    // -1927 MHz tail deltas, leaving 0..74 untouched and ownedLockCi == -1.
    // Must report no selective offset (0 MHz, exclude 0) and 0 uniform offset.
    {
        Table t;
        rtx_rising_stock(&t);
        t.offsets[75] = 112000;
        t.curve[75].freq_kHz += 112000u;
        t.offsets[76] = 2962000 - (int)t.curve[76].freq_kHz;
        t.curve[76].freq_kHz = 2962000u;
        for (int i = 77; i < 127; ++i) {
            t.offsets[i] = -1927000;
            t.curve[i].freq_kHz = 2962000u;
        }
        const VfFloorTopology floor = floor_for(t);
        const VfSelectiveGpuOffsetDetection det =
            vf_detect_live_selective_gpu_offset(t.curve, t.offsets, &floor, -1);
        if (det.detected || det.gpuOffsetMHz != 0 || det.excludeLowCount != 0) return 12450;
        if (!det.tail.fromObservedTail || det.tail.fromOwnedLock ||
            det.tail.tailStartCi != 76 || det.tail.observedPlateauMHz != 2962) return 12451;

        VfOffsetTailBoundary uTail = {};
        const int uniformKHz = vf_uniform_curve_offset_khz(t.curve, t.offsets, &floor, -1, &uTail);
        if (uniformKHz != 0 || !uTail.fromObservedTail || uTail.tailStartCi != 76) return 12452;

        // 12453: null / invalid floor falls back cleanly to computed topology.
        const VfSelectiveGpuOffsetDetection detNoFloor =
            vf_detect_live_selective_gpu_offset(t.curve, t.offsets, nullptr, -1);
        if (detNoFloor.detected || !detNoFloor.tail.fromObservedTail ||
            detNoFloor.tail.tailStartCi != 76) return 12453;
    }

    // 12454: external flatten with untouched anchor (0..76 at 0 offset, 77..126
    // flattened via -2045 MHz) -> no selective offset, 0 uniform offset.
    {
        Table t;
        rtx_rising_stock(&t);
        const unsigned int anchorKHz = t.curve[76].freq_kHz;
        for (int i = 77; i < 127; ++i) {
            t.offsets[i] = -2045000;
            t.curve[i].freq_kHz = anchorKHz;
        }
        const VfFloorTopology floor = floor_for(t);
        const VfSelectiveGpuOffsetDetection det =
            vf_detect_live_selective_gpu_offset(t.curve, t.offsets, &floor, -1);
        if (det.detected || det.tail.tailStartCi != 76 || !det.tail.fromObservedTail) return 12454;
        if (vf_uniform_curve_offset_khz(t.curve, t.offsets, &floor, -1) != 0) return 12455;
    }

    // 12456: moderate negative tail offset (-500 MHz, inside normal +/-1000 MHz
    // range) is still excluded as a programmed flat tail, proving the fix does
    // not depend on range heuristics.
    {
        Table t;
        rtx_rising_stock(&t);
        t.offsets[76] = 2800000 - (int)t.curve[76].freq_kHz;
        t.curve[76].freq_kHz = 2800000u;
        for (int i = 77; i < 127; ++i) {
            t.offsets[i] = -500000;
            t.curve[i].freq_kHz = 2800000u;
        }
        const VfFloorTopology floor = floor_for(t);
        const VfSelectiveGpuOffsetDetection det =
            vf_detect_live_selective_gpu_offset(t.curve, t.offsets, &floor, -1);
        if (det.detected || det.tail.tailStartCi != 76) return 12456;
        if (vf_uniform_curve_offset_khz(t.curve, t.offsets, &floor, -1) != 0) return 12457;
    }

    // 12458: external flatten BELOW the midpoint (anchor at ci=50 -> 76 tail
    // points > 50% of 127 populated points all carrying -1927 MHz) must report
    // 0 uniform offset (not -1927 MHz) and no selective offset.
    {
        Table t;
        rtx_rising_stock(&t);
        t.offsets[50] = 300000;
        t.curve[50].freq_kHz += 300000u;
        const unsigned int plateauKHz = t.curve[50].freq_kHz;
        for (int i = 51; i < 127; ++i) {
            t.offsets[i] = -1927000;
            t.curve[i].freq_kHz = plateauKHz;
        }
        const VfFloorTopology floor = floor_for(t);
        if (vf_uniform_curve_offset_khz(t.curve, t.offsets, &floor, -1) != 0) return 12458;
        const VfSelectiveGpuOffsetDetection det =
            vf_detect_live_selective_gpu_offset(t.curve, t.offsets, &floor, -1);
        if (det.detected || det.tail.tailStartCi != 50) return 12459;
    }

    // 12460: external flatten BELOW the midpoint (anchor at ci=50) combined
    // with a genuine uniform +200 MHz offset on 0..49 -> uniform offset reads
    // +200 MHz (not -1927 MHz and not 0), selective stays false.
    {
        Table t;
        rtx_rising_stock(&t);
        for (int i = 0; i <= 50; ++i) {
            t.offsets[i] = 200000;
            t.curve[i].freq_kHz += 200000u;
        }
        const unsigned int plateauKHz = t.curve[50].freq_kHz;
        for (int i = 51; i < 127; ++i) {
            t.offsets[i] = -1927000;
            t.curve[i].freq_kHz = plateauKHz;
        }
        const VfFloorTopology floor = floor_for(t);
        if (vf_uniform_curve_offset_khz(t.curve, t.offsets, &floor, -1) != 200000) return 12460;
        const VfSelectiveGpuOffsetDetection det =
            vf_detect_live_selective_gpu_offset(t.curve, t.offsets, &floor, -1);
        if (det.detected || det.tail.tailStartCi != 50) return 12461;
    }

    // 12462: external flatten at ci=76 combined with a genuine selective
    // +200 MHz offset excluding first 30 points -> detects +200 MHz, exclude 30.
    {
        Table t;
        rtx_rising_stock(&t);
        for (int i = 30; i < 76; ++i) {
            t.offsets[i] = 200000;
            t.curve[i].freq_kHz += 200000u;
        }
        t.offsets[76] = 2962000 - (int)t.curve[76].freq_kHz;
        t.curve[76].freq_kHz = 2962000u;
        for (int i = 77; i < 127; ++i) {
            t.offsets[i] = -1927000;
            t.curve[i].freq_kHz = 2962000u;
        }
        const VfFloorTopology floor = floor_for(t);
        const VfSelectiveGpuOffsetDetection det =
            vf_detect_live_selective_gpu_offset(t.curve, t.offsets, &floor, -1);
        if (!det.detected || det.gpuOffsetMHz != 200 || det.excludeLowCount != 30 ||
            det.tail.tailStartCi != 76) return 12462;

        // 12463: shape matcher also skips the observed flat tail cleanly.
        int repKHz = 0;
        if (!vf_live_selective_gpu_offset_matches_shape(
                t.curve, t.offsets, &floor, -1, 200, 30, 12, &repKHz) ||
            repKHz != 200000) return 12463;
        if (vf_live_selective_gpu_offset_matches_shape(
                t.curve, t.offsets, &floor, -1, -1927, 76, 12, nullptr)) return 12464;
    }

    // 12465: Pascal sub-700 mV jittered external flatten (anchor at ci=21) ->
    // recognized and excluded cleanly.
    {
        Table pascal;
        pascal_external_flatten(&pascal);
        const VfFloorTopology floor = floor_for(pascal);
        const VfSelectiveGpuOffsetDetection det =
            vf_detect_live_selective_gpu_offset(pascal.curve, pascal.offsets, &floor, -1);
        if (det.detected || !det.tail.fromObservedTail || det.tail.tailStartCi != 21) return 12465;
        if (vf_uniform_curve_offset_khz(pascal.curve, pascal.offsets, &floor, -1) != 0) return 12466;
    }

    return 0;
}

int run_legitimate_offset_and_owned_lock_cases() {
    // 12467: genuine selective negative GPU offset (-250 MHz, exclude first 70)
    // on a rising curve with no flatten is still detected accurately.
    {
        Table t;
        rtx_rising_stock(&t);
        for (int i = 70; i < 127; ++i) {
            t.offsets[i] = -250000;
            t.curve[i].freq_kHz -= 250000u;
        }
        const VfFloorTopology floor = floor_for(t);
        const VfSelectiveGpuOffsetDetection det =
            vf_detect_live_selective_gpu_offset(t.curve, t.offsets, &floor, -1);
        if (!det.detected || det.gpuOffsetMHz != -250 || det.excludeLowCount != 70 ||
            det.tail.tailStartCi != -1) return 12467;
    }

    // 12468: stock curve with a level top (rtx_stock: 71..126 all 2400 MHz)
    // has no programmed flat tail; uniform +150 MHz and selective +150 MHz
    // (exclude 70) over that level top are not mistaken for a flatten.
    {
        Table stock;
        rtx_stock(&stock, 45);
        const VfFloorTopology floor = floor_for(stock);
        const VfOffsetTailBoundary stockTail =
            vf_offset_detection_tail_start(stock.curve, stock.offsets, &floor, -1);
        if (stockTail.tailStartCi != -1 || stockTail.fromObservedTail) return 12468;

        Table uniform;
        rtx_stock(&uniform, 45);
        for (int i = 0; i < 127; ++i) {
            uniform.offsets[i] = 150000;
            uniform.curve[i].freq_kHz += 150000u;
        }
        const VfFloorTopology uFloor = floor_for(uniform);
        if (vf_uniform_curve_offset_khz(uniform.curve, uniform.offsets, &uFloor, -1) != 150000)
            return 12469;
        if (vf_detect_live_selective_gpu_offset(uniform.curve, uniform.offsets, &uFloor, -1).detected)
            return 12469;

        Table sel;
        rtx_stock(&sel, 45);
        for (int i = 70; i < 127; ++i) {
            sel.offsets[i] = 150000;
            sel.curve[i].freq_kHz += 150000u;
        }
        const VfFloorTopology sFloor = floor_for(sel);
        const VfSelectiveGpuOffsetDetection sDet =
            vf_detect_live_selective_gpu_offset(sel.curve, sel.offsets, &sFloor, -1);
        if (!sDet.detected || sDet.gpuOffsetMHz != 150 || sDet.excludeLowCount != 70 ||
            sDet.tail.tailStartCi != -1) return 12470;
    }

    // 12471: Green Curve owned lock at ci=76 over a stock level top (71..76 all
    // carrying +100 MHz selective offset from exclude=68, tail 77..126 at -100 MHz):
    // ownedLockCi=76 is preserved because 71..75 share one offset (no earlier
    // external flatten), so selective +100 MHz exclude 68 still detects.
    {
        Table t;
        rtx_stock(&t, 45);
        for (int i = 68; i <= 76; ++i) {
            t.offsets[i] = 100000;
            t.curve[i].freq_kHz += 100000u;
        }
        const unsigned int plateauKHz = t.curve[76].freq_kHz;
        for (int i = 77; i < 127; ++i) {
            t.offsets[i] = -100000;
            t.curve[i].freq_kHz = plateauKHz;
        }
        const VfFloorTopology floor = floor_for(t);
        const VfSelectiveGpuOffsetDetection det =
            vf_detect_live_selective_gpu_offset(t.curve, t.offsets, &floor, 76);
        if (!det.tail.fromOwnedLock || det.tail.tailStartCi != 76) return 12471;
        if (!det.detected || det.gpuOffsetMHz != 100 || det.excludeLowCount != 68) return 12471;
    }

    // 12472: ownedLockCi=76 superseded by an earlier external flatten at ci=60
    // (61..126 pushed to -1927 MHz) pulls the tail boundary back to ci=60 so
    // 61..75 cannot trigger a bogus -1927 MHz selective offset.
    {
        Table t;
        rtx_rising_stock(&t);
        t.offsets[60] = 200000;
        t.curve[60].freq_kHz += 200000u;
        const unsigned int plateauKHz = t.curve[60].freq_kHz;
        for (int i = 61; i < 127; ++i) {
            t.offsets[i] = -1927000;
            t.curve[i].freq_kHz = plateauKHz;
        }
        const VfFloorTopology floor = floor_for(t);
        const VfSelectiveGpuOffsetDetection det =
            vf_detect_live_selective_gpu_offset(t.curve, t.offsets, &floor, 76);
        if (det.detected || !det.tail.fromObservedTail || det.tail.tailStartCi != 60) return 12472;
    }

    // 12473: owned HARD lock at ci=76 (curve is rising, not flat) still skips
    // ci >= 76 via ownedLockCi.
    {
        Table t;
        rtx_rising_stock(&t);
        for (int i = 50; i < 76; ++i) {
            t.offsets[i] = 180000;
            t.curve[i].freq_kHz += 180000u;
        }
        const VfFloorTopology floor = floor_for(t);
        const VfSelectiveGpuOffsetDetection det =
            vf_detect_live_selective_gpu_offset(t.curve, t.offsets, &floor, 76);
        if (!det.tail.fromOwnedLock || det.tail.tailStartCi != 76) return 12473;
        if (!det.detected || det.gpuOffsetMHz != 180 || det.excludeLowCount != 50) return 12473;
    }

    // 12474: residual <= 30 MHz noise and two-point high edits before a locked
    // tail are still rejected.
    {
        Table noise;
        rtx_rising_stock(&noise);
        for (int i = 70; i < 127; ++i) {
            noise.offsets[i] = 25000;
            noise.curve[i].freq_kHz += 25000u;
        }
        const VfFloorTopology nFloor = floor_for(noise);
        if (vf_detect_live_selective_gpu_offset(noise.curve, noise.offsets, &nFloor, -1).detected)
            return 12474;

        Table twoPts;
        rtx_rising_stock(&twoPts);
        twoPts.offsets[74] = 200000;
        twoPts.curve[74].freq_kHz += 200000u;
        twoPts.offsets[75] = 200000;
        twoPts.curve[75].freq_kHz += 200000u;
        const VfFloorTopology tFloor = floor_for(twoPts);
        if (vf_detect_live_selective_gpu_offset(twoPts.curve, twoPts.offsets, &tFloor, 76).detected)
            return 12474;
    }

    return 0;
}

}  // namespace

int run_vf_live_offset_tests() {
    if (int r = run_external_flatten_cases()) return r;
    if (int r = run_legitimate_offset_and_owned_lock_cases()) return r;
    return 0;
}
