// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// ============================================================================
// Main Window
// ============================================================================

static void apply_changes() {
    if (!g_app.backgroundServiceAvailable || !g_app.loaded) return;
    if (g_app.applyInFlight) return;
    g_app.applyInFlight = true;
    if (g_app.hApplyBtn) EnableWindow(g_app.hApplyBtn, FALSE);
    set_pending_operation_source("GUI apply");
    record_ui_action("Apply clicked");
    DesiredSettings desired = {};
    char err[256] = {};
    if (g_app.usingBackgroundService && !refresh_service_snapshot_and_active_desired(err, sizeof(err))) {
        write_error_report_log_for_user_failure("GUI apply refresh failed", err);
        MessageBoxA(g_app.hMainWnd, err, "Green Curve", MB_OK | MB_ICONERROR);
        g_app.applyInFlight = false;
        if (g_app.hApplyBtn) EnableWindow(g_app.hApplyBtn, TRUE);
        return;
    }
    if (!capture_gui_apply_settings(&desired, err, sizeof(err))) {
        write_error_report_log_for_user_failure("GUI apply validation failed", err);
        MessageBoxA(g_app.hMainWnd, err, "Green Curve", MB_OK | MB_ICONERROR);
        g_app.applyInFlight = false;
        if (g_app.hApplyBtn) EnableWindow(g_app.hApplyBtn, TRUE);
        return;
    }
    char result[512] = {};
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    bool ok = apply_desired_settings(&desired, true, result, sizeof(result));
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    if (ok) {
        set_gui_state_dirty(false);
        populate_global_controls();
        populate_edits();
        invalidate_main_window();
    }
    boost_fan_telemetry_for_ms(3000);
    refresh_live_fan_telemetry(true);
    MessageBoxA(g_app.hMainWnd, result, "Green Curve", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING));
    g_app.applyInFlight = false;
    if (g_app.hApplyBtn) EnableWindow(g_app.hApplyBtn, TRUE);
}

static void destroy_edit_controls(HWND hParent) {
    HWND child = GetWindow(hParent, GW_CHILD);
    while (child) {
        HWND next = GetWindow(child, GW_HWNDNEXT);
        LONG_PTR id = GetWindowLongPtr(child, GWLP_ID);
        if (id != APPLY_BTN_ID && id != REFRESH_BTN_ID && id != RESET_BTN_ID && id != LICENSE_BTN_ID
            && id != PROFILE_COMBO_ID && id != PROFILE_LOAD_ID && id != PROFILE_SAVE_ID && id != PROFILE_CLEAR_ID
            && id != APP_LAUNCH_COMBO_ID && id != LOGON_COMBO_ID
            && id != PROFILE_LABEL_ID && id != PROFILE_STATE_ID && id != APP_LAUNCH_LABEL_ID
            && id != LOGON_LABEL_ID && id != PROFILE_STATUS_ID && id != START_ON_LOGON_CHECK_ID
            && id != START_ON_LOGON_LABEL_ID
            && id != SERVICE_ENABLE_CHECK_ID && id != SERVICE_ENABLE_LABEL_ID && id != SERVICE_STATUS_ID
            && id != LOGON_HINT_ID) {
            DestroyWindow(child);
        }
        child = next;
    }
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        g_app.hEditsMhz[i] = nullptr;
        g_app.hEditsMv[i] = nullptr;
        g_app.hLocks[i] = nullptr;
    }
    g_app.hGpuOffsetEdit = nullptr;
    g_app.hGpuOffsetExcludeLowEdit = nullptr;
    g_app.hGpuOffsetExcludeLowLabel = nullptr;
    g_app.hMemOffsetEdit = nullptr;
    g_app.hPowerLimitEdit = nullptr;
    g_app.hFanEdit = nullptr;
    g_app.hFanModeCombo = nullptr;
    g_app.hFanCurveBtn = nullptr;
}

