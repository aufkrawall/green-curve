// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// auto_profile_win32.cpp — the auto-profile driver.  Ties the pure resolver +
// controller (coalescing/hysteresis/manual-pin) to Win32 foreground detection
// (an OUTOFCONTEXT WinEvent hook — no injection), global hotkeys, the debounce +
// backstop timers, and the existing profile-apply path.  Runs entirely on the
// GUI message thread, so applies are naturally serialized and the coalescing is
// "latest-wins" without touching the single-instance service pipe.
//
// Included late in the main.cpp unity chain so every profile-apply/UI static it
// calls (load_profile_from_config, apply_desired_settings,
// populate_desired_into_gui, refresh_*, update_tray_icon, ...) is already
// defined.

#include "app_shared.h"
#include "auto_profile.h"
#include "hotkeys.h"

#define AUTO_PROFILE_BACKSTOP_INTERVAL_MS 3000

static AutoProfileConfig g_apConfig;
static AutoProfileController g_apCtrl;
static HWINEVENTHOOK g_apForegroundHook = nullptr;
static bool g_apInitialized = false;
static bool g_apBackstopRunning = false;
static bool g_apApplyInFlight = false;
static HotkeyBinding g_apHotkeys[CONFIG_NUM_SLOTS + 1] = {};      // 1-based by slot
static bool g_apHotkeyRegistered[CONFIG_NUM_SLOTS + 1] = {};

static long long ap_now_ms() { return (long long)GetTickCount64(); }

// Do not auto-switch while the main window is open/being edited (would clobber
// live edits) or while an apply is already running (defensive re-entrancy guard).
static bool ap_suppressed() {
    if (g_apApplyInFlight) return true;
    if (g_apConfig.suppressWhenWindowOpen && g_app.hMainWnd && IsWindowVisible(g_app.hMainWnd)) return true;
    return false;
}

static int ap_resolve_current_target() {
    ForegroundInfo fg = {};
    auto_profile_get_foreground_info(g_app.hMainWnd, &fg);   // leaves fg.valid=false on desktop/shell
    ProcessPresence pres = {};
    auto_profile_compute_presence(&g_apConfig, &pres);
    return resolve_auto_profile_slot(&g_apConfig, &fg, &pres);
}

// The actual apply: reuse the same TDR-safe path + idempotency skip the app-start
// auto-load uses (maybe_load_app_launch_profile_to_gui).
static void ap_do_apply_slot(int slot, bool manual) {
    if (slot < 1 || slot > CONFIG_NUM_SLOTS) return;
    if (g_apApplyInFlight) return;
    if (!is_profile_slot_saved(g_app.configPath, slot)) {
        debug_log("auto-profile: target slot %d is empty; skipping apply\n", slot);
        return;
    }
    DesiredSettings desired = {};
    char err[256] = {};
    if (!load_profile_from_config(g_app.configPath, slot, &desired, err, sizeof(err))) {
        debug_log("auto-profile: load slot %d failed: %s\n", slot, err);
        return;
    }

    // "Don't needlessly re-apply": if the service already owns this exact intent,
    // skip the disruptive reset+reapply and just reflect the slot in the GUI.
    // (Uses the cached service-availability flags — apply_desired_settings() does
    // its own refresh, so we avoid an extra blocking ping on the message thread.)
    if (g_app.usingBackgroundService && g_app.backgroundServiceAvailable) {
        DesiredSettings active = {};
        char snapErr[256] = {};
        char detail[256] = {};
        if (refresh_service_snapshot_and_active_desired(snapErr, sizeof(snapErr), &active) &&
            desired_settings_match_active_service_intent(&desired, &active, detail, sizeof(detail))) {
            debug_log("auto-profile: slot %d already active in service; skipping apply (%s)\n",
                      slot, detail[0] ? detail : "match");
            populate_desired_into_gui(&desired);
            set_config_int(g_app.configPath, "profiles", "selected_slot", slot);
            set_config_int(g_app.configPath, "profiles", "applied_slot", slot);
            refresh_profile_controls_from_config();
            ap_on_applied(&g_apCtrl, slot, ap_now_ms());
            update_tray_icon();
            invalidate_main_window();
            return;
        }
    }

    // A real switch runs the ~3s service-side VF apply synchronously on the
    // message thread (same as the manual Apply button). Show a wait cursor so a
    // timer-driven or menu-pick switch reads as "busy" rather than a random hang.
    desired.resetOcBeforeApply = true;
    char result[512] = {};
    HCURSOR prevCursor = SetCursor(LoadCursorA(nullptr, IDC_WAIT));
    g_apApplyInFlight = true;
    bool ok = apply_desired_settings(&desired, false, result, sizeof(result));
    g_apApplyInFlight = false;
    SetCursor(prevCursor ? prevCursor : LoadCursorA(nullptr, IDC_ARROW));
    if (ok) {
        populate_desired_into_gui(&desired);
        set_config_int(g_app.configPath, "profiles", "selected_slot", slot);
        set_config_int(g_app.configPath, "profiles", "applied_slot", slot);
        refresh_profile_controls_from_config();
        ap_on_applied(&g_apCtrl, slot, ap_now_ms());
        update_tray_icon();
        invalidate_main_window();
        debug_log("auto-profile: applied slot %d (%s)\n", slot, manual ? "manual" : "auto");
    } else {
        debug_log("auto-profile: apply slot %d FAILED: %s\n", slot, result[0] ? result : "unknown");
    }
}

