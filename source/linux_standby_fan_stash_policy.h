// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#pragma once

// A Linux fan runtime lockout between a suspend and the resume restore
// (pure).  The Windows counterpart is service_standby_fan_stash_policy.h; the
// window differs because the Linux daemon has no suspend edge of its own.
//
// The fan worker keeps its place across a suspend (its waits run on
// CLOCK_MONOTONIC, which stops while the machine sleeps), so the first polls
// after the wake can reach a driver that greencurve-resume.service has not
// waited for yet (it runs After=nvidia-resume.service).  Enough failed polls
// there lock the worker out: the fan goes to the driver, the curve leaves the
// owned intent, and the daemon enters the uncertain state.  The uncertain
// gate then refused the resume restore, so clocks stayed at stock as well
// until an explicit Apply or Reset.
//
// Window: a suspend happened since the last resume restore command (or since
// the daemon started).  The kernel's suspended-time offset,
// CLOCK_BOOTTIME - CLOCK_MONOTONIC, grows only while the system sleeps, so
// comparing it with the value taken at the last resume restore says whether
// one is still ahead.  Clock steps and NTP slewing move both clocks together.
// A lockout that completes before the freeze (after nvidia-suspend.service,
// before the kernel suspends) is not recognized: the daemon only sees the
// resume.
//
// Inside the window the lockout keeps a copy of the intent it replaced.  The
// next resume restore replays it once, and the uncertainty that lockout
// entered does not refuse that restore, but only while
//   - the driver took the fan back and the UNCERTAIN record landed (the
//     state is known: driver automatic fan, the recorded intent),
//   - the daemon was not uncertain before the lockout and nothing has entered
//     uncertainty since (the uncertainty serial is unchanged), and
//   - the owned intent still equals what the lockout left.
// An explicit Apply or Reset drops the copy.  Every other gate (guard
// lockout, resume rate limit, pending ownership recovery, target ownership)
// still applies.  A curve that fails again is handed back again, now outside
// the window.

#ifndef GREEN_CURVE_LINUX_STANDBY_FAN_STASH_POLICY_H
#define GREEN_CURVE_LINUX_STANDBY_FAN_STASH_POLICY_H

#include "configured_gpu_selection.h"
#include "desired_settings_ui_policy.h"
#include "linux_gpu_uuid.h"
#include "service_fan_fault.h"

// The suspended-time offset, bracketed by two monotonic reads around one
// boot-time read: lowNs <= offset <= highNs.  A suspend moves the offset by
// its whole duration, far more than the width of a bracket.
struct LinuxSuspendMark {
    bool valid;
    unsigned long long lowNs;
    unsigned long long highNs;
};

// monotonicBefore <= (monotonic at the boot-time read) <= monotonicAfter, and
// boot time never trails monotonic time.  The lower bound clamps at zero: on
// a machine that never slept the offset is zero and the boot-time read
// trails the later monotonic read.  Readings that are out of order (a failed
// read, an impossible clock) yield an invalid mark, which never opens the
// window.
static inline LinuxSuspendMark linux_suspend_mark_from_readings(
    unsigned long long monotonicBeforeNs, unsigned long long boottimeNs,
    unsigned long long monotonicAfterNs) {
    LinuxSuspendMark mark = {};
    if (monotonicAfterNs < monotonicBeforeNs || boottimeNs < monotonicBeforeNs)
        return mark;
    mark.valid = true;
    mark.lowNs = boottimeNs > monotonicAfterNs ? boottimeNs - monotonicAfterNs : 0;
    mark.highNs = boottimeNs - monotonicBeforeNs;
    return mark;
}

// True only when the offset provably grew: some suspend completed between the
// two marks.  An invalid mark answers false (the current behaviour).
static inline bool linux_suspend_since(const LinuxSuspendMark* baseline,
                                       const LinuxSuspendMark* now) {
    if (!baseline || !now || !baseline->valid || !now->valid) return false;
    return now->lowNs > baseline->highNs;
}

