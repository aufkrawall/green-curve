// ============================================================================
// Profile Slot I/O
// ============================================================================

static bool load_profile_from_config(const char* path, int slot, DesiredSettings* desired, char* err, size_t errSize) {
    if (!path || !desired || slot < 1 || slot > CONFIG_NUM_SLOTS) {
        set_message(err, errSize, "Invalid profile load arguments");
        return false;
    }
    initialize_desired_settings_defaults(desired);

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

    GetPrivateProfileStringA(controlsSection, "gpu_offset_exclude_low_70", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        int value = 0;
        if (!parse_int_strict(buf, &value)) {
            set_message(err, errSize, "Invalid gpu_offset_exclude_low_70 in profile %d", slot);
            return false;
        }
        desired->gpuOffsetExcludeLow70 = value != 0;
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

    char curveSemantics[64] = {};
    GetPrivateProfileStringA(curveSection, "curve_semantics", "", curveSemantics, sizeof(curveSemantics), path);
    trim_ascii(curveSemantics);
    bool legacyCurveSemantics = curveSemantics[0] == 0;

    for (int i = 0; i < VF_NUM_POINTS; i++) {
        char key[32];
        StringCchPrintfA(key, ARRAY_COUNT(key), "point%d", i);
        GetPrivateProfileStringA(curveSection, key, "", buf, sizeof(buf), path);
        trim_ascii(buf);
        if (!buf[0]) continue;
        int v = 0;
        if (!parse_int_strict(buf, &v) || v <= 0) {
            set_message(err, errSize, "Invalid curve point %d in profile %d", i, slot);
            return false;
        }
        desired->hasCurvePoint[i] = true;
        desired->curvePointMHz[i] = (unsigned int)v;
    }

    bool basePlusGpuOffsetCurve = streqi_ascii(curveSemantics, "base_plus_gpu_offset");
    if (basePlusGpuOffsetCurve && desired->hasGpuOffset && desired->gpuOffsetMHz != 0) {
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            if (!desired->hasCurvePoint[i]) continue;
            int offsetCompmhz = gpu_offset_component_mhz_for_point(i, desired->gpuOffsetMHz, desired->gpuOffsetExcludeLow70);
            int absoluteMhz = (int)desired->curvePointMHz[i] + offsetCompmhz;
            if (absoluteMhz <= 0) {
                desired->hasCurvePoint[i] = false;
                desired->curvePointMHz[i] = 0;
                continue;
            }
            desired->curvePointMHz[i] = (unsigned int)absoluteMhz;
        }
    } else if (legacyCurveSemantics && desired->hasGpuOffset && desired->gpuOffsetMHz != 0) {
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            desired->hasCurvePoint[i] = false;
            desired->curvePointMHz[i] = 0;
        }
    }

    for (int i = 1; i < VF_NUM_POINTS; i++) {
        if (desired->hasCurvePoint[i] && desired->hasCurvePoint[i - 1]) {
            if (desired->curvePointMHz[i] < desired->curvePointMHz[i - 1]) {
                desired->curvePointMHz[i] = desired->curvePointMHz[i - 1];
            }
        }
    }

    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!desired->hasCurvePoint[i]) continue;
        unsigned int volt_mv = g_app.curve[i].volt_uV / 1000;
        if (volt_mv == 0 || volt_mv < (unsigned)MIN_VISIBLE_VOLT_mV) {
            desired->hasCurvePoint[i] = false;
            desired->curvePointMHz[i] = 0;
        }
    }

    if (!desired->hasFan) {
        desired->fanAuto = true;
        desired->fanMode = FAN_MODE_AUTO;
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

    // Read existing profile preferences
    int appLaunchSlot = get_config_int(path, "profiles", "app_launch_slot", 0);
    int logonSlot = get_config_int(path, "profiles", "logon_slot", 0);
    bool startOnLogon = is_start_on_logon_enabled(path);
    int selectedSlot = slot;
    if (appLaunchSlot < 0 || appLaunchSlot > CONFIG_NUM_SLOTS) appLaunchSlot = 0;
    if (logonSlot < 0 || logonSlot > CONFIG_NUM_SLOTS) logonSlot = 0;

    // Buffer for building complete config
    char cfg[131072] = {};
    size_t used = 0;
    auto appendf = [&](const char* fmt, ...) {
        if (used >= sizeof(cfg) - 1) return;
        va_list ap;
        va_start(ap, fmt);
        int n = _vsnprintf_s(cfg + used, sizeof(cfg) - used, _TRUNCATE, fmt, ap);
        va_end(ap);
        if (n > 0) used += (size_t)n;
    };
    // Read existing file to preserve sections we're not touching
    char existingBuf[131072] = {};
    EnterCriticalSection(&g_configLock);
    DWORD existingLen = GetPrivateProfileStringA(nullptr, nullptr, "", existingBuf, sizeof(existingBuf), path);
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

        appendf("[%s]\r\n", controlsSection);
        appendf("gpu_offset_mhz=%d\r\n", desired->hasGpuOffset ? desired->gpuOffsetMHz : (g_app.gpuClockOffsetkHz / 1000));
        appendf("gpu_offset_exclude_low_70=%d\r\n", desired->hasGpuOffset && desired->gpuOffsetExcludeLow70 ? 1 : 0);
        appendf("lock_ci=%d\r\n", desired->hasLock ? desired->lockCi : (g_app.lockedCi >= 0 ? g_app.lockedCi : -1));
        appendf("lock_mhz=%u\r\n", desired->hasLock ? desired->lockMHz : g_app.lockedFreq);
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
        appendf("curve_semantics=base_plus_gpu_offset\r\n");
        int profileGpuOffsetMHz = desired->hasGpuOffset ? desired->gpuOffsetMHz : (g_app.gpuClockOffsetkHz / 1000);
        bool profileExcludeLow70 = desired->hasGpuOffset ? desired->gpuOffsetExcludeLow70 : false;
        bool profileHasGpuOffset = desired->hasGpuOffset || g_app.gpuClockOffsetkHz != 0;
        if (!desired->hasGpuOffset) {
            profileExcludeLow70 = g_app.appliedGpuOffsetExcludeLow70;
        }
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            unsigned int mhz = 0;
            if (desired->hasCurvePoint[i]) {
                mhz = desired->curvePointMHz[i];
                if (profileHasGpuOffset && g_app.curve[i].freq_kHz > 0) {
                    int offsetCompmhz = gpu_offset_component_mhz_for_point(i, profileGpuOffsetMHz, profileExcludeLow70);
                    mhz = (unsigned int)((int)mhz - offsetCompmhz);
                    if ((int)mhz <= 0) mhz = 0;
                }
            } else if (g_app.curve[i].freq_kHz > 0) {
                mhz = displayed_curve_mhz(g_app.curve[i].freq_kHz);
                if (profileHasGpuOffset) {
                    int offsetCompmhz = gpu_offset_component_mhz_for_point(i, profileGpuOffsetMHz, profileExcludeLow70);
                    mhz = (unsigned int)((int)mhz - offsetCompmhz);
                    if ((int)mhz <= 0) mhz = 0;
                }
            }
            if (mhz == 0) continue;
            char key[16];
            StringCchPrintfA(key, ARRAY_COUNT(key), "point%d", i);
            appendf("%s=%u\r\n", key, mhz);
        }
        appendf("\r\n");

        const FanCurveConfig* curveToWrite = desired->hasFan ? &desired->fanCurve : &g_app.activeFanCurve;
        append_fan_curve_section_text(cfg, sizeof(cfg), &used, fanCurveSection, curveToWrite);
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
                EnterCriticalSection(&g_configLock);
                GetPrivateProfileStringA(p, nullptr, "", keys, sizeof(keys), path);
                const char* kp = keys;
                while (*kp) {
                    char val[4096] = {};
                    GetPrivateProfileStringA(p, kp, "", val, sizeof(val), path);
                    LeaveCriticalSection(&g_configLock);
                    appendf("%s=%s\r\n", kp, val);
                    EnterCriticalSection(&g_configLock);
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
        appendf("[controls]\r\n");
        appendf("gpu_offset_mhz=%d\r\n", desired->hasGpuOffset ? desired->gpuOffsetMHz : (g_app.gpuClockOffsetkHz / 1000));
        appendf("gpu_offset_exclude_low_70=%d\r\n", desired->hasGpuOffset && desired->gpuOffsetExcludeLow70 ? 1 : 0);
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
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            unsigned int mhz = 0;
            if (desired->hasCurvePoint[i]) {
                mhz = desired->curvePointMHz[i];
            } else if (g_app.curve[i].freq_kHz > 0) {
                mhz = displayed_curve_mhz(g_app.curve[i].freq_kHz);
            }
            if (mhz == 0) continue;
            char key[16];
            StringCchPrintfA(key, ARRAY_COUNT(key), "point%d", i);
            appendf("%s=%u\r\n", key, mhz);
        }
        appendf("\r\n");

        const FanCurveConfig* curveToWrite = desired->hasFan ? &desired->fanCurve : &g_app.activeFanCurve;
        append_fan_curve_section_text(cfg, sizeof(cfg), &used, "fan_curve", curveToWrite);
    }

    appendf("[startup]\r\napply_on_launch=%d\r\nstart_program_on_logon=%d\r\n\r\n", logonSlot > 0 ? 1 : 0, startOnLogon ? 1 : 0);

    bool ok = write_text_file_atomic(path, cfg, used, err, errSize);
    if (ok) {
        EnterCriticalSection(&g_configLock);
        WritePrivateProfileStringA(NULL, NULL, NULL, NULL);
        LeaveCriticalSection(&g_configLock);
        invalidate_tray_profile_cache();
    }
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

    // Read existing file
    char existingBuf[131072] = {};
    EnterCriticalSection(&g_configLock);
    GetPrivateProfileStringA(nullptr, nullptr, "", existingBuf, sizeof(existingBuf), path);
    LeaveCriticalSection(&g_configLock);

    char cfg[131072] = {};
    size_t used = 0;
    auto appendf = [&](const char* fmt, ...) {
        if (used >= sizeof(cfg) - 1) return;
        va_list ap;
        va_start(ap, fmt);
        int n = _vsnprintf_s(cfg + used, sizeof(cfg) - used, _TRUNCATE, fmt, ap);
        va_end(ap);
        if (n > 0) used += (size_t)n;
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
            EnterCriticalSection(&g_configLock);
            GetPrivateProfileStringA(p, nullptr, "", keys, sizeof(keys), path);
            const char* kp = keys;
            while (*kp) {
                char val[4096] = {};
                GetPrivateProfileStringA(p, kp, "", val, sizeof(val), path);
                LeaveCriticalSection(&g_configLock);
                appendf("%s=%s\r\n", kp, val);
                EnterCriticalSection(&g_configLock);
                kp += strlen(kp) + 1;
            }
            LeaveCriticalSection(&g_configLock);
            appendf("\r\n");
        }
        p += strlen(p) + 1;
    }

    appendf("[startup]\r\napply_on_launch=%d\r\nstart_program_on_logon=%d\r\n\r\n", logonSlot > 0 ? 1 : 0, startOnLogon ? 1 : 0);

    bool ok2 = write_text_file_atomic(path, cfg, used, err, errSize);
    if (ok2) {
        EnterCriticalSection(&g_configLock);
        WritePrivateProfileStringA(NULL, NULL, NULL, NULL);
        LeaveCriticalSection(&g_configLock);
        invalidate_tray_profile_cache();
    }
    return ok2;
}

