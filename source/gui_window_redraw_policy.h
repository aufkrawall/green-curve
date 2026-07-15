// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_GUI_WINDOW_REDRAW_POLICY_H
#define GREEN_CURVE_GUI_WINDOW_REDRAW_POLICY_H

// DefWindowProc implements WM_SETREDRAW(TRUE) for a top-level HWND by adding
// WS_VISIBLE.  A hidden window therefore must not enter that toggle pair at
// all; it can accept control/model updates without painting and be invalidated
// asynchronously for its next explicit show.
static inline bool gui_top_level_redraw_uses_wm_setredraw(
    bool initiallyVisible) {
    return initiallyVisible;
}

#endif // GREEN_CURVE_GUI_WINDOW_REDRAW_POLICY_H
