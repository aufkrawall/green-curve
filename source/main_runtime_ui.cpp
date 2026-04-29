static const char* ui_font_face_name() {
    return "Segoe UI";
}

static HFONT create_ui_sized_font(int heightPx, int weight) {
    LOGFONTA lf = {};
    if (s_uiBaseLogFontReady) {
        lf = s_uiBaseLogFont;
    } else {
        NONCLIENTMETRICSA ncm = {};
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
            lf = ncm.lfMessageFont;
        } else {
            lf.lfCharSet = DEFAULT_CHARSET;
            lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
            lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
            lf.lfPitchAndFamily = DEFAULT_PITCH | FF_SWISS;
            StringCchCopyA(lf.lfFaceName, ARRAY_COUNT(lf.lfFaceName), ui_font_face_name());
        }
    }

    lf.lfHeight = -nvmax(1, heightPx);
    lf.lfWeight = weight;
    lf.lfItalic = FALSE;
    lf.lfUnderline = FALSE;
    lf.lfStrikeOut = FALSE;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfOutPrecision = OUT_TT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_SWISS;
    StringCchCopyA(lf.lfFaceName, ARRAY_COUNT(lf.lfFaceName), ui_font_face_name());
    return CreateFontIndirectA(&lf);
}

static HFONT get_ui_font() {
    if (s_hUiFont) return s_hUiFont;

    NONCLIENTMETRICSA ncm = {};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        s_uiBaseLogFont = ncm.lfMessageFont;
        s_uiBaseLogFontReady = true;
    } else {
        memset(&s_uiBaseLogFont, 0, sizeof(s_uiBaseLogFont));
        s_uiBaseLogFont.lfHeight = -MulDiv(9, g_dpi, 72);
        s_uiBaseLogFont.lfWeight = FW_NORMAL;
        s_uiBaseLogFont.lfCharSet = DEFAULT_CHARSET;
        s_uiBaseLogFont.lfOutPrecision = OUT_TT_PRECIS;
        s_uiBaseLogFont.lfClipPrecision = CLIP_DEFAULT_PRECIS;
        s_uiBaseLogFont.lfQuality = CLEARTYPE_QUALITY;
        s_uiBaseLogFont.lfPitchAndFamily = DEFAULT_PITCH | FF_SWISS;
        StringCchCopyA(s_uiBaseLogFont.lfFaceName, ARRAY_COUNT(s_uiBaseLogFont.lfFaceName), ui_font_face_name());
        s_uiBaseLogFontReady = true;
    }

    s_hUiFont = create_ui_sized_font(dp(12), FW_NORMAL);
    return s_hUiFont;
}

static void apply_ui_font(HWND hwnd) {
    if (!hwnd) return;
    HFONT font = get_ui_font();
    if (!font) return;
    SendMessageA(hwnd, WM_SETFONT, (WPARAM)font, FALSE);
}

static void apply_ui_font_to_children(HWND parent) {
    if (!parent) return;
    HWND child = GetWindow(parent, GW_CHILD);
    while (child) {
        apply_ui_font(child);
        child = GetWindow(child, GW_HWNDNEXT);
    }
}

static int themed_combo_item_height() {
    return dp(22);
}

struct GdpStartupInput { UINT32 ver; void* debugCb; int suppressBg; int suppressExt; };
enum { GDP_SMOOTH_AA = 4, GDP_UNIT_PX = 2 };

typedef int  (WINAPI *GdpStartupFn)(ULONG_PTR*, const GdpStartupInput*, void*);
typedef void (WINAPI *GdpShutdownFn)(ULONG_PTR);
typedef int  (WINAPI *GdpGfxFromHDCFn)(HDC, void**);
typedef int  (WINAPI *GdpDelGfxFn)(void*);
typedef int  (WINAPI *GdpSetSmoothFn)(void*, int);
typedef int  (WINAPI *GdpMakePenFn)(unsigned int, float, int, void**);
typedef int  (WINAPI *GdpDelPenFn)(void*);
typedef int  (WINAPI *GdpDrawBeziersIFn)(void*, void*, const POINT*, int);
typedef int  (WINAPI *GdpDrawLinesIFn)(void*, void*, const POINT*, int);
typedef int  (WINAPI *GdpFillEllipseIFn)(void*, void*, int, int, int, int);
typedef int  (WINAPI *GdpDrawEllipseIFn)(void*, void*, int, int, int, int);
typedef int  (WINAPI *GdpMakeBrushFn)(unsigned int, void**);
typedef int  (WINAPI *GdpDelBrushFn)(void*);

