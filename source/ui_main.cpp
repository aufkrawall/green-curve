// ============================================================================
// UAC Elevation
// ============================================================================

static HBRUSH g_hBtnBr = nullptr;
static HBRUSH g_hInputBr = nullptr;
static HBRUSH g_hStaticBr = nullptr;
static HBRUSH g_hListBr = nullptr;
static HBRUSH g_hEditBr = nullptr;

static bool is_elevated() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
        return false;

    TOKEN_ELEVATION elev = {};
    DWORD size = 0;
    bool result = false;
    if (GetTokenInformation(hToken, TokenElevation, &elev, sizeof(elev), &size)) {
        result = elev.TokenIsElevated != 0;
    }
    CloseHandle(hToken);
    return result;
}

static void apply_system_titlebar_theme(HWND hwnd) {
    if (!hwnd) return;
    HMODULE d = load_system_library_a("dwmapi.dll");
    if (!d) return;
    typedef HRESULT (WINAPI *DwmSetWindowAttribute_t)(HWND, DWORD, LPCVOID, DWORD);
    auto setAttr = (DwmSetWindowAttribute_t)GetProcAddress(d, "DwmSetWindowAttribute");
    if (setAttr) {
        DWORD lightValue = 1;
        DWORD type = 0, size = sizeof(lightValue);
        LONG useDark = 0;
        HKEY hKey = nullptr;
        if (RegOpenKeyExA(HKEY_CURRENT_USER,
                "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            if (RegQueryValueExA(hKey, "AppsUseLightTheme", nullptr, (DWORD*)&type, (LPBYTE)&lightValue, &size) == ERROR_SUCCESS) {
                useDark = (lightValue == 0) ? 1 : 0;
            }
            RegCloseKey(hKey);
        }
        setAttr(hwnd, 20, &useDark, sizeof(useDark));
        setAttr(hwnd, 19, &useDark, sizeof(useDark));
    }
    FreeLibrary(d);
}

// ============================================================================
// GDI Graph Drawing
// ============================================================================

static void create_backbuffer(HWND hwnd) {
    destroy_backbuffer();
    HDC hdc = GetDC(hwnd);
    if (!hdc) return;
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (rc.right < 1) rc.right = 1;
    if (rc.bottom < 1) rc.bottom = 1;
    g_app.hMemDC = CreateCompatibleDC(hdc);
    if (!g_app.hMemDC) {
        ReleaseDC(hwnd, hdc);
        return;
    }
    g_app.hMemBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    if (!g_app.hMemBmp) {
        DeleteDC(g_app.hMemDC);
        g_app.hMemDC = nullptr;
        ReleaseDC(hwnd, hdc);
        return;
    }
    g_app.hOldBmp = (HBITMAP)SelectObject(g_app.hMemDC, g_app.hMemBmp);
    HBRUSH bg = CreateSolidBrush(COL_BG);
    FillRect(g_app.hMemDC, &rc, bg);
    DeleteObject(bg);
    ReleaseDC(hwnd, hdc);
}

static void fill_window_background(HWND hwnd, HDC hdc) {
    if (!hdc) return;
    RECT rc = {};
    if (!GetClientRect(hwnd, &rc)) return;
    HBRUSH brush = g_app.hWindowClassBrush ? g_app.hWindowClassBrush : (HBRUSH)GetStockObject(BLACK_BRUSH);
    FillRect(hdc, &rc, brush);
}

static void destroy_backbuffer() {
    if (g_app.hMemDC) {
        SelectObject(g_app.hMemDC, g_app.hOldBmp);
        if (g_app.hMemBmp) DeleteObject(g_app.hMemBmp);
        DeleteDC(g_app.hMemDC);
        g_app.hMemDC = nullptr;
        g_app.hMemBmp = nullptr;
        g_app.hOldBmp = nullptr;
    }
}

