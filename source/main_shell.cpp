static void start_fan_curve_runtime() {
    if (!g_app.fanSupported) return;
    fan_curve_normalize(&g_app.activeFanCurve);
    char err[256] = {};
    if (!fan_manual_control_available(err, sizeof(err))) {
        return;
    }
    if (!fan_curve_validate(&g_app.activeFanCurve, err, sizeof(err))) {
        return;
    }
    if (!validate_fan_curve_for_runtime(&g_app.activeFanCurve, err, sizeof(err))) {
        return;
    }

    g_app.activeFanMode = FAN_MODE_CURVE;
    g_app.fanCurveRuntimeActive = true;
    g_app.fanFixedRuntimeActive = false;
    g_app.fanCurveHasLastAppliedTemp = false;
    g_app.fanRuntimeConsecutiveFailures = 0;
    g_app.fanRuntimeLastApplyTickMs = 0;

    if (g_app.hMainWnd) {
        KillTimer(g_app.hMainWnd, FAN_CURVE_TIMER_ID);
        if (!SetTimer(g_app.hMainWnd, FAN_CURVE_TIMER_ID, (UINT)g_app.activeFanCurve.pollIntervalMs, nullptr)) {
            stop_fan_curve_runtime();
            return;
        }
    } else if (g_app.isServiceProcess) {
        if (!ensure_service_fan_runtime_thread()) {
            stop_fan_curve_runtime();
            return;
        }
    }

    boost_fan_telemetry_for_ms(3000);
    update_fan_telemetry_timer();

    if (g_app.fanCurveRuntimeActive) {
        apply_fan_curve_tick();
    }
    if (g_app.isServiceProcess) {
        populate_control_state(&g_serviceControlState);
        g_serviceControlStateValid = true;
        mark_service_telemetry_cache_updated("fan curve start");
    }
    update_tray_icon();
}

static void start_fixed_fan_runtime() {
    if (!g_app.fanSupported) return;

    g_app.activeFanFixedPercent = clamp_percent(g_app.activeFanFixedPercent);
    char detail[256] = {};
    if (!fan_manual_control_available(detail, sizeof(detail))) return;
    if (!validate_manual_fan_percent_for_runtime(g_app.activeFanFixedPercent, detail, sizeof(detail))) return;
    g_app.activeFanMode = FAN_MODE_FIXED;
    g_app.fanCurveRuntimeActive = false;
    g_app.fanFixedRuntimeActive = true;
    g_app.fanCurveHasLastAppliedTemp = false;
    g_app.fanRuntimeConsecutiveFailures = 0;
    g_app.fanRuntimeLastApplyTickMs = 0;

    if (g_app.hMainWnd) {
        KillTimer(g_app.hMainWnd, FAN_CURVE_TIMER_ID);
        if (!SetTimer(g_app.hMainWnd, FAN_CURVE_TIMER_ID, FAN_FIXED_RUNTIME_INTERVAL_MS, nullptr)) {
            stop_fan_curve_runtime();
            return;
        }
    } else if (g_app.isServiceProcess) {
        if (!ensure_service_fan_runtime_thread()) {
            stop_fan_curve_runtime();
            return;
        }
    }

    boost_fan_telemetry_for_ms(3000);
    update_fan_telemetry_timer();

    apply_fan_curve_tick();
    if (g_app.isServiceProcess) {
        populate_control_state(&g_serviceControlState);
        g_serviceControlStateValid = true;
        mark_service_telemetry_cache_updated("fixed fan start");
    }
    update_tray_icon();
}

