static bool show_best_guess_support_warning(HWND parent) {
    if (!should_show_best_guess_warning()) return true;

    char message[768] = {};
    StringCchPrintfA(message, ARRAY_COUNT(message),
        "Detected an unrecognized NVIDIA GPU family (%s, %s).\n\n"
        "Green Curve will allow best-effort support for a new NVIDIA GPU family using the fallback VF backend layout. Writes stay enabled, but this exact architecture has not been tested yet.\n\n"
        "Check applied clocks and offsets carefully after changes.\n\n"
        "Yes continues this time. No continues and disables this warning. Cancel exits.",
        gpu_family_name(g_app.gpuFamily),
        g_app.gpuName[0] ? g_app.gpuName : "NVIDIA GPU");

    bool handled = false;
    bool suppressWarning = false;
    HMODULE comctl = load_system_library_a("comctl32.dll");
    if (comctl) {
        typedef HRESULT (WINAPI *TaskDialogIndirect_t)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
        auto taskDialogIndirect = (TaskDialogIndirect_t)GetProcAddress(comctl, "TaskDialogIndirect");
        if (taskDialogIndirect) {
            TASKDIALOGCONFIG config = {};
            config.cbSize = sizeof(config);
            config.hwndParent = parent;
            config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
            config.dwCommonButtons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
            config.pszWindowTitle = L"Green Curve - Unrecognized GPU Support";
            config.pszMainIcon = TD_WARNING_ICON;
            config.pszVerificationText = L"Do not show this warning again for unrecognized GPUs";

            WCHAR mainInstruction[128] = {};
            StringCchPrintfW(mainInstruction, ARRAY_COUNT(mainInstruction), L"Best-effort support for an unrecognized GPU");
            config.pszMainInstruction = mainInstruction;

            WCHAR content[2048] = {};
            StringCchPrintfW(content, ARRAY_COUNT(content),
                L"Detected an unrecognized NVIDIA GPU family (%hs, %hs).\n\n"
                L"Green Curve will allow best-effort support for a new NVIDIA GPU family using the fallback VF backend layout. Writes stay enabled, but this exact architecture has not been tested yet.\n\n"
                L"Check applied clocks and offsets carefully after changes.",
                gpu_family_name(g_app.gpuFamily),
                g_app.gpuName[0] ? g_app.gpuName : "NVIDIA GPU");
            config.pszContent = content;

            int button = 0;
            BOOL verificationChecked = FALSE;
            HRESULT hr = taskDialogIndirect(&config, &button, nullptr, &verificationChecked);
            if (SUCCEEDED(hr)) {
                handled = true;
                if (button == IDCANCEL) {
                    remove_tray_icon();
                    release_single_instance_mutex();
                    return false;
                }
                suppressWarning = verificationChecked != FALSE;
            }
        }
        FreeLibrary(comctl);
    }

    if (!handled) {
        int result = MessageBoxA(parent, message, "Green Curve - Unrecognized GPU Support", MB_YESNOCANCEL | MB_ICONWARNING);
        if (result == IDCANCEL || result == 0) {
            remove_tray_icon();
            release_single_instance_mutex();
            return false;
        }
        suppressWarning = result == IDNO;
    }

    if (suppressWarning) {
        if (set_config_int(g_app.configPath, "warnings", "hide_unrecognized_gpu_warning", 1)) {
            debug_log("unrecognized GPU warning disabled by user\n");
        } else {
            debug_log("unrecognized GPU warning: failed to persist suppression flag\n");
        }
    }

    g_bestGuessWarningShownThisSession = true;
    return true;
}

static bool vf_curve_global_gpu_offset_supported() {
    const VfBackendSpec* backend = g_app.vfBackend;
    if (!backend || !backend->writeSupported) {
        debug_log_on_change("vf_curve_global_gpu_offset_supported: no backend or not writable\n");
        return false;
    }
    bool supported = backend->family == GPU_FAMILY_BLACKWELL || backend->family == GPU_FAMILY_UNKNOWN;
    debug_log_on_change("vf_curve_global_gpu_offset_supported: family=%d supported=%d\n", backend->family, supported ? 1 : 0);
    return supported;
}

static bool is_locked_tail_detection_point(int pointIndex) {
    return g_app.lockedCi >= 0 && pointIndex >= g_app.lockedCi;
}

