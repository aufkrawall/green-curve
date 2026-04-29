static bool ensure_directory_recursive_windows(const char* path, char* err, size_t errSize) {
    if (!path || !*path) return true;

    char temp[MAX_PATH] = {};
    StringCchCopyA(temp, ARRAY_COUNT(temp), path);
    trim_ascii(temp);
    size_t len = strlen(temp);
    while (len > 0 && (temp[len - 1] == '\\' || temp[len - 1] == '/')) {
        temp[--len] = 0;
    }
    if (len == 0) return true;

    for (char* p = temp; *p; ++p) {
        if (*p != '\\' && *p != '/') continue;
        if (p == temp) continue;
        if (*(p - 1) == ':') continue;
        char save = *p;
        *p = 0;
        if (temp[0]) {
            if (!CreateDirectoryA(temp, nullptr)) {
                DWORD e = GetLastError();
                if (e != ERROR_ALREADY_EXISTS) {
                    set_message(err, errSize, "Failed creating directory %s (error %lu)", temp, e);
                    return false;
                }
                DWORD attrs = GetFileAttributesA(temp);
                if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    set_message(err, errSize, "Directory %s is a reparse point, refusing to traverse", temp);
                    return false;
                }
            }
        }
        *p = save;
    }

    if (!CreateDirectoryA(temp, nullptr)) {
        DWORD e = GetLastError();
        if (e != ERROR_ALREADY_EXISTS) {
            set_message(err, errSize, "Failed creating directory %s (error %lu)", temp, e);
            return false;
        }
    }
    return true;
}

static bool ensure_parent_directory_for_file(const char* path, char* err, size_t errSize) {
    if (!path || !*path) return false;
    char parent[MAX_PATH] = {};
    StringCchCopyA(parent, ARRAY_COUNT(parent), path);
    char* slash = strrchr(parent, '\\');
    if (!slash) slash = strrchr(parent, '/');
    if (!slash) return true;
    *slash = 0;
    return ensure_directory_recursive_windows(parent, err, errSize);
}

static bool get_known_folder_path_utf8(REFKNOWNFOLDERID folderId, char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    out[0] = 0;
    PWSTR wide = nullptr;
    HRESULT hr = SHGetKnownFolderPath(folderId, KF_FLAG_DEFAULT, nullptr, &wide);
    if (FAILED(hr) || !wide) return false;
    bool ok = copy_wide_to_utf8(wide, out, (int)outSize);
    CoTaskMemFree(wide);
    return ok;
}

static const char* cli_log_path() {
    return g_cliLogPath[0] ? g_cliLogPath : APP_CLI_LOG_FILE;
}

static bool resolve_data_paths(char* err, size_t errSize) {
    if (g_userDataDir[0] && g_cliLogPath[0] && g_debugLogPath[0] && g_jsonPath[0] && g_errorLogPath[0]) {
        return true;
    }

    char localAppData[MAX_PATH] = {};
    if (!get_known_folder_path_utf8(FOLDERID_LocalAppData, localAppData, sizeof(localAppData))) {
        set_message(err, errSize, "Failed resolving LocalAppData");
        return false;
    }
    if (FAILED(StringCchPrintfA(g_userDataDir, ARRAY_COUNT(g_userDataDir), "%s\\Green Curve", localAppData)) ||
        FAILED(StringCchPrintfA(g_cliLogPath, ARRAY_COUNT(g_cliLogPath), "%s\\%s", g_userDataDir, APP_CLI_LOG_FILE)) ||
        FAILED(StringCchPrintfA(g_debugLogPath, ARRAY_COUNT(g_debugLogPath), "%s\\%s", g_userDataDir, APP_DEBUG_LOG_FILE)) ||
        FAILED(StringCchPrintfA(g_jsonPath, ARRAY_COUNT(g_jsonPath), "%s\\%s", g_userDataDir, APP_JSON_FILE)) ||
        FAILED(StringCchPrintfA(g_errorLogPath, ARRAY_COUNT(g_errorLogPath), "%s\\%s", g_userDataDir, APP_LOG_FILE))) {
        set_message(err, errSize, "Resolved storage paths are too long");
        return false;
    }

    if (!ensure_directory_recursive_windows(g_userDataDir, err, errSize)) return false;
    return true;
}

