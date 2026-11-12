// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_VF_FLAT_TAIL_RECOGNITION_H
#define GREEN_CURVE_VF_FLAT_TAIL_RECOGNITION_H

// Where does a flattened VF tail start?  Pure; one coherent curve observation
// in, one anchor out.  The live detector (detect_locked_tail_from_curve())
// uses it for anchor selection; ownership and applied-lock recording stay
// with the caller.
//
// The previous detector ran an all-points exact-kHz pass before its 1 MHz
// tolerant pass, over the editor's visible points only.  A flattened Pascal
// curve read with sub-1-MHz bin jitter across its plateau (points 21..66) and
// an exact-kHz run at the top (67..79), so the exact pass anchored at point 67
// (1125 mV) before the tolerant pass could see the plateau's true start --
// which sat below 700 mV, outside the visible set anyway.  Save then stored a
// 1000 MHz flatten at 1125 mV, and after a reset to stock the hidden point 22
// (1050 MHz) refused it.
//
// Rule: candidates in ascending physical voltage (index order inside the
// graphics domain) over every writable listed point; the first candidate
// whose whole populated tail is flat wins.  "Flat" reuses the established
// thresholds unchanged: every tail point at exactly the anchor's kHz with at
// least one tail point, or within `toleranceMHz` display MHz of the anchor
// with at least `minTolerantTail` tail points.  A floor point is never a
// candidate: any write there is clamped back to the floor.

#include "vf_writable_point_policy.h"

struct VfFlatTailRecognition {
    bool found;
    int anchorCi;
    unsigned int plateauMHz;  // exact: the anchor's display MHz; tolerant: rounded mean
    int tailCount;            // populated points after the anchor
    int toleranceMHz;
    int maxDeviationKHz;      // largest |tail point - anchor| in kHz
    bool exact;
    int candidatesTried;
};

static inline unsigned int vf_flat_tail_display_mhz(unsigned int kHz) {
    return (kHz + 500u) / 1000u;
}

static inline bool vf_flat_tail_candidate(const VfFloorTopology& floor,
                                          const VFCurvePoint* curve, int ci) {
    return vf_point_listed(floor, curve, ci) && !vf_point_in_flat_floor(floor, curve, ci);
}

static inline VfFlatTailRecognition vf_recognize_flat_tail(
    const VFCurvePoint* curve, const VfFloorTopology& floor, int toleranceMHz,
    int minTolerantTail) {
    VfFlatTailRecognition out = {};
    out.anchorCi = -1;
    out.toleranceMHz = toleranceMHz;
    if (!curve || !floor.valid) return out;
    const int end = floor.graphicsEnd < VF_NUM_POINTS ? floor.graphicsEnd : VF_NUM_POINTS;
    for (int c = 0; c < end; ++c) {
        if (!vf_flat_tail_candidate(floor, curve, c)) continue;
        ++out.candidatesTried;
        const unsigned int anchorKHz = curve[c].freq_kHz;
        const unsigned int anchorMHz = vf_flat_tail_display_mhz(anchorKHz);
        int tail = 0;
        bool exact = true;
        bool tolerant = true;
        int maxDevKHz = 0;
        unsigned long long sumMHz = anchorMHz;
        for (int j = c + 1; j < end; ++j) {
            if (curve[j].freq_kHz == 0) continue;
            ++tail;
            const int devKHz = (int)curve[j].freq_kHz - (int)anchorKHz;
            const int absDev = devKHz < 0 ? -devKHz : devKHz;
            if (absDev > maxDevKHz) maxDevKHz = absDev;
            if (curve[j].freq_kHz != anchorKHz) exact = false;
            const int mhzDev = (int)vf_flat_tail_display_mhz(curve[j].freq_kHz) - (int)anchorMHz;
            if (mhzDev > toleranceMHz || mhzDev < -toleranceMHz) {
                tolerant = false;
                break;
            }
            sumMHz += vf_flat_tail_display_mhz(curve[j].freq_kHz);
        }
        if (!tolerant) continue;
        const bool exactOk = exact && tail >= 1;
        const bool tolerantOk = tail >= minTolerantTail;
        if (!exactOk && !tolerantOk) continue;
        out.found = true;
        out.anchorCi = c;
        out.tailCount = tail;
        out.exact = exactOk;
        out.maxDeviationKHz = maxDevKHz;
        const unsigned long long count = (unsigned long long)tail + 1u;
        out.plateauMHz = exactOk ? anchorMHz
                                 : (unsigned int)((sumMHz + count / 2u) / count);
        return out;
    }
    return out;
}

#endif  // GREEN_CURVE_VF_FLAT_TAIL_RECOGNITION_H
