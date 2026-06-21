static void copy_fan_curve(FanCurveConfig* destination, const FanCurveConfig* source) {
    if (!destination || !source) return;
    memcpy(destination, source, sizeof(*destination));
}
static void ensure_valid_fan_curve_config(FanCurveConfig* curve) {
    if (!curve) return;
    if (curve->pollIntervalMs == 0) {
        fan_curve_set_default(curve);
        if (g_app.fanRangeKnown) {
            fan_curve_clamp_percentages(curve, (int)g_app.fanMinPct, (int)g_app.fanMaxPct);
        }
        return;
    }
    fan_curve_normalize(curve);
    if (g_app.fanRangeKnown) {
        fan_curve_clamp_percentages(curve, (int)g_app.fanMinPct, (int)g_app.fanMaxPct);
    }
    char err[256] = {};
    if (!fan_curve_validate(curve, err, sizeof(err))) {
        fan_curve_set_default(curve);
        if (g_app.fanRangeKnown) {
            fan_curve_clamp_percentages(curve, (int)g_app.fanMinPct, (int)g_app.fanMaxPct);
        }
    }
}
static int get_effective_live_fan_mode() {
    if (g_app.fanCurveRuntimeActive) return FAN_MODE_CURVE;
    return g_app.fanIsAuto ? FAN_MODE_AUTO : FAN_MODE_FIXED;
}
static void initialize_gui_fan_settings_from_live_state(bool syncGuiCurve) {
    ensure_valid_fan_curve_config(&g_app.guiFanCurve);
    ensure_valid_fan_curve_config(&g_app.activeFanCurve);
    if (!g_app.serviceSnapshotAuthoritative) {
        g_app.activeFanMode = get_effective_live_fan_mode();
    }
    if (g_app.activeFanMode == FAN_MODE_FIXED) {
        g_app.activeFanFixedPercent = current_manual_fan_target_percent();
    }
    if (g_app.guiFanMode < FAN_MODE_AUTO || g_app.guiFanMode > FAN_MODE_CURVE) {
        g_app.guiFanMode = g_app.activeFanMode;
    }
    if (g_app.guiFanMode == FAN_MODE_FIXED) {
        if (g_app.guiFanFixedPercent <= 0) {
            g_app.guiFanFixedPercent = g_app.activeFanFixedPercent > 0 ? g_app.activeFanFixedPercent : 50;
        }
    } else {
        g_app.guiFanFixedPercent = current_displayed_fan_percent();
    }
    g_app.guiFanFixedPercent = clamp_percent(g_app.guiFanFixedPercent);
    if (syncGuiCurve && g_app.activeFanMode == FAN_MODE_CURVE) {
        copy_fan_curve(&g_app.guiFanCurve, &g_app.activeFanCurve);
    }
}
static void refresh_live_fan_telemetry(bool redrawControls) {
    if (!g_app.isServiceProcess) {
        bool wasAvailable = g_app.backgroundServiceAvailable;
        char detail[128] = {};
        ServiceSnapshot snapshot = {};
        if (!wasAvailable) {
            // Service was known to be down: do a lightweight health check
            if (!refresh_background_service_state()) {
                sync_fan_ui_from_cached_state(redrawControls);
                return;
            }
            // Service is back! Fetch full snapshot and update everything
            char snapDetail[256] = {};
            if (!service_client_get_snapshot(&snapshot, snapDetail, sizeof(snapDetail))) {
                sync_fan_ui_from_cached_state(redrawControls);
                update_all_gui_for_service_state();
                return;
            }
            apply_service_snapshot_to_app(&snapshot);
            sync_fan_ui_from_cached_state(redrawControls);
            update_all_gui_for_service_state();
            return;
        }
        // Service was available: do normal telemetry
        if (!service_client_get_telemetry(&snapshot, detail, sizeof(detail))) {
            sync_fan_ui_from_cached_state(redrawControls);
            if (!g_app.backgroundServiceAvailable) {
                update_all_gui_for_service_state();
            }
            return;
        }
        apply_service_snapshot_to_app(&snapshot);
        sync_fan_ui_from_cached_state(redrawControls);
        // F-REL-2e: normally the telemetry poll does not resync the editable
        // curve/lock controls (avoids wiping in-progress edits).  Force a full
        // resync when first creating the edit controls, OR when a service reset
        // just cleared a stale adopted lock (so graph/fields/checkboxes/header all
        // reflect the default state, not just internal state).
        if ((g_app.numVisible > 0 && !g_app.hEditsMhz[0]) || g_guiForceFullRefresh) {
            g_guiForceFullRefresh = false;
            update_all_gui_for_service_state();
        }
        return;
    }
    char detail[128] = {};
    if (!nvml_read_fans(detail, sizeof(detail))) return;
    sync_fan_ui_from_cached_state(redrawControls);
}
static bool is_start_on_logon_enabled(const char* path) {
    return get_config_int(path, "startup", "start_program_on_logon", 0) != 0;
}
static bool set_start_on_logon_enabled(const char* path, bool enabled) {
    return set_config_int(path, "startup", "start_program_on_logon", enabled ? 1 : 0);
}
static bool should_enable_startup_task_from_config(const char* path) {
    if (!path || !*path) return false;
    if (is_start_on_logon_enabled(path)) return true;
    if (get_config_int(path, "profiles", "logon_slot", 0) > 0) return true;
    // A per-user "apply admin shared profile N at logon" choice also needs the
    // startup task so the tray client launches at logon (the service router
    // applies it regardless, but the task keeps the GUI behavior consistent).
    return get_config_int(path, "profiles", "logon_shared_slot", 0) > 0;
}
static bool live_state_has_custom_oc() {
    if (g_app.gpuClockOffsetkHz != 0) return true;
    if (g_app.memClockOffsetkHz != 0) return true;
    if (g_app.powerLimitPct != 100) return true;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (g_app.freqOffsets[i] != 0) return true;
    }
    return false;
}
static bool live_state_has_custom_fan() {
    return get_effective_live_fan_mode() != FAN_MODE_AUTO;
}
static void refresh_fan_curve_button_text() {
    if (!g_app.hFanCurveBtn) return;
    ensure_valid_fan_curve_config(&g_app.guiFanCurve);
    char summary[96] = {};
    if (g_app.guiFanMode == FAN_MODE_CURVE) {
        fan_curve_format_summary(&g_app.guiFanCurve, summary, sizeof(summary));
        char text[128] = {};
        StringCchPrintfA(text, ARRAY_COUNT(text), "Curve: %s", summary);
        SetWindowTextA(g_app.hFanCurveBtn, text);
    } else {
        SetWindowTextA(g_app.hFanCurveBtn, "Edit Curve...");
    }
}
static void update_fan_controls_enabled_state() {
    bool serviceReady = g_app.backgroundServiceAvailable;
    if (g_app.hFanModeCombo) {
        bool dropdownOpen = SendMessageA(g_app.hFanModeCombo, CB_GETDROPPEDSTATE, 0, 0) != 0;
        if (!dropdownOpen) {
            SendMessageA(g_app.hFanModeCombo, CB_SETCURSEL, (WPARAM)g_app.guiFanMode, 0);
        }
        EnableWindow(g_app.hFanModeCombo, (serviceReady && g_app.fanSupported) ? TRUE : FALSE);
    }
    if (g_app.hFanEdit) {
        char buf[32] = {};
        StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", clamp_percent(g_app.guiFanFixedPercent));
        SetWindowTextA(g_app.hFanEdit, buf);
        EnableWindow(g_app.hFanEdit, (serviceReady && g_app.fanSupported && g_app.guiFanMode == FAN_MODE_FIXED) ? TRUE : FALSE);
    }
    if (g_app.hFanCurveBtn) {
        EnableWindow(g_app.hFanCurveBtn,
            (serviceReady && g_app.fanSupported && g_app.guiFanMode == FAN_MODE_CURVE) ? TRUE : FALSE);
        refresh_fan_curve_button_text();
    }
}
static void update_tray_icon() {
    if (!g_app.hMainWnd) return;
    if (g_app.usingBackgroundService && !g_app.isServiceProcess) {
        refresh_background_service_state();
    }
    // Only report OC/fan as active when the GPU live state is actually available.
    // When the driver is disabled in Device Manager (or the GPU is removed, or the
    // service is down), there is NO OC/fan in effect — the snapshot reflects the
    // pending DESIRED profile, not live hardware.  Reporting OC/fan-active then
    // would be false; show the default icon.  tray_hardware_live() is shared with
    // build_tray_tooltip() so the icon theme and the tooltip stay consistent.
    bool hardwareLive = tray_hardware_live();
    bool hasCustomOc = hardwareLive && live_state_has_custom_oc();
    bool hasCustomFan = hardwareLive && live_state_has_custom_fan();
    int state = TRAY_ICON_STATE_DEFAULT;
    if (hasCustomOc && hasCustomFan) {
        state = TRAY_ICON_STATE_OC_FAN;
    } else if (hasCustomOc) {
        state = TRAY_ICON_STATE_OC;
    } else if (hasCustomFan) {
        state = TRAY_ICON_STATE_FAN;
    }
    g_app.trayIconState = state;
    char tip[128] = {};
    build_tray_tooltip(tip, sizeof(tip));
    if (!g_app.trayIconAdded) return;
    if (g_app.trayLastRenderedValid &&
        g_app.trayLastRenderedState == state &&
        strcmp(g_app.trayLastRenderedTip, tip) == 0) {
        return;
    }
    NOTIFYICONDATAA nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_app.hMainWnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.hIcon = g_app.trayIcons[state] ? g_app.trayIcons[state] : LoadIcon(nullptr, IDI_APPLICATION);
    StringCchCopyA(nid.szTip, ARRAY_COUNT(nid.szTip), tip);
    if (Shell_NotifyIconA(NIM_MODIFY, &nid)) {
        g_app.trayLastRenderedValid = true;
        g_app.trayLastRenderedState = state;
        StringCchCopyA(g_app.trayLastRenderedTip, ARRAY_COUNT(g_app.trayLastRenderedTip), tip);
    } else {
        g_app.trayLastRenderedValid = false;
    }
}
static bool ensure_tray_icon() {
    if (!g_app.hMainWnd) return false;
    if (g_app.trayIconAdded) {
        update_tray_icon();
        return true;
    }
    NOTIFYICONDATAA nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_app.hMainWnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = APP_WM_TRAYICON;
    nid.hIcon = g_app.trayIcons[g_app.trayIconState] ? g_app.trayIcons[g_app.trayIconState] : LoadIcon(nullptr, IDI_APPLICATION);
    StringCchCopyA(nid.szTip, ARRAY_COUNT(nid.szTip), "Green Curve");
    if (!Shell_NotifyIconA(NIM_ADD, &nid)) return false;
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconA(NIM_SETVERSION, &nid);
    g_app.trayIconAdded = true;
    update_tray_icon();
    return true;
}
static void remove_tray_icon() {
    if (!g_app.trayIconAdded || !g_app.hMainWnd) return;
    NOTIFYICONDATAA nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_app.hMainWnd;
    nid.uID = 1;
    Shell_NotifyIconA(NIM_DELETE, &nid);
    g_app.trayIconAdded = false;
    g_app.trayLastRenderedValid = false;
}
static void hide_main_window_to_tray() {
    if (!g_app.hMainWnd) return;
    ensure_tray_icon();
    ShowWindow(g_app.hMainWnd, SW_HIDE);
    update_fan_telemetry_timer();
}
static void show_main_window_from_tray() {
    if (!g_app.hMainWnd) return;
    ShowWindow(g_app.hMainWnd, SW_RESTORE);
    ShowWindow(g_app.hMainWnd, SW_SHOW);
    update_fan_telemetry_timer();
    bool wasAvailable = g_app.backgroundServiceAvailable;
    refresh_background_service_state();
    if (wasAvailable != g_app.backgroundServiceAvailable || (g_app.numVisible > 0 && !g_app.hEditsMhz[0])) {
        update_all_gui_for_service_state();
    } else {
        update_background_service_controls();
        invalidate_main_window();
    }
    SetForegroundWindow(g_app.hMainWnd);
    g_app.startHiddenToTray = false;
}
static void show_tray_menu(HWND hwnd) {
    if (!hwnd) return;
    refresh_menu_theme_cache();
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuA(menu, MF_STRING, TRAY_MENU_SHOW_ID, IsWindowVisible(hwnd) ? "Show Window" : "Open Green Curve");
    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(menu, MF_STRING, TRAY_MENU_EXIT_ID, "Exit");
    POINT pt = {};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}
