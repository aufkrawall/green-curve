// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Theme, DPI, and owner-drawn controls for the setup window.
//
// The setup window is painted with the application's own palette so the two
// look like one product, while the title bar follows the Windows light/dark
// setting — exactly the split the program's main window already uses.  Windows
// paints the caption itself and offers no way to colour it arbitrarily, so
// forcing it dark under a light system would produce a window that matches
// nothing on the desktop.
//
// Text sharpness at fractional scaling comes from three things together:
// per-monitor-v2 awareness (manifest), fonts built from the *window's* DPI via
// SystemParametersInfoForDpi rather than a scaled 96-DPI font, and
// CLEARTYPE_QUALITY.  Getting any one of them wrong is what makes an installer
// look blurry next to the program it installs.

#include "installer_common.h"
#include "installer_ui_internal.h"

bool gc_system_dark_theme_active() {
    DWORD value = 1;
    DWORD type = 0;
    DWORD size = sizeof(value);
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    bool dark = false;
    if (RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type, (LPBYTE)&value, &size) == ERROR_SUCCESS &&
        type == REG_DWORD) {
        dark = value == 0;
    }
    RegCloseKey(key);
    return dark;
}

void gc_apply_titlebar_theme(HWND hwnd) {
    if (!hwnd) return;
    HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dwm) return;
    typedef HRESULT(WINAPI * DwmSetWindowAttributeFn)(HWND, DWORD, LPCVOID, DWORD);
    auto setAttribute = (DwmSetWindowAttributeFn)GetProcAddress(dwm, "DwmSetWindowAttribute");
    if (setAttribute) {
        BOOL useDark = gc_system_dark_theme_active() ? TRUE : FALSE;
        // 20 is DWMWA_USE_IMMERSIVE_DARK_MODE on Windows 10 20H1 and later; 19
        // was the undocumented attribute on earlier builds.  Trying 20 first and
        // falling back keeps one code path correct on both.
        if (FAILED(setAttribute(hwnd, 20, &useDark, sizeof(useDark)))) {
            setAttribute(hwnd, 19, &useDark, sizeof(useDark));
        }
    }
    FreeLibrary(dwm);
}

UINT gc_dpi_for_window(HWND hwnd) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 && hwnd) {
        typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
        auto getDpiForWindow = (GetDpiForWindowFn)GetProcAddress(user32, "GetDpiForWindow");
        if (getDpiForWindow) {
            UINT dpi = getDpiForWindow(hwnd);
            if (dpi > 0) return dpi;
        }
    }
    HDC screen = GetDC(nullptr);
    UINT dpi = 96;
    if (screen) {
        int measured = GetDeviceCaps(screen, LOGPIXELSX);
        if (measured > 0) dpi = (UINT)measured;
        ReleaseDC(nullptr, screen);
    }
    return dpi;
}

int gc_scaled(UINT dpi, int logicalPixels) {
    return MulDiv(logicalPixels, (int)dpi, 96);
}

// Build the UI fonts for a specific DPI.
//
// SystemParametersInfoForDpi returns the message font already expressed in that
// DPI, so the result is the same typeface and metrics Windows itself would use
// on that monitor.  Scaling a 96-DPI LOGFONT by hand instead is what produces
// the slightly-wrong stem weights that read as "blurry installer".
void gc_create_theme_fonts(GcThemeFonts* fonts, UINT dpi) {
    if (!fonts) return;
    gc_destroy_theme_fonts(fonts);

    LOGFONTW base = {};
    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof(metrics);
    bool haveMetrics = false;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        typedef BOOL(WINAPI * SystemParametersInfoForDpiFn)(UINT, UINT, PVOID, UINT, UINT);
        auto forDpi = (SystemParametersInfoForDpiFn)GetProcAddress(user32, "SystemParametersInfoForDpi");
        if (forDpi) {
            haveMetrics = forDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi) != FALSE;
        }
    }
    if (!haveMetrics) {
        metrics.cbSize = sizeof(metrics);
        haveMetrics = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) != FALSE;
        if (haveMetrics) {
            // The non-DPI-aware call answers for the primary monitor, so this
            // one really does need manual scaling.
            metrics.lfMessageFont.lfHeight = MulDiv(metrics.lfMessageFont.lfHeight, (int)dpi,
                                                    (int)gc_dpi_for_window(nullptr));
        }
    }
    if (haveMetrics) {
        base = metrics.lfMessageFont;
    } else {
        base.lfHeight = -MulDiv(9, (int)dpi, 72);
        base.lfWeight = FW_NORMAL;
        base.lfCharSet = DEFAULT_CHARSET;
        base.lfOutPrecision = OUT_TT_PRECIS;
        base.lfClipPrecision = CLIP_DEFAULT_PRECIS;
        base.lfPitchAndFamily = DEFAULT_PITCH | FF_SWISS;
        StringCchCopyW(base.lfFaceName, GC_ARRAY_COUNT(base.lfFaceName), L"Segoe UI");
    }
    base.lfQuality = CLEARTYPE_QUALITY;
    fonts->body = CreateFontIndirectW(&base);

    LOGFONTW heading = base;
    heading.lfHeight = -MulDiv(16, (int)dpi, 72);
    heading.lfWeight = FW_SEMIBOLD;
    fonts->heading = CreateFontIndirectW(&heading);

    LOGFONTW small = base;
    small.lfHeight = -MulDiv(8, (int)dpi, 72);
    fonts->small = CreateFontIndirectW(&small);

    LOGFONTW monospace = base;
    monospace.lfHeight = -MulDiv(8, (int)dpi, 72);
    monospace.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    StringCchCopyW(monospace.lfFaceName, GC_ARRAY_COUNT(monospace.lfFaceName), L"Consolas");
    fonts->monospace = CreateFontIndirectW(&monospace);
    fonts->dpi = dpi;
}

