// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#pragma once

#include "gpu_core.h"

static inline bool profile_should_strip_legacy_unlocked_curve(
    bool lockCiWasExplicit, int lockCi, bool explicitVfPointsFormat) {
    return lockCiWasExplicit && lockCi < 0 && !explicitVfPointsFormat;
}

// Profiles written before lock_mode existed represented every lock as a
// flattened tail. Keep that meaning when the key is absent. A persisted lock
// with NONE is also contradictory and came from the Linux omission fixed in
// 2026; recover it as FLATTEN rather than silently resetting locked clocks.
static inline LockMode profile_lock_mode_after_load(
    bool hasLock, bool hasExplicitMode, int storedMode) {
    if (!hasLock) return LOCK_MODE_NONE;
    if (!hasExplicitMode) return LOCK_MODE_FLATTEN;
    if (storedMode <= LOCK_MODE_NONE || storedMode > LOCK_MODE_HARD)
        return LOCK_MODE_FLATTEN;
    return (LockMode)storedMode;
}

static inline int profile_slot_reference_after_clear(
    int referenceSlot, int clearedSlot, int fallbackSlot) {
    return referenceSlot == clearedSlot ? fallbackSlot : referenceSlot;
}