static void refresh_curve() {
    if (!g_app.backgroundServiceAvailable) {
        refresh_background_service_state();
        update_all_gui_for_service_state();
        return;
    }
    if (g_app.usingBackgroundService && !g_app.isServiceProcess) {
        char detail[256] = {};
        ServiceSnapshot snapshot = {};
        int oldNumVisible = g_app.numVisible;
        if (service_client_get_snapshot(&snapshot, detail, sizeof(detail))) {
            apply_service_snapshot_to_app(&snapshot);
            if (g_app.numVisible != oldNumVisible) {
                destroy_edit_controls(g_app.hMainWnd);
                create_edit_controls(g_app.hMainWnd, g_app.hInst);
            } else {
                update_all_gui_for_service_state();
            }
            update_background_service_controls();
            ensure_main_window_min_size(g_app.hMainWnd);
            invalidate_main_window();
        } else {
            refresh_background_service_state();
            clear_service_authoritative_state();
            update_all_gui_for_service_state();
            debug_log("Green Curve: Failed to read service snapshot: %s\n", detail);
        }
        return;
    }
}

static void reset_curve() {
    if (!g_app.backgroundServiceAvailable || !g_app.loaded) return;

    int confirm = MessageBoxA(g_app.hMainWnd,
        "Reset will restore all GPU settings to their default values.\n\nContinue?",
        "Confirm Reset",
        MB_YESNO | MB_ICONWARNING);
    if (confirm != IDYES) return;

    if (g_app.usingBackgroundService && !g_app.isServiceProcess) {
        char result[512] = {};
        ServiceSnapshot snapshot = {};
        bool ok = service_client_reset(result, sizeof(result), &snapshot);
        if (ok && (snapshot.initialized || snapshot.loaded || g_app.fanSupported || snapshot.fanSupported)) {
            clear_service_authoritative_state();
            apply_service_snapshot_to_app(&snapshot);
            if (ok) {
                g_app.guiFanMode = FAN_MODE_AUTO;
                g_app.guiFanFixedPercent = 0;
                fan_curve_set_default(&g_app.guiFanCurve);
                g_app.lockedVi = -1;
                g_app.lockedCi = -1;
                g_app.lockedFreq = 0;
                set_gui_state_dirty(false);
            }
            destroy_edit_controls(g_app.hMainWnd);
            create_edit_controls(g_app.hMainWnd, g_app.hInst);
            populate_global_controls();
            update_background_service_controls();
            invalidate_main_window();
        }
        boost_fan_telemetry_for_ms(3000);
        refresh_live_fan_telemetry(true);
        MessageBoxA(g_app.hMainWnd, result, "Green Curve", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING));
        return;
    }

}

