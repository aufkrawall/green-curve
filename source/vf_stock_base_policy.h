// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_VF_STOCK_BASE_POLICY_H
#define GREEN_CURVE_VF_STOCK_BASE_POLICY_H

// Stock-base VF frequency estimation for GUI/TUI curve previews.
//
// The naive formula `live_freq - freqOffsets[ci]` is exact ONLY on an unshaped
// curve or on points far from offset discontinuities and monotonic clamps. On
// real NVIDIA hardware (confirmed on Blackwell RTX 5070), two driver behaviors
// distort `live - offset` on a shaped curve:
//
//   1. Neighbor cross-talk / slope coupling: raising point N (e.g. ci=76,
//      925 mV -> +480 MHz to 2962 MHz) while leaving point N-1 (ci=75, 920 mV)
//      at offset=0 causes the driver to pull live[75] up from ~2457 MHz to
//      2572 MHz without changing freqOffsets[75] (still 0). Naive `live - 0`
//      then claims stock[75] = 2572 MHz (> stock[76] = 2482 MHz), so previewing
//      a +475 MHz profile projects point 75 at 3047 MHz instead of ~2932 MHz.
//   2. Monotonic clamping on flattened tails: tail points (ci=77..126) carry
//      large negative offsets (e.g. -1000 MHz) clamped up to the plateau
//      (2962 MHz), so `live - offset` fabricates 3962 MHz.
//
// Two-tier pure policy:
//   - Tier 1 (observed unshaped stock cache): whenever an unshaped snapshot
//     (zero offsets, no uniform scalar, no hard pin) is observed for the same
//     GPU topology, retain it and align it to the live snapshot's thermal/boost
//     shift via the median delta over clean zero-offset points far from any
//     offset step. Use that aligned stock value for any corrupted or
//     step-adjacent point that diverges from it.
//   - Tier 2 (shaped-curve fallback when no stock cache exists yet): mark
//     monotonically clamped points and cross-talk-pulled points (strict stock
//     monotonicity violations against a higher-offset reliable point above, plus
//     large upward spikes above the local voltage chord right before a >=150 MHz
//     positive offset step), then linearly interpolate interior points by
//     voltage between their nearest clean neighbors and bound clamped tails.
//
// Clean points far from steps always keep exact live `freq - offset` with zero
// thermal lag. Unshaped curves return untouched.

#include "vf_flat_tail_recognition.h"

enum {
    VF_STOCK_BASE_CLAMP_FREQ_TOL_KHZ    = 1000,    // 1 MHz monotonic plateau equality
    VF_STOCK_BASE_CLAMP_OFFSET_STEP_KHZ = 15000,   // 15 MHz offset step confirming clamp
    VF_STOCK_BASE_CROSSTALK_STEP_KHZ    = 150000,  // 150 MHz step inducing neighbor pull
    VF_STOCK_BASE_CROSSTALK_WINDOW      = 3,       // writable neighbors checked/excluded
    VF_STOCK_BASE_CHORD_EXCESS_KHZ      = 25000,   // 25 MHz above local voltage chord
    VF_STOCK_BASE_CACHE_DEV_KHZ         = 15000,   // 15 MHz (~2 bins) cache deviation
};

struct VfStockCurveCache {
    bool valid;
    unsigned int topologyKey;
    int graphicsEnd;
    VFCurvePoint curve[VF_NUM_POINTS];
};

struct VfStockBaseEstimate {
    bool valid;
    bool shaped;
    bool usedCachedStock;
    bool thermalAligned;
    long long thermalDeltaKHz;
    int referenceCount;
    int crosstalkCount;
    int firstCrosstalkCi;
    long long firstCrosstalkRawKHz;
    long long firstCrosstalkEstKHz;
    int clampedTailCount;
    int firstClampedTailCi;
    long long baseKHz[VF_NUM_POINTS];
};