void gc_destroy_theme_fonts(GcThemeFonts* fonts) {
    if (!fonts) return;
    if (fonts->body) DeleteObject(fonts->body);
    if (fonts->heading) DeleteObject(fonts->heading);
    if (fonts->small) DeleteObject(fonts->small);
    if (fonts->monospace) DeleteObject(fonts->monospace);
    fonts->body = nullptr;
    fonts->heading = nullptr;
    fonts->small = nullptr;
    fonts->monospace = nullptr;
}

// ---------------------------------------------------------------------------
// Owner-drawn controls
//
// The same shapes the application draws: a filled rounded-ish rectangle with a
// 1 px border (2 px when focused, which is how the default action announces
// itself), and a tick drawn as two strokes rather than a font glyph so it stays
// crisp at every scaling factor.
// ---------------------------------------------------------------------------

void gc_draw_themed_button(const DRAWITEMSTRUCT* item, HFONT font) {
    if (!item) return;
    HDC dc = item->hDC;
    RECT rect = item->rcItem;
    bool pressed = (item->itemState & ODS_SELECTED) != 0;
    bool disabled = (item->itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
    bool focused = (item->itemState & ODS_FOCUS) != 0;

    COLORREF face = disabled ? COL_BUTTON_DISABLED : (pressed ? COL_BUTTON_PRESSED : COL_BUTTON);
    HBRUSH fill = CreateSolidBrush(face);
    FillRect(dc, &rect, fill);
    DeleteObject(fill);

    HPEN border = CreatePen(PS_SOLID, focused && !disabled ? 2 : 1,
                            disabled ? COL_DISABLED_BORDER : COL_BUTTON_BORDER);
    HPEN oldPen = (HPEN)SelectObject(dc, border);
    HBRUSH oldBrush = (HBRUSH)SelectObject(dc, (HBRUSH)GetStockObject(HOLLOW_BRUSH));
    Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(dc, oldBrush);
    DeleteObject(SelectObject(dc, oldPen));

    WCHAR label[128] = {};
    GetWindowTextW(item->hwndItem, label, GC_ARRAY_COUNT(label));
    HFONT oldFont = (HFONT)SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? COL_LABEL : COL_BUTTON_LABEL);
    RECT textRect = rect;
    if (pressed) OffsetRect(&textRect, 0, 1);
    DrawTextW(dc, label, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, oldFont);
}

