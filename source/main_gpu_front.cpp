static UINT fan_telemetry_interval_for_window_state() {
    if (!g_app.backgroundServiceAvailable) return 2000;
    return FAN_TELEMETRY_INTERVAL_MS;
}
static const ULONGLONG FAN_RUNTIME_REAPPLY_INTERVAL_MS = 15000;
static const ULONGLONG FAN_RUNTIME_FAILURE_WINDOW_MS = 10000;
static const char* gpu_family_name(GpuFamily family) {
    switch (family) {
        case GPU_FAMILY_PASCAL: return "pascal";
        case GPU_FAMILY_TURING: return "turing";
        case GPU_FAMILY_AMPERE: return "ampere";
        case GPU_FAMILY_LOVELACE: return "lovelace";
        case GPU_FAMILY_BLACKWELL: return "blackwell";
        default: return "unknown";
    }
}
static bool select_vf_backend_for_current_gpu() {
    g_app.vfBackend = nullptr;
    g_app.gpuFamily = GPU_FAMILY_UNKNOWN;
    switch (g_app.gpuArchitecture) {
        case NV_GPU_ARCHITECTURE_GP100:
            g_app.gpuFamily = GPU_FAMILY_PASCAL;
            g_app.vfBackend = &g_vfBackendPascal;
            return true;
        case NV_GPU_ARCHITECTURE_TU100:
            g_app.gpuFamily = GPU_FAMILY_TURING;
            g_app.vfBackend = &g_vfBackendTuring;
            return true;
        case NV_GPU_ARCHITECTURE_GA100:
            g_app.gpuFamily = GPU_FAMILY_AMPERE;
            g_app.vfBackend = &g_vfBackendAmpere;
            return true;
        case NV_GPU_ARCHITECTURE_AD100:
            g_app.gpuFamily = GPU_FAMILY_LOVELACE;
            g_app.vfBackend = &g_vfBackendLovelace;
            return true;
        case NV_GPU_ARCHITECTURE_GB200:
            g_app.gpuFamily = GPU_FAMILY_BLACKWELL;
            g_app.vfBackend = &g_vfBackendBlackwell;
            return true;
        default:
            debug_log("select_vf_backend_for_current_gpu: unrecognized architecture 0x%08X, using future backend\n",
                g_app.gpuArchitecture);
            g_app.gpuFamily = GPU_FAMILY_UNKNOWN;
            g_app.vfBackend = &g_vfBackendFuture;
            return true;
    }
}
static const VfBackendSpec* probe_backend_for_current_gpu() {
    if (g_app.vfBackend) return g_app.vfBackend;
    return &g_vfBackendBlackwell;
}
static bool nvapi_get_interface_version_string(char* text, size_t textSize) {
    if (!text || textSize == 0) return false;
    text[0] = 0;
    typedef int (*version_t)(char*);
    auto getVersion = (version_t)nvapi_qi(NVAPI_GET_INTERFACE_VERSION_STRING_ID);
    if (!getVersion) return false;
    return getVersion(text) == 0;
}
static bool nvapi_get_error_message(int status, char* text, size_t textSize) {
    if (!text || textSize == 0) return false;
    text[0] = 0;
    typedef int (*error_message_t)(int, char*);
    auto getErrorMessage = (error_message_t)nvapi_qi(NVAPI_GET_ERROR_MESSAGE_ID);
    if (!getErrorMessage) return false;
    char shortText[64] = {};
    if (getErrorMessage(status, shortText) != 0) return false;
    StringCchCopyA(text, textSize, shortText);
    return true;
}
static bool nvapi_read_gpu_metadata() {
    g_app.gpuArchInfoValid = false;
    g_app.gpuPciInfoValid = false;
    g_app.gpuArchitecture = 0;
    g_app.gpuImplementation = 0;
    g_app.gpuChipRevision = 0;
    g_app.gpuDeviceId = 0;
    g_app.gpuSubSystemId = 0;
    g_app.gpuPciRevisionId = 0;
    g_app.gpuExtDeviceId = 0;
    typedef int (*get_arch_t)(GPU_HANDLE, nvapiGpuArchInfo_t*);
    auto getArchInfo = (get_arch_t)nvapi_qi(NVAPI_GPU_GET_ARCH_INFO_ID);
    if (getArchInfo) {
        nvapiGpuArchInfo_t info = {};
        info.version = NVAPI_GPU_ARCH_INFO_VER2;
        if (getArchInfo(g_app.gpuHandle, &info) == 0) {
            g_app.gpuArchitecture = info.architecture;
            g_app.gpuImplementation = info.implementation;
            g_app.gpuChipRevision = info.revision;
            g_app.gpuArchInfoValid = true;
        }
    }
    typedef int (*get_pci_t)(GPU_HANDLE, unsigned int*, unsigned int*, unsigned int*, unsigned int*);
    auto getPciIdentifiers = (get_pci_t)nvapi_qi(NVAPI_GPU_GET_PCI_IDENTIFIERS_ID);
    if (getPciIdentifiers) {
        if (getPciIdentifiers(
                g_app.gpuHandle,
                &g_app.gpuDeviceId,
                &g_app.gpuSubSystemId,
                &g_app.gpuPciRevisionId,
                &g_app.gpuExtDeviceId) == 0) {
            g_app.gpuPciInfoValid = true;
        }
    }
    return select_vf_backend_for_current_gpu() || g_app.gpuArchInfoValid || g_app.gpuPciInfoValid;
}
static const char* fan_mode_label(int mode) {
    switch (mode) {
        case FAN_MODE_FIXED: return "Fixed Custom";
        case FAN_MODE_CURVE: return "Custom Curve";
        default: return "Default / Auto";
    }
}
static const char* tray_mode_label(bool customOc, bool customFan) {
    if (customOc && customFan) return "OC + Custom Fan";
    if (customOc) return "OC";
    if (customFan) return "Custom Fan";
    return "Default";
}
void invalidate_tray_profile_cache() {
    g_app.trayProfileCacheValid = false;
    g_app.trayLastRenderedValid = false;
    g_app.trayLastRenderedState = TRAY_ICON_STATE_DEFAULT;
    g_app.trayProfileCacheProfilePart[0] = 0;
    g_app.trayLastRenderedTip[0] = 0;
}
static void clear_last_operation_details() {
    g_lastOperationIntent[0] = 0;
    g_lastOperationPlan[0] = 0;
    g_lastOperationBeforeSnapshot[0] = 0;
    g_lastOperationAfterSnapshot[0] = 0;
}
static void set_pending_operation_source(const char* source) {
    StringCchCopyA(g_pendingOperationSource, ARRAY_COUNT(g_pendingOperationSource),
        (source && source[0]) ? source : "unspecified");
}
static void record_ui_action(const char* fmt, ...) {
    if (!fmt || !fmt[0]) return;
    char entry[96] = {};
    va_list ap;
    va_start(ap, fmt);
    StringCchVPrintfA(entry, ARRAY_COUNT(entry), fmt, ap);
    va_end(ap);
    if (!entry[0]) return;
    StringCchCopyA(g_recentUiActions[g_recentUiActionNext], ARRAY_COUNT(g_recentUiActions[0]), entry);
    g_recentUiActionNext = (g_recentUiActionNext + 1) % ARRAY_COUNT(g_recentUiActions);
    if (g_recentUiActionCount < ARRAY_COUNT(g_recentUiActions)) g_recentUiActionCount++;
    debug_log("ui action: %s\n", entry);
}
static void build_recent_ui_actions_text(char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = 0;
    if (g_recentUiActionCount == 0) {
        StringCchCopyA(out, outSize, "none");
        return;
    }
    unsigned int capacity = (unsigned int)ARRAY_COUNT(g_recentUiActions);
    unsigned int start = (g_recentUiActionNext + capacity - g_recentUiActionCount) % capacity;
    for (unsigned int i = 0; i < g_recentUiActionCount; i++) {
        unsigned int index = (start + i) % capacity;
        if (out[0]) {
            StringCchCatA(out, outSize, " | ");
        }
        StringCchCatA(out, outSize, g_recentUiActions[index]);
    }
}
static void build_point_list_from_flags(const bool* flags, char* out, size_t outSize, int maxItems) {
    if (!out || outSize == 0) return;
    out[0] = 0;
    if (!flags) {
        StringCchCopyA(out, outSize, "none");
        return;
    }
    int count = 0;
    int omitted = 0;
    char part[32] = {};
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (!flags[ci]) continue;
        count++;
        if (count > maxItems) {
            omitted++;
            continue;
        }
        StringCchPrintfA(part, ARRAY_COUNT(part), "%s%d", out[0] ? ", " : "", ci);
        StringCchCatA(out, outSize, part);
    }
    if (count == 0) {
        StringCchCopyA(out, outSize, "none");
        return;
    }
    if (omitted > 0) {
        StringCchPrintfA(part, ARRAY_COUNT(part), ", ... (+%d)", omitted);
        StringCchCatA(out, outSize, part);
    }
}
static void describe_live_gpu_offset_state(char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    int selectiveMHz = 0;
    bool selective = detect_live_selective_gpu_offset_state(&selectiveMHz);
    int uniformMHz = g_app.gpuClockOffsetkHz / 1000;
    bool mixedCurveOffsets = false;
    int firstNonZeroCi = -1;
    int firstNonZeroOffsetkHz = 0;
    int referenceOffsetkHz = 0;
    bool haveReference = false;
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (g_app.curve[ci].freq_kHz == 0) continue;
        int offsetkHz = g_app.freqOffsets[ci];
        if (offsetkHz != 0 && firstNonZeroCi < 0) {
            firstNonZeroCi = ci;
            firstNonZeroOffsetkHz = offsetkHz;
        }
        if (!haveReference) {
            referenceOffsetkHz = offsetkHz;
            haveReference = true;
        } else if (offsetkHz != referenceOffsetkHz) {
            mixedCurveOffsets = true;
        }
    }
    if (selective) {
        StringCchPrintfA(out, outSize, "selective %d MHz (exclude low 70)", selectiveMHz);
    } else if (mixedCurveOffsets && firstNonZeroCi >= 0) {
        StringCchPrintfA(out, outSize,
            "mixed/custom VF state (uniform readback %d MHz, first non-zero point %d = %d kHz)",
            uniformMHz,
            firstNonZeroCi,
            firstNonZeroOffsetkHz);
    } else {
        StringCchPrintfA(out, outSize, "uniform %d MHz", uniformMHz);
    }
}
static void build_operation_intent_summary(const DesiredSettings* desired, bool interactive, char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = 0;
    bool explicitCurveFlags[VF_NUM_POINTS] = {};
    int curvePointCount = 0;
    if (desired) {
        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
            explicitCurveFlags[ci] = desired->hasCurvePoint[ci];
            if (desired->hasCurvePoint[ci]) curvePointCount++;
        }
    }
    char explicitPoints[256] = {};
    build_point_list_from_flags(explicitCurveFlags, explicitPoints, sizeof(explicitPoints));
    char recentActions[768] = {};
    build_recent_ui_actions_text(recentActions, sizeof(recentActions));
    char liveOffsetState[256] = {};
    describe_live_gpu_offset_state(liveOffsetState, sizeof(liveOffsetState));
    int displayGpuOffsetMHz = 0;
    int displayGpuOffsetExcludeLowCount = 0;
    resolve_displayed_live_gpu_offset_state_for_gui(&displayGpuOffsetMHz, &displayGpuOffsetExcludeLowCount);
    char gpuOffsetText[96] = {};
    if (desired && desired->hasGpuOffset) {
        StringCchPrintfA(gpuOffsetText, ARRAY_COUNT(gpuOffsetText), "%d MHz", desired->gpuOffsetMHz);
    } else {
        StringCchPrintfA(gpuOffsetText, ARRAY_COUNT(gpuOffsetText), "unchanged (%d MHz)", displayGpuOffsetMHz);
    }
    char lockText[96] = {};
    if (desired && desired->hasLock) {
        StringCchPrintfA(lockText, ARRAY_COUNT(lockText), "point %d @ %u MHz (%s)", desired->lockCi, desired->lockMHz,
            desired->lockTracksAnchor ? "track anchor" : "absolute");
    } else {
        StringCchCopyA(lockText, ARRAY_COUNT(lockText), "unchanged / none");
    }
    char memText[96] = {};
    if (desired && desired->hasMemOffset) {
        StringCchPrintfA(memText, ARRAY_COUNT(memText), "%d MHz", desired->memOffsetMHz);
    } else {
        StringCchPrintfA(memText, ARRAY_COUNT(memText), "unchanged (%d MHz)", mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz));
    }
    char powerText[96] = {};
    if (desired && desired->hasPowerLimit) {
        StringCchPrintfA(powerText, ARRAY_COUNT(powerText), "%d%%", desired->powerLimitPct);
    } else {
        StringCchPrintfA(powerText, ARRAY_COUNT(powerText), "unchanged (%d%%)", g_app.powerLimitPct);
    }
    char fanText[96] = {};
    if (desired && desired->hasFan) {
        if (desired->fanMode == FAN_MODE_AUTO) {
            StringCchCopyA(fanText, ARRAY_COUNT(fanText), "auto");
        } else if (desired->fanMode == FAN_MODE_FIXED) {
            StringCchPrintfA(fanText, ARRAY_COUNT(fanText), "fixed %d%%", desired->fanPercent);
        } else {
            StringCchCopyA(fanText, ARRAY_COUNT(fanText), "curve");
        }
    } else {
        StringCchCopyA(fanText, ARRAY_COUNT(fanText), "unchanged");
    }
    unsigned int graphicsClock = 0;
    unsigned int memClock = 0;
    bool haveGraphicsClock = false;
    bool haveMemClock = false;
    if (nvml_ensure_ready() && g_nvml_api.getClock) {
        if (g_nvml_api.getClock(g_app.nvmlDevice, NVML_CLOCK_GRAPHICS, NVML_CLOCK_ID_CURRENT, &graphicsClock) == NVML_SUCCESS) {
            haveGraphicsClock = true;
        }
        if (g_nvml_api.getClock(g_app.nvmlDevice, NVML_CLOCK_MEM, NVML_CLOCK_ID_CURRENT, &memClock) == NVML_SUCCESS) {
            haveMemClock = true;
        }
    }
    char clockText[128] = {};
    if (haveGraphicsClock || haveMemClock) {
        StringCchPrintfA(clockText, ARRAY_COUNT(clockText),
            "graphics=%s memory=%s",
            haveGraphicsClock ? "available" : "unavailable",
            haveMemClock ? "available" : "unavailable");
        if (haveGraphicsClock || haveMemClock) {
            char detail[64] = {};
            StringCchPrintfA(detail, ARRAY_COUNT(detail), " (%s%u MHz%s%s%u MHz)",
                haveGraphicsClock ? "gfx=" : "",
                haveGraphicsClock ? graphicsClock : 0,
                haveGraphicsClock && haveMemClock ? ", " : "",
                haveMemClock ? "mem=" : "",
                haveMemClock ? memClock : 0);
            StringCchCatA(clockText, ARRAY_COUNT(clockText), detail);
        }
    } else {
        StringCchCopyA(clockText, ARRAY_COUNT(clockText), "unavailable");
    }
    StringCchPrintfA(out, outSize,
        "Source: %s\r\n"
        "Interactive: %s\r\n"
        "Requested GPU offset: %s\r\n"
        "Requested selective GPU offset: %s\r\n"
        "Requested lock: %s\r\n"
        "Requested explicit curve points: %d (%s)\r\n"
        "Requested mem offset: %s\r\n"
        "Requested power limit: %s\r\n"
        "Requested fan: %s\r\n"
        "Reset OC baseline before apply: %s\r\n"
        "Live GPU offset state: %s\r\n"
        "GUI display GPU offset: %d MHz (exclude low 70: %s)\r\n"
        "Live clocks: %s\r\n"
        "Recent UI actions: %s\r\n",
        g_pendingOperationSource[0] ? g_pendingOperationSource : (interactive ? "GUI apply" : "non-interactive apply"),
        interactive ? "yes" : "no",
        gpuOffsetText,
        (desired && desired->hasGpuOffset) ? (desired->gpuOffsetExcludeLowCount > 0 ? "yes" : "no") : "unchanged",
        lockText,
        curvePointCount,
        explicitPoints,
        memText,
        powerText,
        fanText,
        (desired && desired->resetOcBeforeApply) ? "yes" : "no",
        liveOffsetState,
        displayGpuOffsetMHz,
        displayGpuOffsetExcludeLowCount > 0 ? "yes" : "no",
        clockText,
        recentActions);
}
static bool build_state_snapshot_text(char* text, size_t textSize) {
    if (!text || textSize == 0) return false;
    text[0] = 0;
    size_t used = 0;
    auto appendf = [&used, text, textSize](const char* fmt, ...) -> bool {
        if (used >= textSize) return false;
        va_list ap;
        va_start(ap, fmt);
        int written = _vsnprintf_s(text + used, textSize - used, _TRUNCATE, fmt, ap);
        va_end(ap);
        if (written < 0) {
            used = textSize - 1;
            text[textSize - 1] = 0;
            return false;
        }
        used += (size_t)written;
        return true;
    };
    char liveOffsetState[256] = {};
    describe_live_gpu_offset_state(liveOffsetState, sizeof(liveOffsetState));
    int displayGpuOffsetMHz = 0;
    int displayGpuOffsetExcludeLowCount = 0;
    resolve_displayed_live_gpu_offset_state_for_gui(&displayGpuOffsetMHz, &displayGpuOffsetExcludeLowCount);
    unsigned int liveLockPointMHz = 0;
    unsigned int liveLockPointMv = 0;
    if (g_app.lockedCi >= 0 && g_app.lockedCi < VF_NUM_POINTS && g_app.curve[g_app.lockedCi].freq_kHz > 0) {
        liveLockPointMHz = displayed_curve_mhz(g_app.curve[g_app.lockedCi].freq_kHz);
        liveLockPointMv = g_app.curve[g_app.lockedCi].volt_uV / 1000;
    }
    appendf("GPU: %s\r\n", g_app.gpuName);
    appendf("Populated points: %d\r\n", g_app.numPopulated);
    appendf("Live GPU offset state: %s\r\n", liveOffsetState);
    appendf("Derived GPU offset: %d MHz\r\n", g_app.gpuClockOffsetkHz / 1000);
    appendf("GUI display GPU offset: %d MHz (exclude low 70: %s)\r\n", displayGpuOffsetMHz, displayGpuOffsetExcludeLowCount > 0 ? "yes" : "no");
    appendf("GUI lock state: ci=%d storedMHz=%u livePointMHz=%u livePointmV=%u trackAnchor=%s\r\n",
        g_app.lockedCi,
        g_app.lockedFreq,
        liveLockPointMHz,
        liveLockPointMv,
        g_app.guiLockTracksAnchor ? "yes" : "no");
    appendf("Mem offset: %d MHz\r\n", mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz));
    appendf("Power limit: %d%%\r\n", g_app.powerLimitPct);
    appendf("Fan: %s\r\n", g_app.fanIsAuto ? "auto" : "manual");
    appendf("\r\n%-6s  %-10s  %-10s  %-12s\r\n", "Point", "Freq(MHz)", "Volt(mV)", "Offset(kHz)");
    appendf("------  ----------  ----------  ------------\r\n");
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (g_app.curve[i].freq_kHz == 0 && g_app.curve[i].volt_uV == 0) continue;
        appendf("%-6d  %-10u  %-10u  %-12d\r\n",
            i,
            displayed_curve_mhz(g_app.curve[i].freq_kHz),
            g_app.curve[i].volt_uV / 1000,
            g_app.freqOffsets[i]);
    }
    return true;
}
static void capture_last_operation_snapshot(char* dst, size_t dstSize) {
    if (!dst || dstSize == 0) return;
    if (!build_state_snapshot_text(dst, dstSize)) {
        StringCchCopyA(dst, dstSize, "Unavailable");
    }
}
static void begin_programmatic_edit_update() {
    g_programmaticEditUpdateDepth++;
}
static void end_programmatic_edit_update() {
    if (g_programmaticEditUpdateDepth > 0) g_programmaticEditUpdateDepth--;
}
static bool programmatic_edit_update_active() {
    return g_programmaticEditUpdateDepth > 0;
}
static bool gui_has_pending_curve_or_lock_edits() {
    if (g_app.lockedVi >= 0 && g_app.lockedVi < g_app.numVisible && g_app.lockedFreq > 0) return true;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (g_app.guiCurvePointExplicit[i]) return true;
    }
    return false;
}
static bool gui_has_pending_global_edits() {
    return g_app.guiStateDirty;
}
static void ensure_tray_profile_cache() {
    if (g_app.trayProfileCacheValid) return;
    g_app.trayProfileCacheValid = true;
    g_app.trayProfileCacheProfilePart[0] = 0;
    int selectedSlot = CONFIG_DEFAULT_SLOT;
    bool hasConfigPath = g_app.configPath[0] != '\0';
    if (hasConfigPath) {
        selectedSlot = get_config_int(g_app.configPath, "profiles", "selected_slot", CONFIG_DEFAULT_SLOT);
    }
    if (selectedSlot < 1 || selectedSlot > CONFIG_NUM_SLOTS) {
        selectedSlot = CONFIG_DEFAULT_SLOT;
    }
    if (!hasConfigPath) {
        StringCchPrintfA(
            g_app.trayProfileCacheProfilePart,
            ARRAY_COUNT(g_app.trayProfileCacheProfilePart),
            "Profile %d",
            selectedSlot);
        return;
    }
    bool hasSavedProfile = is_profile_slot_saved(g_app.configPath, selectedSlot);
    StringCchPrintfA(
        g_app.trayProfileCacheProfilePart,
        ARRAY_COUNT(g_app.trayProfileCacheProfilePart),
        "Profile %d (%s)",
        selectedSlot,
        hasSavedProfile ? "saved" : "empty");
}
static void build_tray_tooltip(char* tip, size_t tipSize) {
    if (!tip || tipSize == 0) return;
    ensure_tray_profile_cache();
    char mode[64] = {};
    bool customOc = live_state_has_custom_oc();
    bool customFan = live_state_has_custom_fan();
    StringCchCopyA(mode, ARRAY_COUNT(mode), tray_mode_label(customOc, customFan));
    const char* profilePart = g_app.trayProfileCacheProfilePart[0]
        ? g_app.trayProfileCacheProfilePart
        : "Profile 1";
    StringCchPrintfA(tip, tipSize, "Green Curve - %s | %s", mode, profilePart);
}
static int clamp_percent(int value) {
    if (value < 0) return 0;
    if (value > 100) return 100;
    return value;
}
static int current_displayed_fan_percent() {
    if (!g_app.fanCount) return 0;
    if (g_app.fanPercent[0] > 0) return (int)g_app.fanPercent[0];
    if (g_app.fanTargetPercent[0] > 0) return (int)g_app.fanTargetPercent[0];
    return 0;
}
static int current_manual_fan_target_percent() {
    if (!g_app.fanCount) return 0;
    if (g_app.fanTargetPercent[0] > 0) return (int)g_app.fanTargetPercent[0];
    return current_displayed_fan_percent();
}
static bool window_should_redraw_fan_controls() {
    if (!g_app.hMainWnd || !IsWindowVisible(g_app.hMainWnd)) return false;
    return g_app.guiFanMode != FAN_MODE_FIXED || GetFocus() != g_app.hFanEdit;
}
static void boost_fan_telemetry_for_ms(DWORD durationMs) {
    ULONGLONG now = GetTickCount64();
    ULONGLONG until = now + durationMs;
    if (until > g_fanTelemetryBoostUntilTickMs) {
        g_fanTelemetryBoostUntilTickMs = until;
    }
    update_fan_telemetry_timer();
}
static void sync_fan_ui_from_cached_state(bool redrawControls) {
    EnterCriticalSection(&g_appLock);
    initialize_gui_fan_settings_from_live_state(false);
    update_tray_icon();
    if (redrawControls) {
        update_fan_controls_enabled_state();
    }
    LeaveCriticalSection(&g_appLock);
}
static void update_all_gui_for_service_state() {
#ifdef GREEN_CURVE_SERVICE_BINARY
    populate_global_controls();
#else
    if (g_app.numVisible > 0 && !g_app.hEditsMhz[0]) {
        destroy_edit_controls(g_app.hMainWnd);
        create_edit_controls(g_app.hMainWnd, g_app.hInst);
    } else if (g_app.hEditsMhz[0]) {
        populate_edits();
    } else {
        populate_global_controls();
    }
#endif
    update_background_service_controls();
    invalidate_main_window();
}
static void update_fan_telemetry_timer() {
    if (!g_app.hMainWnd) return;
    KillTimer(g_app.hMainWnd, FAN_TELEMETRY_TIMER_ID);
    if (!IsWindowVisible(g_app.hMainWnd)) return;
    UINT intervalMs = fan_telemetry_interval_for_window_state();
    ULONGLONG now = GetTickCount64();
    if (g_fanTelemetryBoostUntilTickMs > now) {
        intervalMs = nvmin(intervalMs, (UINT)300);
    }
    SetTimer(g_app.hMainWnd, FAN_TELEMETRY_TIMER_ID, intervalMs, nullptr);
}
static bool fan_manual_control_available(char* detail, size_t detailSize) {
    if (!nvml_ensure_ready()) {
        set_message(detail, detailSize, "NVML not ready");
        return false;
    }
    if (!g_app.fanSupported || g_app.fanCount == 0) {
        char refreshDetail[128] = {};
        if (!nvml_read_fans(refreshDetail, sizeof(refreshDetail))) {
            set_message(detail, detailSize, "%s", refreshDetail[0] ? refreshDetail : "Manual fan control unsupported on this GPU");
            return false;
        }
    }
    if (!g_app.fanSupported || g_app.fanCount == 0) {
        set_message(detail, detailSize, "Manual fan control unsupported on this GPU");
        return false;
    }
    if (!g_nvml_api.setFanSpeed) {
        set_message(detail, detailSize, "Manual fan control unsupported by this NVIDIA driver");
        return false;
    }
    return true;
}
static bool validate_manual_fan_percent_for_runtime(int pct, char* detail, size_t detailSize) {
    if (pct < 0 || pct > 100) {
        set_message(detail, detailSize, "Requested %d%% is outside the valid range 0..100%%", pct);
        return false;
    }
    if (pct == 0) {
        if (g_app.fanRangeKnown && g_app.fanMinPct == 0) return true;
        if (g_app.fanRangeKnown) {
            set_message(detail, detailSize,
                "Requested 0%% manual fan is blocked because the GPU reports a supported range of %u..%u%%",
                g_app.fanMinPct,
                g_app.fanMaxPct);
        } else {
            set_message(detail, detailSize,
                "Requested 0%% manual fan is blocked because the GPU did not report zero-speed support");
        }
        return false;
    }
    if (!g_app.fanRangeKnown) return true;
    if (pct < (int)g_app.fanMinPct || pct > (int)g_app.fanMaxPct) {
        set_message(detail, detailSize,
            "Requested %d%% is outside the supported range %u..%u%%",
            pct,
            g_app.fanMinPct,
            g_app.fanMaxPct);
        return false;
    }
    return true;
}
static bool validate_fan_curve_for_runtime(const FanCurveConfig* curve, char* detail, size_t detailSize) {
    if (!curve) {
        set_message(detail, detailSize, "No fan curve config");
        return false;
    }
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        if (!curve->points[i].enabled) continue;
        char pointDetail[256] = {};
        if (!validate_manual_fan_percent_for_runtime(curve->points[i].fanPercent, pointDetail, sizeof(pointDetail))) {
            set_message(detail, detailSize, "Fan curve point %d is invalid: %s", i + 1, pointDetail);
            return false;
        }
    }
    return true;
}
static bool manual_fan_readback_matches_target(int wantPct, int actualPct, unsigned int requestedPct) {
    if (wantPct == 0) return actualPct == 0;
    if (actualPct >= wantPct - 2 && actualPct <= wantPct + 2) return true;
    int requested = (int)requestedPct;
    if (requested <= 0) return false;
    return requested >= wantPct - 2 && requested <= wantPct + 2;
}
static bool is_gpu_offset_excluded_low_point(int pointIndex, int gpuOffsetMHz, int excludeLowCount) {
    if (excludeLowCount <= 0) return false;
    if (pointIndex < 0 || pointIndex >= VF_NUM_POINTS) return false;
    if (g_app.curve[pointIndex].freq_kHz == 0) return false;
    (void)gpuOffsetMHz;
    int populatedCount = 0;
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (g_app.curve[ci].freq_kHz == 0) continue;
        if (ci == pointIndex) return true;
        populatedCount++;
        if (populatedCount >= excludeLowCount) return false;
    }
    return true;
}
static int gpu_offset_component_mhz_for_point(int pointIndex, int gpuOffsetMHz, int excludeLowCount) {
    if (gpuOffsetMHz == 0) return 0;
    if (excludeLowCount > 0 && is_gpu_offset_excluded_low_point(pointIndex, gpuOffsetMHz, excludeLowCount)) return 0;
    return gpuOffsetMHz;
}
static bool vf_backend_is_best_guess(const VfBackendSpec* backend) {
    return backend && backend->bestGuessOnly && gpu_family_uses_best_guess_backend(backend->family);
}
static bool should_show_best_guess_warning() {
    if (!g_app.vfBackend || !vf_backend_is_best_guess(g_app.vfBackend)) return false;
    if (!g_app.configPath[0]) return true;
    char key[64] = {};
    StringCchPrintfA(key, ARRAY_COUNT(key), "hide_best_guess_warning_%s", gpu_family_name(g_app.gpuFamily));
    return get_config_int(g_app.configPath, "warnings", key, 0) == 0;
}
static void rollback_to_safe_defaults() {
    debug_log("rollback_to_safe_defaults: partial apply detected, reverting to safe defaults\n");
    // Reset VF curve offsets to zero.
    int resetOffsets[VF_NUM_POINTS] = {};
    bool resetMask[VF_NUM_POINTS] = {};
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (g_app.curve[ci].freq_kHz == 0) continue;
        resetMask[ci] = true;
    }
    bool hadCurveOffsets = false;
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (g_app.freqOffsets[ci] != 0) {
            hadCurveOffsets = true;
            break;
        }
    }
    if (hadCurveOffsets) {
        if (!apply_curve_offsets_verified(resetOffsets, resetMask, 2)) {
            debug_log("rollback: VF curve offsets did not reset cleanly\n");
        }
    }
    // Reset GPU offset.
    if (g_app.gpuClockOffsetkHz != 0) {
        if (!nvapi_set_gpu_offset(0)) {
            debug_log("rollback: GPU offset did not reset to default\n");
        }
    }
    // Reset memory offset.
    if (g_app.memClockOffsetkHz != 0) {
        if (!nvapi_set_mem_offset(0)) {
            debug_log("rollback: Memory offset did not reset to default\n");
        }
    }
    // Reset power limit to default.
    if (g_app.powerLimitPct != 100) {
        if (!nvapi_set_power_limit(100)) {
            debug_log("rollback: Power limit did not reset to default\n");
        }
    }
    // Stop fan runtime and return to driver auto.
    stop_fan_curve_runtime();
    if (g_app.isServiceProcess && g_serviceFanThread) {
        stop_service_fan_runtime_thread();
    }
    if (!g_app.fanIsAuto || g_app.activeFanMode != FAN_MODE_AUTO) {
        char fanDetail[128] = {};
        if (nvml_set_fan_auto(fanDetail, sizeof(fanDetail))) {
            g_app.fanIsAuto = true;
            g_app.activeFanMode = FAN_MODE_AUTO;
            g_app.activeFanFixedPercent = 0;
        } else {
            debug_log("rollback: Fan control did not return to driver auto: %s\n",
                fanDetail[0] ? fanDetail : "");
        }
    }
    clear_runtime_selective_gpu_offset_request();
    // Also clear in-memory selective offset state so the GUI does not
    // display stale values after rollback.
    g_app.appliedGpuOffsetExcludeLowCount = 0;
    g_app.appliedGpuOffsetMHz = 0;
}
