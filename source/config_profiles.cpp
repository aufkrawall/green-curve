// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// ============================================================================
// Profile Slot I/O
// ============================================================================

#define CFG_BUFFER_SIZE 131072

struct ConfigLockGuard {
    ConfigLockGuard() { EnterCriticalSection(&g_configLock); }
    ~ConfigLockGuard() { LeaveCriticalSection(&g_configLock); }
};

static void infer_profile_lock_from_curve(const DesiredSettings* desired, int* lockCiOut, unsigned int* lockMHzOut);
static void resolve_profile_gpu_offset_state_for_save(const DesiredSettings* desired, int* gpuOffsetMHzOut, int* excludeLowCountOut);

static bool curve_section_uses_base_plus_gpu_offset_semantics(const char* path, const char* section, const DesiredSettings* desired) {
    if (!path || !section || !desired) return false;

    char semanticsBuf[64] = {};
    GetPrivateProfileStringA(section, "curve_semantics", "", semanticsBuf, sizeof(semanticsBuf), path);
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

static bool load_profile_from_config(const char* path, int slot, DesiredSettings* desired, char* err, size_t errSize) {
    if (!path || !desired || slot < 1 || slot > CONFIG_NUM_SLOTS) {
        set_message(err, errSize, "Invalid profile load arguments");
        return false;
    }
    initialize_desired_settings_defaults(desired);

    ConfigLockGuard lock;

    // Use slot-specific sections if they exist, else legacy sections for slot 1
    char controlsSection[32];
    char curveSection[32];
    char fanCurveSection[32];
    StringCchPrintfA(controlsSection, ARRAY_COUNT(controlsSection), "profile%d", slot);
    StringCchPrintfA(curveSection, ARRAY_COUNT(curveSection), "profile%d_curve", slot);
    StringCchPrintfA(fanCurveSection, ARRAY_COUNT(fanCurveSection), "profile%d_fan_curve", slot);

    bool hasSlotSections = config_section_has_keys(path, controlsSection) || config_section_has_keys(path, curveSection) || config_section_has_keys(path, fanCurveSection);
    if (!hasSlotSections && slot == 1) {
        if (config_section_has_keys(path, "controls") || config_section_has_keys(path, "curve") || config_section_has_keys(path, "fan_curve")) {
            StringCchCopyA(controlsSection, ARRAY_COUNT(controlsSection), "controls");
            StringCchCopyA(curveSection, ARRAY_COUNT(curveSection), "curve");
            StringCchCopyA(fanCurveSection, ARRAY_COUNT(fanCurveSection), "fan_curve");
        } else {
            set_message(err, errSize, "Profile %d is empty", slot);
            return false;
        }
    } else if (!hasSlotSections) {
        set_message(err, errSize, "Profile %d is empty", slot);
        return false;
    }

    if (!config_section_has_keys(path, controlsSection) && !config_section_has_keys(path, curveSection) && !config_section_has_keys(path, fanCurveSection)) {
        set_message(err, errSize, "Profile %d is empty", slot);
        return false;
    }

    char fanBuf[64] = {};
    char buf[64] = {};
    bool hasExplicitFanMode = false;

    GetPrivateProfileStringA(controlsSection, "gpu_offset_mhz", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        int v = 0;
        if (!parse_int_strict(buf, &v)) {
            set_message(err, errSize, "Invalid gpu_offset_mhz in profile %d", slot);
            return false;
        }
        desired->hasGpuOffset = true;
        desired->gpuOffsetMHz = v;
    }

    GetPrivateProfileStringA(controlsSection, "gpu_offset_exclude_low_count", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        int value = 0;
        if (!parse_int_strict(buf, &value)) {
            set_message(err, errSize, "Invalid gpu_offset_exclude_low_count in profile %d", slot);
            return false;
        }
        desired->gpuOffsetExcludeLowCount = value;
    } else {
        GetPrivateProfileStringA(controlsSection, "gpu_offset_exclude_low_70", "", buf, sizeof(buf), path);
        trim_ascii(buf);
        if (buf[0]) {
            int value = 0;
            if (!parse_int_strict(buf, &value)) {
                set_message(err, errSize, "Invalid gpu_offset_exclude_low_70 in profile %d", slot);
                return false;
            }
            desired->gpuOffsetExcludeLowCount = value != 0 ? 70 : 0;
        }
    }

    GetPrivateProfileStringA(controlsSection, "lock_ci", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        int value = -1;
        if (!parse_int_strict(buf, &value)) {
            set_message(err, errSize, "Invalid lock_ci in profile %d", slot);
            return false;
        }
        desired->hasLock = value >= 0;
        desired->lockCi = value;
    }

    GetPrivateProfileStringA(controlsSection, "lock_mhz", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        int value = 0;
        if (!parse_int_strict(buf, &value) || value < 0) {
            set_message(err, errSize, "Invalid lock_mhz in profile %d", slot);
            return false;
        }
        if (value > 0) {
            desired->hasLock = true;
            desired->lockMHz = (unsigned int)value;
        }
    }

    GetPrivateProfileStringA(controlsSection, "lock_tracks_anchor", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        int value = 0;
        if (!parse_int_strict(buf, &value)) {
            set_message(err, errSize, "Invalid lock_tracks_anchor in profile %d", slot);
            return false;
        }
        desired->lockTracksAnchor = value != 0;
    }

    GetPrivateProfileStringA(controlsSection, "mem_offset_mhz", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        int v = 0;
        if (!parse_int_strict(buf, &v)) {
            set_message(err, errSize, "Invalid mem_offset_mhz in profile %d", slot);
            return false;
        }
        desired->hasMemOffset = true;
        desired->memOffsetMHz = v;
    }

    GetPrivateProfileStringA(controlsSection, "power_limit_pct", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        int v = 0;
        if (!parse_int_strict(buf, &v)) {
            set_message(err, errSize, "Invalid power_limit_pct in profile %d", slot);
            return false;
        }
        if (v < 50 || v > 150) {
            set_message(err, errSize, "power_limit_pct %d is outside the safe range 50..150 in profile %d", v, slot);
            return false;
        }
        desired->hasPowerLimit = true;
        desired->powerLimitPct = v;
    }

    GetPrivateProfileStringA(controlsSection, "fan_mode", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        int fanMode = FAN_MODE_AUTO;
        if (!parse_fan_mode_config_value(buf, &fanMode)) {
            set_message(err, errSize, "Invalid fan_mode in profile %d", slot);
            return false;
        }
        desired->hasFan = true;
        desired->fanMode = fanMode;
        desired->fanAuto = fanMode == FAN_MODE_AUTO;
        hasExplicitFanMode = true;
    }

    GetPrivateProfileStringA(controlsSection, "fan", "", fanBuf, sizeof(fanBuf), path);
    trim_ascii(fanBuf);
    if (fanBuf[0]) {
        bool fanAuto = false;
        int fanPercent = 0;
        if (!parse_fan_value(fanBuf, &fanAuto, &fanPercent)) {
            set_message(err, errSize, "Invalid fan setting in profile %d", slot);
            return false;
        }
        if (!hasExplicitFanMode) {
            set_desired_fan_from_legacy_value(desired, fanAuto, fanPercent);
        } else if (desired->fanMode == FAN_MODE_FIXED && !fanAuto) {
            desired->hasFan = true;
            desired->fanAuto = false;
            desired->fanPercent = clamp_percent(fanPercent);
        }
    }

    GetPrivateProfileStringA(controlsSection, "fan_fixed_pct", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        int value = 0;
        if (!parse_int_strict(buf, &value)) {
            set_message(err, errSize, "Invalid fan_fixed_pct in profile %d", slot);
            return false;
        }
        if (!hasExplicitFanMode || desired->fanMode == FAN_MODE_FIXED) {
            desired->hasFan = true;
            desired->fanMode = FAN_MODE_FIXED;
            desired->fanAuto = false;
            desired->fanPercent = clamp_percent(value);
        }
    }

    if (!load_fan_curve_config_from_section(path, fanCurveSection, &desired->fanCurve, err, errSize)) return false;

    if (!load_curve_points_explicit_from_section(path, curveSection, desired, err, errSize)) {
        set_message(err, errSize, "Profile %d is missing explicit [%s] point*_mhz entries", slot, curveSection);
        return false;
    }

    if (curve_section_uses_base_plus_gpu_offset_semantics(path, curveSection, desired)) {
        restore_curve_points_from_base_plus_gpu_offset(desired);
    }

    for (int i = 1; i < VF_NUM_POINTS; i++) {
        if (desired->hasCurvePoint[i] && desired->hasCurvePoint[i - 1]) {
            if (desired->curvePointMHz[i] < desired->curvePointMHz[i - 1]) {
                set_message(err, errSize, "Profile %d has a non-monotonic curve point at index %d (%u MHz < %u MHz)", slot, i, desired->curvePointMHz[i], desired->curvePointMHz[i - 1]);
                return false;
            }
        }
    }

    if (!desired->hasLock || desired->lockCi < 0 || desired->lockMHz == 0) {
        int inferredLockCi = -1;
        unsigned int inferredLockMHz = 0;
        infer_profile_lock_from_curve(desired, &inferredLockCi, &inferredLockMHz);
        if (inferredLockCi >= 0 && inferredLockMHz > 0) {
            desired->hasLock = true;
            desired->lockCi = inferredLockCi;
            desired->lockMHz = inferredLockMHz;
            desired->lockTracksAnchor = false;
        }
    }

    if (desired->hasLock && desired->lockCi >= 0 && desired->lockMHz > 0) {
        bool sawVisibleTailPoint = false;
        bool tailMatchesLock = true;
        for (int ci = desired->lockCi; ci < VF_NUM_POINTS; ci++) {
            if (!desired->hasCurvePoint[ci]) continue;
            if (!is_curve_point_visible_in_gui(ci)) continue;
            sawVisibleTailPoint = true;
            if (desired->curvePointMHz[ci] != desired->lockMHz) {
                tailMatchesLock = false;
                break;
            }
        }
        if (sawVisibleTailPoint && tailMatchesLock) {
            desired->lockTracksAnchor = false;
        }
    }

    bool haveLiveCurveVisibility = g_app.loaded && g_app.numPopulated > 0;
    if (haveLiveCurveVisibility) {
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            if (!desired->hasCurvePoint[i]) continue;
            unsigned int volt_mv = g_app.curve[i].volt_uV / 1000;
            if (volt_mv == 0 || volt_mv < (unsigned)MIN_VISIBLE_VOLT_mV) {
                desired->hasCurvePoint[i] = false;
                desired->curvePointMHz[i] = 0;
            }
        }
    }

    if (!desired->hasFan) {
        desired->fanAuto = true;
        desired->fanMode = FAN_MODE_AUTO;
    }

    return true;
}

