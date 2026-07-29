// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Themed replacement for MessageBox.  The stock message box is painted by
// user32 from system colors and offers no way to influence them, so every
// confirmation appeared as a bright light-mode window in front of the dark main
// window even with Windows itself set to dark.  This renders the same dialog
// with the application palette when Windows is in dark mode and a standard
// light palette when it is not, so the prompt follows the OS the way the rest
// of the window chrome already does via apply_system_titlebar_theme().
//
// Button sets, the default button, the Escape mapping, and the geometry live in
// the pure message_box_policy.h so they can be pinned without a window.

#include "message_box_policy.h"

enum { GC_MESSAGE_BOX_FIRST_BUTTON_ID = 0x4D42 };

struct GcMessageBoxPalette {
    COLORREF background;
    COLORREF text;
    COLORREF buttonFace;
    COLORREF buttonPressed;
    COLORREF buttonBorder;
    COLORREF buttonText;
};

struct GcMessageBoxState {
    MessageBoxButtonSet buttons;
    HWND button[MESSAGE_BOX_MAX_BUTTONS];
    HICON icon;
    int iconSize;
    const char* text;
    MessageBoxLayoutPlan plan;
    int textWidth;
    int textHeight;
    GcMessageBoxPalette palette;
    HFONT font;
    int result;
    bool done;
};

static GcMessageBoxPalette gc_message_box_palette() {
    GcMessageBoxPalette palette = {};
    if (is_system_dark_theme_active()) {
        palette.background = COL_BG;
        palette.text = COL_TEXT;
        palette.buttonFace = COL_BUTTON;
        palette.buttonPressed = COL_BUTTON_PRESSED;
        palette.buttonBorder = COL_BUTTON_BORDER;
        palette.buttonText = COL_BUTTON_LABEL;
    } else {
        // Deliberately close to the stock light dialog rather than a tinted
        // version of the dark palette: when Windows is light this should look
        // like every other confirmation on the system.
        palette.background = RGB(0xF0, 0xF0, 0xF0);
        palette.text = RGB(0x10, 0x10, 0x10);
        palette.buttonFace = RGB(0xE1, 0xE1, 0xE1);
        palette.buttonPressed = RGB(0xC4, 0xC4, 0xC4);
        palette.buttonBorder = RGB(0x8A, 0x8A, 0x8A);
        palette.buttonText = RGB(0x10, 0x10, 0x10);
    }
    return palette;
}

static HICON gc_message_box_icon(unsigned int type) {
    switch (type & GC_MB_ICONMASK) {
        case GC_MB_ICONERROR:       return LoadIconA(nullptr, IDI_ERROR);
        case GC_MB_ICONQUESTION:    return LoadIconA(nullptr, IDI_QUESTION);
        case GC_MB_ICONWARNING:     return LoadIconA(nullptr, IDI_WARNING);
        case GC_MB_ICONINFORMATION: return LoadIconA(nullptr, IDI_INFORMATION);
        default:                    return nullptr;
    }
}

static void gc_message_box_draw_button(const DRAWITEMSTRUCT* dis,
                                       const GcMessageBoxState* state) {
    if (!dis || !state) return;
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    bool focused = (dis->itemState & ODS_FOCUS) != 0;

    HBRUSH fill = CreateSolidBrush(pressed ? state->palette.buttonPressed
                                           : state->palette.buttonFace);
    FillRect(hdc, &rc, fill);
    DeleteObject(fill);

    // The focused button carries the thicker accent border Windows gives its
    // default button. Focus starts on the policy's default index, so the
    // recommended answer is visually obvious before anything is touched.
    HPEN border = CreatePen(PS_SOLID, focused ? 2 : 1, state->palette.buttonBorder);
    HPEN oldPen = (HPEN)SelectObject(hdc, border);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, (HBRUSH)GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, oldBrush);
    DeleteObject(SelectObject(hdc, oldPen));

    char label[64] = {};
    GetWindowTextA(dis->hwndItem, label, ARRAY_COUNT(label));
    HFONT oldFont = (HFONT)SelectObject(hdc, state->font ? state->font : get_ui_font());
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, state->palette.buttonText);
    RECT textRc = rc;
    if (pressed) OffsetRect(&textRc, 0, 1);
    DrawTextA(hdc, label, -1, &textRc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, oldFont);

    if (focused) {
        RECT focus = rc;
        InflateRect(&focus, -3, -3);
        DrawFocusRect(hdc, &focus);
    }
}