static bool activate_existing_instance_window() {
    for (int attempt = 0; attempt < 20; attempt++) {
        HWND existing = FindWindowA(APP_CLASS_NAME, nullptr);
        if (existing && IsWindow(existing)) {
            HWND target = GetLastActivePopup(existing);
            if (!target || !IsWindow(target)) target = existing;
            ShowWindow(existing, SW_SHOW);
            ShowWindow(existing, SW_RESTORE);
            if (target != existing) {
                ShowWindow(target, SW_SHOW);
                ShowWindow(target, SW_RESTORE);
            }
            BringWindowToTop(target);
            SetForegroundWindow(target);
            return true;
        }
        if (attempt + 1 < 20) Sleep(50);
    }
    return false;
}
static bool acquire_single_instance_mutex() {
    if (g_singleInstanceMutex) return true;
    g_singleInstanceMutex = CreateMutexA(nullptr, TRUE, APP_SINGLE_INSTANCE_MUTEX_NAME);
    if (!g_singleInstanceMutex) return false;
    if (GetLastError() != ERROR_ALREADY_EXISTS) return true;
    CloseHandle(g_singleInstanceMutex);
    g_singleInstanceMutex = nullptr;
    activate_existing_instance_window();
    return false;
}
static void release_single_instance_mutex() {
    if (!g_singleInstanceMutex) return;
    ReleaseMutex(g_singleInstanceMutex);
    CloseHandle(g_singleInstanceMutex);
    g_singleInstanceMutex = nullptr;
}
static unsigned int fan_runtime_failure_limit() {
    UINT intervalMs = g_app.fanFixedRuntimeActive
        ? FAN_FIXED_RUNTIME_INTERVAL_MS
        : (UINT)g_app.activeFanCurve.pollIntervalMs;
    if (intervalMs < 250) intervalMs = 250;
    unsigned int limit = (unsigned int)((FAN_RUNTIME_FAILURE_WINDOW_MS + intervalMs - 1) / intervalMs);
    if (limit < 3) limit = 3;
    if (limit > 10) limit = 10;
    return limit;
}
static void mark_fan_runtime_success(ULONGLONG now) {
    g_app.fanRuntimeConsecutiveFailures = 0;
    g_app.fanRuntimeLastApplyTickMs = now;
}
static void handle_fan_runtime_failure(const char* action, const char* detail) {
    if (!g_app.fanCurveRuntimeActive && !g_app.fanFixedRuntimeActive) return;
    g_app.fanRuntimeLastApplyTickMs = 0;
    g_app.fanRuntimeConsecutiveFailures++;
    unsigned int limit = fan_runtime_failure_limit();
    // Suppress repetitive identical failure logs within a 30-second window.
    // Requires g_appLock (caller must hold it).
    static ULONGLONG s_lastFailureLogTickMs = 0;
    static char s_lastFailureAction[128] = {};
    static char s_lastFailureDetail[128] = {};
    ULONGLONG now = GetTickCount64();
    bool sameAction = (action && action[0]) ? strcmp(action, s_lastFailureAction) == 0 : s_lastFailureAction[0] == 0;
    bool sameDetail = (detail && detail[0]) ? strcmp(detail, s_lastFailureDetail) == 0 : s_lastFailureDetail[0] == 0;
    bool suppress = sameAction && sameDetail && (now - s_lastFailureLogTickMs < 30000);
    if (!suppress) {
        s_lastFailureLogTickMs = now;
        StringCchCopyA(s_lastFailureAction, ARRAY_COUNT(s_lastFailureAction), action ? action : "");
        StringCchCopyA(s_lastFailureDetail, ARRAY_COUNT(s_lastFailureDetail), detail ? detail : "");
        debug_log("fan runtime failure %u/%u: %s%s%s\n",
            g_app.fanRuntimeConsecutiveFailures,
            limit,
            action ? action : "fan runtime failure",
            (detail && detail[0]) ? " - " : "",
            (detail && detail[0]) ? detail : "");
    }
    if (g_app.fanRuntimeConsecutiveFailures < limit) return;
    char summary[512] = {};
    if (action && action[0] && detail && detail[0]) {
        set_message(summary, sizeof(summary), "%s: %s", action, detail);
    } else if (action && action[0]) {
        set_message(summary, sizeof(summary), "%s", action);
    } else if (detail && detail[0]) {
        set_message(summary, sizeof(summary), "%s", detail);
    } else {
        set_message(summary, sizeof(summary), "Custom fan runtime failed repeatedly");
    }
    char autoDetail[128] = {};
    bool autoRestored = nvml_set_fan_auto(autoDetail, sizeof(autoDetail));
    if (!autoRestored) {
        char emergencyDetail[128] = {};
        if (nvml_set_fan_manual(100, nullptr, emergencyDetail, sizeof(emergencyDetail))) {
            debug_log("fan runtime failure emergency: set fan to 100%% after auto-restore failed\n");
        } else {
            debug_log("fan runtime failure emergency: could not set fan to 100%% after auto-restore failed: %s\n", emergencyDetail);
        }
    }
    stop_fan_curve_runtime();
    if (autoRestored) {
        g_app.activeFanMode = FAN_MODE_AUTO;
        sync_fan_ui_from_cached_state(window_should_redraw_fan_controls());
    } else if (g_app.hMainWnd) {
        refresh_live_fan_telemetry(window_should_redraw_fan_controls());
    }
    char reportDetails[768] = {};
    if (autoRestored) {
        if (autoDetail[0]) {
            set_message(reportDetails, sizeof(reportDetails),
                "%s. Driver auto fan restored (%s).", summary, autoDetail);
        } else {
            set_message(reportDetails, sizeof(reportDetails),
                "%s. Driver auto fan restored.", summary);
        }
    } else {
        if (autoDetail[0]) {
            set_message(reportDetails, sizeof(reportDetails),
                "%s. Attempt to restore driver auto fan failed: %s", summary, autoDetail);
        } else {
            set_message(reportDetails, sizeof(reportDetails),
                "%s. Attempt to restore driver auto fan failed.", summary);
        }
    }
    char logErr[256] = {};
    if (!write_error_report_log(
            "Fan control runtime disabled after repeated failures",
            reportDetails,
            logErr,
            sizeof(logErr)) &&
        logErr[0]) {
        debug_log("fan runtime error log failed: %s\n", logErr);
    }
    if (g_app.hProfileStatusLabel) {
        set_profile_status_text(
            autoRestored
                ? "Custom fan runtime disabled after repeated failures. Driver auto fan restored. See the Green Curve error log."
                : "Custom fan runtime disabled after repeated failures. Could not confirm driver auto fan restore. See the Green Curve error log.");
    }
    update_tray_icon();
}
static void stop_fan_curve_runtime(bool restoreFanAutoOnExit) {
    if (restoreFanAutoOnExit && (g_app.fanCurveRuntimeActive || g_app.fanFixedRuntimeActive)) {
        char detail[128] = {};
        if (g_app.fanSupported && !g_app.fanIsAuto && nvml_set_fan_auto(detail, sizeof(detail))) {
            g_app.fanIsAuto = true;
        }
    }
    if (g_app.hMainWnd) {
        KillTimer(g_app.hMainWnd, FAN_CURVE_TIMER_ID);
    }
    bool hadRuntime = g_app.fanCurveRuntimeActive || g_app.fanFixedRuntimeActive;
    g_app.fanCurveRuntimeActive = false;
    g_app.fanFixedRuntimeActive = false;
    g_app.fanCurveHasLastAppliedTemp = false;
    g_app.fanRuntimeConsecutiveFailures = 0;
    g_app.fanRuntimeLastApplyTickMs = 0;
    if (hadRuntime && (g_app.activeFanMode == FAN_MODE_CURVE || g_app.activeFanMode == FAN_MODE_FIXED)) {
        g_app.activeFanMode = g_app.fanIsAuto ? FAN_MODE_AUTO : FAN_MODE_FIXED;
    }
    if (g_app.isServiceProcess && hadRuntime) {
        stop_service_fan_runtime_thread();
    }
    if (g_app.hMainWnd) {
        refresh_live_fan_telemetry(true);
    }
    boost_fan_telemetry_for_ms(3000);
    update_fan_telemetry_timer();
    update_tray_icon();
}
static bool nvml_read_temperature(int* temperatureC, char* detail, size_t detailSize) {
    if (temperatureC) *temperatureC = 0;
    g_app.gpuTemperatureValid = false;
    if (!nvml_ensure_ready() || !g_nvml_api.getTemperature) {
        set_message(detail, detailSize, "GPU temperature unsupported");
        return false;
    }
    unsigned int value = 0;
    nvmlReturn_t r = g_nvml_api.getTemperature(g_app.nvmlDevice, NVML_TEMPERATURE_GPU, &value);
    if (r != NVML_SUCCESS) {
        set_message(detail, detailSize, "%s", nvml_err_name(r));
        return false;
    }
    g_app.gpuTemperatureC = (int)value;
    g_app.gpuTemperatureValid = true;
    if (temperatureC) *temperatureC = (int)value;
    return true;
}
static void apply_fan_curve_tick() {
    // Phase 1: snapshot mode flags and configuration under CS, then release
    // before any NVML calls (which can crash in nvml.dll and kill this thread
    // via VEH, corrupting the CS if held across the crash).
    bool fixedActive = false;
    bool curveActive = false;
    ULONGLONG backoffMs = FAN_RUNTIME_REAPPLY_INTERVAL_MS;
    ULONGLONG now = 0;
    int fixedTargetPercent = 0;
    EnterCriticalSection(&g_appLock);
    fixedActive = g_app.fanFixedRuntimeActive;
    curveActive = g_app.fanCurveRuntimeActive;
    now = GetTickCount64();
    if (fixedActive) {
        fixedTargetPercent = clamp_percent(g_app.activeFanFixedPercent);
        if (g_app.fanRuntimeConsecutiveFailures > 0) {
            unsigned int factor = 1u << (g_app.fanRuntimeConsecutiveFailures > 4 ? 4 : g_app.fanRuntimeConsecutiveFailures);
            backoffMs *= factor;
            if (backoffMs > 120000) backoffMs = 120000;
        }
    }
    LeaveCriticalSection(&g_appLock);
    if (!fixedActive && !curveActive) return;

    // Phase 2a: fixed-fan mode — NVML calls are made WITHOUT holding g_appLock.
    if (fixedActive) {
        bool needsReapply = (g_app.fanRuntimeLastApplyTickMs == 0) ||
            ((now - g_app.fanRuntimeLastApplyTickMs) >= backoffMs);
        bool matches = false;
        char detail[128] = {};
        // nvml_manual_fan_matches_target can crash in nvml.dll — NO CS held.
        if (nvml_manual_fan_matches_target(fixedTargetPercent, &matches, detail, sizeof(detail))) {
            if (matches) {
                EnterCriticalSection(&g_appLock);
                if (needsReapply) {
                    mark_fan_runtime_success(now);
                }
                g_app.activeFanMode = FAN_MODE_FIXED;
                g_app.activeFanFixedPercent = fixedTargetPercent;
                g_app.fanRuntimeConsecutiveFailures = 0;
                sync_fan_ui_from_cached_state(window_should_redraw_fan_controls());
                LeaveCriticalSection(&g_appLock);
                return;
            }
        } else if (!needsReapply) {
            EnterCriticalSection(&g_appLock);
            handle_fan_runtime_failure("Fixed fan runtime verify failed", detail);
            LeaveCriticalSection(&g_appLock);
            return;
        }
        bool exact = false;
        // nvml_set_fan_manual can crash in nvml.dll — NO CS held.
        if (!nvml_set_fan_manual(fixedTargetPercent, &exact, detail, sizeof(detail)) || !exact) {
            if (!detail[0] && !exact) {
                set_message(detail, sizeof(detail), "Fan readback did not confirm %d%%", fixedTargetPercent);
            }
            EnterCriticalSection(&g_appLock);
            handle_fan_runtime_failure("Fixed fan runtime apply failed", detail);
            LeaveCriticalSection(&g_appLock);
            return;
        }
        EnterCriticalSection(&g_appLock);
        g_app.activeFanMode = FAN_MODE_FIXED;
        g_app.activeFanFixedPercent = fixedTargetPercent;
        mark_fan_runtime_success(now);
        if (g_app.isServiceProcess) {
            populate_control_state(&g_serviceControlState);
            g_serviceControlStateValid = true;
        }
        sync_fan_ui_from_cached_state(window_should_redraw_fan_controls());
        LeaveCriticalSection(&g_appLock);
        return;
    }

    // Phase 2b: curve-fan mode — read temperature first (NVML call, no CS).
    int currentTempC = 0;
    char detail[128] = {};
    // nvml_read_temperature can crash in nvml.dll — NO CS held.
    if (!nvml_read_temperature(&currentTempC, detail, sizeof(detail))) {
        EnterCriticalSection(&g_appLock);
        handle_fan_runtime_failure("Fan curve temperature poll failed", detail);
        LeaveCriticalSection(&g_appLock);
        return;
    }

    // Phase 3: process temperature and decide whether to apply.
    // Re-acquire CS for g_app reads/writes.
    bool shouldApply = false;
    bool hasLastAppliedTemp = false;
    int lastAppliedPercent = 0;
    int lastAppliedTempC = 0;
    ULONGLONG lastApplyTickMs = 0;
    int consecutiveFailures = 0;
    FanCurveConfig activeCurve = {};
    EnterCriticalSection(&g_appLock);
    if (!g_app.fanCurveRuntimeActive) {
        LeaveCriticalSection(&g_appLock);
        return;
    }
    activeCurve = g_app.activeFanCurve;
    hasLastAppliedTemp = g_app.fanCurveHasLastAppliedTemp;
    lastAppliedPercent = g_app.fanCurveLastAppliedPercent;
    lastAppliedTempC = g_app.fanCurveLastAppliedTempC;
    lastApplyTickMs = g_app.fanRuntimeLastApplyTickMs;
    consecutiveFailures = g_app.fanRuntimeConsecutiveFailures;
    LeaveCriticalSection(&g_appLock);

    int targetPercent = fan_curve_interpolate_percent(&activeCurve, currentTempC);
    if (!hasLastAppliedTemp) {
        shouldApply = true;
    } else if (targetPercent > lastAppliedPercent) {
        shouldApply = true;
    } else if (targetPercent < lastAppliedPercent) {
        int minDrop = activeCurve.hysteresisC;
        if (minDrop < 0) minDrop = 0;
        if (currentTempC <= lastAppliedTempC - minDrop) {
            shouldApply = true;
        }
        // If hysteresis blocked the drop, do not let the timed re-apply override it.
        // The fan will adjust naturally when the temperature drops past the hysteresis
        // threshold. This prevents the re-apply timer from defeating user-configured
        // hysteresis smoothing.
    }
    if (!shouldApply) {
        backoffMs = FAN_RUNTIME_REAPPLY_INTERVAL_MS;
        if (consecutiveFailures > 0) {
            unsigned int factor = 1u << (consecutiveFailures > 4 ? 4 : consecutiveFailures);
            backoffMs *= factor;
            if (backoffMs > 120000) backoffMs = 120000;
        }
        bool timerExpired = (lastApplyTickMs == 0) ||
            ((now - lastApplyTickMs) >= backoffMs);
        if (timerExpired) {
            bool matches = false;
            // nvml_manual_fan_matches_target can crash — NO CS held.
            if (nvml_manual_fan_matches_target(targetPercent, &matches, detail, sizeof(detail))) {
                if (matches) {
                    EnterCriticalSection(&g_appLock);
                    g_app.fanCurveLastAppliedPercent = targetPercent;
                    g_app.fanCurveLastAppliedTempC = currentTempC;
                    g_app.fanCurveHasLastAppliedTemp = true;
                    mark_fan_runtime_success(now);
                    LeaveCriticalSection(&g_appLock);
                    return;
                }
                debug_log("fan curve tick: timed re-apply needed (speed drifted from %d%%, temp=%d)\n", targetPercent, currentTempC);
                shouldApply = true;
            } else {
                debug_log("fan curve tick: timed re-apply forced (verify read failed)\n");
                shouldApply = true;
            }
        }
    }
    if (!shouldApply) return;

    // Phase 4: apply fan speed — nvml_set_fan_manual can crash, NO CS held.
    bool exact = false;
    if (!nvml_set_fan_manual(targetPercent, &exact, detail, sizeof(detail)) || !exact) {
        if (!detail[0] && !exact) {
            set_message(detail, sizeof(detail), "Fan readback did not confirm %d%%", targetPercent);
        }
        EnterCriticalSection(&g_appLock);
        handle_fan_runtime_failure("Fan curve runtime apply failed", detail);
        LeaveCriticalSection(&g_appLock);
        return;
    }

    // Phase 5: write results under CS.
    EnterCriticalSection(&g_appLock);
    g_app.activeFanMode = FAN_MODE_CURVE;
    g_app.activeFanFixedPercent = targetPercent;
    g_app.fanCurveLastAppliedPercent = targetPercent;
    g_app.fanCurveLastAppliedTempC = currentTempC;
    g_app.fanCurveHasLastAppliedTemp = true;
    mark_fan_runtime_success(now);
    if (g_app.isServiceProcess) {
        populate_control_state(&g_serviceControlState);
        g_serviceControlStateValid = true;
    }
    sync_fan_ui_from_cached_state(window_should_redraw_fan_controls());
    LeaveCriticalSection(&g_appLock);
}
static bool validate_desired_fan_settings_for_apply(const DesiredSettings* desired, char* result, size_t resultSize) {
    if (!desired->hasFan) return true;
    int desiredFanMode = desired->fanMode;
    if (desiredFanMode < FAN_MODE_AUTO || desiredFanMode > FAN_MODE_CURVE) {
        desiredFanMode = desired->fanAuto ? FAN_MODE_AUTO : FAN_MODE_FIXED;
    }
    FanCurveConfig desiredCurve = desired->fanCurve;
    fan_curve_normalize(&desiredCurve);
    if (g_app.fanRangeKnown) {
        fan_curve_clamp_percentages(&desiredCurve, (int)g_app.fanMinPct, (int)g_app.fanMaxPct);
    }
    if (desiredFanMode == FAN_MODE_CURVE) {
        char validationErr[256] = {};
        if (!fan_curve_validate(&desiredCurve, validationErr, sizeof(validationErr))) {
            set_message(result, resultSize, "%s", validationErr);
            return false;
        }
        if (!validate_fan_curve_for_runtime(&desiredCurve, validationErr, sizeof(validationErr))) {
            set_message(result, resultSize, "%s", validationErr);
            return false;
        }
    } else if (desiredFanMode == FAN_MODE_FIXED) {
        char detail[128] = {};
        if (!validate_manual_fan_percent_for_runtime(desired->fanPercent, detail, sizeof(detail))) {
            set_message(result, resultSize, "%s", detail);
            return false;
        }
    }
    return true;
}

