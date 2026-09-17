// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_CURVE_POINT_OFFSET_POLICY_H
#define GREEN_CURVE_CURVE_POINT_OFFSET_POLICY_H

// THE one answer to "what offset does this VF point want?".
//
// An offset is the only thing this program writes. The driver owns the stock
// base and moves it with load and temperature, so a point's absolute MHz is
// never a controllable quantity -- it is base + offset, and base is whatever
// the driver felt like reporting at the moment it was sampled.
//
// That rule was previously re-derived at four sites: the initial target build,
// the correction loop, the Linux target build, and verification. They drifted,
// and the drift was the 2026-09-17 under-load failures. The last one is the
// clearest: a profile-1 apply wrote point 74 correctly at offset 0, the TAIL
// then missed by one VF bin because the base had moved, that sent the apply
// into the correction loop -- and the correction loop, which still derived
// `absolute - live base` on its own, overwrote point 74 with +495000 kHz and
// failed the apply on a value it had just invented. Fixing the build site
// alone could never have caught that; only one shared answer can.
//
// So: any call site that needs a point's offset calls this. Verification asks
// the same question by comparing against the offset this produced.

struct CurvePointOffsetRequest {
    // Provenance: this point's intent IS an offset. Its absolute MHz is a
    // projection over a base sampled at some earlier moment (profile save, an
    // editor refresh, a previous apply) and must not be reconstructed from a
    // base sampled now -- that is what turns the asked-for offset into a
    // different one every time the driver re-reports the base.
    bool fromGpuOffset;
    // This request's own GPU-offset component for the point, already resolved
    // against the exclude-low count. Zero for a profile carrying no GPU offset,
    // which is exactly what a point saved with `pointN_offset_khz=0` asks for.
    int gpuOffsetComponentKHz;
    // The absolute target, honoured only for a point the user really typed.
    unsigned int absoluteMHz;
    // The stock base as read right now. Consulted ONLY for a genuine absolute,
    // because only there is the caller actually asking to hit a frequency.
    long long liveBaseKHz;
};

static inline long long curve_point_target_offset_khz(const CurvePointOffsetRequest* r) {
    if (!r) return 0;
    if (r->fromGpuOffset) return (long long)r->gpuOffsetComponentKHz;
    long long base = r->liveBaseKHz < 0 ? 0 : r->liveBaseKHz;
    return (long long)r->absoluteMHz * 1000 - base;
}

// Convenience for the common shape: caller has the live frequency and the
// offset currently programmed into the point, and wants the stock base implied
// by them.
static inline long long curve_point_stock_base_khz(long long liveFreqKHz,
                                                   long long programmedOffsetKHz) {
    long long base = liveFreqKHz - programmedOffsetKHz;
    return base < 0 ? 0 : base;
}

#endif
