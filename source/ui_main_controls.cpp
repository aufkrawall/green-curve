// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

static void create_edit_controls(HWND hParent, HINSTANCE hInst) {
    begin_programmatic_edit_update();
    int cbW = dp(16);
    int editW = dp(65);
    int labelW = dp(32);
    int gap = dp(2);
    int rowH = dp(20);
    int headerH = dp(16);
    int colW = labelW + cbW + editW + editW + gap * 3 + dp(8);
    int startY = 0;

    // Create the maximum header set once. The responsive layout shows only the
    // number of columns which actually contains points at the current width.
    for (int col = 0; col < MAIN_LAYOUT_MAX_COLUMNS; col++) {
        int x = dp(8) + col * colW;
        int y = startY - headerH - dp(2);
        HWND lockHeader = CreateWindowExA(0, "STATIC", "Lk",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            x + labelW + gap, y, cbW, headerH,
            hParent, nullptr, hInst, nullptr);
        HWND mhzHeader = CreateWindowExA(0, "STATIC", "MHz",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            x + labelW + cbW + gap * 2, y, editW, headerH,
            hParent, nullptr, hInst, nullptr);
        HWND mvHeader = CreateWindowExA(0, "STATIC", "mV",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            x + labelW + cbW + gap * 2 + editW + gap, y, editW, headerH,
            hParent, nullptr, hInst, nullptr);
        main_layout_register_point_header(col, 0, lockHeader);
        main_layout_register_point_header(col, 1, mhzHeader);
        main_layout_register_point_header(col, 2, mvHeader);
    }

    for (int vi = 0; vi < g_app.numVisible; vi++) {
        int ci = g_app.visibleMap[vi];
        int x = dp(8);
        int y = startY;
        char label[16];
        StringCchPrintfA(label, ARRAY_COUNT(label), "%3d", ci);
        HWND pointLabel = CreateWindowExA(0, "STATIC", label,
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            x, y + dp(1), labelW - gap, rowH - dp(2),
            hParent, nullptr, hInst, nullptr);
        main_layout_register_point_label(vi, pointLabel);

        g_app.hLocks[vi] = CreateWindowExA(
            0, "BUTTON", "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            x + labelW, y + dp(1), cbW, rowH - dp(2),
            hParent, (HMENU)(INT_PTR)(LOCK_BASE_ID + vi), hInst, nullptr);
        if (g_app.hLocks[vi]) {
            SetLastError(ERROR_SUCCESS);
            if (!SetWindowSubclass(g_app.hLocks[vi], lock_checkbox_subclass_proc, 0, 0)) {
                debug_log("lock checkbox subclass install FAILED: vi=%d hwnd=%p lastError=%lu (API may not set it); BN_CLICKED filtering remains active\n",
                          vi, (void*)g_app.hLocks[vi], (unsigned long)GetLastError());
            } else {
                debug_log("lock checkbox subclass installed: vi=%d hwnd=%p\n",
                          vi, (void*)g_app.hLocks[vi]);
            }
        }

        g_app.hEditsMhz[vi] = CreateWindowExA(
            0, "EDIT", "0",
            WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_RIGHT | ES_AUTOHSCROLL,
            x + labelW + cbW + gap * 2, y, editW, rowH - dp(2),
            hParent, (HMENU)(INT_PTR)(1000 + vi), hInst, nullptr);
        g_app.hEditsMv[vi] = CreateWindowExA(
            0, "EDIT", "0",
            WS_CHILD | WS_VISIBLE | ES_RIGHT | ES_AUTOHSCROLL | ES_READONLY,
            x + labelW + cbW + gap * 2 + editW + gap, y, editW, rowH - dp(2),
            hParent, (HMENU)(INT_PTR)(1000 + VF_NUM_POINTS + vi), hInst, nullptr);
        style_input_control(g_app.hEditsMhz[vi]);
        style_input_control(g_app.hEditsMv[vi]);
    }

    int gpuSelectW = dp(420);
    int gpuSelectX = dp(WINDOW_WIDTH) - gpuSelectW - dp(12);
    int gpuSelectY = dp(10);
    int gpuLabelW = dp(42);
    HWND gpuLabel = CreateWindowExA(0, "STATIC", "GPU:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        gpuSelectX - gpuLabelW - dp(6), gpuSelectY + dp(3), gpuLabelW, dp(18),
        hParent, nullptr, hInst, nullptr);
    main_layout_register_static(MAIN_STATIC_GPU_LABEL, gpuLabel);
    g_app.hGpuSelectCombo = CreateWindowExA(
        0, "COMBOBOX", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
        gpuSelectX, gpuSelectY, gpuSelectW, dp(220),
        hParent, (HMENU)(INT_PTR)GPU_SELECT_COMBO_ID, hInst, nullptr);
    style_combo_control(g_app.hGpuSelectCombo);

    int ocY = 0;
    int fieldW = dp(78);
    HWND gpuOffsetLabel = CreateWindowExA(0, "STATIC", "GPU Offset (MHz):",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        dp(8), ocY + dp(2), dp(126), dp(18), hParent, nullptr, hInst, nullptr);
    main_layout_register_static(MAIN_STATIC_GPU_OFFSET_LABEL, gpuOffsetLabel);
    g_app.hGpuOffsetEdit = CreateWindowExA(
        0, "EDIT", "0", WS_CHILD | WS_VISIBLE | ES_RIGHT | ES_AUTOHSCROLL,
        dp(136), ocY, fieldW, dp(20),
        hParent, (HMENU)(INT_PTR)GPU_OFFSET_ID, hInst, nullptr);
    style_input_control(g_app.hGpuOffsetEdit);
    g_app.hGpuOffsetExcludeLowLabel = CreateWindowExA(
        0, "STATIC", "Exclude first", WS_CHILD | WS_VISIBLE | SS_LEFT,
        dp(8), ocY + dp(25), dp(76), dp(18),
        hParent, (HMENU)(INT_PTR)GPU_OFFSET_EXCLUDE_LOW_LABEL_ID, hInst, nullptr);
    g_app.hGpuOffsetExcludeLowEdit = CreateWindowExA(
        0, "EDIT", "0", WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_RIGHT | ES_AUTOHSCROLL,
        dp(86), ocY + dp(23), dp(50), dp(20),
        hParent, (HMENU)(INT_PTR)GPU_OFFSET_EXCLUDE_LOW_EDIT_ID, hInst, nullptr);
    style_input_control(g_app.hGpuOffsetExcludeLowEdit);
    HWND excludeSuffix = CreateWindowExA(
        0, "STATIC", "VF points", WS_CHILD | WS_VISIBLE | SS_LEFT,
        dp(140), ocY + dp(25), dp(70), dp(18), hParent, nullptr, hInst, nullptr);
    main_layout_register_static(MAIN_STATIC_GPU_EXCLUDE_SUFFIX, excludeSuffix);

    HWND memOffsetLabel = CreateWindowExA(0, "STATIC", "Mem Offset (MHz):",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        dp(230), ocY + dp(2), dp(126), dp(18), hParent, nullptr, hInst, nullptr);
    main_layout_register_static(MAIN_STATIC_MEM_OFFSET_LABEL, memOffsetLabel);
    g_app.hMemOffsetEdit = CreateWindowExA(
        0, "EDIT", "0", WS_CHILD | WS_VISIBLE | ES_RIGHT | ES_AUTOHSCROLL,
        dp(358), ocY, fieldW, dp(20),
        hParent, (HMENU)(INT_PTR)MEM_OFFSET_ID, hInst, nullptr);
    style_input_control(g_app.hMemOffsetEdit);

    HWND powerLimitLabel = CreateWindowExA(0, "STATIC", "Power Limit (%):",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        dp(452), ocY + dp(2), dp(100), dp(18), hParent, nullptr, hInst, nullptr);
    main_layout_register_static(MAIN_STATIC_POWER_LIMIT_LABEL, powerLimitLabel);
    g_app.hPowerLimitEdit = CreateWindowExA(
        0, "EDIT", "100", WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_RIGHT | ES_AUTOHSCROLL,
        dp(552), ocY, fieldW, dp(20),
        hParent, (HMENU)(INT_PTR)POWER_LIMIT_ID, hInst, nullptr);
    style_input_control(g_app.hPowerLimitEdit);

    HWND fanModeLabel = CreateWindowExA(0, "STATIC", "Fan Control:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        dp(650), ocY + dp(2), dp(88), dp(18), hParent, nullptr, hInst, nullptr);
    main_layout_register_static(MAIN_STATIC_FAN_MODE_LABEL, fanModeLabel);
    g_app.hFanModeCombo = CreateWindowExA(
        0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
        dp(738), ocY, dp(136), dp(220),
        hParent, (HMENU)(INT_PTR)FAN_MODE_COMBO_ID, hInst, nullptr);
    style_combo_control(g_app.hFanModeCombo);
    SendMessageA(g_app.hFanModeCombo, CB_ADDSTRING, 0, (LPARAM)fan_mode_label(FAN_MODE_AUTO));
    SendMessageA(g_app.hFanModeCombo, CB_ADDSTRING, 0, (LPARAM)fan_mode_label(FAN_MODE_FIXED));
    SendMessageA(g_app.hFanModeCombo, CB_ADDSTRING, 0, (LPARAM)fan_mode_label(FAN_MODE_CURVE));
    SendMessageA(g_app.hFanModeCombo, CB_SETCURSEL, (WPARAM)g_app.guiFanMode, 0);

    HWND fanFixedLabel = CreateWindowExA(0, "STATIC", "Fixed %:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        dp(882), ocY + dp(2), dp(58), dp(18), hParent, nullptr, hInst, nullptr);
    main_layout_register_static(MAIN_STATIC_FAN_FIXED_LABEL, fanFixedLabel);
    g_app.hFanEdit = CreateWindowExA(
        0, "EDIT", "50", WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_RIGHT | ES_AUTOHSCROLL,
        dp(942), ocY, dp(56), dp(20),
        hParent, (HMENU)(INT_PTR)FAN_CONTROL_ID, hInst, nullptr);
    style_input_control(g_app.hFanEdit);
    g_app.hFanCurveBtn = CreateWindowExA(
        0, "BUTTON", "Edit Curve...", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        dp(1006), ocY - dp(1), dp(160), dp(24),
        hParent, (HMENU)(INT_PTR)FAN_CURVE_BTN_ID, hInst, nullptr);

    layout_bottom_buttons(hParent);
    style_combo_control(g_app.hProfileCombo);
    style_combo_control(g_app.hAppLaunchCombo);
    style_combo_control(g_app.hLogonCombo);
    apply_ui_font_to_children(hParent);
    populate_global_controls();
    if (g_app.loaded && !g_guiRebuildPreserveDraft) populate_edits();
    create_lock_tooltips(hParent);
    refresh_profile_controls_from_config();
    end_programmatic_edit_update();
}