static bool detect_live_selective_gpu_offset_state(int* gpuOffsetMHzOut, int* representativeOffsetkHzOut, int* detectedExcludeLowCountOut) {
    if (!vf_curve_global_gpu_offset_supported()) {
        if (gpuOffsetMHzOut) *gpuOffsetMHzOut = 0;
        if (representativeOffsetkHzOut) *representativeOffsetkHzOut = 0;
        if (detectedExcludeLowCountOut) *detectedExcludeLowCountOut = 0;
        return false;
    }

    static const int TOLERANCE_MHZ = 30;

    int candidateOffsets[VF_NUM_POINTS] = {};
    int candidateCount = 0;
    int numPopulated = 0;

    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (g_app.curve[ci].freq_kHz == 0) continue;
        numPopulated++;
        int offsetMHz = g_app.freqOffsets[ci] / 1000;
        if (offsetMHz == 0) continue;

        bool seen = false;
        for (int i = 0; i < candidateCount; i++) {
            if (abs(candidateOffsets[i] - offsetMHz) <= TOLERANCE_MHZ) {
                seen = true;
                break;
            }
        }
        if (!seen && candidateCount < VF_NUM_POINTS) {
            candidateOffsets[candidateCount++] = offsetMHz;
        }
    }

    // Track the best match across all exclude counts.
    // Best = highest includedMatchCount, then lowest excludedViolations.
    int bestDetectedMHz = 0;
    int bestMatchedAverageKHz = 0;
    int bestExcludeLowCount = 0;
    int bestMatchCount = 0;
    int bestViolations = INT_MAX;

    for (int excludeTry = 1; excludeTry <= numPopulated; excludeTry++) {
        for (int candidateIndex = 0; candidateIndex < candidateCount; candidateIndex++) {
            int candidateMHz = candidateOffsets[candidateIndex];
            int toleranceKHz = TOLERANCE_MHZ * 1000;
            bool sawExcludedPoint = false;
            bool sawIncludedPoint = false;
            bool skippedLockedTail = false;
            int includedMatchSumKHz = 0;
            int includedMatchCount = 0;
            int includedConsideredCount = 0;
            int excludedViolations = 0;
            int excludedTotal = 0;
            int includedZeroGaps = 0;
            int candidateTargetKHz = candidateMHz * 1000;
            int firstIncludedMatchCi = -1;
            int lastIncludedMatchCi = -1;

            for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                if (g_app.curve[ci].freq_kHz == 0) continue;

                if (is_locked_tail_detection_point(ci)) {
                    skippedLockedTail = true;
                    continue;
                }

                bool excluded = is_gpu_offset_excluded_low_point(ci, candidateMHz, excludeTry);
                int actualOffsetKHz = g_app.freqOffsets[ci];
                if (excluded) {
                    sawExcludedPoint = true;
                    excludedTotal++;
                    if (abs(actualOffsetKHz) > toleranceKHz) {
                        excludedViolations++;
                    }
                } else {
                    sawIncludedPoint = true;
                    includedConsideredCount++;
                    if (abs(actualOffsetKHz) <= toleranceKHz && abs(candidateTargetKHz) > toleranceKHz) {
                        includedZeroGaps++;
                    } else if (abs(actualOffsetKHz - candidateTargetKHz) <= toleranceKHz) {
                        includedMatchSumKHz += actualOffsetKHz;
                        includedMatchCount++;
                        if (firstIncludedMatchCi < 0) firstIncludedMatchCi = ci;
                        lastIncludedMatchCi = ci;
                    }
                }
            }

            if (!sawIncludedPoint || !sawExcludedPoint) continue;
            if (includedMatchCount == 0) continue;

            int minimumMatches = skippedLockedTail ? 4 : 3;
            if (includedMatchCount < minimumMatches) continue;
            if (skippedLockedTail && firstIncludedMatchCi >= 0 && lastIncludedMatchCi - firstIncludedMatchCi < 4) continue;
            if (includedMatchCount * 2 < includedConsideredCount) continue;
            if (includedZeroGaps > 0) continue;
            if (excludedTotal > 0 && excludedViolations * 3 > excludedTotal) continue;

            int matchedAverageKHz = includedMatchSumKHz / includedMatchCount;
            int detectedMHz = (matchedAverageKHz >= 0 ? (matchedAverageKHz + 500) : (matchedAverageKHz - 500)) / 1000;
            if (detectedMHz == 0) continue;
            // Reject detected offsets at or below the tolerance threshold
            // (30 MHz). These are not meaningful selective offsets — they
            // typically arise from residual noise (e.g. 1-10 MHz offsets
            // left by correction loop writes on non-tail points after a
            // profile transitions away from a selective offset). A genuine
            // user-set selective offset is always significantly larger.
            if (abs(detectedMHz) <= TOLERANCE_MHZ) continue;

            // Prefer more matches; on tie, prefer fewer excluded violations.
            bool isBetter = (includedMatchCount > bestMatchCount)
                || (includedMatchCount == bestMatchCount && excludedViolations < bestViolations);
            if (isBetter) {
                bestDetectedMHz = detectedMHz;
                bestMatchedAverageKHz = matchedAverageKHz;
                bestExcludeLowCount = excludeTry;
                bestMatchCount = includedMatchCount;
                bestViolations = excludedViolations;
                debug_log("detect_live_selective: exclude=%d candidate=%d MHz -> detected=%d MHz (matches=%d/%d, exclViolations=%d/%d, skippedTail=%d)  [new best]\n",
                    excludeTry, candidateMHz, detectedMHz,
                    includedMatchCount, includedConsideredCount,
                    excludedViolations, excludedTotal,
                    skippedLockedTail ? 1 : 0);
            } else {
                debug_log("detect_live_selective: exclude=%d candidate=%d MHz -> detected=%d MHz (matches=%d/%d, exclViolations=%d/%d, skippedTail=%d)\n",
                    excludeTry, candidateMHz, detectedMHz,
                    includedMatchCount, includedConsideredCount,
                    excludedViolations, excludedTotal,
                    skippedLockedTail ? 1 : 0);
            }
        }
    }

    if (bestExcludeLowCount > 0) {
        debug_log("detect_live_selective: BEST exclude=%d MHz=%d matches=%d violations=%d (total candidates=%d bestMatchCount=%d)\n",
            bestExcludeLowCount, bestDetectedMHz, bestMatchCount, bestViolations, candidateCount, bestMatchCount);
        if (gpuOffsetMHzOut) *gpuOffsetMHzOut = bestDetectedMHz;
        if (representativeOffsetkHzOut) *representativeOffsetkHzOut = bestMatchedAverageKHz;
        if (detectedExcludeLowCountOut) *detectedExcludeLowCountOut = bestExcludeLowCount;
        return true;
    }

    if (candidateCount > 0) {
        debug_log("detect_live_selective: no selective offset pattern found (candidates=%d, populated=%d) - candidates rejected:",
            candidateCount, numPopulated);
        for (int i = 0; i < candidateCount && i < 5; i++) {
            debug_log(" candidate[%d]=%d MHz", i, candidateOffsets[i]);
        }
        debug_log("\n");
    } else {
        debug_log("detect_live_selective: no selective offset pattern found (candidates=%d, populated=%d)\n", candidateCount, numPopulated);
    }
    if (gpuOffsetMHzOut) *gpuOffsetMHzOut = 0;
    if (representativeOffsetkHzOut) *representativeOffsetkHzOut = 0;
    if (detectedExcludeLowCountOut) *detectedExcludeLowCountOut = 0;
    return false;
}