static unsigned int saved_curve_point_mhz(const DesiredSettings* desired, int pointIndex, int gpuOffsetMHz, int excludeLowCount) {
    if (pointIndex < 0 || pointIndex >= VF_NUM_POINTS) return 0;

    unsigned int mhz = 0;
    if (desired && desired->hasCurvePoint[pointIndex]) {
        mhz = desired->curvePointMHz[pointIndex];
    } else if (g_app.curve[pointIndex].freq_kHz > 0) {
        mhz = displayed_curve_mhz(g_app.curve[pointIndex].freq_kHz);
    }
    if (mhz == 0) return 0;
    if (!is_curve_point_visible_in_gui(pointIndex)) return mhz;

    int offsetCompMHz = gpu_offset_component_mhz_for_point(pointIndex, gpuOffsetMHz, excludeLowCount);
    int baseMHz = (int)mhz - offsetCompMHz;
    if (baseMHz <= 0) return mhz;
    return (unsigned int)baseMHz;
}

static bool can_save_curve_as_base_plus_gpu_offset(const DesiredSettings* desired, int gpuOffsetMHz, int excludeLowCount) {
    if (!desired || gpuOffsetMHz == 0) return false;
    if (!g_app.loaded || g_app.numPopulated <= 0) return false;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!desired->hasCurvePoint[i]) continue;
        if (!is_curve_point_visible_in_gui(i)) continue;
        int offsetCompmhz = gpu_offset_component_mhz_for_point(i, gpuOffsetMHz, excludeLowCount);
        int baseMhz = (int)desired->curvePointMHz[i] - offsetCompmhz;
        if (baseMhz <= 0) return false;
    }
    return true;
}

