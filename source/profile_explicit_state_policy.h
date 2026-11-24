// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_EXPLICIT_STATE_POLICY_H
#define GREEN_CURVE_PROFILE_EXPLICIT_STATE_POLICY_H

#include "gpu_core.h"
#include "platform.h"
#include "service_scoped_apply_policy.h"

// Validates that a loaded profile record carries enough explicit state for
// unattended application (Windows logon, app-launch, CLI logon, and settings
// transfer).
//
// Legacy records (DESIRED_SCOPE_LEGACY) always serialized every domain, so a
// missing scalar or curve indicates a truncated/corrupt INI section and stays
// rejected fail-closed.
//
// Modern explicit-scope records (DESIRED_SCOPE_REPLACE_PROFILE /
// DESIRED_SCOPE_UPDATE_INCLUDED) intentionally omit domains they do not own
// (for example graphics_offset or fan omitted by Save Current capture, or VF
// points omitted by a pin-only/power-only profile). For those records, a valid
// non-release scope, valid authoring metadata, and at least one explicitly
// owned control domain constitute a complete profile.
static inline bool profile_desired_has_explicit_state(
    const DesiredSettings* desired,
    bool requireCurveForLegacy,
    char* err,
    size_t errSize) {
    if (err && errSize > 0) err[0] = '\0';
    if (!desired) {
        if (err && errSize > 0) gc_snprintf(err, errSize, "No desired settings");
        return false;
    }
    if (!desired_application_scope_valid(desired->applicationScope)) {
        if (err && errSize > 0) {
            gc_snprintf(err, errSize, "Profile has an unsupported application scope");
        }
        return false;
    }
    if (desired->applicationScope != DESIRED_SCOPE_LEGACY) {
        if (desired_application_scope_kind(desired->applicationScope) ==
            DESIRED_SCOPE_EXPLICIT_RELEASE) {
            if (err && errSize > 0) {
                gc_snprintf(err, errSize, "Profile contains no configured GPU settings");
            }
            return false;
        }
        if (!desired_authoring_runtime_ready(desired)) {
            if (err && errSize > 0) {
                gc_snprintf(err, errSize, "Profile has invalid authoring metadata");
            }
            return false;
        }
        if (!service_scoped_has_owned_intent(*desired)) {
            if (err && errSize > 0) {
                gc_snprintf(err, errSize, "Profile contains no configured GPU settings");
            }
            return false;
        }
        return true;
    }
    if (!desired->hasGpuOffset) {
        if (err && errSize > 0) gc_snprintf(err, errSize, "Profile is missing gpu_offset_mhz");
        return false;
    }
    if (!desired->hasMemOffset) {
        if (err && errSize > 0) gc_snprintf(err, errSize, "Profile is missing mem_offset_mhz");
        return false;
    }
    if (!desired->hasPowerLimit) {
        if (err && errSize > 0) gc_snprintf(err, errSize, "Profile is missing power_limit_pct");
        return false;
    }
    if (!desired->hasFan) {
        if (err && errSize > 0) gc_snprintf(err, errSize, "Profile is missing fan_mode/fan settings");
        return false;
    }
    if (requireCurveForLegacy) {
        bool haveCurvePoint = false;
        for (int i = 0; i < VF_NUM_POINTS; ++i) {
            if (desired_curve_point_present(desired, i)) {
                haveCurvePoint = true;
                break;
            }
        }
        if (!haveCurvePoint) {
            if (err && errSize > 0) {
                gc_snprintf(err, errSize, "Profile is missing VF curve points");
            }
            return false;
        }
    }
    return true;
}

#endif
