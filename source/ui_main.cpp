// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

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

    // GDI objects (cached in AppData to avoid churn across paint cycles)
    HPEN gridPen = g_app.hCachedGridPen ? g_app.hCachedGridPen : CreatePen(PS_SOLID, 1, COL_GRID);
    HPEN axisPen = g_app.hCachedAxisPen ? g_app.hCachedAxisPen : CreatePen(PS_SOLID, 1, COL_AXIS);
    HPEN oldPen = (HPEN)SelectObject(hdc, gridPen);
    HFONT hFont = g_app.hCachedFont ? g_app.hCachedFont : CreateFontA(dp(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT hFontSmall = g_app.hCachedFontSmall ? g_app.hCachedFontSmall : CreateFontA(dp(11), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
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
    if (!g_app.hCachedFont) DeleteObject(hFont);
    if (!g_app.hCachedFontSmall) DeleteObject(hFontSmall);
    if (!g_app.hCachedGridPen) DeleteObject(gridPen);
    if (!g_app.hCachedAxisPen) DeleteObject(axisPen);
}

// ============================================================================
// Edit Controls
// ============================================================================

static void populate_gpu_selector() {
    if (!g_app.hGpuSelectCombo) return;
    begin_programmatic_edit_update();
    SendMessageA(g_app.hGpuSelectCombo, CB_RESETCONTENT, 0, 0);
    unsigned int count = g_app.adapterCount;
    if (count == 0 && g_app.gpuName[0]) count = 1;
    for (unsigned int i = 0; i < count; i++) {
        char label[256] = {};
        if (i < g_app.adapterCount && g_app.adapters[i].valid) {
            format_gpu_adapter_label(&g_app.adapters[i], label, sizeof(label));
            if (g_app.adapters[i].vfBestGuess) {
                StringCchCatA(label, ARRAY_COUNT(label), " - experimental VF write");
            }
        } else {
            StringCchPrintfA(label, ARRAY_COUNT(label), "0: %s", g_app.gpuName[0] ? g_app.gpuName : "NVIDIA GPU");
        }
        SendMessageA(g_app.hGpuSelectCombo, CB_ADDSTRING, 0, (LPARAM)label);
    }
    unsigned int selected = g_app.selectedGpuIndex;
    if (selected >= count) selected = 0;
    SendMessageA(g_app.hGpuSelectCombo, CB_SETCURSEL, selected, 0);
    EnableWindow(g_app.hGpuSelectCombo, (count > 1 && g_app.backgroundServiceAvailable) ? TRUE : FALSE);
    end_programmatic_edit_update();
}

static void apply_gpu_selection_from_ui() {
    if (!g_app.hGpuSelectCombo || programmatic_edit_update_active()) return;
    LRESULT selection = SendMessageA(g_app.hGpuSelectCombo, CB_GETCURSEL, 0, 0);
    if (selection < 0 || selection >= MAX_GPU_ADAPTERS) return;
    unsigned int newIndex = (unsigned int)selection;
    if (newIndex == g_app.selectedGpuIndex && g_app.selectedGpuExplicit) return;
    set_config_int(g_app.configPath, "gpu", "selected_index", (int)newIndex);
    g_app.selectedGpuIndex = newIndex;
    g_app.selectedNvmlIndex = newIndex;
    g_app.selectedGpuExplicit = true;
    reset_gpu_runtime_selection();
    refresh_background_service_state();
    char detail[256] = {};
    refresh_global_state(detail, sizeof(detail));
    populate_global_controls();
    if (g_app.loaded) populate_edits();
    invalidate_main_window();
    set_profile_status_text("Selected GPU %u. Live state refreshed for that adapter.", newIndex);
    debug_log("gpu selection changed through GUI: index=%u detail=%s\n", newIndex, detail[0] ? detail : "ok");
}

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
    char* end = nullptr;
    unsigned long value = strtoul(buf, &end, 10);
    if (end && *end != '\0') return 0;
    return (unsigned int)value;
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
        InvalidateRect(g_app.hLocks[g_app.lockedVi], nullptr, FALSE);
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

    int gpuSelectW = dp(420);
    int gpuSelectX = dp(WINDOW_WIDTH) - gpuSelectW - dp(12);
    int gpuSelectY = dp(10);
    int gpuLabelW = dp(42);
    CreateWindowExA(0, "STATIC", "GPU:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        gpuSelectX - gpuLabelW - dp(6), gpuSelectY + dp(3), gpuLabelW, dp(18),
        hParent, nullptr, hInst, nullptr);
    g_app.hGpuSelectCombo = CreateWindowExA(
        0, "COMBOBOX", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
        gpuSelectX, gpuSelectY, gpuSelectW, dp(220),
        hParent, (HMENU)(INT_PTR)GPU_SELECT_COMBO_ID, hInst, nullptr);
    style_combo_control(g_app.hGpuSelectCombo);

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
    g_app.hGpuOffsetExcludeLowLabel = CreateWindowExA(
        0, "STATIC", "Exclude first",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        dp(8), ocY + dp(25), dp(76), dp(18),
        hParent, (HMENU)(INT_PTR)GPU_OFFSET_EXCLUDE_LOW_LABEL_ID, hInst, nullptr);
    g_app.hGpuOffsetExcludeLowEdit = CreateWindowExA(
        0, "EDIT", "0",
        WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_RIGHT | ES_AUTOHSCROLL,
        dp(86), ocY + dp(23), dp(50), dp(20),
        hParent, (HMENU)(INT_PTR)GPU_OFFSET_EXCLUDE_LOW_EDIT_ID, hInst, nullptr);
    style_input_control(g_app.hGpuOffsetExcludeLowEdit);
    CreateWindowExA(
        0, "STATIC", "VF points",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        dp(140), ocY + dp(25), dp(70), dp(18),
        hParent, nullptr, hInst, nullptr);

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

#include "ui_main_window.cpp"