static void ap_arm_debounce(HWND hwnd, int delayMs) {
    SetTimer(hwnd, AUTO_PROFILE_DEBOUNCE_TIMER_ID, (UINT)(delayMs > 0 ? delayMs : 1), nullptr);
}

// Dispatch a controller action.  Recursion is bounded: RESUME_AUTO re-resolves
// and dispatches a follow-up action that is only ever ARM_DEBOUNCE or NONE.
static void ap_execute(HWND hwnd, AutoProfileAction a) {
    switch (a.kind) {
        case AP_ACTION_ARM_DEBOUNCE:
            ap_arm_debounce(hwnd, a.delayMs);
            break;
        case AP_ACTION_APPLY_SLOT: {
            KillTimer(hwnd, AUTO_PROFILE_DEBOUNCE_TIMER_ID);
            ap_do_apply_slot(a.slot, false);
            // Latest-wins: the ~3s apply blocked the pump; re-check the current
            // foreground once and arm a follow-up switch if it moved on.
            int t = ap_resolve_current_target();
            AutoProfileAction re = ap_on_target_resolved(&g_apCtrl, t, ap_now_ms(), ap_suppressed());
            if (re.kind == AP_ACTION_ARM_DEBOUNCE) ap_arm_debounce(hwnd, re.delayMs);
            break;
        }
        case AP_ACTION_RESUME_AUTO: {
            int t = ap_resolve_current_target();
            AutoProfileAction re = ap_on_target_resolved(&g_apCtrl, t, ap_now_ms(), ap_suppressed());
            ap_execute(hwnd, re);
            break;
        }
        case AP_ACTION_NONE:
        default:
            break;
    }
}

// ---- Event entry points ----------------------------------------------------

void auto_profile_on_foreground_changed(HWND hwnd) {
    if (!g_apInitialized) return;
    if (!ap_controller_is_driving(&g_apCtrl, ap_suppressed())) return;
    int t = ap_resolve_current_target();
    ap_execute(hwnd, ap_on_target_resolved(&g_apCtrl, t, ap_now_ms(), ap_suppressed()));
}

void auto_profile_on_debounce_timer(HWND hwnd) {
    KillTimer(hwnd, AUTO_PROFILE_DEBOUNCE_TIMER_ID);
    if (!g_apInitialized) return;
    int t = ap_resolve_current_target();
    ap_execute(hwnd, ap_on_debounce_fire(&g_apCtrl, t, ap_now_ms(), ap_suppressed()));
}

void auto_profile_on_backstop_timer(HWND hwnd) {
    if (!g_apInitialized) return;
    if (!ap_controller_is_driving(&g_apCtrl, ap_suppressed())) return;
    // Catches game exits and focus-optional presence changes that fired no
    // foreground event.  Feeds the resolver like a foreground change (arms the
    // debounce only when the target actually differs).
    int t = ap_resolve_current_target();
    ap_execute(hwnd, ap_on_target_resolved(&g_apCtrl, t, ap_now_ms(), ap_suppressed()));
}