static HMODULE       s_gdp_dll = nullptr;
static ULONG_PTR     s_gdp_token = 0;
static bool          s_gdp_tried = false;
static bool          s_gdp_ok = false;
static GdpGfxFromHDCFn  s_fnGfxHDC;
static GdpDelGfxFn      s_fnDelGfx;
static GdpSetSmoothFn   s_fnSmooth;
static GdpMakePenFn     s_fnMakePen;
static GdpDelPenFn      s_fnDelPen;
static GdpDrawBeziersIFn s_fnBeziers;
static GdpDrawLinesIFn   s_fnLines;
static GdpFillEllipseIFn s_fnFillEllipse;
static GdpDrawEllipseIFn s_fnDrawEllipse;
static GdpMakeBrushFn    s_fnMakeBrush;
static GdpDelBrushFn     s_fnDelBrush;
static GdpShutdownFn     s_fnShutdown;

static unsigned int colorref_to_argb(COLORREF c) {
    return 0xFF000000u | ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
}

static bool gdiplus_ensure() {
    if (s_gdp_tried) return s_gdp_ok;
    s_gdp_tried = true;
    s_gdp_dll = load_system_library_a("gdiplus.dll");
    if (!s_gdp_dll) return false;
    auto r = [](const char* n) -> void* { return (void*)GetProcAddress(s_gdp_dll, n); };
    GdpStartupFn startup = (GdpStartupFn)r("GdiplusStartup");
    s_fnShutdown    = (GdpShutdownFn)r("GdiplusShutdown");
    s_fnGfxHDC      = (GdpGfxFromHDCFn)r("GdipCreateFromHDC");
    s_fnDelGfx      = (GdpDelGfxFn)r("GdipDeleteGraphics");
    s_fnSmooth      = (GdpSetSmoothFn)r("GdipSetSmoothingMode");
    s_fnMakePen     = (GdpMakePenFn)r("GdipCreatePen1");
    s_fnDelPen      = (GdpDelPenFn)r("GdipDeletePen");
    s_fnBeziers     = (GdpDrawBeziersIFn)r("GdipDrawBeziersI");
    s_fnLines       = (GdpDrawLinesIFn)r("GdipDrawLinesI");
    s_fnFillEllipse = (GdpFillEllipseIFn)r("GdipFillEllipseI");
    s_fnDrawEllipse = (GdpDrawEllipseIFn)r("GdipDrawEllipseI");
    s_fnMakeBrush   = (GdpMakeBrushFn)r("GdipCreateSolidFill");
    s_fnDelBrush    = (GdpDelBrushFn)r("GdipDeleteBrush");
    if (!startup || !s_fnShutdown || !s_fnGfxHDC || !s_fnDelGfx || !s_fnSmooth ||
        !s_fnMakePen || !s_fnDelPen || !s_fnBeziers || !s_fnLines ||
        !s_fnFillEllipse || !s_fnDrawEllipse || !s_fnMakeBrush || !s_fnDelBrush)
        return false;
    GdpStartupInput inp = { 1, nullptr, 0, 0 };
    if (startup(&s_gdp_token, &inp, nullptr) != 0) return false;
    s_gdp_ok = true;
    return true;
}

static void shutdown_gdiplus() {
    if (s_gdp_ok && s_fnShutdown) s_fnShutdown(s_gdp_token);
    s_gdp_ok = false;
    s_gdp_token = 0;
    if (s_gdp_dll) { FreeLibrary(s_gdp_dll); s_gdp_dll = nullptr; }
}

