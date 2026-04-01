static int fan_curve_dialog_combo_value(HWND combo, int fallback) {
    if (!combo) return fallback;
    int sel = (int)SendMessageA(combo, CB_GETCURSEL, 0, 0);
    if (sel < 0) return fallback;
    LRESULT value = SendMessageA(combo, CB_GETITEMDATA, (WPARAM)sel, 0);
    if (value == CB_ERR) return fallback;
    return (int)value;
}

static void fan_curve_dialog_select_combo_value(HWND combo, int value) {
    if (!combo) return;
    int count = (int)SendMessageA(combo, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < count; i++) {
        if ((int)SendMessageA(combo, CB_GETITEMDATA, (WPARAM)i, 0) == value) {
            SendMessageA(combo, CB_SETCURSEL, (WPARAM)i, 0);
            return;
        }
    }
    if (count > 0) SendMessageA(combo, CB_SETCURSEL, 0, 0);
}

static SIZE fan_curve_dialog_min_size() {
    return adjusted_window_size_for_client(
        dp(500),
        dp(520),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        WS_EX_DLGMODALFRAME);
}

static SIZE fan_curve_dialog_default_size() {
    return adjusted_window_size_for_client(
        dp(520),
        dp(540),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        WS_EX_DLGMODALFRAME);
}

static void fan_curve_dialog_sync_controls() {
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        if (g_fanCurveDialog.enableChecks[i]) {
            SendMessageA(
                g_fanCurveDialog.enableChecks[i],
                BM_SETCHECK,
                (WPARAM)(g_fanCurveDialog.working.points[i].enabled ? BST_CHECKED : BST_UNCHECKED),
                0);
        }
        if (g_fanCurveDialog.tempEdits[i]) {
            char buf[16] = {};
            StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", g_fanCurveDialog.working.points[i].temperatureC);
            SetWindowTextA(g_fanCurveDialog.tempEdits[i], buf);
        }
        if (g_fanCurveDialog.percentEdits[i]) {
            char buf[16] = {};
            StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", g_fanCurveDialog.working.points[i].fanPercent);
            SetWindowTextA(g_fanCurveDialog.percentEdits[i], buf);
        }
    }

    fan_curve_dialog_select_combo_value(g_fanCurveDialog.intervalCombo, g_fanCurveDialog.working.pollIntervalMs);
    fan_curve_dialog_select_combo_value(g_fanCurveDialog.hysteresisCombo, g_fanCurveDialog.working.hysteresisC);
}

static bool fan_curve_dialog_capture_working(bool strict, bool normalize, FanCurveConfig* out, char* err, size_t errSize) {
    FanCurveConfig preview = g_fanCurveDialog.working;
    preview.pollIntervalMs = fan_curve_dialog_combo_value(g_fanCurveDialog.intervalCombo, preview.pollIntervalMs);
    preview.hysteresisC = fan_curve_dialog_combo_value(g_fanCurveDialog.hysteresisCombo, preview.hysteresisC);

    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        char buf[32] = {};

        if (g_fanCurveDialog.tempEdits[i]) {
            get_window_text_safe(g_fanCurveDialog.tempEdits[i], buf, sizeof(buf));
            int value = preview.points[i].temperatureC;
            if (buf[0]) {
                if (!parse_int_strict(buf, &value)) {
                    if (strict) {
                        set_message(err, errSize, "Invalid temperature for fan point %d", i + 1);
                        return false;
                    }
                } else {
                    preview.points[i].temperatureC = value;
                }
            }
        }

        if (g_fanCurveDialog.percentEdits[i]) {
            get_window_text_safe(g_fanCurveDialog.percentEdits[i], buf, sizeof(buf));
            int value = preview.points[i].fanPercent;
            if (buf[0]) {
                if (!parse_int_strict(buf, &value)) {
                    if (strict) {
                        set_message(err, errSize, "Invalid fan percentage for fan point %d", i + 1);
                        return false;
                    }
                } else {
                    preview.points[i].fanPercent = value;
                }
            }
        }

        if (preview.points[i].temperatureC < 0) preview.points[i].temperatureC = 0;
        if (preview.points[i].temperatureC > 100) preview.points[i].temperatureC = 100;
        preview.points[i].fanPercent = clamp_percent(preview.points[i].fanPercent);
    }

    if (normalize) fan_curve_normalize(&preview);
    if (strict && !fan_curve_validate(&preview, err, errSize)) return false;

    if (out) *out = preview;
    return true;
}

