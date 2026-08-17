// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Pure policy for replaying settings captured by an in-app update.
//
// The capture is deliberately written before the install command because the
// GUI must hand its live service state to the future binary.  That means the
// file can outlive a refused or failed install.  It must not turn into an
// unsolicited GPU apply at some later startup: replay is permitted only when
// the running build is exactly the release the user consented to install and
// the capture is still fresh.

#ifndef GREEN_CURVE_UPDATE_RESTORE_POLICY_H
#define GREEN_CURVE_UPDATE_RESTORE_POLICY_H

#include "update_version_policy.h"

#define GC_UPDATE_RESTORE_SECTION "green_curve_update_restore"
#define GC_UPDATE_RESTORE_VERSION_KEY "expected_version"
#define GC_UPDATE_RESTORE_MAX_AGE_SECONDS (24LL * 60LL * 60LL)

// ## The legacy capture, and why it must be accepted
//
// The version binding did not exist in 0.23 -- this header did not exist in
// 0.23.  A 0.23 GUI therefore writes a capture with no `expected_version` key
// at all, and the build it hands that capture to is by definition a NEWER one
// that does have the binding.  Reading a missing key as an invalid version made
// the very first update every shipped copy performs discard the user's settings
// silently.  Observed live 2026-08-17 on the 0.23 -> 0.23.1 hop:
//
//     update handoff: discarding the pending restore
//     (captured for version '<none>', running 0.23.1, age -1s)
//
// It cannot be fixed on the writing side, because the writing side is a binary
// that is already installed.  So the reading side accepts a capture with **no**
// version key as what it is -- one written before the binding existed -- and
// falls back to the only other evidence it has, freshness.
//
// A *present but unparseable* version is still refused.  That is corruption or
// tampering, not history, and the two must not collapse into one case.
//
// The legacy window is an hour rather than a day.  With no version to compare,
// freshness is the entire gate, and the capture is written seconds before the
// installer launches: an hour covers a slow install with room to spare while
// keeping a capture from a *failed* update far away from tomorrow's startup.
// The 24-hour window stays for version-bound captures, where the running build
// already proves the install it was captured for actually completed.
#define GC_UPDATE_RESTORE_LEGACY_MAX_AGE_SECONDS (60LL * 60LL)

enum GcUpdateRestoreDecision {
    GC_UPDATE_RESTORE_APPLY = 0,
    GC_UPDATE_RESTORE_DISCARD = 1,
};

// A capture written by a build that predates the version binding.  Absent, not
// empty-because-something-failed: the writer either writes a valid version or
// deletes the capture, so there is no path that leaves the key blank on purpose.
static inline bool gc_update_restore_is_legacy_capture(const char* expectedVersionText) {
    return !expectedVersionText || !expectedVersionText[0];
}

static inline GcUpdateRestoreDecision gc_update_restore_decide(
    const char* expectedVersionText, const char* runningVersionText,
    long long ageSeconds) {
    GcUpdateVersion running;
    gc_update_version_parse(runningVersionText, &running);
    if (!running.valid) return GC_UPDATE_RESTORE_DISCARD;
    // A negative age means the age could not be measured at all, which is a
    // failure rather than a young file, and is refused in both shapes.
    if (ageSeconds < 0) return GC_UPDATE_RESTORE_DISCARD;

    if (gc_update_restore_is_legacy_capture(expectedVersionText)) {
        return ageSeconds <= GC_UPDATE_RESTORE_LEGACY_MAX_AGE_SECONDS
                   ? GC_UPDATE_RESTORE_APPLY
                   : GC_UPDATE_RESTORE_DISCARD;
    }

    GcUpdateVersion expected;
    gc_update_version_parse(expectedVersionText, &expected);
    if (!expected.valid) return GC_UPDATE_RESTORE_DISCARD;
    if (gc_update_version_compare(&expected, &running) != 0)
        return GC_UPDATE_RESTORE_DISCARD;
    if (ageSeconds > GC_UPDATE_RESTORE_MAX_AGE_SECONDS)
        return GC_UPDATE_RESTORE_DISCARD;
    return GC_UPDATE_RESTORE_APPLY;
}

#endif  // GREEN_CURVE_UPDATE_RESTORE_POLICY_H