static void show_license_dialog(HWND parent) {
    if (g_licenseDialog.hwnd) {
        ShowWindow(g_licenseDialog.hwnd, SW_SHOW);
        SetForegroundWindow(g_licenseDialog.hwnd);
        return;
    }

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = LicenseDialogProc;
    wc.hInstance = g_app.hInst;
    wc.lpszClassName = "GreenCurveLicenseDialog";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_app.hWindowClassBrush;
    wc.hIcon = (HICON)SendMessageA(g_app.hMainWnd, WM_GETICON, ICON_SMALL, 0);
    WNDCLASSEXA existing = {};
    if (!GetClassInfoExA(g_app.hInst, wc.lpszClassName, &existing)) {
        RegisterClassExA(&wc);
    }

    RECT ownerRect = {};
    if (parent) GetWindowRect(parent, &ownerRect);
    int width = dp(760);
    int height = dp(560);
    int x = parent ? ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2 : CW_USEDEFAULT;
    int y = parent ? ownerRect.top + dp(30) : CW_USEDEFAULT;

    g_licenseDialog.owner = parent;
    g_licenseDialog.hwnd = CreateWindowExA(
        WS_EX_DLGMODALFRAME,
        wc.lpszClassName,
        APP_NAME " License",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        x, y, width, height,
        parent, nullptr, g_app.hInst, nullptr);
    if (!g_licenseDialog.hwnd) return;

    if (parent) EnableWindow(parent, FALSE);
    ShowWindow(g_licenseDialog.hwnd, SW_SHOW);
    UpdateWindow(g_licenseDialog.hwnd);
}

static void layout_license_dialog(HWND hwnd) {
    if (!hwnd || !g_licenseDialog.textEdit || !g_licenseDialog.closeButton) return;
    RECT rc = {};
    GetClientRect(hwnd, &rc);
    int margin = dp(16);
    int buttonW = dp(92);
    int buttonH = dp(30);
    SetWindowPos(g_licenseDialog.textEdit, nullptr,
        margin, margin, nvmax(dp(320), rc.right - margin * 2), nvmax(dp(220), rc.bottom - margin * 3 - buttonH),
        SWP_NOZORDER);
    SetWindowPos(g_licenseDialog.closeButton, nullptr,
        rc.right - margin - buttonW, rc.bottom - margin - buttonH, buttonW, buttonH,
        SWP_NOZORDER);
}

static LRESULT CALLBACK LicenseDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            apply_system_titlebar_theme(hwnd);
            allow_dark_mode_for_window(hwnd);

            g_licenseDialog.textEdit = CreateWindowExA(
                WS_EX_CLIENTEDGE, "EDIT", APP_LICENSE_TEXT,
                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY,
                0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)LICENSE_DIALOG_TEXT_ID, g_app.hInst, nullptr);
            g_licenseDialog.closeButton = CreateWindowExA(
                0, "BUTTON", "Close",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)LICENSE_DIALOG_CLOSE_ID, g_app.hInst, nullptr);

            style_input_control(g_licenseDialog.textEdit);
            apply_ui_font(g_licenseDialog.textEdit);
            apply_ui_font(g_licenseDialog.closeButton);
            layout_license_dialog(hwnd);
            return 0;
        }

        case WM_SIZE:
            layout_license_dialog(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            apply_system_titlebar_theme(hwnd);
            allow_dark_mode_for_window(hwnd);
            return 0;

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wParam;
            HWND hCtl = (HWND)lParam;
            if (hCtl == g_licenseDialog.textEdit) {
                SetTextColor(hdc, COL_TEXT);
                SetBkColor(hdc, COL_INPUT);
                if (!g_licenseDialog.hInputBrush) g_licenseDialog.hInputBrush = CreateSolidBrush(COL_INPUT);
                return (LRESULT)g_licenseDialog.hInputBrush;
            }
            SetTextColor(hdc, COL_LABEL);
            SetBkColor(hdc, COL_BG);
            if (!g_licenseDialog.hBgBrush) g_licenseDialog.hBgBrush = CreateSolidBrush(COL_BG);
            return (LRESULT)g_licenseDialog.hBgBrush;
        }

        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
            if (dis && dis->CtlType == ODT_BUTTON && dis->CtlID == LICENSE_DIALOG_CLOSE_ID) {
                draw_themed_button(dis);
                return TRUE;
            }
            break;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == LICENSE_DIALOG_CLOSE_ID && HIWORD(wParam) == BN_CLICKED) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (g_licenseDialog.hInputBrush) { DeleteObject(g_licenseDialog.hInputBrush); g_licenseDialog.hInputBrush = nullptr; }
            if (g_licenseDialog.hBgBrush) { DeleteObject(g_licenseDialog.hBgBrush); g_licenseDialog.hBgBrush = nullptr; }
            g_licenseDialog.textEdit = nullptr;
            g_licenseDialog.closeButton = nullptr;
            g_licenseDialog.hwnd = nullptr;
            if (g_licenseDialog.owner) {
                EnableWindow(g_licenseDialog.owner, TRUE);
                SetForegroundWindow(g_licenseDialog.owner);
                g_licenseDialog.owner = nullptr;
            }
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static int layout_rows_per_column() {
    return (g_app.numVisible + 5) / 6;
}

