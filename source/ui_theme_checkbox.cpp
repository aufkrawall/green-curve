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
        ? rc.left + dp(2)
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
        textRect.left = box.right + dp(8);
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
