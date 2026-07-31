// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "gui_window_redraw_policy.h"

struct GuiTopLevelRedrawTransaction {
    HWND hwnd;
    bool active;
    bool beganVisible;
};

// g_guiTopLevelRedrawDepth (defined in main_runtime_gpu.cpp, ahead of every
// shard that reads it) is the depth of the enclosing coherent projection
// transactions.  While it is non-zero the main window neither erases nor paints
// and every invalidation stays deferred; the outermost transaction issues the
// one settled repaint.
//
// This replaces the old top-level redraw-toggle pair.  That pair suppressed
// painting by clearing the window's own WS_VISIBLE bit, which the shell reads
// as "window hidden": the taskbar button disappeared for the length of every
// structural projection (a Refresh click, a GPU switch, the post-mutation
// reconciliation) and was re-created afterwards.  The source guard in
// tools/ui_gates.py keeps that message out of this file entirely; see
// gui_window_redraw_policy.h for the rule.

static void gui_top_level_redraw_begin(
    GuiTopLevelRedrawTransaction* transaction, HWND hwnd,
    const char* reason) {
    if (!transaction) return;
    *transaction = {};
    if (!hwnd || !IsWindow(hwnd)) return;
    transaction->hwnd = hwnd;
    transaction->active = true;
    // Reliable now that the transaction no longer clears the bit it reads.
    transaction->beganVisible = IsWindowVisible(hwnd) != FALSE;
    ++g_guiTopLevelRedrawDepth;
    // depth==1 is the outermost transaction, i.e. the one that will paint.
    debug_log("GUI redraw transaction: begin reason=%s initiallyVisible=%d depth=%d hiddenIntent=%d\n",
        reason && reason[0] ? reason : "coherent projection",
        transaction->beganVisible ? 1 : 0, g_guiTopLevelRedrawDepth,
        g_app.trayWindowHiddenIntent ? 1 : 0);
}

static void gui_top_level_redraw_end(
    GuiTopLevelRedrawTransaction* transaction, UINT visibleRedrawFlags,
    const char* reason) {
    if (!transaction || !transaction->active) return;
    HWND hwnd = transaction->hwnd;
    if (g_guiTopLevelRedrawDepth > 0) --g_guiTopLevelRedrawDepth;
    if (hwnd && IsWindow(hwnd)) {
        // The window's own visibility was never touched by the transaction, so
        // anything that changed it did so out of band; the durable tray-hidden
        // intent still outranks it and is reasserted before any paint.
        bool mustRemainHidden = !transaction->beganVisible ||
            g_app.trayWindowHiddenIntent;
        if (mustRemainHidden && IsWindowVisible(hwnd)) {
            debug_log("GUI redraw transaction: corrected unexpected visibility before paint reason=%s initiallyVisible=%d hiddenIntent=%d\n",
                reason && reason[0] ? reason : "coherent projection",
                transaction->beganVisible ? 1 : 0,
                g_app.trayWindowHiddenIntent ? 1 : 0);
            ShowWindow(hwnd, SW_HIDE);
        }
        bool paintNow = gui_top_level_redraw_may_paint_synchronously(
            transaction->beganVisible, g_app.trayWindowHiddenIntent) &&
            IsWindowVisible(hwnd) != FALSE &&
            !gui_top_level_paint_suppressed();
        if (paintNow) {
            RedrawWindow(hwnd, nullptr, nullptr, visibleRedrawFlags);
        } else {
            // Do not synchronously paint a hidden top-level window, and do not
            // paint from an inner transaction while an outer one still owns the
            // frame.  Mark the parent and children dirty so the next settled
            // repaint -- or the next explicit show -- is coherent.
            RedrawWindow(hwnd, nullptr, nullptr,
                RDW_INVALIDATE | RDW_ALLCHILDREN);
        }
        debug_log("GUI redraw transaction: end reason=%s initiallyVisible=%d depth=%d painted=%d finallyVisible=%d hiddenIntent=%d\n",
            reason && reason[0] ? reason : "coherent projection",
            transaction->beganVisible ? 1 : 0, g_guiTopLevelRedrawDepth,
            paintNow ? 1 : 0, IsWindowVisible(hwnd) ? 1 : 0,
            g_app.trayWindowHiddenIntent ? 1 : 0);
    }
    *transaction = {};
}
