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
        dp(640),
        dp(570),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        WS_EX_DLGMODALFRAME);
}

static SIZE fan_curve_dialog_default_size() {
    return adjusted_window_size_for_client(
        dp(660),
        dp(580),
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

static void fan_curve_dialog_temperature_bounds(const FanCurveConfig* curve, int pointIndex, int* minimumOut, int* maximumOut) {
    int minimum = 0;
    int maximum = 100;
    if (curve) {
        for (int i = pointIndex - 1; i >= 0; i--) {
            if (!curve->points[i].enabled) continue;
            minimum = curve->points[i].temperatureC + 1;
            break;
        }
        for (int i = pointIndex + 1; i < FAN_CURVE_MAX_POINTS; i++) {
            if (!curve->points[i].enabled) continue;
            maximum = curve->points[i].temperatureC - 1;
            break;
        }
    }
    minimum = nvmax(0, minimum);
    maximum = nvmin(100, maximum);
    if (maximum < minimum) maximum = minimum;
    if (minimumOut) *minimumOut = minimum;
    if (maximumOut) *maximumOut = maximum;
}

static bool fan_curve_dialog_temperature_in_bounds(const FanCurveConfig* curve, int pointIndex, int value) {
    int minimum = 0;
    int maximum = 100;
    fan_curve_dialog_temperature_bounds(curve, pointIndex, &minimum, &maximum);
    return value >= minimum && value <= maximum;
}

static void fan_curve_dialog_percent_bounds(const FanCurveConfig* curve, int pointIndex, int* minimumOut, int* maximumOut) {
    int minimum = 0;
    int maximum = 100;
    if (curve) {
        for (int i = pointIndex - 1; i >= 0; i--) {
            if (!curve->points[i].enabled) continue;
            minimum = curve->points[i].fanPercent;
            break;
        }
        for (int i = pointIndex + 1; i < FAN_CURVE_MAX_POINTS; i++) {
            if (!curve->points[i].enabled) continue;
            maximum = curve->points[i].fanPercent;
            break;
        }
    }
    minimum = nvmax(0, minimum);
    maximum = nvmin(100, maximum);
    if (maximum < minimum) maximum = minimum;
    if (minimumOut) *minimumOut = minimum;
    if (maximumOut) *maximumOut = maximum;
}

static bool fan_curve_dialog_percent_in_bounds(const FanCurveConfig* curve, int pointIndex, int value) {
    int minimum = 0;
    int maximum = 100;
    fan_curve_dialog_percent_bounds(curve, pointIndex, &minimum, &maximum);
    return value >= minimum && value <= maximum;
}

static void fan_curve_dialog_set_temperature_error(const FanCurveConfig* curve, int pointIndex, char* err, size_t errSize) {
    int minimum = 0;
    int maximum = 100;
    fan_curve_dialog_temperature_bounds(curve, pointIndex, &minimum, &maximum);
    if (minimum == maximum) {
        set_message(err, errSize, "Fan point %d temperature must be %d\xB0""C", pointIndex + 1, minimum);
        return;
    }
    set_message(err, errSize, "Fan point %d temperature must be between %d\xB0""C and %d\xB0""C", pointIndex + 1, minimum, maximum);
}

static void fan_curve_dialog_set_percent_error(const FanCurveConfig* curve, int pointIndex, char* err, size_t errSize) {
    int minimum = 0;
    int maximum = 100;
    fan_curve_dialog_percent_bounds(curve, pointIndex, &minimum, &maximum);
    if (minimum == maximum) {
        set_message(err, errSize, "Fan point %d percentage must be %d%%", pointIndex + 1, minimum);
        return;
    }
    set_message(err, errSize, "Fan point %d percentage must be between %d%% and %d%%", pointIndex + 1, minimum, maximum);
}

static bool fan_curve_dialog_validate_temperature_order(const FanCurveConfig* curve, char* err, size_t errSize) {
    if (!curve) return false;
    int previousEnabled = -1;
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        if (!curve->points[i].enabled) continue;
        if (previousEnabled >= 0 && curve->points[i].temperatureC <= curve->points[previousEnabled].temperatureC) {
            fan_curve_dialog_set_temperature_error(curve, i, err, errSize);
            return false;
        }
        previousEnabled = i;
    }
    return true;
}

static bool fan_curve_dialog_validate_percent_order(const FanCurveConfig* curve, char* err, size_t errSize) {
    if (!curve) return false;
    int previousEnabled = -1;
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        if (!curve->points[i].enabled) continue;
        if (previousEnabled >= 0 && curve->points[i].fanPercent < curve->points[previousEnabled].fanPercent) {
            fan_curve_dialog_set_percent_error(curve, i, err, errSize);
            return false;
        }
        previousEnabled = i;
    }
    return true;
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
                    if (value < 0) value = 0;
                    if (value > 100) value = 100;
                    if (preview.points[i].enabled && !fan_curve_dialog_temperature_in_bounds(&preview, i, value)) {
                        if (strict) {
                            fan_curve_dialog_set_temperature_error(&preview, i, err, errSize);
                            return false;
                        }
                    } else {
                        preview.points[i].temperatureC = value;
                    }
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
                    value = clamp_percent(value);
                    if (preview.points[i].enabled && !fan_curve_dialog_percent_in_bounds(&preview, i, value)) {
                        if (strict) {
                            fan_curve_dialog_set_percent_error(&preview, i, err, errSize);
                            return false;
                        }
                    } else {
                        preview.points[i].fanPercent = value;
                    }
                }
            }
        }

        if (preview.points[i].temperatureC < 0) preview.points[i].temperatureC = 0;
        if (preview.points[i].temperatureC > 100) preview.points[i].temperatureC = 100;
        preview.points[i].fanPercent = clamp_percent(preview.points[i].fanPercent);
    }

    if (strict && !fan_curve_dialog_validate_temperature_order(&preview, err, errSize)) return false;
    if (strict && !fan_curve_dialog_validate_percent_order(&preview, err, errSize)) return false;
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

