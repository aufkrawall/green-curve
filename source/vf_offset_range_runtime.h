// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The single adapter between live GPU capability state and the pure range
// decision in vf_offset_range_policy.h, plus the clamps derived from it.
//
// Split out of main_runtime_nvml.cpp for the file-size guidance and included
// from the exact position it occupied, so declaration order in the amalgamated
// translation unit is unchanged.
//
// Everything that needs to know how far a VF curve point may be pushed goes
// through here: the FLATTEN tail floor, the apply's out-of-range refusal, the
// write clamp and verification.  Before CT-03 was fixed there were three
// answers and they disagreed -- a planner generating a tail floor that its own
// pre-check then refused, with the refusal reported to the user as success.
#ifndef GREEN_CURVE_VF_OFFSET_RANGE_RUNTIME_H
#define GREEN_CURVE_VF_OFFSET_RANGE_RUNTIME_H

// THE VF offset range for this GPU, as one value.
//
// The single adapter between live capability state and vf_offset_range_policy.h.
// Every consumer -- the FLATTEN tail floor, the apply's out-of-range refusal,
// the low-level write clamp and verification -- goes through here, so the
// three disagreeing answers that produced CT-03 cannot come back: a planner
// generating a floor its own pre-check then refuses is now unrepresentable.
//
// The GPU offset range (±1000 MHz) is the authoritative hardware capability
// that the driver actually accepts for VF curve offsets, superseding the
// narrower curve-specific range reported by NVML.
static VfOffsetRange vf_offset_range_current() {
    if (g_app.gpuOffsetRangeKnown &&
        g_app.gpuClockOffsetMinMHz <= g_app.gpuClockOffsetMaxMHz &&
        g_app.gpuClockOffsetMinMHz >= INT_MIN / 1000 &&
        g_app.gpuClockOffsetMaxMHz <= INT_MAX / 1000) {
        return vf_offset_range_from_probe(true, g_app.gpuClockOffsetMinMHz * 1000,
                                          g_app.gpuClockOffsetMaxMHz * 1000);
    }
    if (g_app.curveOffsetRangeKnown &&
        g_app.curveOffsetMinkHz <= g_app.curveOffsetMaxkHz) {
        return vf_offset_range_from_probe(true, g_app.curveOffsetMinkHz,
                                          g_app.curveOffsetMaxkHz);
    }
    return vf_offset_range_fallback();
}

static bool get_curve_offset_range_khz(int* minkHz, int* maxkHz) {
    VfOffsetRange r = vf_offset_range_current();
    if (minkHz) *minkHz = r.minKHz;
    if (maxkHz) *maxkHz = r.maxKHz;
    return r.known;
}


// Clamp a requested VF delta to the legal range, reporting whether the value
// actually sent to the driver differs from the one that was asked for.
//
// The reporting half matters: a silently clamped delta becomes a DIFFERENT
// request, which verification then compares against the ORIGINAL target and
// fails -- or, worse, accepts by absorbing the mismatch.  Callers that can act
// on the difference now can; the unreported wrapper below keeps the old shape
// for the many call sites where the value is already range-checked upstream.
static int clamp_freq_delta_khz_reported(int freqDelta_kHz, bool* alteredOut) {
    VfOffsetRange r = vf_offset_range_current();
    bool altered = false;
    int clamped = vf_offset_range_clamp_khz(r, freqDelta_kHz, &altered);
    if (altered) {
        debug_log("clamp_freq_delta_khz: clamping %d kHz into range %d..%d kHz"
                  " (known=%d) -> %d kHz\n",
            freqDelta_kHz, r.minKHz, r.maxKHz, r.known ? 1 : 0, clamped);
    }
    if (alteredOut) *alteredOut = altered;
    return clamped;
}

static int clamp_freq_delta_khz(int freqDelta_kHz) {
    return clamp_freq_delta_khz_reported(freqDelta_kHz, nullptr);
}

static void set_curve_offset_range_khz(int minkHz, int maxkHz) {
    if (minkHz > maxkHz) return;
    g_app.curveOffsetMinkHz = minkHz;
    g_app.curveOffsetMaxkHz = maxkHz;
    g_app.curveOffsetRangeKnown = true;
}

#endif  // GREEN_CURVE_VF_OFFSET_RANGE_RUNTIME_H
