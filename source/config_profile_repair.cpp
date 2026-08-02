// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// ---------------------------------------------------------------------------
// Legacy profile-format compatibility
//
// Older Windows builds stored the VF curve as BASE MHz plus separate
// gpu_offset metadata rather than absolute MHz.  Decoding that back lives
// here, next to the locked-curve readback repair, because both are the same
// job: turning what an older build happened to write into the record the
// current model expects.  Moved out of config_profiles.cpp, which is at its
// size ratchet; the amalgamation includes this shard first, so both remain
// visible to every caller.
// ---------------------------------------------------------------------------

static bool curve_section_uses_base_plus_gpu_offset_semantics(const char* path, const char* section, const DesiredSettings* desired) {
    if (!path || !section || !desired) return false;

    char semanticsBuf[64] = {};
    gc_GetPrivateProfileStringUtf8(section, "curve_semantics", "", semanticsBuf, sizeof(semanticsBuf), path);
    trim_ascii(semanticsBuf);
    if (_stricmp(semanticsBuf, "base_plus_gpu_offset") == 0) {
        return desired->hasGpuOffset && desired->gpuOffsetMHz != 0;
    }
    if (semanticsBuf[0]) {
        return false;
    }

    // Compatibility heuristic for Windows builds that saved base MHz plus
    // gpu_offset metadata but did not emit curve_semantics yet.
    if (!desired->hasGpuOffset || desired->gpuOffsetMHz == 0) return false;
    if (!desired->hasLock || desired->lockCi < 0 || desired->lockCi >= VF_NUM_POINTS || desired->lockMHz == 0) return false;
    if (!desired->hasCurvePoint[desired->lockCi]) return false;

    int offsetAtLockMHz = gpu_offset_component_mhz_for_point(desired->lockCi, desired->gpuOffsetMHz, desired->gpuOffsetExcludeLowCount);
    if (offsetAtLockMHz <= 0) return false;

    unsigned int storedLockPointMHz = desired->curvePointMHz[desired->lockCi];
    unsigned int absoluteLockPointMHz = storedLockPointMHz + (unsigned int)offsetAtLockMHz;

    bool directTailMatches = storedLockPointMHz == desired->lockMHz;
    bool offsetTailMatches = absoluteLockPointMHz == desired->lockMHz;
    return !directTailMatches && offsetTailMatches;
}
static void restore_curve_points_from_base_plus_gpu_offset(DesiredSettings* desired) {
    if (!desired || !desired->hasGpuOffset || desired->gpuOffsetMHz == 0) return;

    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!desired->hasCurvePoint[i]) continue;
        int offsetCompMHz = gpu_offset_component_mhz_for_point(i, desired->gpuOffsetMHz, desired->gpuOffsetExcludeLowCount);
        int absoluteMHz = (int)desired->curvePointMHz[i] + offsetCompMHz;
        if (absoluteMHz <= 0) {
            desired->hasCurvePoint[i] = false;
            desired->curvePointMHz[i] = 0;
            continue;
        }
        desired->curvePointMHz[i] = (unsigned int)absoluteMHz;
    }
}

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

// save_profile_to_config() always emits `pointN_visible` next to
// `pointN_offset_khz`, so the fallback below only fires for a profile written
// by a build that predates the key.  An ownership read must not take it: this
// repair CLEARS curve points, and the mask it produces is exactly what the
// applied-profile comparison tests, so a live-derived answer would make the
// same file decode differently as the GPU boosts.  Absent the key, the record
// asserts visibility by containing the point at all.
static bool profile_point_saved_visible(const char* path, const char* section, int pointIndex,
                                        ProfileReadMode mode) {
    int visible = 0;
    if (read_profile_point_int(path, section, pointIndex, "visible", &visible)) {
        return visible != 0;
    }
    if (mode == PROFILE_READ_FOR_OWNERSHIP) return true;
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

static bool profile_tail_points_flat_to_lock(const char* path, const char* section, const DesiredSettings* desired, int* visibleTailCountOut,
                                             ProfileReadMode mode) {
    if (visibleTailCountOut) *visibleTailCountOut = 0;
    if (!desired || !desired->hasLock || desired->lockCi < 0 || desired->lockCi >= VF_NUM_POINTS || desired->lockMHz == 0) return false;

    int visibleTailCount = 0;
    for (int ci = desired->lockCi; ci < VF_NUM_POINTS; ci++) {
        if (!desired->hasCurvePoint[ci]) continue;
        if (!profile_point_saved_visible(path, section, ci, mode)) continue;
        visibleTailCount++;
        if (desired->curvePointMHz[ci] != desired->lockMHz) return false;
    }
    if (visibleTailCountOut) *visibleTailCountOut = visibleTailCount;
    return visibleTailCount > 0;
}

static void repair_profile_locked_curve_readback_artifacts(const char* path, const char* section, int slot, DesiredSettings* desired,
                                                           ProfileReadMode mode) {
    if (!path || !section || !desired) return;
    if (!desired->hasLock || desired->lockCi < 3 || desired->lockCi >= VF_NUM_POINTS || desired->lockMHz == 0) return;
    if (desired->hasGpuOffset && desired->gpuOffsetMHz != 0) return;
    if (desired->gpuOffsetExcludeLowCount > 0) return;

    int visibleTailCount = 0;
    if (!profile_tail_points_flat_to_lock(path, section, desired, &visibleTailCount, mode)) return;
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
        if (!profile_point_saved_visible(path, section, ci, mode)) continue;
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
        if (hasLargeGapToIntent && hasModerateReadbackOffset && profile_point_saved_visible(path, section, ciMinus3, mode)) {
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