static void draw_graph(HDC hdc, RECT* rc) {
    int w = rc->right;
    int h = dp(GRAPH_HEIGHT);

    // Background
    HBRUSH bgBrush = CreateSolidBrush(COL_PANEL);
    RECT graphRc = {0, 0, w, h};
    FillRect(hdc, &graphRc, bgBrush);
    DeleteObject(bgBrush);

    if (!g_app.backgroundServiceAvailable) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, COL_TEXT);
        const char* msg = g_app.backgroundServiceInstalled
            ? "Background service not responding. Live controls disabled."
            : "Background service not installed. Live controls disabled.";
        TextOutA(hdc, w / 2 - dp(170), h / 2 - dp(8), msg, (int)strlen(msg));
        return;
    }

    if (!g_app.loaded) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, COL_TEXT);
        const char* msg = "Background service running, waiting for GPU snapshot...";
        TextOutA(hdc, w / 2 - dp(150), h / 2 - dp(8), msg, (int)strlen(msg));
        return;
    }

    // Axis ranges
    const int MIN_VOLT_mV = 700;
    const int MAX_VOLT_mV = 1250;
    const int MIN_FREQ_MHz = 500;
    const int MAX_FREQ_MHz = 3400;

    // DPI-scaled margins
    int ml = dp(70), mr = dp(30), mt = dp(35), mb = dp(55);
    int pw = w - ml - mr;
    int ph = h - mt - mb;

    // Helper: map voltage mV to X pixel
    auto volt_to_x = [&](unsigned int mv) -> int {
        if (mv < (unsigned)MIN_VOLT_mV) mv = MIN_VOLT_mV;
        if (mv > (unsigned)MAX_VOLT_mV) mv = MAX_VOLT_mV;
        return ml + (int)((long long)(mv - MIN_VOLT_mV) * pw / (MAX_VOLT_mV - MIN_VOLT_mV));
    };

    // Helper: map frequency MHz to Y pixel
    auto freq_to_y = [&](unsigned int mhz) -> int {
        if (mhz < (unsigned)MIN_FREQ_MHz) mhz = MIN_FREQ_MHz;
        if (mhz > (unsigned)MAX_FREQ_MHz) mhz = MAX_FREQ_MHz;
        return mt + ph - (int)((long long)(mhz - MIN_FREQ_MHz) * ph / (MAX_FREQ_MHz - MIN_FREQ_MHz));
    };

    // GDI objects
    HPEN gridPen = CreatePen(PS_SOLID, 1, COL_GRID);
    HPEN axisPen = CreatePen(PS_SOLID, 1, COL_AXIS);
    HPEN oldPen = (HPEN)SelectObject(hdc, gridPen);
    HFONT hFont = CreateFontA(dp(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT hFontSmall = CreateFontA(dp(11), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT oldFont = (HFONT)SelectObject(hdc, hFont);
    SetBkMode(hdc, TRANSPARENT);

    // Vertical grid lines (voltage axis, every 50mV, label every 100mV)
    for (int mv = MIN_VOLT_mV; mv <= MAX_VOLT_mV; mv += 50) {
            int x = volt_to_x((unsigned int)mv);
        SelectObject(hdc, gridPen);
        MoveToEx(hdc, x, mt, nullptr);
        LineTo(hdc, x, mt + ph);

        if (mv % 100 == 0) {
            SelectObject(hdc, hFontSmall);
            SetTextColor(hdc, COL_LABEL);
            char buf[16];
            StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", mv);
            SIZE sz;
            GetTextExtentPoint32A(hdc, buf, (int)strlen(buf), &sz);
            TextOutA(hdc, x - sz.cx / 2, mt + ph + dp(4), buf, (int)strlen(buf));
        }
    }

    // Horizontal grid lines (frequency axis, every 500MHz, label every 500MHz)
    for (int mhz = MIN_FREQ_MHz; mhz <= MAX_FREQ_MHz; mhz += 500) {
        int y = freq_to_y((unsigned int)mhz);
        SelectObject(hdc, gridPen);
        MoveToEx(hdc, ml, y, nullptr);
        LineTo(hdc, ml + pw, y);

        SelectObject(hdc, hFontSmall);
        SetTextColor(hdc, COL_LABEL);
        char buf[16];
        StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", mhz);
        SIZE sz;
        GetTextExtentPoint32A(hdc, buf, (int)strlen(buf), &sz);
        TextOutA(hdc, ml - sz.cx - dp(6), y - sz.cy / 2, buf, (int)strlen(buf));
    }

    // Axes
    SelectObject(hdc, axisPen);
    MoveToEx(hdc, ml, mt, nullptr);
    LineTo(hdc, ml, mt + ph);
    MoveToEx(hdc, ml, mt + ph, nullptr);
    LineTo(hdc, ml + pw, mt + ph);

    // Axis titles
    SelectObject(hdc, hFont);
    SetTextColor(hdc, COL_TEXT);
    const char* xTitle = "Voltage (mV)";
    SIZE sz;
    GetTextExtentPoint32A(hdc, xTitle, (int)strlen(xTitle), &sz);
    TextOutA(hdc, ml + pw / 2 - sz.cx / 2, mt + ph + dp(24), xTitle, (int)strlen(xTitle));

    const char* yTitle = "Frequency (MHz)";
    GetTextExtentPoint32A(hdc, yTitle, (int)strlen(yTitle), &sz);
    // Rotate for Y axis is hard in GDI, place horizontally left of Y labels
    TextOutA(hdc, dp(2), mt - dp(4), yTitle, (int)strlen(yTitle));

    // Build polyline: sort curve points by voltage, only plot within our ranges
    POINT pts[VF_NUM_POINTS];
    int nPts = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        unsigned int freq_mhz = displayed_curve_mhz(g_app.curve[i].freq_kHz);
        unsigned int volt_mv = g_app.curve[i].volt_uV / 1000;
        if (freq_mhz == 0 && volt_mv == 0) continue;
        // Only plot points within our visible range
        if (volt_mv < (unsigned)MIN_VOLT_mV || volt_mv > (unsigned)MAX_VOLT_mV) continue;
        if (freq_mhz < (unsigned)MIN_FREQ_MHz || freq_mhz > (unsigned)MAX_FREQ_MHz) continue;
        pts[nPts].x = volt_to_x(volt_mv);
        pts[nPts].y = freq_to_y(freq_mhz);
        nPts++;
    }

    if (nPts > 1) {
        draw_curve_polyline_smooth(hdc, pts, nPts, dp(2), COL_CURVE);
        SelectObject(hdc, oldPen);
    }

    draw_curve_points_ringed(hdc, pts, nPts, dp(2), dp(4));
    SelectObject(hdc, oldPen);

    // Frequency labels on curve (every 8 visible points)
    SelectObject(hdc, hFontSmall);
    SetTextColor(hdc, COL_TEXT);
    for (int i = 0; i < nPts; i += nvmax(1, nPts / 10)) {
        // Find original curve index for this point
        int visIdx = 0;
        for (int j = 0; j < VF_NUM_POINTS; j++) {
            unsigned int freq_mhz = displayed_curve_mhz(g_app.curve[j].freq_kHz);
            unsigned int volt_mv = g_app.curve[j].volt_uV / 1000;
            if (freq_mhz == 0 && volt_mv == 0) continue;
            if (volt_mv < (unsigned)MIN_VOLT_mV || volt_mv > (unsigned)MAX_VOLT_mV) continue;
            if (freq_mhz < (unsigned)MIN_FREQ_MHz || freq_mhz > (unsigned)MAX_FREQ_MHz) continue;
            if (visIdx == i) {
                char buf[32];
                StringCchPrintfA(buf, ARRAY_COUNT(buf), "%u", freq_mhz);
                SIZE sz2;
                GetTextExtentPoint32A(hdc, buf, (int)strlen(buf), &sz2);
                TextOutA(hdc, pts[i].x - sz2.cx / 2, pts[i].y - dp(16), buf, (int)strlen(buf));
                break;
            }
            visIdx++;
        }
    }

    // Info line at top
    SelectObject(hdc, hFont);
    SetTextColor(hdc, COL_TEXT);
    unsigned int actualMaxFreq = 0;
    unsigned int actualMaxVolt = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (g_app.curve[i].freq_kHz > actualMaxFreq) {
            actualMaxFreq = g_app.curve[i].freq_kHz;
            actualMaxVolt = g_app.curve[i].volt_uV;
        }
    }
    char info[512];
    StringCchPrintfA(info, ARRAY_COUNT(info), "%s  |  %d pts  |  Peak: %u MHz @ %u mV",
                     g_app.gpuName, g_app.numPopulated,
                     displayed_curve_mhz(actualMaxFreq), actualMaxVolt / 1000);
    TextOutA(hdc, ml + dp(6), dp(4), info, (int)strlen(info));

    // Cleanup
    SelectObject(hdc, oldFont);
    DeleteObject(hFont);
    DeleteObject(hFontSmall);
    DeleteObject(gridPen);
    DeleteObject(axisPen);
}

