// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "gui_mutation_worker.cpp"
#include "gui_window_redraw.cpp"
#include "gui_service_state.cpp"
#include "gui_tray_visibility.cpp"
#include "gui_selected_gpu_pnp.cpp"

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

static int g_backbufferWidth = 0;
static int g_backbufferHeight = 0;
static gc_u64 g_guiGdiGeneration = 1;
static gc_u64 g_backbufferGeneration = 0;

static void create_backbuffer(HWND hwnd) {
    destroy_backbuffer();
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (rc.right < 1) rc.right = 1;
    if (rc.bottom < 1) rc.bottom = 1;

    // A compatible bitmap may remain backed by a retired display-driver
    // surface after an adapter disable/enable.  Keep the retained backbuffer
    // in process-owned system memory instead; the final BitBlt is the only
    // display-dependent operation and is checked on every paint.
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = rc.right;
    info.bmiHeader.biHeight = -rc.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    g_app.hMemBmp = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS,
        &pixels, nullptr, 0);
    if (!g_app.hMemBmp) {
        debug_log_on_change("GUI GDI: CreateDIBSection failed (error %lu)\n",
            GetLastError());
        return;
    }
    g_app.hMemDC = CreateCompatibleDC(nullptr);
    if (!g_app.hMemDC) {
        DeleteObject(g_app.hMemBmp);
        g_app.hMemBmp = nullptr;
        debug_log_on_change("GUI GDI: CreateCompatibleDC failed (error %lu)\n",
            GetLastError());
        return;
    }
    g_app.hOldBmp = (HBITMAP)SelectObject(g_app.hMemDC, g_app.hMemBmp);
    if (!g_app.hOldBmp || g_app.hOldBmp == (HBITMAP)HGDI_ERROR) {
        DeleteDC(g_app.hMemDC);
        DeleteObject(g_app.hMemBmp);
        g_app.hMemDC = nullptr;
        g_app.hMemBmp = nullptr;
        g_app.hOldBmp = nullptr;
        debug_log_on_change("GUI GDI: could not select DIB backbuffer (error %lu)\n",
            GetLastError());
        return;
    }
    g_backbufferWidth = rc.right;
    g_backbufferHeight = rc.bottom;
    g_backbufferGeneration = g_guiGdiGeneration;
    HBRUSH bg = CreateSolidBrush(COL_BG);
    FillRect(g_app.hMemDC, &rc, bg);
    DeleteObject(bg);
    debug_log("GUI GDI: created system-memory backbuffer %dx%d generation=%llu\n",
        g_backbufferWidth, g_backbufferHeight,
        (unsigned long long)g_guiGdiGeneration);
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
    g_backbufferWidth = 0;
    g_backbufferHeight = 0;
    g_backbufferGeneration = 0;
}

static void reset_gui_gdi_generation(const char* reason) {
    ++g_guiGdiGeneration;
    destroy_backbuffer();
    debug_log("GUI GDI: retired display generation; next paint rebuilds DIB generation=%llu (%s)\n",
        (unsigned long long)g_guiGdiGeneration,
        reason && reason[0] ? reason : "display transition");
    if (g_app.hMainWnd && IsWindowVisible(g_app.hMainWnd))
        RedrawWindow(g_app.hMainWnd, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ALLCHILDREN);
}

static unsigned int displayed_curve_mhz_for_gui_point(int ci) {
    if (ci < 0 || ci >= VF_NUM_POINTS) return 0;
    // Note: the locked tail override was removed after the uniform floor offset
    // fix (Build 109) eliminated tail drift. Real driver-reported values now
    // match the lock target, so no intent-vs-reality substitution is needed.
    if (g_app.guiDraft.attached && g_app.guiDraft.curveValueValid[ci])
        return g_app.guiDraft.curveMHz[ci];
    return displayed_curve_mhz(g_app.curve[ci].freq_kHz);
}