static bool resolve_service_user_data_paths(DWORD sessionId, char* err, size_t errSize) {
    if (g_serviceUserPathsResolved && g_serviceUserPathsSessionId == sessionId) {
        return true;
    }
    if (g_serviceUserPathsResolved && g_serviceUserPathsSessionId != sessionId) {
        close_debug_log_file();
        g_userDataDir[0] = 0;
        g_cliLogPath[0] = 0;
        g_debugLogPath[0] = 0;
        g_jsonPath[0] = 0;
        g_errorLogPath[0] = 0;
        g_serviceUserProfileDir[0] = 0;
        g_app.configPath[0] = 0;
        g_serviceUserPathsResolved = false;
        g_serviceUserPathsSessionId = (DWORD)-1;
    }
    HANDLE hToken = nullptr;
    if (!WTSQueryUserToken(sessionId, &hToken)) {
        set_message(err, errSize, "WTSQueryUserToken failed");
        return false;
    }
    WCHAR profileDirW[MAX_PATH] = {};
    DWORD profileSize = ARRAY_COUNT(profileDirW);
    if (!GetUserProfileDirectoryW(hToken, profileDirW, &profileSize)) {
        set_message(err, errSize, "GetUserProfileDirectoryW failed");
        CloseHandle(hToken);
        return false;
    }
    CloseHandle(hToken);
    char profileDir[MAX_PATH] = {};
    if (!copy_wide_to_utf8(profileDirW, profileDir, ARRAY_COUNT(profileDir))) {
        set_message(err, errSize, "Profile path conversion failed");
        return false;
    }
    StringCchCopyA(g_serviceUserProfileDir, ARRAY_COUNT(g_serviceUserProfileDir), profileDir);
    char localAppData[MAX_PATH] = {};
    if (FAILED(StringCchPrintfA(localAppData, ARRAY_COUNT(localAppData), "%s\\AppData\\Local", profileDir))) {
        set_message(err, errSize, "Profile path too long");
        return false;
    }
    if (FAILED(StringCchPrintfA(g_userDataDir, ARRAY_COUNT(g_userDataDir), "%s\\Green Curve", localAppData)) ||
        FAILED(StringCchPrintfA(g_cliLogPath, ARRAY_COUNT(g_cliLogPath), "%s\\%s", g_userDataDir, APP_CLI_LOG_FILE)) ||
        FAILED(StringCchPrintfA(g_debugLogPath, ARRAY_COUNT(g_debugLogPath), "%s\\%s", g_userDataDir, APP_DEBUG_LOG_FILE)) ||
        FAILED(StringCchPrintfA(g_jsonPath, ARRAY_COUNT(g_jsonPath), "%s\\%s", g_userDataDir, APP_JSON_FILE)) ||
        FAILED(StringCchPrintfA(g_errorLogPath, ARRAY_COUNT(g_errorLogPath), "%s\\%s", g_userDataDir, APP_LOG_FILE))) {
        set_message(err, errSize, "Resolved storage paths are too long");
        return false;
    }
    if (!ensure_directory_recursive_windows(g_userDataDir, err, errSize)) {
        return false;
    }
    g_serviceUserPathsResolved = true;
    g_serviceUserPathsSessionId = sessionId;
    return true;
}

static bool service_debug_env_override(bool* enabledOut) {
    if (enabledOut) *enabledOut = false;
    char value[32] = {};
    DWORD n = GetEnvironmentVariableA(APP_DEBUG_ENV, value, ARRAY_COUNT(value));
    if (n == 0) return false;
    value[ARRAY_COUNT(value) - 1] = 0;
    trim_ascii(value);
    bool enabled = true;
    if (n >= ARRAY_COUNT(value) || value[0] == 0) {
        enabled = true;
    } else if (value[0] == '0' && value[1] == 0) {
        enabled = false;
    } else if ((value[0] == 'f' || value[0] == 'F') &&
        (value[1] == 'a' || value[1] == 'A') &&
        (value[2] == 'l' || value[2] == 'L') &&
        (value[3] == 's' || value[3] == 'S') &&
        (value[4] == 'e' || value[4] == 'E') &&
        value[5] == 0) {
        enabled = false;
    }
    if (enabledOut) *enabledOut = enabled;
    return true;
}

static bool service_initial_debug_logging_enabled() {
    bool envEnabled = false;
    if (service_debug_env_override(&envEnabled)) return envEnabled;
    return SERVICE_DEBUG_DEFAULT_ENABLED != 0;
}

static bool service_config_debug_logging_enabled(int* envValueOut, int* configValueOut) {
    bool envEnabled = false;
    bool haveEnv = service_debug_env_override(&envEnabled);
    int configDebug = g_app.configPath[0]
        ? get_config_int(g_app.configPath, "debug", "enabled", SERVICE_DEBUG_DEFAULT_ENABLED)
        : SERVICE_DEBUG_DEFAULT_ENABLED;
    if (envValueOut) *envValueOut = haveEnv ? (envEnabled ? 1 : 0) : -1;
    if (configValueOut) *configValueOut = configDebug;
    return haveEnv ? envEnabled : (configDebug != 0);
}