static bool live_selective_gpu_offset_matches_requested_state_with_tolerance(int gpuOffsetMHz, int toleranceMHz) {
    if (!vf_curve_global_gpu_offset_supported() || gpuOffsetMHz == 0) return false;
    if (toleranceMHz < 0) toleranceMHz = 0;

    int detectedSelectiveOffsetMHz = 0;
    int representativeOffsetkHz = 0;
    int detectedExclude = 0;
    if (!detect_live_selective_gpu_offset_state(&detectedSelectiveOffsetMHz, &representativeOffsetkHz, &detectedExclude)) return false;

    int tolerancekHz = toleranceMHz * 1000;
    int requestedOffsetkHz = gpuOffsetMHz * 1000;
    return abs(representativeOffsetkHz - requestedOffsetkHz) <= tolerancekHz;
}

static bool live_selective_gpu_offset_matches_requested_shape(int gpuOffsetMHz, int excludeLowCount, int toleranceMHz, int* representativeOffsetkHzOut) {
    if (representativeOffsetkHzOut) *representativeOffsetkHzOut = 0;
    if (!vf_curve_global_gpu_offset_supported() || gpuOffsetMHz == 0 || excludeLowCount <= 0) return false;
    if (toleranceMHz < 0) toleranceMHz = 0;

    int toleranceKHz = toleranceMHz * 1000;
    int targetOffsetKHz = gpuOffsetMHz * 1000;
    bool sawExcludedPoint = false;
    bool sawIncludedPoint = false;
    bool skippedLockedTail = false;
    int includedMatchSumKHz = 0;
    int includedMatchCount = 0;
    int includedConsideredCount = 0;
    int excludedViolations = 0;
    int excludedTotal = 0;
    int includedZeroGaps = 0;

    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (g_app.curve[ci].freq_kHz == 0) continue;

        if (is_locked_tail_detection_point(ci)) {
            skippedLockedTail = true;
            continue;
        }

        bool excluded = is_gpu_offset_excluded_low_point(ci, gpuOffsetMHz, excludeLowCount);
        int actualOffsetKHz = g_app.freqOffsets[ci];
        if (excluded) {
            sawExcludedPoint = true;
            excludedTotal++;
            if (abs(actualOffsetKHz) > toleranceKHz) excludedViolations++;
        } else {
            sawIncludedPoint = true;
            includedConsideredCount++;
            if (abs(actualOffsetKHz) <= toleranceKHz && abs(targetOffsetKHz) > toleranceKHz) {
                includedZeroGaps++;
            } else if (abs(actualOffsetKHz - targetOffsetKHz) <= toleranceKHz) {
                includedMatchSumKHz += actualOffsetKHz;
                includedMatchCount++;
            }
        }
    }

    if (!sawIncludedPoint || !sawExcludedPoint) return false;
    int minimumMatches = skippedLockedTail ? 2 : 3;
    if (includedMatchCount < minimumMatches) return false;
    if (includedMatchCount * 2 < includedConsideredCount) return false;
    if (includedZeroGaps > 0) return false;
    if (excludedTotal > 0 && excludedViolations * 3 > excludedTotal) return false;

    if (representativeOffsetkHzOut && includedMatchCount > 0) {
        *representativeOffsetkHzOut = includedMatchSumKHz / includedMatchCount;
    }
    return true;
}

