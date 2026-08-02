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
    state->powerLimitReadbackValid = facts->powerRead &&
        facts->powerDefaultmW > 0 && facts->powerCurrentmW > 0;
    bool fansPresent = facts->fanSupported && facts->fanCount > 0;
    state->fanPolicyReadbackValid = fansPresent &&
        all_fans_known(facts->fanPolicyKnown, facts->fanCount);
    state->fanTargetReadbackValid = fansPresent &&
        all_fans_known(facts->fanTargetKnown, facts->fanCount);
}

#endif  // GREEN_CURVE_CONTROL_READBACK_POLICY_H
