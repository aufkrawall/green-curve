// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#pragma once

// Convert UTF-8 UI text to a NUL-terminated UTF-16 buffer for wide Win32 APIs.
// Returns the character count written (excluding NUL).  Truncates cleanly on
// oversized inputs so callers never depend on the process ANSI code page.
static inline int gui_utf8_to_wide_buf(const char* text, WCHAR* out, size_t outCount) {
    if (!out || outCount == 0) return 0;
    out[0] = L'\0';
    if (!text || !text[0]) return 0;
    int cap = (int)outCount;
    int written = MultiByteToWideChar(CP_UTF8, 0, text, -1, out, cap);
    if (written > 0) {
        out[outCount - 1] = L'\0';
        return written - 1;
    }
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && cap > 1) {
        int needed = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
        if (needed > 1) {
            WCHAR* tmp = (WCHAR*)HeapAlloc(GetProcessHeap(), 0, (size_t)needed * sizeof(WCHAR));
            if (tmp) {
                int full = MultiByteToWideChar(CP_UTF8, 0, text, -1, tmp, needed);
                int copyLen = (full > 0 && full - 1 < cap - 1) ? (full - 1) : (cap - 1);
                if (copyLen > 0) memcpy(out, tmp, (size_t)copyLen * sizeof(WCHAR));
                out[copyLen] = L'\0';
                HeapFree(GetProcessHeap(), 0, tmp);
                return copyLen;
            }
        }
    }
    return 0;
}

// Win32 setters can synchronously repaint owner-drawn children. Keep repeated
// state projections inert so background service probes cannot create flicker.
// Uses wide Win32 calls so UTF-8 text (e.g. °C) survives non-CP1252 locales.
static inline bool gui_set_window_text_if_changed(HWND hwnd, const char* text) {
    if (!hwnd) return false;
    const char* next = text ? text : "";
    WCHAR nextW[768] = {};
    const size_t cap = sizeof(nextW) / sizeof(nextW[0]);
    int nextLen = gui_utf8_to_wide_buf(next, nextW, cap);
    WCHAR currentW[768] = {};
    int length = GetWindowTextLengthW(hwnd);
    if (length == nextLen && length >= 0 && length < (int)cap) {
        GetWindowTextW(hwnd, currentW, (int)cap);
        if (wcscmp(currentW, nextW) == 0) return false;
    }
    SetWindowTextW(hwnd, nextW);
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
