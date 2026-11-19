// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Single-owner GPU retargeting rule for the Windows service.
//
// The service holds ONE active intent (g_serviceActiveDesired), bound to the
// GPU it was written to (g_serviceActiveDesiredGpu), and ONE runtime target
// (g_app.selectedGpu, its NVML device and the fan runtime). Retargeting the
// runtime to another GPU while the intent is held would:
//   - leave the owning GPU's clocks and manual fan duty in hardware with
//     nothing tracking, maintaining or restoring them;
//   - merge the owner's remaining fields into the other GPU's new intent,
//     so a later restore writes them to the wrong card;
//   - drive the other GPU's fan from the owner's fan curve until replaced.
// So while an intent is held, a retarget may only go to its owner. Resetting
// the owner releases the intent; after that any validated GPU may be
// selected. When the owner has provably left the adapter list, a Reset of
// another GPU may release it too (its hardware is unreachable and nothing is
// orphaned); an Apply may not, because it would inherit the owner's fields.
// Requests that keep the current runtime target are not retargets and are
// never refused here. The Linux daemon enforces the equivalent rule in
// linux_gpu_switch_preserves_active_intent() (linux_gpu_selection.h).

#pragma once

#include "gpu_adapter_identity.h"

// Fixed underlying type: every unsigned value is a valid object of the type,
// so the name/allows fallbacks for an unknown value are defined behaviour.
enum GpuIntentOwnerVerdict : unsigned int {
    // No active intent: any validated target may be selected.
    GPU_INTENT_OWNER_ALLOW_NO_INTENT = 0,
    // The target is provably the GPU that owns the active intent.
    GPU_INTENT_OWNER_ALLOW_OWNER = 1,
    // Not a retarget, and the current target is the owner (or the owner
    // predates identity capture).
    GPU_INTENT_OWNER_ALLOW_CURRENT_TARGET = 2,
    // Not a retarget, but the current target is NOT the recorded owner. Not
    // refused (no retarget happens here), reported for diagnosis.
    GPU_INTENT_OWNER_ALLOW_CURRENT_NOT_OWNER = 3,
    // A Reset of another GPU releases an owner that left the adapter list.
    GPU_INTENT_OWNER_ALLOW_RELEASE_ABSENT_OWNER = 4,
    // Refused: another GPU that may be present owns the active intent.
    GPU_INTENT_OWNER_REFUSE_OTHER_GPU = 5,
    // Refused: the owner left the adapter list; only a Reset releases it.
    GPU_INTENT_OWNER_REFUSE_ABSENT_OWNER = 6,
    // Refused: the owner identity was never recorded, so no retarget can be
    // proven safe. A Reset of the current target releases the intent.
    GPU_INTENT_OWNER_REFUSE_UNKNOWN_OWNER = 7,
};

static inline bool gpu_intent_owner_verdict_allows(GpuIntentOwnerVerdict v) {
    return v == GPU_INTENT_OWNER_ALLOW_NO_INTENT ||
        v == GPU_INTENT_OWNER_ALLOW_OWNER ||
        v == GPU_INTENT_OWNER_ALLOW_CURRENT_TARGET ||
        v == GPU_INTENT_OWNER_ALLOW_CURRENT_NOT_OWNER ||
        v == GPU_INTENT_OWNER_ALLOW_RELEASE_ABSENT_OWNER;
}

static inline const char* gpu_intent_owner_verdict_name(
    GpuIntentOwnerVerdict v) {
    switch (v) {
    case GPU_INTENT_OWNER_ALLOW_NO_INTENT: return "no-intent";
    case GPU_INTENT_OWNER_ALLOW_OWNER: return "owner";
    case GPU_INTENT_OWNER_ALLOW_CURRENT_TARGET: return "current-target";
    case GPU_INTENT_OWNER_ALLOW_CURRENT_NOT_OWNER: return "current-not-owner";
    case GPU_INTENT_OWNER_ALLOW_RELEASE_ABSENT_OWNER:
        return "release-absent-owner";
    case GPU_INTENT_OWNER_REFUSE_OTHER_GPU: return "refuse-other-gpu";
    case GPU_INTENT_OWNER_REFUSE_ABSENT_OWNER: return "refuse-absent-owner";
    case GPU_INTENT_OWNER_REFUSE_UNKNOWN_OWNER: return "refuse-unknown-owner";
    }
    return "invalid";
}

// Whether `candidate` could be `owner`: provably the same write target, or
// not distinguishable from it because one side lacks PCI identity on a
// multi-adapter system. Used to decide that an owner has provably LEFT the
// adapter list; an undecidable comparison keeps it present (fail closed).
static inline bool gpu_intent_owner_may_be(const GpuAdapterInfo* owner,
    const GpuAdapterInfo* candidate, unsigned int adapterCount) {
    if (!owner || !candidate || !owner->valid || !candidate->valid)
        return false;
    if (gpu_adapter_is_same_write_target(owner, candidate, adapterCount))
        return true;
    return !(owner->pciInfoValid && candidate->pciInfoValid);
}

static inline bool gpu_intent_owner_present(const GpuAdapterInfo* owner,
    const GpuAdapterInfo* adapters, unsigned int adapterCount) {
    if (!adapters) return true;  // no evidence of absence
    for (unsigned int i = 0; i < adapterCount && i < MAX_GPU_ADAPTERS; ++i) {
        if (gpu_intent_owner_may_be(owner, &adapters[i], adapterCount))
            return true;
    }
    return false;
}

// `owner` is the identity recorded with the active intent (invalid for an
// intent that predates owner capture). `target` is the GPU the request would
// write. `adapters`/`adapterCount` is the live adapter list. `retargets` is
// true when serving the request moves the runtime away from its current GPU.
// `releasesIntent` is true for an explicit Reset, which drops the intent.
static inline GpuIntentOwnerVerdict gpu_intent_owner_verdict(
    bool hasActiveIntent, const GpuAdapterInfo* owner,
    const GpuAdapterInfo* target, const GpuAdapterInfo* adapters,
    unsigned int adapterCount, bool retargets, bool releasesIntent) {
    if (!hasActiveIntent) return GPU_INTENT_OWNER_ALLOW_NO_INTENT;
    const bool ownerKnown = owner && owner->valid;
    if (!retargets) {
        return !ownerKnown ||
                gpu_adapter_is_same_write_target(owner, target, adapterCount)
            ? GPU_INTENT_OWNER_ALLOW_CURRENT_TARGET
            : GPU_INTENT_OWNER_ALLOW_CURRENT_NOT_OWNER;
    }
    if (!ownerKnown) return GPU_INTENT_OWNER_REFUSE_UNKNOWN_OWNER;
    if (gpu_adapter_is_same_write_target(owner, target, adapterCount))
        return GPU_INTENT_OWNER_ALLOW_OWNER;
    if (gpu_intent_owner_present(owner, adapters, adapterCount))
        return GPU_INTENT_OWNER_REFUSE_OTHER_GPU;
    return releasesIntent ? GPU_INTENT_OWNER_ALLOW_RELEASE_ABSENT_OWNER
                          : GPU_INTENT_OWNER_REFUSE_ABSENT_OWNER;
}
