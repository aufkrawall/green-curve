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
        bool fanOnlyApply = desired_is_fan_only_apply_request(&desired);
        set_gui_state_dirty(false);
        if (fanOnlyApply) {
            debug_log("GUI apply: preserving VF editor intent after fan-only apply\n");
        } else {
            populate_desired_into_gui(&desired);
        }
        populate_global_controls();
        invalidate_main_window();
    }
    boost_fan_telemetry_for_ms(3000);
    refresh_live_fan_telemetry(true);
    MessageBoxA(g_app.hMainWnd, result, "Green Curve", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING));
    g_app.applyInFlight = false;
    if (g_app.hApplyBtn) EnableWindow(g_app.hApplyBtn, TRUE);
}

static void destroy_edit_controls(HWND hParent) {
    // The lock tooltip subclasses the lock checkboxes; tear it down before the
    // checkboxes are destroyed so it is rebuilt cleanly with the new controls.
    if (g_app.hLockTooltip) {
        DestroyWindow(g_app.hLockTooltip);
        g_app.hLockTooltip = nullptr;
    }
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
            && id != LOGON_HINT_ID
            && id != SHARE_ALL_USERS_CHECK_ID && id != SHARED_PROFILES_BTN_ID) {
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

// Rebuild the per-point edit controls with painting suppressed. The bulk
// destroy/recreate of 40+ child windows during a service-state transition
// (service start/restart, reset) otherwise flashes partially-built or stale
// content because each CreateWindow/DestroyWindow triggers its own paints.
// WM_SETREDRAW gates painting off for the whole batch; one full redraw at the
// end paints the final layout cleanly (children + frame).
static void rebuild_edit_controls() {
    HWND hwnd = g_app.hMainWnd;
    if (!hwnd) return;
    debug_log("rebuild_edit_controls: redraw-suppressed rebuild (numVisible=%d loaded=%d)\n",
        g_app.numVisible, g_app.loaded ? 1 : 0);
    SendMessageA(hwnd, WM_SETREDRAW, FALSE, 0);
    destroy_edit_controls(hwnd);
    create_edit_controls(hwnd, g_app.hInst);
    SendMessageA(hwnd, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(hwnd, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_FRAME);
}

static void refresh_curve() {
    if (!g_app.backgroundServiceAvailable) {
        refresh_background_service_state();
        start_service_reconnect_timer_if_needed();
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
                rebuild_edit_controls();
            } else {
                update_all_gui_for_service_state();
            }
            update_background_service_controls();
            ensure_main_window_min_size(g_app.hMainWnd);
            invalidate_main_window();
        } else {
            refresh_background_service_state();
            start_service_reconnect_timer_if_needed();
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
                g_app.lockMode = LOCK_MODE_NONE;
                set_gui_state_dirty(false);
            }
            rebuild_edit_controls();
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

// Resolve a lock checkbox HWND back to its visible-point index, or -1.
static int lock_index_from_hwnd(HWND h) {
    if (!h) return -1;
    for (int vi = 0; vi < g_app.numVisible; vi++) {
        if (g_app.hLocks[vi] == h) return vi;
    }
    return -1;
}

// Right-click on a lock checkbox: choose the lock mode directly instead of
// cycling through it with repeated left clicks. This also lets the user switch
// HARD<->FLATTEN without first clearing the lock (which the left-click cycle
// forces via NONE).
static void show_lock_context_menu(HWND hwnd, int vi, POINT screenPt) {
    if (vi < 0 || vi >= g_app.numVisible) return;
    if (!g_app.hLocks[vi] || !IsWindowEnabled(g_app.hLocks[vi])) return;

    LockMode current = (vi == g_app.lockedVi) ? g_app.lockMode : LOCK_MODE_NONE;

    refresh_menu_theme_cache();
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuA(menu, MF_STRING, LOCK_CTX_NONE_ID, "No lock");
    AppendMenuA(menu, MF_STRING, LOCK_CTX_FLATTEN_ID, "Flatten (cap tail)");
    AppendMenuA(menu, MF_STRING, LOCK_CTX_PIN_ID, "Pin (hard lock)");
    UINT currentId = current == LOCK_MODE_HARD ? LOCK_CTX_PIN_ID
                   : current == LOCK_MODE_FLATTEN ? LOCK_CTX_FLATTEN_ID
                   : LOCK_CTX_NONE_ID;
    CheckMenuRadioItem(menu, LOCK_CTX_NONE_ID, LOCK_CTX_PIN_ID, currentId, MF_BYCOMMAND);

    SetForegroundWindow(hwnd);
    int cmd = (int)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, screenPt.x, screenPt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd == 0) return;

    int ci = g_app.visibleMap[vi];
    if (cmd == LOCK_CTX_NONE_ID) {
        if (vi == g_app.lockedVi) {
            record_ui_action("unlock point %d via menu (was %s)", ci, lock_mode_name(g_app.lockMode));
            unlock_all();
            set_gui_state_dirty(true);
            invalidate_main_window();
        }
    } else if (cmd == LOCK_CTX_FLATTEN_ID || cmd == LOCK_CTX_PIN_ID) {
        LockMode target = (cmd == LOCK_CTX_PIN_ID) ? LOCK_MODE_HARD : LOCK_MODE_FLATTEN;
        if (vi == g_app.lockedVi) {
            if (g_app.lockMode != target) {
                g_app.lockMode = target;
                InvalidateRect(g_app.hLocks[vi], nullptr, FALSE);
                set_gui_state_dirty(true);
                record_ui_action("%s lock point %d @ %u MHz via menu",
                    target == LOCK_MODE_HARD ? "hard" : "flatten", ci, g_app.lockedFreq);
                invalidate_main_window();
            }
        } else {
            apply_lock(vi, target);
            record_ui_action("%s lock point %d @ %u MHz via menu",
                target == LOCK_MODE_HARD ? "hard" : "flatten", ci, g_app.lockedFreq);
            invalidate_main_window();
        }
    }
}

// Run a short-lived elevated copy of the current executable with `args` and wait
// for it to finish. Shows its own error/cancel messages. Returns true only if
// the elevated process was launched and exited with code 0.
static bool run_elevated_command(const char* args, const char* cancelledStatus, const char* failedPrefix) {
    char exePath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exePath, ARRAY_COUNT(exePath)) == 0) {
        MessageBoxA(g_app.hMainWnd, "Unable to locate the Green Curve executable.",
            "Green Curve", MB_OK | MB_ICONERROR);
        return false;
    }

    SHELLEXECUTEINFOA sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NO_CONSOLE | SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = g_app.hMainWnd;
    sei.lpVerb = "runas";
    sei.lpFile = exePath;
    sei.lpParameters = args;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExA(&sei)) {
        DWORD err = GetLastError();
        if (err == ERROR_CANCELLED) {
            set_profile_status_text("%s", cancelledStatus);
        } else {
            char errMsg[256] = {};
            StringCchPrintfA(errMsg, ARRAY_COUNT(errMsg), "Failed to request administrator rights (error %lu).", err);
            MessageBoxA(g_app.hMainWnd, errMsg, "Green Curve", MB_OK | MB_ICONERROR);
        }
        return false;
    }
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, INFINITE);
        DWORD exitCode = 0;
        if (GetExitCodeProcess(sei.hProcess, &exitCode) && exitCode != 0) {
            char errMsg[256] = {};
            StringCchPrintfA(errMsg, ARRAY_COUNT(errMsg),
                "%s failed with exit code %lu.\n\nCheck greencurve_cli_log.txt for details.", failedPrefix, exitCode);
            MessageBoxA(g_app.hMainWnd, errMsg, "Green Curve", MB_OK | MB_ICONERROR);
            CloseHandle(sei.hProcess);
            return false;
        }
        CloseHandle(sei.hProcess);
    }
    return true;
}

