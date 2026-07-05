// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// auto_profile.h — Win32 public surface of the auto-profile subsystem (the
// driver that ties the pure resolver + controller to foreground detection,
// global hotkeys, timers, and the existing profile-apply path).  Include
// app_shared.h (windows.h) BEFORE this header; the unity build already does.

#ifndef GREEN_CURVE_AUTO_PROFILE_H
#define GREEN_CURVE_AUTO_PROFILE_H

#include "auto_profile_rules.h"
#include "auto_profile_controller.h"

#if defined(_WIN32)

// Lifecycle (called from the main WndProc).
void auto_profile_init(HWND hwnd);
void auto_profile_shutdown(HWND hwnd);
void auto_profile_reload_config(HWND hwnd);   // re-read config + re-arm hook/hotkeys/timers

// Event entry points.
void auto_profile_on_foreground_changed(HWND hwnd);   // WinEvent foreground hook
void auto_profile_on_debounce_timer(HWND hwnd);       // AUTO_PROFILE_DEBOUNCE_TIMER_ID
void auto_profile_on_backstop_timer(HWND hwnd);       // AUTO_PROFILE_BACKSTOP_TIMER_ID
void auto_profile_on_hotkey(HWND hwnd, int hotkeyId); // WM_HOTKEY (id -> slot)

// Menu / tray actions.
void auto_profile_pick_slot(HWND hwnd, int slot);     // profile pick (manual-pin semantics)
void auto_profile_toggle_enabled(HWND hwnd);          // flip enabled + persist + revert/resume

// Queries for menu rendering.
bool auto_profile_is_enabled();
bool auto_profile_is_manual_pinned();
int  auto_profile_active_slot();                      // controller applied slot (0 = unknown)
const AutoProfileConfig* auto_profile_config();

// Detection helpers (auto_profile_detect.cpp).  Kept separate so the resolver is
// fed the neutral ForegroundInfo/ProcessPresence structs.
bool auto_profile_get_foreground_info(HWND selfWnd, ForegroundInfo* out);
void auto_profile_compute_presence(const AutoProfileConfig* cfg, ProcessPresence* out);

// Rule-editor dialog (auto_profile_dialog.cpp).
void auto_profile_open_config_dialog(HWND parent);

#endif // _WIN32

#endif // GREEN_CURVE_AUTO_PROFILE_H