static bool is_profile_slot_saved(const char* path, int slot) {
    if (!path || slot < 1 || slot > CONFIG_NUM_SLOTS) return false;
    char section[32];
    char curveSection[32];
    char fanCurveSection[32];
    StringCchPrintfA(section, ARRAY_COUNT(section), "profile%d", slot);
    StringCchPrintfA(curveSection, ARRAY_COUNT(curveSection), "profile%d_curve", slot);
    StringCchPrintfA(fanCurveSection, ARRAY_COUNT(fanCurveSection), "profile%d_fan_curve", slot);
    if (config_section_has_keys(path, section) || config_section_has_keys(path, curveSection) || config_section_has_keys(path, fanCurveSection)) return true;
    // Fallback: check legacy sections for slot 1
    if (slot == 1) {
        if (config_section_has_keys(path, "controls") || config_section_has_keys(path, "curve") || config_section_has_keys(path, "fan_curve")) return true;
    }
    return false;
}

static bool save_profile_to_config(const char* path, int slot, const DesiredSettings* desired, char* err, size_t errSize) {
    if (!path || !desired || slot < 1 || slot > CONFIG_NUM_SLOTS) {
        set_message(err, errSize, "Invalid profile save arguments");
        return false;
    }

    if (!refresh_service_snapshot_and_active_desired(err, errSize)) {
        return false;
    }

    int desiredCurveCount = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (desired->hasCurvePoint[i]) desiredCurveCount++;
    }
    debug_log("save_profile_to_config: slot=%d visible=%d populated=%d desiredCurveCount=%d point126=%d/%u point127=%d/%u service=%d dirty=%d\n",
        slot,
        g_app.numVisible,
        g_app.numPopulated,
        desiredCurveCount,
        desired->hasCurvePoint[126] ? 1 : 0,
        desired->curvePointMHz[126],
        desired->hasCurvePoint[127] ? 1 : 0,
        desired->curvePointMHz[127],
        g_app.usingBackgroundService ? 1 : 0,
        gui_state_dirty() ? 1 : 0);

    // Read existing profile preferences
    int appLaunchSlot = get_config_int(path, "profiles", "app_launch_slot", 0);
    int logonSlot = get_config_int(path, "profiles", "logon_slot", 0);
    bool startOnLogon = is_start_on_logon_enabled(path);
    int selectedSlot = slot;
    if (appLaunchSlot < 0 || appLaunchSlot > CONFIG_NUM_SLOTS) appLaunchSlot = 0;
    if (logonSlot < 0 || logonSlot > CONFIG_NUM_SLOTS) logonSlot = 0;

    // Buffer for building complete config (heap-allocated to avoid large stack usage)
    char* cfg = (char*)calloc(1, CFG_BUFFER_SIZE);
    if (!cfg) {
        set_message(err, errSize, "Out of memory allocating config buffer");
        return false;
    }
    char* existingBuf = (char*)calloc(1, CFG_BUFFER_SIZE);
    if (!existingBuf) {
        free(cfg);
        set_message(err, errSize, "Out of memory allocating existing buffer");
        return false;
    }
    size_t used = 0;
    bool truncated = false;
    auto appendf = [&](const char* fmt, ...) {
        if (truncated || used >= CFG_BUFFER_SIZE - 1) {
            truncated = true;
            return;
        }
        va_list ap;
        va_start(ap, fmt);
        int n = _vsnprintf_s(cfg + used, CFG_BUFFER_SIZE - used, _TRUNCATE, fmt, ap);
        va_end(ap);
        if (n > 0) used += (size_t)n;
        else truncated = true;
    };
    // Read existing file to preserve sections we're not touching
    EnterCriticalSection(&g_configLock);
    DWORD existingLen = GetPrivateProfileStringA(nullptr, nullptr, "", existingBuf, CFG_BUFFER_SIZE, path);
    LeaveCriticalSection(&g_configLock);
    (void)existingLen;

    // Build [meta]
    appendf("[meta]\r\nformat_version=2\r\n\r\n");

    // Build [profiles] section
    appendf("[profiles]\r\n");
    appendf("selected_slot=%d\r\n", selectedSlot);
    appendf("app_launch_slot=%d\r\n", appLaunchSlot);
    appendf("logon_slot=%d\r\n", logonSlot);
    appendf("\r\n");

    // Write the target profile section
    {
        char controlsSection[32];
        char curveSection[32];
        char fanCurveSection[32];
        StringCchPrintfA(controlsSection, ARRAY_COUNT(controlsSection), "profile%d", slot);
        StringCchPrintfA(curveSection, ARRAY_COUNT(curveSection), "profile%d_curve", slot);
        StringCchPrintfA(fanCurveSection, ARRAY_COUNT(fanCurveSection), "profile%d_fan_curve", slot);

        int profileGpuOffsetMHz = 0;
        int profileExcludeLowCount = 0;
        resolve_profile_gpu_offset_state_for_save(desired, &profileGpuOffsetMHz, &profileExcludeLowCount);
        bool saveCurveAsBasePlusGpuOffset = can_save_curve_as_base_plus_gpu_offset(desired, profileGpuOffsetMHz, profileExcludeLowCount);

        appendf("[%s]\r\n", controlsSection);
        appendf("gpu_offset_mhz=%d\r\n", profileGpuOffsetMHz);
        appendf("gpu_offset_exclude_low_count=%d\r\n", profileExcludeLowCount);
        appendf("lock_ci=%d\r\n", desired->hasLock ? desired->lockCi : (g_app.lockedCi >= 0 ? g_app.lockedCi : -1));
        appendf("lock_mhz=%u\r\n", desired->hasLock ? desired->lockMHz : g_app.lockedFreq);
        appendf("lock_tracks_anchor=%d\r\n", desired->hasLock ? (desired->lockTracksAnchor ? 1 : 0) : (g_app.guiLockTracksAnchor ? 1 : 0));
        appendf("mem_offset_mhz=%d\r\n", desired->hasMemOffset ? desired->memOffsetMHz : mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz));
        appendf("power_limit_pct=%d\r\n", desired->hasPowerLimit ? desired->powerLimitPct : g_app.powerLimitPct);
        appendf("fan_mode=%s\r\n", fan_mode_to_config_value(desired->hasFan ? desired->fanMode : get_effective_live_fan_mode()));
        if (desired->hasFan) {
            if (desired->fanMode == FAN_MODE_AUTO) appendf("fan=auto\r\n");
            else appendf("fan=%d\r\n", desired->fanPercent);
            appendf("fan_fixed_pct=%d\r\n", clamp_percent(desired->fanPercent));
        } else {
            if (g_app.fanIsAuto) appendf("fan=auto\r\n");
            else appendf("fan=%u\r\n", current_manual_fan_target_percent());
            appendf("fan_fixed_pct=%u\r\n", current_manual_fan_target_percent());
        }
        appendf("\r\n");

        appendf("[%s]\r\n", curveSection);
        appendf("format=explicit_vf_points_v1\r\n");
        appendf("gpu_offset_mhz=%d\r\n", profileGpuOffsetMHz);
        appendf("gpu_offset_exclude_low_count=%d\r\n", profileExcludeLowCount);
        if (saveCurveAsBasePlusGpuOffset) {
            appendf("curve_semantics=base_plus_gpu_offset\r\n");
        }
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            unsigned int mhz = saved_curve_point_mhz(desired, i,
                saveCurveAsBasePlusGpuOffset ? profileGpuOffsetMHz : 0,
                saveCurveAsBasePlusGpuOffset ? profileExcludeLowCount : 0);
            if (mhz == 0) continue;
            unsigned int voltMv = g_app.curve[i].volt_uV / 1000;
            int offsetKHz = g_app.curve[i].freq_kHz > 0 ? g_app.freqOffsets[i] : 0;
            appendf("point%d_mhz=%u\r\n", i, mhz);
            appendf("point%d_mv=%u\r\n", i, voltMv);
            appendf("point%d_offset_khz=%d\r\n", i, offsetKHz);
            appendf("point%d_visible=%d\r\n", i, is_curve_point_visible_in_gui(i) ? 1 : 0);
        }
        appendf("\r\n");

        const FanCurveConfig* curveToWrite = desired->hasFan ? &desired->fanCurve : &g_app.activeFanCurve;
        append_fan_curve_section_text(cfg, CFG_BUFFER_SIZE, &used, fanCurveSection, curveToWrite);
    }

    // Copy other profile sections (except the one being written)
    {
        const char* p = existingBuf;
        while (*p) {
            bool skip = false;
            char targetControls[32], targetCurve[32], targetFanCurve[32];
            StringCchPrintfA(targetControls, ARRAY_COUNT(targetControls), "profile%d", slot);
            StringCchPrintfA(targetCurve, ARRAY_COUNT(targetCurve), "profile%d_curve", slot);
            StringCchPrintfA(targetFanCurve, ARRAY_COUNT(targetFanCurve), "profile%d_fan_curve", slot);
            if (strcmp(p, targetControls) == 0 || strcmp(p, targetCurve) == 0 || strcmp(p, targetFanCurve) == 0 ||
                strcmp(p, "meta") == 0 || strcmp(p, "profiles") == 0 || strcmp(p, "startup") == 0 ||
                (slot == 1 && (strcmp(p, "controls") == 0 || strcmp(p, "curve") == 0 || strcmp(p, "fan_curve") == 0))) {
                skip = true;
            }
            if (!skip) {
                appendf("[%s]\r\n", p);
                char keys[16384] = {};
                char val[4096] = {};
                EnterCriticalSection(&g_configLock);
                GetPrivateProfileStringA(p, nullptr, "", keys, sizeof(keys), path);
                const char* kp = keys;
                while (*kp) {
                    GetPrivateProfileStringA(p, kp, "", val, sizeof(val), path);
                    if (strchr(kp, '\r') || strchr(kp, '\n') || strchr(val, '\r') || strchr(val, '\n')) {
                        kp += strlen(kp) + 1;
                        continue;
                    }
                    appendf("%s=%s\r\n", kp, val);
                    kp += strlen(kp) + 1;
                }
                LeaveCriticalSection(&g_configLock);
                appendf("\r\n");
            }
            p += strlen(p) + 1;
        }
    }

    // Write legacy sections for backward compatibility when slot 1 is saved
    if (slot == 1) {
        int profileGpuOffsetMHz = 0;
        int profileExcludeLowCount = 0;
        resolve_profile_gpu_offset_state_for_save(desired, &profileGpuOffsetMHz, &profileExcludeLowCount);
        bool saveCurveAsBasePlusGpuOffset = can_save_curve_as_base_plus_gpu_offset(desired, profileGpuOffsetMHz, profileExcludeLowCount);

        appendf("[controls]\r\n");
        appendf("gpu_offset_mhz=%d\r\n", profileGpuOffsetMHz);
        appendf("gpu_offset_exclude_low_count=%d\r\n", profileExcludeLowCount);
        appendf("lock_ci=%d\r\n", desired->hasLock ? desired->lockCi : (g_app.lockedCi >= 0 ? g_app.lockedCi : -1));
        appendf("lock_mhz=%u\r\n", desired->hasLock ? desired->lockMHz : g_app.lockedFreq);
        appendf("lock_tracks_anchor=%d\r\n", desired->hasLock ? (desired->lockTracksAnchor ? 1 : 0) : (g_app.guiLockTracksAnchor ? 1 : 0));
        appendf("mem_offset_mhz=%d\r\n", desired->hasMemOffset ? desired->memOffsetMHz : mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz));
        appendf("power_limit_pct=%d\r\n", desired->hasPowerLimit ? desired->powerLimitPct : g_app.powerLimitPct);
        appendf("fan_mode=%s\r\n", fan_mode_to_config_value(desired->hasFan ? desired->fanMode : get_effective_live_fan_mode()));
        if (desired->hasFan) {
            if (desired->fanMode == FAN_MODE_AUTO) appendf("fan=auto\r\n");
            else appendf("fan=%d\r\n", desired->fanPercent);
            appendf("fan_fixed_pct=%d\r\n", clamp_percent(desired->fanPercent));
        } else {
            if (g_app.fanIsAuto) appendf("fan=auto\r\n");
            else appendf("fan=%u\r\n", current_manual_fan_target_percent());
            appendf("fan_fixed_pct=%u\r\n", current_manual_fan_target_percent());
        }
        appendf("\r\n");
        appendf("[curve]\r\n");
        appendf("format=explicit_vf_points_v1\r\n");
        appendf("gpu_offset_mhz=%d\r\n", profileGpuOffsetMHz);
        appendf("gpu_offset_exclude_low_count=%d\r\n", profileExcludeLowCount);
        if (saveCurveAsBasePlusGpuOffset) {
            appendf("curve_semantics=base_plus_gpu_offset\r\n");
        }
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            unsigned int mhz = saved_curve_point_mhz(desired, i,
                saveCurveAsBasePlusGpuOffset ? profileGpuOffsetMHz : 0,
                saveCurveAsBasePlusGpuOffset ? profileExcludeLowCount : 0);
            if (mhz == 0) continue;
            unsigned int voltMv = g_app.curve[i].volt_uV / 1000;
            int offsetKHz = g_app.curve[i].freq_kHz > 0 ? g_app.freqOffsets[i] : 0;
            appendf("point%d_mhz=%u\r\n", i, mhz);
            appendf("point%d_mv=%u\r\n", i, voltMv);
            appendf("point%d_offset_khz=%d\r\n", i, offsetKHz);
            appendf("point%d_visible=%d\r\n", i, is_curve_point_visible_in_gui(i) ? 1 : 0);
        }
        appendf("\r\n");

        const FanCurveConfig* curveToWrite = desired->hasFan ? &desired->fanCurve : &g_app.activeFanCurve;
        append_fan_curve_section_text(cfg, CFG_BUFFER_SIZE, &used, "fan_curve", curveToWrite);
    }

    appendf("[startup]\r\napply_on_launch=%d\r\nstart_program_on_logon=%d\r\n\r\n", logonSlot > 0 ? 1 : 0, startOnLogon ? 1 : 0);

    bool ok = !truncated;
    if (truncated) {
        set_message(err, errSize, "Config buffer truncated during save");
    }
    if (ok) {
        ok = write_text_file_atomic(path, cfg, used, err, errSize);
    }
    if (ok) {
        EnterCriticalSection(&g_configLock);
        WritePrivateProfileStringA(NULL, NULL, NULL, NULL);
        LeaveCriticalSection(&g_configLock);
        invalidate_tray_profile_cache();
    }
    free(cfg);
    free(existingBuf);
    return ok;
}