// ============================================================================
// Edit Controls
// ============================================================================

static void set_edit_value(HWND hEdit, unsigned int value) {
    char buf[16];
    StringCchPrintfA(buf, ARRAY_COUNT(buf), "%u", value);
    begin_programmatic_edit_update();
    SetWindowTextA(hEdit, buf);
    end_programmatic_edit_update();
}

static unsigned int get_edit_value(HWND hEdit) {
    char buf[16] = {};
    GetWindowTextA(hEdit, buf, sizeof(buf));
    return (unsigned int)strtoul(buf, nullptr, 10);
}

static void populate_edits() {
    bool preserveDirty = gui_state_dirty();
    populate_global_controls();
    bool serviceReady = g_app.backgroundServiceAvailable;
    begin_programmatic_edit_update();
    memset(g_app.guiCurvePointExplicit, 0, sizeof(g_app.guiCurvePointExplicit));
    for (int vi = 0; vi < g_app.numVisible; vi++) {
        int ci = g_app.visibleMap[vi];
        set_edit_value(g_app.hEditsMhz[vi], displayed_curve_mhz(g_app.curve[ci].freq_kHz));
        set_edit_value(g_app.hEditsMv[vi], g_app.curve[ci].volt_uV / 1000);
        SendMessageA(g_app.hEditsMhz[vi], EM_SETREADONLY, FALSE, 0);
        EnableWindow(g_app.hEditsMhz[vi], serviceReady ? TRUE : FALSE);
        SendMessageA(g_app.hEditsMv[vi], EM_SETREADONLY, TRUE, 0);
        EnableWindow(g_app.hEditsMv[vi], FALSE);
        SendMessageA(g_app.hLocks[vi], BM_SETCHECK, BST_UNCHECKED, 0);
        EnableWindow(g_app.hLocks[vi], serviceReady ? TRUE : FALSE);
        InvalidateRect(g_app.hLocks[vi], nullptr, FALSE);
    }
    // Re-apply lock state if active
    if (g_app.lockedVi >= 0 && g_app.lockedVi < g_app.numVisible) {
        SendMessageA(g_app.hLocks[g_app.lockedVi], BM_SETCHECK, BST_CHECKED, 0);
        set_edit_value(g_app.hEditsMhz[g_app.lockedVi], g_app.lockedFreq);
        for (int j = g_app.lockedVi + 1; j < g_app.numVisible; j++) {
            set_edit_value(g_app.hEditsMhz[j], g_app.lockedFreq);
            SendMessageA(g_app.hEditsMhz[j], EM_SETREADONLY, TRUE, 0);
            EnableWindow(g_app.hEditsMhz[j], FALSE);
            EnableWindow(g_app.hLocks[j], FALSE);
            InvalidateRect(g_app.hLocks[j], nullptr, FALSE);
        }
        if (!serviceReady) {
            EnableWindow(g_app.hEditsMhz[g_app.lockedVi], FALSE);
            EnableWindow(g_app.hLocks[g_app.lockedVi], FALSE);
        }
    }
    end_programmatic_edit_update();
    if (!preserveDirty && gui_state_dirty()) {
        debug_log("populate_edits: restoring clean GUI state after programmatic repaint\n");
        set_gui_state_dirty(false);
        populate_global_controls();
    }
}

