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

// The same ownership read, for the advanced ClkDomains fields (XBAR clock,
// XBAR MSVDD, SYS clock, VIDEO clock), in the opposite direction.
//
// A GUI Apply that goes through reset-before-apply claims every advanced
// domain the GPU exposes, at its current value -- stock (0) when the user
// never touched it.  A profile saved before a domain existed, or while the
// GUI had not yet learned the GPU exposes it, has no key for it and so claims
// nothing.  Strict equality then recorded every such profile as ad-hoc after
// a successful Apply ("xbar clock ownership differs profile=0 active=1"), and
// the tray showed "Manual settings" instead of the profile the user picked.
//
// Leaving a domain at stock is exactly what a profile that does not mention
// it asks for, so only that case matches: an active claim at a NON-zero
// offset is a real difference and still breaks the match.
static inline bool profile_ownership_advanced_mismatch_allowed(
    bool relaxedOwnershipRead, bool profileClaims, bool activeClaims,
    int activeValue) {
    return relaxedOwnershipRead && !profileClaims && activeClaims &&
        activeValue == 0;
}

#endif  // GREEN_CURVE_PROFILE_OWNERSHIP_POLICY_H
