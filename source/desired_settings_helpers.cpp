// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

static int desired_curve_point_count(const DesiredSettings* desired) {
    if (!desired) return 0;
    int count = 0;
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (desired->hasCurvePoint[ci]) count++;
    }
    return count;
}

static bool desired_updates_curve_or_gpu_offset_state(const DesiredSettings* desired) {
    if (!desired) return false;
    return desired->resetOcBeforeApply
        || desired->hasGpuOffset
        || desired->hasLock
        || desired_curve_point_count(desired) > 0;
}

static bool desired_has_nonfan_apply_fields(const DesiredSettings* desired) {
    if (!desired) return false;
    return desired_updates_curve_or_gpu_offset_state(desired)
        || desired->hasMemOffset
        || desired->hasPowerLimit;
}

static bool desired_is_fan_only_apply_request(const DesiredSettings* desired) {
    return desired
        && desired->hasFan
        && !desired_has_nonfan_apply_fields(desired);
}

static int desired_normalized_gpu_exclude_low_count(const DesiredSettings* desired) {
    if (!desired || !desired->hasGpuOffset || desired->gpuOffsetMHz == 0) return 0;
    if (desired->gpuOffsetExcludeLowCount < 0) return 0;
    if (desired->gpuOffsetExcludeLowCount > VF_NUM_POINTS) return VF_NUM_POINTS;
    return desired->gpuOffsetExcludeLowCount;
}

static bool desired_bool_equal(gc_bool8 lhs, gc_bool8 rhs) {
    return (lhs != 0) == (rhs != 0);
}

static bool desired_settings_match_active_service_intent(const DesiredSettings* profile, const DesiredSettings* active, char* detail, size_t detailSize) {
    if (!profile || !active) {
        set_message(detail, detailSize, "missing desired settings");
        return false;
    }

    if (!desired_bool_equal(profile->hasGpuOffset, active->hasGpuOffset)) {
        set_message(detail, detailSize, "gpu offset ownership differs profile=%d active=%d", profile->hasGpuOffset ? 1 : 0, active->hasGpuOffset ? 1 : 0);
        return false;
    }
    if (profile->hasGpuOffset) {
        int profileExclude = desired_normalized_gpu_exclude_low_count(profile);
        int activeExclude = desired_normalized_gpu_exclude_low_count(active);
        if (profile->gpuOffsetMHz != active->gpuOffsetMHz || profileExclude != activeExclude) {
            set_message(detail, detailSize, "gpu offset differs profile=%d/%d active=%d/%d",
                profile->gpuOffsetMHz, profileExclude, active->gpuOffsetMHz, activeExclude);
            return false;
        }
    }

    if (!desired_bool_equal(profile->hasMemOffset, active->hasMemOffset)) {
        set_message(detail, detailSize, "memory offset ownership differs profile=%d active=%d", profile->hasMemOffset ? 1 : 0, active->hasMemOffset ? 1 : 0);
        return false;
    }
    if (profile->hasMemOffset && profile->memOffsetMHz != active->memOffsetMHz) {
        set_message(detail, detailSize, "memory offset differs profile=%d active=%d", profile->memOffsetMHz, active->memOffsetMHz);
        return false;
    }

    if (!desired_bool_equal(profile->hasPowerLimit, active->hasPowerLimit)) {
        set_message(detail, detailSize, "power ownership differs profile=%d active=%d", profile->hasPowerLimit ? 1 : 0, active->hasPowerLimit ? 1 : 0);
        return false;
    }
    if (profile->hasPowerLimit && profile->powerLimitPct != active->powerLimitPct) {
        set_message(detail, detailSize, "power differs profile=%d active=%d", profile->powerLimitPct, active->powerLimitPct);
        return false;
    }

    if (!desired_bool_equal(profile->hasFan, active->hasFan)) {
        set_message(detail, detailSize, "fan ownership differs profile=%d active=%d", profile->hasFan ? 1 : 0, active->hasFan ? 1 : 0);
        return false;
    }
    if (profile->hasFan) {
        if (profile->fanMode != active->fanMode) {
            set_message(detail, detailSize, "fan intent differs profile=%d active=%d", profile->fanMode, active->fanMode);
            return false;
        }
        if (profile->fanMode == FAN_MODE_FIXED && clamp_percent(profile->fanPercent) != clamp_percent(active->fanPercent)) {
            set_message(detail, detailSize, "fixed fan differs profile=%d active=%d", clamp_percent(profile->fanPercent), clamp_percent(active->fanPercent));
            return false;
        }
        if (profile->fanMode == FAN_MODE_CURVE && !fan_curve_equals(&profile->fanCurve, &active->fanCurve)) {
            set_message(detail, detailSize, "fan curve differs");
            return false;
        }
    }

    if (!desired_bool_equal(profile->hasLock, active->hasLock)) {
        set_message(detail, detailSize, "lock ownership differs profile=%d active=%d", profile->hasLock ? 1 : 0, active->hasLock ? 1 : 0);
        return false;
    }
    if (profile->hasLock) {
        if (profile->lockCi != active->lockCi || profile->lockMHz != active->lockMHz
            || profile->lockMode != active->lockMode
            || !desired_bool_equal(profile->lockTracksAnchor, active->lockTracksAnchor)) {
            set_message(detail, detailSize, "lock differs profile=ci%d/%u/mode%d active=ci%d/%u/mode%d",
                profile->lockCi, profile->lockMHz, profile->lockMode,
                active->lockCi, active->lockMHz, active->lockMode);
            return false;
        }
    }

    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (!desired_bool_equal(profile->hasCurvePoint[ci], active->hasCurvePoint[ci])) {
            set_message(detail, detailSize, "curve ownership differs at ci%d profile=%d active=%d",
                ci, profile->hasCurvePoint[ci] ? 1 : 0, active->hasCurvePoint[ci] ? 1 : 0);
            return false;
        }
        if (profile->hasCurvePoint[ci] && profile->curvePointMHz[ci] != active->curvePointMHz[ci]) {
            set_message(detail, detailSize, "curve point ci%d differs profile=%u active=%u",
                ci, profile->curvePointMHz[ci], active->curvePointMHz[ci]);
            return false;
        }
    }

    set_message(detail, detailSize, "active service intent matches profile");
    return true;
}