static void refresh_profile_controls_from_config() {
    if (!g_app.hProfileCombo) return;
    int selectedSlot = get_config_int(g_app.configPath, "profiles", "selected_slot", CONFIG_DEFAULT_SLOT);
    int appLaunchSlot = get_config_int(g_app.configPath, "profiles", "app_launch_slot", 0);
    int logonSlot = get_config_int(g_app.configPath, "profiles", "logon_slot", 0);

    SendMessageA(g_app.hProfileCombo, WM_SETREDRAW, FALSE, 0);
    SendMessageA(g_app.hProfileCombo, CB_RESETCONTENT, 0, 0);
    for (int s = 1; s <= CONFIG_NUM_SLOTS; s++) {
        char label[32] = {};
        StringCchPrintfA(label, ARRAY_COUNT(label), "Slot %d - %s", s,
            is_profile_slot_saved(g_app.configPath, s) ? "Saved" : "Empty");
        SendMessageA(g_app.hProfileCombo, CB_ADDSTRING, 0, (LPARAM)label);
    }
    SendMessageA(g_app.hProfileCombo, CB_SETDROPPEDWIDTH, (WPARAM)dp(170), 0);

    if (g_app.hAppLaunchCombo) {
        SendMessageA(g_app.hAppLaunchCombo, WM_SETREDRAW, FALSE, 0);
        SendMessageA(g_app.hAppLaunchCombo, CB_RESETCONTENT, 0, 0);
        SendMessageA(g_app.hAppLaunchCombo, CB_ADDSTRING, 0, (LPARAM)"Disabled");
        for (int s = 1; s <= CONFIG_NUM_SLOTS; s++) {
            char label[32] = {};
            StringCchPrintfA(label, ARRAY_COUNT(label), "Slot %d - %s", s,
                is_profile_slot_saved(g_app.configPath, s) ? "Saved" : "Empty");
            SendMessageA(g_app.hAppLaunchCombo, CB_ADDSTRING, 0, (LPARAM)label);
        }
        SendMessageA(g_app.hAppLaunchCombo, CB_SETDROPPEDWIDTH, (WPARAM)dp(180), 0);
    }
    if (g_app.hLogonCombo) {
        SendMessageA(g_app.hLogonCombo, WM_SETREDRAW, FALSE, 0);
        SendMessageA(g_app.hLogonCombo, CB_RESETCONTENT, 0, 0);
        SendMessageA(g_app.hLogonCombo, CB_ADDSTRING, 0, (LPARAM)"Disabled");
        for (int s = 1; s <= CONFIG_NUM_SLOTS; s++) {
            char label[32] = {};
            StringCchPrintfA(label, ARRAY_COUNT(label), "Slot %d - %s", s,
                is_profile_slot_saved(g_app.configPath, s) ? "Saved" : "Empty");
            SendMessageA(g_app.hLogonCombo, CB_ADDSTRING, 0, (LPARAM)label);
        }
        SendMessageA(g_app.hLogonCombo, CB_SETDROPPEDWIDTH, (WPARAM)dp(180), 0);
    }

    if (appLaunchSlot < 0 || appLaunchSlot > CONFIG_NUM_SLOTS) appLaunchSlot = 0;
    if (logonSlot < 0 || logonSlot > CONFIG_NUM_SLOTS) logonSlot = 0;
    if (selectedSlot < 1 || selectedSlot > CONFIG_NUM_SLOTS) selectedSlot = CONFIG_DEFAULT_SLOT;
    SendMessageA(g_app.hProfileCombo, CB_SETCURSEL, (WPARAM)(selectedSlot - 1), 0);

    if (appLaunchSlot >= 0 && appLaunchSlot <= CONFIG_NUM_SLOTS)
        SendMessageA(g_app.hAppLaunchCombo, CB_SETCURSEL, (WPARAM)appLaunchSlot, 0);
    if (logonSlot >= 0 && logonSlot <= CONFIG_NUM_SLOTS)
        SendMessageA(g_app.hLogonCombo, CB_SETCURSEL, (WPARAM)logonSlot, 0);

    SendMessageA(g_app.hProfileCombo, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_app.hProfileCombo, nullptr, TRUE);
    if (g_app.hAppLaunchCombo) {
        SendMessageA(g_app.hAppLaunchCombo, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_app.hAppLaunchCombo, nullptr, TRUE);
    }
    if (g_app.hLogonCombo) {
        SendMessageA(g_app.hLogonCombo, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_app.hLogonCombo, nullptr, TRUE);
    }
    if (g_app.hStartOnLogonCheck) {
        SendMessageA(g_app.hStartOnLogonCheck, BM_SETCHECK,
            (WPARAM)(is_start_on_logon_enabled(g_app.configPath) ? BST_CHECKED : BST_UNCHECKED), 0);
    }

    update_profile_state_label();
    update_profile_action_buttons();
    update_tray_icon();
}