// Observe a live curve snapshot and capture it when genuinely unshaped.
// Clears any stale cache belonging to a different GPU/topology.
static inline bool vf_stock_curve_cache_observe(
    VfStockCurveCache* cache, const VFCurvePoint* curve,
    const int* freqOffsetsKHz, const VfFloorTopology& floor,
    int uniformScalarOffsetKHz, bool hardPinned) {
    if (!cache || !curve || !freqOffsetsKHz) return false;
    const VfFloorTopology f = floor.valid
        ? floor : vf_floor_topology_compute(curve, freqOffsetsKHz, VF_NUM_POINTS);
    if (!f.valid) return false;
    const unsigned int key = vf_floor_topology_key(curve, VF_NUM_POINTS, 0);
    if (cache->valid && cache->topologyKey != key) {
        *cache = VfStockCurveCache{};
    }
    if (hardPinned || uniformScalarOffsetKHz != 0) return false;

    const int end = f.graphicsEnd < VF_NUM_POINTS ? f.graphicsEnd : VF_NUM_POINTS;
    int writablePopulated = 0;
    for (int ci = 0; ci < end; ++ci) {
        if (curve[ci].freq_kHz == 0) continue;
        if (freqOffsetsKHz[ci] != 0) return false;
        if (vf_point_listed(f, curve, ci) && !vf_point_in_flat_floor(f, curve, ci))
            ++writablePopulated;
    }
    if (writablePopulated <= 0) return false;

    cache->valid = true;
    cache->topologyKey = key;
    cache->graphicsEnd = end;
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) cache->curve[ci] = curve[ci];
    return true;
}

static inline long long vf_stock_base_interp_khz(
    const VFCurvePoint* curve, int loCi, long long loBaseKHz,
    int hiCi, long long hiBaseKHz, int ci) {
    const unsigned int vLo = curve[loCi].volt_uV;
    const unsigned int vHi = curve[hiCi].volt_uV;
    const unsigned int vCi = curve[ci].volt_uV;
    long long est = loBaseKHz;
    if (vHi > vLo && vCi >= vLo && vCi <= vHi) {
        const long long diff = hiBaseKHz - loBaseKHz;
        const long long num = diff * (long long)(vCi - vLo);
        const long long den = (long long)(vHi - vLo);
        est = loBaseKHz + (num >= 0 ? (num + den / 2) / den : (num - den / 2) / den);
    } else if (hiCi > loCi) {
        const long long diff = hiBaseKHz - loBaseKHz;
        const long long num = diff * (long long)(ci - loCi);
        const long long den = (long long)(hiCi - loCi);
        est = loBaseKHz + (num >= 0 ? (num + den / 2) / den : (num - den / 2) / den);
    }
    const long long minB = loBaseKHz < hiBaseKHz ? loBaseKHz : hiBaseKHz;
    const long long maxB = loBaseKHz > hiBaseKHz ? loBaseKHz : hiBaseKHz;
    if (est < minB) est = minB;
    if (est > maxB) est = maxB;
    if (est < 0) est = 0;
    return est;
}

static inline long long vf_stock_base_median(long long* values, int count) {
    if (!values || count <= 0) return 0;
    for (int i = 1; i < count; ++i) {
        const long long key = values[i];
        int j = i - 1;
        while (j >= 0 && values[j] > key) {
            values[j + 1] = values[j];
            --j;
        }
        values[j + 1] = key;
    }
    return values[count / 2];
}