static void apply_lock(int vi) {
    // Uncheck and re-enable previous lock
    if (g_app.lockedVi >= 0 && g_app.lockedVi < g_app.numVisible) {
        SendMessageA(g_app.hLocks[g_app.lockedVi], BM_SETCHECK, BST_UNCHECKED, 0);
        EnableWindow(g_app.hLocks[g_app.lockedVi], TRUE);
        InvalidateRect(g_app.hLocks[g_app.lockedVi], nullptr, FALSE);
    }

    // Check this one
    SendMessageA(g_app.hLocks[vi], BM_SETCHECK, BST_CHECKED, 0);
    g_app.lockedVi = vi;
    g_app.lockedCi = g_app.visibleMap[vi];
    g_app.lockedFreq = get_edit_value(g_app.hEditsMhz[vi]);
    g_app.guiLockTracksAnchor = true;
    if (!programmatic_edit_update_active()) {
        set_gui_state_dirty(true);
        record_ui_action("lock point %d @ %u MHz (track anchor)", g_app.lockedCi, g_app.lockedFreq);
    }
    EnableWindow(g_app.hLocks[vi], TRUE);
    InvalidateRect(g_app.hLocks[vi], nullptr, FALSE);

    // Set all subsequent MHz fields to locked value, make read-only, disable lock checkboxes
    for (int j = vi + 1; j < g_app.numVisible; j++) {
        set_edit_value(g_app.hEditsMhz[j], g_app.lockedFreq);
        SendMessageA(g_app.hEditsMhz[j], EM_SETREADONLY, TRUE, 0);
        EnableWindow(g_app.hEditsMhz[j], FALSE);
        EnableWindow(g_app.hLocks[j], FALSE);
        InvalidateRect(g_app.hLocks[j], nullptr, FALSE);
    }
}