static void migrate_legacy_config_if_needed(const char* path) {
    if (!path) return;
    char test[8] = {};
    GetPrivateProfileStringA("meta", "format_version", "_X", test, sizeof(test), path);
    if (strcmp(test, "_X") != 0) return;

    GetPrivateProfileStringA("controls", "gpu_offset_mhz", "_X", test, sizeof(test), path);
    if (strcmp(test, "_X") == 0) return;

    DesiredSettings desired = {};
    char err[256] = {};
    if (load_desired_settings_from_ini(path, &desired, err, sizeof(err))) {
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            if (!desired.hasCurvePoint[i] && g_app.curve[i].freq_kHz > 0) {
                desired.hasCurvePoint[i] = true;
                desired.curvePointMHz[i] = displayed_curve_mhz(g_app.curve[i].freq_kHz);
            }
        }
        if (!desired.hasGpuOffset) { desired.hasGpuOffset = true; desired.gpuOffsetMHz = g_app.gpuClockOffsetkHz / 1000; }
        if (!desired.hasMemOffset) { desired.hasMemOffset = true; desired.memOffsetMHz = mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz); }
        if (!desired.hasPowerLimit) { desired.hasPowerLimit = true; desired.powerLimitPct = g_app.powerLimitPct; }
        if (!desired.hasFan) {
            desired.hasFan = true;
            desired.fanMode = g_app.activeFanMode;
            desired.fanAuto = g_app.activeFanMode == FAN_MODE_AUTO;
            desired.fanPercent = g_app.activeFanFixedPercent;
            copy_fan_curve(&desired.fanCurve, &g_app.activeFanCurve);
        }

        bool wasStartupEnabled = false;
        load_startup_enabled_from_config(path, &wasStartupEnabled);

        save_profile_to_config(path, 1, &desired, err, sizeof(err));
        if (wasStartupEnabled) {
            set_config_int(path, "profiles", "logon_slot", 1);
        }
        set_config_int(path, "profiles", "selected_slot", 1);
        set_config_int(path, "profiles", "app_launch_slot", 0);
    }
}