struct LinuxStandbyFanStash {
    bool valid;
    // The owned intent and its target before the lockout.
    DesiredSettings replaced;
    GpuAdapterInfo replacedTarget;
    GpuLocationRole replacedRole;
    LinuxGpuUuid replacedUuid;
    // The owned intent the lockout left, to detect any later change.
    bool handbackHasIntent;
    DesiredSettings handbackIntent;
    // ctl.uncertainSerial after the lockout entered uncertainty.
    unsigned long long uncertainSerial;
};

// Only a repeated-failure lockout (never a confirmed fan stall) inside the
// window that took a manual fan (curve or fixed duty) from an owned intent,
// from a settled daemon, with the fan back under driver control and its
// UNCERTAIN record durable, has anything to replay.
static inline bool linux_standby_fan_stash_should_capture(bool windowOpen,
    gc_u32 faultKind, bool wasUncertainBefore, bool driverTookFan,
    bool recordPersisted, bool hadIntent, const DesiredSettings* previous) {
    if (!windowOpen || faultKind != SERVICE_FAN_FAULT_REPEATED_FAILURES ||
        wasUncertainBefore || !driverTookFan || !recordPersisted)
        return false;
    if (!hadIntent || !previous || !previous->hasFan || previous->fanAuto)
        return false;
    return previous->fanMode == FAN_MODE_CURVE ||
           previous->fanMode == FAN_MODE_FIXED;
}

enum LinuxStandbyFanStashUse {
    LINUX_STANDBY_FAN_STASH_NONE = 0,
    LINUX_STANDBY_FAN_STASH_REPLAY,
    LINUX_STANDBY_FAN_STASH_UNCERTAINTY_CHANGED,
    LINUX_STANDBY_FAN_STASH_INTENT_CHANGED,
};

static inline const char* linux_standby_fan_stash_use_name(
    LinuxStandbyFanStashUse use) {
    switch (use) {
        case LINUX_STANDBY_FAN_STASH_REPLAY: return "replay";
        case LINUX_STANDBY_FAN_STASH_UNCERTAINTY_CHANGED: return "uncertainty-changed";
        case LINUX_STANDBY_FAN_STASH_INTENT_CHANGED: return "intent-changed";
        default: return "none";
    }
}

// What the resume restore does with a stash.  Any uncertainty entered after
// the lockout, a daemon that is no longer uncertain (something else settled
// it), or any change to the owned intent wins over the copy.
static inline LinuxStandbyFanStashUse linux_standby_fan_stash_use(
    const LinuxStandbyFanStash* stash, unsigned long long uncertainSerial,
    bool stateUncertain, bool currentHasIntent, const DesiredSettings* current) {
    if (!stash || !stash->valid) return LINUX_STANDBY_FAN_STASH_NONE;
    if (!stateUncertain || stash->uncertainSerial != uncertainSerial)
        return LINUX_STANDBY_FAN_STASH_UNCERTAINTY_CHANGED;
    if (stash->handbackHasIntent != currentHasIntent)
        return LINUX_STANDBY_FAN_STASH_INTENT_CHANGED;
    if (currentHasIntent && !desired_settings_equal(current, &stash->handbackIntent))
        return LINUX_STANDBY_FAN_STASH_INTENT_CHANGED;
    return LINUX_STANDBY_FAN_STASH_REPLAY;
}

// The uncertainty an unattended write is judged against.  Only a replayable
// stash waives it, and only for the resume restore that replays it; the
// caller re-evaluates after backend preparation, which may enter uncertainty.
static inline bool linux_standby_fan_stash_effective_uncertain(
    bool stateUncertain, LinuxStandbyFanStashUse use) {
    return stateUncertain && use != LINUX_STANDBY_FAN_STASH_REPLAY;
}

// A replayed stash is used up by a restore that reached the hardware (success
// or a failed write).  A refusal before any write keeps it for the next
// resume restore.
static inline bool linux_standby_fan_stash_used_up(bool replay, bool success,
                                                   bool writeAttempted) {
    return replay && (success || writeAttempted);
}

#endif // GREEN_CURVE_LINUX_STANDBY_FAN_STASH_POLICY_H