void gc_draw_themed_checkbox(const DRAWITEMSTRUCT* item, HFONT font, UINT dpi, bool checked) {
    if (!item) return;
    HDC dc = item->hDC;
    RECT rect = item->rcItem;
    bool disabled = (item->itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
    bool focused = (item->itemState & ODS_FOCUS) != 0;

    HBRUSH background = CreateSolidBrush(COL_BG);
    FillRect(dc, &rect, background);
    DeleteObject(background);

    int boxSize = gc_scaled(dpi, 14);
    RECT box = {};
    box.left = rect.left;
    box.top = rect.top + ((rect.bottom - rect.top) - boxSize) / 2;
    box.right = box.left + boxSize;
    box.bottom = box.top + boxSize;

    HBRUSH boxFill = CreateSolidBrush(disabled ? COL_BUTTON_DISABLED : COL_INPUT);
    FillRect(dc, &box, boxFill);
    DeleteObject(boxFill);
    HPEN boxPen = CreatePen(PS_SOLID, 1, disabled ? COL_DISABLED_BORDER : COL_BUTTON_BORDER);
    HPEN oldPen = (HPEN)SelectObject(dc, boxPen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(dc, (HBRUSH)GetStockObject(HOLLOW_BRUSH));
    Rectangle(dc, box.left, box.top, box.right, box.bottom);
    SelectObject(dc, oldBrush);
    DeleteObject(SelectObject(dc, oldPen));

    if (checked) {
        // Two strokes scaled to the box, so the tick keeps its proportions at
        // 100%, 125%, 150% and everything between.
        int thickness = gc_scaled(dpi, 2);
        if (thickness < 1) thickness = 1;
        HPEN tick = CreatePen(PS_SOLID, thickness, disabled ? COL_LABEL : COL_CURVE);
        HPEN previous = (HPEN)SelectObject(dc, tick);
        int left = box.left + boxSize / 4;
        int middle = box.left + boxSize / 2;
        int right = box.right - boxSize / 5;
        int top = box.top + boxSize / 4;
        int bottom = box.bottom - boxSize / 3;
        MoveToEx(dc, left, box.top + boxSize / 2, nullptr);
        LineTo(dc, middle, bottom);
        LineTo(dc, right, top);
        DeleteObject(SelectObject(dc, previous));
    }

    WCHAR label[256] = {};
    GetWindowTextW(item->hwndItem, label, GC_ARRAY_COUNT(label));
    RECT textRect = rect;
    textRect.left = box.right + gc_scaled(dpi, 8);
    HFONT oldFont = (HFONT)SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? COL_LABEL : COL_TEXT);
    DrawTextW(dc, label, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, oldFont);

    if (focused && !disabled) {
        RECT focus = textRect;
        DrawTextW(dc, label, -1, &focus, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
        focus.top = rect.top;
        focus.bottom = rect.bottom;
        focus.right += gc_scaled(dpi, 2);
        DrawFocusRect(dc, &focus);
    }
}

// A flat progress bar in the application palette.  The stock common control
// cannot be recoloured without theming tricks, and a two-rectangle bar is both
// simpler and sharper at fractional scaling.
void gc_draw_progress_bar(HDC dc, const RECT* bounds, int percent) {
    if (!dc || !bounds) return;
    RECT rect = *bounds;
    HBRUSH trough = CreateSolidBrush(COL_INPUT);
    FillRect(dc, &rect, trough);
    DeleteObject(trough);

    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    RECT filled = rect;
    filled.right = rect.left + MulDiv(rect.right - rect.left, percent, 100);
    if (filled.right > filled.left) {
        HBRUSH fill = CreateSolidBrush(COL_CURVE);
        FillRect(dc, &filled, fill);
        DeleteObject(fill);
    }
    HPEN border = CreatePen(PS_SOLID, 1, COL_GRID);
    HPEN oldPen = (HPEN)SelectObject(dc, border);
    HBRUSH oldBrush = (HBRUSH)SelectObject(dc, (HBRUSH)GetStockObject(HOLLOW_BRUSH));
    Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(dc, oldBrush);
    DeleteObject(SelectObject(dc, oldPen));
}

// ---------------------------------------------------------------------------
// Message box
// ---------------------------------------------------------------------------

// Silent runs and pre-window failures still need to say something.  MessageBoxW
// is used deliberately here rather than a hand-drawn dialog: it is always
// available, it is already DPI-correct, and these paths appear either never or
// exactly once at the end of a failed run, where matching the system dialog is
// more useful than matching the wizard.
void gc_show_message(HWND owner, const char* text, const char* caption, bool error) {
    WCHAR wideText[1024] = {};
    WCHAR wideCaption[128] = {};
    gc_utf8_to_wide(text ? text : "", wideText, (int)GC_ARRAY_COUNT(wideText));
    gc_utf8_to_wide(caption ? caption : GC_SETUP_PRODUCT_NAME, wideCaption,
                    (int)GC_ARRAY_COUNT(wideCaption));
    MessageBoxW(owner, wideText, wideCaption,
                MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION) | MB_SETFOREGROUND);
}