// Estimate stock base frequencies (kHz) across the VF curve.
static inline VfStockBaseEstimate vf_estimate_stock_base(
    const VFCurvePoint* curve, const int* freqOffsetsKHz,
    const VfFloorTopology& floor, const VfStockCurveCache* cache,
    int uniformScalarOffsetKHz) {
    VfStockBaseEstimate out = {};
    out.firstCrosstalkCi = -1;
    out.firstClampedTailCi = -1;
    if (!curve || !freqOffsetsKHz) return out;

    const VfFloorTopology f = floor.valid
        ? floor : vf_floor_topology_compute(curve, freqOffsetsKHz, VF_NUM_POINTS);
    const int end = (f.valid && f.graphicsEnd < VF_NUM_POINTS)
        ? f.graphicsEnd : VF_NUM_POINTS;

    long long rawBase[VF_NUM_POINTS] = {};
    bool anyOffset = false;
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        if (curve[ci].freq_kHz == 0) continue;
        long long b = (long long)curve[ci].freq_kHz - (long long)freqOffsetsKHz[ci];
        if (b < 0) b = 0;
        rawBase[ci] = b;
        out.baseKHz[ci] = b;
        if (ci < end && freqOffsetsKHz[ci] != 0) anyOffset = true;
    }
    out.valid = true;
    if (!anyOffset || !f.valid) return out;
    out.shaped = true;

    int writable[VF_NUM_POINTS] = {};
    int wCount = 0;
    for (int ci = 0; ci < end; ++ci) {
        if (curve[ci].freq_kHz == 0) continue;
        if (vf_point_listed(f, curve, ci) && !vf_point_in_flat_floor(f, curve, ci))
            writable[wCount++] = ci;
    }
    if (wCount < 2) return out;

    bool unreliable[VF_NUM_POINTS] = {};
    bool clamped[VF_NUM_POINTS] = {};
    bool crosstalk[VF_NUM_POINTS] = {};

    // 1. Monotonically clamped points: programmed flat tail plus any pair where
    // a lower programmed offset did not drop below its predecessor's frequency
    // (or a higher offset pinned against its successor's ceiling).
    const VfFlatTailRecognition tail =
        vf_recognize_programmed_flat_tail(curve, freqOffsetsKHz, f, 1, 3);
    if (tail.found && tail.anchorCi >= 0 && tail.anchorCi < end) {
        const int anchorOff = freqOffsetsKHz[tail.anchorCi];
        for (int k = 0; k < wCount; ++k) {
            const int ci = writable[k];
            if (ci > tail.anchorCi && freqOffsetsKHz[ci] < anchorOff) {
                clamped[ci] = true;
                unreliable[ci] = true;
            }
        }
    }
    for (int k = 1; k < wCount; ++k) {
        const int prev = writable[k - 1];
        const int ci = writable[k];
        if (curve[ci].freq_kHz <= curve[prev].freq_kHz + (unsigned int)VF_STOCK_BASE_CLAMP_FREQ_TOL_KHZ &&
            freqOffsetsKHz[ci] < freqOffsetsKHz[prev] - VF_STOCK_BASE_CLAMP_OFFSET_STEP_KHZ) {
            clamped[ci] = true;
            unreliable[ci] = true;
        }
    }

    // 2. Strict stock-curve monotonicity violation caused by upward cross-talk:
    // a reliable higher-voltage point carries a strictly lower raw base AND a
    // strictly higher programmed offset than point `ci` (preventing a downward
    // anomaly at a negative-offset point from blaming lower clean points).
    bool haveSuffixMin = false;
    long long suffixMin = 0;
    int suffixMinOffset = 0;
    for (int k = wCount - 1; k >= 0; --k) {
        const int ci = writable[k];
        if (unreliable[ci]) continue;
        if (haveSuffixMin && rawBase[ci] > suffixMin &&
            suffixMinOffset > freqOffsetsKHz[ci]) {
            crosstalk[ci] = true;
            unreliable[ci] = true;
            continue;
        }
        if (!haveSuffixMin || rawBase[ci] < suffixMin) {
            haveSuffixMin = true;
            suffixMin = rawBase[ci];
            suffixMinOffset = freqOffsetsKHz[ci];
        }
    }

    // 3. Sub-monotonic upward cross-talk right before a large positive offset
    // step: stock VF curves are smooth/concave-down (f'' <= 0), so a clean
    // point may sit above a wide secant chord but never jumps >25 MHz above
    // BOTH the secant chord to the step AND the left tangent extrapolated from
    // the clean points immediately below.
    for (int s = 1; s < wCount; ++s) {
        const int stepCi = writable[s];
        if (unreliable[stepCi]) continue;
        const int prevCi = writable[s - 1];
        if (freqOffsetsKHz[stepCi] - freqOffsetsKHz[prevCi] < VF_STOCK_BASE_CROSSTALK_STEP_KHZ)
            continue;
        int winStart = s - VF_STOCK_BASE_CROSSTALK_WINDOW;
        if (winStart < 1) winStart = 1;
        int loIdx = -1;
        for (int j = winStart - 1; j >= 0; --j) {
            if (!unreliable[writable[j]]) {
                loIdx = j;
                break;
            }
        }
        if (loIdx < 0) continue;
        int loPrevIdx = -1;
        for (int j = loIdx - 1; j >= 0; --j) {
            if (!unreliable[writable[j]]) {
                loPrevIdx = j;
                break;
            }
        }
        const int loCi = writable[loIdx];
        const int loPrevCi = loPrevIdx >= 0 ? writable[loPrevIdx] : -1;
        for (int k = winStart; k < s; ++k) {
            const int ci = writable[k];
            if (unreliable[ci]) continue;
            if (freqOffsetsKHz[stepCi] - freqOffsetsKHz[ci] < VF_STOCK_BASE_CROSSTALK_STEP_KHZ)
                continue;
            const long long chordKHz = vf_stock_base_interp_khz(
                curve, loCi, rawBase[loCi], stepCi, rawBase[stepCi], ci);
            if (rawBase[ci] <= chordKHz + VF_STOCK_BASE_CHORD_EXCESS_KHZ) continue;
            if (loPrevCi >= 0 && curve[loCi].volt_uV > curve[loPrevCi].volt_uV &&
                curve[ci].volt_uV >= curve[loCi].volt_uV) {
                const long long slopeNum = (rawBase[loCi] - rawBase[loPrevCi]) *
                    (long long)(curve[ci].volt_uV - curve[loCi].volt_uV);
                const long long slopeDen = (long long)(curve[loCi].volt_uV - curve[loPrevCi].volt_uV);
                const long long tangentKHz = rawBase[loCi] +
                    (slopeNum >= 0 ? (slopeNum + slopeDen / 2) / slopeDen
                                   : (slopeNum - slopeDen / 2) / slopeDen);
                if (rawBase[ci] <= tangentKHz + VF_STOCK_BASE_CHORD_EXCESS_KHZ) continue;
            }
            crosstalk[ci] = true;
            unreliable[ci] = true;
        }
    }

    // 4. Optional thermally-aligned stock cache (same GPU topology, no uniform
    // scalar offset). Clean zero-offset points far from any non-zero offset
    // measure the current temperature/boost shift without cross-talk bias.
    const bool cacheMatches = cache && cache->valid && uniformScalarOffsetKHz == 0 &&
        cache->topologyKey == vf_floor_topology_key(curve, VF_NUM_POINTS, 0);
    if (cacheMatches) {
        long long deltas[VF_NUM_POINTS] = {};
        int deltaCount = 0;
        for (int k = 0; k < wCount; ++k) {
            const int ci = writable[k];
            if (unreliable[ci] || freqOffsetsKHz[ci] != 0) continue;
            if (cache->curve[ci].freq_kHz == 0 ||
                cache->curve[ci].volt_uV != curve[ci].volt_uV)
                continue;
            bool cleanWindow = true;
            const int wLo = k - VF_STOCK_BASE_CROSSTALK_WINDOW > 0
                ? k - VF_STOCK_BASE_CROSSTALK_WINDOW : 0;
            const int wHi = k + VF_STOCK_BASE_CROSSTALK_WINDOW < wCount - 1
                ? k + VF_STOCK_BASE_CROSSTALK_WINDOW : wCount - 1;
            for (int j = wLo; j <= wHi; ++j) {
                if (freqOffsetsKHz[writable[j]] != 0) {
                    cleanWindow = false;
                    break;
                }
            }
            if (!cleanWindow) continue;
            deltas[deltaCount++] =
                (long long)curve[ci].freq_kHz - (long long)cache->curve[ci].freq_kHz;
        }
        if (deltaCount > 0) {
            out.thermalAligned = true;
            out.referenceCount = deltaCount;
            out.thermalDeltaKHz = vf_stock_base_median(deltas, deltaCount);
            // Catch smaller cross-talk near a large offset step when we have an
            // exact thermally aligned stock baseline to compare against.
            for (int k = 0; k < wCount; ++k) {
                const int ci = writable[k];
                if (unreliable[ci] || cache->curve[ci].freq_kHz == 0) continue;
                bool nearStep = false;
                const int wLo = k - VF_STOCK_BASE_CROSSTALK_WINDOW > 0
                    ? k - VF_STOCK_BASE_CROSSTALK_WINDOW : 0;
                const int wHi = k + VF_STOCK_BASE_CROSSTALK_WINDOW < wCount - 1
                    ? k + VF_STOCK_BASE_CROSSTALK_WINDOW : wCount - 1;
                for (int j = wLo; j <= wHi; ++j) {
                    const int dOff = freqOffsetsKHz[writable[j]] - freqOffsetsKHz[ci];
                    if (dOff >= VF_STOCK_BASE_CROSSTALK_STEP_KHZ ||
                        dOff <= -VF_STOCK_BASE_CROSSTALK_STEP_KHZ) {
                        nearStep = true;
                        break;
                    }
                }
                if (!nearStep) continue;
                long long expected = (long long)cache->curve[ci].freq_kHz + out.thermalDeltaKHz;
                if (expected < 0) expected = 0;
                const long long diff = rawBase[ci] - expected;
                if (diff > VF_STOCK_BASE_CACHE_DEV_KHZ || diff < -VF_STOCK_BASE_CACHE_DEV_KHZ) {
                    crosstalk[ci] = true;
                    unreliable[ci] = true;
                }
            }
        }
    }

    // 5. Reconstruct every unreliable point from the aligned stock cache when
    // available, otherwise by voltage interpolation (interior) or bounded
    // fallback (clamped tail).
    for (int k = 0; k < wCount; ++k) {
        const int ci = writable[k];
        if (!unreliable[ci]) continue;
        long long est = rawBase[ci];
        if (out.thermalAligned && cache && cache->curve[ci].freq_kHz > 0) {
            est = (long long)cache->curve[ci].freq_kHz + out.thermalDeltaKHz;
            if (est < 0) est = 0;
            out.usedCachedStock = true;
        } else {
            int loCi = -1;
            for (int j = k - 1; j >= 0; --j) {
                if (!unreliable[writable[j]]) { loCi = writable[j]; break; }
            }
            int hiCi = -1;
            for (int j = k + 1; j < wCount; ++j) {
                if (!unreliable[writable[j]]) { hiCi = writable[j]; break; }
            }
            if (loCi >= 0 && hiCi >= 0) {
                est = vf_stock_base_interp_khz(
                    curve, loCi, rawBase[loCi], hiCi, rawBase[hiCi], ci);
            } else if (clamped[ci] && loCi >= 0) {
                est = (long long)curve[ci].freq_kHz;
                if (rawBase[loCi] > est) est = rawBase[loCi];
            } else if (loCi >= 0) {
                est = rawBase[loCi];
            } else if (hiCi >= 0) {
                est = rawBase[hiCi];
            }
        }
        out.baseKHz[ci] = est;
        if (clamped[ci]) {
            if (out.firstClampedTailCi < 0) out.firstClampedTailCi = ci;
            ++out.clampedTailCount;
        } else if (crosstalk[ci]) {
            if (out.firstCrosstalkCi < 0) {
                out.firstCrosstalkCi = ci;
                out.firstCrosstalkRawKHz = rawBase[ci];
                out.firstCrosstalkEstKHz = est;
            }
            ++out.crosstalkCount;
        }
    }

    return out;
}

#endif  // GREEN_CURVE_VF_STOCK_BASE_POLICY_H