static void sync_locked_tail_preview_from_anchor() {
    if (g_app.lockedVi < 0 || g_app.lockedVi >= g_app.numVisible) return;

    char buf[32] = {};
    get_window_text_safe(g_app.hEditsMhz[g_app.lockedVi], buf, sizeof(buf));

    int lockMhz = 0;
    if (!parse_int_strict(buf, &lockMhz) || lockMhz <= 0) return;

    g_app.lockedFreq = (unsigned int)lockMhz;
    if (g_app.lockedCi >= 0) g_app.guiCurvePointExplicit[g_app.lockedCi] = true;
    g_app.guiLockTracksAnchor = false;
    set_gui_state_dirty(true);
    if (g_app.lockedCi >= 0) record_ui_action("lock anchor point %d edited to %u MHz (absolute)", g_app.lockedCi, g_app.lockedFreq);
    for (int j = g_app.lockedVi + 1; j < g_app.numVisible; j++) {
        set_edit_value(g_app.hEditsMhz[j], g_app.lockedFreq);
    }
}

static void unlock_all() {
    begin_programmatic_edit_update();
    g_app.lockedVi = -1;
    g_app.lockedCi = -1;
    g_app.lockedFreq = 0;
    g_app.guiLockTracksAnchor = true;
    memset(g_app.guiCurvePointExplicit, 0, sizeof(g_app.guiCurvePointExplicit));
    set_gui_state_dirty(false);

    for (int vi = 0; vi < g_app.numVisible; vi++) {
        SendMessageA(g_app.hEditsMhz[vi], EM_SETREADONLY, FALSE, 0);
        EnableWindow(g_app.hEditsMhz[vi], TRUE);
        int ci = g_app.visibleMap[vi];
        set_edit_value(g_app.hEditsMhz[vi], displayed_curve_mhz(g_app.curve[ci].freq_kHz));
        SendMessageA(g_app.hLocks[vi], BM_SETCHECK, BST_UNCHECKED, 0);
        EnableWindow(g_app.hLocks[vi], TRUE);
        InvalidateRect(g_app.hLocks[vi], nullptr, FALSE);
    }
    end_programmatic_edit_update();
}