static bool clear_profile_from_config(const char* path, int slot, char* err, size_t errSize) {
    if (!path || slot < 1 || slot > CONFIG_NUM_SLOTS) {
        set_message(err, errSize, "Invalid profile clear arguments");
        return false;
    }

    // Read existing profile preferences
    int appLaunchSlot = get_config_int(path, "profiles", "app_launch_slot", 0);
    int logonSlot = get_config_int(path, "profiles", "logon_slot", 0);
    int selectedSlot = get_config_int(path, "profiles", "selected_slot", CONFIG_DEFAULT_SLOT);
    bool startOnLogon = is_start_on_logon_enabled(path);
    if (appLaunchSlot < 0 || appLaunchSlot > CONFIG_NUM_SLOTS) appLaunchSlot = 0;
    if (logonSlot < 0 || logonSlot > CONFIG_NUM_SLOTS) logonSlot = 0;
    if (selectedSlot < 1 || selectedSlot > CONFIG_NUM_SLOTS) selectedSlot = CONFIG_DEFAULT_SLOT;

    if (appLaunchSlot == slot) appLaunchSlot = 0;
    if (logonSlot == slot) logonSlot = 0;
    if (selectedSlot == slot) {
        selectedSlot = CONFIG_DEFAULT_SLOT;
        if (selectedSlot == slot) {
            for (int s = 1; s <= CONFIG_NUM_SLOTS; s++) {
                if (s != slot && is_profile_slot_saved(path, s)) {
                    selectedSlot = s;
                    break;
                }
            }
        }
    }

    // Read existing file (heap-allocated to avoid large stack usage)
    char* existingBuf = (char*)calloc(1, CFG_BUFFER_SIZE);
    if (!existingBuf) {
        set_message(err, errSize, "Out of memory allocating existing buffer");
        return false;
    }
    EnterCriticalSection(&g_configLock);
    GetPrivateProfileStringA(nullptr, nullptr, "", existingBuf, CFG_BUFFER_SIZE, path);
    LeaveCriticalSection(&g_configLock);

    char* cfg = (char*)calloc(1, CFG_BUFFER_SIZE);
    if (!cfg) {
        free(existingBuf);
        set_message(err, errSize, "Out of memory allocating config buffer");
        return false;
    }
    size_t used = 0;
    bool truncated = false;
    auto appendf = [&](const char* fmt, ...) {
        if (truncated || used >= CFG_BUFFER_SIZE - 1) {
            truncated = true;
            return;
        }
        va_list ap;
        va_start(ap, fmt);
        int n = _vsnprintf_s(cfg + used, CFG_BUFFER_SIZE - used, _TRUNCATE, fmt, ap);
        va_end(ap);
        if (n > 0) used += (size_t)n;
        else truncated = true;
    };

    char targetControls[32], targetCurve[32], targetFanCurve[32];
    StringCchPrintfA(targetControls, ARRAY_COUNT(targetControls), "profile%d", slot);
    StringCchPrintfA(targetCurve, ARRAY_COUNT(targetCurve), "profile%d_curve", slot);
    StringCchPrintfA(targetFanCurve, ARRAY_COUNT(targetFanCurve), "profile%d_fan_curve", slot);

    // Write [meta] and [profiles]
    appendf("[meta]\r\nformat_version=2\r\n\r\n");
    appendf("[profiles]\r\nselected_slot=%d\r\napp_launch_slot=%d\r\nlogon_slot=%d\r\n\r\n", selectedSlot, appLaunchSlot, logonSlot);

    // Copy all sections except the cleared ones and managed sections
    const char* p = existingBuf;
    while (*p) {
        bool skip = (strcmp(p, targetControls) == 0 || strcmp(p, targetCurve) == 0 || strcmp(p, targetFanCurve) == 0 ||
                     strcmp(p, "meta") == 0 || strcmp(p, "profiles") == 0 || strcmp(p, "startup") == 0 ||
                 (slot == 1 && (strcmp(p, "controls") == 0 || strcmp(p, "curve") == 0 || strcmp(p, "fan_curve") == 0)));
        if (!skip) {
            appendf("[%s]\r\n", p);
            char keys[16384] = {};
            char val[4096] = {};
            EnterCriticalSection(&g_configLock);
            GetPrivateProfileStringA(p, nullptr, "", keys, sizeof(keys), path);
            const char* kp = keys;
            while (*kp) {
                GetPrivateProfileStringA(p, kp, "", val, sizeof(val), path);
                appendf("%s=%s\r\n", kp, val);
                kp += strlen(kp) + 1;
            }
            LeaveCriticalSection(&g_configLock);
            appendf("\r\n");
        }
        p += strlen(p) + 1;
    }

    appendf("[startup]\r\napply_on_launch=%d\r\nstart_program_on_logon=%d\r\n\r\n", logonSlot > 0 ? 1 : 0, startOnLogon ? 1 : 0);

    bool ok2 = write_text_file_atomic(path, cfg, used, err, errSize);
    if (ok2 && truncated) {
        ok2 = false;
        set_message(err, errSize, "Config buffer truncated during clear");
    }
    if (ok2) {
        EnterCriticalSection(&g_configLock);
        WritePrivateProfileStringA(NULL, NULL, NULL, NULL);
        LeaveCriticalSection(&g_configLock);
        invalidate_tray_profile_cache();
    }
    free(cfg);
    free(existingBuf);
    return ok2;
}

