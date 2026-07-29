// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_GUI_RENDER_FORWARD_H
#define GREEN_CURVE_GUI_RENDER_FORWARD_H

// Forward declarations for GUI rendering helpers and for the F-PENDING
// presentation queries.
//
// Both groups are needed by shards that are amalgamated BEFORE the ones that
// define them: the curve renderers live in main_runtime_ui.cpp, the pending
// queries in ui_pending_changes.cpp, but draw_lock_checkbox (main_shell.cpp),
// draw_themed_button (main_runtime_ui.cpp), and the Apply enable gate
// (main_runtime_capture.cpp) all run earlier in the include order.
//
// The service binary has no UI; main_shell.cpp carries no-op stubs for the
// pending queries there.

static void draw_curve_polyline_smooth(HDC hdc, const POINT* pts, int count,
                                       int widthPx, COLORREF color,
                                       bool dashed = false);
static void draw_curve_points_ringed(HDC hdc, const POINT* pts, int count,
                                     int innerRadiusPx, int outerRadiusPx,
                                     COLORREF ringColor = COL_CURVE,
                                     COLORREF fillColor = COL_POINT);

static void gui_pending_changes_refresh();
static bool gui_pending_edit_is_changed(HWND control);
static bool gui_pending_curve_point_is_changed(int ci);
static bool gui_pending_domain_changed(unsigned int mask);
static const GuiPendingSummary* gui_pending_summary();
static bool gui_pending_gpu_offset_projection(int* appliedMHz,
                                              int* appliedExcludeLowCount,
                                              int* pendingMHz,
                                              int* pendingExcludeLowCount);

#endif // GREEN_CURVE_GUI_RENDER_FORWARD_H