static void gc_message_box_paint(HWND hwnd, GcMessageBoxState* state) {
    PAINTSTRUCT ps = {};
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT client = {};
    GetClientRect(hwnd, &client);
    HBRUSH background = CreateSolidBrush(state->palette.background);
    FillRect(hdc, &client, background);
    DeleteObject(background);

    if (state->icon) {
        DrawIconEx(hdc, state->plan.iconX, state->plan.iconY, state->icon,
                   state->iconSize, state->iconSize, 0, nullptr, DI_NORMAL);
    }
    HFONT oldFont = (HFONT)SelectObject(hdc, state->font ? state->font : get_ui_font());
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, state->palette.text);
    RECT textRc = {
        state->plan.textX, state->plan.textY,
        state->plan.textX + state->textWidth,
        state->plan.textY + state->textHeight,
    };
    DrawTextA(hdc, state->text, -1, &textRc, DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(hdc, oldFont);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK GcMessageBoxProc(HWND hwnd, UINT msg, WPARAM wParam,
                                         LPARAM lParam) {
    auto* state = (GcMessageBoxState*)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;  // fully repainted in WM_PAINT; avoids a flash of white
        case WM_PAINT:
            if (state) {
                gc_message_box_paint(hwnd, state);
                return 0;
            }
            break;
        case WM_DRAWITEM:
            if (state) {
                gc_message_box_draw_button((const DRAWITEMSTRUCT*)lParam, state);
                return TRUE;
            }
            break;
        case DM_GETDEFID:
            // IsDialogMessage asks for this to route Enter to the default
            // button; without it Enter would do nothing.
            if (state && state->buttons.count > 0) {
                return MAKELRESULT(
                    GC_MESSAGE_BOX_FIRST_BUTTON_ID + state->buttons.defaultIndex,
                    DC_HASDEFID);
            }
            break;
        case WM_COMMAND:
            if (state && HIWORD(wParam) == BN_CLICKED) {
                int id = LOWORD(wParam);
                int index = id - GC_MESSAGE_BOX_FIRST_BUTTON_ID;
                if (index >= 0 && index < state->buttons.count) {
                    state->result = state->buttons.id[index];
                    state->done = true;
                    return 0;
                }
                // IsDialogMessage turns Escape into a synthetic IDCANCEL even
                // when no Cancel button exists; resolve it to the safe answer.
                if (id == GC_ID_CANCEL) {
                    state->result = state->buttons.escapeId;
                    state->done = true;
                    return 0;
                }
            }
            break;
        case WM_CLOSE:
            if (state) {
                state->result = state->buttons.escapeId;
                state->done = true;
                return 0;
            }
            break;
        default:
            break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

// Center on the owner (or the monitor under the cursor when there is none) and
// keep the whole frame inside that monitor's work area.
static void gc_message_box_place(HWND hwnd, HWND owner, int width, int height) {
    RECT anchor = {};
    if (owner && IsWindow(owner) && GetWindowRect(owner, &anchor)) {
        // anchor is the owner rect
    } else {
        POINT cursor = {};
        GetCursorPos(&cursor);
        anchor.left = cursor.x; anchor.top = cursor.y;
        anchor.right = cursor.x; anchor.bottom = cursor.y;
    }
    int x = anchor.left + ((anchor.right - anchor.left) - width) / 2;
    int y = anchor.top + ((anchor.bottom - anchor.top) - height) / 3;

    POINT center = { x + width / 2, y + height / 2 };
    HMONITOR monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (monitor && GetMonitorInfoA(monitor, &info)) {
        if (x + width > info.rcWork.right) x = info.rcWork.right - width;
        if (y + height > info.rcWork.bottom) y = info.rcWork.bottom - height;
        if (x < info.rcWork.left) x = info.rcWork.left;
        if (y < info.rcWork.top) y = info.rcWork.top;
    }
    SetWindowPos(hwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

int gc_message_box(HWND owner, const char* text, const char* caption,
                   unsigned int type) {
    GcMessageBoxState state = {};
    state.buttons = message_box_button_set(type);
    state.result = state.buttons.escapeId;
    state.text = text ? text : "";
    state.palette = gc_message_box_palette();
    state.font = get_ui_font();
    state.icon = gc_message_box_icon(type);
    state.iconSize = state.icon ? dp(32) : 0;

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = GcMessageBoxProc;
    wc.hInstance = g_app.hInst;
    wc.lpszClassName = "GreenCurveMessageBox";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = g_app.hMainWnd
        ? (HICON)SendMessageA(g_app.hMainWnd, WM_GETICON, ICON_SMALL, 0) : nullptr;
    WNDCLASSEXA existing = {};
    if (!GetClassInfoExA(g_app.hInst, wc.lpszClassName, &existing) &&
        !RegisterClassExA(&wc)) {
        debug_log("themed message box: class registration FAILED lastError=%lu; "
                  "falling back to the system message box\n",
                  (unsigned long)GetLastError());
        return MessageBoxA(owner, text, caption, type);
    }

    // Measure the body against a comfortable maximum line length, then let the
    // dialog size itself around the result.
    const int maxTextWidth = dp(420);
    HDC measureDc = GetDC(nullptr);
    HFONT oldFont = (HFONT)SelectObject(measureDc, state.font);
    RECT textRc = { 0, 0, maxTextWidth, 0 };
    DrawTextA(measureDc, state.text, -1, &textRc,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    state.textWidth = textRc.right - textRc.left;
    state.textHeight = textRc.bottom - textRc.top;
    int buttonWidth = dp(88);
    for (int i = 0; i < state.buttons.count; i++) {
        SIZE extent = {};
        const char* label = state.buttons.label[i];
        if (label && GetTextExtentPoint32A(measureDc, label, (int)strlen(label), &extent)) {
            int needed = extent.cx + dp(28);
            if (needed > buttonWidth) buttonWidth = needed;
        }
    }
    SelectObject(measureDc, oldFont);
    ReleaseDC(nullptr, measureDc);

    MessageBoxLayoutInput layout = {};
    layout.dpi = g_dpi;
    layout.hasIcon = state.icon != nullptr;
    layout.iconSize = state.iconSize;
    layout.textWidth = state.textWidth;
    layout.textHeight = state.textHeight;
    layout.buttonCount = state.buttons.count;
    layout.buttonWidth = buttonWidth;
    layout.buttonHeight = dp(28);
    state.plan = message_box_build_layout(&layout);

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
    RECT frame = { 0, 0, state.plan.clientWidth, state.plan.clientHeight };
    AdjustWindowRectEx(&frame, style, FALSE, exStyle);

    HWND hwnd = CreateWindowExA(exStyle, wc.lpszClassName,
        caption ? caption : "Green Curve", style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        frame.right - frame.left, frame.bottom - frame.top,
        owner, nullptr, g_app.hInst, nullptr);
    if (!hwnd) {
        debug_log("themed message box: window creation FAILED lastError=%lu; "
                  "falling back to the system message box\n",
                  (unsigned long)GetLastError());
        return MessageBoxA(owner, text, caption, type);
    }
    SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)&state);
    apply_system_titlebar_theme(hwnd);
    allow_dark_mode_for_window(hwnd);

    for (int i = 0; i < state.buttons.count; i++) {
        state.button[i] = CreateWindowExA(0, "BUTTON", state.buttons.label[i],
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            state.plan.firstButtonX + i * state.plan.buttonStride,
            state.plan.buttonY, buttonWidth, layout.buttonHeight,
            hwnd, (HMENU)(INT_PTR)(GC_MESSAGE_BOX_FIRST_BUTTON_ID + i),
            g_app.hInst, nullptr);
        if (state.button[i]) {
            SendMessageA(state.button[i], WM_SETFONT, (WPARAM)state.font, TRUE);
        }
    }

    gc_message_box_place(hwnd, owner, frame.right - frame.left,
                         frame.bottom - frame.top);
    bool ownerWasEnabled = owner && IsWindow(owner) && IsWindowEnabled(owner);
    if (ownerWasEnabled) EnableWindow(owner, FALSE);
    MessageBeep(type & GC_MB_ICONMASK);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    if (state.button[state.buttons.defaultIndex]) {
        SetFocus(state.button[state.buttons.defaultIndex]);
    }
    debug_log("themed message box shown: caption=\"%s\" buttons=%d default=%d "
              "escape=%d dark=%d\n",
        caption ? caption : "", state.buttons.count,
        state.buttons.defaultIndex, state.buttons.escapeId,
        is_system_dark_theme_active() ? 1 : 0);

    MSG msg = {};
    while (!state.done) {
        BOOL got = GetMessageA(&msg, nullptr, 0, 0);
        if (got == 0) {
            // The application is quitting.  Repost so the outer loop sees it,
            // and answer with the safe result rather than hanging here.
            PostQuitMessage((int)msg.wParam);
            break;
        }
        if (got == -1) break;
        if (!IsDialogMessageA(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    // Re-enable before destroying so focus returns to the owner rather than to
    // an arbitrary window behind it.
    if (ownerWasEnabled) EnableWindow(owner, TRUE);
    SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0);
    DestroyWindow(hwnd);
    if (ownerWasEnabled) SetActiveWindow(owner);
    debug_log("themed message box closed: result=%d\n", state.result);
    return state.result;
}
