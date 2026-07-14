    bool lockTracksAnchor = g_app.guiLockTracksAnchor;
    if (hasLock) {
        lockCi = g_app.visibleMap[g_app.lockedVi];
        currentLockMHz = displayed_curve_mhz(g_app.curve[lockCi].freq_kHz);
        effectiveLockTargetMHz = (int)g_app.lockedFreq;
        {
            char lockBuf[32] = {};
            get_window_text_safe(g_app.hEditsMhz[g_app.lockedVi], lockBuf, sizeof(lockBuf));
            if (lockBuf[0]) {
                int parsed = 0;
                if (!parse_int_strict(lockBuf, &parsed) || parsed <= 0) {
                    set_message(err, errSize, "Invalid MHz value for point %d", lockCi);
                    return false;
                }
                effectiveLockTargetMHz = parsed;
            } else if (effectiveLockTargetMHz <= 0 && captureAllCurvePoints) {
                effectiveLockTargetMHz = (int)currentLockMHz;
            }
            if (effectiveLockTargetMHz <= 0) {
                set_message(err, errSize, "Invalid or missing MHz value for point %d", lockCi);
                return false;
            }
        }
        int currentLockGpuOffsetMHz = gpu_offset_component_mhz_for_point(lockCi, currentGpuOffsetMHz, currentActiveGpuOffsetExcludeLowCount);
        int desiredLockGpuOffsetMHz = gpu_offset_component_mhz_for_point(lockCi, gpuOffsetMHz, desiredActiveGpuOffsetExcludeLowCount);
        if (lockTracksAnchor && desiredLockGpuOffsetMHz != currentLockGpuOffsetMHz) {
            debug_log("capture lock: anchor tracks offset: base=%u + desiredOff=%d - currentOff=%d -> %u\n",
                effectiveLockTargetMHz, desiredLockGpuOffsetMHz, currentLockGpuOffsetMHz,
                (unsigned int)(effectiveLockTargetMHz + desiredLockGpuOffsetMHz - currentLockGpuOffsetMHz));
            effectiveLockTargetMHz += desiredLockGpuOffsetMHz - currentLockGpuOffsetMHz;
            if (effectiveLockTargetMHz <= 0) effectiveLockTargetMHz = 1;
        } else {
            debug_log("capture lock: tracksAnchor=%d desiredOff=%d currentOff=%d lockMHz=%u\n",
                lockTracksAnchor ? 1 : 0, desiredLockGpuOffsetMHz, currentLockGpuOffsetMHz, g_app.lockedFreq);
        }
        desired->hasLock = true;
        desired->lockCi = lockCi;
        desired->lockMHz = (unsigned int)effectiveLockTargetMHz;
        desired->lockMode = g_app.lockMode;
        desired->lockTracksAnchor = lockTracksAnchor;
    }

    for (int vi = 0; vi < g_app.numVisible; vi++) {
        int ci = g_app.visibleMap[vi];
        unsigned int currentMHz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
        int mhz = 0;
        if (hasLock && expandLockedTail && vi >= g_app.lockedVi) {
            mhz = effectiveLockTargetMHz;
        } else if (hasLock && vi > g_app.lockedVi) {
            continue;
        } else {
            char pointBuf[32] = {};
            get_window_text_safe(g_app.hEditsMhz[vi], pointBuf, sizeof(pointBuf));
            if (!pointBuf[0] && captureAllCurvePoints) {
                mhz = (int)currentMHz;
            } else if (!parse_int_strict(pointBuf, &mhz) || mhz <= 0) {
                set_message(err, errSize, "Invalid MHz value for point %d", ci);
                return false;
            }
        }
        parsedCurveMHz[ci] = mhz;
        parsedCurveHave[ci] = true;
        bool userExplicit = g_app.guiCurvePointExplicit[ci];
        (void)userExplicit;
    }

    int previousRequestedCurveMHz = 0;
    int previousRequestedCurveCi = -1;

    // If any pre-tail point was explicitly set from a profile load, don't
    // infer pre-tail points from live GPU offsets (would leak previous profile).
    bool inferPreTailFromGpu = hasLock;
    if (hasLock) {
        inferPreTailFromGpu = true;
        for (int pi = 0; pi < g_app.lockedVi && inferPreTailFromGpu; pi++) {
            int pci = g_app.visibleMap[pi];
            if (g_app.guiCurvePointExplicit[pci]) inferPreTailFromGpu = false;
        }
    }
    for (int vi = 0; vi < g_app.numVisible; vi++) {
        int ci = g_app.visibleMap[vi];
        if (!parsedCurveHave[ci]) continue;
        bool lockTailPoint = hasLock && expandLockedTail && vi >= g_app.lockedVi;
        int mhz = lockTailPoint ? effectiveLockTargetMHz : parsedCurveMHz[ci];
        unsigned int currentMHz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
        int effectiveMHz = mhz;
        bool preTailInferred = inferPreTailFromGpu && !lockTailPoint && vi < g_app.lockedVi
            && g_app.freqOffsets[ci] != 0;
        bool explicitPoint = captureAllCurvePoints || g_app.guiCurvePointExplicit[ci]
            || (hasLock && ci == lockCi && !lockTracksAnchor) || preTailInferred;
        if (previousRequestedCurveCi >= 0 && effectiveMHz < previousRequestedCurveMHz) {
            if (lockTailPoint) {
                effectiveMHz = previousRequestedCurveMHz;
                if (lockTailPoint) {
                    effectiveLockTargetMHz = effectiveMHz;
                }
            } else {
                set_message(err, errSize,
                    "Curve point %d (%d MHz) is below point %d (%d MHz). The VF curve must remain non-decreasing.",
                    ci, effectiveMHz, previousRequestedCurveCi, previousRequestedCurveMHz);
                return false;
            }
        }
        previousRequestedCurveMHz = effectiveMHz;
        previousRequestedCurveCi = ci;
        if (lockTailPoint || explicitPoint || (captureAllCurvePoints && (unsigned int)effectiveMHz != currentMHz)) {
            desired->hasCurvePoint[ci] = true;
            desired->curvePointMHz[ci] = (unsigned int)effectiveMHz;
        }
    }

    if (!desired->hasLock && g_app.guiHasUserModifiedValues) {
        int inferredLockCi = -1;
        unsigned int inferredLockMHz = 0;
        infer_profile_lock_from_curve(desired, &inferredLockCi, &inferredLockMHz);
        if (inferredLockCi >= 0 && inferredLockMHz > 0) {
            desired->hasLock = true;
            desired->lockCi = inferredLockCi;
            desired->lockMHz = inferredLockMHz;
            desired->lockTracksAnchor = true;
            debug_log("capture_gui_desired_settings: inferred lock ci=%d mhz=%u from loaded flat tail\n",
                inferredLockCi,
                inferredLockMHz);
        }
    }

    if (includeCurrentGlobals || forceExplicitGlobals || gpuOffsetMHz != currentGpuOffsetMHz || gpuOffsetExcludeLowCount != currentGpuOffsetExcludeLowCount) {
        desired->hasGpuOffset = true;
        desired->gpuOffsetMHz = gpuOffsetMHz;
        desired->gpuOffsetExcludeLowCount = gpuOffsetExcludeLowCount;
    }

    int currentMemOffsetMHz = haveControlState && control.hasMemOffset ? control.memOffsetMHz : mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz);
    get_window_text_safe(g_app.hMemOffsetEdit, buf, sizeof(buf));
    int memOffsetMHz = currentMemOffsetMHz;
    if (buf[0]) {
        if (!parse_int_strict(buf, &memOffsetMHz)) {
            set_message(err, errSize, "Invalid memory offset");
            return false;
        }
    }
    if (includeCurrentGlobals || forceExplicitGlobals || memOffsetMHz != currentMemOffsetMHz) {
        desired->hasMemOffset = true;
        desired->memOffsetMHz = memOffsetMHz;
    }

    int currentPowerLimitPct = haveControlState && control.hasPowerLimit ? control.powerLimitPct : g_app.powerLimitPct;
    get_window_text_safe(g_app.hPowerLimitEdit, buf, sizeof(buf));
    int powerLimitPct = currentPowerLimitPct;
    if (buf[0]) {
        if (!parse_int_strict(buf, &powerLimitPct)) {
            set_message(err, errSize, "Invalid power limit");
            return false;
        }
    }
    if (includeCurrentGlobals || forceExplicitGlobals || powerLimitPct != currentPowerLimitPct) {
        desired->hasPowerLimit = true;
        desired->powerLimitPct = powerLimitPct;
    }

    int selectedFanMode = g_app.guiFanMode;
    if (g_app.hFanModeCombo) {
        LRESULT selection = SendMessageA(g_app.hFanModeCombo, CB_GETCURSEL, 0, 0);
        if (selection >= 0 && selection <= FAN_MODE_CURVE) {
            selectedFanMode = (int)selection;
        }
    }
    get_window_text_safe(g_app.hFanEdit, buf, sizeof(buf));
    int fanPercent = g_app.guiFanFixedPercent;
    if (selectedFanMode == FAN_MODE_FIXED) {
        if (!parse_int_strict(buf, &fanPercent)) {
            set_message(err, errSize, "Invalid fixed fan percentage");
            return false;
        }
        fanPercent = clamp_percent(fanPercent);
    } else if (selectedFanMode == FAN_MODE_AUTO) {
        fanPercent = 0;
    }

    FanCurveConfig guiCurve = g_app.guiFanCurve;
    fan_curve_normalize(&guiCurve);
    if (!fan_curve_validate(&guiCurve, err, errSize)) {
        return false;
    }

    if (includeCurrentGlobals || forceExplicitGlobals || !fan_setting_matches_current(selectedFanMode, fanPercent, &guiCurve)) {
        desired->hasFan = true;
        desired->fanMode = selectedFanMode;
        desired->fanAuto = selectedFanMode == FAN_MODE_AUTO;
        desired->fanPercent = fanPercent;
        copy_fan_curve(&desired->fanCurve, &guiCurve);
    }

    char capturedCurvePoints[256] = {};
    build_point_list_from_flags(desired->hasCurvePoint, capturedCurvePoints, sizeof(capturedCurvePoints));
    debug_log("capture_gui_desired_settings: serviceMode=%d includeCurrent=%d forceExplicit=%d hasGpu=%d gpu=%d exclude=%d hasMem=%d mem=%d hasPower=%d power=%d hasFan=%d fanMode=%d fanPct=%d hasLock=%d lockCi=%d lockMHz=%u curvePoints=%d (%s)\n",
        g_app.usingBackgroundService ? 1 : 0,
        includeCurrentGlobals ? 1 : 0,
        forceExplicitGlobals ? 1 : 0,
        desired->hasGpuOffset ? 1 : 0,
        desired->gpuOffsetMHz,
        desired->gpuOffsetExcludeLowCount,
        desired->hasMemOffset ? 1 : 0,
        desired->memOffsetMHz,
        desired->hasPowerLimit ? 1 : 0,
        desired->powerLimitPct,
        desired->hasFan ? 1 : 0,
        desired->fanMode,
        desired->fanPercent,
        desired->hasLock ? 1 : 0,
        desired->hasLock ? desired->lockCi : -1,
        desired->hasLock ? desired->lockMHz : 0u,
        desired_curve_point_count(desired),
        capturedCurvePoints);

    return true;
}