static void merge_desired_settings(DesiredSettings* base, const DesiredSettings* override) {
    if (!base || !override) return;
    if (override->hasGpuOffset) {
        base->hasGpuOffset = true;
        base->gpuOffsetMHz = override->gpuOffsetMHz;
        base->gpuOffsetExcludeLow70 = override->gpuOffsetExcludeLow70;
    }
    if (override->hasMemOffset) {
        base->hasMemOffset = true;
        base->memOffsetMHz = override->memOffsetMHz;
    }
    if (override->hasPowerLimit) {
        base->hasPowerLimit = true;
        base->powerLimitPct = override->powerLimitPct;
    }
    if (override->hasFan) {
        base->hasFan = true;
        base->fanMode = override->fanMode;
        base->fanAuto = override->fanAuto;
        base->fanPercent = override->fanPercent;
        copy_fan_curve(&base->fanCurve, &override->fanCurve);
    }
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (override->hasCurvePoint[i]) {
            base->hasCurvePoint[i] = true;
            base->curvePointMHz[i] = override->curvePointMHz[i];
        }
    }
}

static bool desired_has_any_action(const DesiredSettings* desired) {
    if (!desired) return false;
    if (desired->hasGpuOffset || desired->hasMemOffset || desired->hasPowerLimit || desired->hasFan) return true;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (desired->hasCurvePoint[i]) return true;
    }
    return false;
}

