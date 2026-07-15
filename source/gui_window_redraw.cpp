// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "gui_window_redraw_policy.h"

struct GuiTopLevelRedrawTransaction {
    HWND hwnd;
    bool active;
    bool beganVisible;
    bool redrawDisabled;
};

static void gui_top_level_redraw_begin(
    GuiTopLevelRedrawTransaction* transaction, HWND hwnd,
    const char* reason) {
    if (!transaction) return;
    *transaction = {};
    if (!hwnd || !IsWindow(hwnd)) return;
    transaction->hwnd = hwnd;
    transaction->active = true;
    transaction->beganVisible = IsWindowVisible(hwnd) != FALSE;
    transaction->redrawDisabled = gui_top_level_redraw_uses_wm_setredraw(
        transaction->beganVisible);
    if (transaction->redrawDisabled)
        SendMessageA(hwnd, WM_SETREDRAW, FALSE, 0);
    debug_log("GUI redraw transaction: begin reason=%s initiallyVisible=%d toggled=%d hiddenIntent=%d\n",
        reason && reason[0] ? reason : "coherent projection",
        transaction->beganVisible ? 1 : 0,
        transaction->redrawDisabled ? 1 : 0,
        g_app.trayWindowHiddenIntent ? 1 : 0);
}

static void gui_top_level_redraw_end(
    GuiTopLevelRedrawTransaction* transaction, UINT visibleRedrawFlags,
    const char* reason) {
    if (!transaction || !transaction->active) return;
    HWND hwnd = transaction->hwnd;
    if (hwnd && IsWindow(hwnd)) {
        if (transaction->redrawDisabled)
            SendMessageA(hwnd, WM_SETREDRAW, TRUE, 0);
        bool mustRemainHidden = !transaction->beganVisible ||
            g_app.trayWindowHiddenIntent;
        if (mustRemainHidden && IsWindowVisible(hwnd)) {
            debug_log("GUI redraw transaction: corrected unexpected visibility before paint reason=%s initiallyVisible=%d hiddenIntent=%d\n",
                reason && reason[0] ? reason : "coherent projection",
                transaction->beganVisible ? 1 : 0,
                g_app.trayWindowHiddenIntent ? 1 : 0);
            ShowWindow(hwnd, SW_HIDE);
        }
        if (transaction->beganVisible && IsWindowVisible(hwnd)) {
            RedrawWindow(hwnd, nullptr, nullptr, visibleRedrawFlags);
        } else {
            // Do not synchronously paint a hidden top-level window. Mark the
            // parent and children dirty so the next explicit show is coherent.
            RedrawWindow(hwnd, nullptr, nullptr,
                RDW_INVALIDATE | RDW_ALLCHILDREN);
        }
        debug_log("GUI redraw transaction: end reason=%s initiallyVisible=%d toggled=%d finallyVisible=%d hiddenIntent=%d\n",
            reason && reason[0] ? reason : "coherent projection",
            transaction->beganVisible ? 1 : 0,
            transaction->redrawDisabled ? 1 : 0,
            IsWindowVisible(hwnd) ? 1 : 0,
            g_app.trayWindowHiddenIntent ? 1 : 0);
    }
    *transaction = {};
}