static void create_edit_controls(HWND hParent, HINSTANCE hInst) {
    begin_programmatic_edit_update();
    int cbW = dp(16);
    int editW = dp(65);
    int labelW = dp(32);
    int gap = dp(2);
    int rowH = dp(20);
    int headerH = dp(16);
    // Layout: [#] [☑] [MHz edit] [mV edit]
    int colW = labelW + cbW + editW + editW + gap * 3 + dp(8);
    int numCols = 6;
    int rowsPerCol = (g_app.numVisible + numCols - 1) / numCols;

    int graphH = dp(GRAPH_HEIGHT);
    int startY = graphH + dp(20);

    // Column headers
    for (int col = 0; col < numCols; col++) {
        int x = dp(8) + col * colW;
        int y = startY - headerH - dp(2);

        // Header: "Lk" over checkbox area, "MHz" over MHz edit, "mV" over mV edit
        CreateWindowExA(0, "STATIC", "Lk",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            x + labelW + gap, y, cbW, headerH,
            hParent, nullptr, hInst, nullptr);
        CreateWindowExA(0, "STATIC", "MHz",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            x + labelW + cbW + gap * 2, y, editW, headerH,
            hParent, nullptr, hInst, nullptr);
        CreateWindowExA(0, "STATIC", "mV",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            x + labelW + cbW + gap * 2 + editW + gap, y, editW, headerH,
            hParent, nullptr, hInst, nullptr);
    }

    for (int vi = 0; vi < g_app.numVisible; vi++) {
        int ci = g_app.visibleMap[vi];
        int col = vi / rowsPerCol;
        int row = vi % rowsPerCol;
        int x = dp(8) + col * colW;
        int y = startY + row * rowH;
        
        // Point label
        char label[16];
        StringCchPrintfA(label, ARRAY_COUNT(label), "%3d", ci);
        CreateWindowExA(0, "STATIC", label,
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            x, y + dp(1), labelW - gap, rowH - dp(2),
            hParent, nullptr, hInst, nullptr);

        // Lock checkbox (after point number)
        g_app.hLocks[vi] = CreateWindowExA(
            0, "BUTTON", "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            x + labelW, y + dp(1), cbW, rowH - dp(2),
            hParent, (HMENU)(INT_PTR)(LOCK_BASE_ID + vi), hInst, nullptr);

        // MHz edit
        g_app.hEditsMhz[vi] = CreateWindowExA(
            0, "EDIT", "0",
            WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_RIGHT | ES_AUTOHSCROLL,
            x + labelW + cbW + gap * 2, y, editW, rowH - dp(2),
            hParent, (HMENU)(INT_PTR)(1000 + vi), hInst, nullptr);

        // mV edit (read-only)
        g_app.hEditsMv[vi] = CreateWindowExA(
            0, "EDIT", "0",
            WS_CHILD | WS_VISIBLE | ES_RIGHT | ES_AUTOHSCROLL | ES_READONLY,
            x + labelW + cbW + gap * 2 + editW + gap, y, editW, rowH - dp(2),
            hParent, (HMENU)(INT_PTR)(1000 + VF_NUM_POINTS + vi), hInst, nullptr);
        style_input_control(g_app.hEditsMhz[vi]);
        style_input_control(g_app.hEditsMv[vi]);
    }

    // Global control fields below edits
    int ocY = layout_global_controls_y();
    int fieldW = dp(78);

    CreateWindowExA(0, "STATIC", "GPU Offset (MHz):",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        dp(8), ocY + dp(2), dp(126), dp(18),
        hParent, nullptr, hInst, nullptr);
    g_app.hGpuOffsetEdit = CreateWindowExA(
        0, "EDIT", "0",
        WS_CHILD | WS_VISIBLE | ES_RIGHT | ES_AUTOHSCROLL,
        dp(136), ocY, fieldW, dp(20),
        hParent, (HMENU)(INT_PTR)GPU_OFFSET_ID, hInst, nullptr);
    style_input_control(g_app.hGpuOffsetEdit);
    g_app.hGpuOffsetExcludeLowCheck = CreateWindowExA(
        0, "BUTTON", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        dp(8), ocY + dp(24), dp(16), dp(16),
        hParent, (HMENU)(INT_PTR)GPU_OFFSET_EXCLUDE_LOW_CHECK_ID, hInst, nullptr);
    g_app.hGpuOffsetExcludeLowLabel = CreateWindowExA(
        0, "STATIC", "Exclude from first 70 VF points",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOTIFY,
        dp(28), ocY + dp(23), dp(220), dp(18),
        hParent, (HMENU)(INT_PTR)GPU_OFFSET_EXCLUDE_LOW_LABEL_ID, hInst, nullptr);

    CreateWindowExA(0, "STATIC", "Mem Offset (MHz):",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        dp(230), ocY + dp(2), dp(126), dp(18),
        hParent, nullptr, hInst, nullptr);
    g_app.hMemOffsetEdit = CreateWindowExA(
        0, "EDIT", "0",
        WS_CHILD | WS_VISIBLE | ES_RIGHT | ES_AUTOHSCROLL,
        dp(358), ocY, fieldW, dp(20),
        hParent, (HMENU)(INT_PTR)MEM_OFFSET_ID, hInst, nullptr);
    style_input_control(g_app.hMemOffsetEdit);

    CreateWindowExA(0, "STATIC", "Power Limit (%):",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        dp(452), ocY + dp(2), dp(100), dp(18),
        hParent, nullptr, hInst, nullptr);
    g_app.hPowerLimitEdit = CreateWindowExA(
        0, "EDIT", "100",
        WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_RIGHT | ES_AUTOHSCROLL,
        dp(552), ocY, fieldW, dp(20),
        hParent, (HMENU)(INT_PTR)POWER_LIMIT_ID, hInst, nullptr);
    style_input_control(g_app.hPowerLimitEdit);

    CreateWindowExA(0, "STATIC", "Fan Control:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        dp(650), ocY + dp(2), dp(88), dp(18),
        hParent, nullptr, hInst, nullptr);
    g_app.hFanModeCombo = CreateWindowExA(
        0, "COMBOBOX", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
        dp(738), ocY, dp(136), dp(220),
        hParent, (HMENU)(INT_PTR)FAN_MODE_COMBO_ID, hInst, nullptr);
    style_combo_control(g_app.hFanModeCombo);
    SendMessageA(g_app.hFanModeCombo, CB_ADDSTRING, 0, (LPARAM)fan_mode_label(FAN_MODE_AUTO));
    SendMessageA(g_app.hFanModeCombo, CB_ADDSTRING, 0, (LPARAM)fan_mode_label(FAN_MODE_FIXED));
    SendMessageA(g_app.hFanModeCombo, CB_ADDSTRING, 0, (LPARAM)fan_mode_label(FAN_MODE_CURVE));
    SendMessageA(g_app.hFanModeCombo, CB_SETCURSEL, (WPARAM)g_app.guiFanMode, 0);

    CreateWindowExA(0, "STATIC", "Fixed %:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        dp(882), ocY + dp(2), dp(58), dp(18),
        hParent, nullptr, hInst, nullptr);
    g_app.hFanEdit = CreateWindowExA(
        0, "EDIT", "50",
        WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_RIGHT | ES_AUTOHSCROLL,
        dp(942), ocY, dp(56), dp(20),
        hParent, (HMENU)(INT_PTR)FAN_CONTROL_ID, hInst, nullptr);
    style_input_control(g_app.hFanEdit);
    g_app.hFanCurveBtn = CreateWindowExA(
        0, "BUTTON", "Edit Curve...",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        dp(1006), ocY - dp(1), dp(160), dp(24),
        hParent, (HMENU)(INT_PTR)FAN_CURVE_BTN_ID, hInst, nullptr);

    layout_bottom_buttons(hParent);

    style_combo_control(g_app.hProfileCombo);
    style_combo_control(g_app.hAppLaunchCombo);
    style_combo_control(g_app.hLogonCombo);
    apply_ui_font_to_children(hParent);

    populate_global_controls();

    if (g_app.loaded) populate_edits();

    refresh_profile_controls_from_config();
    end_programmatic_edit_update();
}