static bool load_runtime_selective_gpu_offset_request(int* gpuOffsetMHzOut, int* excludeLowCountOut) {
    if (gpuOffsetMHzOut) *gpuOffsetMHzOut = 0;
    if (excludeLowCountOut) *excludeLowCountOut = 0;
    if (!g_app.configPath[0]) return false;

    char buf[32] = {};
    HANDLE configMutex = nullptr;
    if (!enter_config_storage_lock(&configMutex)) return false;
    GetPrivateProfileStringA("runtime", "selective_gpu_offset_mhz", "", buf, sizeof(buf), g_app.configPath);
    trim_ascii(buf);
    if (!buf[0]) {
        leave_config_storage_lock(configMutex);
        return false;
    }

    int gpuOffsetMHz = 0;
    if (!parse_int_strict(buf, &gpuOffsetMHz)) {
        leave_config_storage_lock(configMutex);
        return false;
    }
    // Reject values that would overflow when multiplied by 1000 for kHz conversion.
    // The IPC validation clamps to ±1000, but persisted config values bypass it.
    if (gpuOffsetMHz < -1000000 || gpuOffsetMHz > 1000000) {
        debug_log("load_runtime_selective: rejecting out-of-range gpuOffsetMHz=%d\n", gpuOffsetMHz);
        leave_config_storage_lock(configMutex);
        return false;
    }

    // Read both values under the same mutex to avoid TOCTOU race.
    // The exclude count may be stored as "selective_gpu_offset_exclude_low_count"
    // (new format) or "selective_gpu_offset_exclude_low_70" (legacy).
    char excludeBuf[32] = {};
    GetPrivateProfileStringA("runtime", "selective_gpu_offset_exclude_low_count", "", excludeBuf, sizeof(excludeBuf), g_app.configPath);
    bool hasExplicitLowCount = excludeBuf[0] != '\0';
    if (!hasExplicitLowCount) {
        GetPrivateProfileStringA("runtime", "selective_gpu_offset_exclude_low_70", "", excludeBuf, sizeof(excludeBuf), g_app.configPath);
    }
    leave_config_storage_lock(configMutex);

    int excludeLowCount = 0;
    trim_ascii(excludeBuf);
    if (excludeBuf[0]) {
        int value = 0;
        if (!parse_int_strict(excludeBuf, &value)) return false;
        if (hasExplicitLowCount) {
            excludeLowCount = value;
        } else {
            excludeLowCount = value ? 70 : 0;
        }
    }

    if (gpuOffsetMHzOut) *gpuOffsetMHzOut = gpuOffsetMHz;
    if (excludeLowCountOut) *excludeLowCountOut = excludeLowCount;
    return true;
}

static bool desired_settings_has_explicit_curve(const DesiredSettings* desired) {
    if (!desired) return false;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (desired->hasCurvePoint[i]) return true;
    }
    return false;
}

static bool selective_gpu_offset_curve_shape_looks_safe(const DesiredSettings* desired, int gpuOffsetMHz, int excludeLowCount) {
    if (!desired) return false;
    if (gpuOffsetMHz == 0 || excludeLowCount <= 0) return true;
    if (!desired_settings_has_explicit_curve(desired)) return true;

    bool sawExcludedPoint = false;
    bool sawIncludedPoint = false;
    int firstIncludedCi = -1;
    unsigned int previousMHz = 0;
    bool havePreviousMHz = false;

    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (!desired->hasCurvePoint[ci]) continue;
        unsigned int mhz = desired->curvePointMHz[ci];
        if (mhz == 0) continue;

        bool excluded = is_gpu_offset_excluded_low_point(ci, gpuOffsetMHz, excludeLowCount);
        if (excluded) {
            sawExcludedPoint = true;
            if (firstIncludedCi >= 0) return false;
        } else {
            sawIncludedPoint = true;
            if (firstIncludedCi < 0) firstIncludedCi = ci;
        }

        if (havePreviousMHz && mhz < previousMHz) return false;
        previousMHz = mhz;
        havePreviousMHz = true;
    }

    if (!sawIncludedPoint) return false;
    if (!sawExcludedPoint) return true;
    return firstIncludedCi >= excludeLowCount;
}

static bool control_state_has_meaningful_gpu(const ControlState* state) {
    return state && state->valid && state->hasGpuOffset;
}

static bool control_state_has_meaningful_mem(const ControlState* state) {
    return state && state->valid && state->hasMemOffset;
}

static bool control_state_has_meaningful_power(const ControlState* state) {
    return state && state->valid && state->hasPowerLimit;
}

static bool control_state_has_meaningful_fan(const ControlState* state) {
    return state && state->valid && state->hasFan;
}

static bool control_state_has_any_meaningful_value(const ControlState* state) {
    return control_state_has_meaningful_gpu(state)
        || control_state_has_meaningful_mem(state)
        || control_state_has_meaningful_power(state)
        || control_state_has_meaningful_fan(state);
}

static void set_gui_state_dirty(bool dirty) {
    if (g_debug_logging && g_app.guiStateDirty != dirty) {
        debug_log("set_gui_state_dirty: %d -> %d (programmaticDepth=%d)\n",
            g_app.guiStateDirty ? 1 : 0,
            dirty ? 1 : 0,
            g_programmaticEditUpdateDepth);
    }
    g_app.guiStateDirty = dirty;
}

static bool gui_state_dirty() {
    return g_app.guiStateDirty;
}

static bool should_accept_service_curve_lock_detection() {
    if (!g_app.usingBackgroundService) return true;
    if (gui_state_dirty()) return false;
    if (g_app.lockedCi >= 0 && g_app.lockedCi < VF_NUM_POINTS && g_app.lockedFreq > 0) return false;
    return true;
}

static bool should_auto_detect_locked_tail_from_live_curve() {
    if (g_app.isServiceProcess
        && g_serviceHasActiveDesired
        && g_serviceActiveDesired.hasLock
        && g_serviceActiveDesired.lockCi >= 0
        && g_serviceActiveDesired.lockMHz > 0) {
        return false;
    }
    if (g_app.usingBackgroundService) {
        return should_accept_service_curve_lock_detection();
    }
    return true;
}

