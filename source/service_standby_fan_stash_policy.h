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
// the reducer's standby stash epoch (ServiceLifecycleState::standbyStashEpoch).
// The next standby restore replays the replaced intent once, and only while
// that epoch is unchanged (no automatic write, explicit supersede or lockout
// since) and the owned intent still equals what the handback left.  A further
// suspend before the restore writes keeps the copy: that is the same waking
// driver.  The handback itself is unchanged: the driver has the fan until the
// restore writes, and a curve that fails again is handed back again, this time
// outside the window.

#ifndef GREEN_CURVE_SERVICE_STANDBY_FAN_STASH_POLICY_H
#define GREEN_CURVE_SERVICE_STANDBY_FAN_STASH_POLICY_H

struct ServiceStandbyFanStash {
    gc_bool8 valid;
    gc_u64 epoch;
    // The owned intent, its GPU and its profile attribution before the handback.
    DesiredSettings replaced;
    GpuAdapterInfo replacedGpu;
    ServiceProfileSource replacedProfileSource;
    int replacedProfileSlot;
    // The owned intent the handback left, to detect any later change.
    gc_bool8 handbackHasIntent;
    DesiredSettings handbackIntent;
};

// True while a standby restore is still ahead: suspended (no resume seen yet),
// or resumed with the restore not yet written.  A lockout, a confirmed driver
// recovery or a stopping service closes the window: none of them will run the
// standby restore.  The restore's own write (standbyWriteIssued) is outside
// the window, so a fan that fails during the restore itself is a real failure.
// `epochOut` receives the epoch the stash is tagged with.
static inline bool service_standby_fan_handback_window(
    const ServiceLifecycleState* state, gc_u64* epochOut) {
    if (epochOut) *epochOut = state ? state->standbyStashEpoch : 0;
    if (!state || state->stopped || state->lockedOut || state->driverPending)
        return false;
    if (state->suspendArmed) return true;
    return state->standbyPending && !state->standbyWriteIssued &&
           state->resumedSuspendGeneration == state->suspendGeneration;
}

// Only a repeated-failure handback (never a confirmed fan stall) that took a
// manual fan setting (curve or fixed duty) away from an owned intent has
// anything to replay.
static inline bool service_standby_fan_stash_should_capture(bool windowOpen,
    gc_u32 faultKind, bool hadIntent, const DesiredSettings* previous) {
    if (!windowOpen || faultKind != SERVICE_FAN_FAULT_REPEATED_FAILURES ||
        !hadIntent || !previous)
        return false;
    if (!previous->hasFan || previous->fanAuto) return false;
    return previous->fanMode == FAN_MODE_CURVE ||
           previous->fanMode == FAN_MODE_FIXED;
}

enum ServiceStandbyFanStashUse {
    SERVICE_STANDBY_FAN_STASH_NONE = 0,
    SERVICE_STANDBY_FAN_STASH_REPLAY,
    SERVICE_STANDBY_FAN_STASH_SUPERSEDED,
    SERVICE_STANDBY_FAN_STASH_INTENT_CHANGED,
};

static inline const char* service_standby_fan_stash_use_name(
    ServiceStandbyFanStashUse use) {
    switch (use) {
        case SERVICE_STANDBY_FAN_STASH_REPLAY: return "replay";
        case SERVICE_STANDBY_FAN_STASH_SUPERSEDED: return "superseded";
        case SERVICE_STANDBY_FAN_STASH_INTENT_CHANGED: return "intent-changed";
        default: return "none";
    }
}

// Decides what the standby restore does with a stash, given the reducer's
// current epoch.  An automatic write, explicit supersede or lockout since the
// handback (epoch moved), or any change to the owned intent, wins.
static inline ServiceStandbyFanStashUse service_standby_fan_stash_use(
    const ServiceStandbyFanStash* stash, gc_u64 currentEpoch,
    bool currentHasIntent, const DesiredSettings* current) {
    if (!stash || !stash->valid) return SERVICE_STANDBY_FAN_STASH_NONE;
    if (stash->epoch != currentEpoch) return SERVICE_STANDBY_FAN_STASH_SUPERSEDED;
    if ((stash->handbackHasIntent != 0) != currentHasIntent)
        return SERVICE_STANDBY_FAN_STASH_INTENT_CHANGED;
    if (currentHasIntent &&
        !desired_settings_equal(current, &stash->handbackIntent))
        return SERVICE_STANDBY_FAN_STASH_INTENT_CHANGED;
    return SERVICE_STANDBY_FAN_STASH_REPLAY;
}

// What the standby restore replays: the kept intent and its GPU when the stash
// replays, otherwise the owned intent.  False means there is nothing to
// restore (no owned intent and nothing kept): the restore ends without a write.
static inline bool service_standby_restore_select(bool replay,
    const ServiceStandbyFanStash* stash, bool hasActiveIntent,
    const DesiredSettings* activeIntent, const GpuAdapterInfo* activeGpu,
    const DesiredSettings** intentOut, GpuAdapterInfo* targetOut) {
    if (intentOut) *intentOut = nullptr;
    if (targetOut) *targetOut = GpuAdapterInfo{};
    if (replay && stash && stash->valid) {
        if (intentOut) *intentOut = &stash->replaced;
        if (targetOut) *targetOut = stash->replacedGpu;
        return true;
    }
    if (!hasActiveIntent || !activeIntent) return false;
    if (intentOut) *intentOut = activeIntent;
    if (targetOut && activeGpu) *targetOut = *activeGpu;
    return true;
}

// A replayed stash is used up by a restore that reached the hardware (success
// or a failed write).  A refusal before any write leaves the restore pending,
// and the stash with it.
static inline bool service_standby_fan_stash_used_up(bool replay, bool success,
    bool writeAttempted) {
    return replay && (success || writeAttempted);
}

// Saved profile attribution is restored only when a replayed standby restore
// succeeded without an error-class post-apply outcome (such as a failed fan
// invariant repair).
static inline bool service_standby_fan_stash_should_restore_profile(
    bool replay, bool applySucceeded, gc_u32 outcomeSeverity) {
    return replay && applySucceeded &&
           outcomeSeverity != (gc_u32)SERVICE_OUTCOME_SEVERITY_ERROR;
}

#endif // GREEN_CURVE_SERVICE_STANDBY_FAN_STASH_POLICY_H