void auto_profile_on_hotkey(HWND hwnd, int hotkeyId) {
    int slot = hotkeyId - AUTO_PROFILE_HOTKEY_ID_BASE;
    if (slot < 1 || slot > CONFIG_NUM_SLOTS) return;
    ap_execute(hwnd, ap_on_hotkey(&g_apCtrl, slot));
    update_tray_icon();
}

void auto_profile_pick_slot(HWND hwnd, int slot) {
    if (slot < 1 || slot > CONFIG_NUM_SLOTS) return;
    ap_execute(hwnd, ap_on_hotkey(&g_apCtrl, slot));   // same manual-pin semantics as a hotkey
    update_tray_icon();
}

// ---- WinEvent foreground hook ---------------------------------------------

static void CALLBACK ap_winevent_proc(HWINEVENTHOOK, DWORD event, HWND, LONG idObject,
                                      LONG idChild, DWORD, DWORD) {
    if (event != EVENT_SYSTEM_FOREGROUND) return;
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    if (g_app.hMainWnd) auto_profile_on_foreground_changed(g_app.hMainWnd);
}

static void ap_apply_runtime_state(HWND hwnd) {
    if (g_apConfig.enabled) {
        if (!g_apForegroundHook) {
            g_apForegroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                nullptr, ap_winevent_proc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        }
        if (!g_apBackstopRunning) {
            SetTimer(hwnd, AUTO_PROFILE_BACKSTOP_TIMER_ID, AUTO_PROFILE_BACKSTOP_INTERVAL_MS, nullptr);
            g_apBackstopRunning = true;
        }
    } else {
        if (g_apForegroundHook) { UnhookWinEvent(g_apForegroundHook); g_apForegroundHook = nullptr; }
        if (g_apBackstopRunning) { KillTimer(hwnd, AUTO_PROFILE_BACKSTOP_TIMER_ID); g_apBackstopRunning = false; }
        KillTimer(hwnd, AUTO_PROFILE_DEBOUNCE_TIMER_ID);
    }
}

// ---- Hotkeys ---------------------------------------------------------------

static void ap_load_hotkeys() {
    for (int s = 1; s <= CONFIG_NUM_SLOTS; s++) {
        g_apHotkeys[s].mods = 0;
        g_apHotkeys[s].vk = 0;
        char key[16] = {};
        StringCchPrintfA(key, sizeof(key), "slot%d", s);
        char val[64] = {};
        if (get_config_string(g_app.configPath, "hotkeys", key, "", val, sizeof(val)) && val[0]) {
            HotkeyBinding b = {};
            if (hotkey_parse(val, &b)) g_apHotkeys[s] = b;
            else debug_log("auto-profile: unparseable hotkey for slot %d: '%s'\n", s, val);
        }
    }
}

static void ap_register_hotkeys(HWND hwnd) {
    for (int s = 1; s <= CONFIG_NUM_SLOTS; s++) {
        if (g_apHotkeyRegistered[s]) {
            hotkey_unregister(hwnd, AUTO_PROFILE_HOTKEY_ID_BASE + s);
            g_apHotkeyRegistered[s] = false;
        }
        if (g_apHotkeys[s].vk) {
            if (hotkey_register(hwnd, AUTO_PROFILE_HOTKEY_ID_BASE + s, &g_apHotkeys[s])) {
                g_apHotkeyRegistered[s] = true;
            } else {
                debug_log("auto-profile: hotkey for slot %d failed to register (already in use?)\n", s);
            }
        }
    }
}

static void ap_unregister_hotkeys(HWND hwnd) {
    for (int s = 1; s <= CONFIG_NUM_SLOTS; s++) {
        if (g_apHotkeyRegistered[s]) {
            hotkey_unregister(hwnd, AUTO_PROFILE_HOTKEY_ID_BASE + s);
            g_apHotkeyRegistered[s] = false;
        }
    }
}

// ---- Lifecycle -------------------------------------------------------------

