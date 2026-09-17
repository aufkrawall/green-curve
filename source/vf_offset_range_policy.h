// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// THE VF offset range.  One struct, one source, consumed by everything that
// needs to know how far a curve point may be pushed: the planner that builds
// the FLATTEN tail floor, the batch pre-check that refuses an out-of-range
// request, the low-level write clamp, and verification.
//
// THE BUG THIS EXISTS FOR (source-confirmed 2026-09-16, fixed here):
//
//   There were three answers to "what is the legal offset range", and they
//   disagreed.
//     1. get_curve_offset_range_khz() (main_runtime_nvml.cpp) returned
//        known=false for hardware whose range could not be probed -- while
//        still filling in +/-1,000,000 kHz as the value to use.
//     2. The FLATTEN tail floor took the `else` branch on !known and used a
//        hardcoded -1,000,000 kHz.
//     3. The apply's own pre-write check used a *different* hardcoded fallback
//        of 500,000 kHz and refused anything beyond it.
//
//   So on any board whose offset range could not be probed, a FLATTEN request
//   generated a -1,000,000 kHz tail floor and then refused its own floor at
//   500,000 kHz.  That refusal skipped the branch owning the curve domain's
//   only failCount++, so the apply returned failCount == 0 -- SUCCESS -- and
//   released the transition clamp over a curve that was never written.  The
//   user got "applied" for a profile that did not exist on the GPU.
//
//   The fix is structural, not a bigger number: the floor and the limit are
//   now read off the SAME struct, so `floor == range.minKHz` and
//   `hardLimit >= |range.minKHz|` hold by construction and a policy-generated
//   floor can no longer be refused by the policy that generated it.
//
// WHY THE FLATTEN FLOOR IS A BLANKET MINIMUM AND NOT A PER-POINT TARGET.
// NVIDIA enforces a monotonic non-decreasing VF curve, which is the entire
// mechanism behind "flatten the curve" undervolting: pushing every tail point
// far down makes the driver collapse them onto the frequency of the anchor
// point, which IS the requested plateau.  Computing an exact per-point delta
// instead would look tidier and would change behaviour that only the hardware
// can validate, so the floor stays a floor.
#ifndef GREEN_CURVE_VF_OFFSET_RANGE_POLICY_H
#define GREEN_CURVE_VF_OFFSET_RANGE_POLICY_H
#include <limits.h>

// What the driver accepts for a VF curve point offset, and whether that came
// from the driver or from the fallback.
//
// `known` is deliberately kept next to the values rather than collapsed into
// them: a fallback range is a legal range to WRITE against but not evidence of
// a driver capability, and callers that report to the user must be able to
// tell the two apart.  Conflating them is what let `rangeKnown=true` be
// inferred from a populated min/max.
struct VfOffsetRange {
    int minKHz;
    int maxKHz;
    bool known;
};

// The fallback when the driver will not say.  1000 MHz is the GPU offset range
// NVIDIA's own control panel exposes and the value the driver has been observed
// to accept for VF offsets; it is also what get_curve_offset_range_khz() has
// always returned, so adopting it here changes nothing on hardware that was
// already working.  It replaces the apply's separate 500,000 kHz cutoff, which
// existed only to bound corrupted profiles -- a job the IPC boundary already
// does with an absolute bound, and which that cutoff was doing by silently
// breaking FLATTEN on unprobeable boards.
enum { VF_OFFSET_RANGE_FALLBACK_LIMIT_KHZ = 1000000 };

static inline VfOffsetRange vf_offset_range_fallback() {
    VfOffsetRange r = {};
    r.minKHz = -VF_OFFSET_RANGE_FALLBACK_LIMIT_KHZ;
    r.maxKHz = VF_OFFSET_RANGE_FALLBACK_LIMIT_KHZ;
    r.known = false;
    return r;
}

// Build a range from a probe result.  An inverted or empty probe is not a
// range; it falls back rather than producing a window nothing can satisfy.
static inline VfOffsetRange vf_offset_range_from_probe(bool probed, int minKHz,
                                                       int maxKHz) {
    if (!probed || minKHz > maxKHz) return vf_offset_range_fallback();
    VfOffsetRange r = {};
    r.minKHz = minKHz;
    r.maxKHz = maxKHz;
    r.known = true;
    return r;
}

// The magnitude beyond which a requested offset is refused before any write.
// Derived from the same struct the floor comes from, which is the whole point:
// the two can no longer disagree.
static inline int vf_offset_range_hard_limit_khz(const VfOffsetRange& r) {
    long long lo = r.minKHz < 0 ? -(long long)r.minKHz : r.minKHz;
    long long hi = r.maxKHz < 0 ? -(long long)r.maxKHz : r.maxKHz;
    long long limit = lo > hi ? lo : hi;
    if (limit <= 0) limit = VF_OFFSET_RANGE_FALLBACK_LIMIT_KHZ;
    return limit > INT_MAX ? INT_MAX : (int)limit;
}

// The FLATTEN tail floor.  Always the range's own minimum, so
// vf_offset_range_permits_khz(r, vf_offset_range_flatten_floor_khz(r)) is true
// for every range this header can produce.
static inline bool vf_offset_range_supports_flatten(const VfOffsetRange& r) {
    return r.minKHz < 0;
}

static inline int vf_offset_range_flatten_floor_khz(const VfOffsetRange& r) {
    return r.minKHz;
}

// Whether the exact signed offset is inside the driver range.
static inline bool vf_offset_range_permits_khz(const VfOffsetRange& r,
                                               int offsetKHz) {
    return offsetKHz >= r.minKHz && offsetKHz <= r.maxKHz;
}

static inline bool vf_offset_zero_readback_is_benign(int target, int actual,
    unsigned int liveKHz, unsigned int operatingMinKHz) {
    return target > 0 && actual == 0 && liveKHz > 0 && liveKHz < operatingMinKHz;
}

// Clamp to the exact endpoints, reporting whether the value was altered.
//
// The reporting half is not decoration.  The old clamp_freq_delta_khz()
// returned only the clamped number, so a request the driver could not honour
// became a different request that verification then compared against the
// ORIGINAL target -- the caller had no way to know its intent had been edited
// on the way to the hardware.
static inline int vf_offset_range_clamp_khz(const VfOffsetRange& r,
                                            int offsetKHz, bool* alteredOut) {
    int v = offsetKHz;
    if (v > r.maxKHz) v = r.maxKHz;
    if (v < r.minKHz) v = r.minKHz;
    if (alteredOut) *alteredOut = (v != offsetKHz);
    return v;
}

#endif  // GREEN_CURVE_VF_OFFSET_RANGE_POLICY_H