static void persist_runtime_selective_gpu_offset_request(int gpuOffsetMHz, int excludeLowCount) {
    if (!g_app.configPath[0]) return;
    if (gpuOffsetMHz == 0 || excludeLowCount <= 0) {
        debug_log("persist_runtime_selective_gpu_offset_request: non-selective request clears runtime state (%d MHz excludeLowCount=%d)\n",
            gpuOffsetMHz,
            excludeLowCount);
        clear_runtime_selective_gpu_offset_request();
        return;
    }
    HANDLE configMutex = nullptr;
    if (!enter_config_storage_lock(&configMutex)) return;
    debug_log("persist_runtime_selective_gpu_offset_request: storing %d MHz excludeLowCount=%d\n",
        gpuOffsetMHz,
        excludeLowCount);
    char buf[32] = {};
    char countBuf[32] = {};
    StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", gpuOffsetMHz);
    StringCchPrintfA(countBuf, ARRAY_COUNT(countBuf), "%d", excludeLowCount);
    char section[128] = {};
    StringCchPrintfA(section, ARRAY_COUNT(section),
        "selective_gpu_offset_mhz=%s%cselective_gpu_offset_exclude_low_count=%s%c%c",
        buf,
        '\0',
        countBuf,
        '\0',
        '\0');
    WritePrivateProfileSectionA("runtime", section, g_app.configPath);
    leave_config_storage_lock(configMutex);
}

static void clear_runtime_selective_gpu_offset_request() {
    if (!g_app.configPath[0]) return;
    HANDLE configMutex = nullptr;
    if (!enter_config_storage_lock(&configMutex)) return;
    debug_log("clear_runtime_selective_gpu_offset_request: clearing runtime selective state\n");
    WritePrivateProfileStringA("runtime", nullptr, nullptr, g_app.configPath);
    leave_config_storage_lock(configMutex);
}

static bool load_matching_runtime_selective_gpu_offset_request(int* gpuOffsetMHzOut, int* excludeLowCountOut) {
    if (gpuOffsetMHzOut) *gpuOffsetMHzOut = 0;
    if (excludeLowCountOut) *excludeLowCountOut = 0;

    int persistedOffsetMHz = 0;
    int persistedExcludeLowCount = 0;
    if (!load_runtime_selective_gpu_offset_request(&persistedOffsetMHz, &persistedExcludeLowCount)) return false;
    if (persistedExcludeLowCount <= 0 || persistedOffsetMHz == 0) {
        debug_log("runtime selective: ignoring persisted non-selective request value=%d exclude=%d\n",
            persistedOffsetMHz,
            persistedExcludeLowCount);
        return false;
    }

    if (!live_selective_gpu_offset_matches_requested_shape(persistedOffsetMHz, persistedExcludeLowCount, 12, nullptr)) {
        debug_log("runtime selective: ignoring persisted request value=%d exclude=%d because live readback does not match\n",
            persistedOffsetMHz,
            persistedExcludeLowCount);
        return false;
    }

    if (gpuOffsetMHzOut) *gpuOffsetMHzOut = persistedOffsetMHz;
    if (excludeLowCountOut) *excludeLowCountOut = persistedExcludeLowCount;
    return true;
}

static bool live_curve_has_any_nonzero_offsets() {
    if (!g_app.loaded) return false;
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (g_app.curve[ci].freq_kHz == 0) continue;
        if (g_app.freqOffsets[ci] != 0) return true;
    }
    return false;
}

static bool service_active_desired_gpu_offset_fallback(int* gpuOffsetMHzOut, int* excludeLowCountOut) {
    if (gpuOffsetMHzOut) *gpuOffsetMHzOut = 0;
    if (excludeLowCountOut) *excludeLowCountOut = 0;
    if (!g_app.isServiceProcess || !g_serviceHasActiveDesired || !g_serviceActiveDesired.hasGpuOffset) return false;

    int gpuOffsetMHz = g_serviceActiveDesired.gpuOffsetMHz;
    int excludeLowCount = (g_serviceActiveDesired.gpuOffsetExcludeLowCount > 0 && gpuOffsetMHz != 0) ? g_serviceActiveDesired.gpuOffsetExcludeLowCount : 0;
    if (gpuOffsetMHz == 0 || excludeLowCount <= 0) return false;
    if (!live_curve_has_any_nonzero_offsets()) return false;

    if (gpuOffsetMHzOut) *gpuOffsetMHzOut = gpuOffsetMHz;
    if (excludeLowCountOut) *excludeLowCountOut = excludeLowCount;
    return true;
}