static bool save_desired_to_config(const char* path, const DesiredSettings* desired, bool useCurrentForUnset, char* err, size_t errSize) {
    return save_desired_to_config_with_startup(path, desired, useCurrentForUnset, CONFIG_STARTUP_PRESERVE, err, errSize);
}

static bool save_current_gui_state_to_config(int startupState, char* err, size_t errSize) {
    DesiredSettings desired = {};
    if (!capture_gui_config_settings(&desired, err, errSize)) return false;
    return save_desired_to_config_with_startup(g_app.configPath, &desired, false, startupState, err, errSize);
}

static bool save_current_gui_state_for_startup(char* err, size_t errSize) {
    return save_current_gui_state_to_config(CONFIG_STARTUP_ENABLE, err, errSize);
}

static bool desired_requires_resident_runtime(const DesiredSettings* desired) {
    return desired && desired->hasFan && desired->fanMode != FAN_MODE_AUTO;
}

static bool logon_profile_requires_resident_runtime(const char* path) {
    if (!path || !*path) return false;

    int logonSlot = get_config_int(path, "profiles", "logon_slot", 0);
    if (logonSlot < 1 || logonSlot > CONFIG_NUM_SLOTS) return false;

    DesiredSettings desired = {};
    char err[256] = {};
    if (!load_profile_from_config(path, logonSlot, &desired, err, sizeof(err))) {
        return false;
    }
    return desired_requires_resident_runtime(&desired);
}