static bool config_file_exists() {
    if (!g_app.configPath[0]) return false;
    DWORD attrs = GetFileAttributesA(g_app.configPath);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static void clear_service_authoritative_state() {
    g_app.serviceSnapshotAuthoritative = false;
    g_app.serviceControlStateValid = false;
    memset(&g_app.serviceControlState, 0, sizeof(g_app.serviceControlState));
    g_serviceControlStateValid = false;
    memset(&g_serviceControlState, 0, sizeof(g_serviceControlState));
    g_serviceTelemetryLastHardwarePollTickMs = 0;
    g_serviceTelemetryLastPollSource[0] = 0;
}

static int format_log_timestamp_prefix(char* out, size_t outSize) {
    if (!out || outSize == 0) return 0;
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    HRESULT hr = StringCchPrintfA(out, outSize,
        "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
        now.wYear,
        now.wMonth,
        now.wDay,
        now.wHour,
        now.wMinute,
        now.wSecond,
        now.wMilliseconds);
    if (FAILED(hr)) {
        out[0] = 0;
        return 0;
    }
    return (int)strlen(out);
}


static void set_default_config_path() {
    if (g_app.configPath[0]) return;

    char err[256] = {};
    if (resolve_data_paths(err, sizeof(err))) {
        StringCchPrintfA(g_app.configPath, ARRAY_COUNT(g_app.configPath), "%s\\%s", g_userDataDir, CONFIG_FILE_NAME);

        char exeConfigPath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exeConfigPath, ARRAY_COUNT(exeConfigPath));
        char* slash = strrchr(exeConfigPath, '\\');
        if (!slash) slash = strrchr(exeConfigPath, '/');
        if (slash) {
            slash[1] = 0;
            StringCchCatA(exeConfigPath, ARRAY_COUNT(exeConfigPath), CONFIG_FILE_NAME);
            DWORD legacyAttrs = GetFileAttributesA(exeConfigPath);
            DWORD currentAttrs = GetFileAttributesA(g_app.configPath);
            if (legacyAttrs != INVALID_FILE_ATTRIBUTES && currentAttrs == INVALID_FILE_ATTRIBUTES) {
                CopyFileA(exeConfigPath, g_app.configPath, TRUE);
            }
        }
    } else {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        char* slash = strrchr(path, '\\');
        if (!slash) slash = strrchr(path, '/');
        if (slash) {
            slash[1] = 0;
            StringCchCatA(path, ARRAY_COUNT(path), CONFIG_FILE_NAME);
        } else {
            StringCchCopyA(path, ARRAY_COUNT(path), CONFIG_FILE_NAME);
        }
        StringCchCopyA(g_app.configPath, ARRAY_COUNT(g_app.configPath), path);
    }
    invalidate_tray_profile_cache();
}

static void refresh_service_debug_logging_from_config() {
    if (!g_app.isServiceProcess) return;
    bool newDebugLogging = service_config_debug_logging_enabled(nullptr, nullptr);
    g_debug_logging = newDebugLogging;
    if (!g_debug_logging) {
        close_debug_log_file();
    }
}

static bool hardware_initialize(char* detail, size_t detailSize) {
    if (g_app.gpuHandle && g_app.loaded && g_app.vfBackend) return true;
    set_last_apply_phase("hardware initialize: begin");
    debug_log("hardware_initialize: (re)initializing GPU backend\n");
    if (!nvapi_init()) {
        set_message(detail, detailSize, "Failed to initialize NvAPI");
        set_last_apply_phase("hardware initialize: NvAPI init failed");
        return false;
    }
    set_last_apply_phase("hardware initialize: enumerate GPU");
    if (!nvapi_enum_gpu()) {
        set_message(detail, detailSize, "No NVIDIA GPU found");
        set_last_apply_phase("hardware initialize: no GPU found");
        return false;
    }
    nvapi_get_name();
    nvapi_read_gpu_metadata();
    bool offsetsOk = false;
    set_last_apply_phase("hardware initialize: VF curve readback");
    if (!read_live_curve_snapshot_settled(4, 40, &offsetsOk)) {
        set_message(detail, detailSize, "Failed to read VF curve from GPU");
        set_last_apply_phase("hardware initialize: VF curve read failed");
        return false;
    }
    (void)offsetsOk;
    set_last_apply_phase("hardware initialize: global state refresh");
    refresh_global_state(detail, detailSize);
    initialize_gui_fan_settings_from_live_state(false);
    // Preserve the service active desired state across reinitializations
    // (e.g. after a driver TDR) so the GUI does not lose track of what
    // the service had applied.
    if (g_serviceHasActiveDesired) {
        debug_log("hardware_initialize: preserving existing service active desired state\n");
    } else {
        g_serviceActiveDesired = {};
        g_serviceHasActiveDesired = false;
    }
#ifdef GREEN_CURVE_SERVICE_BINARY
    trim_working_set();
#endif
    set_last_apply_phase("hardware initialize: complete");
    return true;
}

