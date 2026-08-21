// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// xbar_dialog.cpp -- Advanced XBAR clock domain controls (Blackwell only).
// Properly themed dialog integrated with the service pending model.
// Included by main_shell.cpp after fan_curve_dialog.cpp.


#define XBAR_DIALOG_CLASS "GreenCurveXbarDialog"
#define XBAR_OFFSET_EDIT_ID    3200
#define XBAR_MSVDD_EDIT_ID     3201
#define XBAR_OK_BTN_ID         3202
#define XBAR_CANCEL_BTN_ID     3203
#define XBAR_RESET_BTN_ID      3204
#define XBAR_CURRENT_LABEL_ID  3205
#define XBAR_MEASURED_LABEL_ID 3206
#define XBAR_HINT_LABEL_ID     3207

struct XbarDialogState {
    HWND hwnd;
    HWND hOffsetEdit;
    HWND hMsvddEdit;
    HWND hCurrentLabel;
    HWND hMeasuredLabel;
    HWND hHintLabel;
    HWND hOkBtn;
    HWND hCancelBtn;
    HWND hResetBtn;
    HBRUSH hEditBrush;
    HBRUSH hBgBrush;
    HBRUSH hInputBrush;
    bool active;
};

static XbarDialogState g_xbarDialog = {};

static void xbar_dialog_sync_controls() {
    if (!g_xbarDialog.hwnd) return;

    // Effective current values come from the service control state when available,
    // otherwise from the live snapshot. This matches the pending model's applied side.
    ControlState control = {};
    bool haveControl = get_effective_control_state(&control);
    int appliedKhz = 0;
    int appliedUv = 0;
    unsigned int measuredKhz = g_app.xbarMeasuredClockKhz;
    bool xbarSupported = false;
    if (haveControl) {
        appliedKhz = control.hasXbarOffset ? control.xbarOffsetKhz : 0;
        appliedUv = control.hasXbarMsvddOffset ? control.xbarMsvddOffsetUv : 0;
        measuredKhz = control.hasXbarOffset ? g_app.xbarMeasuredClockKhz : measuredKhz;
        xbarSupported = control.hasXbarOffset || control.hasXbarMsvddOffset || g_app.xbarProbeValid;
    } else {
        appliedKhz = g_app.xbarFreqOffsetKhz;
        appliedUv = g_app.xbarMsvddOffsetUv;
        xbarSupported = g_app.xbarProbeValid;
    }
    // Pending values: draft text if dirty, otherwise guiXbar fields.
    int pendingKhz = appliedKhz;
    int pendingUv = appliedUv;
    if (gui_state_dirty()) {
        if (g_app.guiDraft.xbarOffsetText[0]) {
            int v = 0;
            if (parse_int_strict(g_app.guiDraft.xbarOffsetText, &v)) pendingKhz = v * 1000;
        } else {
            pendingKhz = g_app.guiXbarOffsetKhz;
        }
        if (g_app.guiDraft.xbarMsvddOffsetText[0]) {
            int v = 0;
            if (parse_int_strict(g_app.guiDraft.xbarMsvddOffsetText, &v)) pendingUv = v * 1000;
        } else {
            pendingUv = g_app.guiXbarMsvddOffsetUv;
        }
    } else {
        pendingKhz = g_app.guiXbarOffsetKhz;
        pendingUv = g_app.guiXbarMsvddOffsetUv;
        // If guiXbar is still zero-initialized and applied has a value (e.g. after service reconnect),
        // show the applied value rather than 0.
        if (pendingKhz == 0 && appliedKhz != 0) pendingKhz = appliedKhz;
        if (pendingUv == 0 && appliedUv != 0) pendingUv = appliedUv;
    }

    char buf[128] = {};
    if (!xbarSupported) {
        StringCchPrintfA(buf, 128, "XBAR not available on this GPU / driver");
        SetWindowTextA(g_xbarDialog.hCurrentLabel, buf);
        SetWindowTextA(g_xbarDialog.hMeasuredLabel, "");
        EnableWindow(g_xbarDialog.hOffsetEdit, FALSE);
        EnableWindow(g_xbarDialog.hMsvddEdit, FALSE);
        EnableWindow(g_xbarDialog.hOkBtn, FALSE);
        EnableWindow(g_xbarDialog.hResetBtn, FALSE);
        return;
    }
    EnableWindow(g_xbarDialog.hOffsetEdit, TRUE);
    EnableWindow(g_xbarDialog.hMsvddEdit, TRUE);
    EnableWindow(g_xbarDialog.hOkBtn, TRUE);
    EnableWindow(g_xbarDialog.hResetBtn, TRUE);

    // Populate edits with pending values (what the user will apply next).
    begin_programmatic_edit_update();
    StringCchPrintfA(buf, 32, "%d", pendingKhz / 1000);
    SetWindowTextA(g_xbarDialog.hOffsetEdit, buf);
    StringCchPrintfA(buf, 32, "%d", pendingUv / 1000);
    SetWindowTextA(g_xbarDialog.hMsvddEdit, buf);
    end_programmatic_edit_update();

    // Current applied label
    if (appliedKhz == 0 && appliedUv == 0) {
        StringCchPrintfA(buf, 128, "Current: stock (0 MHz / 0 mV)");
    } else {
        StringCchPrintfA(buf, 128, "Current: %d MHz / %d mV", appliedKhz / 1000, appliedUv / 1000);
    }
    SetWindowTextA(g_xbarDialog.hCurrentLabel, buf);

    // Measured XBAR clock
    if (measuredKhz > 0) {
        StringCchPrintfA(buf, 128, "Measured XBAR: %u MHz", measuredKhz / 1000);
        SetWindowTextA(g_xbarDialog.hMeasuredLabel, buf);
    } else {
        SetWindowTextA(g_xbarDialog.hMeasuredLabel, "Measured XBAR: ---");
    }
}

