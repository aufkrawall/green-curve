// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Daemon side of linux_standby_fan_stash_policy.h: the suspend window the
// resume restore closes, and the copy a fan runtime lockout inside it keeps.
// Included by linux_daemon.cpp after linux_fan_ownership.h and before the fan
// worker (linux_fan_runtime.h) and the resume handler
// (linux_auto_restore_runtime.h), which call into it.  Everything here runs
// under g_lock.

#ifndef GREEN_CURVE_LINUX_STANDBY_FAN_STASH_H
#define GREEN_CURVE_LINUX_STANDBY_FAN_STASH_H

#include "linux_standby_fan_stash_policy.h"

static unsigned long long daemon_timespec_ns(const struct timespec& value) {
    return (unsigned long long)value.tv_sec * 1000000000ull +
           (unsigned long long)value.tv_nsec;
}

// The kernel's suspended-time offset now.  A failed read gives an invalid
// mark, which keeps the window closed (the behaviour before the stash).
static LinuxSuspendMark daemon_suspend_mark_now() {
    struct timespec before = {}, boot = {}, after = {};
    if (clock_gettime(CLOCK_MONOTONIC, &before) != 0 ||
        clock_gettime(CLOCK_BOOTTIME, &boot) != 0 ||
        clock_gettime(CLOCK_MONOTONIC, &after) != 0) {
        dlog("daemon standby fan stash: suspend clock read failed (errno=%d); window stays closed\n",
             errno);
        return LinuxSuspendMark{};
    }
    return linux_suspend_mark_from_readings(daemon_timespec_ns(before),
        daemon_timespec_ns(boot), daemon_timespec_ns(after));
}

// Daemon start and every resume restore command: no restore is ahead until
// the next suspend.
static void daemon_standby_window_close(LinuxGpuController& ctl, const char* why) {
    const LinuxSuspendMark previous = ctl.resumeRestoreSuspendMark;
    ctl.resumeRestoreSuspendMark = daemon_suspend_mark_now();
    dlog("daemon standby fan stash: window closed by %s (suspendedSinceLast=%d "
         "offsetMs=%llu valid=%d)\n", why ? why : "unspecified",
         linux_suspend_since(&previous, &ctl.resumeRestoreSuspendMark) ? 1 : 0,
         ctl.resumeRestoreSuspendMark.lowNs / 1000000ull,
         ctl.resumeRestoreSuspendMark.valid ? 1 : 0);
}

static void daemon_standby_fan_stash_clear(LinuxGpuController& ctl, const char* reason) {
    if (!ctl.standbyFanStash.valid) return;
    dlog("daemon standby fan stash: dropped (serial=%llu) by %s\n",
         ctl.standbyFanStash.uncertainSerial, reason ? reason : "unspecified");
    ctl.standbyFanStash = LinuxStandbyFanStash{};
}

// Called by the fan runtime lockout after it entered uncertainty.  The
// lockout itself is unchanged; this only decides whether the next resume
// restore may replay what it replaced.
static void daemon_standby_fan_stash_note_lockout(LinuxGpuController& ctl, bool hadIntent,
    const DesiredSettings* previous, const GpuAdapterInfo* previousTarget,
    bool wasUncertainBefore, bool driverTookFan, bool recordPersisted) {
    const LinuxSuspendMark now = daemon_suspend_mark_now();
    const bool windowOpen = linux_suspend_since(&ctl.resumeRestoreSuspendMark, &now);
    const bool capture = linux_standby_fan_stash_should_capture(windowOpen,
        wasUncertainBefore, driverTookFan, recordPersisted, hadIntent, previous);
    if (capture) {
        LinuxStandbyFanStash stash = {};
        stash.valid = true;
        stash.replaced = *previous;
        stash.replacedTarget = previousTarget ? *previousTarget : GpuAdapterInfo{};
        stash.replacedRole = ctl.activeTargetRole;
        stash.replacedUuid = ctl.activeTargetUuid;
        stash.handbackHasIntent = ctl.hasActiveDesired;
        stash.handbackIntent = ctl.activeDesired;
        stash.uncertainSerial = ctl.uncertainSerial;
        ctl.standbyFanStash = stash;
    } else {
        daemon_standby_fan_stash_clear(ctl, windowOpen
            ? "a lockout inside the window that keeps nothing to replay"
            : "a lockout outside the suspend window");
    }
    dlog("daemon standby fan stash: lockout window=%d captured=%d wasUncertain=%d "
         "driverTookFan=%d recordPersisted=%d previousMode=%d serial=%llu\n",
         windowOpen ? 1 : 0, capture ? 1 : 0, wasUncertainBefore ? 1 : 0,
         driverTookFan ? 1 : 0, recordPersisted ? 1 : 0,
         fan_owned_intent_mode(hadIntent && previous && previous->hasFan,
             previous ? previous->fanMode : FAN_MODE_AUTO,
             previous && previous->fanAuto != 0),
         ctl.uncertainSerial);
}

// What the resume restore does with the stash now.  A copy that can no longer
// replay is dropped here; a replayable one stays until the restore reaches
// the hardware.
static LinuxStandbyFanStashUse daemon_standby_fan_stash_peek(LinuxGpuController& ctl) {
    const LinuxStandbyFanStashUse use = linux_standby_fan_stash_use(
        &ctl.standbyFanStash, ctl.uncertainSerial, ctl.stateUncertain,
        ctl.hasActiveDesired, &ctl.activeDesired);
    if (use != LINUX_STANDBY_FAN_STASH_NONE && use != LINUX_STANDBY_FAN_STASH_REPLAY) {
        dlog("daemon standby fan stash: not replayed (%s; stash serial=%llu current=%llu "
             "uncertain=%d)\n", linux_standby_fan_stash_use_name(use),
             ctl.standbyFanStash.uncertainSerial, ctl.uncertainSerial,
             ctl.stateUncertain ? 1 : 0);
        daemon_standby_fan_stash_clear(ctl, linux_standby_fan_stash_use_name(use));
    }
    return use;
}

#endif // GREEN_CURVE_LINUX_STANDBY_FAN_STASH_H
