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

#include "ui_main_layout.cpp"

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

static void set_edit_value(HWND, unsigned int) {
}

static void populate_edits() {
}

static void apply_lock(int, LockMode) {
}

static void unlock_all() {
}
#endif

#include "main_data_paths.cpp"
#include "main_state_sync.cpp"
#include "main_service_recovery_clock.cpp"
#include "main_service_recovery_ledger.cpp"
#include "main_service_persist.cpp"
#include "main_service_recovery.cpp"
#include "main_service_runtime.cpp"
// The continuous VF-drift monitor + auto-reapply was removed in 0.18 (it fought
// NVIDIA's expected temperature/boost curve drift by re-applying the whole OC under
// game load — a TDR risk — and looped forever on a below-floor flatten target).
// Settings persist only via the event-driven reapply worker (resume/recovery/logon).

#include "main_service_selected_gpu_pnp.cpp"
#include "main_service_sessions.cpp"
#include "main_service_lifecycle_events.cpp"
#include "main_service_dxgi_readiness.cpp"
#include "main_service_lifecycle_apply.cpp"
#include "main_service_logon_coordinator.cpp"
#include "main_service_controlled_restart.cpp"

#include "main_service_ipc.cpp"
#include "main_service_install.cpp"
#include "main_service_server.cpp"

#include "main_diagnostics.cpp"
#include "main_secure_write.cpp"