static bool apply_fan_settings(const DesiredSettings* desired, char* failureDetails, size_t failureDetailsSize, int& successCount, int& failCount, char* result, size_t resultSize, bool& outFanChanged) {
    outFanChanged = false;
    if (!desired->hasFan) return true;
    set_last_apply_phase("apply: fan settings");
    auto append_failure = [&](const char* fmt, ...) {
        char part[256] = {};
        va_list ap;
        va_start(ap, fmt);
        StringCchVPrintfA(part, ARRAY_COUNT(part), fmt, ap);
        va_end(ap);
        if (!part[0]) return;
        if (failureDetails[0]) {
            StringCchCatA(failureDetails, (int)failureDetailsSize, "; ");
        }
        StringCchCatA(failureDetails, (int)failureDetailsSize, part);
        debug_log("apply failure: %s\n", part);
    };
    int desiredFanMode = desired->fanMode;
    if (desiredFanMode < FAN_MODE_AUTO || desiredFanMode > FAN_MODE_CURVE) {
        desiredFanMode = desired->fanAuto ? FAN_MODE_AUTO : FAN_MODE_FIXED;
    }
    FanCurveConfig desiredCurve = desired->fanCurve;
    fan_curve_normalize(&desiredCurve);
    if (g_app.fanRangeKnown) {
        fan_curve_clamp_percentages(&desiredCurve, (int)g_app.fanMinPct, (int)g_app.fanMaxPct);
    }
    bool fanChanged = false;
    if (!fan_setting_matches_current(desiredFanMode, desired->fanPercent, &desiredCurve)) {
        fanChanged = true;
        int previousMode = g_app.activeFanMode;
        int previousFixed = g_app.activeFanFixedPercent;
        FanCurveConfig previousCurve = g_app.activeFanCurve;
        bool previousCurveRuntime = g_app.fanCurveRuntimeActive;
        bool previousFixedRuntime = g_app.fanFixedRuntimeActive;
        bool exact = false;
        char detail[128] = {};
        bool ok = false;
        if (desiredFanMode == FAN_MODE_AUTO) {
            set_last_apply_phase("apply: fan auto write");
            ok = nvml_set_fan_auto(detail, sizeof(detail));
            if (ok) {
                stop_fan_curve_runtime();
                g_app.activeFanMode = FAN_MODE_AUTO;
                debug_log("apply fan auto: restored driver auto, runtime stopped\n");
            }
        } else if (desiredFanMode == FAN_MODE_FIXED) {
            if (validate_manual_fan_percent_for_runtime(desired->fanPercent, detail, sizeof(detail))) {
                stop_fan_curve_runtime();
                if (g_app.hMainWnd || g_app.isServiceProcess) {
                    set_last_apply_phase("apply: fixed fan runtime start");
                    g_app.activeFanFixedPercent = clamp_percent(desired->fanPercent);
                    start_fixed_fan_runtime();
                    ok = g_app.fanFixedRuntimeActive && g_app.fanRuntimeLastApplyTickMs != 0;
                    if (!ok) {
                        set_message(detail, sizeof(detail),
                            g_app.fanRuntimeConsecutiveFailures > 0
                                ? "Failed to verify the initial fixed fan apply"
                                : "Failed to start fixed fan maintenance");
                    }
                } else {
                    set_last_apply_phase("apply: fixed fan write");
                    ok = nvml_set_fan_manual(desired->fanPercent, &exact, detail, sizeof(detail));
                }
            }
            if (ok) {
                g_app.activeFanMode = FAN_MODE_FIXED;
                g_app.activeFanFixedPercent = clamp_percent(desired->fanPercent);
            }
        } else {
            if (!validate_fan_curve_for_runtime(&desiredCurve, detail, sizeof(detail))) {
                ok = false;
            } else if (g_app.hMainWnd || g_app.isServiceProcess) {
                stop_fan_curve_runtime();
                copy_fan_curve(&g_app.activeFanCurve, &desiredCurve);
                debug_log("apply fan curve: pollMs=%d hysteresis=%d firstEnabledPct=%d serviceProcess=%d\n",
                    g_app.activeFanCurve.pollIntervalMs,
                    g_app.activeFanCurve.hysteresisC,
                    g_app.activeFanCurve.points[0].enabled ? g_app.activeFanCurve.points[0].fanPercent : 0,
                    g_app.isServiceProcess ? 1 : 0);
                set_last_apply_phase("apply: fan curve runtime start");
                start_fan_curve_runtime();
                ok = g_app.fanCurveRuntimeActive && g_app.fanRuntimeLastApplyTickMs != 0;
                debug_log("apply fan curve runtime start: active=%d lastApply=%llu failures=%u\n",
                    g_app.fanCurveRuntimeActive ? 1 : 0,
                    g_app.fanRuntimeLastApplyTickMs,
                    g_app.fanRuntimeConsecutiveFailures);
                if (!ok) {
                    set_message(detail, sizeof(detail),
                        g_app.fanRuntimeConsecutiveFailures > 0
                            ? "Failed to verify the initial fan curve apply"
                            : "Failed to start fan curve maintenance");
                }
            } else {
                set_message(detail, sizeof(detail),
                    "Fan curve mode requires the resident Green Curve tray app. Start the program hidden to tray for long-running fan control.");
            }
        }
        if (ok) successCount++;
        else {
            g_app.activeFanMode = previousMode;
            g_app.activeFanFixedPercent = previousFixed;
            copy_fan_curve(&g_app.activeFanCurve, &previousCurve);
            if (previousCurveRuntime) {
                start_fan_curve_runtime();
            } else if (previousFixedRuntime) {
                start_fixed_fan_runtime();
            }
            failCount++;
            append_failure("Fan control change failed%s%s",
                detail[0] ? ": " : "",
                detail[0] ? detail : "");
        }
    }
    outFanChanged = fanChanged;
    return true;
}
