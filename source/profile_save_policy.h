// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Profile Save's capture source.  The old rule saved live hardware defaults
// whenever the user had not typed anything, which was correct for a stock GPU
// but silently dropped an applied pinned/curved profile from the slot and
// cleared the tray tick.  Applied curve/lock intent is real intent, not drift,
// so it must use the GUI capture too.

#ifndef GREEN_CURVE_PROFILE_SAVE_POLICY_H
#define GREEN_CURVE_PROFILE_SAVE_POLICY_H

static inline bool profile_save_uses_gui_capture(
    bool userModified, bool pendingCurveOrLock, bool appliedCurveOrLock) {
    return userModified || pendingCurveOrLock || appliedCurveOrLock;
}

#endif  // GREEN_CURVE_PROFILE_SAVE_POLICY_H