static bool xbar_dialog_commit(HWND hwnd) {
    char offsetBuf[64] = {};
    char msvddBuf[64] = {};
    GetWindowTextA(g_xbarDialog.hOffsetEdit, offsetBuf, 64);
    GetWindowTextA(g_xbarDialog.hMsvddEdit, msvddBuf, 64);
    trim_ascii(offsetBuf);
    trim_ascii(msvddBuf);
    if (!offsetBuf[0]) StringCchCopyA(offsetBuf, 64, "0");
    if (!msvddBuf[0]) StringCchCopyA(msvddBuf, 64, "0");
    int offsetMhz = 0;
    int msvddMv = 0;
    if (!parse_int_strict(offsetBuf, &offsetMhz)) {
        gc_message_box(hwnd, "Invalid XBAR clock offset. Use integer MHz.", "Green Curve", MB_OK | MB_ICONERROR);
        return false;
    }
    if (!parse_int_strict(msvddBuf, &msvddMv)) {
        gc_message_box(hwnd, "Invalid XBAR voltage offset. Use integer mV.", "Green Curve", MB_OK | MB_ICONERROR);
        return false;
    }
    if (offsetMhz < -1000 || offsetMhz > 1000) {
        gc_message_box(hwnd, "XBAR clock offset must be between -1000 and 1000 MHz.", "Green Curve", MB_OK | MB_ICONERROR);
        return false;
    }
    if (msvddMv < -100 || msvddMv > 100) {
        gc_message_box(hwnd, "XBAR voltage offset must be between -100 and 100 mV.", "Green Curve", MB_OK | MB_ICONERROR);
        return false;
    }
    int offsetKhz = offsetMhz * 1000;
    int msvddUv = msvddMv * 1000;

    // Write into draft and gui fields. This marks the editor dirty and makes
    // the Advanced button show pending orange until the user presses Apply.
    g_app.guiXbarOffsetKhz = offsetKhz;
    g_app.guiXbarMsvddOffsetUv = msvddUv;
    g_app.guiXbarOffsetFromProfileLoad = false;
    g_app.guiXbarMsvddOffsetFromProfileLoad = false;
    g_app.guiHasUserModifiedValues = true;
    set_gui_state_dirty(true);
    StringCchPrintfA(g_app.guiDraft.xbarOffsetText, 32, "%d", offsetMhz);
    StringCchPrintfA(g_app.guiDraft.xbarMsvddOffsetText, 32, "%d", msvddMv);
    debug_log("xbar dialog: committed offset %d MHz (%d kHz) msvdd %d mV (%d uV)\n", offsetMhz, offsetKhz, msvddMv, msvddUv);
    gui_pending_changes_refresh();
    return true;
}