// ============================================================================
// Main Window
// ============================================================================

static void apply_changes() {
    if (!g_app.backgroundServiceAvailable || !g_app.loaded) return;
    set_pending_operation_source("GUI apply");
    record_ui_action("Apply clicked");
    DesiredSettings desired = {};
    char err[256] = {};
    if (g_app.usingBackgroundService && !refresh_service_snapshot_and_active_desired(err, sizeof(err))) {
        write_error_report_log_for_user_failure("GUI apply refresh failed", err);
        MessageBoxA(g_app.hMainWnd, err, "Green Curve", MB_OK | MB_ICONERROR);
        return;
    }
    if (!capture_gui_apply_settings(&desired, err, sizeof(err))) {
        write_error_report_log_for_user_failure("GUI apply validation failed", err);
        MessageBoxA(g_app.hMainWnd, err, "Green Curve", MB_OK | MB_ICONERROR);
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
    g_app.hGpuOffsetExcludeLowCheck = nullptr;
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
        destroy_edit_controls(g_app.hMainWnd);
        create_edit_controls(g_app.hMainWnd, g_app.hInst);
        update_background_service_controls();
        ensure_main_window_min_size(g_app.hMainWnd);
        invalidate_main_window();
        return;
    }
    if (g_app.usingBackgroundService && !g_app.isServiceProcess) {
        char detail[256] = {};
        ServiceSnapshot snapshot = {};
        if (service_client_get_snapshot(&snapshot, detail, sizeof(detail))) {
            apply_service_snapshot_to_app(&snapshot);
            destroy_edit_controls(g_app.hMainWnd);
            create_edit_controls(g_app.hMainWnd, g_app.hInst);
            update_background_service_controls();
            ensure_main_window_min_size(g_app.hMainWnd);
            invalidate_main_window();
        } else {
            refresh_background_service_state();
            clear_service_authoritative_state();
            debug_log("Green Curve: Failed to read service snapshot: %s\n", detail);
        }
        return;
    }
}