static void fan_curve_dialog_sanitize_temperature_edit(int pointIndex) {
    if (pointIndex < 0 || pointIndex >= FAN_CURVE_MAX_POINTS) return;
    if (!g_fanCurveDialog.tempEdits[pointIndex]) return;

    char buf[32] = {};
    get_window_text_safe(g_fanCurveDialog.tempEdits[pointIndex], buf, sizeof(buf));

    int minimum = 0;
    int maximum = 100;
    if (g_fanCurveDialog.working.points[pointIndex].enabled) {
        fan_curve_dialog_temperature_bounds(&g_fanCurveDialog.working, pointIndex, &minimum, &maximum);
    }

    int value = g_fanCurveDialog.working.points[pointIndex].temperatureC;
    if (buf[0] && parse_int_strict(buf, &value)) {
        if (value < minimum) value = minimum;
        if (value > maximum) value = maximum;
    }

    char normalized[16] = {};
    StringCchPrintfA(normalized, ARRAY_COUNT(normalized), "%d", value);
    if (strcmp(buf, normalized) != 0) {
        SetWindowTextA(g_fanCurveDialog.tempEdits[pointIndex], normalized);
    }
}

static void fan_curve_dialog_sanitize_percent_edit(int pointIndex) {
    if (pointIndex < 0 || pointIndex >= FAN_CURVE_MAX_POINTS) return;
    if (!g_fanCurveDialog.percentEdits[pointIndex]) return;

    char buf[32] = {};
    get_window_text_safe(g_fanCurveDialog.percentEdits[pointIndex], buf, sizeof(buf));

    int minimum = 0;
    int maximum = 100;
    if (g_fanCurveDialog.working.points[pointIndex].enabled) {
        fan_curve_dialog_percent_bounds(&g_fanCurveDialog.working, pointIndex, &minimum, &maximum);
    }

    int value = g_fanCurveDialog.working.points[pointIndex].fanPercent;
    if (buf[0] && parse_int_strict(buf, &value)) {
        value = clamp_percent(value);
        if (value < minimum) value = minimum;
        if (value > maximum) value = maximum;
    }

    char normalized[16] = {};
    StringCchPrintfA(normalized, ARRAY_COUNT(normalized), "%d", value);
    if (strcmp(buf, normalized) != 0) {
        SetWindowTextA(g_fanCurveDialog.percentEdits[pointIndex], normalized);
    }
}

