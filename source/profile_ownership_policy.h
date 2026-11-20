// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Ownership comparison relaxation for the fan domain.
//
// A GUI Apply is a delta: when the editor shows an unchanged Auto fan,
// capture_gui_apply_settings() leaves hasFan=false so the apply does not claim
// or rewrite fan ownership (another tool may own it).  Saved profiles always
// carry fan fields, so the strict record equality used for pre-write profile
// claims would clear the tray tick after every ordinary OC apply.  The
// post-write/ownership check therefore allows a profile whose fan is AUTO to
// match an active intent that leaves the fan unclaimed: both leave the fan to
// the driver.
//
// It never allows a Fixed or Curve profile to match an unclaimed fan.  That
// combination means nothing drives the profile's fan setting, so the profile is
// not what is in force; accepting it once kept "Profile N" ticked while the
// fan sat in driver control after an Apply had released the curve.

#ifndef GREEN_CURVE_PROFILE_OWNERSHIP_POLICY_H
#define GREEN_CURVE_PROFILE_OWNERSHIP_POLICY_H

#include "desired_curve_intent.h"

// `profileFanMode` is the profile's effective fan mode
// (fan_request_effective_mode()); it only matters when the profile claims fan.
static inline bool profile_ownership_fan_mismatch_allowed(
    bool allowUnclaimedFan, bool profileHasFan, int profileFanMode,
    bool activeHasFan) {
    return allowUnclaimedFan && profileHasFan && !activeHasFan &&
        profileFanMode == FAN_MODE_AUTO;
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

// One point inside an explicit lock's tail (`ci >= lockCi`, `lockMHz > 0`)
// is consistent with that lock when it either leaves the tail to the lock
// (absent), holds the lock's exact frequency (absolute), or carries a legacy
// or imported projection at zero or the lock's whole MHz.  A captured point
// or a divergent frequency is never equivalent to the lock.
static inline bool profile_ownership_point_consistent_with_lock(
    DesiredCurvePointKind kind, long long pointHz, unsigned int storedMHz,
    long long lockHz, unsigned int lockMHz) {
    if (kind == DESIRED_CURVE_ABSENT) return true;
    if (kind == DESIRED_CURVE_ABSOLUTE) return pointHz == lockHz;
    if (kind == DESIRED_CURVE_PROJECTED) return storedMHz == 0 || storedMHz == lockMHz;
    return false;
}

// The third ownership-read relaxation, for locked-tail curve points.
//
// Once both records agree on an explicit lock (`hasLock`, same `lockCi`,
// `lockMHz`, `lockMode`, `lockTracksAnchor` and authoring), every backend
// applies the lock target to the whole tail `ci >= lockCi`.  Different paths
// still represent those tail points differently:
//   - a GUI Apply expands every visible tail point as absolute `lockMHz`;
//   - a CLI, imported, or hand-edited profile may omit tail points (or cover
//     a different visible-point count);
//   - a pre-fix `base_plus_gpu_offset` active snapshot (across a restart or
//     upgrade) or an imported legacy slot may mark tail points projected.
//
// On a relaxed ownership read (post-write confirmation and GUI tray indicator),
// two lock-tail points that are both consistent with the agreed lock describe
// the same running tail.  Strict pre-write profile claims still require exact
// semantic equality, and any pre-tail point (`ci < lockCi`) or divergent tail
// frequency still breaks the match.
static inline bool profile_ownership_lock_tail_mismatch_allowed(
    bool relaxedOwnershipRead, bool lockMatchesExplicit, int lockCi,
    long long lockHz, unsigned int lockMHz, int ci,
    DesiredCurvePointKind profileKind, long long profilePointHz,
    unsigned int profileStoredMHz,
    DesiredCurvePointKind activeKind, long long activePointHz,
    unsigned int activeStoredMHz) {
    if (!relaxedOwnershipRead || !lockMatchesExplicit) return false;
    if (lockCi < 0 || lockCi >= VF_NUM_POINTS || lockMHz == 0 || lockHz <= 0) return false;
    if (ci < lockCi || ci >= VF_NUM_POINTS) return false;
    return profile_ownership_point_consistent_with_lock(
               profileKind, profilePointHz, profileStoredMHz, lockHz, lockMHz) &&
           profile_ownership_point_consistent_with_lock(
               activeKind, activePointHz, activeStoredMHz, lockHz, lockMHz);
}

// True when `scope` replaces the VF point set rather than merging only the
// explicitly listed points onto prior curve state.
static inline bool profile_ownership_scope_replaces_curve_points(gc_u32 scope) {
    const gc_u32 kind = desired_application_scope_kind(scope);
    return kind == DESIRED_SCOPE_LEGACY ||
           kind == DESIRED_SCOPE_REPLACE_PROFILE ||
           (scope & DESIRED_SCOPE_REPLACE_VF_POINTS) != 0;
}

// The fourth ownership-read relaxation, for GPU-offset projected points versus
// absent points.
//
// A projected point (`DESIRED_CURVE_PROJECTED`) carries no independent clock
// authority of its own: its target is purely the record's agreed GPU offset
// component for `ci`.  Once both records already agree on `hasGpuOffset`,
// `gpuOffsetMHz`, `gpuOffsetExcludeLowCount`, and graphics authoring (and both
// scopes replace the VF point set rather than performing a partial merge that
// leaves omitted points untouched), spelling a point as `PROJECTED` vs leaving
// it `ABSENT` describes the exact same running curve intent at `ci` (for
// example an imported indexed profile carrying zero-MHz projected points vs an
// upgrade export written by an older build that dropped zero-MHz entries, or an
// equivalent CLI apply).
//
// Strict pre-write profile claims still require exact semantic equality, and
// any `ABSOLUTE` or `CAPTURED` point on either side still breaks the match.
static inline bool profile_ownership_projected_point_mismatch_allowed(
    bool relaxedOwnershipRead,
    bool bothHaveGpuOffset,
    bool replacesCurvePoints,
    DesiredCurvePointKind profileKind,
    DesiredCurvePointKind activeKind) {
    if (!relaxedOwnershipRead || !bothHaveGpuOffset || !replacesCurvePoints)
        return false;
    return (profileKind == DESIRED_CURVE_PROJECTED && activeKind == DESIRED_CURVE_ABSENT) ||
           (profileKind == DESIRED_CURVE_ABSENT && activeKind == DESIRED_CURVE_PROJECTED);
}

#endif  // GREEN_CURVE_PROFILE_OWNERSHIP_POLICY_H
