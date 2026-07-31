// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Which windows a coherent projection transaction is allowed to redraw-toggle,
// and when it may paint synchronously.
//
// `DefWindowProc` implements `WM_SETREDRAW` by clearing (FALSE) and setting
// (TRUE) the target window's `WS_VISIBLE` bit -- that bit IS the redraw flag.
// On a CHILD control this is invisible to everything outside the process.  On a
// TOP-LEVEL window it is not: the shell derives the taskbar button, the Alt-Tab
// entry and its z-order bookkeeping from exactly that bit and reacts to a
// change of it as though the window had been hidden and shown again.  Two
// reported bugs came out of that one mechanism:
//
//   * the TRUE message resurrected a tray-hidden owner as a ghost window
//     (2026-07-15), and
//   * the FALSE message made an open window drop out of the taskbar window list
//     for the length of the transaction and reappear afterwards, which is
//     visible as a flicker and re-enters the shell's z-order/focus bookkeeping
//     (2026-07-31, reported for the Refresh button).
//
// The first was patched by skipping the toggle only while hidden.  That was
// half a fix: the rule is unconditional.  A top-level HWND is never
// redraw-toggled, visible or not.  Suppression is done instead by gating the
// window's own painting internally (`WM_PAINT` validates without drawing,
// `WM_ERASEBKGND` does not erase, invalidation stays deferred) and letting the
// transaction issue exactly one settled repaint when it ends.

#ifndef GREEN_CURVE_GUI_WINDOW_REDRAW_POLICY_H
#define GREEN_CURVE_GUI_WINDOW_REDRAW_POLICY_H

// WS_CHILD / WS_VISIBLE spelled out so this rule compiles and is asserted on
// either host; gui_window_redraw.cpp static_asserts them against windows.h.
#define GUI_REDRAW_STYLE_CHILD    0x40000000u
#define GUI_REDRAW_STYLE_VISIBLE  0x10000000u

// The invariant that the taskbar bug violated: only a child window's WS_VISIBLE
// bit is private to the process, so only a child may be redraw-toggled.
static inline bool gui_redraw_toggle_is_visibility_safe(unsigned int style) {
    return (style & GUI_REDRAW_STYLE_CHILD) != 0u;
}

// A transaction paints synchronously only over a window that was already on
// screen when it began and that is not meant to be living in the tray.
static inline bool gui_top_level_redraw_may_paint_synchronously(
    bool beganVisible, bool hiddenIntent) {
    return beganVisible && !hiddenIntent;
}

// While a projection transaction owns the frame, nothing else may force pixels
// out: an intermediate synchronous repaint is exactly the half-updated frame
// the transaction exists to prevent.  Invalidation is still recorded, so the
// settled repaint at the end (or the next explicit show) covers the region.
static inline bool gui_window_invalidation_must_defer(bool paintSuppressed,
    bool hiddenIntent, bool visible) {
    return paintSuppressed || hiddenIntent || !visible;
}

#endif // GREEN_CURVE_GUI_WINDOW_REDRAW_POLICY_H