static void ap_converge_now(HWND hwnd) {
    if (!g_apConfig.enabled) return;
    int t = ap_resolve_current_target();
    ap_execute(hwnd, ap_on_target_resolved(&g_apCtrl, t, ap_now_ms(), ap_suppressed()));
}

void auto_profile_init(HWND hwnd) {
    if (g_apInitialized) return;
    auto_profile_config_load(g_app.configPath, &g_apConfig);
    ap_controller_init(&g_apCtrl, &g_apConfig);
    ap_load_hotkeys();
    ap_register_hotkeys(hwnd);
    ap_apply_runtime_state(hwnd);
    g_apInitialized = true;
    debug_log("auto-profile: init (enabled=%d rules=%d default_slot=%d debounce=%d cooldown=%d)\n",
              g_apConfig.enabled ? 1 : 0, g_apConfig.ruleCount, g_apConfig.defaultSlot,
              g_apConfig.switchDebounceMs, g_apConfig.minSwitchIntervalMs);
    // Do NOT converge here: auto_profile_init runs from the main window's
    // WM_CREATE, before entry.cpp finishes creating the child controls, so an
    // apply + populate_desired_into_gui now would touch not-yet-created controls.
    // The backstop timer (armed above when enabled) performs the first
    // convergence within ~3s, by which point the GUI is fully built and shown
    // (and, when visible, suppression correctly holds auto off anyway).
}

void auto_profile_shutdown(HWND hwnd) {
    if (!g_apInitialized) return;
    ap_unregister_hotkeys(hwnd);
    if (g_apForegroundHook) { UnhookWinEvent(g_apForegroundHook); g_apForegroundHook = nullptr; }
    if (g_apBackstopRunning) { KillTimer(hwnd, AUTO_PROFILE_BACKSTOP_TIMER_ID); g_apBackstopRunning = false; }
    KillTimer(hwnd, AUTO_PROFILE_DEBOUNCE_TIMER_ID);
    g_apInitialized = false;
}

void auto_profile_reload_config(HWND hwnd) {
    if (!g_apInitialized) { auto_profile_init(hwnd); return; }
    auto_profile_config_load(g_app.configPath, &g_apConfig);
    ap_controller_sync_config(&g_apCtrl, &g_apConfig);
    ap_load_hotkeys();
    ap_register_hotkeys(hwnd);
    ap_apply_runtime_state(hwnd);
    update_tray_icon();
    debug_log("auto-profile: config reloaded (enabled=%d rules=%d)\n",
              g_apConfig.enabled ? 1 : 0, g_apConfig.ruleCount);
    ap_converge_now(hwnd);
}

void auto_profile_toggle_enabled(HWND hwnd) {
    bool newEnabled = !g_apConfig.enabled;
    g_apConfig.enabled = newEnabled;
    auto_profile_config_save(g_app.configPath, &g_apConfig);
    AutoProfileAction a = ap_set_enabled(&g_apCtrl, newEnabled);
    ap_apply_runtime_state(hwnd);   // install/remove hook + backstop for the new state
    ap_execute(hwnd, a);
    update_tray_icon();
    debug_log("auto-profile: %s by user\n", newEnabled ? "ENABLED" : "DISABLED");
}

// ---- Queries (for menu rendering) -----------------------------------------

bool auto_profile_is_enabled() { return g_apConfig.enabled; }
bool auto_profile_is_manual_pinned() { return g_apInitialized && g_apCtrl.mode == AP_MODE_MANUAL; }

// "Active" for the tray/menu tick = the last slot actually applied to the GPU
// (`[profiles] applied_slot`).  Every apply path (auto-switch, hotkey, manual
// Apply button, logon apply) writes this.  The combo-selection handler only
// writes "selected_slot" (for editing/saving) and must not affect the checkmark.
// The controller's own appliedSlot only tracks auto/hotkey applies, so it would
// miss a manual Apply or a fresh start — applied_slot is the consistent
// source of truth for "which profile is actually active on the GPU".
int auto_profile_active_slot() {
    int applied = get_config_int(g_app.configPath, "profiles", "applied_slot", 0);
    if (applied >= 1 && applied <= CONFIG_NUM_SLOTS) return applied;
    return 0;
}
const AutoProfileConfig* auto_profile_config() { return &g_apConfig; }
