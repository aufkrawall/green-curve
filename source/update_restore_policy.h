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

enum GcUpdateRestoreDecision {
    GC_UPDATE_RESTORE_APPLY = 0,
    GC_UPDATE_RESTORE_DISCARD = 1,
};

static inline GcUpdateRestoreDecision gc_update_restore_decide(
    const char* expectedVersionText, const char* runningVersionText,
    long long ageSeconds) {
    GcUpdateVersion expected;
    GcUpdateVersion running;
    gc_update_version_parse(expectedVersionText, &expected);
    gc_update_version_parse(runningVersionText, &running);
    if (!expected.valid || !running.valid) return GC_UPDATE_RESTORE_DISCARD;
    if (gc_update_version_compare(&expected, &running) != 0)
        return GC_UPDATE_RESTORE_DISCARD;
    if (ageSeconds < 0 || ageSeconds > GC_UPDATE_RESTORE_MAX_AGE_SECONDS)
        return GC_UPDATE_RESTORE_DISCARD;
    return GC_UPDATE_RESTORE_APPLY;
}

#endif  // GREEN_CURVE_UPDATE_RESTORE_POLICY_H