static void draw_curve_polyline_smooth(HDC hdc, const POINT* pts, int count, int widthPx, COLORREF color) {
    if (!hdc || !pts || count < 2) return;
    if (gdiplus_ensure()) {
        void* gfx = nullptr;
        s_fnGfxHDC(hdc, &gfx);
        if (gfx) {
            s_fnSmooth(gfx, GDP_SMOOTH_AA);
            void* pen = nullptr;
            s_fnMakePen(colorref_to_argb(color), (float)nvmax(1, widthPx), GDP_UNIT_PX, &pen);
            if (pen) {
                if (count < 3) {
                    s_fnLines(gfx, pen, pts, count);
                } else {
                    POINT bez[VF_NUM_POINTS * 3] = {};
                    int bc = 0;
                    bez[bc++] = pts[0];
                    for (int i = 0; i < count - 1; i++) {
                        POINT p0 = (i > 0) ? pts[i - 1] : pts[i];
                        POINT p1 = pts[i];
                        POINT p2 = pts[i + 1];
                        POINT p3 = (i + 2 < count) ? pts[i + 2] : pts[i + 1];
                        int c1y = p1.y + (p2.y - p0.y) / 6;
                        int c2y = p2.y - (p3.y - p1.y) / 6;
                        int yLo = nvmin(p1.y, p2.y);
                        int yHi = nvmax(p1.y, p2.y);
                        if (c1y < yLo) c1y = yLo;
                        if (c1y > yHi) c1y = yHi;
                        if (c2y < yLo) c2y = yLo;
                        if (c2y > yHi) c2y = yHi;
                        bez[bc++] = { p1.x + (p2.x - p0.x) / 6, c1y };
                        bez[bc++] = { p2.x - (p3.x - p1.x) / 6, c2y };
                        bez[bc++] = p2;
                    }
                    s_fnBeziers(gfx, pen, bez, bc);
                }
                s_fnDelPen(pen);
            }
            s_fnDelGfx(gfx);
        }
        return;
    }
    HPEN pen = CreatePen(PS_SOLID, nvmax(1, widthPx), color);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    int oldMode = SetBkMode(hdc, TRANSPARENT);
    if (count < 3) {
        Polyline(hdc, pts, count);
    } else {
        POINT bez[VF_NUM_POINTS * 3] = {};
        int bc = 0;
        bez[bc++] = pts[0];
        for (int i = 0; i < count - 1; i++) {
            POINT p0 = (i > 0) ? pts[i - 1] : pts[i];
            POINT p1 = pts[i];
            POINT p2 = pts[i + 1];
            POINT p3 = (i + 2 < count) ? pts[i + 2] : pts[i + 1];
            int c1y = p1.y + (p2.y - p0.y) / 6;
            int c2y = p2.y - (p3.y - p1.y) / 6;
            int yLo = nvmin(p1.y, p2.y);
            int yHi = nvmax(p1.y, p2.y);
            if (c1y < yLo) c1y = yLo;
            if (c1y > yHi) c1y = yHi;
            if (c2y < yLo) c2y = yLo;
            if (c2y > yHi) c2y = yHi;
            bez[bc++] = { p1.x + (p2.x - p0.x) / 6, c1y };
            bez[bc++] = { p2.x - (p3.x - p1.x) / 6, c2y };
            bez[bc++] = p2;
        }
        PolyBezier(hdc, bez, bc);
    }
    SetBkMode(hdc, oldMode);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

static void draw_curve_points_ringed(HDC hdc, const POINT* pts, int count, int innerRadiusPx, int outerRadiusPx) {
    if (!hdc || !pts || count < 1) return;
    if (gdiplus_ensure()) {
        void* gfx = nullptr;
        s_fnGfxHDC(hdc, &gfx);
        if (gfx) {
            s_fnSmooth(gfx, GDP_SMOOTH_AA);
            void* ringPen = nullptr;
            s_fnMakePen(colorref_to_argb(COL_CURVE), 1.0f, GDP_UNIT_PX, &ringPen);
            void* fillBr = nullptr;
            s_fnMakeBrush(colorref_to_argb(COL_POINT), &fillBr);
            if (ringPen) {
                for (int i = 0; i < count; i++) {
                    s_fnDrawEllipse(gfx, ringPen,
                        pts[i].x - outerRadiusPx, pts[i].y - outerRadiusPx,
                        outerRadiusPx * 2 + 1, outerRadiusPx * 2 + 1);
                }
                s_fnDelPen(ringPen);
            }
            if (fillBr) {
                for (int i = 0; i < count; i++) {
                    s_fnFillEllipse(gfx, fillBr,
                        pts[i].x - innerRadiusPx, pts[i].y - innerRadiusPx,
                        innerRadiusPx * 2 + 1, innerRadiusPx * 2 + 1);
                }
                s_fnDelBrush(fillBr);
            }
            s_fnDelGfx(gfx);
        }
        return;
    }
    HBRUSH fillBrush = CreateSolidBrush(COL_POINT);
    HPEN ringPen = CreatePen(PS_SOLID, 1, COL_CURVE);
    HPEN oldPen = (HPEN)SelectObject(hdc, ringPen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    for (int i = 0; i < count; i++) {
        Ellipse(hdc,
            pts[i].x - outerRadiusPx,
            pts[i].y - outerRadiusPx,
            pts[i].x + outerRadiusPx + 1,
            pts[i].y + outerRadiusPx + 1);
    }
    SelectObject(hdc, fillBrush);
    for (int i = 0; i < count; i++) {
        Ellipse(hdc,
            pts[i].x - innerRadiusPx,
            pts[i].y - innerRadiusPx,
            pts[i].x + innerRadiusPx + 1,
            pts[i].y + innerRadiusPx + 1);
    }
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(fillBrush);
    DeleteObject(ringPen);
}

static void style_input_control(HWND hwnd) {
    if (!hwnd) return;
    LONG_PTR exStyle = GetWindowLongPtrA(hwnd, GWL_EXSTYLE);
    exStyle &= ~WS_EX_CLIENTEDGE;
    SetWindowLongPtrA(hwnd, GWL_EXSTYLE, exStyle);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    apply_ui_font(hwnd);
}

static void style_combo_control(HWND hwnd) {
    if (!hwnd) return;

    typedef HRESULT (WINAPI *set_window_theme_t)(HWND, LPCWSTR, LPCWSTR);
    static set_window_theme_t setWindowTheme = nullptr;
    static bool resolved = false;
    if (!resolved) {
        HMODULE ux = load_system_library_a("uxtheme.dll");
        if (ux) setWindowTheme = (set_window_theme_t)GetProcAddress(ux, "SetWindowTheme");
        resolved = true;
    }
    // Use DarkMode_CFD so WM_CTLCOLORSTATIC works for text
    if (setWindowTheme) {
        setWindowTheme(hwnd, is_system_dark_theme_active() ? L"DarkMode_CFD" : L"CFD", nullptr);
    }
    allow_dark_mode_for_window(hwnd);
    COMBOBOXINFO info = {};
    info.cbSize = sizeof(info);
    if (GetComboBoxInfo(hwnd, &info)) {
        if (info.hwndList) {
            allow_dark_mode_for_window(info.hwndList);
            apply_ui_font(info.hwndList);
        }
        if (info.hwndItem) {
            allow_dark_mode_for_window(info.hwndItem);
            apply_ui_font(info.hwndItem);
        }
    }
    install_themed_combo_subclass(hwnd);
    apply_ui_font(hwnd);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static bool is_themed_combo_id(UINT id) {
    return id == FAN_MODE_COMBO_ID || id == PROFILE_COMBO_ID || id == APP_LAUNCH_COMBO_ID ||
           id == LOGON_COMBO_ID || id == FAN_DIALOG_INTERVAL_ID || id == FAN_DIALOG_HYSTERESIS_ID;
}

static void paint_themed_combo_overlay(HWND hwnd, HDC hdc) {
    if (!hwnd || !hdc) return;
    COMBOBOXINFO info = {};
    info.cbSize = sizeof(info);
    if (!GetComboBoxInfo(hwnd, &info)) return;

    RECT client = {};
    GetClientRect(hwnd, &client);
    RECT buttonRc = info.rcButton;
    MapWindowPoints(nullptr, hwnd, (POINT*)&buttonRc, 2);
    bool disabled = !IsWindowEnabled(hwnd);
    bool dropped = SendMessageA(hwnd, CB_GETDROPPEDSTATE, 0, 0) != 0;

    // Ensure buttonRc has valid coordinates
    if (buttonRc.right <= buttonRc.left || buttonRc.bottom <= buttonRc.top) {
        buttonRc.left = client.right - dp(20);
        buttonRc.right = client.right;
        buttonRc.top = client.top;
        buttonRc.bottom = client.bottom;
    }

    // Paint over the button area with our theming
    HRGN hOldClip = CreateRectRgn(0, 0, 0, 0);
    int clipResult = GetClipRgn(hdc, hOldClip);
    HRGN hButtonRegion = CreateRectRgnIndirect(&buttonRc);
    SelectClipRgn(hdc, hButtonRegion);
    DeleteObject(hButtonRegion);

    // Fill button area with panel color (or pressed color when dropped)
    HBRUSH buttonBr = CreateSolidBrush(dropped ? COL_BUTTON_PRESSED : COL_PANEL);
    FillRect(hdc, &buttonRc, buttonBr);
    DeleteObject(buttonBr);

    // Draw separator line between text and button
    HPEN sepPen = CreatePen(PS_SOLID, 1, disabled ? RGB(0x56, 0x56, 0x64) : COL_BUTTON_BORDER);
    HPEN oldSepPen = (HPEN)SelectObject(hdc, sepPen);
    MoveToEx(hdc, buttonRc.left, buttonRc.top + 1, nullptr);
    LineTo(hdc, buttonRc.left, buttonRc.bottom - 1);
    SelectObject(hdc, oldSepPen);
    DeleteObject(sepPen);

    // Draw dropdown arrow triangle (theme-matched muted blue-gray)
    int centerX = buttonRc.left + (buttonRc.right - buttonRc.left) / 2;
    int centerY = buttonRc.top + (buttonRc.bottom - buttonRc.top) / 2;
    
    POINT tri[3] = {
        { centerX - dp(3), centerY - dp(2) },
        { centerX + dp(3), centerY - dp(2) },
        { centerX, centerY + dp(2) }
    };
    COLORREF arrowColor = disabled ? COL_LABEL : RGB(0xB0, 0xC0, 0xD0);
    HBRUSH arrowBr = CreateSolidBrush(arrowColor);
    HBRUSH oldArrowBrush = (HBRUSH)SelectObject(hdc, arrowBr);
    HPEN oldArrowPen = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
    Polygon(hdc, tri, 3);
    SelectObject(hdc, oldArrowPen);
    SelectObject(hdc, oldArrowBrush);
    DeleteObject(arrowBr);

    // Restore clipping
    if (clipResult == 1) {
        SelectClipRgn(hdc, hOldClip);
    } else {
        SelectClipRgn(hdc, nullptr);
    }
    DeleteObject(hOldClip);

    // Paint over Windows' default border with our themed border
    HBRUSH borderBrush = CreateSolidBrush(disabled ? RGB(0x56, 0x56, 0x64) : COL_BUTTON_BORDER);
    // Top edge
    RECT topEdge = { client.left, client.top, client.right, client.top + 1 };
    FillRect(hdc, &topEdge, borderBrush);
    // Bottom edge  
    RECT bottomEdge = { client.left, client.bottom - 1, client.right, client.bottom };
    FillRect(hdc, &bottomEdge, borderBrush);
    // Left edge
    RECT leftEdge = { client.left, client.top + 1, client.left + 1, client.bottom - 1 };
    FillRect(hdc, &leftEdge, borderBrush);
    // Right edge
    RECT rightEdge = { client.right - 1, client.top + 1, client.right, client.bottom - 1 };
    FillRect(hdc, &rightEdge, borderBrush);
    DeleteObject(borderBrush);
}

static void paint_themed_combo_full_custom(HWND hwnd, HDC hdc) {
    if (!hwnd || !hdc) return;
    COMBOBOXINFO info = {};
    info.cbSize = sizeof(info);
    if (!GetComboBoxInfo(hwnd, &info)) return;

    RECT client = {};
    GetClientRect(hwnd, &client);
    RECT buttonRc = info.rcButton;
    MapWindowPoints(nullptr, hwnd, (POINT*)&buttonRc, 2);
    bool disabled = !IsWindowEnabled(hwnd);
    bool dropped = SendMessageA(hwnd, CB_GETDROPPEDSTATE, 0, 0) != 0;

    // Ensure buttonRc has valid coordinates
    if (buttonRc.right <= buttonRc.left || buttonRc.bottom <= buttonRc.top) {
        buttonRc.left = client.right - dp(20);
        buttonRc.right = client.right;
        buttonRc.top = client.top;
        buttonRc.bottom = client.bottom;
    }

    // Fill entire background with input color first
    HBRUSH bgBrush = CreateSolidBrush(COL_INPUT);
    FillRect(hdc, &client, bgBrush);
    DeleteObject(bgBrush);

    // Fill button area with panel color (or pressed color when dropped)
    HBRUSH buttonBr = CreateSolidBrush(dropped ? COL_BUTTON_PRESSED : COL_PANEL);
    FillRect(hdc, &buttonRc, buttonBr);
    DeleteObject(buttonBr);

    // Draw the text
    int sel = (int)SendMessageA(hwnd, CB_GETCURSEL, 0, 0);
    char textBuf[256] = {};
    if (sel != CB_ERR) {
        SendMessageA(hwnd, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)textBuf);
    }

    if (textBuf[0]) {
        RECT textRc = client;
        textRc.left += dp(6);
        textRc.right = buttonRc.left - dp(4);
        textRc.top += 2;
        textRc.bottom -= 2;
        
        if (textRc.right > textRc.left) {
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, disabled ? COL_LABEL : COL_TEXT);
            
            HFONT uiFont = get_ui_font();
            HFONT oldFont = nullptr;
            if (uiFont) {
                oldFont = (HFONT)SelectObject(hdc, uiFont);
            }
            
            DrawTextA(hdc, textBuf, -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            
            if (oldFont) {
                SelectObject(hdc, oldFont);
            }
        }
    }

    // Draw separator line between text and button
    HPEN sepPen = CreatePen(PS_SOLID, 1, disabled ? RGB(0x56, 0x56, 0x64) : COL_BUTTON_BORDER);
    HPEN oldSepPen = (HPEN)SelectObject(hdc, sepPen);
    MoveToEx(hdc, buttonRc.left, buttonRc.top + 1, nullptr);
    LineTo(hdc, buttonRc.left, buttonRc.bottom - 1);
    SelectObject(hdc, oldSepPen);
    DeleteObject(sepPen);

    // Draw dropdown arrow triangle (theme-matched muted blue-gray)
    int centerX = buttonRc.left + (buttonRc.right - buttonRc.left) / 2;
    int centerY = buttonRc.top + (buttonRc.bottom - buttonRc.top) / 2;
    
    POINT tri[3] = {
        { centerX - dp(3), centerY - dp(2) },
        { centerX + dp(3), centerY - dp(2) },
        { centerX, centerY + dp(2) }
    };
    COLORREF arrowColor = disabled ? COL_LABEL : RGB(0xB0, 0xC0, 0xD0);
    HBRUSH arrowBr = CreateSolidBrush(arrowColor);
    HBRUSH oldArrowBrush = (HBRUSH)SelectObject(hdc, arrowBr);
    HPEN oldArrowPen = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
    Polygon(hdc, tri, 3);
    SelectObject(hdc, oldArrowPen);
    SelectObject(hdc, oldArrowBrush);
    DeleteObject(arrowBr);

    // Draw themed border around entire control
    HBRUSH borderBrush = CreateSolidBrush(disabled ? RGB(0x56, 0x56, 0x64) : COL_BUTTON_BORDER);
    // Top edge
    RECT topEdge = { client.left, client.top, client.right, client.top + 1 };
    FillRect(hdc, &topEdge, borderBrush);
    // Bottom edge  
    RECT bottomEdge = { client.left, client.bottom - 1, client.right, client.bottom };
    FillRect(hdc, &bottomEdge, borderBrush);
    // Left edge
    RECT leftEdge = { client.left, client.top + 1, client.left + 1, client.bottom - 1 };
    FillRect(hdc, &leftEdge, borderBrush);
    // Right edge
    RECT rightEdge = { client.right - 1, client.top + 1, client.right, client.bottom - 1 };
    FillRect(hdc, &rightEdge, borderBrush);
    DeleteObject(borderBrush);
}

static LRESULT CALLBACK themed_combo_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    WNDPROC original = (WNDPROC)GetPropA(hwnd, "GreenCurveComboOrigProc");
    if (!original) return DefWindowProcA(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_PAINT: {
            // Let Windows draw with its theming - we accept the default border/arrow
            // The WM_CTLCOLORSTATIC handler ensures text is readable
            LRESULT result = CallWindowProcA(original, hwnd, msg, wParam, lParam);
            return result;
        }

        case WM_ERASEBKGND:
            // Prevent default erasing to avoid flicker
            return 1;

        case WM_NCPAINT:
        case WM_ENABLE:
        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
        case CB_SHOWDROPDOWN:
        case WM_MOUSELEAVE:
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP: {
            LRESULT result = CallWindowProcA(original, hwnd, msg, wParam, lParam);
            InvalidateRect(hwnd, nullptr, FALSE);
            return result;
        }

        case WM_NCDESTROY:
            RemovePropA(hwnd, "GreenCurveComboOrigProc");
            SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)original);
            break;
    }

    return CallWindowProcA(original, hwnd, msg, wParam, lParam);
}