static void populate_service_snapshot(ServiceSnapshot* snapshot) {
    if (!snapshot) return;
    EnterCriticalSection(&g_appLock);
    memset(snapshot, 0, sizeof(*snapshot));
    int snapshotGpuOffsetMHz = g_app.appliedGpuOffsetMHz;
    int snapshotGpuOffsetExcludeLowCount = (g_app.appliedGpuOffsetExcludeLowCount > 0 && snapshotGpuOffsetMHz != 0) ? g_app.appliedGpuOffsetExcludeLowCount : 0;
    if (g_serviceControlStateValid && control_state_has_meaningful_gpu(&g_serviceControlState)) {
        snapshotGpuOffsetMHz = g_serviceControlState.gpuOffsetMHz;
        snapshotGpuOffsetExcludeLowCount = (g_serviceControlState.gpuOffsetExcludeLowCount > 0 && snapshotGpuOffsetMHz != 0) ? g_serviceControlState.gpuOffsetExcludeLowCount : 0;
    } else {
        int desiredServiceOffsetMHz = 0;
        int desiredServiceExcludeLowCount = 0;
        if (service_active_desired_gpu_offset_fallback(&desiredServiceOffsetMHz, &desiredServiceExcludeLowCount)) {
            snapshotGpuOffsetMHz = desiredServiceOffsetMHz;
            snapshotGpuOffsetExcludeLowCount = desiredServiceExcludeLowCount;
        }
    }
    snapshot->initialized = g_app.gpuHandle != nullptr;
    snapshot->loaded = g_app.loaded;
    snapshot->fanSupported = g_app.fanSupported;
    snapshot->fanRangeKnown = g_app.fanRangeKnown;
    snapshot->fanIsAuto = g_app.fanIsAuto;
    snapshot->fanCurveRuntimeActive = g_app.fanCurveRuntimeActive;
    snapshot->fanFixedRuntimeActive = g_app.fanFixedRuntimeActive;
    snapshot->gpuOffsetRangeKnown = g_app.gpuOffsetRangeKnown;
    snapshot->memOffsetRangeKnown = g_app.memOffsetRangeKnown;
    snapshot->curveOffsetRangeKnown = g_app.curveOffsetRangeKnown;
    snapshot->gpuTemperatureValid = g_app.gpuTemperatureValid;
    snapshot->vfReadSupported = g_app.vfBackend && g_app.vfBackend->readSupported;
    snapshot->vfWriteSupported = g_app.vfBackend && g_app.vfBackend->writeSupported;
    snapshot->vfBestGuess = vf_backend_is_best_guess(g_app.vfBackend);
    snapshot->adapterCount = g_app.adapterCount;
    snapshot->selectedAdapterIndex = g_app.selectedGpuIndex;
    snapshot->selectedAdapterOrdinalFallback = g_app.selectedGpuOrdinalFallback;
    memcpy(snapshot->adapters, g_app.adapters, sizeof(snapshot->adapters));
    snapshot->gpuFamily = g_app.gpuFamily;
    snapshot->numPopulated = g_app.numPopulated;
    snapshot->gpuClockOffsetkHz = g_app.gpuClockOffsetkHz;
    snapshot->memClockOffsetkHz = g_app.memClockOffsetkHz;
    snapshot->gpuClockOffsetMinMHz = g_app.gpuClockOffsetMinMHz;
    snapshot->gpuClockOffsetMaxMHz = g_app.gpuClockOffsetMaxMHz;
    snapshot->memOffsetMinMHz = g_app.memClockOffsetMinMHz;
    snapshot->memOffsetMaxMHz = g_app.memClockOffsetMaxMHz;
    snapshot->curveOffsetMinkHz = g_app.curveOffsetMinkHz;
    snapshot->curveOffsetMaxkHz = g_app.curveOffsetMaxkHz;
    snapshot->powerLimitPct = g_app.powerLimitPct;
    snapshot->powerLimitDefaultmW = g_app.powerLimitDefaultmW;
    snapshot->powerLimitCurrentmW = g_app.powerLimitCurrentmW;
    snapshot->powerLimitMinmW = g_app.powerLimitMinmW;
    snapshot->powerLimitMaxmW = g_app.powerLimitMaxmW;
    snapshot->appliedGpuOffsetMHz = snapshotGpuOffsetMHz;
    snapshot->appliedGpuOffsetExcludeLowCount = snapshotGpuOffsetExcludeLowCount;
    snapshot->activeFanMode = g_app.activeFanMode;
    snapshot->activeFanFixedPercent = g_app.activeFanFixedPercent;
    snapshot->gpuTemperatureC = g_app.gpuTemperatureC;
    snapshot->fanCount = g_app.fanCount;
    snapshot->fanMinPct = g_app.fanMinPct;
    snapshot->fanMaxPct = g_app.fanMaxPct;
    memcpy(snapshot->fanPercent, g_app.fanPercent, sizeof(snapshot->fanPercent));
    memcpy(snapshot->fanTargetPercent, g_app.fanTargetPercent, sizeof(snapshot->fanTargetPercent));
    memcpy(snapshot->fanRpm, g_app.fanRpm, sizeof(snapshot->fanRpm));
    memcpy(snapshot->fanPolicy, g_app.fanPolicy, sizeof(snapshot->fanPolicy));
    memcpy(snapshot->fanControlSignal, g_app.fanControlSignal, sizeof(snapshot->fanControlSignal));
    memcpy(snapshot->fanTargetMask, g_app.fanTargetMask, sizeof(snapshot->fanTargetMask));
    memcpy(snapshot->curve, g_app.curve, sizeof(snapshot->curve));
    memcpy(snapshot->freqOffsets, g_app.freqOffsets, sizeof(snapshot->freqOffsets));
    copy_fan_curve(&snapshot->activeFanCurve, &g_app.activeFanCurve);
    StringCchCopyA(snapshot->gpuName, ARRAY_COUNT(snapshot->gpuName), g_app.gpuName);
    StringCchCopyA(snapshot->ownerUser, ARRAY_COUNT(snapshot->ownerUser), g_app.backgroundServiceOwnerUser);
    snapshot->ownerSessionId = g_app.backgroundServiceOwnerSessionId;
    snapshot->ownerUtcMs = g_app.backgroundServiceOwnerUtcMs;
    LeaveCriticalSection(&g_appLock);
}

