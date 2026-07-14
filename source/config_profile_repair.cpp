// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

static bool read_profile_point_int(const char* path, const char* section, int pointIndex, const char* suffix, int* valueOut) {
    if (valueOut) *valueOut = 0;
    if (!path || !section || !suffix || pointIndex < 0 || pointIndex >= VF_NUM_POINTS) return false;

    char key[64] = {};
    char buf[64] = {};
    StringCchPrintfA(key, ARRAY_COUNT(key), "point%d_%s", pointIndex, suffix);
    gc_GetPrivateProfileStringUtf8(section, key, "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (!buf[0]) return false;

    int value = 0;
    if (!parse_int_strict(buf, &value)) return false;
    if (valueOut) *valueOut = value;
    return true;
}

static bool profile_point_saved_visible(const char* path, const char* section, int pointIndex) {
    int visible = 0;
    if (read_profile_point_int(path, section, pointIndex, "visible", &visible)) {
        return visible != 0;
    }
    return is_curve_point_visible_in_gui(pointIndex);
}

static int profile_point_saved_offset_khz(const char* path, const char* section, int pointIndex, bool* foundOut) {
    int value = 0;
    bool found = read_profile_point_int(path, section, pointIndex, "offset_khz", &value);
    if (foundOut) *foundOut = found;
    return found ? value : 0;
}

static void clear_profile_curve_point(DesiredSettings* desired, int pointIndex) {
    if (!desired || pointIndex < 0 || pointIndex >= VF_NUM_POINTS) return;
    desired->hasCurvePoint[pointIndex] = false;
    desired->curvePointMHz[pointIndex] = 0;
}

static bool profile_tail_points_flat_to_lock(const char* path, const char* section, const DesiredSettings* desired, int* visibleTailCountOut) {
    if (visibleTailCountOut) *visibleTailCountOut = 0;
    if (!desired || !desired->hasLock || desired->lockCi < 0 || desired->lockCi >= VF_NUM_POINTS || desired->lockMHz == 0) return false;

    int visibleTailCount = 0;
    for (int ci = desired->lockCi; ci < VF_NUM_POINTS; ci++) {
        if (!desired->hasCurvePoint[ci]) continue;
        if (!profile_point_saved_visible(path, section, ci)) continue;
        visibleTailCount++;
        if (desired->curvePointMHz[ci] != desired->lockMHz) return false;
    }
    if (visibleTailCountOut) *visibleTailCountOut = visibleTailCount;
    return visibleTailCount > 0;
}

static void repair_profile_locked_curve_readback_artifacts(const char* path, const char* section, int slot, DesiredSettings* desired) {
    if (!path || !section || !desired) return;
    if (!desired->hasLock || desired->lockCi < 3 || desired->lockCi >= VF_NUM_POINTS || desired->lockMHz == 0) return;
    if (desired->hasGpuOffset && desired->gpuOffsetMHz != 0) return;
    if (desired->gpuOffsetExcludeLowCount > 0) return;

    int visibleTailCount = 0;
    if (!profile_tail_points_flat_to_lock(path, section, desired, &visibleTailCount)) return;
    if (visibleTailCount < 4) return;

    int firstIntentCi = desired->lockCi;
    int ciMinus1 = desired->lockCi - 1;
    int ciMinus2 = desired->lockCi - 2;
    int ciMinus3 = desired->lockCi - 3;

    bool hasTwoHighPreTailPoints =
        desired->hasCurvePoint[ciMinus1] &&
        desired->hasCurvePoint[ciMinus2] &&
        desired->curvePointMHz[ciMinus1] < desired->lockMHz &&
        desired->curvePointMHz[ciMinus2] < desired->curvePointMHz[ciMinus1] &&
        desired->lockMHz - desired->curvePointMHz[ciMinus1] <= 60 &&
        desired->curvePointMHz[ciMinus1] - desired->curvePointMHz[ciMinus2] <= 60;

    if (hasTwoHighPreTailPoints) {
        firstIntentCi = ciMinus2;
    }

    int removedStockScaffold = 0;
    for (int ci = 0; ci < firstIntentCi; ci++) {
        if (!desired->hasCurvePoint[ci]) continue;
        if (!profile_point_saved_visible(path, section, ci)) continue;
        bool haveOffset = false;
        int savedOffset = profile_point_saved_offset_khz(path, section, ci, &haveOffset);
        if (!haveOffset || savedOffset != 0) continue;
        clear_profile_curve_point(desired, ci);
        removedStockScaffold++;
    }

    bool removedReadbackArtifact = false;
    if (hasTwoHighPreTailPoints && desired->hasCurvePoint[ciMinus3]) {
        bool haveOffset = false;
        int savedOffset = profile_point_saved_offset_khz(path, section, ciMinus3, &haveOffset);
        unsigned int nextMHz = desired->curvePointMHz[ciMinus2];
        unsigned int candidateMHz = desired->curvePointMHz[ciMinus3];
        bool hasLargeGapToIntent = nextMHz > candidateMHz && (nextMHz - candidateMHz) >= 150;
        long long savedOffsetMagnitude = savedOffset < 0 ? -(long long)savedOffset : (long long)savedOffset;
        bool hasModerateReadbackOffset = haveOffset && savedOffsetMagnitude >= 60000 && savedOffsetMagnitude <= 250000;
        if (hasLargeGapToIntent && hasModerateReadbackOffset && profile_point_saved_visible(path, section, ciMinus3)) {
            debug_log("profile repair: removed non-tail readback artifact slot=%d section=%s ci=%d actual=%u offset=%d lockCi=%d lockMHz=%u\n",
                slot, section, ciMinus3, candidateMHz, savedOffset, desired->lockCi, desired->lockMHz);
            clear_profile_curve_point(desired, ciMinus3);
            removedReadbackArtifact = true;
        }
    }

    if (removedStockScaffold > 0 || removedReadbackArtifact) {
        debug_log("profile repair: sparse locked curve intent slot=%d section=%s removedStock=%d removedArtifact=%d firstIntentCi=%d lockCi=%d tailPoints=%d\n",
            slot, section, removedStockScaffold, removedReadbackArtifact ? 1 : 0,
            firstIntentCi, desired->lockCi, visibleTailCount);
    }
}