// Graph area helper  -  same coordinates used by draw and mouse handlers
static void fan_graph_get_rects(HWND hwnd, RECT* graphOut, RECT* plotOut) {
    RECT client = {};
    GetClientRect(hwnd, &client);
    // graph box: from y=16 to y=216
    // Inside: plot area leaves room for Y-axis labels (left) and X-axis labels (bottom)
    RECT graph = { dp(16), dp(16), client.right - dp(16), dp(216) };
    RECT plot = graph;
    plot.left   += dp(36);   // room for % labels on left
    plot.right  -= dp(8);
    plot.top    += dp(6);
    plot.bottom -= dp(22);   // room for degree labels below plot inside graph box
    if ((plot.right - plot.left) < dp(40) || (plot.bottom - plot.top) < dp(40)) plot = graph;
    if (graphOut) *graphOut = graph;
    if (plotOut)  *plotOut  = plot;
}

static int fan_graph_temp_to_x(const RECT& plot, int tempC) {
    int w = nvmax(1, plot.right - plot.left);
    return plot.left + (tempC * (w - 1)) / 100;
}
static int fan_graph_pct_to_y(const RECT& plot, int pct) {
    int h = nvmax(1, plot.bottom - plot.top);
    return plot.bottom - 1 - (pct * (h - 1)) / 100;
}
static int fan_graph_x_to_temp(const RECT& plot, int x) {
    int w = nvmax(1, plot.right - plot.left);
    int t = (x - plot.left) * 100 / (w - 1);
    if (t < 0) t = 0;
    if (t > 100) t = 100;
    return t;
}
static int fan_graph_y_to_pct(const RECT& plot, int y) {
    int h = nvmax(1, plot.bottom - plot.top);
    int p = (plot.bottom - 1 - y) * 100 / (h - 1);
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    return p;
}

// Find the working index of the enabled point closest to the mouse (-1 = none)
static int fan_graph_hit_test(const RECT& plot, int mx, int my) {
    int hitR = dp(10);
    int bestDist = INT_MAX;
    int bestIdx  = -1;
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        if (!g_fanCurveDialog.working.points[i].enabled) continue;
        int px = fan_graph_temp_to_x(plot, g_fanCurveDialog.working.points[i].temperatureC);
        int py = fan_graph_pct_to_y (plot, g_fanCurveDialog.working.points[i].fanPercent);
        int dx = mx - px, dy = my - py;
        int dist = dx*dx + dy*dy;
        if (dist < hitR * hitR && dist < bestDist) {
            bestDist = dist;
            bestIdx  = i;
        }
    }
    return bestIdx;
}