static int layout_global_controls_y() {
    return dp(GRAPH_HEIGHT) + dp(20) + layout_rows_per_column() * dp(20) + dp(6);
}

static int layout_bottom_buttons_y() {
    return layout_global_controls_y() + dp(56);
}

static int layout_bottom_panel_bottom_y() {
    int buttonsY = layout_bottom_buttons_y();
    int profileY = buttonsY + dp(40);
    int autoY = profileY + dp(34);
    int serviceY = autoY + dp(26);
    int hintY = serviceY + dp(26);
    int statusY = hintY + dp(40);
    return statusY + dp(18);
}

static int minimum_client_height() {
    return nvmax(dp(WINDOW_HEIGHT), layout_bottom_panel_bottom_y() + dp(12));
}

static SIZE adjusted_window_size_for_client(int clientWidth, int clientHeight, DWORD style, DWORD exStyle) {
    RECT rc = { 0, 0, clientWidth, clientHeight };
    typedef BOOL (WINAPI *AdjustWindowRectExForDpi_t)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static AdjustWindowRectExForDpi_t adjustForDpi = (AdjustWindowRectExForDpi_t)GetProcAddress(GetModuleHandleA("user32.dll"), "AdjustWindowRectExForDpi");
    if (adjustForDpi) {
        adjustForDpi(&rc, style, FALSE, exStyle, (UINT)g_dpi);
    } else {
        AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    }
    SIZE size = {};
    size.cx = rc.right - rc.left;
    size.cy = rc.bottom - rc.top;
    return size;
}

