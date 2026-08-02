// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Ownership comparison relaxation for the fan domain.
//
// A GUI Apply is a delta: when the fan was not changed, capture_gui_apply_settings()
// intentionally leaves hasFan=false so the apply does not claim or rewrite fan
// ownership (another tool may own it).  Saved profiles always carry fan fields,
// so the strict record equality used for pre-write profile claims would clear
// the tray tick after every ordinary OC apply.  The post-write/ownership check
// therefore allows the profile to claim fan while the active intent does not.

#ifndef GREEN_CURVE_PROFILE_OWNERSHIP_POLICY_H
#define GREEN_CURVE_PROFILE_OWNERSHIP_POLICY_H

static inline bool profile_ownership_fan_mismatch_allowed(
    bool allowUnclaimedFan, bool profileHasFan, bool activeHasFan) {
    return allowUnclaimedFan && profileHasFan && !activeHasFan;
}

#endif  // GREEN_CURVE_PROFILE_OWNERSHIP_POLICY_H