static void xbar_draw_button(HWND hwnd, const DRAWITEMSTRUCT* dis) {
    if (!dis) return;
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    bool focused = (dis->itemState & ODS_FOCUS) != 0;
    HFONT controlFont = dis->hwndItem ? (HFONT)SendMessageA(dis->hwndItem, WM_GETFONT, 0, 0) : nullptr;
    HFONT oldFont = (HFONT)SelectObject(hdc, controlFont ? controlFont : get_ui_font());
    HBRUSH bg = CreateSolidBrush(COL_BG);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(hdc, TRANSPARENT);
    COLORREF fill = disabled ? COL_BUTTON_DISABLED : (pressed ? COL_BUTTON_PRESSED : COL_BUTTON);
    HBRUSH fillBr = CreateSolidBrush(fill);
    FillRect(hdc, &rc, fillBr);
    DeleteObject(fillBr);
    COLORREF border = disabled ? COL_DISABLED_BORDER : COL_BUTTON_BORDER;
    HPEN borderPen = CreatePen(PS_SOLID, 1, border);
    HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, oldBrush);
    DeleteObject(SelectObject(hdc, oldPen));
    char text[64] = {};
    GetWindowTextA(dis->hwndItem, text, 64);
    RECT textRc = rc;
    if (pressed) OffsetRect(&textRc, 0, 1);
    SetTextColor(hdc, disabled ? COL_LABEL : COL_BUTTON_LABEL);
    DrawTextA(hdc, text, -1, &textRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (focused) {
        RECT focus = rc;
        InflateRect(&focus, -3, -3);
        DrawFocusRect(hdc, &focus);
    }
    SelectObject(hdc, oldFont);
}

