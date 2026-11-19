// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Legacy base_plus_gpu_offset locked-tail provenance and profile ownership
// matching across GUI Load->Apply, legacy/imported slots, and restart
// snapshots (cases 12400-12425).
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "gpu_core.h"
#include "fan_curve_api.h"
#include "desired_settings_ui_policy.h"
#include "applied_profile_indicator_policy.h"
#include "service_profile_identity_policy.h"

namespace {

static int clamp_percent(int value) {
    return value < 0 ? 0 : (value > 100 ? 100 : value);
}

static void set_message(char* dst, size_t dstSize, const char* fmt, ...) {
    if (!dst || dstSize == 0) return;
    if (!fmt) {
        dst[0] = '\0';
        return;
    }
    va_list args;
    va_start(args, fmt);
    vsnprintf(dst, dstSize, fmt, args);
    va_end(args);
    dst[dstSize - 1] = '\0';
}

#include "desired_settings_helpers.cpp"

DesiredSettings make_locked_slot_base() {
    DesiredSettings d = {};
    initialize_desired_settings_defaults(&d);
    d.hasGpuOffset = 1;
    d.gpuOffsetMHz = 475;
    d.gpuOffsetExcludeLowCount = 70;
    d.hasMemOffset = 1;
    d.memOffsetMHz = 3000;
    d.hasPowerLimit = 1;
    d.powerLimitPct = 85;
    d.hasFan = 1;
    d.fanAuto = 0;
    d.fanMode = FAN_MODE_CURVE;
    d.hasLock = 1;
    d.lockCi = 76;
    d.lockMHz = 2957;
    d.lockMode = LOCK_MODE_FLATTEN;
    d.lockTracksAnchor = 0;
    // Pre-tail points projected from the +475 MHz GPU offset.
    for (int ci = 70; ci < 76; ++ci) {
        d.hasCurvePoint[ci] = 1;
        d.curvePointMHz[ci] = (unsigned int)(2797 + (ci - 70) * 24);
        d.curvePointFromGpuOffset[ci] = 1;
    }
    return d;
}

int provenance_helper_cases() {
    DesiredSettings d = make_locked_slot_base();
    // 12400: pre-tail points stay projected; lock anchor and tail at lockMHz
    // are absolute lock-tail targets, never GPU-offset projections.
    if (!desired_base_plus_offset_point_is_projected(&d, 70, 2797u) ||
        !desired_base_plus_offset_point_is_projected(&d, 75, 2917u)) return 12400;
    if (desired_base_plus_offset_point_is_projected(&d, 76, 2957u) ||
        desired_base_plus_offset_point_is_projected(&d, 100, 2957u)) return 12401;
    // 12402: a divergent tail frequency is not silently promoted to absolute.
    if (!desired_base_plus_offset_point_is_projected(&d, 80, 3100u)) return 12402;
    // 12403: an interactive lock with lockMHz == 0 keeps anchor provenance.
    DesiredSettings noTarget = d;
    noTarget.lockMHz = 0;
    if (!desired_base_plus_offset_point_is_projected(&noTarget, 76, 2957u)) return 12403;
    // 12404: an unlocked curve keeps every reconstructed point projected.
    DesiredSettings unlocked = d;
    unlocked.hasLock = 0;
    unlocked.lockCi = -1;
    unlocked.lockMHz = 0;
    if (!desired_base_plus_offset_point_is_projected(&unlocked, 76, 2957u)) return 12404;

    // 12405: clearing stale tail projection flags (e.g. after inferring a lock
    // from a legacy curve) touches only ci >= lockCi matching lockMHz.
    DesiredSettings inferred = make_locked_slot_base();
    inferred.hasCurvePoint[76] = 1; inferred.curvePointMHz[76] = 2957; inferred.curvePointFromGpuOffset[76] = 1;
    inferred.hasCurvePoint[77] = 1; inferred.curvePointMHz[77] = 2957; inferred.curvePointFromGpuOffset[77] = 1;
    inferred.hasCurvePoint[78] = 1; inferred.curvePointMHz[78] = 3100; inferred.curvePointFromGpuOffset[78] = 1;
    if (desired_clear_explicit_lock_tail_projection_flags(&inferred) != 2) return 12405;
    if (!inferred.curvePointFromGpuOffset[75]) return 12406;
    if (inferred.curvePointFromGpuOffset[76] || inferred.curvePointFromGpuOffset[77]) return 12407;
    if (!inferred.curvePointFromGpuOffset[78]) return 12408;

    // 12409: every curve point kind has a stable non-unknown label.
    if (strcmp(desired_curve_point_kind_name(DESIRED_CURVE_ABSENT), "absent") != 0 ||
        strcmp(desired_curve_point_kind_name(DESIRED_CURVE_ABSOLUTE), "absolute") != 0 ||
        strcmp(desired_curve_point_kind_name(DESIRED_CURVE_PROJECTED), "projected") != 0 ||
        strcmp(desired_curve_point_kind_name(DESIRED_CURVE_CAPTURED), "captured") != 0 ||
        strcmp(desired_curve_point_kind_name(DESIRED_CURVE_INVALID), "invalid") != 0)
        return 12409;
    return 0;
}

int ownership_policy_cases() {
    const long long lockHz = 2957000000LL;
    // 12410: strict pre-write claims never relax lock-tail differences.
    if (profile_ownership_lock_tail_mismatch_allowed(
            false, true, 76, lockHz, 2957u, 76,
            DESIRED_CURVE_PROJECTED, lockHz, 2957u,
            DESIRED_CURVE_ABSOLUTE, lockHz, 2957u)) return 12410;
    // 12411: relaxed ownership accepts absent, projected (0 or lockMHz), and
    // absolute (exact lockHz) on ci >= lockCi.
    if (!profile_ownership_lock_tail_mismatch_allowed(
            true, true, 76, lockHz, 2957u, 76,
            DESIRED_CURVE_PROJECTED, lockHz, 2957u,
            DESIRED_CURVE_ABSOLUTE, lockHz, 2957u)) return 12411;
    if (!profile_ownership_lock_tail_mismatch_allowed(
            true, true, 76, lockHz, 2957u, 77,
            DESIRED_CURVE_ABSENT, 0, 0u,
            DESIRED_CURVE_ABSOLUTE, lockHz, 2957u)) return 12412;
    if (!profile_ownership_lock_tail_mismatch_allowed(
            true, true, 76, lockHz, 2957u, 76,
            DESIRED_CURVE_PROJECTED, 0, 0u,
            DESIRED_CURVE_ABSOLUTE, lockHz, 2957u)) return 12413;
    // 12414: pre-tail points are never relaxed.
    if (profile_ownership_lock_tail_mismatch_allowed(
            true, true, 76, lockHz, 2957u, 75,
            DESIRED_CURVE_PROJECTED, lockHz, 2957u,
            DESIRED_CURVE_ABSOLUTE, lockHz, 2957u)) return 12414;
    // 12415: divergent tail frequency, captured points, and sub-MHz vs whole-MHz
    // mismatches are refused even on a relaxed ownership read.
    if (profile_ownership_lock_tail_mismatch_allowed(
            true, true, 76, lockHz, 2957u, 80,
            DESIRED_CURVE_ABSOLUTE, 3100000000LL, 3100u,
            DESIRED_CURVE_ABSOLUTE, lockHz, 2957u)) return 12415;
    if (profile_ownership_lock_tail_mismatch_allowed(
            true, true, 76, lockHz, 2957u, 80,
            DESIRED_CURVE_CAPTURED, 0, 0u,
            DESIRED_CURVE_ABSOLUTE, lockHz, 2957u)) return 12416;
    if (profile_ownership_lock_tail_mismatch_allowed(
            true, true, 76, 2957500000LL, 2957u, 76,
            DESIRED_CURVE_ABSENT, 0, 0u,
            DESIRED_CURVE_ABSOLUTE, 2957000000LL, 2957u)) return 12417;
    if (profile_ownership_lock_tail_mismatch_allowed(
            true, false, 76, lockHz, 2957u, 76,
            DESIRED_CURVE_ABSENT, 0, 0u,
            DESIRED_CURVE_ABSOLUTE, lockHz, 2957u)) return 12418;
    return 0;
}

int production_matcher_cases() {
    char detail[256] = {};

    // 12419: Profile 3 scenario — decoded with absolute lock-tail points,
    // matched against GUI Load->Apply both strictly (pre-write) and relaxed
    // (delta post-write without fan).
    DesiredSettings profile = make_locked_slot_base();
    for (int ci = 76; ci <= 126; ++ci) {
        profile.hasCurvePoint[ci] = 1;
        profile.curvePointMHz[ci] = 2957;
        profile.curvePointFromGpuOffset[ci] = 0;
    }
    DesiredSettings guiApply = profile;
    if (!desired_settings_match_active_service_intent(&profile, &guiApply, detail, sizeof(detail), false))
        return 12419;
    guiApply.hasFan = 0;
    if (desired_settings_match_active_service_intent(&profile, &guiApply, detail, sizeof(detail), false))
        return 12420;
    if (!desired_settings_match_active_service_intent(&profile, &guiApply, detail, sizeof(detail), true))
        return 12421;

    // 12422: A legacy or imported slot omitting tail points (or a pre-fix
    // restart snapshot carrying projected tail flags) still fails strict
    // pre-write replacement, but succeeds on the relaxed ownership check.
    DesiredSettings noTailProfile = make_locked_slot_base();
    if (desired_settings_match_active_service_intent(&noTailProfile, &profile, detail, sizeof(detail), false))
        return 12422;
    if (!strstr(detail, "ci76") || !strstr(detail, "profile=absent/0") || !strstr(detail, "active=absolute/2957"))
        return 12423;
    if (!desired_settings_match_active_service_intent(&noTailProfile, &profile, detail, sizeof(detail), true))
        return 12424;

    DesiredSettings preFixActive = profile;
    for (int ci = 76; ci <= 126; ++ci) preFixActive.curvePointFromGpuOffset[ci] = 1;
    if (!desired_settings_match_active_service_intent(&profile, &preFixActive, detail, sizeof(detail), true))
        return 12425;

    // 12426: Editing a pre-tail point still fails relaxed ownership and reports
    // both sides clearly in the diagnostic detail.
    DesiredSettings editedPreTail = profile;
    editedPreTail.curvePointFromGpuOffset[75] = 0;
    editedPreTail.curvePointMHz[75] = 2930;
    if (desired_settings_match_active_service_intent(&profile, &editedPreTail, detail, sizeof(detail), true))
        return 12426;
    if (!strstr(detail, "ci75") || !strstr(detail, "profile=projected/") || !strstr(detail, "active=absolute/2930"))
        return 12427;

    return 0;
}

int ad_hoc_unique_slot_cases() {
    AppliedProfileIndicatorReason reason = APPLIED_PROFILE_REASON_NO_AUTHORITY;
    AppliedProfileIndicatorInputs in = {};
    in.serviceAuthoritative = true;
    in.activeDesiredValid = true;
    in.profileSource = SERVICE_PROFILE_SOURCE_AD_HOC;
    in.maxSlots = 5;

    // 12433: ad-hoc active intent uniquely matching slot 3 ticks slot 3.
    in.matchedUserSlot = 3;
    in.matchedUserSlotCount = 1;
    if (applied_profile_indicator_slot(in, &reason) != 3 ||
        reason != APPLIED_PROFILE_REASON_MATCHED_AD_HOC_SLOT) return 12433;

    // 12434: duplicate personal slots stay unticked and report ambiguity.
    in.matchedUserSlot = 0;
    in.matchedUserSlotCount = 2;
    if (applied_profile_indicator_slot(in, &reason) != 0 ||
        reason != APPLIED_PROFILE_REASON_AMBIGUOUS_AD_HOC_SLOT) return 12434;

    // 12435: out-of-range unique slot stays unticked.
    in.matchedUserSlot = 6;
    in.matchedUserSlotCount = 1;
    if (applied_profile_indicator_slot(in, &reason) != 0 ||
        reason != APPLIED_PROFILE_REASON_AMBIGUOUS_AD_HOC_SLOT) return 12435;

    // 12436: without an active intent or without authority, never tick.
    in.matchedUserSlot = 3;
    in.matchedUserSlotCount = 1;
    in.activeDesiredValid = false;
    if (applied_profile_indicator_slot(in, &reason) != 0 ||
        reason != APPLIED_PROFILE_REASON_NOT_A_USER_SLOT) return 12436;
    in.activeDesiredValid = true;
    in.serviceAuthoritative = false;
    if (applied_profile_indicator_slot(in, &reason) != 0 ||
        reason != APPLIED_PROFILE_REASON_NO_AUTHORITY) return 12437;

    // 12438: shared, machine, and none sources never consult personal-slot scan.
    in.serviceAuthoritative = true;
    in.profileSource = SERVICE_PROFILE_SOURCE_SHARED_SLOT;
    if (applied_profile_indicator_slot(in, &reason) != 0 ||
        reason != APPLIED_PROFILE_REASON_NOT_A_USER_SLOT) return 12438;
    in.profileSource = SERVICE_PROFILE_SOURCE_MACHINE_SLOT;
    if (applied_profile_indicator_slot(in, &reason) != 0 ||
        reason != APPLIED_PROFILE_REASON_NOT_A_USER_SLOT) return 12439;
    in.profileSource = SERVICE_PROFILE_SOURCE_NONE;
    if (applied_profile_indicator_slot(in, &reason) != 0 ||
        reason != APPLIED_PROFILE_REASON_NOT_A_USER_SLOT) return 12440;

    // 12441: new reason names are distinct and non-unknown.
    const char* matchedName = applied_profile_indicator_reason_name(
        APPLIED_PROFILE_REASON_MATCHED_AD_HOC_SLOT);
    const char* ambiguousName = applied_profile_indicator_reason_name(
        APPLIED_PROFILE_REASON_AMBIGUOUS_AD_HOC_SLOT);
    if (!matchedName || !ambiguousName ||
        strcmp(matchedName, "unknown") == 0 ||
        strcmp(ambiguousName, "unknown") == 0 ||
        strcmp(matchedName, ambiguousName) == 0) return 12441;

    // 12442-12447: service-side unique personal-slot fallback.
    if (service_profile_identity_unique_user_slot_match(
            SERVICE_PROFILE_SOURCE_AD_HOC, 3, 1, 5) != 3) return 12442;
    if (service_profile_identity_unique_user_slot_match(
            SERVICE_PROFILE_SOURCE_USER_SLOT, 3, 1, 5) != 3) return 12443;
    if (service_profile_identity_unique_user_slot_match(
            SERVICE_PROFILE_SOURCE_AD_HOC, 3, 2, 5) != 0) return 12444;
    if (service_profile_identity_unique_user_slot_match(
            SERVICE_PROFILE_SOURCE_AD_HOC, 3, 0, 5) != 0) return 12445;
    if (service_profile_identity_unique_user_slot_match(
            SERVICE_PROFILE_SOURCE_SHARED_SLOT, 3, 1, 5) != 0) return 12446;
    if (service_profile_identity_unique_user_slot_match(
            SERVICE_PROFILE_SOURCE_MACHINE_SLOT, 3, 1, 5) != 0) return 12447;
    return 0;
}

}  // namespace

int run_profile_lock_tail_ownership_tests() {
    if (int failure = provenance_helper_cases()) return failure;
    if (int failure = ownership_policy_cases()) return failure;
    if (int failure = production_matcher_cases()) return failure;
    return ad_hoc_unique_slot_cases();
}