static void populate_desired_into_gui(const DesiredSettings* desired) {
    if (!desired) return;
    unlock_all();
    if (g_app.loaded) populate_edits();
    // Curve points
    for (int vi = 0; vi < g_app.numVisible; vi++) {
        int ci = g_app.visibleMap[vi];
        if (g_app.hEditsMhz[vi]) {
            unsigned int mhz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
            if (desired->hasCurvePoint[ci]) mhz = desired->curvePointMHz[ci];
            set_edit_value(g_app.hEditsMhz[vi], mhz);
        }
    }
    // GPU offset
    if (desired->hasGpuOffset) {
        g_app.guiGpuOffsetMHz = desired->gpuOffsetMHz;
        g_app.guiGpuOffsetExcludeLow70 = desired->gpuOffsetExcludeLow70;
    }
    if (desired->hasGpuOffset && g_app.hGpuOffsetEdit) {
        set_edit_value(g_app.hGpuOffsetEdit, desired->gpuOffsetMHz);
        if (g_app.hGpuOffsetExcludeLowCheck) {
            SendMessageA(g_app.hGpuOffsetExcludeLowCheck, BM_SETCHECK,
                (WPARAM)(desired->gpuOffsetExcludeLow70 ? BST_CHECKED : BST_UNCHECKED), 0);
        }
    }
    // Mem offset
    if (desired->hasMemOffset && g_app.hMemOffsetEdit) {
        set_edit_value(g_app.hMemOffsetEdit, desired->memOffsetMHz);
    }
    // Power limit
    if (desired->hasPowerLimit && g_app.hPowerLimitEdit) {
        set_edit_value(g_app.hPowerLimitEdit, desired->powerLimitPct);
    }
    // Fan
    if (desired->hasFan) {
        g_app.guiFanMode = desired->fanMode;
        if (desired->fanMode == FAN_MODE_FIXED) {
            g_app.guiFanFixedPercent = clamp_percent(desired->fanPercent);
        } else {
            g_app.guiFanFixedPercent = current_displayed_fan_percent();
        }
        copy_fan_curve(&g_app.guiFanCurve, &desired->fanCurve);
        ensure_valid_fan_curve_config(&g_app.guiFanCurve);
        if (g_app.hFanModeCombo) {
            SendMessageA(g_app.hFanModeCombo, CB_SETCURSEL, (WPARAM)g_app.guiFanMode, 0);
        }
        if (g_app.hFanEdit) {
            char fanText[16] = {};
            StringCchPrintfA(fanText, ARRAY_COUNT(fanText), "%d", g_app.guiFanFixedPercent);
            SetWindowTextA(g_app.hFanEdit, fanText);
        }
        refresh_fan_curve_button_text();
        update_fan_controls_enabled_state();
    }

    if (desired->hasLock && desired->lockCi >= 0 && desired->lockMHz > 0) {
        for (int vi = 0; vi < g_app.numVisible; vi++) {
            if (g_app.visibleMap[vi] != desired->lockCi) continue;
            g_app.lockedVi = vi;
            g_app.lockedCi = desired->lockCi;
            g_app.lockedFreq = desired->lockMHz;
            if (g_app.loaded) populate_edits();
            break;
        }
    }
}