static bool refresh_service_snapshot_and_active_desired(char* err, size_t errSize, DesiredSettings* activeDesiredOut) {
    if (activeDesiredOut) initialize_desired_settings_defaults(activeDesiredOut);
    if (g_app.isServiceProcess) return true;
    if (!g_app.usingBackgroundService) {
        if (err && errSize > 0) err[0] = 0;
        return true;
    }

    bool previousUsingBackgroundService = g_app.usingBackgroundService;
    bool serviceAvailable = refresh_background_service_state();
    if (!serviceAvailable || !g_app.usingBackgroundService) {
        g_app.usingBackgroundService = previousUsingBackgroundService;
        set_message(err, errSize, "Background service is not available");
        return false;
    }

    ServiceSnapshot snapshot = {};
    if (!service_client_get_snapshot(&snapshot, err, errSize)) {
        return false;
    }
    apply_service_snapshot_to_app(&snapshot);

    DesiredSettings activeDesired = {};
    char desiredErr[256] = {};
    if (service_client_get_active_desired(&activeDesired, nullptr, desiredErr, sizeof(desiredErr))) {
        // RC7 fix: do NOT call apply_service_desired_to_gui() here — that would
        // overwrite g_app.lockedCi/g_app.lockedFreq with the service's stale
        // active desired (which may still hold old lock values from before a
        // GPU device reconnect or reset), even when the just-received snapshot
        // correctly reported hasLock=false.  The snapshot is the authoritative
        // source for the GUI display.  The active desired is only returned to
        // callers that need it (e.g. profile mismatch checks).
        if (activeDesiredOut) *activeDesiredOut = activeDesired;
    } else if (desiredErr[0]) {
        debug_log("refresh_service_snapshot_and_active_desired: active desired unavailable: %s\n", desiredErr);
    }

    if (err && errSize > 0) err[0] = 0;
    return true;
}

static void build_full_live_desired_settings(DesiredSettings* desired) {
    if (!desired) return;
    initialize_desired_settings_defaults(desired);

    ControlState control = {};
    bool haveControlState = get_effective_control_state(&control);
    desired->hasGpuOffset = true;
    if (haveControlState && control_state_has_meaningful_gpu(&control)) {
        desired->gpuOffsetMHz = control.gpuOffsetMHz;
        desired->gpuOffsetExcludeLowCount = control.gpuOffsetExcludeLowCount;
    } else {
        resolve_displayed_live_gpu_offset_state_for_gui(&desired->gpuOffsetMHz, &desired->gpuOffsetExcludeLowCount);
    }
    desired->hasMemOffset = true;
    desired->memOffsetMHz = haveControlState && control_state_has_meaningful_mem(&control)
        ? control.memOffsetMHz
        : mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz);
    desired->hasPowerLimit = true;
    desired->powerLimitPct = haveControlState && control_state_has_meaningful_power(&control)
        ? control.powerLimitPct
        : g_app.powerLimitPct;
    desired->hasFan = true;
    desired->fanMode = haveControlState && control_state_has_meaningful_fan(&control)
        ? control.fanMode
        : g_app.activeFanMode;
    desired->fanAuto = desired->fanMode == FAN_MODE_AUTO;
    desired->fanPercent = haveControlState && control_state_has_meaningful_fan(&control)
        ? control.fanFixedPercent
        : g_app.activeFanFixedPercent;
    copy_fan_curve(&desired->fanCurve,
        haveControlState && control_state_has_meaningful_fan(&control)
            ? &control.fanCurve
            : &g_app.activeFanCurve);
    // Do not save the applied lock or curve points when saving from live state.
    // The VF curve may have been flattened by a previous profile's lock, and saving
    // those frequencies as explicit curve points would cause infer_profile_lock_from_curve
    // to detect the flat tail and re-apply the lock when the profile is loaded.
    // Without curve points, the apply path's reset-before-apply restores the GPU
    // to its factory base frequencies (via offset zeroing), which is the correct
    // "no custom curve" behavior for a default/safety profile.
}

static bool load_curve_points_explicit_from_section(const char* path, const char* section, DesiredSettings* desired, char* err, size_t errSize) {
    if (!path || !section || !desired) return false;
    bool foundAny = false;
    int lastMHz = 0;
    int lastCi = -1;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        char key[32] = {};
        char buf[64] = {};
        StringCchPrintfA(key, ARRAY_COUNT(key), "point%d_mhz", i);
        GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), path);
        trim_ascii(buf);
        if (!buf[0]) continue;

        int mhz = 0;
        if (!parse_int_strict(buf, &mhz) || mhz <= 0) {
            set_message(err, errSize, "Invalid %s in section [%s]", key, section);
            return false;
        }
        if (mhz > 5000) {
            debug_log("load_curve_points_explicit: warning %s=%d MHz exceeds sanity limit, clamping to 5000\n", key, mhz);
            mhz = 5000;
        }

        char visibleKey[32] = {};
        char visibleBuf[64] = {};
        StringCchPrintfA(visibleKey, ARRAY_COUNT(visibleKey), "point%d_visible", i);
        GetPrivateProfileStringA(section, visibleKey, "", visibleBuf, sizeof(visibleBuf), path);
        trim_ascii(visibleBuf);
        if (visibleBuf[0]) {
            int visibleValue = 0;
            if (!parse_int_strict(visibleBuf, &visibleValue)) {
                set_message(err, errSize, "Invalid %s in section [%s]", visibleKey, section);
                return false;
            }
            if (visibleValue == 0) {
                debug_log("load_curve_points_explicit: skipping hidden %s=%d in section [%s]\n", key, mhz, section);
                continue;
            }
        } else {
            char mvKey[32] = {};
            char mvBuf[64] = {};
            StringCchPrintfA(mvKey, ARRAY_COUNT(mvKey), "point%d_mv", i);
            GetPrivateProfileStringA(section, mvKey, "", mvBuf, sizeof(mvBuf), path);
            trim_ascii(mvBuf);
            int mv = 0;
            if (mvBuf[0] && parse_int_strict(mvBuf, &mv) && mv > 0 && mv < MIN_VISIBLE_VOLT_mV) {
                debug_log("load_curve_points_explicit: skipping legacy hidden %s=%d mV in section [%s]\n", mvKey, mv, section);
                continue;
            }
        }

        // Non-monotonic points can confuse the driver. Log a warning but still
        // load the point so the user can see it in the GUI.
        if (lastCi >= 0 && mhz < lastMHz) {
            char semBuf[64] = {};
            GetPrivateProfileStringA(section, "curve_semantics", "", semBuf, sizeof(semBuf), path);
            trim_ascii(semBuf);
            bool isBasePlusOffset = (_stricmp(semBuf, "base_plus_gpu_offset") == 0);
            if (isBasePlusOffset) {
                debug_log("load_curve_points_explicit: note %s=%d MHz is below point %d (%d MHz) "
                    "(base values before GPU offset applied; will be rechecked after offset restore)\n",
                    key, mhz, lastCi, lastMHz);
            } else {
                debug_log("load_curve_points_explicit: warning %s=%d MHz is below point %d (%d MHz)\n",
                    key, mhz, lastCi, lastMHz);
            }
        }

        desired->hasCurvePoint[i] = true;
        desired->curvePointMHz[i] = (unsigned int)mhz;
        foundAny = true;
        lastCi = i;
        lastMHz = mhz;
    }
    return foundAny;
}

