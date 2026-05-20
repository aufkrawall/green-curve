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