static void set_profile_status_text(const char* fmt, ...) {
    if (!g_app.hProfileStatusLabel || !fmt) return;
    char buf[256] = {};
    va_list ap;
    va_start(ap, fmt);
    StringCchVPrintfA(buf, ARRAY_COUNT(buf), fmt, ap);
    va_end(ap);
    SetWindowTextA(g_app.hProfileStatusLabel, buf);
}

static void update_profile_state_label() {
    if (!g_app.hProfileStateLabel || !g_app.hProfileCombo) return;
    int slot = (int)SendMessageA(g_app.hProfileCombo, CB_GETCURSEL, 0, 0);
    if (slot < 0) slot = CONFIG_DEFAULT_SLOT - 1;
    slot += 1;

    bool saved = is_profile_slot_saved(g_app.configPath, slot);
    bool isAppLaunch = (get_config_int(g_app.configPath, "profiles", "app_launch_slot", 0) == slot);
    bool isLogon = (get_config_int(g_app.configPath, "profiles", "logon_slot", 0) == slot);

    char roles[64] = {};
    if (isAppLaunch && isLogon) StringCchCopyA(roles, ARRAY_COUNT(roles), " | app start + logon");
    else if (isAppLaunch) StringCchCopyA(roles, ARRAY_COUNT(roles), " | app start");
    else if (isLogon) StringCchCopyA(roles, ARRAY_COUNT(roles), " | logon");

    char text[128] = {};
    StringCchPrintfA(text, ARRAY_COUNT(text), "Slot %d is %s%s", slot,
        saved ? "saved" : "empty", roles);
    SetWindowTextA(g_app.hProfileStateLabel, text);
}

static void update_profile_action_buttons() {
    if (!g_app.hProfileCombo) return;
    int slot = (int)SendMessageA(g_app.hProfileCombo, CB_GETCURSEL, 0, 0);
    if (slot < 0) slot = CONFIG_DEFAULT_SLOT - 1;
    slot += 1;
    bool saved = is_profile_slot_saved(g_app.configPath, slot);
    if (g_app.hProfileLoadBtn) EnableWindow(g_app.hProfileLoadBtn, saved ? TRUE : FALSE);
    if (g_app.hProfileClearBtn) EnableWindow(g_app.hProfileClearBtn, saved ? TRUE : FALSE);
}