// Right-click on the "All users" machine-logon button: manage the machine-wide
// profile bank (publish the current slot, clear a machine slot).
static void show_machine_logon_context_menu(HWND hwnd, POINT screenPt) {
    refresh_menu_theme_cache();
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    int selectedSlot = (int)SendMessageA(g_app.hProfileCombo, CB_GETCURSEL, 0, 0);
    if (selectedSlot < 0 || selectedSlot > CONFIG_NUM_SLOTS - 1) selectedSlot = CONFIG_DEFAULT_SLOT - 1;
    selectedSlot += 1;

    char publishText[64] = {};
    StringCchPrintfA(publishText, ARRAY_COUNT(publishText),
        "Publish slot %d to all users", selectedSlot);
    AppendMenuA(menu, MF_STRING, MACHINE_LOGON_MENU_PUBLISH_ID, publishText);

    char clearText[64] = {};
    StringCchPrintfA(clearText, ARRAY_COUNT(clearText),
        "Clear machine-wide slot %d", selectedSlot);
    AppendMenuA(menu, MF_STRING, MACHINE_LOGON_MENU_CLEAR_MACHINE_SLOT_ID, clearText);

    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
    bool restrictOn = false;
    get_machine_restrict_policy(&restrictOn);
    AppendMenuA(menu, MF_STRING | (restrictOn ? MF_CHECKED : MF_UNCHECKED),
        MACHINE_LOGON_MENU_RESTRICT_ID, "Restrict standard users to shared profiles");

    SetForegroundWindow(hwnd);
    int cmd = (int)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, screenPt.x, screenPt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd == 0) return;

    if (cmd == MACHINE_LOGON_MENU_PUBLISH_ID) {
        bool ok = false;
        if (!is_elevated()) {
            char args[MAX_PATH * 2] = {};
            // --config is REQUIRED here: the elevated helper reads the admin's
            // profile to publish from g_app.configPath. Without it the helper
            // resolves its own (wrong) config path and publishes an empty/stale
            // profile, or creates a stray config.ini beside the binary.
            StringCchPrintfA(args, ARRAY_COUNT(args),
                "--publish-slot-to-machine %d --config \"%s\"", selectedSlot, g_app.configPath);
            ok = run_elevated_command(args,
                "Administrator consent was cancelled; profile was not published.",
                "Publish profile to machine-wide bank");
        } else {
            char err[256] = {};
            ok = copy_profile_slot_to_machine_config(g_app.configPath, selectedSlot, err, sizeof(err));
            if (!ok) {
                write_error_report_log_for_user_failure("Publish to machine profile bank failed", err[0] ? err : "Unknown error");
                MessageBoxA(g_app.hMainWnd, err[0] ? err : "Failed to publish profile to machine-wide bank.",
                    "Green Curve", MB_OK | MB_ICONERROR);
            }
        }
        if (ok) set_profile_status_text("Slot %d published to the shared bank (default unchanged).", selectedSlot);
        refresh_profile_controls_from_config();
    } else if (cmd == MACHINE_LOGON_MENU_CLEAR_MACHINE_SLOT_ID) {
        bool ok = false;
        if (!is_elevated()) {
            char args[MAX_PATH * 2] = {};
            StringCchPrintfA(args, ARRAY_COUNT(args),
                "--clear-machine-slot %d --config \"%s\"", selectedSlot, g_app.configPath);
            ok = run_elevated_command(args,
                "Administrator consent was cancelled; machine-wide profile slot was not cleared.",
                "Clear machine-wide profile slot");
        } else {
            char err[256] = {};
            ok = clear_machine_profile_slot(selectedSlot, err, sizeof(err));
            if (!ok) {
                write_error_report_log_for_user_failure("Clear machine profile slot failed", err[0] ? err : "Unknown error");
                MessageBoxA(g_app.hMainWnd, err[0] ? err : "Failed to clear machine-wide profile slot.",
                    "Green Curve", MB_OK | MB_ICONERROR);
            }
        }
        if (ok) set_profile_status_text("Cleared machine-wide profile slot %d.", selectedSlot);
        refresh_profile_controls_from_config();
    } else if (cmd == MACHINE_LOGON_MENU_RESTRICT_ID) {
        bool enable = !restrictOn;
        bool ok = false;
        if (!is_elevated()) {
            char args[MAX_PATH * 2] = {};
            StringCchPrintfA(args, ARRAY_COUNT(args),
                "--set-restrict-shared %d --config \"%s\"", enable ? 1 : 0, g_app.configPath);
            ok = run_elevated_command(args,
                "Administrator consent was cancelled; the shared-only policy was not changed.",
                "Shared-only policy update");
        } else {
            char err[256] = {};
            ok = set_machine_restrict_policy(enable, err, sizeof(err));
            if (!ok) {
                write_error_report_log_for_user_failure("Shared-only policy update failed", err[0] ? err : "Unknown error");
                MessageBoxA(g_app.hMainWnd, err[0] ? err : "Failed to update the shared-only policy.",
                    "Green Curve", MB_OK | MB_ICONERROR);
            }
        }
        if (ok) {
            set_profile_status_text(enable
                ? "Shared-only policy enabled: standard users may only apply shared profiles."
                : "Shared-only policy disabled.");
        }
        refresh_profile_controls_from_config();
    }
}

