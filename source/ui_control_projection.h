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

// There is deliberately no gui_set_button_check_if_changed() here.  Every
// checkbox in this program is BS_OWNERDRAW and derives its tick at paint time,
// so there is no native check state to read or write.  Gate those repaints with
// ui_checkbox_state_needs_repaint() against the AppState mirror the owner-draw
// handler writes (see ui_checkbox_state.h).
