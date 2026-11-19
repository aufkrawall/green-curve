// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The editor draft a loaded profile declares (source/gui_draft_desired_policy.h),
// assertion codes 12300-12305. The reported case: Import current GPU settings
// saved another tool's flatten (925 mV, point 76, 2962 MHz) as a lock with no
// explicit anchor point; Load rebuilt the draft from the live curve (2482 MHz
// at point 76) and Apply sent 2482 MHz.

#include <memory>
#include <string.h>

#include "gui_draft_desired_policy.h"

namespace {

constexpr int kAnchor = 76;
constexpr unsigned int kLiveAnchorMHz = 2482;
constexpr unsigned int kLockMHz = 2962;

// The record the capture saved: flatten lock at the anchor, pre-anchor points
// as captured offsets (not explicit MHz), nothing explicit at or above it.
std::unique_ptr<DesiredSettings> captured_flatten() {
    auto d = std::make_unique<DesiredSettings>();  // value-initialized: no fields claimed
    d->hasLock = 1;
    d->lockCi = kAnchor;
    d->lockMHz = kLockMHz;
    d->lockMode = LOCK_MODE_FLATTEN;
    d->lockTracksAnchor = 0;
    for (int ci = 45; ci < kAnchor; ++ci) {
        d->curvePointCaptured[ci] = 1;
        d->curvePointCapturedOffsetHz[ci] = 0;
    }
    return d;
}

// Mirrors gui_draft_capture_desired(): the draft starts as the clean
// projection Load rebuilt (live values) and takes what the record declares.
struct Draft {
    unsigned int mhz[VF_NUM_POINTS]{};
    bool valid[VF_NUM_POINTS]{};
};
Draft load_into_live_draft(const DesiredSettings& d) {
    Draft draft;
    for (int ci = 40; ci < 127; ++ci) {
        draft.mhz[ci] = ci < kAnchor ? 180u + (unsigned int)ci : kLiveAnchorMHz;
        draft.valid[ci] = true;
    }
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        const unsigned int mhz = gui_draft_desired_point_mhz(d, ci);
        if (!mhz && !d.hasCurvePoint[ci]) continue;
        draft.mhz[ci] = mhz;
        draft.valid[ci] = mhz > 0;
    }
    return draft;
}

}  // namespace

int run_gui_draft_desired_tests() {
    auto flatten = captured_flatten();

    // A lock-only record declares its anchor at the lock target, and nothing
    // for captured or tail points (those stay what the editor projects).
    bool fromLock = false;
    if (gui_draft_desired_point_mhz(*flatten, kAnchor, &fromLock) != kLockMHz || !fromLock) return 12300;
    if (gui_draft_desired_point_mhz(*flatten, kAnchor + 1, &fromLock) != 0 || fromLock ||
        gui_draft_desired_point_mhz(*flatten, kAnchor - 1) != 0 ||
        gui_draft_desired_point_mhz(*flatten, 126) != 0)
        return 12300;

    // Load: the anchor of the rebuilt live draft becomes the slot's lock, the
    // value Apply sends; untouched points keep the projection.
    const Draft loaded = load_into_live_draft(*flatten);
    if (loaded.mhz[kAnchor] != kLockMHz || !loaded.valid[kAnchor]) return 12301;
    if (loaded.mhz[kAnchor + 1] != kLiveAnchorMHz || loaded.mhz[kAnchor - 1] != 180u + kAnchor - 1) return 12301;

    // An explicit anchor point that disagrees with the lock: the field shows
    // the lock MHz (populate_desired_into_gui), so the draft holds it too.
    auto both = captured_flatten();
    both->hasCurvePoint[kAnchor] = 1;
    both->curvePointMHz[kAnchor] = 2900;
    if (gui_draft_desired_point_mhz(*both, kAnchor, &fromLock) != kLockMHz || !fromLock) return 12302;

    // An ordinary editor-saved record: explicit points pass through, the
    // anchor point equals the lock.
    auto ordinary = captured_flatten();
    ordinary->hasCurvePoint[70] = 1;
    ordinary->curvePointMHz[70] = 2325;
    ordinary->hasCurvePoint[kAnchor] = 1;
    ordinary->curvePointMHz[kAnchor] = kLockMHz;
    if (gui_draft_desired_point_mhz(*ordinary, 70, &fromLock) != 2325 || fromLock ||
        gui_draft_desired_point_mhz(*ordinary, kAnchor) != kLockMHz)
        return 12303;

    // No usable lock: only explicit points are declared.
    auto unlocked = captured_flatten();
    unlocked->hasLock = 0;
    if (gui_draft_desired_point_mhz(*unlocked, kAnchor) != 0) return 12304;
    unlocked->hasLock = 1;
    unlocked->lockMHz = 0;
    unlocked->hasCurvePoint[kAnchor] = 1;
    unlocked->curvePointMHz[kAnchor] = 2400;
    if (gui_draft_desired_point_mhz(*unlocked, kAnchor, &fromLock) != 2400 || fromLock) return 12304;

    // Out-of-range points declare nothing, and a lock index outside the
    // table never lands on a point.
    if (gui_draft_desired_point_mhz(*flatten, -1) != 0 ||
        gui_draft_desired_point_mhz(*flatten, VF_NUM_POINTS) != 0)
        return 12305;
    auto stray = captured_flatten();
    stray->lockCi = VF_NUM_POINTS;
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci)
        if (gui_draft_desired_point_mhz(*stray, ci) != 0) return 12305;
    return 0;
}