// Subclass the owner-draw "Share with all users" checkbox so right-clicking it
// opens the advanced shared-bank context menu (publish/clear individual slots
// without changing the all-users default).
static LRESULT CALLBACK share_all_users_subclass_proc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                                      UINT_PTR /*uIdSubclass*/, DWORD_PTR /*dwRefData*/) {
    if (uMsg == WM_RBUTTONUP) {
        POINT pt = { (LONG)(short)LOWORD(lParam), (LONG)(short)HIWORD(lParam) };
        ClientToScreen(hWnd, &pt);
        SendMessageA(GetParent(hWnd), WM_CONTEXTMENU, (WPARAM)hWnd, MAKELPARAM(pt.x, pt.y));
        return 0;
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// "Shared profiles" popup: list the admin-published profiles from the shared
// bank and load the chosen one into the editor (read-only).  Available to every
// user — reading the shared bank needs no elevation (Users:Read DACL).  The
// loaded profile is not applied automatically; the user clicks Apply to apply it
// via the service (the active session is permitted to apply).
static void show_shared_profiles_menu(HWND hwnd, POINT screenPt) {
    char machinePath[MAX_PATH] = {};
    if (!resolve_machine_config_path(machinePath, sizeof(machinePath))) {
        set_profile_status_text("Could not locate the shared profile store.");
        return;
    }
    refresh_menu_theme_cache();
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    int shared = 0;
    for (int s = 1; s <= CONFIG_NUM_SLOTS; s++) {
        if (!is_machine_profile_slot_saved(s)) continue;
        char text[64] = {};
        StringCchPrintfA(text, ARRAY_COUNT(text), "Load shared profile %d (read-only)", s);
        AppendMenuA(menu, MF_STRING, SHARED_PROFILE_MENU_BASE + s, text);
        shared++;
    }
    if (shared == 0) {
        AppendMenuA(menu, MF_STRING | MF_GRAYED, 0, "No profiles shared by an administrator");
    } else {
        // To auto-apply a shared profile at logon, the user picks it in the unified
        // "Apply profile after user log in:" dropdown (one always-visible control
        // for the per-account logon choice).  Point them there from here.
        AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuA(menu, MF_STRING | MF_GRAYED, 0,
            "To apply one at logon, use the \"Apply profile after user log in\" list");
    }

    SetForegroundWindow(hwnd);
    int cmd = (int)TrackPopupMenu(menu, TPM_LEFTBUTTON | TPM_RETURNCMD, screenPt.x, screenPt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);

    if (cmd < SHARED_PROFILE_MENU_BASE + 1 || cmd > SHARED_PROFILE_MENU_BASE + CONFIG_NUM_SLOTS) return;

    int slot = cmd - SHARED_PROFILE_MENU_BASE;
    DesiredSettings desired = {};
    char err[256] = {};
    if (!load_profile_from_config(machinePath, slot, &desired, err, sizeof(err))) {
        write_error_report_log_for_user_failure("Shared profile load failed", err[0] ? err : "Unknown error");
        MessageBoxA(g_app.hMainWnd, err[0] ? err : "Failed to load the shared profile.",
            "Green Curve", MB_OK | MB_ICONERROR);
        return;
    }
    // Load into the editor and mark dirty so Apply is enabled.  We intentionally
    // do NOT touch the user's selected_slot or per-user config — this is the
    // admin's read-only profile loaded on demand.
    populate_desired_into_gui(&desired);
    // Mark the editor as holding this admin shared slot (cleared by populate_*
    // above, set here AFTER). A clean Apply sends it as an authoritative
    // "apply shared slot N" so it works under the shared-only policy.
    g_app.loadedSharedSlot = slot;
    set_gui_state_dirty(true);
    populate_global_controls();
    set_profile_status_text(
        "Loaded shared profile %d into the editor. Click Apply to apply it; use Save to copy it into one of your own slots.", slot);
    invalidate_main_window();
    debug_log("shared profiles: loaded shared slot %d from %s into editor\n", slot, machinePath);
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
            start_service_reconnect_timer_if_needed();
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
            if (wParam == SERVICE_RECONNECT_TIMER_ID) {
                // Auto-reconnect: periodic ping when service is unavailable
                if (!g_app.backgroundServiceAvailable) {
                    debug_log("reconnect timer: attempting service ping\n");
                    if (refresh_background_service_state()) {
                        // Service is back! Update GUI with latest state.
                        update_all_gui_for_service_state();
                        refresh_curve();
                        // Only stop the timer once the GUI has fully loaded
                        // curve data from the service.  If recovery is still
                        // in progress the snapshot has loaded=false — keep
                        // polling until it succeeds.
                        if (g_app.loaded) {
                            KillTimer(hwnd, SERVICE_RECONNECT_TIMER_ID);
                            debug_log("reconnect timer: GPU snapshot loaded, stopping timer\n");
                        } else {
                            debug_log("reconnect timer: snapshot loaded=false, keeping timer alive\n");
                        }
                    }
                } else {
                    KillTimer(hwnd, SERVICE_RECONNECT_TIMER_ID);
                }
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
                            g_app.guiHasUserModifiedValues = true;
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
                g_app.guiHasUserModifiedValues = true;
                set_gui_state_dirty(true);
                if (parse_int_strict(buf, &value)) record_ui_action("GPU offset edited to %d MHz", value);
            }
            if (HIWORD(wParam) == EN_CHANGE && LOWORD(wParam) == MEM_OFFSET_ID && !programmatic_edit_update_active()) {
                char buf[32] = {};
                get_window_text_safe(g_app.hMemOffsetEdit, buf, sizeof(buf));
                int value = 0;
                g_app.guiHasUserModifiedValues = true;
                set_gui_state_dirty(true);
                if (parse_int_strict(buf, &value)) record_ui_action("Mem offset edited to %d MHz", value);
            }
            if (HIWORD(wParam) == EN_CHANGE && LOWORD(wParam) == POWER_LIMIT_ID && !programmatic_edit_update_active()) {
                char buf[32] = {};
                get_window_text_safe(g_app.hPowerLimitEdit, buf, sizeof(buf));
                int value = 0;
                g_app.guiHasUserModifiedValues = true;
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
                    g_app.guiHasUserModifiedValues = true;
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
                    g_app.guiHasUserModifiedValues = true;
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
            } else if (LOWORD(wParam) == SHARE_ALL_USERS_CHECK_ID && HIWORD(wParam) == BN_CLICKED) {
                // Toggle "share with all users" for the SELECTED profile slot.
                // Sharing publishes the slot's data into the shared bank AND
                // makes it the all-users default logon profile (one action);
                // unsharing reverses both.
                int sel = g_app.hProfileCombo ? (int)SendMessageA(g_app.hProfileCombo, CB_GETCURSEL, 0, 0) : -1;
                if (sel < 0 || sel > CONFIG_NUM_SLOTS - 1) sel = CONFIG_DEFAULT_SLOT - 1;
                int slot = sel + 1;
                bool currentlyShared = is_machine_profile_slot_saved(slot) && g_app.machineLogonSlotCache == slot;

                // Sharing requires the slot to actually hold a saved profile.
                if (!currentlyShared && !is_profile_slot_saved(g_app.configPath, slot)) {
                    MessageBoxA(g_app.hMainWnd,
                        "The selected profile slot is empty. Save a profile into this slot before sharing it with all users.",
                        "Green Curve", MB_OK | MB_ICONINFORMATION);
                    update_share_all_users_check_state();
                    break;
                }

                bool ok = false;
                if (!is_elevated()) {
                    char args[MAX_PATH * 2] = {};
                    StringCchPrintfA(args, ARRAY_COUNT(args),
                        currentlyShared ? "--unshare-slot %d --config \"%s\"" : "--share-slot %d --config \"%s\"",
                        slot, g_app.configPath);
                    ok = run_elevated_command(args,
                        currentlyShared
                            ? "Administrator consent was cancelled; profile is still shared."
                            : "Administrator consent was cancelled; profile was not shared.",
                        currentlyShared ? "Stop sharing profile with all users" : "Share profile with all users");
                } else {
                    char err[256] = {};
                    ok = currentlyShared
                        ? unshare_profile_slot_for_all_users(slot, err, sizeof(err))
                        : share_profile_slot_for_all_users(g_app.configPath, slot, err, sizeof(err));
                    if (!ok) {
                        write_error_report_log_for_user_failure("Share-with-all-users update failed", err[0] ? err : "Unknown error");
                        MessageBoxA(g_app.hMainWnd, err[0] ? err : "Failed to update the shared profile.",
                            "Green Curve", MB_OK | MB_ICONERROR);
                    }
                }
                if (ok) {
                    if (currentlyShared) {
                        set_profile_status_text("Slot %d is no longer shared with all users.", slot);
                    } else {
                        set_profile_status_text(
                            "Slot %d is now shared with all users and applied on logon for users without their own profile.", slot);
                    }
                }
                update_share_all_users_check_state();
                refresh_profile_controls_from_config();
            } else if (LOWORD(wParam) == SHARED_PROFILES_BTN_ID && HIWORD(wParam) == BN_CLICKED) {
                // Any user: open the list of admin-published shared profiles.
                RECT rc = {};
                GetWindowRect(g_app.hSharedProfilesBtn, &rc);
                POINT pt = { rc.left, rc.bottom };
                show_shared_profiles_menu(hwnd, pt);
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
                if (g_app.guiHasUserModifiedValues || gui_has_pending_curve_or_lock_edits()) {
                    if (!capture_gui_config_settings(&desired, err, sizeof(err))) {
                        write_error_report_log_for_user_failure("Profile save capture failed", err);
                        MessageBoxA(g_app.hMainWnd, err, "Green Curve", MB_OK | MB_ICONERROR);
                        break;
                    }
                    debug_log("PROFILE_SAVE: saving sparse GUI curve intent (modified=%d curveOrLock=%d)\n",
                        g_app.guiHasUserModifiedValues ? 1 : 0,
                        gui_has_pending_curve_or_lock_edits() ? 1 : 0);
                } else {
                    build_full_live_desired_settings(&desired);
                    debug_log("PROFILE_SAVE: no user edits, saving live state\n");
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
                if (LOWORD(wParam) == LOGON_COMBO_ID) {
                    // Unified per-account logon selector: decode the selected item's
                    // CB_SETITEMDATA tag (see refresh_profile_controls_from_config):
                    // 0 = no personal choice, 1..N = per-user logon_slot,
                    // LOGON_COMBO_SHARED_FLAG|N = admin shared bank slot N.
                    int sel = (int)SendMessageA(g_app.hLogonCombo, CB_GETCURSEL, 0, 0);
                    LRESULT itemData = (sel < 0) ? 0 : SendMessageA(g_app.hLogonCombo, CB_GETITEMDATA, (WPARAM)sel, 0);
                    if (itemData == CB_ERR || itemData < 0) itemData = 0;
                    int perUserSlot = (itemData & LOGON_COMBO_SHARED_FLAG) ? 0 : (int)itemData;
                    int sharedSlot = (itemData & LOGON_COMBO_SHARED_FLAG) ? (int)(itemData & 0xFF) : 0;

                    if (perUserSlot > 0 && !is_profile_slot_saved(g_app.configPath, perUserSlot)) {
                        MessageBoxA(g_app.hMainWnd,
                            "That slot is empty. Save a profile there before using it for automatic actions.",
                            "Green Curve", MB_OK | MB_ICONINFORMATION);
                        refresh_profile_controls_from_config();
                        break;
                    }
                    if (sharedSlot > 0 && !is_machine_profile_slot_saved(sharedSlot)) {
                        set_profile_status_text("Shared profile %d is no longer available.", sharedSlot);
                        refresh_profile_controls_from_config();
                        break;
                    }

                    // logon_slot and logon_shared_slot are mutually exclusive for this
                    // account; set both (one to 0) and roll back together on failure.
                    char err[256] = {};
                    int prevLogon = get_config_int(g_app.configPath, "profiles", "logon_slot", 0);
                    int prevShared = get_config_int(g_app.configPath, "profiles", "logon_shared_slot", 0);
                    set_config_int(g_app.configPath, "profiles", "logon_slot", perUserSlot);
                    set_config_int(g_app.configPath, "profiles", "logon_shared_slot", sharedSlot);
                    if (!set_startup_task_enabled(should_enable_startup_task_from_config(g_app.configPath), err, sizeof(err))) {
                        set_config_int(g_app.configPath, "profiles", "logon_slot", prevLogon);
                        set_config_int(g_app.configPath, "profiles", "logon_shared_slot", prevShared);
                        write_error_report_log_for_user_failure("Logon startup task update failed", err);
                        MessageBoxA(g_app.hMainWnd, err, "Green Curve", MB_OK | MB_ICONERROR);
                        refresh_profile_controls_from_config();
                        break;
                    }
                    bool startProgramAtLogon = is_start_on_logon_enabled(g_app.configPath);
                    if (sharedSlot > 0) {
                        set_profile_status_text(
                            "Shared profile %d (admin) will apply at your logon%s - this overrides the all-users default.",
                            sharedSlot, startProgramAtLogon ? " and Green Curve will start hidden in the tray" : "");
                    } else if (perUserSlot > 0) {
                        set_profile_status_text(startProgramAtLogon
                            ? "At Windows logon, slot %d will be applied and Green Curve will start hidden in the tray."
                            : "At Windows logon, slot %d will be applied silently without showing the tray icon.", perUserSlot);
                    } else {
                        int machineDefault = g_app.machineLogonSlotCache;
                        if (machineDefault > 0 && is_machine_profile_slot_saved(machineDefault)) {
                            set_profile_status_text(
                                "No personal logon profile for this account - the admin all-users default (Shared profile %d) applies.",
                                machineDefault);
                        } else {
                            set_profile_status_text("Windows logon auto-apply disabled for this account.");
                        }
                    }
                    update_share_all_users_check_state();
                } else {
                    // App-launch combo: index 0 = Disabled, 1..N = per-user slot.
                    int sel = (int)SendMessageA(g_app.hAppLaunchCombo, CB_GETCURSEL, 0, 0);
                    int slot = (sel < 0) ? 0 : sel;
                    if (slot > 0 && !is_profile_slot_saved(g_app.configPath, slot)) {
                        MessageBoxA(g_app.hMainWnd,
                            "That slot is empty. Save a profile there before using it for automatic actions.",
                            "Green Curve", MB_OK | MB_ICONINFORMATION);
                        refresh_profile_controls_from_config();
                        break;
                    }
                    set_config_int(g_app.configPath, "profiles", "app_launch_slot", slot);
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
                // Lock checkbox clicked — tri-state cycle: NONE → FLATTEN → HARD → NONE
                int vi = LOWORD(wParam) - LOCK_BASE_ID;
                if (vi == g_app.lockedVi) {
                    if (g_app.lockMode == LOCK_MODE_FLATTEN) {
                        // FLATTEN → HARD
                        g_app.lockMode = LOCK_MODE_HARD;
                        SendMessageA(g_app.hLocks[vi], BM_SETCHECK, BST_CHECKED, 0);
                        InvalidateRect(g_app.hLocks[vi], nullptr, FALSE);
                        if (g_app.lockedCi >= 0) record_ui_action("hard lock point %d @ %u MHz (pinned)", g_app.lockedCi, g_app.lockedFreq);
                        set_gui_state_dirty(true);
                    } else {
                        // HARD → NONE (or any other → NONE)
                        SendMessageA(g_app.hLocks[vi], BM_SETCHECK, BST_UNCHECKED, 0);
                        InvalidateRect(g_app.hLocks[vi], nullptr, FALSE);
                        if (g_app.lockedCi >= 0) record_ui_action("unlock point %d (was %s)", g_app.lockedCi, lock_mode_name(g_app.lockMode));
                        unlock_all();
                        set_gui_state_dirty(true);
                    }
                } else {
                    // New lock point — default to FLATTEN
                    SendMessageA(g_app.hLocks[vi], BM_SETCHECK, BST_CHECKED, 0);
                    InvalidateRect(g_app.hLocks[vi], nullptr, FALSE);
                    apply_lock(vi, LOCK_MODE_FLATTEN);
                }
            } else if (LOWORD(wParam) == LICENSE_BTN_ID) {
                show_license_dialog(g_app.hMainWnd);
            }
            return 0;

        case WM_CONTEXTMENU: {
            // Owner-drawn lock checkboxes forward WM_CONTEXTMENU here with
            // wParam = the checkbox HWND. Right-click selects the lock mode.
            int vi = lock_index_from_hwnd((HWND)wParam);
            if (vi >= 0) {
                POINT pt = { (LONG)(short)LOWORD(lParam), (LONG)(short)HIWORD(lParam) };
                if (pt.x == -1 && pt.y == -1) {
                    // Keyboard-invoked (Shift+F10 / menu key): anchor on the control.
                    RECT rc = {};
                    GetWindowRect(g_app.hLocks[vi], &rc);
                    pt.x = rc.left;
                    pt.y = rc.bottom;
                }
                show_lock_context_menu(hwnd, vi, pt);
                return 0;
            }
            // The "Share with all users" checkbox forwards WM_CONTEXTMENU on
            // right-click so the admin can manage the shared profile bank
            // (publish/clear individual slots without changing the default).
            if ((HWND)wParam == g_app.hShareAllUsersCheck) {
                POINT pt = { (LONG)(short)LOWORD(lParam), (LONG)(short)HIWORD(lParam) };
                if (pt.x == -1 && pt.y == -1) {
                    RECT rc = {};
                    GetWindowRect(g_app.hShareAllUsersCheck, &rc);
                    pt.x = rc.left;
                    pt.y = rc.bottom;
                }
                show_machine_logon_context_menu(hwnd, pt);
                return 0;
            }
            break;
        }

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