static bool maybe_confirm_profile_load_replace(int slot) {
    DesiredSettings current = {};
    DesiredSettings target = {};
    char err[256] = {};
    if (!capture_gui_config_settings(&current, err, sizeof(err))) return true;
    if (!load_profile_from_config(g_app.configPath, slot, &target, err, sizeof(err))) return true;

    DesiredSettings targetFull = {};
    initialize_desired_settings_defaults(&targetFull);
    targetFull.hasGpuOffset = true;
    targetFull.gpuOffsetMHz = current_applied_gpu_offset_mhz();
    targetFull.gpuOffsetExcludeLow70 = current_applied_gpu_offset_excludes_low_points();
    targetFull.hasMemOffset = true;
    targetFull.memOffsetMHz = mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz);
    targetFull.hasPowerLimit = true;
    targetFull.powerLimitPct = g_app.powerLimitPct;
    targetFull.hasFan = true;
    targetFull.fanMode = g_app.activeFanMode;
    targetFull.fanAuto = g_app.activeFanMode == FAN_MODE_AUTO;
    targetFull.fanPercent = g_app.activeFanFixedPercent;
    copy_fan_curve(&targetFull.fanCurve, &g_app.activeFanCurve);
    for (int vi = 0; vi < g_app.numVisible; vi++) {
        int ci = g_app.visibleMap[vi];
        targetFull.hasCurvePoint[ci] = true;
        targetFull.curvePointMHz[ci] = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
    }
    merge_desired_settings(&targetFull, &target);

    bool same = true;
    if (current.gpuOffsetMHz != targetFull.gpuOffsetMHz) same = false;
    if (current.gpuOffsetExcludeLow70 != targetFull.gpuOffsetExcludeLow70) same = false;
    if (current.memOffsetMHz != targetFull.memOffsetMHz) same = false;
    if (current.powerLimitPct != targetFull.powerLimitPct) same = false;
    if (current.fanMode != targetFull.fanMode || current.fanPercent != targetFull.fanPercent || !fan_curve_equals(&current.fanCurve, &targetFull.fanCurve)) same = false;
    for (int i = 0; same && i < VF_NUM_POINTS; i++) {
        if (current.hasCurvePoint[i] != targetFull.hasCurvePoint[i]) same = false;
        else if (current.hasCurvePoint[i] && current.curvePointMHz[i] != targetFull.curvePointMHz[i]) same = false;
    }
    if (same) return true;

    char msg[256] = {};
    StringCchPrintfA(msg, ARRAY_COUNT(msg),
        "Loading slot %d will replace the values currently typed into the GUI. Continue?", slot);
    return MessageBoxA(g_app.hMainWnd, msg, "Green Curve", MB_YESNO | MB_ICONQUESTION) == IDYES;
}