static SIZE main_window_min_size() {
    return adjusted_window_size_for_client(dp(WINDOW_WIDTH), minimum_client_height(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0);
}

#ifdef GREEN_CURVE_SERVICE_BINARY
static void trim_working_set() {
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    if (kernel32) {
        typedef BOOL (WINAPI *K32EmptyWorkingSet_t)(HANDLE);
        auto k32 = (K32EmptyWorkingSet_t)GetProcAddress(kernel32, "K32EmptyWorkingSet");
        if (k32) {
            k32(GetCurrentProcess());
            return;
        }
    }
    HMODULE psapi = load_system_library_a("psapi.dll");
    if (psapi) {
        typedef BOOL (WINAPI *EmptyWorkingSet_t)(HANDLE);
        auto fn = (EmptyWorkingSet_t)GetProcAddress(psapi, "EmptyWorkingSet");
        if (fn) fn(GetCurrentProcess());
    }
}

static bool is_elevated() {
    return true;
}

static void set_profile_status_text(const char*, ...) {
}

static void update_profile_state_label() {
}

static void update_profile_action_buttons() {
}

static void update_background_service_controls() {
}

static void apply_system_titlebar_theme(HWND) {
}

static unsigned int get_edit_value(HWND) {
    return 0;
}

static void set_edit_value(HWND, unsigned int) {
}

static void populate_edits() {
}

static void apply_lock(int) {
}

static void unlock_all() {
}
#endif

static void ensure_main_window_min_size(HWND hwnd) {
    if (!hwnd) return;
    RECT client = {};
    GetClientRect(hwnd, &client);
    int needClientW = dp(WINDOW_WIDTH);
    int needClientH = minimum_client_height();
    if (client.right >= needClientW && client.bottom >= needClientH) return;

    RECT window = {};
    GetWindowRect(hwnd, &window);
    SIZE needWindow = main_window_min_size();
    int currentW = window.right - window.left;
    int currentH = window.bottom - window.top;
    SetWindowPos(hwnd, nullptr, 0, 0,
        nvmax(currentW, needWindow.cx),
        nvmax(currentH, needWindow.cy),
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void layout_bottom_buttons(HWND hParent) {
    if (!hParent) return;
    RECT rc = {};
    GetClientRect(hParent, &rc);
    const int margin = dp(8);
    const int gap = dp(6);
    const int buttonH = dp(30);
    const int smallButtonW = dp(76);
    const int comboDropH = dp(220);
    const int buttonsY = layout_bottom_buttons_y();
    const int profileY = buttonsY + dp(40);
    const int autoY = profileY + dp(34);
    const int serviceY = autoY + dp(26);
    const int hintY = serviceY + dp(26);
    const int statusY = hintY + dp(40);

    if (g_app.hApplyBtn)
        SetWindowPos(g_app.hApplyBtn, nullptr, margin, buttonsY, dp(132), buttonH, SWP_NOZORDER);
    if (g_app.hRefreshBtn)
        SetWindowPos(g_app.hRefreshBtn, nullptr, margin + dp(144), buttonsY, dp(98), buttonH, SWP_NOZORDER);
    if (g_app.hResetBtn)
        SetWindowPos(g_app.hResetBtn, nullptr, margin + dp(254), buttonsY, dp(98), buttonH, SWP_NOZORDER);
    if (g_app.hLicenseBtn)
        SetWindowPos(g_app.hLicenseBtn, nullptr, rc.right - margin - dp(118), buttonsY, dp(118), buttonH, SWP_NOZORDER);

    if (g_app.hProfileLabel)
        SetWindowPos(g_app.hProfileLabel, nullptr, margin, profileY + dp(4), dp(72), dp(18), SWP_NOZORDER);
    if (g_app.hProfileCombo)
        SetWindowPos(g_app.hProfileCombo, nullptr, margin + dp(76), profileY, dp(156), comboDropH, SWP_NOZORDER);
    if (g_app.hProfileLoadBtn)
        SetWindowPos(g_app.hProfileLoadBtn, nullptr, margin + dp(244), profileY, smallButtonW, dp(28), SWP_NOZORDER);
    if (g_app.hProfileSaveBtn)
        SetWindowPos(g_app.hProfileSaveBtn, nullptr, margin + dp(244) + smallButtonW + gap, profileY, smallButtonW, dp(28), SWP_NOZORDER);
    if (g_app.hProfileClearBtn)
        SetWindowPos(g_app.hProfileClearBtn, nullptr, margin + dp(244) + (smallButtonW + gap) * 2, profileY, smallButtonW, dp(28), SWP_NOZORDER);
    if (g_app.hProfileStateLabel) {
        int stateX = margin + dp(244) + (smallButtonW + gap) * 3 + dp(12);
        int stateW = nvmax(dp(140), rc.right - stateX - margin);
        SetWindowPos(g_app.hProfileStateLabel, nullptr, stateX, profileY + dp(4), stateW, dp(18), SWP_NOZORDER);
    }

    if (g_app.hAppLaunchLabel)
        SetWindowPos(g_app.hAppLaunchLabel, nullptr, margin, autoY + dp(4), dp(170), dp(18), SWP_NOZORDER);
    if (g_app.hAppLaunchCombo)
        SetWindowPos(g_app.hAppLaunchCombo, nullptr, margin + dp(174), autoY, dp(170), comboDropH, SWP_NOZORDER);
    if (g_app.hLogonLabel)
        SetWindowPos(g_app.hLogonLabel, nullptr, margin + dp(366), autoY + dp(4), dp(208), dp(18), SWP_NOZORDER);
    if (g_app.hLogonCombo)
        SetWindowPos(g_app.hLogonCombo, nullptr, margin + dp(578), autoY, dp(170), comboDropH, SWP_NOZORDER);
    if (g_app.hStartOnLogonCheck)
        SetWindowPos(g_app.hStartOnLogonCheck, nullptr, margin + dp(760), autoY + dp(4), dp(16), dp(16), SWP_NOZORDER);
    if (g_app.hStartOnLogonLabel)
        SetWindowPos(g_app.hStartOnLogonLabel, nullptr, margin + dp(784), autoY + dp(3), dp(296), dp(18), SWP_NOZORDER);
    if (g_app.hServiceEnableCheck)
        SetWindowPos(g_app.hServiceEnableCheck, nullptr, margin, serviceY + dp(4), dp(16), dp(16), SWP_NOZORDER);
    if (g_app.hServiceEnableLabel)
        SetWindowPos(g_app.hServiceEnableLabel, nullptr, margin + dp(24), serviceY + dp(3), dp(330), dp(18), SWP_NOZORDER);
    if (g_app.hServiceStatusLabel)
        SetWindowPos(g_app.hServiceStatusLabel, nullptr, margin + dp(370), serviceY + dp(3), nvmax(dp(220), rc.right - margin - dp(370)), dp(18), SWP_NOZORDER);
    if (g_app.hLogonHintLabel)
        SetWindowPos(g_app.hLogonHintLabel, nullptr, margin, hintY, nvmax(dp(320), rc.right - margin * 2), dp(34), SWP_NOZORDER);
    if (g_app.hProfileStatusLabel)
        SetWindowPos(g_app.hProfileStatusLabel, nullptr, margin, statusY, nvmax(dp(300), rc.right - margin * 2), dp(18), SWP_NOZORDER);
}


#include "main_state_sync.cpp"
#include "main_service_runtime.cpp"

#include "main_service_ipc.cpp"
#include "main_service_server.cpp"
    switch (r) {
        case NVML_SUCCESS: return "NVML_SUCCESS";
        case NVML_ERROR_UNINITIALIZED: return "NVML_ERROR_UNINITIALIZED";
        case NVML_ERROR_INVALID_ARGUMENT: return "NVML_ERROR_INVALID_ARGUMENT";
        case NVML_ERROR_NOT_SUPPORTED: return "NVML_ERROR_NOT_SUPPORTED";
        case NVML_ERROR_NO_PERMISSION: return "NVML_ERROR_NO_PERMISSION";
        case NVML_ERROR_ALREADY_INITIALIZED: return "NVML_ERROR_ALREADY_INITIALIZED";
        case NVML_ERROR_NOT_FOUND: return "NVML_ERROR_NOT_FOUND";
        case NVML_ERROR_INSUFFICIENT_SIZE: return "NVML_ERROR_INSUFFICIENT_SIZE";
        case NVML_ERROR_FUNCTION_NOT_FOUND: return "NVML_ERROR_FUNCTION_NOT_FOUND";
        case NVML_ERROR_GPU_IS_LOST: return "NVML_ERROR_GPU_IS_LOST";
        case NVML_ERROR_ARG_VERSION_MISMATCH: return "NVML_ERROR_ARGUMENT_VERSION_MISMATCH";
        default: return "NVML_ERROR_OTHER";
    }
}

#include "main_diagnostics.cpp"

#include "main_probe_config.cpp"
    if (!path || !desired) return false;
    initialize_desired_settings_defaults(desired);
    char fanBuf[64] = {};
    char buf[64] = {};
    bool hasExplicitFanMode = false;

    GetPrivateProfileStringA("controls", "gpu_offset_mhz", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        int v = 0;
        if (!parse_int_strict(buf, &v)) {
            set_message(err, errSize, "Invalid gpu_offset_mhz in %s", path);
            return false;
        }
        desired->hasGpuOffset = true;
        desired->gpuOffsetMHz = v;
        if (g_app.gpuClockOffsetMinMHz != 0 || g_app.gpuClockOffsetMaxMHz != 0) {
            if (v < g_app.gpuClockOffsetMinMHz || v > g_app.gpuClockOffsetMaxMHz) {
                debug_log("warning: config gpu_offset_mhz=%d outside driver range %d..%d, clamping\n",
                    v, g_app.gpuClockOffsetMinMHz, g_app.gpuClockOffsetMaxMHz);
                desired->gpuOffsetMHz = v < g_app.gpuClockOffsetMinMHz ? g_app.gpuClockOffsetMinMHz : g_app.gpuClockOffsetMaxMHz;
            }
        }
    }

    GetPrivateProfileStringA("controls", "gpu_offset_exclude_low_count", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (!buf[0]) {
        GetPrivateProfileStringA("controls", "gpu_offset_exclude_low_70", "", buf, sizeof(buf), path);
        trim_ascii(buf);
        if (buf[0]) {
            int value = 0;
            if (!parse_int_strict(buf, &value)) {
                set_message(err, errSize, "Invalid gpu_offset_exclude_low_count in %s", path);
                return false;
            }
            desired->gpuOffsetExcludeLowCount = value ? 70 : 0;
        }
    } else {
        int value = 0;
        if (!parse_int_strict(buf, &value)) {
            set_message(err, errSize, "Invalid gpu_offset_exclude_low_count in %s", path);
            return false;
        }
        desired->gpuOffsetExcludeLowCount = value;
    }

    GetPrivateProfileStringA("controls", "lock_ci", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        int value = -1;
        if (!parse_int_strict(buf, &value)) {
            set_message(err, errSize, "Invalid lock_ci in %s", path);
            return false;
        }
        desired->hasLock = value >= 0;
        desired->lockCi = value;
    }

    GetPrivateProfileStringA("controls", "lock_mhz", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        int value = 0;
        if (!parse_int_strict(buf, &value) || value < 0) {
            set_message(err, errSize, "Invalid lock_mhz in %s", path);
            return false;
        }
        if (value > 0) {
            desired->hasLock = true;
            desired->lockMHz = (unsigned int)value;
        }
    }

    GetPrivateProfileStringA("controls", "mem_offset_mhz", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        int v = 0;
        if (!parse_int_strict(buf, &v)) {
            set_message(err, errSize, "Invalid mem_offset_mhz in %s", path);
            return false;
        }
        desired->hasMemOffset = true;
        desired->memOffsetMHz = v;
        if (g_app.memClockOffsetMinMHz != 0 || g_app.memClockOffsetMaxMHz != 0) {
            if (v < g_app.memClockOffsetMinMHz || v > g_app.memClockOffsetMaxMHz) {
                debug_log("warning: config mem_offset_mhz=%d outside reported driver range %d..%d; preserving requested value\n",
                    v, g_app.memClockOffsetMinMHz, g_app.memClockOffsetMaxMHz);
            }
        }
    }

    GetPrivateProfileStringA("controls", "power_limit_pct", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        int v = 0;
        if (!parse_int_strict(buf, &v)) {
            set_message(err, errSize, "Invalid power_limit_pct in %s", path);
            return false;
        }
        desired->hasPowerLimit = true;
        if (v < 50 || v > 150) {
            set_message(err, errSize, "power_limit_pct %d is outside the safe range 50..150", v);
            return false;
        }
        desired->powerLimitPct = v;
    }

    GetPrivateProfileStringA("controls", "fan_mode", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        int fanMode = FAN_MODE_AUTO;
        if (!parse_fan_mode_config_value(buf, &fanMode)) {
            set_message(err, errSize, "Invalid fan_mode in %s", path);
            return false;
        }
        desired->hasFan = true;
        desired->fanMode = fanMode;
        desired->fanAuto = fanMode == FAN_MODE_AUTO;
        hasExplicitFanMode = true;
    }

    GetPrivateProfileStringA("controls", "fan", "", fanBuf, sizeof(fanBuf), path);
    trim_ascii(fanBuf);
    if (fanBuf[0]) {
        bool fanAuto = false;
        int fanPercent = 0;
        if (!parse_fan_value(fanBuf, &fanAuto, &fanPercent)) {
            set_message(err, errSize, "Invalid fan setting in %s", path);
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

    GetPrivateProfileStringA("controls", "fan_fixed_pct", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        int value = 0;
        if (!parse_int_strict(buf, &value)) {
            set_message(err, errSize, "Invalid fan_fixed_pct in %s", path);
            return false;
        }
        if (!hasExplicitFanMode || desired->fanMode == FAN_MODE_FIXED) {
            desired->hasFan = true;
            desired->fanMode = FAN_MODE_FIXED;
            desired->fanAuto = false;
            desired->fanPercent = clamp_percent(value);
        }
    }

    if (!load_fan_curve_config_from_section(path, "fan_curve", &desired->fanCurve, err, errSize)) return false;

    if (!load_curve_points_explicit_from_section(path, "curve", desired, err, errSize)) {
        set_message(err, errSize, "Config is missing explicit [curve] point*_mhz entries in %s", path);
        return false;
    }

    if (curve_section_uses_base_plus_gpu_offset_semantics(path, "curve", desired)) {
        restore_curve_points_from_base_plus_gpu_offset(desired);
    }

    return true;
}

#include "config_profiles.cpp"
#include "config_profiles_ui.cpp"

#include "fan_curve_dialog.cpp"

static bool capture_gui_desired_settings(DesiredSettings* desired, bool includeCurrentGlobals, bool expandLockedTail, bool captureAllCurvePoints, char* err, size_t errSize) {
    if (!desired) return false;
    initialize_desired_settings_defaults(desired);

    char buf[64] = {};
    int parsedCurveMHz[VF_NUM_POINTS] = {};
    bool parsedCurveHave[VF_NUM_POINTS] = {};
    ControlState control = {};
    bool haveControlState = get_effective_control_state(&control);
    bool forceExplicitGlobals = g_app.usingBackgroundService;
    int currentActiveGpuOffsetExcludeLowCount = haveControlState && control.hasGpuOffset ? control.gpuOffsetExcludeLowCount : (current_applied_gpu_offset_excludes_low_points() ? g_app.appliedGpuOffsetExcludeLowCount : 0);
    int currentGpuOffsetMHz = haveControlState && control.hasGpuOffset ? control.gpuOffsetMHz : current_applied_gpu_offset_mhz();
    int currentGpuOffsetExcludeLowCount = currentActiveGpuOffsetExcludeLowCount;
    get_window_text_safe(g_app.hGpuOffsetEdit, buf, sizeof(buf));
    int gpuOffsetMHz = currentGpuOffsetMHz;
    if (buf[0]) {
        if (!parse_int_strict(buf, &gpuOffsetMHz)) {
            set_message(err, errSize, "Invalid GPU offset");
            return false;
        }
    }
    int gpuOffsetExcludeLowCount = g_app.guiGpuOffsetExcludeLowCount;
    int desiredActiveGpuOffsetExcludeLowCount = (gpuOffsetExcludeLowCount > 0 && gpuOffsetMHz != 0) ? gpuOffsetExcludeLowCount : 0;
    debug_log("capture_gui: gpuOffsetMHz=%d excludeState=%d desiredExclude=%d currentExclude=%d\n",
        gpuOffsetMHz, gpuOffsetExcludeLowCount, desiredActiveGpuOffsetExcludeLowCount, currentGpuOffsetExcludeLowCount);
    g_app.guiGpuOffsetMHz = gpuOffsetMHz;
    g_app.guiGpuOffsetExcludeLowCount = gpuOffsetExcludeLowCount;

    bool hasLock = g_app.lockedVi >= 0 && g_app.lockedVi < g_app.numVisible;
    int lockCi = -1;
    int effectiveLockTargetMHz = 0;
    unsigned int currentLockMHz = 0;
#include "main_runtime_control.cpp"
#include "main_runtime_nvml.cpp"
#include "main_runtime_gpu.cpp"
#include "main_runtime_ui.cpp"
static void draw_lock_checkbox(const DRAWITEMSTRUCT* dis) {
    if (!dis) return;
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    int vi = (int)dis->CtlID - LOCK_BASE_ID;
    bool checked = (vi >= 0 && vi < g_app.numVisible && vi == g_app.lockedVi);

    HBRUSH bg = CreateSolidBrush(COL_BG);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    int boxSize = nvmin(rc.right - rc.left, rc.bottom - rc.top) - dp(2);
    if (boxSize < dp(10)) boxSize = dp(10);
    RECT box = {
        rc.left + (rc.right - rc.left - boxSize) / 2,
        rc.top + (rc.bottom - rc.top - boxSize) / 2,
        rc.left + (rc.right - rc.left - boxSize) / 2 + boxSize,
        rc.top + (rc.bottom - rc.top - boxSize) / 2 + boxSize,
    };

    COLORREF border = disabled ? RGB(0x5A, 0x5A, 0x68) : COL_BUTTON_BORDER;
    COLORREF fill = disabled ? COL_BUTTON_DISABLED : (checked ? COL_BUTTON : COL_PANEL);
    HBRUSH fillBr = CreateSolidBrush(fill);
    FillRect(hdc, &box, fillBr);
    DeleteObject(fillBr);

    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, box.left, box.top, box.right + 1, box.bottom + 1);