static void install_themed_combo_subclass(HWND hwnd) {
    if (!hwnd) return;
    if (GetPropA(hwnd, "GreenCurveComboOrigProc")) return;
    WNDPROC original = (WNDPROC)GetWindowLongPtrA(hwnd, GWLP_WNDPROC);
    if (!original) return;
    SetPropA(hwnd, "GreenCurveComboOrigProc", (HANDLE)original);
    SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)themed_combo_wndproc);
}

static void draw_themed_combo_item(const DRAWITEMSTRUCT* dis) {
    if (!dis || dis->CtlType != ODT_COMBOBOX) return;

    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    bool selected = (dis->itemState & ODS_SELECTED) != 0;
    bool focus = (dis->itemState & ODS_FOCUS) != 0;

    // Determine colors based on state
    COLORREF bgColor;
    COLORREF textColor;
    if (selected) {
        bgColor = COL_BUTTON;  // Use button color for selection
        textColor = RGB(0xF0, 0xF4, 0xFF);  // Bright text for selected
    } else {
        bgColor = COL_INPUT;  // Dark input background
        textColor = COL_TEXT;  // Normal text
    }

    // Fill background
    HBRUSH bgBrush = CreateSolidBrush(bgColor);
    FillRect(hdc, &rc, bgBrush);
    DeleteObject(bgBrush);

    // Draw focus rectangle if focused
    if (focus) {
        HPEN focusPen = CreatePen(PS_DOT, 1, COL_BUTTON_BORDER);
        HPEN oldPen = (HPEN)SelectObject(hdc, focusPen);
        HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(hdc, rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(focusPen);
    }

    // Get item text
    char text[256] = {};
    if (dis->itemID != (UINT)-1) {
        SendMessageA(dis->hwndItem, CB_GETLBTEXT, dis->itemID, (LPARAM)text);
    }

    // Draw text
    if (text[0]) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, textColor);
        HFONT font = (HFONT)SendMessageA(dis->hwndItem, WM_GETFONT, 0, 0);
        HFONT oldFont = (HFONT)SelectObject(hdc, font ? font : get_ui_font());
        RECT textRc = rc;
        textRc.left += dp(6);
        textRc.right -= dp(6);
        DrawTextA(hdc, text, -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(hdc, oldFont);
    }
}