static void populate_control_state(ControlState* state) {
    if (!state) return;
    EnterCriticalSection(&g_appLock);
    memset(state, 0, sizeof(*state));
    state->valid = true;
    state->hasGpuOffset = true;
    state->gpuOffsetMHz = current_applied_gpu_offset_mhz();
    state->gpuOffsetExcludeLowCount = (current_applied_gpu_offset_excludes_low_points() && state->gpuOffsetMHz != 0) ? g_app.appliedGpuOffsetExcludeLowCount : 0;
    state->hasMemOffset = true;
    state->memOffsetMHz = mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz);
    state->hasPowerLimit = true;
    state->powerLimitPct = g_app.powerLimitPct;
    state->hasFan = true;
    state->fanMode = g_app.activeFanMode;
    state->fanFixedPercent = g_app.activeFanFixedPercent;
    state->fanCurrentPercent = current_displayed_fan_percent();
    state->fanCurrentTemperatureC = g_app.gpuTemperatureValid ? g_app.gpuTemperatureC : 0;
    copy_fan_curve(&state->fanCurve, &g_app.activeFanCurve);
    ensure_valid_fan_curve_config(&state->fanCurve);
    LeaveCriticalSection(&g_appLock);
}

static void apply_service_snapshot_to_app(const ServiceSnapshot* snapshot) {
    if (!snapshot) return;
    EnterCriticalSection(&g_appLock);
    g_app.serviceSnapshotAuthoritative = true;
    int previousAppliedGpuOffsetMHz = g_app.appliedGpuOffsetMHz;
    int previousAppliedGpuOffsetExcludeLowCount = g_app.appliedGpuOffsetExcludeLowCount;
    ControlState previousServiceControlState = g_app.serviceControlState;
    bool previousServiceGpuMeaningful = g_app.serviceControlStateValid && control_state_has_meaningful_gpu(&g_app.serviceControlState);
    bool previousServiceMemMeaningful = g_app.serviceControlStateValid && control_state_has_meaningful_mem(&g_app.serviceControlState);
    bool previousServicePowerMeaningful = g_app.serviceControlStateValid && control_state_has_meaningful_power(&g_app.serviceControlState);
    bool previousServiceFanMeaningful = g_app.serviceControlStateValid && control_state_has_meaningful_fan(&g_app.serviceControlState);
    g_app.loaded = snapshot->loaded;
    g_app.fanSupported = snapshot->fanSupported;
    g_app.fanRangeKnown = snapshot->fanRangeKnown;
    g_app.fanIsAuto = snapshot->fanIsAuto;
    g_app.fanCurveRuntimeActive = snapshot->fanCurveRuntimeActive;
    g_app.fanFixedRuntimeActive = snapshot->fanFixedRuntimeActive;
    g_app.gpuOffsetRangeKnown = snapshot->gpuOffsetRangeKnown;
    g_app.memOffsetRangeKnown = snapshot->memOffsetRangeKnown;
    g_app.curveOffsetRangeKnown = snapshot->curveOffsetRangeKnown;
    g_app.gpuTemperatureValid = snapshot->gpuTemperatureValid;
    g_app.adapterCount = snapshot->adapterCount > MAX_GPU_ADAPTERS ? MAX_GPU_ADAPTERS : snapshot->adapterCount;
    g_app.selectedGpuIndex = snapshot->selectedAdapterIndex;
    g_app.selectedNvmlIndex = snapshot->selectedAdapterIndex;
    g_app.selectedGpuOrdinalFallback = snapshot->selectedAdapterOrdinalFallback;
    memcpy(g_app.adapters, snapshot->adapters, sizeof(g_app.adapters));
    if (g_app.selectedGpuIndex < g_app.adapterCount) {
        g_app.selectedGpu = g_app.adapters[g_app.selectedGpuIndex];
        g_app.selectedGpuIdentityValid = g_app.selectedGpu.valid;
    }
    g_app.gpuFamily = snapshot->gpuFamily;
    g_app.numPopulated = snapshot->numPopulated;
    g_app.gpuClockOffsetkHz = snapshot->gpuClockOffsetkHz;
    g_app.memClockOffsetkHz = snapshot->memClockOffsetkHz;
    g_app.gpuClockOffsetMinMHz = snapshot->gpuClockOffsetMinMHz;
    g_app.gpuClockOffsetMaxMHz = snapshot->gpuClockOffsetMaxMHz;
    g_app.memClockOffsetMinMHz = snapshot->memOffsetMinMHz;
    g_app.memClockOffsetMaxMHz = snapshot->memOffsetMaxMHz;
    g_app.curveOffsetMinkHz = snapshot->curveOffsetMinkHz;
    g_app.curveOffsetMaxkHz = snapshot->curveOffsetMaxkHz;
    g_app.powerLimitPct = snapshot->powerLimitPct;
    g_app.powerLimitDefaultmW = snapshot->powerLimitDefaultmW;
    g_app.powerLimitCurrentmW = snapshot->powerLimitCurrentmW;
    g_app.powerLimitMinmW = snapshot->powerLimitMinmW;
    g_app.powerLimitMaxmW = snapshot->powerLimitMaxmW;
    bool snapshotGpuMeaningful = snapshot->appliedGpuOffsetMHz != 0 || snapshot->appliedGpuOffsetExcludeLowCount > 0;
    if (snapshotGpuMeaningful || !previousServiceGpuMeaningful) {
        g_app.appliedGpuOffsetMHz = snapshot->appliedGpuOffsetMHz;
        g_app.appliedGpuOffsetExcludeLowCount = snapshot->appliedGpuOffsetExcludeLowCount;
    } else {
        g_app.appliedGpuOffsetMHz = previousAppliedGpuOffsetMHz;
        g_app.appliedGpuOffsetExcludeLowCount = previousAppliedGpuOffsetExcludeLowCount;
    }
    g_app.activeFanMode = snapshot->activeFanMode;
    g_app.activeFanFixedPercent = snapshot->activeFanFixedPercent;
    g_app.gpuTemperatureC = snapshot->gpuTemperatureC;
    g_app.fanCount = snapshot->fanCount;
    g_app.fanMinPct = snapshot->fanMinPct;
    g_app.fanMaxPct = snapshot->fanMaxPct;
    memcpy(g_app.fanPercent, snapshot->fanPercent, sizeof(g_app.fanPercent));
    memcpy(g_app.fanTargetPercent, snapshot->fanTargetPercent, sizeof(g_app.fanTargetPercent));
    memcpy(g_app.fanRpm, snapshot->fanRpm, sizeof(g_app.fanRpm));
    memcpy(g_app.fanPolicy, snapshot->fanPolicy, sizeof(g_app.fanPolicy));
    memcpy(g_app.fanControlSignal, snapshot->fanControlSignal, sizeof(g_app.fanControlSignal));
    memcpy(g_app.fanTargetMask, snapshot->fanTargetMask, sizeof(g_app.fanTargetMask));
    memcpy(g_app.curve, snapshot->curve, sizeof(g_app.curve));
    memcpy(g_app.freqOffsets, snapshot->freqOffsets, sizeof(g_app.freqOffsets));
    copy_fan_curve(&g_app.activeFanCurve, &snapshot->activeFanCurve);
    StringCchCopyA(g_app.gpuName, ARRAY_COUNT(g_app.gpuName), snapshot->gpuName);
    StringCchCopyA(g_app.backgroundServiceOwnerUser, ARRAY_COUNT(g_app.backgroundServiceOwnerUser), snapshot->ownerUser);
    g_app.backgroundServiceOwnerSessionId = snapshot->ownerSessionId;
    g_app.backgroundServiceOwnerUtcMs = snapshot->ownerUtcMs;
    rebuild_visible_map();
    if (snapshot->loaded && should_accept_service_curve_lock_detection()) {
        detect_locked_tail_from_curve();
    }

    // Sync GUI fan mode to the service snapshot only when the user hasn't
    // explicitly changed it (e.g. after loading a profile but before applying).
    if (!gui_state_dirty()) {
        g_app.guiFanMode = snapshot->activeFanMode;
        if (snapshot->activeFanMode == FAN_MODE_FIXED) {
            g_app.guiFanFixedPercent = clamp_percent(snapshot->activeFanFixedPercent);
        } else {
            g_app.guiFanFixedPercent = clamp_percent(current_displayed_fan_percent());
        }
        ensure_valid_fan_curve_config(&g_app.guiFanCurve);
        if (snapshot->activeFanMode == FAN_MODE_CURVE) {
            copy_fan_curve(&g_app.guiFanCurve, &snapshot->activeFanCurve);
        }
    }
    memset(&g_app.serviceControlState, 0, sizeof(g_app.serviceControlState));
    g_app.serviceControlState.valid = true;
    g_app.serviceControlState.hasGpuOffset = true;
    if (snapshotGpuMeaningful || !previousServiceGpuMeaningful) {
        g_app.serviceControlState.gpuOffsetMHz = snapshot->appliedGpuOffsetMHz;
        g_app.serviceControlState.gpuOffsetExcludeLowCount = (snapshot->appliedGpuOffsetExcludeLowCount > 0 && snapshot->appliedGpuOffsetMHz != 0) ? snapshot->appliedGpuOffsetExcludeLowCount : 0;
    } else if (previousServiceGpuMeaningful) {
        g_app.serviceControlState.gpuOffsetMHz = previousAppliedGpuOffsetMHz;
        g_app.serviceControlState.gpuOffsetExcludeLowCount = (previousAppliedGpuOffsetExcludeLowCount > 0 && previousAppliedGpuOffsetMHz != 0) ? previousAppliedGpuOffsetExcludeLowCount : 0;
    }
    g_app.serviceControlState.hasMemOffset = true;
    int snapshotMemOffsetMHz = mem_display_mhz_from_driver_khz(snapshot->memClockOffsetkHz);
    if (snapshotMemOffsetMHz != 0 || !previousServiceMemMeaningful) {
        g_app.serviceControlState.memOffsetMHz = snapshotMemOffsetMHz;
    } else if (previousServiceMemMeaningful) {
        g_app.serviceControlState.memOffsetMHz = previousServiceControlState.memOffsetMHz;
    }
    g_app.serviceControlState.hasPowerLimit = true;
    if (snapshot->powerLimitPct != 100 || !previousServicePowerMeaningful) {
        g_app.serviceControlState.powerLimitPct = snapshot->powerLimitPct;
    } else if (previousServicePowerMeaningful) {
        g_app.serviceControlState.powerLimitPct = previousServiceControlState.powerLimitPct;
    }
    g_app.serviceControlState.hasFan = true;
    bool snapshotFanMeaningful = snapshot->activeFanMode != FAN_MODE_AUTO || snapshot->activeFanFixedPercent != 0 || current_displayed_fan_percent() != 0;
    if (snapshotFanMeaningful || !previousServiceFanMeaningful) {
        g_app.serviceControlState.fanMode = snapshot->activeFanMode;
        g_app.serviceControlState.fanFixedPercent = clamp_percent(snapshot->activeFanFixedPercent);
        g_app.serviceControlState.fanCurrentPercent = current_displayed_fan_percent();
        g_app.serviceControlState.fanCurrentTemperatureC = snapshot->gpuTemperatureValid ? snapshot->gpuTemperatureC : 0;
        copy_fan_curve(&g_app.serviceControlState.fanCurve, &snapshot->activeFanCurve);
        ensure_valid_fan_curve_config(&g_app.serviceControlState.fanCurve);
    } else if (previousServiceFanMeaningful) {
        g_app.serviceControlState.fanMode = previousServiceControlState.fanMode;
        g_app.serviceControlState.fanFixedPercent = previousServiceControlState.fanFixedPercent;
        g_app.serviceControlState.fanCurrentPercent = previousServiceControlState.fanCurrentPercent;
        g_app.serviceControlState.fanCurrentTemperatureC = previousServiceControlState.fanCurrentTemperatureC;
        copy_fan_curve(&g_app.serviceControlState.fanCurve, &previousServiceControlState.fanCurve);
        ensure_valid_fan_curve_config(&g_app.serviceControlState.fanCurve);
    }
    debug_log("apply_service_snapshot_to_app: snapshot gpu=%d exclude=%d cachedControl gpu=%d exclude=%d\n",
        snapshot->appliedGpuOffsetMHz,
        snapshot->appliedGpuOffsetExcludeLowCount,
        g_app.serviceControlState.gpuOffsetMHz,
        g_app.serviceControlState.gpuOffsetExcludeLowCount);
    g_app.serviceControlStateValid = true;
    LeaveCriticalSection(&g_appLock);
}