static bool is_curve_point_visible_in_gui(int pointIndex) {
    if (pointIndex < 0 || pointIndex >= VF_NUM_POINTS) return false;
    unsigned int voltMv = g_app.curve[pointIndex].volt_uV / 1000;
    unsigned int freqMHz = (unsigned int)(curve_base_khz_for_point(pointIndex) / 1000);
    return voltMv >= MIN_VISIBLE_VOLT_mV && freqMHz >= MIN_VISIBLE_FREQ_MHz;
}

static void resolve_displayed_live_gpu_offset_state_for_gui(int* gpuOffsetMHzOut, int* excludeLowCountOut) {
    int gpuOffsetMHz = current_applied_gpu_offset_mhz();
    current_applied_gpu_offset_excludes_low_points();
    int excludeLowCount = g_app.appliedGpuOffsetExcludeLowCount;

    int persistedOffsetMHz = 0;
    int persistedExcludeLowCount = 0;
    if (load_matching_runtime_selective_gpu_offset_request(&persistedOffsetMHz, &persistedExcludeLowCount)) {
        gpuOffsetMHz = persistedOffsetMHz;
        excludeLowCount = persistedExcludeLowCount;
    }

    if (gpuOffsetMHzOut) *gpuOffsetMHzOut = gpuOffsetMHz;
    if (excludeLowCountOut) *excludeLowCountOut = excludeLowCount;
}

static bool live_selective_gpu_offset_matches_requested_state(int gpuOffsetMHz) {
    if (!vf_curve_global_gpu_offset_supported() || gpuOffsetMHz == 0) return false;
    // Use the detection function with multi-exclude scan to find the best match.
    int detectedMHz = 0;
    int detectedExclude = 0;
    if (!detect_live_selective_gpu_offset_state(&detectedMHz, nullptr, &detectedExclude)) return false;
    return abs(detectedMHz - gpuOffsetMHz) <= 12;
}