static void measure_themed_combo_item(MEASUREITEMSTRUCT* mis) {
    if (!mis) return;
    // Set item height to match our themed controls
    mis->itemHeight = dp(18);
}

static void draw_checkbox_tick_smooth(HDC hdc, const RECT* box, COLORREF color) {
    if (!hdc || !box) return;

    POINT pts[3] = {
        { box->left + (box->right - box->left) * 22 / 100, box->top + (box->bottom - box->top) * 54 / 100 },
        { box->left + (box->right - box->left) * 44 / 100, box->top + (box->bottom - box->top) * 74 / 100 },
        { box->left + (box->right - box->left) * 78 / 100, box->top + (box->bottom - box->top) * 28 / 100 },
    };

    if (gdiplus_ensure()) {
        void* gfx = nullptr;
        s_fnGfxHDC(hdc, &gfx);
        if (gfx) {
            s_fnSmooth(gfx, GDP_SMOOTH_AA);
            void* pen = nullptr;
            float width = (float)nvmax(2, (box->right - box->left) / 5);
            s_fnMakePen(colorref_to_argb(color), width, GDP_UNIT_PX, &pen);
            if (pen) {
                s_fnLines(gfx, pen, pts, 3);
                s_fnDelPen(pen);
            }
            s_fnDelGfx(gfx);
            return;
        }
    }

    HPEN pen = CreatePen(PS_SOLID, nvmax(2, (box->right - box->left) / 5), color);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    MoveToEx(hdc, pts[0].x, pts[0].y, nullptr);
    LineTo(hdc, pts[1].x, pts[1].y);
    LineTo(hdc, pts[2].x, pts[2].y);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

static bool is_themed_button_id(UINT id) {
    switch (id) {
        case APPLY_BTN_ID:
        case REFRESH_BTN_ID:
        case RESET_BTN_ID:
        case LICENSE_BTN_ID:
        case PROFILE_LOAD_ID:
        case PROFILE_SAVE_ID:
        case PROFILE_CLEAR_ID:
        case FAN_CURVE_BTN_ID:
        case FAN_DIALOG_OK_ID:
        case FAN_DIALOG_CANCEL_ID:
            return true;
    }
    return false;
}

static bool is_themed_checkbox_id(UINT id) {
    return id == START_ON_LOGON_CHECK_ID || id == SERVICE_ENABLE_CHECK_ID || is_fan_dialog_checkbox_id(id);
}

static bool is_fan_dialog_checkbox_id(UINT id) {
    return id >= FAN_DIALOG_ENABLE_BASE && id < FAN_DIALOG_ENABLE_BASE + FAN_CURVE_MAX_POINTS;
}

static bool themed_checkbox_checked_state(UINT id, HWND hwnd) {
    if (id == START_ON_LOGON_CHECK_ID) return is_start_on_logon_enabled(g_app.configPath);
    if (id == SERVICE_ENABLE_CHECK_ID) return g_app.backgroundServiceInstalled;
    if (is_fan_dialog_checkbox_id(id)) {
        int pointIndex = (int)id - FAN_DIALOG_ENABLE_BASE;
        if (pointIndex >= 0 && pointIndex < FAN_CURVE_MAX_POINTS) {
            return g_fanCurveDialog.working.points[pointIndex].enabled;
        }
    }
    return hwnd && (SendMessageA(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED);
}

static void draw_themed_button(const DRAWITEMSTRUCT* dis) {
    if (!dis) return;

    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    bool focused = (dis->itemState & ODS_FOCUS) != 0;
    bool checkbox = is_themed_checkbox_id(dis->CtlID);
    bool checked = checkbox && themed_checkbox_checked_state(dis->CtlID, dis->hwndItem);
    HFONT controlFont = dis->hwndItem ? (HFONT)SendMessageA(dis->hwndItem, WM_GETFONT, 0, 0) : nullptr;
    HFONT oldFont = (HFONT)SelectObject(hdc, controlFont ? controlFont : get_ui_font());

    HBRUSH bg = CreateSolidBrush(COL_BG);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(hdc, TRANSPARENT);

    if (checkbox) {
        char text[64] = {};
        GetWindowTextA(dis->hwndItem, text, ARRAY_COUNT(text));
        bool labeledCheckbox = is_fan_dialog_checkbox_id(dis->CtlID) && text[0];
        int controlW = rc.right - rc.left;
        int controlH = rc.bottom - rc.top;
        int boxSize = labeledCheckbox
            ? nvmin(controlH - dp(4), dp(16))
            : nvmin(controlW, controlH) - dp(2);
        if (boxSize < dp(12)) boxSize = dp(12);
        if (boxSize > controlW) boxSize = controlW;
        if (boxSize > controlH) boxSize = controlH;
        int boxLeft = labeledCheckbox
            ? rc.left + dp(2)
            : rc.left + (controlW - boxSize) / 2;
        RECT box = {
            boxLeft,
            rc.top + (controlH - boxSize) / 2,
            boxLeft + boxSize,
            rc.top + (controlH - boxSize) / 2 + boxSize,
        };

        COLORREF fill = disabled ? COL_BUTTON_DISABLED : (checked ? COL_BUTTON : COL_PANEL);
        COLORREF border = disabled ? RGB(0x5A, 0x5A, 0x68) : COL_BUTTON_BORDER;
        HBRUSH fillBr = CreateSolidBrush(fill);
        FillRect(hdc, &box, fillBr);
        DeleteObject(fillBr);

        HPEN pen = CreatePen(PS_SOLID, 1, border);
        HPEN oldPen = (HPEN)SelectObject(hdc, pen);
        HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(hdc, box.left, box.top, box.right + 1, box.bottom + 1);
        SelectObject(hdc, oldBrush);
        DeleteObject(SelectObject(hdc, oldPen));

        if (checked) {
            draw_checkbox_tick_smooth(hdc, &box, disabled ? COL_LABEL : RGB(0xE8, 0xF2, 0xFF));
        }

        if (labeledCheckbox) {
            RECT textRc = rc;
            textRc.left = box.right + dp(8);
            SetTextColor(hdc, disabled ? COL_LABEL : COL_TEXT);
            DrawTextA(hdc, text, -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    } else {
        COLORREF fill = disabled ? COL_BUTTON_DISABLED : (pressed ? COL_BUTTON_PRESSED : COL_BUTTON);
        HBRUSH fillBr = CreateSolidBrush(fill);
        FillRect(hdc, &rc, fillBr);
        DeleteObject(fillBr);

        HPEN borderPen = CreatePen(PS_SOLID, 1, disabled ? RGB(0x56, 0x56, 0x64) : COL_BUTTON_BORDER);
        HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
        HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(hdc, oldBrush);
        DeleteObject(SelectObject(hdc, oldPen));

        char text[128] = {};
        GetWindowTextA(dis->hwndItem, text, ARRAY_COUNT(text));
        RECT textRc = rc;
        if (pressed) OffsetRect(&textRc, 0, 1);
        SetTextColor(hdc, disabled ? COL_LABEL : RGB(0xF0, 0xF4, 0xFF));
        DrawTextA(hdc, text, -1, &textRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    if (focused) {
        RECT focus = rc;
        InflateRect(&focus, -3, -3);
        DrawFocusRect(hdc, &focus);
    }
    SelectObject(hdc, oldFont);
}