static void log_gui_locked_tail_display_drift_if_needed() {
    static int lastLockCi = -2;
    static unsigned int lastLockMHz = 0;
    static int lastDriftCount = -1;
    static int lastMaxDriftCi = -1;
    static unsigned int lastMaxDeltaMHz = 0;

    if (g_app.lockedCi < 0 || g_app.lockedCi >= VF_NUM_POINTS || g_app.lockedFreq == 0) {
        lastLockCi = -2;
        lastLockMHz = 0;
        lastDriftCount = -1;
        lastMaxDriftCi = -1;
        lastMaxDeltaMHz = 0;
        return;
    }

    int driftCount = 0;
    int maxDriftCi = -1;
    unsigned int maxDeltaMHz = 0;
    for (int ci = g_app.lockedCi; ci < VF_NUM_POINTS; ci++) {
        if (!is_curve_point_visible_in_gui(ci)) continue;
        if (g_app.curve[ci].freq_kHz == 0) continue;
        unsigned int liveMHz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
        unsigned int deltaMHz = liveMHz > g_app.lockedFreq
            ? liveMHz - g_app.lockedFreq
            : g_app.lockedFreq - liveMHz;
        if (deltaMHz <= 2) continue;
        driftCount++;
        if (deltaMHz > maxDeltaMHz) {
            maxDeltaMHz = deltaMHz;
            maxDriftCi = ci;
        }
    }

    if (driftCount > 0
        && (g_app.lockedCi != lastLockCi
            || g_app.lockedFreq != lastLockMHz
            || driftCount != lastDriftCount
            || maxDriftCi != lastMaxDriftCi
            || maxDeltaMHz != lastMaxDeltaMHz)) {
        debug_log("gui locked tail live readback drift: ci=%d lock=%u MHz drifted=%d max=ci%d/%uMHz temp=%d valid=%d\n",
            g_app.lockedCi,
            g_app.lockedFreq,
            driftCount,
            maxDriftCi,
            maxDeltaMHz,
            g_app.gpuTemperatureC,
            g_app.gpuTemperatureValid ? 1 : 0);
    }

    lastLockCi = g_app.lockedCi;
    lastLockMHz = g_app.lockedFreq;
    lastDriftCount = driftCount;
    lastMaxDriftCi = maxDriftCi;
    lastMaxDeltaMHz = maxDeltaMHz;
}

static void draw_graph(HDC hdc, RECT* rc) {
    (void)rc;
    int w = main_layout_content_width();
    int h = main_layout_graph_height();
    int savedDc = SaveDC(hdc);
    SetViewportOrgEx(hdc, -main_layout_scroll_x(), -main_layout_scroll_y(), nullptr);

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
        RestoreDC(hdc, savedDc);
        return;
    }

    if (!g_app.loaded) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, COL_TEXT);
        const char* msg = "Background service running, waiting for GPU snapshot...";
        TextOutA(hdc, w / 2 - dp(150), h / 2 - dp(8), msg, (int)strlen(msg));
        RestoreDC(hdc, savedDc);
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

    log_gui_locked_tail_display_drift_if_needed();

    // Build polyline: sort curve points by voltage, only plot within our ranges
    POINT pts[VF_NUM_POINTS];
    int nPts = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        unsigned int freq_mhz = displayed_curve_mhz_for_gui_point(i);
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
            unsigned int freq_mhz = displayed_curve_mhz_for_gui_point(j);
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

    // Info line at top: always show live peak from driver readback
    SelectObject(hdc, hFont);
    SetTextColor(hdc, COL_TEXT);
    char info[512];
    {
        unsigned int actualMaxFreq = 0;
        unsigned int actualMaxVolt = 0;
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            if (g_app.curve[i].freq_kHz > actualMaxFreq) {
                actualMaxFreq = g_app.curve[i].freq_kHz;
                actualMaxVolt = g_app.curve[i].volt_uV;
            }
        }
        if (g_app.lockedVi >= 0 && g_app.lockedCi >= 0 && g_app.lockedFreq > 0) {
            unsigned int lockVoltMv = g_app.curve[g_app.lockedCi].volt_uV / 1000;
            StringCchPrintfA(info, ARRAY_COUNT(info), "%s  |  %d pts  |  Lock: %u MHz @ %u mV  |  Live peak: %u MHz @ %u mV",
                             g_app.gpuName, g_app.numPopulated,
                             g_app.lockedFreq, lockVoltMv,
                             displayed_curve_mhz(actualMaxFreq), actualMaxVolt / 1000);
        } else {
            StringCchPrintfA(info, ARRAY_COUNT(info), "%s  |  %d pts  |  Peak: %u MHz @ %u mV",
                             g_app.gpuName, g_app.numPopulated,
                             displayed_curve_mhz(actualMaxFreq), actualMaxVolt / 1000);
        }
        static unsigned int lastPeakMHz = 0, lastPeakMv = 0;
        unsigned int peakMHz = displayed_curve_mhz(actualMaxFreq);
        unsigned int peakMv = actualMaxVolt / 1000;
        if (peakMHz != lastPeakMHz || peakMv != lastPeakMv) {
            lastPeakMHz = peakMHz;
            lastPeakMv = peakMv;
            debug_log("gui live peak: %u MHz @ %u mV\n", peakMHz, peakMv);
        }
    }
    TextOutA(hdc, ml + dp(6), dp(4), info, (int)strlen(info));

    // Cleanup
    SelectObject(hdc, oldFont);
    if (!g_app.hCachedFont) DeleteObject(hFont);
    if (!g_app.hCachedFontSmall) DeleteObject(hFontSmall);
    if (!g_app.hCachedGridPen) DeleteObject(gridPen);
    if (!g_app.hCachedAxisPen) DeleteObject(axisPen);
    RestoreDC(hdc, savedDc);
}