static void maybe_load_app_launch_profile_to_gui() {
    if (g_app.launchedFromLogon) {
        set_profile_status_text("Ready. Skipped app-start auto-load for the logon launch.");
        return;
    }
    int appLaunchSlot = get_config_int(g_app.configPath, "profiles", "app_launch_slot", 0);
    if (appLaunchSlot < 1 || appLaunchSlot > CONFIG_NUM_SLOTS) {
        set_profile_status_text("Ready. App start auto-load is disabled.");
        return;
    }
    if (!is_profile_slot_saved(g_app.configPath, appLaunchSlot)) {
        set_config_int(g_app.configPath, "profiles", "app_launch_slot", 0);
        set_profile_status_text("App start slot %d was empty and has been disabled.", appLaunchSlot);
        refresh_profile_controls_from_config();
        layout_bottom_buttons(g_app.hMainWnd);
        return;
    }
    DesiredSettings desired = {};
    char err[256] = {};
    if (!load_profile_from_config(g_app.configPath, appLaunchSlot, &desired, err, sizeof(err))) {
        set_config_int(g_app.configPath, "profiles", "app_launch_slot", 0);
        refresh_profile_controls_from_config();
        write_error_report_log_for_user_failure("App-start profile load failed", err);
        set_profile_status_text("App start load failed: %s", err[0] ? err : "unknown error");
        return;
    }
    if (!desired_settings_have_explicit_state(&desired, true, err, sizeof(err))) {
        set_config_int(g_app.configPath, "profiles", "app_launch_slot", 0);
        refresh_profile_controls_from_config();
        write_error_report_log_for_user_failure("App-start profile rejected", err);
        set_profile_status_text("App start slot %d was rejected: %s", appLaunchSlot, err);
        return;
    }
    char result[512] = {};
    bool ok = apply_desired_settings(&desired, false, result, sizeof(result));
    if (ok) {
        populate_desired_into_gui(&desired);
        set_config_int(g_app.configPath, "profiles", "selected_slot", appLaunchSlot);
        refresh_profile_controls_from_config();
        set_profile_status_text("Loaded slot %d into the GUI and applied it on app start.", appLaunchSlot);
    } else {
        char detail[128] = {};
        refresh_global_state(detail, sizeof(detail));
        populate_global_controls();
        if (g_app.loaded) populate_edits();
        invalidate_main_window();
        set_profile_status_text("App start apply for slot %d failed and the GUI was refreshed from live state: %s", appLaunchSlot, result);
    }
}

static bool ensure_profile_slot_available_for_auto_action(int slot) {
    if (slot <= 0) return true;
    if (is_profile_slot_saved(g_app.configPath, slot)) return true;
    set_profile_status_text("Slot %d is empty, so that automatic action was disabled.", slot);
    return false;
}

static void apply_logon_startup_behavior() {
    if (!g_app.launchedFromLogon) return;

    bool startProgramAtLogon = is_start_on_logon_enabled(g_app.configPath);
    g_app.startHiddenToTray = startProgramAtLogon;

    int logonSlot = get_config_int(g_app.configPath, "profiles", "logon_slot", 0);
    if (logonSlot < 0 || logonSlot > CONFIG_NUM_SLOTS) logonSlot = 0;
    if (logonSlot == 0) {
        set_profile_status_text(startProgramAtLogon
            ? "Started in the tray at Windows logon."
            : "Applied Windows logon startup rules without opening the tray app.");
        return;
    }

    if (!ensure_profile_slot_available_for_auto_action(logonSlot)) {
        set_config_int(g_app.configPath, "profiles", "logon_slot", 0);
        refresh_profile_controls_from_config();
        return;
    }

    DesiredSettings desired = {};
    char err[256] = {};
    if (!load_profile_from_config(g_app.configPath, logonSlot, &desired, err, sizeof(err))) {
        set_config_int(g_app.configPath, "profiles", "logon_slot", 0);
        refresh_profile_controls_from_config();
        write_error_report_log_for_user_failure("Logon profile load failed", err);
        set_profile_status_text("Logon apply failed for slot %d: %s", logonSlot, err[0] ? err : "unknown error");
        return;
    }
    if (!desired_settings_have_explicit_state(&desired, true, err, sizeof(err))) {
        set_config_int(g_app.configPath, "profiles", "logon_slot", 0);
        refresh_profile_controls_from_config();
        write_error_report_log_for_user_failure("Logon profile rejected", err);
        set_profile_status_text("Logon slot %d was rejected: %s", logonSlot, err);
        return;
    }

    char result[512] = {};
    bool ok = apply_desired_settings(&desired, false, result, sizeof(result));
    if (ok) {
        populate_desired_into_gui(&desired);
        set_config_int(g_app.configPath, "profiles", "selected_slot", logonSlot);
        refresh_profile_controls_from_config();
        set_profile_status_text(startProgramAtLogon
            ? "Started in the tray and applied slot %d at Windows logon."
            : "Applied slot %d silently at Windows logon.",
            logonSlot);
    } else {
        char detail[128] = {};
        refresh_global_state(detail, sizeof(detail));
        populate_global_controls();
        if (g_app.loaded) populate_edits();
        invalidate_main_window();
        set_profile_status_text(startProgramAtLogon
            ? "Started in the tray, but slot %d apply failed and live state was reloaded: %s"
            : "Silent Windows logon apply for slot %d failed and live state was reloaded: %s",
            logonSlot, result);
    }
}

