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

// The Updates dialog (source/gui_update_dialog.cpp).  Declared here for the
// same reason as the line above: ui_main_window.cpp opens it, and that shard
// is compiled before the dialog's own translation unit in the amalgamation.
void gui_update_open_dialog(HWND parent);

// The once-per-machine "may Green Curve check for updates?" question
// (source/gui_update_dialog.cpp).  Driven from the main window's poll tick,
// which is compiled first, so it needs the same forward declaration.
void gui_update_maybe_prompt_first_run(HWND parent);

// Replay settings captured by an in-app update (source/
// gui_update_settings_handoff.cpp).  Declared here for the same reason as
// the line above: entry.cpp calls it during startup and is compiled before
// the shard that defines it.
void gui_update_replay_pending_restore();

#endif // _WIN32

#endif // GREEN_CURVE_AUTO_PROFILE_H
