// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

static void destroy_edit_controls(HWND hParent) {
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
            && id != SHARE_ALL_USERS_CHECK_ID && id != SHARED_PROFILES_BTN_ID
            && id != AUTO_PROFILE_BTN_ID) {
            DestroyWindow(child);
        }
        child = next;
    }
    main_layout_forget_dynamic_controls();
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
    g_app.hGpuSelectCombo = nullptr;
}

static void rebuild_edit_controls() {
    HWND hwnd = g_app.hMainWnd;
    if (!hwnd) return;
    debug_log("rebuild_edit_controls: redraw-suppressed rebuild (numVisible=%d loaded=%d)\n",
        g_app.numVisible, g_app.loaded ? 1 : 0);
    main_layout_grow_window_for_content(
        hwnd, g_app.numVisible, "VF edit-control rebuild");
    SendMessageA(hwnd, WM_SETREDRAW, FALSE, 0);
    destroy_edit_controls(hwnd);
    create_edit_controls(hwnd, g_app.hInst);
    SendMessageA(hwnd, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(hwnd, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_FRAME);
}