static void fan_curve_dialog_draw_preview(HWND hwnd, HDC hdc) {
    RECT graph, plot;
    fan_graph_get_rects(hwnd, &graph, &plot);

    HBRUSH bg = CreateSolidBrush(RGB(0x18, 0x18, 0x28));
    FillRect(hdc, &graph, bg);
    DeleteObject(bg);

    HPEN gridPen  = CreatePen(PS_SOLID, 1, COL_GRID);
    HPEN axisPen  = CreatePen(PS_SOLID, 1, COL_AXIS);
    HPEN curvePen = CreatePen(PS_SOLID, 2, RGB(0x50, 0xD0, 0x80));
    HGDIOBJ oldPen   = SelectObject(hdc, gridPen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, COL_LABEL);

    for (int t = 0; t <= 100; t += 10) {
        int x = fan_graph_temp_to_x(plot, t);
        MoveToEx(hdc, x, plot.top, nullptr);
        LineTo(hdc, x, plot.bottom);
    }
    for (int p = 0; p <= 100; p += 20) {
        int y = fan_graph_pct_to_y(plot, p);
        MoveToEx(hdc, plot.left, y, nullptr);
        LineTo(hdc, plot.right, y);
    }

    SelectObject(hdc, axisPen);
    Rectangle(hdc, plot.left, plot.top, plot.right, plot.bottom);

    for (int p = 0; p <= 100; p += 20) {
        char label[16] = {};
        StringCchPrintfA(label, ARRAY_COUNT(label), "%d%%", p);
        SIZE sz = {};
        GetTextExtentPoint32A(hdc, label, (int)strlen(label), &sz);
        int y = fan_graph_pct_to_y(plot, p) - sz.cy / 2;
        y = nvmax(graph.top, nvmin(graph.bottom - sz.cy, y));
        TextOutA(hdc, nvmax(graph.left, plot.left - dp(6) - sz.cx), y, label, (int)strlen(label));
    }
    for (int t = 0; t <= 100; t += 10) {
        char label[16] = {};
        StringCchPrintfA(label, ARRAY_COUNT(label), "%d\xB0""C", t);
        SIZE sz = {};
        GetTextExtentPoint32A(hdc, label, (int)strlen(label), &sz);
        int x = fan_graph_temp_to_x(plot, t) - sz.cx / 2;
        x = nvmax(graph.left, nvmin(graph.right - sz.cx, x));
        // Temperature labels sit just below the plot line, still inside the graph box
        TextOutA(hdc, x, plot.bottom + dp(3), label, (int)strlen(label));
    }

    // Draw curve
    FanCurvePoint active[FAN_CURVE_MAX_POINTS] = {};
    int activeIdx[FAN_CURVE_MAX_POINTS] = {}; // working indexleri
    int activeCount = 0;
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        if (!g_fanCurveDialog.working.points[i].enabled) continue;
        active[activeCount]    = g_fanCurveDialog.working.points[i];
        activeIdx[activeCount] = i;
        activeCount++;
    }
    // Sort by temperature
    for (int i = 0; i < activeCount - 1; i++) {
        for (int j = i + 1; j < activeCount; j++) {
            if (active[j].temperatureC < active[i].temperatureC) {
                FanCurvePoint tmp = active[i]; active[i] = active[j]; active[j] = tmp;
                int ti = activeIdx[i]; activeIdx[i] = activeIdx[j]; activeIdx[j] = ti;
            }
        }
    }

    if (activeCount > 0) {
        SelectObject(hdc, curvePen);
        for (int i = 0; i < activeCount; i++) {
            int x = fan_graph_temp_to_x(plot, active[i].temperatureC);
            int y = fan_graph_pct_to_y (plot, active[i].fanPercent);
            if (i == 0) MoveToEx(hdc, x, y, nullptr);
            else        LineTo(hdc, x, y);
        }
    }

    // Draw points
    int pr = dp(5);
    for (int i = 0; i < activeCount; i++) {
        int widx = activeIdx[i];
        int x = fan_graph_temp_to_x(plot, active[i].temperatureC);
        int y = fan_graph_pct_to_y (plot, active[i].fanPercent);
        bool isDragged = (g_fanCurveDialog.graphDragging && g_fanCurveDialog.graphDragPointIdx == widx);
        bool isHovered = (!g_fanCurveDialog.graphDragging && g_fanCurveDialog.graphHoverIdx == widx);
        COLORREF col = isDragged ? RGB(0xFF, 0xFF, 0x00)
                     : isHovered ? RGB(0xFF, 0xC0, 0x40)
                     : COL_POINT;
        HBRUSH ptBrush = CreateSolidBrush(col);
        SelectObject(hdc, ptBrush);
        SelectObject(hdc, GetStockObject(NULL_PEN));
        int r = isDragged ? dp(7) : isHovered ? dp(6) : pr;
        Ellipse(hdc, x - r, y - r, x + r + 1, y + r + 1);
        DeleteObject(ptBrush);

        // Show temp/% tooltip while dragging or hovering
        if (isDragged || isHovered) {
            char tip[32] = {};
            StringCchPrintfA(tip, ARRAY_COUNT(tip), "%d\xB0""C / %d%%",
                active[i].temperatureC, active[i].fanPercent);
            SetTextColor(hdc, RGB(0xFF, 0xFF, 0x80));
            int tx = x + dp(8), ty = y - dp(14);
            if (tx + dp(70) > plot.right)  tx = x - dp(70);
            if (ty < graph.top)             ty = y + dp(4);
            TextOutA(hdc, tx, ty, tip, (int)strlen(tip));
            SetTextColor(hdc, COL_LABEL);
        }
    }

    // Mode indicator and summary - drawn BELOW the graph box on the window background
    const char* modeHint =
        (g_fanCurveDialog.graphMode == FAN_GRAPH_MODE_ADD)    ? "[A] Add mode  -  click graph to add a point  |  Esc: normal mode" :
        (g_fanCurveDialog.graphMode == FAN_GRAPH_MODE_DELETE)  ? "[D] Delete mode  -  click a point to remove it  |  Esc: normal mode" :
        "Click and drag points  |  [A]: add mode  |  [D]: delete mode";
    COLORREF hintCol =
        (g_fanCurveDialog.graphMode == FAN_GRAPH_MODE_ADD)    ? RGB(0x60, 0xFF, 0x80) :
        (g_fanCurveDialog.graphMode == FAN_GRAPH_MODE_DELETE)  ? RGB(0xFF, 0x70, 0x70) :
        COL_LABEL;

    // Fill the strip below the graph with the window background colour before drawing text
    RECT stripHint    = { graph.left, graph.bottom + dp(4),  graph.right, graph.bottom + dp(20) };
    RECT stripSummary = { graph.left, graph.bottom + dp(20), graph.right, graph.bottom + dp(38) };
    HBRUSH bgBrush = CreateSolidBrush(COL_BG);
    FillRect(hdc, &stripHint,    bgBrush);
    FillRect(hdc, &stripSummary, bgBrush);
    DeleteObject(bgBrush);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, hintCol);
    TextOutA(hdc, graph.left, graph.bottom + dp(5), modeHint, (int)strlen(modeHint));

    char summary[128] = {};
    fan_curve_format_summary(&g_fanCurveDialog.working, summary, sizeof(summary));
    SetTextColor(hdc, COL_LABEL);
    TextOutA(hdc, graph.left, graph.bottom + dp(21), summary, (int)strlen(summary));

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
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

            // --- Left column: point table (P1-P8) ---
            // Column headers
            CreateWindowExA(0, "STATIC", "Enable", WS_CHILD | WS_VISIBLE | SS_LEFT,
                dp(16), dp(258), dp(70), dp(18), hwnd, nullptr, g_app.hInst, nullptr);
            CreateWindowExA(0, "STATIC", "Temp \xB0""C", WS_CHILD | WS_VISIBLE | SS_LEFT,
                dp(92), dp(258), dp(72), dp(18), hwnd, nullptr, g_app.hInst, nullptr);
            CreateWindowExA(0, "STATIC", "Fan %", WS_CHILD | WS_VISIBLE | SS_LEFT,
                dp(172), dp(258), dp(72), dp(18), hwnd, nullptr, g_app.hInst, nullptr);

            for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
                int y = dp(280 + i * 28);
                char label[16] = {};
                StringCchPrintfA(label, ARRAY_COUNT(label), "P%d", i + 1);
                g_fanCurveDialog.enableChecks[i] = CreateWindowExA(
                    0, "BUTTON", label,
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_CHECKBOX,
                    dp(16), y, dp(72), dp(22),
                    hwnd, (HMENU)(INT_PTR)(FAN_DIALOG_ENABLE_BASE + i), g_app.hInst, nullptr);
                g_fanCurveDialog.tempEdits[i] = CreateWindowExA(
                    WS_EX_CLIENTEDGE, "EDIT", "",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
                    dp(92), y, dp(72), dp(22),
                    hwnd, (HMENU)(INT_PTR)(FAN_DIALOG_TEMP_BASE + i), g_app.hInst, nullptr);
                g_fanCurveDialog.percentEdits[i] = CreateWindowExA(
                    WS_EX_CLIENTEDGE, "EDIT", "",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
                    dp(172), y, dp(72), dp(22),
                    hwnd, (HMENU)(INT_PTR)(FAN_DIALOG_PERCENT_BASE + i), g_app.hInst, nullptr);
                SendMessageA(g_fanCurveDialog.enableChecks[i], WM_SETFONT, (WPARAM)font, TRUE);
                SendMessageA(g_fanCurveDialog.tempEdits[i], WM_SETFONT, (WPARAM)font, TRUE);
                SendMessageA(g_fanCurveDialog.percentEdits[i], WM_SETFONT, (WPARAM)font, TRUE);
                SendMessageA(g_fanCurveDialog.tempEdits[i], EM_SETLIMITTEXT, 3, 0);
                SendMessageA(g_fanCurveDialog.percentEdits[i], EM_SETLIMITTEXT, 3, 0);
            }

            // --- Right column: Poll interval, Hysteresis, OK/Close ---
            // Right column starts at x=262 (after left table 16+72+8+72+8+72+14=262)
            CreateWindowExA(0, "STATIC", "Poll interval", WS_CHILD | WS_VISIBLE | SS_LEFT,
                dp(262), dp(258), dp(110), dp(18), hwnd, nullptr, g_app.hInst, nullptr);
            g_fanCurveDialog.intervalCombo = CreateWindowExA(
                0, "COMBOBOX", "",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                dp(262), dp(278), dp(160), dp(200),
                hwnd, (HMENU)(INT_PTR)FAN_DIALOG_INTERVAL_ID, g_app.hInst, nullptr);
            const int intervals[] = { 250, 500, 750, 1000, 1500, 2000, 3000, 4000, 5000 };
            for (int value : intervals) {
                char text[32] = {};
                StringCchPrintfA(text, ARRAY_COUNT(text), "%.2fs", (double)value / 1000.0);
                int index = (int)SendMessageA(g_fanCurveDialog.intervalCombo, CB_ADDSTRING, 0, (LPARAM)text);
                SendMessageA(g_fanCurveDialog.intervalCombo, CB_SETITEMDATA, (WPARAM)index, (LPARAM)value);
            }

            CreateWindowExA(0, "STATIC", "Hysteresis", WS_CHILD | WS_VISIBLE | SS_LEFT,
                dp(262), dp(318), dp(110), dp(18), hwnd, nullptr, g_app.hInst, nullptr);
            g_fanCurveDialog.hysteresisCombo = CreateWindowExA(
                0, "COMBOBOX", "",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                dp(262), dp(338), dp(160), dp(200),
                hwnd, (HMENU)(INT_PTR)FAN_DIALOG_HYSTERESIS_ID, g_app.hInst, nullptr);
            for (int value = 0; value <= FAN_CURVE_MAX_HYSTERESIS_C; value++) {
                char text[16] = {};
                StringCchPrintfA(text, ARRAY_COUNT(text), "%d\xB0""C", value);
                int index = (int)SendMessageA(g_fanCurveDialog.hysteresisCombo, CB_ADDSTRING, 0, (LPARAM)text);
                SendMessageA(g_fanCurveDialog.hysteresisCombo, CB_SETITEMDATA, (WPARAM)index, (LPARAM)value);
            }

            g_fanCurveDialog.okButton = CreateWindowExA(
                0, "BUTTON", "OK",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                dp(262), dp(388), dp(80), dp(28),
                hwnd, (HMENU)(INT_PTR)FAN_DIALOG_OK_ID, g_app.hInst, nullptr);
            g_fanCurveDialog.cancelButton = CreateWindowExA(
                0, "BUTTON", "Close",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                dp(350), dp(388), dp(80), dp(28),
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

        case WM_KEYDOWN: {
            if (wParam == 'A' || wParam == 'a') {
                g_fanCurveDialog.graphMode =
                    (g_fanCurveDialog.graphMode == FAN_GRAPH_MODE_ADD) ? FAN_GRAPH_MODE_NORMAL : FAN_GRAPH_MODE_ADD;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wParam == 'D' || wParam == 'd') {
                g_fanCurveDialog.graphMode =
                    (g_fanCurveDialog.graphMode == FAN_GRAPH_MODE_DELETE) ? FAN_GRAPH_MODE_NORMAL : FAN_GRAPH_MODE_DELETE;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                g_fanCurveDialog.graphMode = FAN_GRAPH_MODE_NORMAL;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        }

        case WM_SETFOCUS:
            // Klavye olaylarini almak icin focus yeterli
            return 0;

        case WM_MOUSEMOVE: {
            int mx = GET_X_LPARAM(lParam);
            int my = GET_Y_LPARAM(lParam);
            RECT plot;
            fan_graph_get_rects(hwnd, nullptr, &plot);

            if (g_fanCurveDialog.graphDragging) {
                int idx = g_fanCurveDialog.graphDragPointIdx;
                if (idx >= 0 && idx < FAN_CURVE_MAX_POINTS) {
                    // Calculate new temperature and fan%
                    int newTemp = fan_graph_x_to_temp(plot, mx);
                    int newPct  = fan_graph_y_to_pct (plot, my);

                    // Clamp to neighbours: temp must stay ordered, pct must not decrease
                    int minTemp = 0, maxTemp = 100;
                    int minPct  = 0, maxPct  = 100;
                    for (int i = idx - 1; i >= 0; i--) {
                        if (!g_fanCurveDialog.working.points[i].enabled) continue;
                        minTemp = g_fanCurveDialog.working.points[i].temperatureC + 1;
                        minPct  = g_fanCurveDialog.working.points[i].fanPercent;
                        break;
                    }
                    for (int i = idx + 1; i < FAN_CURVE_MAX_POINTS; i++) {
                        if (!g_fanCurveDialog.working.points[i].enabled) continue;
                        maxTemp = g_fanCurveDialog.working.points[i].temperatureC - 1;
                        maxPct  = g_fanCurveDialog.working.points[i].fanPercent;
                        break;
                    }
                    if (newTemp < minTemp) newTemp = minTemp;
                    if (newTemp > maxTemp) newTemp = maxTemp;
                    if (newPct  < minPct)  newPct  = minPct;
                    if (newPct  > maxPct)  newPct  = maxPct;

                    g_fanCurveDialog.working.points[idx].temperatureC = newTemp;
                    g_fanCurveDialog.working.points[idx].fanPercent   = newPct;

                    // Update edit boxes
                    if (g_fanCurveDialog.tempEdits[idx]) {
                        char buf[16] = {};
                        StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", newTemp);
                        SetWindowTextA(g_fanCurveDialog.tempEdits[idx], buf);
                    }
                    if (g_fanCurveDialog.percentEdits[idx]) {
                        char buf[16] = {};
                        StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", newPct);
                        SetWindowTextA(g_fanCurveDialog.percentEdits[idx], buf);
                    }
                    SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            } else {
                // Hover test
                int hitIdx = fan_graph_hit_test(plot, mx, my);
                if (hitIdx != g_fanCurveDialog.graphHoverIdx) {
                    g_fanCurveDialog.graphHoverIdx = hitIdx;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                if (hitIdx >= 0) {
                    SetCursor(LoadCursor(nullptr,
                        g_fanCurveDialog.graphMode == FAN_GRAPH_MODE_DELETE ? IDC_NO : IDC_SIZEALL));
                } else if (g_fanCurveDialog.graphMode == FAN_GRAPH_MODE_ADD) {
                    // Grafik alaninda mi?
                    RECT graph;
                    fan_graph_get_rects(hwnd, &graph, nullptr);
                    if (mx >= plot.left && mx <= plot.right && my >= plot.top && my <= plot.bottom)
                        SetCursor(LoadCursor(nullptr, IDC_CROSS));
                }
                // Start tracking (for WM_MOUSELEAVE)
                TRACKMOUSEEVENT tme = {};
                tme.cbSize    = sizeof(tme);
                tme.dwFlags   = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
            if (g_fanCurveDialog.graphHoverIdx != -1) {
                g_fanCurveDialog.graphHoverIdx = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            int mx = GET_X_LPARAM(lParam);
            int my = GET_Y_LPARAM(lParam);
            RECT graph, plot;
            fan_graph_get_rects(hwnd, &graph, &plot);

            // Only process if within graph area
            if (mx < graph.left || mx > graph.right || my < graph.top || my > graph.bottom)
                break;

            SetFocus(hwnd); // for keyboard events

            if (g_fanCurveDialog.graphMode == FAN_GRAPH_MODE_ADD) {
                // Is mouse within plot area?
                if (mx >= plot.left && mx <= plot.right && my >= plot.top && my <= plot.bottom) {
                    int newTemp = fan_graph_x_to_temp(plot, mx);
                    int newPct  = fan_graph_y_to_pct (plot, my);
                    // Find a free slot
                    int freeSlot = -1;
                    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
                        if (!g_fanCurveDialog.working.points[i].enabled) { freeSlot = i; break; }
                    }
                    if (freeSlot < 0) {
                        MessageBoxA(hwnd, "Maximum number of points reached.", "Green Curve", MB_OK | MB_ICONINFORMATION);
                    } else {
                        g_fanCurveDialog.working.points[freeSlot].enabled     = true;
                        g_fanCurveDialog.working.points[freeSlot].temperatureC = newTemp;
                        g_fanCurveDialog.working.points[freeSlot].fanPercent   = newPct;
                        fan_curve_normalize(&g_fanCurveDialog.working);
                        fan_curve_dialog_sync_controls();
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                }
                return 0;
            }

            int hitIdx = fan_graph_hit_test(plot, mx, my);

            if (g_fanCurveDialog.graphMode == FAN_GRAPH_MODE_DELETE) {
                if (hitIdx >= 0) {
                    FanCurveConfig candidate = g_fanCurveDialog.working;
                    candidate.points[hitIdx].enabled = false;
                    if (fan_curve_active_count(&candidate) < 2) {
                        MessageBoxA(hwnd, "At least two points must remain active.", "Green Curve", MB_OK | MB_ICONINFORMATION);
                    } else {
                        g_fanCurveDialog.working = candidate;
                        fan_curve_normalize(&g_fanCurveDialog.working);
                        fan_curve_dialog_sync_controls();
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                }
                return 0;
            }

            // Normal mode: drag
            if (hitIdx >= 0) {
                g_fanCurveDialog.graphDragging      = true;
                g_fanCurveDialog.graphDragPointIdx  = hitIdx;
                g_fanCurveDialog.graphDragAnchorX   = mx;
                g_fanCurveDialog.graphDragAnchorY   = my;
                g_fanCurveDialog.graphDragStartTemp = g_fanCurveDialog.working.points[hitIdx].temperatureC;
                g_fanCurveDialog.graphDragStartPct  = g_fanCurveDialog.working.points[hitIdx].fanPercent;
                SetCapture(hwnd);
                SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (g_fanCurveDialog.graphDragging) {
                g_fanCurveDialog.graphDragging     = false;
                g_fanCurveDialog.graphDragPointIdx = -1;
                ReleaseCapture();
                // Normalize: fix ordering, sync edit boxes
                fan_curve_normalize(&g_fanCurveDialog.working);
                fan_curve_dialog_sync_controls();
                SetCursor(LoadCursor(nullptr, IDC_ARROW));
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_SETCURSOR: {
            if (g_fanCurveDialog.graphDragging) {
                SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
                return TRUE;
            }
            break;
        }

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
            if (id >= FAN_DIALOG_TEMP_BASE && id < FAN_DIALOG_TEMP_BASE + FAN_CURVE_MAX_POINTS && notification == EN_KILLFOCUS) {
                fan_curve_dialog_sanitize_temperature_edit(id - FAN_DIALOG_TEMP_BASE);
                return 0;
            }
            if (id >= FAN_DIALOG_PERCENT_BASE && id < FAN_DIALOG_PERCENT_BASE + FAN_CURVE_MAX_POINTS && notification == EN_KILLFOCUS) {
                fan_curve_dialog_sanitize_percent_edit(id - FAN_DIALOG_PERCENT_BASE);
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
    g_fanCurveDialog.graphMode         = FAN_GRAPH_MODE_NORMAL;
    g_fanCurveDialog.graphDragging     = false;
    g_fanCurveDialog.graphDragPointIdx = -1;
    g_fanCurveDialog.graphHoverIdx     = -1;

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