static void apply_service_desired_to_gui(const DesiredSettings* desired) {
    if (!desired) return;
    if (desired->hasGpuOffset) {
        g_app.appliedGpuOffsetMHz = desired->gpuOffsetMHz;
        g_app.appliedGpuOffsetExcludeLowCount = (desired->gpuOffsetExcludeLowCount > 0 && desired->gpuOffsetMHz != 0) ? desired->gpuOffsetExcludeLowCount : 0;
        if (!gui_state_dirty()) {
            g_app.guiGpuOffsetMHz = desired->gpuOffsetMHz;
            g_app.guiGpuOffsetExcludeLowCount = (desired->gpuOffsetExcludeLowCount > 0 && desired->gpuOffsetMHz != 0) ? desired->gpuOffsetExcludeLowCount : 0;
        }
    }
    if (!gui_state_dirty()) {
        if (desired->hasLock && desired->lockCi >= 0 && desired->lockMHz > 0) {
            g_app.lockedCi = desired->lockCi;
            g_app.lockedFreq = desired->lockMHz;
            g_app.guiLockTracksAnchor = desired->lockTracksAnchor;
            g_app.lockedVi = -1;
            for (int vi = 0; vi < g_app.numVisible; vi++) {
                if (g_app.visibleMap[vi] == desired->lockCi) {
                    g_app.lockedVi = vi;
                    break;
                }
            }
        } else if (g_app.lockedFreq == 0 || g_app.lockedCi < 0) {
            g_app.lockedVi = -1;
            g_app.lockedCi = -1;
            g_app.lockedFreq = 0;
            g_app.guiLockTracksAnchor = true;
        }
    }
    if (desired->hasFan) {
        if (!gui_state_dirty()) {
            g_app.guiFanMode = desired->fanMode;
            if (desired->fanMode == FAN_MODE_FIXED) {
                g_app.guiFanFixedPercent = clamp_percent(desired->fanPercent);
            } else {
                g_app.guiFanFixedPercent = clamp_percent(current_displayed_fan_percent());
            }
            copy_fan_curve(&g_app.guiFanCurve, &desired->fanCurve);
            ensure_valid_fan_curve_config(&g_app.guiFanCurve);
        }
    }
}