static void fan_curve_dialog_toggle_point(int pointIndex, HWND hwnd) {
    if (pointIndex < 0 || pointIndex >= FAN_CURVE_MAX_POINTS) return;

    FanCurveConfig candidate = g_fanCurveDialog.working;
    candidate.points[pointIndex].enabled = !candidate.points[pointIndex].enabled;
    if (fan_curve_active_count(&candidate) < 2) {
        MessageBoxA(hwnd, "At least two fan curve points must remain enabled.", "Green Curve", MB_OK | MB_ICONINFORMATION);
        fan_curve_dialog_sync_controls();
        return;
    }

    g_fanCurveDialog.working = candidate;
    fan_curve_dialog_sync_controls();
    InvalidateRect(hwnd, nullptr, FALSE);
}

static void fan_curve_dialog_update_working_from_controls(HWND hwnd) {
    FanCurveConfig preview = {};
    if (!fan_curve_dialog_capture_working(false, false, &preview, nullptr, 0)) return;
    g_fanCurveDialog.working = preview;
    InvalidateRect(hwnd, nullptr, FALSE);
}

static void fan_curve_dialog_draw_preview(HWND hwnd, HDC hdc) {
    RECT client = {};
    GetClientRect(hwnd, &client);

    RECT graph = { dp(16), dp(16), client.right - dp(16), dp(220) };
    RECT plot = graph;
    int pointRadius = dp(3);
    InflateRect(&plot, -dp(8), -dp(8));
    if ((plot.right - plot.left) < dp(40) || (plot.bottom - plot.top) < dp(40)) {
        plot = graph;
    }

    HBRUSH bg = CreateSolidBrush(RGB(0x18, 0x18, 0x28));
    FillRect(hdc, &graph, bg);
    DeleteObject(bg);

    HPEN gridPen = CreatePen(PS_SOLID, 1, COL_GRID);
    HPEN axisPen = CreatePen(PS_SOLID, 1, COL_AXIS);
    HPEN curvePen = CreatePen(PS_SOLID, 2, RGB(0x50, 0xD0, 0x80));
    HBRUSH pointBrush = CreateSolidBrush(COL_POINT);
    HGDIOBJ oldPen = SelectObject(hdc, gridPen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, COL_LABEL);

    const int width = nvmax(1, plot.right - plot.left);
    const int height = nvmax(1, plot.bottom - plot.top);
    for (int t = 0; t <= 100; t += 10) {
        int x = plot.left + (t * (width - 1)) / 100;
        MoveToEx(hdc, x, plot.top, nullptr);
        LineTo(hdc, x, plot.bottom);
    }
    for (int p = 0; p <= 100; p += 20) {
        int y = plot.bottom - 1 - (p * (height - 1)) / 100;
        MoveToEx(hdc, plot.left, y, nullptr);
        LineTo(hdc, plot.right, y);
    }

    SelectObject(hdc, axisPen);
    Rectangle(hdc, plot.left, plot.top, plot.right, plot.bottom);

    FanCurveConfig preview = {};
    if (!fan_curve_dialog_capture_working(false, false, &preview, nullptr, 0)) {
        preview = g_fanCurveDialog.working;
    }

    FanCurvePoint active[FAN_CURVE_MAX_POINTS] = {};
    int activeCount = 0;
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        if (preview.points[i].enabled) active[activeCount++] = preview.points[i];
    }
    for (int i = 0; i < activeCount; i++) {
        for (int j = i + 1; j < activeCount; j++) {
            if (active[j].temperatureC < active[i].temperatureC) {
                FanCurvePoint temp = active[i];
                active[i] = active[j];
                active[j] = temp;
            }
        }
    }

    if (activeCount > 0) {
        SelectObject(hdc, curvePen);
        for (int i = 0; i < activeCount; i++) {
            int x = plot.left + (active[i].temperatureC * (width - 1)) / 100;
            int y = plot.bottom - 1 - (active[i].fanPercent * (height - 1)) / 100;
            if (i == 0) MoveToEx(hdc, x, y, nullptr);
            else LineTo(hdc, x, y);
        }

        SelectObject(hdc, pointBrush);
        for (int i = 0; i < activeCount; i++) {
            int x = plot.left + (active[i].temperatureC * (width - 1)) / 100;
            int y = plot.bottom - 1 - (active[i].fanPercent * (height - 1)) / 100;
            Ellipse(hdc, x - pointRadius, y - pointRadius, x + pointRadius + 1, y + pointRadius + 1);
        }
    }

    char summary[128] = {};
    fan_curve_format_summary(&preview, summary, sizeof(summary));
    TextOutA(hdc, graph.left, graph.bottom + dp(6), summary, (int)strlen(summary));
    TextOutA(hdc, graph.left, graph.top - dp(14), "Fan %", 5);
    TextOutA(hdc, graph.right - dp(46), graph.bottom + dp(6), "Temp C", 6);

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pointBrush);
    DeleteObject(curvePen);
    DeleteObject(axisPen);
    DeleteObject(gridPen);
}

