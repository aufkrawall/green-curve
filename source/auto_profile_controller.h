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

// The driver finished applying `slot` (auto or manual).  Records applied state.
void ap_on_applied(AutoProfileController* c, int slot, long long nowMs);

// A per-slot hotkey / tray profile pick fired.  Same slot while already pinned
// to it → resume AUTO; otherwise pin the slot and apply it immediately.
AutoProfileAction ap_on_hotkey(AutoProfileController* c, int slot);

// A manual GUI Apply of custom (non-slot) editor state — suspend auto without a
// known pinned slot so auto never clobbers the user's deliberate action.
void ap_enter_manual_custom(AutoProfileController* c);

// Master enable/disable toggle.  Enabling resumes AUTO (re-resolve + converge);
// disabling reverts to the default slot.
AutoProfileAction ap_set_enabled(AutoProfileController* c, bool enabled);

#endif // GREEN_CURVE_AUTO_PROFILE_CONTROLLER_H
