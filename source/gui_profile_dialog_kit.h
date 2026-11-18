// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
// Shared plumbing for the profile tool dialogs (import current GPU settings,
// import from file, export to file). They use the main window's themed
// buttons, checkboxes, combos and fonts, size to their content, center over
// the main window and keep it disabled while open (the auto-profile dialog's
// pattern). Included by gui_profile_tools.cpp inside the GUI translation unit.

struct GuiProfileDialogBrushes {
    HBRUSH edit = nullptr, label = nullptr, button = nullptr, list = nullptr;
};
static void gui_profile_dialog_release(GuiProfileDialogBrushes& brushes) {
    for (HBRUSH* brush : {&brushes.edit, &brushes.label, &brushes.button, &brushes.list})
        if (*brush) { DeleteObject(*brush); *brush = nullptr; }
}
// WM_CTLCOLOR* for the dark theme. `strong` statics (values, notes the user
// must read) use the text colour; captions use the dimmer label colour.
static LRESULT gui_profile_dialog_color(GuiProfileDialogBrushes& brushes, UINT msg, HDC dc, HWND control,
    bool strong, bool warning = false) {
    switch (msg) {
        case WM_CTLCOLOREDIT:
            SetTextColor(dc, COL_TEXT); SetBkColor(dc, COL_INPUT);
            if (!brushes.edit) brushes.edit = CreateSolidBrush(COL_INPUT);
            return (LRESULT)brushes.edit;
        case WM_CTLCOLORLISTBOX:
            SetTextColor(dc, COL_TEXT); SetBkColor(dc, COL_INPUT);
            if (!brushes.list) brushes.list = CreateSolidBrush(COL_INPUT);
            return (LRESULT)brushes.list;
        case WM_CTLCOLORBTN:
            SetTextColor(dc, COL_TEXT); SetBkColor(dc, COL_BG);
            if (!brushes.button) brushes.button = CreateSolidBrush(COL_BG);
            return (LRESULT)brushes.button;
        default:
            SetTextColor(dc, warning ? COL_PENDING : !IsWindowEnabled(control) ? COL_LABEL : strong ? COL_TEXT : COL_LABEL);
            SetBkColor(dc, COL_BG);
            if (!brushes.label) brushes.label = CreateSolidBrush(COL_BG);
            return (LRESULT)brushes.label;
    }
}
static HWND gui_profile_dialog_label(HWND parent, const char* text, int id = 0, DWORD extra = 0) {
    GcWideUtf8Arg wide(text ? text : "");
    HWND label = CreateWindowExW(0, L"STATIC", wide.value ? wide.value : L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | extra,
        0, 0, 1, 1, parent, (HMENU)(INT_PTR)id, g_app.hInst, nullptr);
    return label;
}
static void gui_profile_dialog_set_text(HWND control, const char* text) {
    if (!control) return;
    GcWideUtf8Arg wide(text ? text : "");
    SetWindowTextW(control, wide.value ? wide.value : L"");
}
static HWND gui_profile_dialog_button(HWND parent, const char* text, int id) {
    return CreateWindowExA(0, "BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 1, 1, parent, (HMENU)(INT_PTR)id, g_app.hInst, nullptr);
}
static HWND gui_profile_dialog_combo(HWND parent, int id) {
    HWND combo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
        0, 0, 1, dp(220), parent, (HMENU)(INT_PTR)id, g_app.hInst, nullptr);
    style_combo_control(combo);
    return combo;
}
static void gui_profile_dialog_combo_add(HWND combo, const char* text, LPARAM data) {
    GcWideUtf8Arg wide(text ? text : "");
    const LRESULT index = SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)(wide.value ? wide.value : L""));
    if (index >= 0) SendMessageW(combo, CB_SETITEMDATA, (WPARAM)index, data);
}
static LPARAM gui_profile_dialog_combo_data(HWND combo, LPARAM fallback) {
    const LRESULT index = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (index < 0) return fallback;
    const LRESULT data = SendMessageW(combo, CB_GETITEMDATA, (WPARAM)index, 0);
    return data == CB_ERR ? fallback : data;
}
static void gui_profile_dialog_combo_select(HWND combo, LPARAM data) {
    const int count = (int)SendMessageW(combo, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < count; ++i)
        if (SendMessageW(combo, CB_GETITEMDATA, (WPARAM)i, 0) == data) { SendMessageW(combo, CB_SETCURSEL, (WPARAM)i, 0); return; }
    if (count > 0) SendMessageW(combo, CB_SETCURSEL, 0, 0);
}
static void gui_profile_dialog_place(HWND control, int x, int y, int w, int h, bool visible = true) {
    if (!control) return;
    MoveWindow(control, dp(x), dp(y), dp(w), dp(h), FALSE);
    ShowWindow(control, visible ? SW_SHOWNA : SW_HIDE);
}
// Resize the dialog to its laid-out client height and repaint once.
static void gui_profile_dialog_fit(HWND hwnd, int clientW, int clientH) {
    if (!hwnd) return;
    SIZE size = adjusted_window_size_for_client(dp(clientW), dp(clientH), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        WS_EX_DLGMODALFRAME);
    RECT current{};
    GetWindowRect(hwnd, &current);
    RECT work = main_window_work_area(hwnd);
    RECT target = {current.left, current.top, current.left + size.cx, current.top + size.cy};
    target = clamp_window_rect_to_work_area(target, work);
    SetWindowPos(hwnd, nullptr, target.left, target.top, target.right - target.left, target.bottom - target.top,
        SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(hwnd, nullptr, TRUE);
}
static HWND gui_profile_dialog_create(const wchar_t* className, const char* title, WNDPROC proc, int clientW, int clientH) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = g_app.hInst;
    wc.lpszClassName = className;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_app.hWindowClassBrush;
    wc.hIcon = g_app.hMainWnd ? (HICON)SendMessageW(g_app.hMainWnd, WM_GETICON, ICON_SMALL, 0) : nullptr;
    WNDCLASSEXW existing{};
    existing.cbSize = sizeof(existing);
    if (!GetClassInfoExW(g_app.hInst, className, &existing) && !RegisterClassExW(&wc)) return nullptr;
    SIZE size = adjusted_window_size_for_client(dp(clientW), dp(clientH), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        WS_EX_DLGMODALFRAME);
    HWND owner = g_app.hMainWnd;
    RECT ownerRect{};
    if (owner) GetWindowRect(owner, &ownerRect);
    RECT work = owner ? main_window_work_area(owner) : main_window_work_area_for_rect(nullptr);
    MainLayoutRect anchor = main_layout_rect_from_win32(owner ? ownerRect : work);
    RECT target = clamp_window_rect_to_work_area(main_layout_rect_to_win32(
        main_layout_center_rect(anchor, size.cx, size.cy)), work);
    GcWideUtf8Arg wideTitle(title);
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, className, wideTitle.value ? wideTitle.value : L"Green Curve",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, target.left, target.top, target.right - target.left,
        target.bottom - target.top, owner, nullptr, g_app.hInst, nullptr);
    if (!hwnd) return nullptr;
    apply_system_titlebar_theme(hwnd);
    allow_dark_mode_for_window(hwnd);
    return hwnd;
}
// Show the dialog and disable the main window until it closes.
static void gui_profile_dialog_show(HWND hwnd, HWND focus) {
    if (!hwnd) return;
    apply_ui_font_to_children(hwnd);
    if (g_app.hMainWnd) EnableWindow(g_app.hMainWnd, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    if (focus) SetFocus(focus);
}
static void gui_profile_dialog_restore_owner() {
    if (!g_app.hMainWnd) return;
    EnableWindow(g_app.hMainWnd, TRUE);
    SetForegroundWindow(g_app.hMainWnd);
}
static void gui_profile_dialog_paint_background(HWND hwnd, HDC dc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    HBRUSH bg = CreateSolidBrush(COL_BG);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
}
// Keyboard: Esc closes, Enter activates the focused button (owner-drawn
// buttons never become the dialog manager's default), Tab moves focus.
static bool gui_profile_dialog_route(HWND dialog, MSG& message) {
    if (!dialog || (message.hwnd != dialog && !IsChild(dialog, message.hwnd))) return false;
    if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
        SendMessageW(dialog, WM_CLOSE, 0, 0); return true;
    }
    if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN) {
        HWND focus = GetFocus();
        wchar_t cls[16]{};
        if (focus && IsChild(dialog, focus) && IsWindowEnabled(focus) && GetClassNameW(focus, cls, 16) &&
            !lstrcmpiW(cls, L"Button")) {
            SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(focus), BN_CLICKED), (LPARAM)focus);
            return true;
        }
    }
    return IsDialogMessageW(dialog, &message) != FALSE;
}
// "Slot 3 - Empty" / "Slot 1 - Saved, applied at logon"
static std::string gui_profile_slot_label(int slot) {
    std::string label = "Slot " + std::to_string(slot) + " - ";
    if (!is_profile_slot_saved(g_app.configPath, slot)) return label + "Empty";
    label += "Saved";
    std::string roles;
    if (get_config_int(g_app.configPath, "profiles", "logon_slot", 0) == slot) roles += "logon";
    if (get_config_int(g_app.configPath, "profiles", "app_launch_slot", 0) == slot) roles += roles.empty() ? "app start" : " and app start";
    return roles.empty() ? label : label + ", used at " + roles;
}