static void apply_control_state_to_gui(const ControlState* state) {
    if (!state || !state->valid) return;
    bool meaningfulGpuState = control_state_has_meaningful_gpu(state);
    bool meaningfulMemState = control_state_has_meaningful_mem(state);
    bool meaningfulPowerState = control_state_has_meaningful_power(state);
    bool meaningfulFanState = control_state_has_meaningful_fan(state);
    debug_log("apply_control_state_to_gui: gpu=%d exclude=%d mem=%d power=%d fanMode=%d fanPct=%d\n",
        state->gpuOffsetMHz,
        state->gpuOffsetExcludeLowCount,
        state->memOffsetMHz,
        state->powerLimitPct,
        state->fanMode,
        state->fanCurrentPercent > 0 ? state->fanCurrentPercent : state->fanFixedPercent);
    if (!control_state_has_any_meaningful_value(state)) {
        debug_log("apply_control_state_to_gui: ignoring non-meaningful service control update\n");
        return;
    }

    ControlState merged = {};
    if (g_app.serviceControlStateValid) merged = g_app.serviceControlState;
    merged.valid = true;
    if (state->hasGpuOffset && meaningfulGpuState) {
        merged.hasGpuOffset = true;
        merged.gpuOffsetMHz = state->gpuOffsetMHz;
        merged.gpuOffsetExcludeLowCount = (state->gpuOffsetExcludeLowCount > 0 && state->gpuOffsetMHz != 0) ? state->gpuOffsetExcludeLowCount : 0;
        debug_log("apply_control_state_to_gui: merged service gpu=%d exclude=%d\n",
            merged.gpuOffsetMHz,
            merged.gpuOffsetExcludeLowCount);
    }
    if (state->hasMemOffset && meaningfulMemState) {
        merged.hasMemOffset = true;
        merged.memOffsetMHz = state->memOffsetMHz;
    }
    if (state->hasPowerLimit && meaningfulPowerState) {
        merged.hasPowerLimit = true;
        merged.powerLimitPct = state->powerLimitPct;
    }
    if (state->hasFan && meaningfulFanState) {
        merged.hasFan = true;
        merged.fanMode = state->fanMode;
        merged.fanFixedPercent = state->fanFixedPercent;
        merged.fanCurrentPercent = state->fanCurrentPercent;
        merged.fanCurrentTemperatureC = state->fanCurrentTemperatureC;
        copy_fan_curve(&merged.fanCurve, &state->fanCurve);
        ensure_valid_fan_curve_config(&merged.fanCurve);
    }
    g_app.serviceControlStateValid = true;
    g_app.serviceControlState = merged;
    bool updateGui = !gui_state_dirty();
    if (meaningfulGpuState) {
        g_app.appliedGpuOffsetMHz = state->gpuOffsetMHz;
        g_app.appliedGpuOffsetExcludeLowCount = (state->gpuOffsetExcludeLowCount > 0 && state->gpuOffsetMHz != 0) ? state->gpuOffsetExcludeLowCount : 0;
        if (updateGui) {
            g_app.guiGpuOffsetMHz = state->gpuOffsetMHz;
            g_app.guiGpuOffsetExcludeLowCount = (state->gpuOffsetExcludeLowCount > 0 && state->gpuOffsetMHz != 0) ? state->gpuOffsetExcludeLowCount : 0;
        }
    }
    if (meaningfulMemState) {
        g_app.memClockOffsetkHz = mem_driver_khz_from_display_mhz(state->memOffsetMHz);
    }
    if (meaningfulPowerState) {
        g_app.powerLimitPct = state->powerLimitPct;
    }
    if (meaningfulFanState) {
        g_app.activeFanMode = state->fanMode;
        if (updateGui) {
            g_app.guiFanMode = state->fanMode;
        }
        if (state->fanMode == FAN_MODE_FIXED) {
            g_app.activeFanFixedPercent = clamp_percent(state->fanFixedPercent);
            if (updateGui) g_app.guiFanFixedPercent = g_app.activeFanFixedPercent;
        } else {
            int currentPercent = state->fanCurrentPercent > 0 ? state->fanCurrentPercent : current_displayed_fan_percent();
            g_app.activeFanFixedPercent = clamp_percent(currentPercent);
            if (updateGui) g_app.guiFanFixedPercent = clamp_percent(currentPercent);
        }
        copy_fan_curve(&g_app.activeFanCurve, &state->fanCurve);
        ensure_valid_fan_curve_config(&g_app.activeFanCurve);
        if (updateGui) {
            copy_fan_curve(&g_app.guiFanCurve, &state->fanCurve);
            ensure_valid_fan_curve_config(&g_app.guiFanCurve);
        }
        g_app.fanIsAuto = state->fanMode == FAN_MODE_AUTO;
        g_app.fanCurveRuntimeActive = state->fanMode == FAN_MODE_CURVE;
        g_app.fanFixedRuntimeActive = state->fanMode == FAN_MODE_FIXED;
    }
}

