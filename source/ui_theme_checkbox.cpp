// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Shared owner-drawn checkbox renderer used by the main window and auxiliary
// dialogs. Checked state stays with each caller's model/BM state.
static void draw_themed_checkbox_control(
    const DRAWITEMSTRUCT* dis, bool checked, bool labeledCheckbox) {
    if (!dis) return;
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool focused = (dis->itemState & ODS_FOCUS) != 0;
    HFONT controlFont = dis->hwndItem
        ? (HFONT)SendMessageA(dis->hwndItem, WM_GETFONT, 0, 0) : nullptr;
    HFONT oldFont = (HFONT)SelectObject(
        hdc, controlFont ? controlFont : get_ui_font());

    HBRUSH bg = CreateSolidBrush(COL_BG);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(hdc, TRANSPARENT);

    int controlW = rc.right - rc.left;
    int controlH = rc.bottom - rc.top;
    int boxSize = ui_theme_checkbox_box_size(controlW, controlH, g_dpi);
    int boxLeft = labeledCheckbox
        ? rc.left + ui_theme_checkbox_box_inset(g_dpi)
        : rc.left + (controlW - boxSize) / 2;
    RECT box = {
        boxLeft,
        rc.top + (controlH - boxSize) / 2,
        boxLeft + boxSize,
        rc.top + (controlH - boxSize) / 2 + boxSize,
    };

    COLORREF fill = disabled
        ? COL_BUTTON_DISABLED : (checked ? COL_BUTTON : COL_PANEL);
    COLORREF border = disabled ? RGB(0x5A, 0x5A, 0x68) : COL_BUTTON_BORDER;
    HBRUSH fillBrush = CreateSolidBrush(fill);
    FillRect(hdc, &box, fillBrush);
    DeleteObject(fillBrush);

    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, box.left, box.top, box.right + 1, box.bottom + 1);
    SelectObject(hdc, oldBrush);
    DeleteObject(SelectObject(hdc, oldPen));

    if (checked) {
        draw_checkbox_tick_smooth(
            hdc, &box, disabled ? COL_LABEL : RGB(0xE8, 0xF2, 0xFF));
    }

    if (labeledCheckbox) {
        char text[128] = {};
        GetWindowTextA(dis->hwndItem, text, ARRAY_COUNT(text));
        RECT textRect = rc;
        textRect.left = box.right + ui_theme_checkbox_label_gap(g_dpi);
        // A disabled label is drawn in the ordinary dimmed label colour, exactly
        // like every other greyed caption in the window.  Letting a STATIC do it
        // instead is what this replaced: user32 paints a disabled static in the
        // system grey-text colour and ignores the WM_CTLCOLORSTATIC palette, so
        // one caption came out foreign against the dark theme.
        SetTextColor(hdc, disabled ? COL_LABEL : COL_TEXT);
        DrawTextA(hdc, text, -1, &textRect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    if (focused) {
        RECT focus = rc;
        InflateRect(&focus, -3, -3);
        DrawFocusRect(hdc, &focus);
    }
    SelectObject(hdc, oldFont);
}

// F-CHECKBOX-HIT  Measure what ui_theme_labeled_checkbox_width() needs: the
// label's own extent in the control's font.  Returns 0 when there is no text or
// the measurement fails, so callers keep their static fallback width.
static int themed_checkbox_label_fit_width(HWND check, int controlHeight) {
    if (!check) return 0;
    char text[128] = {};
    int length = GetWindowTextA(check, text, ARRAY_COUNT(text));
    if (length <= 0) return 0;
    HDC dc = GetDC(check);
    if (!dc) {
        debug_log("checkbox fit: GetDC failed for hwnd=%p lastError=%lu\n",
            (void*)check, (unsigned long)GetLastError());
        return 0;
    }
    HFONT font = (HFONT)SendMessageA(check, WM_GETFONT, 0, 0);
    if (!font) font = get_ui_font();
    HFONT oldFont = font ? (HFONT)SelectObject(dc, font) : nullptr;
    SIZE extent = {};
    BOOL measured = GetTextExtentPoint32A(dc, text, length, &extent);
    if (oldFont) SelectObject(dc, oldFont);
    ReleaseDC(check, dc);
    if (!measured) {
        debug_log("checkbox fit: GetTextExtentPoint32 failed for hwnd=%p\n",
            (void*)check);
        return 0;
    }
    return ui_theme_labeled_checkbox_width(extent.cx, controlHeight, g_dpi);
}

// Resize a labeled checkbox in place so its clickable rectangle ends with its
// label.  Position and height are preserved, so this can follow either the
// layout pass or a label text change.
static void fit_themed_checkbox_to_label(HWND check) {
    if (!check) return;
    RECT rc = {};
    if (!GetWindowRect(check, &rc)) return;
    int height = rc.bottom - rc.top;
    int currentWidth = rc.right - rc.left;
    int width = themed_checkbox_label_fit_width(check, height);
    if (width <= 0 || width == currentWidth) return;
    debug_log("checkbox fit: hwnd=%p id=%ld width %d -> %d (h=%d dpi=%d)\n",
        (void*)check, (long)GetDlgCtrlID(check), currentWidth, width, height,
        g_dpi);
    SetWindowPos(check, nullptr, 0, 0, width, height,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}
