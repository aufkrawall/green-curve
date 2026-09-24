// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// auto_profile_controller.h — pure decision core for the auto-profile switcher.
// Holds the AUTO/MANUAL-pin state machine plus the debounce + cooldown
// (hysteresis) logic that coalesces rapid foreground changes into at most one
// "latest-wins" apply.  All transitions are pure functions returning an
// AutoProfileAction for the Win32 driver (auto_profile_detect.cpp) to execute —
// no timers, no I/O — so the coalescing/cooldown/manual-pin behavior is fully
// unit-testable without a GPU or an interactive desktop.

#ifndef GREEN_CURVE_AUTO_PROFILE_CONTROLLER_H
#define GREEN_CURVE_AUTO_PROFILE_CONTROLLER_H

#include "auto_profile_rules.h"

enum AutoProfileMode {
    AP_MODE_AUTO = 0,     // the switcher is driving from foreground detection
    AP_MODE_MANUAL = 1,   // a hotkey / tray pick / GUI apply pinned a profile; auto is suspended
};

enum AutoProfileActionKind {
    AP_ACTION_NONE = 0,          // do nothing
    AP_ACTION_ARM_DEBOUNCE = 1,  // (re)start the debounce/cooldown timer for `delayMs`
    AP_ACTION_APPLY_SLOT = 2,    // apply profile `slot` now, then call ap_on_applied()
    AP_ACTION_RESUME_AUTO = 3,   // pin cleared; driver should re-resolve foreground and converge now
};

struct AutoProfileAction {
    AutoProfileActionKind kind;
    int slot;      // for AP_ACTION_APPLY_SLOT
    int delayMs;   // for AP_ACTION_ARM_DEBOUNCE
};

struct AutoProfileController {
    bool autoEnabled;      // mirrors AutoProfileConfig.enabled
    int mode;              // AutoProfileMode
    int pinnedSlot;        // slot pinned in MANUAL mode (0 = custom/unknown)
    int appliedSlot;       // slot we believe is currently applied (0 = unknown)
    int pendingTarget;     // debounce-pending target slot (0 = none)
    long long lastApplyMs; // tick of the last apply we drove
    // Failure backoff.  A switch that failed is not retried until
    // retryNotBeforeMs; each consecutive failure of the same slot doubles the
    // wait (minIntervalMs, 2x, 4x ... capped at AUTO_PROFILE_FAILURE_BACKOFF_MAX_MS).
    // Without it a deterministic failure -- an empty slot, a profile that does
    // not load, a refused apply -- was re-armed at the debounce interval
    // forever while the matching app kept focus.
    int failedSlot;           // 0 = no failure pending
    int consecutiveFailures;
    long long retryNotBeforeMs;
    // Synced from config:
    int debounceMs;
    int minIntervalMs;
    int defaultSlot;
};

// Initialize/sync from config.  init sets AUTO mode + unknown applied state.
void ap_controller_init(AutoProfileController* c, const AutoProfileConfig* cfg);
void ap_controller_sync_config(AutoProfileController* c, const AutoProfileConfig* cfg);

// True when auto-switching is actively driving (enabled, AUTO mode, not
// suppressed).  Suppression (main window open) is supplied by the caller.
bool ap_controller_is_driving(const AutoProfileController* c, bool suppressed);

// Detection produced a resolved target slot (from a foreground change or a
// backstop tick).  Arms the debounce when the target differs from the applied
// slot; otherwise clears any pending switch.
AutoProfileAction ap_on_target_resolved(AutoProfileController* c, int targetSlot,
                                        long long nowMs, bool suppressed);

// The debounce/cooldown timer fired.  `currentTarget` is the FRESHLY re-resolved
// target at fire time (latest-wins).  Applies it, defers for the remaining
// cooldown, or does nothing.
AutoProfileAction ap_on_debounce_fire(AutoProfileController* c, int currentTarget,
                                      long long nowMs, bool suppressed);

// The driver finished applying `slot` (auto or manual).  Records applied state
// and clears any failure backoff.
void ap_on_applied(AutoProfileController* c, int slot, long long nowMs);

// Applying `slot` failed (refused before the queue, or completed with an
// error).  Charges the cooldown and arms the per-slot failure backoff, so the
// resolver keeps its target but does not retry until the backoff elapses.
// Explicit picks (ap_on_hotkey), enable transitions and config changes clear it.
void ap_on_apply_failed(AutoProfileController* c, int slot, long long nowMs);

// Remaining failure backoff for `slot` at `nowMs`, 0 when it may be tried now.
long long ap_failure_backoff_remaining_ms(const AutoProfileController* c,
                                          int slot, long long nowMs);

// A per-slot hotkey / tray profile pick fired.  Same slot while already pinned
// to it → resume AUTO; otherwise pin the slot and apply it immediately.
// While auto is DISABLED a pick is just an apply and records no pin: there is
// no automatic switching to override, and a pin taken then would outlive the
// pick and suppress auto once it is enabled.
AutoProfileAction ap_on_hotkey(AutoProfileController* c, int slot);

// A manual GUI Apply of custom (non-slot) editor state — suspend auto without a
// known pinned slot so auto never clobbers the user's deliberate action.
void ap_enter_manual_custom(AutoProfileController* c);

// Master enable/disable toggle.  Enabling resumes AUTO (re-resolve + converge);
// disabling reverts to the default slot.
AutoProfileAction ap_set_enabled(AutoProfileController* c, bool enabled);

// Adopt a freshly loaded/edited configuration.  Values are always synced, and a
// CHANGE of the master enable additionally performs the enable/disable
// TRANSITION (ap_set_enabled) rather than only updating the flag.  Every
// surface that can flip that flag — the tray toggle and the configuration
// dialog alike — must route through this, so none of them can leave a manual
// pin behind that keeps ap_controller_is_driving() false after auto is enabled.
AutoProfileAction ap_apply_config_change(AutoProfileController* c, const AutoProfileConfig* cfg);

#endif // GREEN_CURVE_AUTO_PROFILE_CONTROLLER_H