#include "main_probe_config.cpp"
    if (!path || !desired) return false;
    initialize_desired_settings_defaults(desired);
    char fanBuf[64] = {};
    char buf[64] = {};
    bool hasExplicitFanMode = false;

    gc_GetPrivateProfileStringUtf8("controls", "gpu_offset_mhz", "", buf, sizeof(buf), path);
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

    gc_GetPrivateProfileStringUtf8("controls", "gpu_offset_exclude_low_count", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (!buf[0]) {
        gc_GetPrivateProfileStringUtf8("controls", "gpu_offset_exclude_low_70", "", buf, sizeof(buf), path);
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

    gc_GetPrivateProfileStringUtf8("controls", "lock_ci", "", buf, sizeof(buf), path);
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

    gc_GetPrivateProfileStringUtf8("controls", "lock_mhz", "", buf, sizeof(buf), path);
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

    gc_GetPrivateProfileStringUtf8("controls", "mem_offset_mhz", "", buf, sizeof(buf), path);
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

    gc_GetPrivateProfileStringUtf8("controls", "power_limit_pct", "", buf, sizeof(buf), path);
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

    gc_GetPrivateProfileStringUtf8("controls", "fan_mode", "", buf, sizeof(buf), path);
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

    gc_GetPrivateProfileStringUtf8("controls", "fan", "", fanBuf, sizeof(fanBuf), path);
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

    gc_GetPrivateProfileStringUtf8("controls", "fan_fixed_pct", "", buf, sizeof(buf), path);
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

    char curveLoadErr[256] = {};
    if (!load_curve_points_explicit_from_section(path, "curve", desired, curveLoadErr, sizeof(curveLoadErr))) {
        if (curveLoadErr[0]) {
            set_message(err, errSize, "%s", curveLoadErr);
            return false;
        }
        debug_log("load_desired_settings_from_ini: config has no explicit [curve] point*_mhz entries in %s\n", path);
    }

    if (curve_section_uses_base_plus_gpu_offset_semantics(path, "curve", desired)) {
        restore_curve_points_from_base_plus_gpu_offset(desired);
    }

    repair_profile_locked_curve_readback_artifacts(path, "curve", 1, desired);

    return true;
}

#include "config_profiles.cpp"
#include "config_profiles_machine.cpp"
#include "config_profile_sync_cache.cpp"
#include "config_profiles_ui.cpp"
#include "main_startup_profiles.cpp"

#include "fan_curve_dialog.cpp"

static bool capture_gui_desired_settings(DesiredSettings* desired, bool includeCurrentGlobals, bool expandLockedTail, bool captureAllCurvePoints, char* err, size_t errSize) {
    if (!desired) return false;
    initialize_desired_settings_defaults(desired);

    int parsedCurveMHz[VF_NUM_POINTS] = {};
    bool parsedCurveHave[VF_NUM_POINTS] = {};
    ControlState control = {};
    bool haveControlState = get_effective_control_state(&control);
    bool forceExplicitGlobals = g_app.usingBackgroundService;
    int currentActiveGpuOffsetExcludeLowCount = haveControlState && control.hasGpuOffset ? control.gpuOffsetExcludeLowCount : (current_applied_gpu_offset_excludes_low_points() ? g_app.appliedGpuOffsetExcludeLowCount : 0);
    int currentGpuOffsetMHz = haveControlState && control.hasGpuOffset ? control.gpuOffsetMHz : current_applied_gpu_offset_mhz();
    int currentGpuOffsetExcludeLowCount = currentActiveGpuOffsetExcludeLowCount;
    int gpuOffsetMHz = currentGpuOffsetMHz;
    if (gui_state_dirty()) {
        if (!parse_int_strict(g_app.guiDraft.gpuOffsetText,
                &gpuOffsetMHz)) {
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
    // If no user has actively edited the GUI since the last profile load/apply,
    // and the lock checkbox's MHz matches the live curve frequency at that point
    // AND the curve point was NOT explicitly loaded from a profile (guiCurvePointExplicit
    // is false), the lock state is stale (left over from a previous profile's apply)
    // and should not be captured.
    if (hasLock && !g_app.guiHasUserModifiedValues && g_app.lockedFreq == 0) {
        int lockCi = g_app.visibleMap[g_app.lockedVi];
        unsigned int liveMHz = displayed_curve_mhz(g_app.curve[lockCi].freq_kHz);
        if (liveMHz > 0 && !g_app.guiCurvePointExplicit[lockCi]) {
            hasLock = false;
            debug_log("capture_gui_desired_settings: skipping stale lock at ci=%d (lockedFreq=0, matches live %u MHz, not explicit in profile)\n",
                lockCi, liveMHz);
        }
    }
    int lockCi = -1;
    int effectiveLockTargetMHz = 0;
    unsigned int currentLockMHz = 0;
#include "main_runtime_control.cpp"
#include "main_tray_autostart.cpp"
#include "main_startup_task_runtime.cpp"
#include "main_startup_task_definition.cpp"
#include "main_runtime_nvml.cpp"
#include "main_runtime_gpu.cpp"
#include "main_runtime_ui.cpp"
#include "ui_theme_checkbox.cpp"
static void draw_lock_checkbox(const DRAWITEMSTRUCT* dis) {
    if (!dis) return;
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    int vi = (int)dis->CtlID - LOCK_BASE_ID;
    bool isActive = (vi >= 0 && vi < g_app.numVisible && vi == g_app.lockedVi);
    LockMode mode = isActive ? g_app.lockMode : LOCK_MODE_NONE;

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
    COLORREF fill = disabled ? COL_BUTTON_DISABLED : (mode != LOCK_MODE_NONE ? COL_BUTTON : COL_PANEL);
    HBRUSH fillBr = CreateSolidBrush(fill);
    FillRect(hdc, &box, fillBr);
    DeleteObject(fillBr);

    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, box.left, box.top, box.right + 1, box.bottom + 1);

    // Distinct glyphs so the two lock modes are unmistakable at a glance:
    //   FLATTEN (cap the tail) -> checkmark
    //   HARD (pin via NVML)    -> filled center dot
    if (mode == LOCK_MODE_FLATTEN && !disabled) {
        // Share the anti-aliased GDI+ checkmark renderer used by the themed
        // checkboxes (service install / share-all-users / tray) so the FLATTEN
        // tick is visually identical to them instead of a jagged raw-GDI Polyline.
        // Color matches draw_themed_button()'s checked tick (RGB 0xE8,0xF2,0xFF).
        draw_checkbox_tick_smooth(hdc, &box, RGB(0xE8, 0xF2, 0xFF));
    } else if (mode == LOCK_MODE_HARD && !disabled) {
        int cx = (box.left + box.right) / 2;
        int cy = (box.top + box.bottom) / 2;
        int dotR = boxSize / 4;
        if (dotR < dp(2)) dotR = dp(2);
        HBRUSH dotBr = CreateSolidBrush(COL_TEXT);
        HPEN dotPen = CreatePen(PS_SOLID, 1, COL_TEXT);
        SelectObject(hdc, dotPen);
        SelectObject(hdc, dotBr);
        Ellipse(hdc, cx - dotR, cy - dotR, cx + dotR + 1, cy + dotR + 1);
        DeleteObject(dotBr);
        DeleteObject(dotPen);
    }

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
