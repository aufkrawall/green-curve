// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#pragma once

// A fan runtime handback inside a standby window (pure; needs the app_shared.h
// context for DesiredSettings, GpuAdapterInfo, ServiceProfileSource and the
// lifecycle reducer state).
//
// When the fan runtime gives a fan up it hands it to the driver and rewrites
// the owned intent to what Green Curve still holds (main_service_fan_fault.cpp).
// The fan worker keeps running across suspend, so a driver that is still
// waking up can make it give up between suspend and the standby restore's
// write.  The rewritten intent would then make that restore skip the fan and
// leave the custom curve off after every such wake.
//
// The handback therefore keeps a copy of the intent it replaced, tagged with
// the suspend generation.  The standby restore of that same wake replays the
// replaced intent once, and only while nothing else has touched the intent
// since the handback.  The handback itself is unchanged: the driver has the fan
// until the restore writes, and a curve that fails again is handed back again,
// this time outside the window.

#ifndef GREEN_CURVE_SERVICE_STANDBY_FAN_STASH_POLICY_H
#define GREEN_CURVE_SERVICE_STANDBY_FAN_STASH_POLICY_H

struct ServiceStandbyFanStash {
    gc_bool8 valid;
    gc_u64 suspendGeneration;
    // The owned intent, its GPU and its profile attribution before the handback.
    DesiredSettings replaced;
    GpuAdapterInfo replacedGpu;
    ServiceProfileSource replacedProfileSource;
    int replacedProfileSlot;
    // The owned intent the handback left, to detect any later change.
    gc_bool8 handbackHasIntent;
    DesiredSettings handbackIntent;
};

// True while a standby restore for the current suspend generation is still
// ahead: suspended (no resume seen yet), or resumed with the restore not yet
// written.  A lockout, a confirmed driver recovery or a stopping service closes
// the window: none of them will run the standby restore.  The restore's own
// write (standbyWriteIssued) is outside the window, so a fan that fails during
// the restore itself is a real failure.
static inline bool service_standby_fan_handback_window(
    const ServiceLifecycleState* state, gc_u64* generationOut) {
    if (generationOut) *generationOut = 0;
    if (!state || state->stopped || state->lockedOut || state->driverPending)
        return false;
    if (state->suspendArmed) {
        if (generationOut) *generationOut = state->suspendGeneration;
        return true;
    }
    if (state->standbyPending && !state->standbyWriteIssued &&
        state->resumedSuspendGeneration == state->suspendGeneration) {
        if (generationOut) *generationOut = state->resumedSuspendGeneration;
        return true;
    }
    return false;
}

// Only a handback that took a manual fan setting (curve or fixed duty) away
// from an owned intent has anything to replay.
static inline bool service_standby_fan_stash_should_capture(bool windowOpen,
    bool hadIntent, const DesiredSettings* previous) {
    if (!windowOpen || !hadIntent || !previous) return false;
    if (!previous->hasFan || previous->fanAuto) return false;
    return previous->fanMode == FAN_MODE_CURVE ||
           previous->fanMode == FAN_MODE_FIXED;
}

enum ServiceStandbyFanStashUse {
    SERVICE_STANDBY_FAN_STASH_NONE = 0,
    SERVICE_STANDBY_FAN_STASH_REPLAY,
    SERVICE_STANDBY_FAN_STASH_STALE_GENERATION,
    SERVICE_STANDBY_FAN_STASH_INTENT_CHANGED,
};

static inline const char* service_standby_fan_stash_use_name(
    ServiceStandbyFanStashUse use) {
    switch (use) {
        case SERVICE_STANDBY_FAN_STASH_REPLAY: return "replay";
        case SERVICE_STANDBY_FAN_STASH_STALE_GENERATION: return "stale-generation";
        case SERVICE_STANDBY_FAN_STASH_INTENT_CHANGED: return "intent-changed";
        default: return "none";
    }
}

// Decides what the standby restore of `resumedGeneration` does with a stash.
// Any change to the owned intent after the handback (an Apply, a profile
// switch, a Reset) wins over the stash.
static inline ServiceStandbyFanStashUse service_standby_fan_stash_use(
    const ServiceStandbyFanStash* stash, gc_u64 resumedGeneration,
    bool currentHasIntent, const DesiredSettings* current) {
    if (!stash || !stash->valid) return SERVICE_STANDBY_FAN_STASH_NONE;
    if (stash->suspendGeneration == 0 ||
        stash->suspendGeneration != resumedGeneration)
        return SERVICE_STANDBY_FAN_STASH_STALE_GENERATION;
    if ((stash->handbackHasIntent != 0) != currentHasIntent)
        return SERVICE_STANDBY_FAN_STASH_INTENT_CHANGED;
    if (currentHasIntent &&
        !desired_settings_equal(current, &stash->handbackIntent))
        return SERVICE_STANDBY_FAN_STASH_INTENT_CHANGED;
    return SERVICE_STANDBY_FAN_STASH_REPLAY;
}

#endif // GREEN_CURVE_SERVICE_STANDBY_FAN_STASH_POLICY_H
