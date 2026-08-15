// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// The main window's right-click menus, plus the elevated helper one of them
// needs.  Split out of ui_main_window.cpp (F-MAINT-1) and included from the
// exact position it occupied, so the amalgamated ordering is unchanged.
//
// All three menus share a shape worth naming: each is raised from
// WM_CONTEXTMENU on a specific control, each is tracked against the main window
// (unlike the TRAY menu, which deliberately is not -- see gui_tray_menu.cpp for
// why that one needs a throwaway owner), and each ends by posting the picked
// command back rather than acting inside the modal loop.

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
            // unlock_all() refreshes while still CLEAN; the dirty transition
            // after it captures a fresh draft, which the graph preview and the
            // Apply enable are both projections of.
            gui_pending_changes_refresh();
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
                // Same rule as the checkbox path: the dirty transition
                // re-snapshots GuiDraft, so F-PENDING has to be re-read before
                // anything paints from it.
                gui_pending_changes_refresh();
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

// Run a short-lived elevated copy of the current executable with argv-like
// arguments and wait for it to finish. Shows its own error/cancel messages.
// Returns true only if the elevated process was launched and exited with code 0.
static bool run_elevated_command(const char* const* argv, const char* cancelledStatus, const char* failedPrefix) {
    WCHAR exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, ARRAY_COUNT(exePath)) == 0) {
        gc_message_box(g_app.hMainWnd, "Unable to locate the Green Curve executable.",
            "Green Curve", MB_OK | MB_ICONERROR);
        return false;
    }
    WCHAR params[2048] = {};
    for (int i = 0; argv && argv[i]; i++) {
        WCHAR warg[MAX_PATH] = {};
        if (!utf8_to_wide(argv[i], warg, ARRAY_COUNT(warg)) ||
            !pl_append_quoted_arg_w(params, ARRAY_COUNT(params), warg)) {
            gc_message_box(g_app.hMainWnd, "Elevated helper command line is too long.",
                "Green Curve", MB_OK | MB_ICONERROR);
            return false;
        }
    }

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NO_CONSOLE | SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = g_app.hMainWnd;
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.lpParameters = params;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        DWORD err = GetLastError();
        if (err == ERROR_CANCELLED) {
            set_profile_status_text("%s", cancelledStatus);
        } else {
            char errMsg[256] = {};
            StringCchPrintfA(errMsg, ARRAY_COUNT(errMsg), "Failed to request administrator rights (error %lu).", err);
            gc_message_box(g_app.hMainWnd, errMsg, "Green Curve", MB_OK | MB_ICONERROR);
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
            gc_message_box(g_app.hMainWnd, errMsg, "Green Curve", MB_OK | MB_ICONERROR);
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
            // --config is REQUIRED here: the elevated helper reads the admin's
            // profile to publish from g_app.configPath. Without it the helper
            // resolves its own (wrong) config path and publishes an empty/stale
            // profile, or creates a stray config.ini beside the binary.
            char slotArg[16] = {};
            StringCchPrintfA(slotArg, ARRAY_COUNT(slotArg), "%d", selectedSlot);
            const char* argv[] = { "--publish-slot-to-machine", slotArg, "--config", g_app.configPath, nullptr };
            ok = run_elevated_command(argv,
                "Administrator consent was cancelled; profile was not published.",
                "Publish profile to machine-wide bank");
        } else {
            char err[256] = {};
            ok = copy_profile_slot_to_machine_config(g_app.configPath, selectedSlot, err, sizeof(err));
            if (!ok) {
                write_error_report_log_for_user_failure("Publish to machine profile bank failed", err[0] ? err : "Unknown error");
                gc_message_box(g_app.hMainWnd, err[0] ? err : "Failed to publish profile to machine-wide bank.",
                    "Green Curve", MB_OK | MB_ICONERROR);
            }
        }
        if (ok) set_profile_status_text("Slot %d published to the shared bank (default unchanged).", selectedSlot);
        refresh_profile_controls_from_config();
    } else if (cmd == MACHINE_LOGON_MENU_CLEAR_MACHINE_SLOT_ID) {
        bool ok = false;
        if (!is_elevated()) {
            char slotArg[16] = {};
            StringCchPrintfA(slotArg, ARRAY_COUNT(slotArg), "%d", selectedSlot);
            const char* argv[] = { "--clear-machine-slot", slotArg, "--config", g_app.configPath, nullptr };
            ok = run_elevated_command(argv,
                "Administrator consent was cancelled; machine-wide profile slot was not cleared.",
                "Clear machine-wide profile slot");
        } else {
            char err[256] = {};
            ok = clear_machine_profile_slot(selectedSlot, err, sizeof(err));
            if (!ok) {
                write_error_report_log_for_user_failure("Clear machine profile slot failed", err[0] ? err : "Unknown error");
                gc_message_box(g_app.hMainWnd, err[0] ? err : "Failed to clear machine-wide profile slot.",
                    "Green Curve", MB_OK | MB_ICONERROR);
            }
        }
        if (ok) set_profile_status_text("Cleared machine-wide profile slot %d.", selectedSlot);
        refresh_profile_controls_from_config();
    } else if (cmd == MACHINE_LOGON_MENU_RESTRICT_ID) {
        bool enable = !restrictOn;
        bool ok = false;
        if (!is_elevated()) {
            const char* enableArg = enable ? "1" : "0";
            const char* argv[] = { "--set-restrict-shared", enableArg, "--config", g_app.configPath, nullptr };
            ok = run_elevated_command(argv,
                "Administrator consent was cancelled; the shared-only policy was not changed.",
                "Shared-only policy update");
        } else {
            char err[256] = {};
            ok = set_machine_restrict_policy(enable, err, sizeof(err));
            if (!ok) {
                write_error_report_log_for_user_failure("Shared-only policy update failed", err[0] ? err : "Unknown error");
                gc_message_box(g_app.hMainWnd, err[0] ? err : "Failed to update the shared-only policy.",
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
        gc_message_box(g_app.hMainWnd, err[0] ? err : "Failed to load the shared profile.",
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
    gui_draft_capture_desired(&desired);
    populate_global_controls();
    set_profile_status_text(
        "Loaded shared profile %d into the editor. Click Apply to apply it; use Save to copy it into one of your own slots.", slot);
    invalidate_main_window();
    debug_log("shared profiles: loaded shared slot %d from %s into editor\n", slot, machinePath);
}
