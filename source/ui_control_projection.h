// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#pragma once

// Win32 setters can synchronously repaint owner-drawn children. Keep repeated
// state projections inert so background service probes cannot create flicker.
static inline bool gui_set_window_text_if_changed(HWND hwnd, const char* text) {
    if (!hwnd) return false;
    const char* next = text ? text : "";
    char current[768] = {};
    int length = GetWindowTextLengthA(hwnd);
    if (length >= 0 && length < (int)ARRAY_COUNT(current)) {
        GetWindowTextA(hwnd, current, ARRAY_COUNT(current));
        if (strcmp(current, next) == 0) return false;
    }
    SetWindowTextA(hwnd, next);
    return true;
}

static inline bool gui_set_window_enabled_if_changed(HWND hwnd, bool enabled) {
    if (!hwnd || (IsWindowEnabled(hwnd) != FALSE) == enabled) return false;
    EnableWindow(hwnd, enabled ? TRUE : FALSE);
    return true;
}

static inline bool gui_set_button_check_if_changed(HWND hwnd, bool checked) {
    if (!hwnd) return false;
    LRESULT next = checked ? BST_CHECKED : BST_UNCHECKED;
    if (SendMessageA(hwnd, BM_GETCHECK, 0, 0) == next) return false;
    SendMessageA(hwnd, BM_SETCHECK, (WPARAM)next, 0);
    return true;
}