static void reset_curve() {
    if (!g_app.backgroundServiceAvailable || !g_app.loaded) return;

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
                static int s_serviceHealthTick = 0;
                if (++s_serviceHealthTick >= 5) {
                    s_serviceHealthTick = 0;
                    bool wasInstalled = g_app.backgroundServiceInstalled;
                    bool wasRunning = g_app.backgroundServiceRunning;
                    bool wasAvailable = g_app.backgroundServiceAvailable;
                    refresh_background_service_state();
                    if (wasInstalled != g_app.backgroundServiceInstalled ||
                        wasRunning != g_app.backgroundServiceRunning ||
                        wasAvailable != g_app.backgroundServiceAvailable) {
                        refresh_curve();
                    }
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
            } else if ((LOWORD(wParam) == GPU_OFFSET_EXCLUDE_LOW_CHECK_ID && HIWORD(wParam) == BN_CLICKED) ||
                       (LOWORD(wParam) == GPU_OFFSET_EXCLUDE_LOW_LABEL_ID && HIWORD(wParam) == STN_CLICKED)) {
                if (!g_app.backgroundServiceAvailable || !g_app.gpuOffsetRangeKnown) return 0;
                g_app.guiGpuOffsetExcludeLow70 = !g_app.guiGpuOffsetExcludeLow70;
                set_gui_state_dirty(true);
                bool checked = g_app.guiGpuOffsetExcludeLow70;
                SendMessageA(g_app.hGpuOffsetExcludeLowCheck, BM_SETCHECK, (WPARAM)(checked ? BST_CHECKED : BST_UNCHECKED), 0);
                InvalidateRect(g_app.hGpuOffsetExcludeLowCheck, nullptr, FALSE);
                record_ui_action("selective GPU offset %s", checked ? "enabled" : "disabled");
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
                bool enable = !g_app.backgroundServiceInstalled;
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
                    set_profile_status_text("Background service installed. Live OC, UV, power, and fan control is now available.");
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
                populate_desired_into_gui(&desired);
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

