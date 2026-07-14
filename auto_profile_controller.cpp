// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// auto_profile_controller.cpp — pure implementation of the auto-profile state
// machine (see auto_profile_controller.h).  No OS APIs; unit-tested in build.py.

#include "auto_profile_controller.h"

static bool ap_slot_valid(int slot) {
    return slot >= 1 && slot <= CONFIG_NUM_SLOTS;
}

static AutoProfileAction ap_action_none() {
    AutoProfileAction a = {};
    a.kind = AP_ACTION_NONE;
    return a;
}

static AutoProfileAction ap_action_arm(int delayMs) {
    AutoProfileAction a = {};
    a.kind = AP_ACTION_ARM_DEBOUNCE;
    a.delayMs = delayMs > 0 ? delayMs : 1;
    return a;
}

static AutoProfileAction ap_action_apply(int slot) {
    AutoProfileAction a = {};
    a.kind = AP_ACTION_APPLY_SLOT;
    a.slot = slot;
    return a;
}

void ap_controller_sync_config(AutoProfileController* c, const AutoProfileConfig* cfg) {
    if (!c || !cfg) return;
    c->autoEnabled = cfg->enabled;
    c->debounceMs = cfg->switchDebounceMs > 0 ? cfg->switchDebounceMs : AUTO_PROFILE_DEFAULT_DEBOUNCE_MS;
    c->minIntervalMs = cfg->minSwitchIntervalMs > 0 ? cfg->minSwitchIntervalMs : AUTO_PROFILE_DEFAULT_MIN_INTERVAL_MS;
    c->defaultSlot = ap_slot_valid(cfg->defaultSlot) ? cfg->defaultSlot : CONFIG_DEFAULT_SLOT;
}

void ap_controller_init(AutoProfileController* c, const AutoProfileConfig* cfg) {
    if (!c) return;
    c->mode = AP_MODE_AUTO;
    c->pinnedSlot = 0;
    c->appliedSlot = 0;      // unknown until the first apply/observation
    c->pendingTarget = 0;
    // Far-past so the first switch is not gated by the cooldown.
    c->lastApplyMs = -1000000;
    c->autoEnabled = false;
    c->debounceMs = AUTO_PROFILE_DEFAULT_DEBOUNCE_MS;
    c->minIntervalMs = AUTO_PROFILE_DEFAULT_MIN_INTERVAL_MS;
    c->defaultSlot = CONFIG_DEFAULT_SLOT;
    ap_controller_sync_config(c, cfg);
}

bool ap_controller_is_driving(const AutoProfileController* c, bool suppressed) {
    return c && c->autoEnabled && c->mode == AP_MODE_AUTO && !suppressed;
}

AutoProfileAction ap_on_target_resolved(AutoProfileController* c, int targetSlot,
                                        long long nowMs, bool suppressed) {
    (void)nowMs;
    if (!ap_controller_is_driving(c, suppressed)) return ap_action_none();
    if (!ap_slot_valid(targetSlot)) return ap_action_none();
    if (targetSlot == c->appliedSlot) {
        // Converged (or bounced back before committing) — cancel any pending switch.
        c->pendingTarget = 0;
        return ap_action_none();
    }
    c->pendingTarget = targetSlot;
    return ap_action_arm(c->debounceMs);
}

AutoProfileAction ap_on_debounce_fire(AutoProfileController* c, int currentTarget,
                                      long long nowMs, bool suppressed) {
    if (!ap_controller_is_driving(c, suppressed)) {
        if (c) c->pendingTarget = 0;
        return ap_action_none();
    }
    if (!ap_slot_valid(currentTarget)) {
        c->pendingTarget = 0;
        return ap_action_none();
    }
    if (currentTarget == c->appliedSlot) {
        c->pendingTarget = 0;
        return ap_action_none();
    }
    // Cooldown: never switch more than once per minInterval.  Defer the remainder.
    long long sinceApply = nowMs - c->lastApplyMs;
    if (sinceApply < c->minIntervalMs) {
        c->pendingTarget = currentTarget;
        int remaining = (int)(c->minIntervalMs - sinceApply);
        return ap_action_arm(remaining);
    }
    c->pendingTarget = 0;
    return ap_action_apply(currentTarget);
}

void ap_on_applied(AutoProfileController* c, int slot, long long nowMs) {
    if (!c) return;
    c->appliedSlot = slot;
    c->lastApplyMs = nowMs;
    c->pendingTarget = 0;
}

AutoProfileAction ap_on_hotkey(AutoProfileController* c, int slot) {
    if (!c || !ap_slot_valid(slot)) return ap_action_none();
    if (c->mode == AP_MODE_MANUAL && c->pinnedSlot == slot) {
        // Same slot pressed again → release the pin and return to auto.
        c->mode = AP_MODE_AUTO;
        c->pinnedSlot = 0;
        c->pendingTarget = 0;
        AutoProfileAction a = {};
        a.kind = AP_ACTION_RESUME_AUTO;
        return a;
    }
    // Pin this slot and apply it now (manual, user-initiated → no debounce/cooldown).
    c->mode = AP_MODE_MANUAL;
    c->pinnedSlot = slot;
    c->pendingTarget = 0;
    return ap_action_apply(slot);
}

void ap_enter_manual_custom(AutoProfileController* c) {
    if (!c) return;
    c->mode = AP_MODE_MANUAL;
    c->pinnedSlot = 0;      // custom editor state, not a known slot
    c->pendingTarget = 0;
}

AutoProfileAction ap_set_enabled(AutoProfileController* c, bool enabled) {
    if (!c) return ap_action_none();
    c->autoEnabled = enabled;
    if (enabled) {
        c->mode = AP_MODE_AUTO;
        c->pinnedSlot = 0;
        c->pendingTarget = 0;
        AutoProfileAction a = {};
        a.kind = AP_ACTION_RESUME_AUTO;
        return a;
    }
    // Disabling: stop switching and revert to the default slot so we never
    // strand an OC/UV profile after auto is turned off.
    c->mode = AP_MODE_AUTO;   // reset mode so a later re-enable starts clean
    c->pinnedSlot = 0;
    c->pendingTarget = 0;
    if (ap_slot_valid(c->defaultSlot) && c->defaultSlot != c->appliedSlot) {
        return ap_action_apply(c->defaultSlot);
    }
    return ap_action_none();
}