static void draw_gui_scene(HDC hdc, RECT* rc) {
    if (!hdc || !rc) return;
    HBRUSH bg = CreateSolidBrush(COL_BG);
    FillRect(hdc, rc, bg ? bg : (HBRUSH)GetStockObject(BLACK_BRUSH));
    if (bg) DeleteObject(bg);

    if (gui_service_model_ready(&g_app.guiServiceModel))
        draw_graph(hdc, rc);
    else
        gui_draw_service_overlay(hdc, rc);

    HPEN separator = CreatePen(PS_SOLID, 1, COL_GRID);
    if (!separator) return;
    HPEN previous = (HPEN)SelectObject(hdc, separator);
    if (previous && previous != (HPEN)HGDI_ERROR) {
        int graphH = main_layout_graph_height();
        int scrollX = main_layout_scroll_x();
        int scrollY = main_layout_scroll_y();
        MoveToEx(hdc, -scrollX, graphH - scrollY, nullptr);
        LineTo(hdc, main_layout_content_width() - scrollX,
            graphH - scrollY);
        SelectObject(hdc, previous);
    }
    DeleteObject(separator);
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
                StringCchCatA(label, ARRAY_COUNT(label), " - best-effort VF write");
            }
        } else {
            StringCchPrintfA(label, ARRAY_COUNT(label), "0: %s", g_app.gpuName[0] ? g_app.gpuName : "NVIDIA GPU");
        }
        SendMessageA(g_app.hGpuSelectCombo, CB_ADDSTRING, 0, (LPARAM)label);
    }
    unsigned int selected = g_app.selectedGpuIndex;
    if (selected >= count) selected = 0;
    SendMessageA(g_app.hGpuSelectCombo, CB_SETCURSEL,
        g_app.configuredGpuSelectionUnresolved ? (WPARAM)-1 : selected, 0);
    EnableWindow(g_app.hGpuSelectCombo,
        ((count > 1 || g_app.configuredGpuSelectionUnresolved) &&
         gui_service_model_ready(&g_app.guiServiceModel)) ? TRUE : FALSE);
    end_programmatic_edit_update();
}

static void apply_gpu_selection_from_ui() {
    if (!g_app.hGpuSelectCombo || programmatic_edit_update_active()) return;
    LRESULT selection = SendMessageA(g_app.hGpuSelectCombo, CB_GETCURSEL, 0, 0);
    if (selection < 0 || selection >= MAX_GPU_ADAPTERS) return;
    unsigned int newIndex = (unsigned int)selection;
    if (newIndex == g_app.selectedGpuIndex && g_app.selectedGpuExplicit &&
        !g_app.configuredGpuSelectionUnresolved) return;
    char persistErr[256] = {};
    if (!save_configured_gpu_selection_atomic(newIndex,
            persistErr, sizeof(persistErr))) {
        debug_log("gpu selection: persistence failed for index=%u: %s\n",
            newIndex, persistErr[0] ? persistErr : "unknown error");
        MessageBoxA(g_app.hMainWnd,
            persistErr[0] ? persistErr : "Failed to save the selected GPU.",
            "Green Curve", MB_OK | MB_ICONERROR);
        populate_gpu_selector();
        return;
    }
    g_app.selectedGpuIndex = newIndex;
    g_app.selectedNvmlIndex = newIndex;
    g_app.selectedGpuExplicit = true;
    g_app.configuredGpuSelectionUnresolved = false;
    gui_mutation_advance_gpu_epoch("GPU selector");
    reset_gpu_runtime_selection();
    gui_service_begin_full_sync("GPU selection changed");
    set_profile_status_text("Selected GPU %u. Synchronizing live state...", newIndex);
    debug_log("gpu selection changed through GUI: index=%u; asynchronous full sync queued\n",
        newIndex);
}