static bool fan_curve_dialog_commit(HWND hwnd) {
    FanCurveConfig validated = {};
    char err[256] = {};
    if (!fan_curve_dialog_capture_working(true, true, &validated, err, sizeof(err))) {
        MessageBoxA(hwnd, err, "Green Curve", MB_OK | MB_ICONERROR);
        return false;
    }

    copy_fan_curve(&g_app.guiFanCurve, &validated);
    ensure_valid_fan_curve_config(&g_app.guiFanCurve);
    refresh_fan_curve_button_text();
    update_fan_controls_enabled_state();
    return true;
}

static LRESULT CALLBACK FanCurveDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

            CreateWindowExA(0, "STATIC", "Enable", WS_CHILD | WS_VISIBLE | SS_LEFT,
                dp(18), dp(252), dp(52), dp(18), hwnd, nullptr, g_app.hInst, nullptr);
            CreateWindowExA(0, "STATIC", "Temp C", WS_CHILD | WS_VISIBLE | SS_LEFT,
                dp(110), dp(252), dp(58), dp(18), hwnd, nullptr, g_app.hInst, nullptr);
            CreateWindowExA(0, "STATIC", "Fan %", WS_CHILD | WS_VISIBLE | SS_LEFT,
                dp(198), dp(252), dp(58), dp(18), hwnd, nullptr, g_app.hInst, nullptr);

            for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
                int y = dp(276 + i * 28);
                char label[16] = {};
                StringCchPrintfA(label, ARRAY_COUNT(label), "P%d", i + 1);
                g_fanCurveDialog.enableChecks[i] = CreateWindowExA(
                    0, "BUTTON", label,
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_CHECKBOX,
                    dp(18), y, dp(70), dp(22),
                    hwnd, (HMENU)(INT_PTR)(FAN_DIALOG_ENABLE_BASE + i), g_app.hInst, nullptr);
                g_fanCurveDialog.tempEdits[i] = CreateWindowExA(
                    WS_EX_CLIENTEDGE, "EDIT", "",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
                    dp(104), y - dp(2), dp(68), dp(22),
                    hwnd, (HMENU)(INT_PTR)(FAN_DIALOG_TEMP_BASE + i), g_app.hInst, nullptr);
                g_fanCurveDialog.percentEdits[i] = CreateWindowExA(
                    WS_EX_CLIENTEDGE, "EDIT", "",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
                    dp(192), y - dp(2), dp(68), dp(22),
                    hwnd, (HMENU)(INT_PTR)(FAN_DIALOG_PERCENT_BASE + i), g_app.hInst, nullptr);
                SendMessageA(g_fanCurveDialog.enableChecks[i], WM_SETFONT, (WPARAM)font, TRUE);
                SendMessageA(g_fanCurveDialog.tempEdits[i], WM_SETFONT, (WPARAM)font, TRUE);
                SendMessageA(g_fanCurveDialog.percentEdits[i], WM_SETFONT, (WPARAM)font, TRUE);
            }

            CreateWindowExA(0, "STATIC", "Poll interval", WS_CHILD | WS_VISIBLE | SS_LEFT,
                dp(304), dp(276), dp(90), dp(18), hwnd, nullptr, g_app.hInst, nullptr);
            g_fanCurveDialog.intervalCombo = CreateWindowExA(
                0, "COMBOBOX", "",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                dp(304), dp(296), dp(150), dp(180),
                hwnd, (HMENU)(INT_PTR)FAN_DIALOG_INTERVAL_ID, g_app.hInst, nullptr);
            const int intervals[] = { 250, 500, 750, 1000, 1500, 2000, 3000, 4000, 5000 };
            for (int value : intervals) {
                char text[32] = {};
                StringCchPrintfA(text, ARRAY_COUNT(text), "%.2fs", (double)value / 1000.0);
                int index = (int)SendMessageA(g_fanCurveDialog.intervalCombo, CB_ADDSTRING, 0, (LPARAM)text);
                SendMessageA(g_fanCurveDialog.intervalCombo, CB_SETITEMDATA, (WPARAM)index, (LPARAM)value);
            }

            CreateWindowExA(0, "STATIC", "Hysteresis", WS_CHILD | WS_VISIBLE | SS_LEFT,
                dp(304), dp(334), dp(90), dp(18), hwnd, nullptr, g_app.hInst, nullptr);
            g_fanCurveDialog.hysteresisCombo = CreateWindowExA(
                0, "COMBOBOX", "",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                dp(304), dp(354), dp(150), dp(180),
                hwnd, (HMENU)(INT_PTR)FAN_DIALOG_HYSTERESIS_ID, g_app.hInst, nullptr);
            for (int value = 0; value <= 5; value++) {
                char text[16] = {};
                StringCchPrintfA(text, ARRAY_COUNT(text), "%d C", value);
                int index = (int)SendMessageA(g_fanCurveDialog.hysteresisCombo, CB_ADDSTRING, 0, (LPARAM)text);
                SendMessageA(g_fanCurveDialog.hysteresisCombo, CB_SETITEMDATA, (WPARAM)index, (LPARAM)value);
            }

            g_fanCurveDialog.okButton = CreateWindowExA(
                0, "BUTTON", "OK",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                dp(304), dp(406), dp(72), dp(28),
                hwnd, (HMENU)(INT_PTR)FAN_DIALOG_OK_ID, g_app.hInst, nullptr);
            g_fanCurveDialog.cancelButton = CreateWindowExA(
                0, "BUTTON", "Close",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                dp(384), dp(406), dp(72), dp(28),
                hwnd, (HMENU)(INT_PTR)FAN_DIALOG_CANCEL_ID, g_app.hInst, nullptr);

            SendMessageA(g_fanCurveDialog.intervalCombo, WM_SETFONT, (WPARAM)font, TRUE);
            SendMessageA(g_fanCurveDialog.hysteresisCombo, WM_SETFONT, (WPARAM)font, TRUE);
            SendMessageA(g_fanCurveDialog.okButton, WM_SETFONT, (WPARAM)font, TRUE);
            SendMessageA(g_fanCurveDialog.cancelButton, WM_SETFONT, (WPARAM)font, TRUE);
            fan_curve_dialog_sync_controls();
            return 0;
        }

        case WM_SIZE:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            if (mmi) {
                SIZE minSize = fan_curve_dialog_min_size();
                mmi->ptMinTrackSize.x = minSize.cx;
                mmi->ptMinTrackSize.y = minSize.cy;
            }
            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int notification = HIWORD(wParam);

            if (id >= FAN_DIALOG_ENABLE_BASE && id < FAN_DIALOG_ENABLE_BASE + FAN_CURVE_MAX_POINTS && notification == BN_CLICKED) {
                fan_curve_dialog_toggle_point(id - FAN_DIALOG_ENABLE_BASE, hwnd);
                return 0;
            }
            if (((id >= FAN_DIALOG_TEMP_BASE && id < FAN_DIALOG_TEMP_BASE + FAN_CURVE_MAX_POINTS) ||
                 (id >= FAN_DIALOG_PERCENT_BASE && id < FAN_DIALOG_PERCENT_BASE + FAN_CURVE_MAX_POINTS)) &&
                notification == EN_CHANGE) {
                fan_curve_dialog_update_working_from_controls(hwnd);
                return 0;
            }
            if ((id == FAN_DIALOG_INTERVAL_ID || id == FAN_DIALOG_HYSTERESIS_ID) && notification == CBN_SELCHANGE) {
                fan_curve_dialog_update_working_from_controls(hwnd);
                return 0;
            }
            if (id == FAN_DIALOG_OK_ID && notification == BN_CLICKED) {
                if (!fan_curve_dialog_commit(hwnd)) return 0;
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == FAN_DIALOG_CANCEL_ID && notification == BN_CLICKED) {
                if (!fan_curve_dialog_commit(hwnd)) return 0;
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps = {};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT client = {};
            GetClientRect(hwnd, &client);
            HBRUSH bg = CreateSolidBrush(COL_BG);
            FillRect(hdc, &client, bg);
            DeleteObject(bg);
            fan_curve_dialog_draw_preview(hwnd, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CTLCOLOREDIT: {
            HDC hdcEdit = (HDC)wParam;
            SetTextColor(hdcEdit, COL_TEXT);
            SetBkColor(hdcEdit, RGB(0x1A, 0x1A, 0x2A));
            static HBRUSH hEditBrush = CreateSolidBrush(RGB(0x1A, 0x1A, 0x2A));
            return (LRESULT)hEditBrush;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORLISTBOX: {
            HDC hdcStatic = (HDC)wParam;
            SetTextColor(hdcStatic, COL_LABEL);
            SetBkColor(hdcStatic, COL_BG);
            static HBRUSH hStaticBrush = CreateSolidBrush(COL_BG);
            return (LRESULT)hStaticBrush;
        }

        case WM_CLOSE:
            if (!fan_curve_dialog_commit(hwnd)) return 0;
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            g_fanCurveDialog.hwnd = nullptr;
            if (g_app.hMainWnd) {
                EnableWindow(g_app.hMainWnd, TRUE);
                SetForegroundWindow(g_app.hMainWnd);
            }
            memset(&g_fanCurveDialog, 0, sizeof(g_fanCurveDialog));
            return 0;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void open_fan_curve_dialog() {
    if (g_fanCurveDialog.hwnd) {
        ShowWindow(g_fanCurveDialog.hwnd, SW_SHOW);
        SetForegroundWindow(g_fanCurveDialog.hwnd);
        return;
    }

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = FanCurveDialogProc;
    wc.hInstance = g_app.hInst;
    wc.lpszClassName = "GreenCurveFanCurveDialog";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = (HICON)SendMessageA(g_app.hMainWnd, WM_GETICON, ICON_SMALL, 0);
    RegisterClassExA(&wc);

    ensure_valid_fan_curve_config(&g_app.guiFanCurve);
    copy_fan_curve(&g_fanCurveDialog.working, &g_app.guiFanCurve);

    RECT ownerRect = {};
    GetWindowRect(g_app.hMainWnd, &ownerRect);
    SIZE defaultSize = fan_curve_dialog_default_size();
    int width = defaultSize.cx;
    int height = defaultSize.cy;
    int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    int y = ownerRect.top + dp(40);

    g_fanCurveDialog.hwnd = CreateWindowExA(
        WS_EX_DLGMODALFRAME,
        wc.lpszClassName,
        "Custom Fan Curve",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        x, y, width, height,
        g_app.hMainWnd, nullptr, g_app.hInst, nullptr);

    if (!g_fanCurveDialog.hwnd) {
        return;
    }

    EnableWindow(g_app.hMainWnd, FALSE);
    ShowWindow(g_fanCurveDialog.hwnd, SW_SHOW);
    UpdateWindow(g_fanCurveDialog.hwnd);
    fan_curve_dialog_sync_controls();
    InvalidateRect(g_fanCurveDialog.hwnd, nullptr, FALSE);
}