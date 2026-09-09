// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Protocol-v14 hardware-readback provenance for the Windows control state.
//
// The published ControlState scalars deliberately fall back to a last-known or
// configured-intent value so a degraded read still leaves the editor populated.
// That fallback is exactly what makes the value alone untrustworthy: 0 MHz is a
// legal offset, 0% is a legal duty, and "the driver did not answer" then looks
// identical to "the hardware still holds what Green Curve wrote".  These flags
// carry the provenance alongside the value so a client can render the domain
// unavailable instead of reporting a false match.
//
// Kept pure and out of the ratcheted Windows shards so it is unit-testable on
// either host; the shards only record facts and call in here.

#ifndef GREEN_CURVE_CONTROL_READBACK_POLICY_H
#define GREEN_CURVE_CONTROL_READBACK_POLICY_H

#include "gpu_core.h"

// Per-fan provenance needs its own sub-struct so a refresh can invalidate the
// whole fan block in one memset without disturbing the scalar flags.
struct FanReadbackValidity {
    bool policy[MAX_GPU_FANS];
    bool target[MAX_GPU_FANS];
};

struct HardwareReadbackValidity {
    // gpuOffset/memOffset/powerLimit describe the matching cached scalar.
    // pstate records whether the NvAPI Pstates20 read answered, because
    // clock-offset detection folds that source into the published GPU offset.
    bool gpuOffset;
    bool memOffset;
    bool powerLimit;
    bool pstate;
    FanReadbackValidity fan;
};

// A rollback zeroes the cached scalars before re-reading them.  Those zeros are
// bookkeeping, not readings, so the validity must drop with them: a refresh
// that then fails must leave the domains unavailable rather than publishing an
// invented "reset to stock" match.
static inline void invalidate_scalar_readbacks(HardwareReadbackValidity* v) {
    if (!v) return;
    v->gpuOffset = false;
    v->memOffset = false;
    v->powerLimit = false;
    v->pstate = false;
}

// Final provenance of the GPU offset scalar after clock-offset detection.
// Detection unconditionally replaces that scalar -- including with the 0 that
// both "no offset applied" and "nothing could be read" produce -- so it, not
// the earlier NVML read, owns the answer.  Its two real sources are a populated
// VF control table and the Pstates20 read.
static inline bool gpu_offset_readback_after_detection(
    bool globalOffsetBackend, int numPopulated, bool pstateRead) {
    return (globalOffsetBackend && numPopulated > 0) || pstateRead;
}

// True once every fan present has answered the given getter.  A partial answer
// is not a readback: the comparison covers all fans, so one silent fan makes
// the domain unknown rather than selectively matching.
static inline bool all_fans_known(const bool* known, unsigned int fanCount) {
    if (!known || fanCount == 0) return false;
    for (unsigned int i = 0; i < fanCount && i < MAX_GPU_FANS; ++i)
        if (!known[i]) return false;
    return true;
}

// Whether Green Curve can read this board's power target AND express it in the
// percentage unit every one of its surfaces speaks -- the editor field, the
// profile key, the IPC request, the reset-to-stock baseline, the readback
// comparison.  The percentage is `current mW / default mW`, so a board that
// refuses either number has no power *control surface*, not merely a stale
// reading: there is nothing to compare against and nothing a write could mean.
//
// This is the single predicate for that question.  It used to be spelled out
// inline in three places and, in the two that mattered most, not at all: the
// reset-to-stock step read the fabricated `powerLimitPct == 0`, concluded the
// target was 100 percentage points off stock, and issued a write that
// `nvapi_set_power_limit()` could only refuse for want of a default limit --
// failing the entire Apply, VF curve included, on hardware whose curve was
// perfectly writable.  Reported 2026-09-07 against 0.25.0 on an RTX 3060 Laptop
// GPU, whose driver answers the power *constraints* but not the limit itself.
static inline bool power_limit_surface_available(bool powerReadbackValid,
                                                 int powerDefaultmW,
                                                 int powerCurrentmW) {
    return powerReadbackValid && powerDefaultmW > 0 && powerCurrentmW > 0;
}

// The published percentage for a pair of mW readings.  An unusable pair yields
// the board default rather than 0; see POWER_LIMIT_DEFAULT_PCT.
static inline int power_limit_pct_from_mw(int currentmW, int defaultmW) {
    if (defaultmW <= 0 || currentmW <= 0) return POWER_LIMIT_DEFAULT_PCT;
    int pct = (currentmW * 100 + defaultmW / 2) / defaultmW;
    return pct > 0 ? pct : POWER_LIMIT_DEFAULT_PCT;
}

// Whether the reset-to-stock-baseline step must write the power target.
//
// All three conditions are load-bearing.  A request that does not own power
// must not have its power reset (a clean baseline is not ownership of unrelated
// controls).  A board with no power control surface has nothing to reset --
// Green Curve cannot have moved a target it cannot write -- and issuing the
// write anyway is a guaranteed failure that aborts the Apply before the VF
// curve is ever touched.  And a target already at the board default is already
// stock.
static inline bool power_reset_before_apply_required(bool requestOwnsPower,
                                                     bool surfaceAvailable,
                                                     int currentPct) {
    return requestOwnsPower && surfaceAvailable &&
           currentPct != POWER_LIMIT_DEFAULT_PCT;
}

struct ControlReadbackFacts {
    // Whether the *published* GPU offset came from a driver reading rather than
    // remembered intent.  Two of the Windows detection branches deliberately
    // answer with configured intent, which the number alone cannot reveal.
    bool gpuOffsetFromHardware;
    bool memOffsetRead;
    bool powerRead;
    int powerDefaultmW;
    int powerCurrentmW;
    bool fanSupported;
    unsigned int fanCount;
    const bool* fanPolicyKnown;
    const bool* fanTargetKnown;
};

static inline void apply_control_readback_validity(
    ControlState* state, const ControlReadbackFacts* facts) {
    if (!state || !facts) return;
    state->gpuOffsetReadbackValid = facts->gpuOffsetFromHardware;
    state->memOffsetReadbackValid = facts->memOffsetRead;
    // A percentage computed from a missing or zero default is not a reading.
    state->powerLimitReadbackValid = power_limit_surface_available(
        facts->powerRead, facts->powerDefaultmW, facts->powerCurrentmW);
    bool fansPresent = facts->fanSupported && facts->fanCount > 0;
    state->fanPolicyReadbackValid = fansPresent &&
        all_fans_known(facts->fanPolicyKnown, facts->fanCount);
    state->fanTargetReadbackValid = fansPresent &&
        all_fans_known(facts->fanTargetKnown, facts->fanCount);
}

#endif  // GREEN_CURVE_CONTROL_READBACK_POLICY_H