// ============================================================================
// Main Window
// ============================================================================

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            create_backbuffer(hwnd);
            if (!g_app.hCachedGridPen) g_app.hCachedGridPen = CreatePen(PS_SOLID, 1, COL_GRID);
            if (!g_app.hCachedAxisPen) g_app.hCachedAxisPen = CreatePen(PS_SOLID, 1, COL_AXIS);
            if (!g_app.hCachedFont) g_app.hCachedFont = CreateFontA(dp(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
            if (!g_app.hCachedFontSmall) g_app.hCachedFontSmall = CreateFontA(dp(11), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
            apply_system_titlebar_theme(hwnd);
            allow_dark_mode_for_window(hwnd);
            refresh_background_service_state();
            update_background_service_controls();
            update_fan_telemetry_timer();
            ensure_main_window_min_size(hwnd);
            layout_bottom_buttons(hwnd);
            return 0;

        default:
            if (g_taskbarCreatedMessage != 0 && msg == g_taskbarCreatedMessage) {
                g_app.trayIconAdded = false;
                g_app.trayLastRenderedValid = false;
                if (!IsWindowVisible(hwnd)) {
                    ensure_tray_icon();
                }
                return 0;
            }
            break;

        case WM_SIZE: {
            if (wParam == SIZE_MINIMIZED) {
                hide_main_window_to_tray();
                return 0;
            }
            destroy_backbuffer();
            create_backbuffer(hwnd);
            layout_bottom_buttons(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_SYSCOMMAND:
            if ((wParam & 0xFFF0) == SC_MINIMIZE) {
                hide_main_window_to_tray();
                return 0;
            }
            break;

        case WM_ERASEBKGND:
            fill_window_background(hwnd, (HDC)wParam);
            return 1;

        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            apply_system_titlebar_theme(hwnd);
            allow_dark_mode_for_window(hwnd);
            refresh_menu_theme_cache();
            break;

        case APP_WM_SYNC_STARTUP:
            close_startup_sync_thread_handle();
            g_app.startupSyncInFlight = false;
            if (g_app.hLogonCombo) {
                int slot = (int)wParam;
                if (slot >= 0 && slot <= CONFIG_NUM_SLOTS)
                    SendMessageA(g_app.hLogonCombo, CB_SETCURSEL, (WPARAM)slot, 0);
            }
            update_profile_state_label();
            return 0;

        case APP_WM_TRAYICON: {
            UINT trayEvent = LOWORD(lParam);
            switch (trayEvent) {
                case WM_CONTEXTMENU:
                case WM_RBUTTONUP:
                    show_tray_menu(hwnd);
                    return 0;
                case WM_LBUTTONUP:
                case WM_LBUTTONDBLCLK:
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    show_main_window_from_tray();
                    return 0;
            }
            break;
        }

        case WM_TIMER:
            if (wParam == FAN_CURVE_TIMER_ID) {
                apply_fan_curve_tick();
                return 0;
            }
            if (wParam == FAN_TELEMETRY_TIMER_ID) {
                bool redrawControls = window_should_redraw_fan_controls();
                refresh_live_fan_telemetry(redrawControls);
                update_fan_telemetry_timer();
                return 0;
            }
            break;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);

            fill_window_background(hwnd, hdc);

            if (!g_app.hMemDC || !g_app.hMemBmp) create_backbuffer(hwnd);
            if (!g_app.hMemDC) {
                EndPaint(hwnd, &ps);
                return 0;
            }

            int graphH = dp(GRAPH_HEIGHT);

            HBRUSH bg = CreateSolidBrush(COL_BG);
            FillRect(g_app.hMemDC, &rc, bg);
            DeleteObject(bg);

            draw_graph(g_app.hMemDC, &rc);

            HPEN sepPen = CreatePen(PS_SOLID, 1, COL_GRID);
            HPEN oldPen = (HPEN)SelectObject(g_app.hMemDC, sepPen);
            MoveToEx(g_app.hMemDC, 0, graphH, nullptr);
            LineTo(g_app.hMemDC, rc.right, graphH);
            SelectObject(g_app.hMemDC, oldPen);
            DeleteObject(sepPen);

            BitBlt(hdc, 0, 0, rc.right, rc.bottom, g_app.hMemDC, 0, 0, SRCCOPY);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
            if (dis && dis->CtlType == ODT_BUTTON) {
                if (dis->CtlID >= LOCK_BASE_ID && dis->CtlID < LOCK_BASE_ID + VF_NUM_POINTS) {
                    draw_lock_checkbox(dis);
                    return TRUE;
                }
                if (is_themed_button_id(dis->CtlID) || is_themed_checkbox_id(dis->CtlID)) {
                    draw_themed_button(dis);
                    return TRUE;
                }
            }
            return FALSE;
        }

        case WM_CTLCOLORBTN: {
            HDC hdcBtn = (HDC)wParam;
            SetBkColor(hdcBtn, COL_BG);
            if (!g_hBtnBr) g_hBtnBr = CreateSolidBrush(COL_BG);
            return (LRESULT)g_hBtnBr;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdcStatic = (HDC)wParam;
            HWND hCtl = (HWND)lParam;
            char className[16] = {};
            if (hCtl) GetClassNameA(hCtl, className, ARRAY_COUNT(className));
            LONG_PTR style = hCtl ? GetWindowLongPtrA(hCtl, GWL_STYLE) : 0;
            bool isEditInput = strcmp(className, "Edit") == 0 &&
                (((style & ES_READONLY) != 0) || !IsWindowEnabled(hCtl));
            if (hCtl == g_app.hFanModeCombo || hCtl == g_app.hProfileCombo || hCtl == g_app.hAppLaunchCombo || hCtl == g_app.hLogonCombo || isEditInput) {
                SetTextColor(hdcStatic, IsWindowEnabled(hCtl) ? COL_TEXT : COL_LABEL);
                SetBkColor(hdcStatic, COL_INPUT);
                if (!g_hInputBr) g_hInputBr = CreateSolidBrush(COL_INPUT);
                return (LRESULT)g_hInputBr;
            }
            SetTextColor(hdcStatic, COL_LABEL);
            SetBkColor(hdcStatic, COL_BG);
            if (!g_hStaticBr) g_hStaticBr = CreateSolidBrush(COL_BG);
            return (LRESULT)g_hStaticBr;
        }

        case WM_CTLCOLORLISTBOX: {
            HDC hdcList = (HDC)wParam;
            SetTextColor(hdcList, COL_TEXT);
            SetBkColor(hdcList, COL_INPUT);
            if (!g_hListBr) g_hListBr = CreateSolidBrush(COL_INPUT);
            return (LRESULT)g_hListBr;
        }

        case WM_CTLCOLOREDIT: {
            HDC hdcEdit = (HDC)wParam;
            HWND hCtl = (HWND)lParam;
            SetTextColor(hdcEdit, (hCtl && IsWindowEnabled(hCtl)) ? COL_TEXT : COL_LABEL);
            SetBkColor(hdcEdit, COL_INPUT);
            if (!g_hEditBr) g_hEditBr = CreateSolidBrush(COL_INPUT);
            return (LRESULT)g_hEditBr;
        }

        case WM_COMMAND:
            if (HIWORD(wParam) == EN_CHANGE && LOWORD(wParam) >= 1000 && LOWORD(wParam) < 1000 + VF_NUM_POINTS) {
                int vi = LOWORD(wParam) - 1000;
                if (!programmatic_edit_update_active() && vi >= 0 && vi < g_app.numVisible) {
                    int ci = g_app.visibleMap[vi];
                    if (ci >= 0 && ci < VF_NUM_POINTS) {
                        bool lockTailPreviewPoint = (g_app.lockedVi >= 0 && vi > g_app.lockedVi);
                        if (!lockTailPreviewPoint) {
                            g_app.guiCurvePointExplicit[ci] = true;
                            set_gui_state_dirty(true);
                        }
                        char pointBuf[32] = {};
                        get_window_text_safe(g_app.hEditsMhz[vi], pointBuf, sizeof(pointBuf));
                        int pointMHz = 0;
                        if (!lockTailPreviewPoint && parse_int_strict(pointBuf, &pointMHz) && pointMHz > 0) {
                            record_ui_action("point %d edited to %d MHz", ci, pointMHz);
                        }
                    }
                }
                if (!programmatic_edit_update_active() && vi == g_app.lockedVi) {
                    sync_locked_tail_preview_from_anchor();
                    return 0;
                }
            }
            if (HIWORD(wParam) == EN_CHANGE && LOWORD(wParam) == GPU_OFFSET_ID && !programmatic_edit_update_active()) {
                char buf[32] = {};
                get_window_text_safe(g_app.hGpuOffsetEdit, buf, sizeof(buf));
                int value = 0;
                set_gui_state_dirty(true);
                if (parse_int_strict(buf, &value)) record_ui_action("GPU offset edited to %d MHz", value);
            }
            if (HIWORD(wParam) == EN_CHANGE && LOWORD(wParam) == MEM_OFFSET_ID && !programmatic_edit_update_active()) {
                char buf[32] = {};
                get_window_text_safe(g_app.hMemOffsetEdit, buf, sizeof(buf));
                int value = 0;
                set_gui_state_dirty(true);
                if (parse_int_strict(buf, &value)) record_ui_action("Mem offset edited to %d MHz", value);
            }
            if (HIWORD(wParam) == EN_CHANGE && LOWORD(wParam) == POWER_LIMIT_ID && !programmatic_edit_update_active()) {
                char buf[32] = {};
                get_window_text_safe(g_app.hPowerLimitEdit, buf, sizeof(buf));
                int value = 0;
                set_gui_state_dirty(true);
                if (parse_int_strict(buf, &value)) record_ui_action("Power limit edited to %d%%", value);
            }
            if (LOWORD(wParam) == APPLY_BTN_ID) {
                apply_changes();
            } else if (LOWORD(wParam) == REFRESH_BTN_ID) {
                refresh_curve();
            } else if (LOWORD(wParam) == RESET_BTN_ID) {
                reset_curve();
            } else if (LOWORD(wParam) == FAN_MODE_COMBO_ID && HIWORD(wParam) == CBN_SELCHANGE) {
                int selection = (int)SendMessageA(g_app.hFanModeCombo, CB_GETCURSEL, 0, 0);
                if (selection >= FAN_MODE_AUTO && selection <= FAN_MODE_CURVE) {
                    g_app.guiFanMode = selection;
                    set_gui_state_dirty(true);
                    update_fan_controls_enabled_state();
                }
            } else if (LOWORD(wParam) == GPU_SELECT_COMBO_ID && HIWORD(wParam) == CBN_SELCHANGE) {
                apply_gpu_selection_from_ui();
            } else if (LOWORD(wParam) == GPU_OFFSET_EXCLUDE_LOW_EDIT_ID && HIWORD(wParam) == EN_CHANGE) {
                if (!g_app.backgroundServiceAvailable || !g_app.gpuOffsetRangeKnown) return 0;
                if (!programmatic_edit_update_active()) {
                    char excludeBuf[16] = {};
                    get_window_text_safe(g_app.hGpuOffsetExcludeLowEdit, excludeBuf, sizeof(excludeBuf));
                    int excludeCount = 0;
                    if (excludeBuf[0] && parse_int_strict(excludeBuf, &excludeCount)) {
                        if (excludeCount < 0) excludeCount = 0;
                        g_app.guiGpuOffsetExcludeLowCount = excludeCount;
                    }
                    set_gui_state_dirty(true);
                }
            } else if (LOWORD(wParam) == FAN_CURVE_BTN_ID && HIWORD(wParam) == BN_CLICKED) {
                open_fan_curve_dialog();
            } else if ((LOWORD(wParam) == START_ON_LOGON_CHECK_ID && HIWORD(wParam) == BN_CLICKED) ||
                       (LOWORD(wParam) == START_ON_LOGON_LABEL_ID && HIWORD(wParam) == STN_CLICKED)) {
                bool enabled = !is_start_on_logon_enabled(g_app.configPath);
                SendMessageA(g_app.hStartOnLogonCheck, BM_SETCHECK, (WPARAM)(enabled ? BST_CHECKED : BST_UNCHECKED), 0);
                InvalidateRect(g_app.hStartOnLogonCheck, nullptr, FALSE);
                bool previous = is_start_on_logon_enabled(g_app.configPath);
                int logonSlot = get_config_int(g_app.configPath, "profiles", "logon_slot", 0);
                if (logonSlot < 0 || logonSlot > CONFIG_NUM_SLOTS) logonSlot = 0;
                char err[256] = {};
                if (!set_start_on_logon_enabled(g_app.configPath, enabled) ||
                    !set_startup_task_enabled(should_enable_startup_task_from_config(g_app.configPath), err, sizeof(err))) {
                    set_start_on_logon_enabled(g_app.configPath, previous);
                    SendMessageA(g_app.hStartOnLogonCheck, BM_SETCHECK, (WPARAM)(previous ? BST_CHECKED : BST_UNCHECKED), 0);
                    write_error_report_log_for_user_failure("Logon startup update failed", err[0] ? err : "Failed to update logon startup");
                    MessageBoxA(g_app.hMainWnd, err[0] ? err : "Failed to update logon startup", "Green Curve", MB_OK | MB_ICONERROR);
                    break;
                }
                refresh_profile_controls_from_config();
                update_background_service_controls();
                if (enabled) {
                    set_profile_status_text(logonSlot > 0
                        ? (g_app.usingBackgroundService
                            ? "At Windows logon, slot %d will be applied through the background service and the tray client will start hidden."
                            : "At Windows logon, slot %d will be applied and Green Curve will start hidden in the tray.")
                        : (g_app.usingBackgroundService
                            ? "The tray client will start hidden at Windows logon while the background service owns GPU control."
                            : "Green Curve will start hidden in the tray at Windows logon."),
                        logonSlot);
                } else {
                    set_profile_status_text(logonSlot > 0
                        ? (g_app.usingBackgroundService
                            ? "At Windows logon, slot %d will still be applied through the background service even if the tray client does not stay running."
                            : "At Windows logon, slot %d will be applied silently without showing the tray icon.")
                        : "Program start at Windows logon disabled.",
                        logonSlot);
                }
            } else if ((LOWORD(wParam) == SERVICE_ENABLE_CHECK_ID && HIWORD(wParam) == BN_CLICKED) ||
                       (LOWORD(wParam) == SERVICE_ENABLE_LABEL_ID && HIWORD(wParam) == STN_CLICKED)) {
                if (g_app.backgroundServiceToggleInFlight) {
                    break;
                }
                bool repair = g_app.backgroundServiceInstalled && g_app.backgroundServiceBroken;
                bool enable = repair || !g_app.backgroundServiceInstalled;
                const char* confirmText = repair
                    ? "Repair and restart the background service using the current service binary?"
                    : enable
                    ? "Install the elevated background service to enable live GPU control?"
                    : "Remove the background service? Live GPU control will be unavailable until it is installed again.";
                int confirm = MessageBoxA(g_app.hMainWnd, confirmText, "Confirm Service Change", MB_YESNO | MB_ICONQUESTION);
                if (confirm != IDYES) {
                    break;
                }
                char err[256] = {};
                bool ok = false;
                begin_background_service_toggle(enable);
                update_background_service_controls();
                if (!is_elevated()) {
                    ok = launch_service_admin_helper(enable, err, sizeof(err));
                    if (ok) {
                        if (enable) {
                            ok = wait_for_background_service_ready(5000, err, sizeof(err));
                        } else {
                            refresh_background_service_state();
                        }
                    }
                } else {
                    ok = service_install_or_remove(enable, err, sizeof(err));
                    if (ok && enable) {
                        ok = wait_for_background_service_ready(15000, err, sizeof(err));
                    }
                }
                if (!ok) {
                    end_background_service_toggle();
                    refresh_background_service_state();
                    update_background_service_controls();
                    MessageBoxA(g_app.hMainWnd, err[0] ? err : "Failed updating background service.", "Green Curve", MB_OK | MB_ICONERROR);
                    break;
                }
                end_background_service_toggle();
                refresh_background_service_state();
                update_background_service_controls();
                schedule_logon_combo_sync();
                if (enable) {
                    set_profile_status_text(repair
                        ? "Background service repaired. Live OC, UV, power, and fan control is now available."
                        : "Background service installed. Live OC, UV, power, and fan control is now available.");
                    refresh_curve();
                } else {
                    set_profile_status_text("Background service removed. Live GPU control is unavailable until the service is installed again.");
                    refresh_background_service_state();
                    update_background_service_controls();
                    populate_global_controls();
                    if (g_app.loaded) populate_edits();
                    invalidate_main_window();
                }
            } else if (LOWORD(wParam) == PROFILE_COMBO_ID && HIWORD(wParam) == CBN_SELCHANGE) {
                int slot = (int)SendMessageA(g_app.hProfileCombo, CB_GETCURSEL, 0, 0);
                if (slot < 0) slot = CONFIG_DEFAULT_SLOT - 1;
                slot += 1;
                set_config_int(g_app.configPath, "profiles", "selected_slot", slot);
                update_profile_state_label();
                update_profile_action_buttons();
                update_tray_icon();
                set_profile_status_text("Selected slot %d for save/load actions.", slot);
            } else if (LOWORD(wParam) == PROFILE_LOAD_ID) {
                int slot = (int)SendMessageA(g_app.hProfileCombo, CB_GETCURSEL, 0, 0);
                if (slot < 0) slot = CONFIG_DEFAULT_SLOT - 1;
                slot += 1;
                if (!is_profile_slot_saved(g_app.configPath, slot)) {
                    set_profile_status_text("Slot %d is empty. Save a profile first.", slot);
                    break;
                }
                if (!maybe_confirm_profile_load_replace(slot)) break;
                DesiredSettings desired = {};
                char err[256] = {};
                if (!load_profile_from_config(g_app.configPath, slot, &desired, err, sizeof(err))) {
                    write_error_report_log_for_user_failure("Profile load failed", err);
                    MessageBoxA(g_app.hMainWnd, err, "Green Curve", MB_OK | MB_ICONERROR);
                    break;
                }
                populate_desired_into_gui(&desired);
                set_gui_state_dirty(true);
                populate_global_controls();
                set_config_int(g_app.configPath, "profiles", "selected_slot", slot);
                refresh_profile_controls_from_config();
                set_profile_status_text("Loaded slot %d into the GUI. GPU settings were not applied.", slot);
                invalidate_main_window();
            } else if (LOWORD(wParam) == PROFILE_SAVE_ID) {
                int slot = (int)SendMessageA(g_app.hProfileCombo, CB_GETCURSEL, 0, 0);
                if (slot < 0) slot = CONFIG_DEFAULT_SLOT - 1;
                slot += 1;
                DesiredSettings desired = {};
                char err[256] = {};
                if (!refresh_service_snapshot_and_active_desired(err, sizeof(err))) {
                    write_error_report_log_for_user_failure("Profile save refresh failed", err);
                    MessageBoxA(g_app.hMainWnd, err, "Green Curve", MB_OK | MB_ICONERROR);
                    break;
                }
                if (!capture_gui_config_settings(&desired, err, sizeof(err))) {
                    write_error_report_log_for_user_failure("Profile save capture failed", err);
                    MessageBoxA(g_app.hMainWnd, err, "Green Curve", MB_OK | MB_ICONERROR);
                    break;
                }
                if (!save_profile_to_config(g_app.configPath, slot, &desired, err, sizeof(err))) {
                    write_error_report_log_for_user_failure("Profile save failed", err);
                    MessageBoxA(g_app.hMainWnd, err, "Green Curve", MB_OK | MB_ICONERROR);
                    break;
                }
                populate_desired_into_gui(&desired);
                refresh_profile_controls_from_config();
                set_profile_status_text("Saved the current GUI values to slot %d.", slot);
                invalidate_main_window();
            } else if (LOWORD(wParam) == PROFILE_CLEAR_ID) {
                int slot = (int)SendMessageA(g_app.hProfileCombo, CB_GETCURSEL, 0, 0);
                if (slot < 0) slot = CONFIG_DEFAULT_SLOT - 1;
                slot += 1;
                if (!is_profile_slot_saved(g_app.configPath, slot)) {
                    set_profile_status_text("Slot %d is already empty.", slot);
                    break;
                }
                char confirm[192];
                StringCchPrintfA(confirm, ARRAY_COUNT(confirm),
                    "Clear profile %d? Any app start or logon assignment for this slot will also be disabled.", slot);
                if (MessageBoxA(g_app.hMainWnd, confirm, "Green Curve", MB_YESNO | MB_ICONQUESTION) != IDYES) break;
                char err[256] = {};
                if (!clear_profile_from_config(g_app.configPath, slot, err, sizeof(err))) {
                    write_error_report_log_for_user_failure("Profile clear failed", err);
                    MessageBoxA(g_app.hMainWnd, err, "Green Curve", MB_OK | MB_ICONERROR);
                    break;
                }
                bool taskOk = true;
                taskOk = set_startup_task_enabled(should_enable_startup_task_from_config(g_app.configPath), err, sizeof(err));
                if (!taskOk) {
                    write_error_report_log_for_user_failure("Startup task update failed after profile clear", err[0] ? err : "Unknown error");
                    MessageBoxA(g_app.hMainWnd, err[0] ? err : "Failed to update startup task after profile clear", "Green Curve", MB_OK | MB_ICONWARNING);
                }
                refresh_profile_controls_from_config();
                update_background_service_controls();
                set_profile_status_text("Cleared slot %d and disabled any auto-use for it.", slot);
                invalidate_main_window();
            } else if (LOWORD(wParam) == APP_LAUNCH_COMBO_ID || LOWORD(wParam) == LOGON_COMBO_ID) {
                if (HIWORD(wParam) != CBN_SELCHANGE) break;
                HWND hCombo = g_app.hAppLaunchCombo;
                const char* key = "app_launch_slot";
                if (LOWORD(wParam) == LOGON_COMBO_ID) {
                    hCombo = g_app.hLogonCombo;
                    key = "logon_slot";
                }
                int sel = (int)SendMessageA(hCombo, CB_GETCURSEL, 0, 0);
                int slot = (sel < 0) ? 0 : sel;  // index 0 = Disabled (slot 0)
                if (slot > 0 && !is_profile_slot_saved(g_app.configPath, slot)) {
                    MessageBoxA(g_app.hMainWnd,
                        "That slot is empty. Save a profile there before using it for automatic actions.",
                        "Green Curve", MB_OK | MB_ICONINFORMATION);
                    refresh_profile_controls_from_config();
                    break;
                }
                if (LOWORD(wParam) == LOGON_COMBO_ID) {
                    char err[256] = {};
                    int previousSlot = get_config_int(g_app.configPath, "profiles", key, 0);
                    if (previousSlot < 0 || previousSlot > CONFIG_NUM_SLOTS) previousSlot = 0;
                    bool ok = false;
                    set_config_int(g_app.configPath, "profiles", key, slot);
                    ok = set_startup_task_enabled(should_enable_startup_task_from_config(g_app.configPath), err, sizeof(err));
                    if (!ok) {
                        set_config_int(g_app.configPath, "profiles", key, previousSlot);
                        write_error_report_log_for_user_failure("Logon startup task update failed", err);
                        MessageBoxA(g_app.hMainWnd, err, "Green Curve", MB_OK | MB_ICONERROR);
                        refresh_profile_controls_from_config();
                        break;
                    }
                    bool startProgramAtLogon = is_start_on_logon_enabled(g_app.configPath);
                    set_profile_status_text(slot > 0
                        ? (startProgramAtLogon
                            ? "At Windows logon, slot %d will be applied and Green Curve will start hidden in the tray."
                            : "At Windows logon, slot %d will be applied silently without showing the tray icon.")
                        : "Windows logon auto-apply disabled.", slot);
                } else {
                    set_config_int(g_app.configPath, "profiles", key, slot);
                    set_profile_status_text(slot > 0
                        ? "At app start, slot %d will load into the GUI and apply automatically."
                        : "App start auto-load disabled.", slot);
                }
                refresh_profile_controls_from_config();
                update_background_service_controls();
                invalidate_main_window();
            } else if (LOWORD(wParam) == TRAY_MENU_SHOW_ID) {
                show_main_window_from_tray();
            } else if (LOWORD(wParam) == TRAY_MENU_EXIT_ID) {
                DestroyWindow(hwnd);
            } else if (LOWORD(wParam) >= LOCK_BASE_ID && LOWORD(wParam) < LOCK_BASE_ID + VF_NUM_POINTS) {
                // Lock checkbox clicked
                int vi = LOWORD(wParam) - LOCK_BASE_ID;
                if (vi == g_app.lockedVi) {
                    SendMessageA(g_app.hLocks[vi], BM_SETCHECK, BST_UNCHECKED, 0);
                    InvalidateRect(g_app.hLocks[vi], nullptr, FALSE);
                    if (g_app.lockedCi >= 0) record_ui_action("unlock point %d", g_app.lockedCi);
                    unlock_all();
                    set_gui_state_dirty(true);
                } else {
                    SendMessageA(g_app.hLocks[vi], BM_SETCHECK, BST_CHECKED, 0);
                    InvalidateRect(g_app.hLocks[vi], nullptr, FALSE);
                    apply_lock(vi);
                }
            } else if (LOWORD(wParam) == LICENSE_BTN_ID) {
                show_license_dialog(g_app.hMainWnd);
            }
            return 0;

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            if (mmi) {
                SIZE minSize = main_window_min_size();
                mmi->ptMinTrackSize.x = minSize.cx;
                mmi->ptMinTrackSize.y = minSize.cy;
            }
            return 0;
        }

        case WM_DESTROY:
            KillTimer(hwnd, FAN_TELEMETRY_TIMER_ID);
            if (!g_app.usingBackgroundService || g_app.isServiceProcess) {
                stop_fan_curve_runtime(true);
            }
            if (g_debug_logging) {
                ULONGLONG elapsedMs = g_debugSessionStartTickMs ? (GetTickCount64() - g_debugSessionStartTickMs) : 0;
                char extra[160] = {};
                StringCchPrintfA(extra, ARRAY_COUNT(extra), "gui shutdown uptimeMs=%llu", elapsedMs);
                debug_log_session_marker("END", "gui", extra);
            }
            remove_tray_icon();
            close_startup_sync_thread_handle();
            destroy_backbuffer();
            if (g_app.hCachedGridPen) { DeleteObject(g_app.hCachedGridPen); g_app.hCachedGridPen = nullptr; }
            if (g_app.hCachedAxisPen) { DeleteObject(g_app.hCachedAxisPen); g_app.hCachedAxisPen = nullptr; }
            if (g_app.hCachedFont) { DeleteObject(g_app.hCachedFont); g_app.hCachedFont = nullptr; }
            if (g_app.hCachedFontSmall) { DeleteObject(g_app.hCachedFontSmall); g_app.hCachedFontSmall = nullptr; }
            if (g_hBtnBr) { DeleteObject(g_hBtnBr); g_hBtnBr = nullptr; }
            if (g_hInputBr) { DeleteObject(g_hInputBr); g_hInputBr = nullptr; }
            if (g_hStaticBr) { DeleteObject(g_hStaticBr); g_hStaticBr = nullptr; }
            if (g_hListBr) { DeleteObject(g_hListBr); g_hListBr = nullptr; }
            if (g_hEditBr) { DeleteObject(g_hEditBr); g_hEditBr = nullptr; }
            shutdown_gdiplus();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