static bool parse_wide_int_arg(LPWSTR text, int* out) {
    if (!text || !out) return false;
    char buf[64] = {};
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, buf, (int)sizeof(buf), nullptr, nullptr);
    if (n <= 0) return false;
    trim_ascii(buf);
    return parse_int_strict(buf, out);
}

static bool copy_wide_to_utf8(LPWSTR text, char* out, int outSize) {
    if (!text || !out || outSize < 1) return false;
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, out, outSize, nullptr, nullptr);
    if (n <= 0) return false;
    trim_ascii(out);
    return true;
}

static bool copy_wide_to_ansi(LPWSTR text, char* out, int outSize) {
    if (!text || !out || outSize < 1) return false;
    int n = WideCharToMultiByte(CP_ACP, 0, text, -1, out, outSize, nullptr, nullptr);
    if (n <= 0) return false;
    trim_ascii(out);
    return true;
}

static bool utf8_to_wide(const char* text, WCHAR* out, int outCount) {
    if (!text || !out || outCount < 1) return false;
    int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, out, outCount);
    if (n <= 0) return false;
    out[outCount - 1] = 0;
    return true;
}

static bool get_current_user_sam_name(WCHAR* out, DWORD outCount) {
    if (!out || outCount == 0) return false;
    out[0] = 0;

    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0) {
        CloseHandle(token);
        return false;
    }

    TOKEN_USER* user = (TOKEN_USER*)malloc(needed);
    if (!user) {
        CloseHandle(token);
        return false;
    }

    WCHAR name[256] = {};
    WCHAR domain[256] = {};
    DWORD nameLen = ARRAY_COUNT(name);
    DWORD domainLen = ARRAY_COUNT(domain);
    SID_NAME_USE use = SidTypeUnknown;
    bool ok = false;
    if (GetTokenInformation(token, TokenUser, user, needed, &needed) &&
        LookupAccountSidW(nullptr, user->User.Sid, name, &nameLen, domain, &domainLen, &use)) {
        if (domain[0]) ok = SUCCEEDED(StringCchPrintfW(out, outCount, L"%ls\\%ls", domain, name));
        else ok = SUCCEEDED(StringCchCopyW(out, outCount, name));
    }

    free(user);
    CloseHandle(token);
    return ok;
}

#include "main_runtime_capture.cpp"