static void set_edit_value(HWND hEdit, unsigned int value) {
    char buf[16];
    StringCchPrintfA(buf, ARRAY_COUNT(buf), "%u", value);
    begin_programmatic_edit_update();
    SetWindowTextA(hEdit, buf);
    end_programmatic_edit_update();
}

static void populate_edits() {
    bool preserveDirty = gui_state_dirty();
    populate_global_controls();
    bool serviceReady = gui_service_model_ready(&g_app.guiServiceModel) &&
        g_app.guiDraft.attached && !g_app.guiDraft.detached;
    begin_programmatic_edit_update();
    memset(g_app.guiCurvePointExplicit, 0, sizeof(g_app.guiCurvePointExplicit));
    for (int vi = 0; vi < g_app.numVisible; vi++) {
        int ci = g_app.visibleMap[vi];
        // Owned points are shown from the drift-free applied-intent baseline, never
        // from live NVAPI readback. The VF curve legitimately drifts under boost/
        // temperature; that drift is telemetry only and must not surface as a
        // configured value in the editor, the graph (via guiCurvePointExplicit +
        // displayed_curve_mhz_for_gui_point), or a subsequent save. Stock/unowned
        // points still show live readback.
        unsigned int ownedMHz = (ci >= 0 && ci < VF_NUM_POINTS) ? g_app.appliedCurveMHz[ci] : 0;
        if (ownedMHz > 0) {
            g_app.guiCurvePointExplicit[ci] = true;
            set_edit_value(g_app.hEditsMhz[vi], ownedMHz);
        } else {
            set_edit_value(g_app.hEditsMhz[vi], displayed_curve_mhz(g_app.curve[ci].freq_kHz));
        }
        set_edit_value(g_app.hEditsMv[vi], g_app.curve[ci].volt_uV / 1000);
        SendMessageA(g_app.hEditsMhz[vi], EM_SETREADONLY, FALSE, 0);
        EnableWindow(g_app.hEditsMhz[vi], serviceReady ? TRUE : FALSE);
        SendMessageA(g_app.hEditsMv[vi], EM_SETREADONLY, TRUE, 0);
        EnableWindow(g_app.hEditsMv[vi], FALSE);
        SendMessageA(g_app.hLocks[vi], BM_SETCHECK, BST_UNCHECKED, 0);
        EnableWindow(g_app.hLocks[vi], serviceReady ? TRUE : FALSE);
        InvalidateRect(g_app.hLocks[vi], nullptr, FALSE);
    }
    // Re-apply lock state if active — show live driver values in edit boxes
    // but keep the tail points visually grayed out/disabled to indicate the
    // locked/flattened state. Live values are shown since the uniform tail
    // floor fix (Build 109) eliminated tail drift; real values equal lockedFreq.
    if (g_app.lockedVi >= 0 && g_app.lockedVi < g_app.numVisible) {
        SendMessageA(g_app.hLocks[g_app.lockedVi], BM_SETCHECK, BST_CHECKED, 0);
        InvalidateRect(g_app.hLocks[g_app.lockedVi], nullptr, FALSE);
        set_edit_value(g_app.hEditsMhz[g_app.lockedVi], g_app.lockedFreq);
        for (int j = g_app.lockedVi + 1; j < g_app.numVisible; j++) {
            // Show live readback value but visually disable to indicate locked state
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

static void apply_lock(int vi, LockMode mode) {
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
    g_app.lockMode = mode;
    if (g_app.lockedFreq == 0) {
        int ci = g_app.visibleMap[vi];
        if (ci >= 0 && ci < VF_NUM_POINTS &&
            g_app.guiDraft.curveValueValid[ci])
            g_app.lockedFreq = g_app.guiDraft.curveMHz[ci];
    }
    g_app.guiLockTracksAnchor = true;
    if (!programmatic_edit_update_active()) {
        set_gui_state_dirty(true);
        record_ui_action("lock point %d @ %u MHz (%s)", g_app.lockedCi, g_app.lockedFreq, lock_mode_name(mode));
    }
    EnableWindow(g_app.hLocks[vi], TRUE);
    InvalidateRect(g_app.hLocks[vi], nullptr, FALSE);

    // Disable tail edit boxes and lock checkboxes to indicate locked
    // state. Edit boxes keep their live readback values (not overwritten).
    for (int j = vi + 1; j < g_app.numVisible; j++) {
        SendMessageA(g_app.hEditsMhz[j], EM_SETREADONLY, TRUE, 0);
        EnableWindow(g_app.hEditsMhz[j], FALSE);
        EnableWindow(g_app.hLocks[j], FALSE);
        InvalidateRect(g_app.hLocks[j], nullptr, FALSE);
    }
}

static void sync_locked_tail_preview_from_anchor() {
    if (g_app.lockedVi < 0 || g_app.lockedVi >= g_app.numVisible) return;

    int ci = g_app.visibleMap[g_app.lockedVi];
    if (ci < 0 || ci >= VF_NUM_POINTS ||
        !g_app.guiDraft.curveValueValid[ci]) return;
    int lockMhz = (int)g_app.guiDraft.curveMHz[ci];

    g_app.lockedFreq = (unsigned int)lockMhz;
    if (g_app.lockedCi >= 0) g_app.guiCurvePointExplicit[g_app.lockedCi] = true;
    g_app.guiLockTracksAnchor = false;
    set_gui_state_dirty(true);
    if (g_app.lockedCi >= 0) record_ui_action("lock anchor point %d edited to %u MHz (absolute)", g_app.lockedCi, g_app.lockedFreq);
    // Note: previously overwrote tail edit boxes with lockedFreq here.
    // Removed after the uniform tail floor fix — real values match the lock target.
}

static void unlock_all() {
    // Note: NVML locked clocks reset is handled by the service apply pipeline,
    // not here — the GUI process does not own the NVML device handle.

    begin_programmatic_edit_update();
    g_app.lockedVi = -1;
    g_app.lockedCi = -1;
    g_app.lockedFreq = 0;
    g_app.lockMode = LOCK_MODE_NONE;
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

#include "ui_lock_checkbox.cpp"

// Create (or recreate) the hover tooltip that explains the tri-state lock
// checkboxes. The checkboxes are destroyed/recreated on service-state changes,
// so the tooltip and its registered tools are rebuilt alongside them. comctl32
// is loaded dynamically (the GUI does not link it), matching the TaskDialog path.
static void create_lock_tooltips(HWND hParent) {
    if (g_app.hLockTooltip) {
        DestroyWindow(g_app.hLockTooltip);
        g_app.hLockTooltip = nullptr;
    }
    HMODULE comctl = load_system_library_a("comctl32.dll");
    if (!comctl) return;
    static bool s_commonControlsInit = false;
    if (!s_commonControlsInit) {
        typedef BOOL (WINAPI *InitCommonControlsExFn)(const INITCOMMONCONTROLSEX*);
        auto initEx = (InitCommonControlsExFn)GetProcAddress(comctl, "InitCommonControlsEx");
        if (initEx) {
            INITCOMMONCONTROLSEX icc = {};
            icc.dwSize = sizeof(icc);
            icc.dwICC = ICC_BAR_CLASSES;  // registers tooltips_class32
            initEx(&icc);
        }
        s_commonControlsInit = true;
    }
    HWND tip = CreateWindowExA(WS_EX_TOPMOST, TOOLTIPS_CLASS, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        hParent, nullptr, hParent ? (HINSTANCE)GetWindowLongPtrA(hParent, GWLP_HINSTANCE) : g_app.hInst, nullptr);
    if (!tip) return;
    SendMessageA(tip, TTM_SETMAXTIPWIDTH, 0, (LPARAM)dp(320));
    SendMessageA(tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, (LPARAM)MAKELONG(15000, 0));
    static const char* kLockTip =
        "Lock this point's GPU clock.\r\n"
        "Left-click cycles: off -> flatten -> pin.\r\n"
        "Right-click to choose the mode directly.\r\n"
        "Flatten (check) caps the tail; Pin (dot) hard-locks via NVML.";
    for (int vi = 0; vi < g_app.numVisible; vi++) {
        if (!g_app.hLocks[vi]) continue;
        TOOLINFOA ti = {};
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = hParent;
        ti.uId = (UINT_PTR)g_app.hLocks[vi];
        ti.lpszText = (LPSTR)kLockTip;
        SendMessageA(tip, TTM_ADDTOOLA, 0, (LPARAM)&ti);
    }
    g_app.hLockTooltip = tip;
}

#include "ui_main_controls.cpp"

#include "ui_main_window.cpp"
#include "gui_process_cleanup.cpp"
