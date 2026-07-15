// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// The tray-hidden state is an application presentation invariant.  Keep its
// enforcement independent from any one display/PnP notification source so a
// Windows visibility reconstruction is corrected by the first main-window
// message, even when that transition did not carry SWP_SHOWWINDOW.

static bool g_mainWindowTrayHideEnforcementActive = false;

static bool main_window_tray_hidden_invariant_violated() {
    return g_app.hMainWnd && gui_tray_hidden_intent_requires_rehide(
        g_app.trayWindowHiddenIntent,
        IsWindowVisible(g_app.hMainWnd) != FALSE,
        g_mainWindowTrayHideEnforcementActive);
}

static void enforce_main_window_tray_state(const char* reason) {
    if (!g_app.hMainWnd || !g_app.trayWindowHiddenIntent) return;
    ensure_tray_icon();
    bool unexpectedlyVisible = IsWindowVisible(g_app.hMainWnd) != FALSE;
    if (gui_tray_hidden_intent_requires_rehide(
            g_app.trayWindowHiddenIntent, unexpectedlyVisible,
            g_mainWindowTrayHideEnforcementActive)) {
        g_mainWindowTrayHideEnforcementActive = true;
        ShowWindow(g_app.hMainWnd, SW_HIDE);
        g_mainWindowTrayHideEnforcementActive = false;
    }
    debug_log_on_change(
        "tray window: hidden intent enforced reason=%s wasVisible=%d nowVisible=%d\n",
        reason && reason[0] ? reason : "presentation event",
        unexpectedlyVisible ? 1 : 0,
        IsWindowVisible(g_app.hMainWnd) ? 1 : 0);
    update_fan_telemetry_timer();
}

static void enforce_main_window_tray_state_from_message(
    HWND hwnd, UINT message) {
    if (message == WM_DESTROY || message == WM_NCDESTROY ||
        hwnd != g_app.hMainWnd ||
        !main_window_tray_hidden_invariant_violated()) return;
    char reason[96] = {};
    snprintf(reason, sizeof(reason),
        "WndProc postcondition msg=0x%04X style=0x%llX",
        (unsigned int)message,
        (unsigned long long)(ULONG_PTR)GetWindowLongPtrA(hwnd, GWL_STYLE));
    enforce_main_window_tray_state(reason);
}