static LRESULT CALLBACK XbarDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        apply_system_titlebar_theme(hwnd);
        allow_dark_mode_for_window(hwnd);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_xbarDialog.hEditBrush) { DeleteObject(g_xbarDialog.hEditBrush); g_xbarDialog.hEditBrush = nullptr; }
        if (g_xbarDialog.hBgBrush) { DeleteObject(g_xbarDialog.hBgBrush); g_xbarDialog.hBgBrush = nullptr; }
        if (g_xbarDialog.hInputBrush) { DeleteObject(g_xbarDialog.hInputBrush); g_xbarDialog.hInputBrush = nullptr; }
        if (g_xbarDialog.hwnd == hwnd) {
            g_xbarDialog.hwnd = nullptr;
            g_xbarDialog.active = false;
            if (g_app.hMainWnd) {
                EnableWindow(g_app.hMainWnd, TRUE);
                SetForegroundWindow(g_app.hMainWnd);
            }
        }
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, COL_LABEL);
        SetBkColor(hdc, COL_BG);
        if (!g_xbarDialog.hBgBrush) g_xbarDialog.hBgBrush = CreateSolidBrush(COL_BG);
        return (LRESULT)g_xbarDialog.hBgBrush;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, COL_TEXT);
        SetBkColor(hdc, COL_INPUT);
        if (!g_xbarDialog.hInputBrush) g_xbarDialog.hInputBrush = CreateSolidBrush(COL_INPUT);
        return (LRESULT)g_xbarDialog.hInputBrush;
    }
    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, COL_TEXT);
        SetBkColor(hdc, COL_BG);
        if (!g_xbarDialog.hBgBrush) g_xbarDialog.hBgBrush = CreateSolidBrush(COL_BG);
        return (LRESULT)g_xbarDialog.hBgBrush;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
        if (dis && dis->CtlType == ODT_BUTTON) {
            if (dis->CtlID == XBAR_OK_BTN_ID || dis->CtlID == XBAR_CANCEL_BTN_ID || dis->CtlID == XBAR_RESET_BTN_ID) {
                xbar_draw_button(hwnd, dis);
                return TRUE;
            }
        }
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case XBAR_OK_BTN_ID:
            if (HIWORD(wParam) == BN_CLICKED) {
                if (xbar_dialog_commit(hwnd)) {
                    DestroyWindow(hwnd);
                }
                return 0;
            }
            break;
        case XBAR_CANCEL_BTN_ID:
            if (HIWORD(wParam) == BN_CLICKED) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case XBAR_RESET_BTN_ID:
            if (HIWORD(wParam) == BN_CLICKED) {
                begin_programmatic_edit_update();
                SetWindowTextA(g_xbarDialog.hOffsetEdit, "0");
                SetWindowTextA(g_xbarDialog.hMsvddEdit, "0");
                end_programmatic_edit_update();
                return 0;
            }
            break;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(COL_BG);
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void open_xbar_dialog() {
    if (g_xbarDialog.hwnd) {
        ShowWindow(g_xbarDialog.hwnd, SW_SHOW);
        SetForegroundWindow(g_xbarDialog.hwnd);
        xbar_dialog_sync_controls();
        return;
    }
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = XbarDialogProc;
    wc.hInstance = g_app.hInst;
    wc.lpszClassName = XBAR_DIALOG_CLASS;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExA(&wc);

    // dlgW/dlgH below are CLIENT coordinates.  CreateWindowExA takes the outer
    // frame size; passing the client height directly cut off the bottom row by
    // exactly the caption plus border.
    int clientW = dp(420);
    int clientH = dp(320);
    const DWORD dialogStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    const DWORD dialogExStyle = WS_EX_DLGMODALFRAME;
    SIZE outerSize = adjusted_window_size_for_client(
        clientW, clientH, dialogStyle, dialogExStyle);
    RECT work = {};
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    int x = work.left + (work.right - work.left - (int)outerSize.cx) / 2;
    int y = work.top + (work.bottom - work.top - (int)outerSize.cy) / 2;

    // Disable main window while dialog is open (modal-like) to match license dialog behavior.
    if (g_app.hMainWnd) EnableWindow(g_app.hMainWnd, FALSE);

    g_xbarDialog.hwnd = CreateWindowExA(
        dialogExStyle, XBAR_DIALOG_CLASS, "Advanced",
        dialogStyle,
        x, y, (int)outerSize.cx, (int)outerSize.cy,
        g_app.hMainWnd, nullptr, g_app.hInst, nullptr);
    if (!g_xbarDialog.hwnd) {
        if (g_app.hMainWnd) EnableWindow(g_app.hMainWnd, TRUE);
        return;
    }
    g_xbarDialog.active = true;
    HFONT hFont = get_ui_font();

    // Labels and edits
    int margin = dp(16);
    int labelW = dp(170);
    int editW = dp(80);
    int rowH = dp(22);
    int y0 = dp(16);

    HWND lblOffset = CreateWindowExA(0, "STATIC", "XBAR Clock Offset (MHz):",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        margin, y0 + dp(2), labelW, dp(18),
        g_xbarDialog.hwnd, (HMENU)(INT_PTR)XBAR_CURRENT_LABEL_ID, g_app.hInst, nullptr);
    g_xbarDialog.hOffsetEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "0",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        margin + labelW + dp(8), y0, editW, rowH,
        g_xbarDialog.hwnd, (HMENU)(INT_PTR)XBAR_OFFSET_EDIT_ID, g_app.hInst, nullptr);

    int y1 = y0 + rowH + dp(12);
    HWND lblMsvdd = CreateWindowExA(0, "STATIC", "XBAR Voltage Offset (mV):",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        margin, y1 + dp(2), labelW, dp(18),
        g_xbarDialog.hwnd, (HMENU)(INT_PTR)XBAR_HINT_LABEL_ID, g_app.hInst, nullptr);
    g_xbarDialog.hMsvddEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "0",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        margin + labelW + dp(8), y1, editW, rowH,
        g_xbarDialog.hwnd, (HMENU)(INT_PTR)XBAR_MSVDD_EDIT_ID, g_app.hInst, nullptr);

    int y2 = y1 + rowH + dp(16);
    g_xbarDialog.hCurrentLabel = CreateWindowExA(0, "STATIC", "Current: ---",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        margin, y2, clientW - margin*2, dp(18),
        g_xbarDialog.hwnd, (HMENU)(INT_PTR)XBAR_CURRENT_LABEL_ID, g_app.hInst, nullptr);
    int y3 = y2 + dp(20);
    g_xbarDialog.hMeasuredLabel = CreateWindowExA(0, "STATIC", "Measured XBAR: ---",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        margin, y3, clientW - margin*2, dp(18),
        g_xbarDialog.hwnd, (HMENU)(INT_PTR)XBAR_MEASURED_LABEL_ID, g_app.hInst, nullptr);

    int y4 = y3 + dp(24);
    const char* hintLines[] = {
        "Blackwell only. Requires the background service.",
        "Clock offset moves the XBAR domain; voltage offset",
        "adjusts its MSVDD rail. Start with small values",
        "(e.g. +50 MHz / +10 mV) and test stability.",
        "Changes apply with the main Apply button.",
    };
    for (int i = 0; i < 5; i++) {
        CreateWindowExA(0, "STATIC", hintLines[i],
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            margin, y4 + i*dp(16), clientW - margin*2, dp(16),
            g_xbarDialog.hwnd, nullptr, g_app.hInst, nullptr);
    }

    // Buttons at bottom
    int btnW = dp(80);
    int btnH = dp(26);
    int btnY = clientH - dp(48);
    g_xbarDialog.hOkBtn = CreateWindowExA(0, "BUTTON", "OK",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        clientW - margin - btnW*2 - dp(8), btnY, btnW, btnH,
        g_xbarDialog.hwnd, (HMENU)(INT_PTR)XBAR_OK_BTN_ID, g_app.hInst, nullptr);
    g_xbarDialog.hCancelBtn = CreateWindowExA(0, "BUTTON", "Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        clientW - margin - btnW, btnY, btnW, btnH,
        g_xbarDialog.hwnd, (HMENU)(INT_PTR)XBAR_CANCEL_BTN_ID, g_app.hInst, nullptr);
    g_xbarDialog.hResetBtn = CreateWindowExA(0, "BUTTON", "Reset",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        margin, btnY, btnW, btnH,
        g_xbarDialog.hwnd, (HMENU)(INT_PTR)XBAR_RESET_BTN_ID, g_app.hInst, nullptr);

    // Apply fonts
    HWND ctrls[] = { lblOffset, g_xbarDialog.hOffsetEdit, lblMsvdd, g_xbarDialog.hMsvddEdit,
        g_xbarDialog.hCurrentLabel, g_xbarDialog.hMeasuredLabel, g_xbarDialog.hOkBtn, g_xbarDialog.hCancelBtn, g_xbarDialog.hResetBtn };
    for (HWND c : ctrls) if (c) SendMessageA(c, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Hint lines fonts
    // Apply to all static children (already covered for labels; hint lines will inherit via parent font enumeration)
    // Ensure edits are styled like other inputs (border etc.)
    if (g_xbarDialog.hOffsetEdit) style_input_control(g_xbarDialog.hOffsetEdit);
    if (g_xbarDialog.hMsvddEdit) style_input_control(g_xbarDialog.hMsvddEdit);

    xbar_dialog_sync_controls();
    ShowWindow(g_xbarDialog.hwnd, SW_SHOW);
    SetForegroundWindow(g_xbarDialog.hwnd);
    UpdateWindow(g_xbarDialog.hwnd);
}