static bool get_effective_control_state(ControlState* stateOut) {
    if (!stateOut) return false;
    memset(stateOut, 0, sizeof(*stateOut));
    if (g_app.usingBackgroundService && g_app.serviceControlStateValid && control_state_has_any_meaningful_value(&g_app.serviceControlState)) {
        *stateOut = g_app.serviceControlState;
        debug_log("get_effective_control_state: using cached service state gpu=%d exclude=%d fanMode=%d\n",
            stateOut->gpuOffsetMHz,
            stateOut->gpuOffsetExcludeLowCount,
            stateOut->fanMode);
        return stateOut->valid;
    }
    if (g_app.isServiceProcess && g_serviceControlStateValid && control_state_has_any_meaningful_value(&g_serviceControlState)) {
        *stateOut = g_serviceControlState;
        debug_log("get_effective_control_state: using service-local state gpu=%d exclude=%d fanMode=%d\n",
            stateOut->gpuOffsetMHz,
            stateOut->gpuOffsetExcludeLowCount,
            stateOut->fanMode);
        return stateOut->valid;
    }
    populate_control_state(stateOut);
    debug_log("get_effective_control_state: using local state gpu=%d exclude=%d fanMode=%d\n",
        stateOut->gpuOffsetMHz,
        stateOut->gpuOffsetExcludeLowCount,
        stateOut->fanMode);
    return stateOut->valid;
}