static int current_applied_gpu_offset_mhz() {
    if (!vf_curve_global_gpu_offset_supported()) {
        int offsetMHz = g_app.gpuClockOffsetkHz / 1000;
        debug_log("current_applied_gpu_offset_mhz: not Blackwell, returning NVML offset=%d kHz -> %d MHz\n", g_app.gpuClockOffsetkHz, offsetMHz);
        g_app.appliedGpuOffsetMHz = offsetMHz;
        g_app.appliedGpuOffsetExcludeLowCount = 0;
        return g_app.appliedGpuOffsetMHz;
    }
    if (g_app.appliedGpuOffsetExcludeLowCount > 0 && g_app.appliedGpuOffsetMHz != 0
        && live_selective_gpu_offset_matches_requested_shape(g_app.appliedGpuOffsetMHz, g_app.appliedGpuOffsetExcludeLowCount, 12, nullptr)) {
        debug_log("current_applied_gpu_offset_mhz: preserving session selective value=%d MHz\n", g_app.appliedGpuOffsetMHz);
        return g_app.appliedGpuOffsetMHz;
    }
    if (!g_app.lastApplyUsedGpuOffset) {
        debug_log_on_change("current_applied_gpu_offset_mhz: last apply did not use GPU offset, skipping detection\n");
        g_app.appliedGpuOffsetMHz = 0;
        g_app.appliedGpuOffsetExcludeLowCount = 0;
        return 0;
    }
    int persistedOffsetMHz = 0;
    int persistedExcludeLowCount = 0;
    if (load_matching_runtime_selective_gpu_offset_request(&persistedOffsetMHz, &persistedExcludeLowCount)) {
        debug_log("current_applied_gpu_offset_mhz: preserving persisted request value=%d MHz exclude=%d\n",
            persistedOffsetMHz, persistedExcludeLowCount);
        g_app.appliedGpuOffsetMHz = persistedOffsetMHz;
        g_app.appliedGpuOffsetExcludeLowCount = persistedExcludeLowCount;
        return persistedOffsetMHz;
    }
    int detectedSelectiveOffsetMHz = 0;
    int detectedExcludeLowCount = 0;
    if (detect_live_selective_gpu_offset_state(&detectedSelectiveOffsetMHz, nullptr, &detectedExcludeLowCount)) {
        debug_log("current_applied_gpu_offset_mhz: detected selective offset=%d MHz exclude=%d\n",
            detectedSelectiveOffsetMHz, detectedExcludeLowCount);
        g_app.appliedGpuOffsetMHz = detectedSelectiveOffsetMHz;
        g_app.appliedGpuOffsetExcludeLowCount = detectedExcludeLowCount;
        return detectedSelectiveOffsetMHz;
    }
    int desiredServiceOffsetMHz = 0;
    int desiredServiceExcludeLowCount = 0;
    if (service_active_desired_gpu_offset_fallback(&desiredServiceOffsetMHz, &desiredServiceExcludeLowCount)) {
        debug_log("current_applied_gpu_offset_mhz: preserving service desired value=%d MHz exclude=%d\n",
            desiredServiceOffsetMHz,
            desiredServiceExcludeLowCount);
        g_app.appliedGpuOffsetMHz = desiredServiceOffsetMHz;
        g_app.appliedGpuOffsetExcludeLowCount = desiredServiceExcludeLowCount;
        return desiredServiceOffsetMHz;
    }
    int offsetMHz = g_app.gpuClockOffsetkHz / 1000;
    debug_log("current_applied_gpu_offset_mhz: no selective detected, uniform offset=%d kHz -> %d MHz\n", g_app.gpuClockOffsetkHz, offsetMHz);
    g_app.appliedGpuOffsetMHz = offsetMHz;
    g_app.appliedGpuOffsetExcludeLowCount = 0;
    return offsetMHz;
}

static bool current_applied_gpu_offset_excludes_low_points() {
    if (!vf_curve_global_gpu_offset_supported()) {
        g_app.appliedGpuOffsetExcludeLowCount = 0;
        return false;
    }

    if (g_app.appliedGpuOffsetExcludeLowCount > 0 && g_app.appliedGpuOffsetMHz != 0
        && live_selective_gpu_offset_matches_requested_shape(g_app.appliedGpuOffsetMHz, g_app.appliedGpuOffsetExcludeLowCount, 12, nullptr)) {
        return true;
    }

    int persistedOffsetMHz = 0;
    int persistedExcludeLowCount = 0;
    if (load_matching_runtime_selective_gpu_offset_request(&persistedOffsetMHz, &persistedExcludeLowCount)) {
        g_app.appliedGpuOffsetMHz = persistedOffsetMHz;
        g_app.appliedGpuOffsetExcludeLowCount = persistedExcludeLowCount;
        return persistedExcludeLowCount > 0;
    }

    if (!g_app.lastApplyUsedGpuOffset) {
        debug_log_on_change("current_applied_gpu_offset_excludes_low_points: last apply did not use GPU offset, skipping detection\n");
        g_app.appliedGpuOffsetMHz = 0;
        g_app.appliedGpuOffsetExcludeLowCount = 0;
        return false;
    }

    int detectedSelectiveOffsetMHz = 0;
    int detectedExcludeLowCount = 0;
    if (detect_live_selective_gpu_offset_state(&detectedSelectiveOffsetMHz, nullptr, &detectedExcludeLowCount)) {
        g_app.appliedGpuOffsetMHz = detectedSelectiveOffsetMHz;
        g_app.appliedGpuOffsetExcludeLowCount = detectedExcludeLowCount;
        return detectedExcludeLowCount > 0;
    }
    int desiredServiceOffsetMHz = 0;
    int desiredServiceExcludeLowCount = 0;
    if (service_active_desired_gpu_offset_fallback(&desiredServiceOffsetMHz, &desiredServiceExcludeLowCount)) {
        g_app.appliedGpuOffsetMHz = desiredServiceOffsetMHz;
        g_app.appliedGpuOffsetExcludeLowCount = desiredServiceExcludeLowCount;
        return desiredServiceExcludeLowCount > 0;
    }
    g_app.appliedGpuOffsetMHz = g_app.gpuClockOffsetkHz / 1000;
    g_app.appliedGpuOffsetExcludeLowCount = 0;
    return false;
}

static void resolve_effective_gpu_offset_state_for_config_save(const DesiredSettings* desired, int* gpuOffsetMHzOut, int* excludeLowCountOut) {
    int gpuOffsetMHz = 0;
    int excludeLowCount = 0;

    if (desired && desired->hasGpuOffset) {
        gpuOffsetMHz = desired->gpuOffsetMHz;
        excludeLowCount = desired->gpuOffsetExcludeLowCount;
    } else {
        resolve_displayed_live_gpu_offset_state_for_gui(&gpuOffsetMHz, &excludeLowCount);
    }

    if (gpuOffsetMHz == 0) excludeLowCount = 0;
    if (gpuOffsetMHzOut) *gpuOffsetMHzOut = gpuOffsetMHz;
    if (excludeLowCountOut) *excludeLowCountOut = excludeLowCount;
}

