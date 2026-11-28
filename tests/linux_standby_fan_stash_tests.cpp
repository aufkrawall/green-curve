// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// A Linux fan runtime lockout between a suspend and the resume restore
// (source/linux_standby_fan_stash_policy.h, source/linux_standby_fan_stash.h),
// cases 14373-14399.  The pure rules run directly; the daemon side runs over
// the real LinuxGpuController with a simulated monotonic/boot-time clock.  The
// lockout's call into it is covered by tests/fan_runtime_followup_tests.cpp
// (14370-14372).
#include "app_shared.h"
#include "fan_curve.h"
#include "fan_intent_policy.h"
#include "linux_backend.h"
#include "linux_daemon_state.h"
#include "linux_gpu_controller.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace linux_standby_fan_stash_fixture {

static unsigned long long monotonicNs, suspendedNs;
static bool clockFails;
static int fixture_clock_gettime(int clock, timespec* value) {
    if (clockFails) return -1;
    const unsigned long long ns = clock == 7 ? monotonicNs + suspendedNs : monotonicNs;
    value->tv_sec = (time_t)(ns / 1000000000ull);
    value->tv_nsec = (long)(ns % 1000000000ull);
    // Each read advances the monotonic clock a little, like a real one.
    monotonicNs += 1000;
    return 0;
}
static void dlog(const char*, ...) {}

#define clock_gettime fixture_clock_gettime
#pragma push_macro("CLOCK_MONOTONIC")
#pragma push_macro("CLOCK_BOOTTIME")
#undef CLOCK_MONOTONIC
#undef CLOCK_BOOTTIME
#define CLOCK_MONOTONIC 1
#define CLOCK_BOOTTIME 7
#include "linux_standby_fan_stash.h"
#undef clock_gettime
#pragma pop_macro("CLOCK_BOOTTIME")
#pragma pop_macro("CLOCK_MONOTONIC")

static DesiredSettings curve_intent() {
    DesiredSettings d = {};
    d.lockCi = -1;
    d.powerLimitPct = POWER_LIMIT_DEFAULT_PCT;
    d.hasFan = 1;
    d.fanMode = FAN_MODE_CURVE;
    fan_curve_set_default(&d.fanCurve);
    d.hasGpuOffset = 1;
    d.gpuOffsetMHz = 150;
    return d;
}

// The owned intent after a driver-auto lockout of curve_intent().
static DesiredSettings after_handback(DesiredSettings d) {
    d.hasFan = 0;
    d.fanAuto = 1;
    d.fanMode = FAN_MODE_AUTO;
    d.fanPercent = 0;
    return d;
}

static int run_pure() {
    // Bracketed readings: out-of-order readings never open the window.
    LinuxSuspendMark a = linux_suspend_mark_from_readings(100, 1100, 102);
    if (!a.valid || a.lowNs != 998 || a.highNs != 1000) return 14373;
    if (linux_suspend_mark_from_readings(102, 1100, 100).valid ||
        linux_suspend_mark_from_readings(100, 99, 102).valid) return 14374;
    // A machine that never slept: the boot-time read trails the later
    // monotonic read; the offset is bracketed from zero and two such marks are
    // not a suspend.
    LinuxSuspendMark awake1 = linux_suspend_mark_from_readings(100, 101, 102);
    LinuxSuspendMark awake2 = linux_suspend_mark_from_readings(900000, 900001, 900002);
    if (!awake1.valid || awake1.lowNs != 0 || awake1.highNs != 1 ||
        !awake2.valid || linux_suspend_since(&awake1, &awake2)) return 14374;
    // A later mark with the same offset (only jitter) is not a suspend; a grown
    // offset is; an invalid mark on either side is not.
    LinuxSuspendMark same = linux_suspend_mark_from_readings(5000, 6000, 5003);
    LinuxSuspendMark slept = linux_suspend_mark_from_readings(9000, 2000000000ull, 9002);
    if (linux_suspend_since(&a, &same) || !linux_suspend_since(&a, &slept) ||
        linux_suspend_since(&slept, &a)) return 14375;
    LinuxSuspendMark invalid = {};
    if (linux_suspend_since(&invalid, &slept) || linux_suspend_since(&a, &invalid) ||
        linux_suspend_since(nullptr, &slept)) return 14376;

    // Capture: only a manual fan from an owned intent, inside the window, from
    // a settled daemon, with the driver holding the fan and the record durable.
    const DesiredSettings curve = curve_intent();
    if (!linux_standby_fan_stash_should_capture(true, false, true, true, true, &curve))
        return 14377;
    if (linux_standby_fan_stash_should_capture(false, false, true, true, true, &curve) ||
        linux_standby_fan_stash_should_capture(true, true, true, true, true, &curve) ||
        linux_standby_fan_stash_should_capture(true, false, false, true, true, &curve) ||
        linux_standby_fan_stash_should_capture(true, false, true, false, true, &curve) ||
        linux_standby_fan_stash_should_capture(true, false, true, true, false, &curve))
        return 14378;
    DesiredSettings fixed = curve;
    fixed.fanMode = FAN_MODE_FIXED;
    fixed.fanPercent = 55;
    DesiredSettings autoFan = curve;
    autoFan.fanAuto = 1;
    autoFan.fanMode = FAN_MODE_AUTO;
    DesiredSettings noFan = curve;
    noFan.hasFan = 0;
    if (!linux_standby_fan_stash_should_capture(true, false, true, true, true, &fixed) ||
        linux_standby_fan_stash_should_capture(true, false, true, true, true, &autoFan) ||
        linux_standby_fan_stash_should_capture(true, false, true, true, true, &noFan))
        return 14379;

    // Use: replay only while uncertain from this lockout alone and the owned
    // intent is still what the lockout left.
    LinuxStandbyFanStash stash = {};
    if (linux_standby_fan_stash_use(&stash, 1, true, false, nullptr) != LINUX_STANDBY_FAN_STASH_NONE)
        return 14380;
    stash.valid = true;
    stash.replaced = curve;
    stash.handbackHasIntent = true;
    stash.handbackIntent = after_handback(curve);
    stash.uncertainSerial = 4;
    const DesiredSettings left = after_handback(curve);
    if (linux_standby_fan_stash_use(&stash, 4, true, true, &left) != LINUX_STANDBY_FAN_STASH_REPLAY)
        return 14381;
    if (linux_standby_fan_stash_use(&stash, 5, true, true, &left) !=
            LINUX_STANDBY_FAN_STASH_UNCERTAINTY_CHANGED ||
        linux_standby_fan_stash_use(&stash, 4, false, true, &left) !=
            LINUX_STANDBY_FAN_STASH_UNCERTAINTY_CHANGED)
        return 14382;
    DesiredSettings changed = left;
    changed.gpuOffsetMHz = 90;
    if (linux_standby_fan_stash_use(&stash, 4, true, true, &changed) !=
            LINUX_STANDBY_FAN_STASH_INTENT_CHANGED ||
        linux_standby_fan_stash_use(&stash, 4, true, false, nullptr) !=
            LINUX_STANDBY_FAN_STASH_INTENT_CHANGED)
        return 14383;
    // The waiver: only a replay passes the uncertainty; a settled daemon
    // stays settled.
    if (linux_standby_fan_stash_effective_uncertain(true, LINUX_STANDBY_FAN_STASH_REPLAY) ||
        !linux_standby_fan_stash_effective_uncertain(true, LINUX_STANDBY_FAN_STASH_NONE) ||
        !linux_standby_fan_stash_effective_uncertain(true, LINUX_STANDBY_FAN_STASH_INTENT_CHANGED) ||
        !linux_standby_fan_stash_effective_uncertain(true, LINUX_STANDBY_FAN_STASH_UNCERTAINTY_CHANGED) ||
        linux_standby_fan_stash_effective_uncertain(false, LINUX_STANDBY_FAN_STASH_NONE))
        return 14384;
    // Used up by a restore that reached the hardware, kept by a refusal.
    if (!linux_standby_fan_stash_used_up(true, true, true) ||
        !linux_standby_fan_stash_used_up(true, false, true) ||
        linux_standby_fan_stash_used_up(true, false, false) ||
        linux_standby_fan_stash_used_up(false, true, true)) return 14385;
    if (std::strcmp(linux_standby_fan_stash_use_name(LINUX_STANDBY_FAN_STASH_REPLAY), "replay") != 0)
        return 14386;
    return 0;
}

static LinuxGpuController ctl;

static void reset_daemon() {
    linux_gpu_controller_init(&ctl);
    monotonicNs = 5000000000ull;
    suspendedNs = 0;
    clockFails = false;
}

// The lockout as linux_fan_runtime.h runs it: intent rewritten, uncertainty
// entered (serial bumped), then the stash note.
static void lockout(bool driverTookFan = true, bool recordPersisted = true) {
    const DesiredSettings previous = ctl.activeDesired;
    const GpuAdapterInfo previousTarget = ctl.activeTarget;
    const bool wasUncertain = ctl.stateUncertain;
    const bool hadIntent = ctl.hasActiveDesired;
    ctl.activeDesired = after_handback(ctl.activeDesired);
    ctl.stateUncertain = true;
    ++ctl.uncertainSerial;
    daemon_standby_fan_stash_note_lockout(ctl, hadIntent, &previous, &previousTarget,
                                          wasUncertain, driverTookFan, recordPersisted);
}

static void commit_curve() {
    ctl.activeDesired = curve_intent();
    ctl.hasActiveDesired = true;
    ctl.activeTarget = {};
    ctl.activeTarget.valid = true;
    ctl.activeTarget.pciBus = 1;
    ctl.activeTargetRole = GPU_LOCATION_ROLE_ADVISORY;
    std::snprintf(ctl.activeTargetUuid.text, sizeof(ctl.activeTargetUuid.text), "GPU-fixture");
    ctl.stateUncertain = false;
}

static int run_daemon() {
    // A lockout with no suspend since daemon start is a real failure: nothing
    // kept, the uncertain gate stays.
    reset_daemon();
    daemon_standby_window_close(ctl, "daemon start");
    if (!ctl.resumeRestoreSuspendMark.valid) return 14387;
    commit_curve();
    monotonicNs += 3000000000ull;
    lockout();
    if (ctl.standbyFanStash.valid) return 14388;
    if (daemon_standby_fan_stash_peek(ctl) != LINUX_STANDBY_FAN_STASH_NONE) return 14389;

    // After a suspend, before the resume restore: the curve, its target, role
    // and UUID are kept and replayable.
    reset_daemon();
    daemon_standby_window_close(ctl, "daemon start");
    commit_curve();
    suspendedNs += 30ull * 1000000000ull;
    lockout();
    const LinuxStandbyFanStash& kept = ctl.standbyFanStash;
    if (!kept.valid || kept.replaced.fanMode != FAN_MODE_CURVE || !kept.replaced.hasFan ||
        !kept.replacedTarget.valid || kept.replacedTarget.pciBus != 1 ||
        kept.replacedRole != GPU_LOCATION_ROLE_ADVISORY ||
        std::strcmp(kept.replacedUuid.text, "GPU-fixture") != 0 ||
        !kept.handbackHasIntent || kept.handbackIntent.hasFan ||
        kept.uncertainSerial != ctl.uncertainSerial) return 14390;
    if (daemon_standby_fan_stash_peek(ctl) != LINUX_STANDBY_FAN_STASH_REPLAY ||
        !ctl.standbyFanStash.valid) return 14391;
    // The resume restore closes the window: a later lockout in this wake is a
    // real failure and drops the copy.
    daemon_standby_window_close(ctl, "the resume restore");
    lockout();
    if (ctl.standbyFanStash.valid) return 14392;

    // Uncertainty entered for another reason after the lockout drops the copy
    // at the restore instead of waiving it.
    reset_daemon();
    daemon_standby_window_close(ctl, "daemon start");
    commit_curve();
    suspendedNs += 1000000000ull;
    lockout();
    ++ctl.uncertainSerial;
    if (daemon_standby_fan_stash_peek(ctl) != LINUX_STANDBY_FAN_STASH_UNCERTAINTY_CHANGED ||
        ctl.standbyFanStash.valid) return 14393;

    // A refused driver handback or an UNCERTAIN record that did not land keeps
    // nothing: the state the waiver relies on is not known.
    reset_daemon();
    daemon_standby_window_close(ctl, "daemon start");
    commit_curve();
    suspendedNs += 1000000000ull;
    lockout(false, true);
    if (ctl.standbyFanStash.valid) return 14394;
    commit_curve();
    lockout(true, false);
    if (ctl.standbyFanStash.valid) return 14395;

    // An explicit Apply/Reset drops the copy (linux_daemon.cpp clears it).
    commit_curve();
    lockout();
    if (!ctl.standbyFanStash.valid) return 14396;
    daemon_standby_fan_stash_clear(ctl, "an explicit Apply");
    if (ctl.standbyFanStash.valid) return 14397;

    // A clock that cannot be read keeps the window closed.
    reset_daemon();
    clockFails = true;
    daemon_standby_window_close(ctl, "daemon start");
    commit_curve();
    suspendedNs += 1000000000ull;
    lockout();
    if (ctl.resumeRestoreSuspendMark.valid || ctl.standbyFanStash.valid) return 14398;
    // A second suspend before the restore keeps a copy taken after the first.
    reset_daemon();
    daemon_standby_window_close(ctl, "daemon start");
    commit_curve();
    suspendedNs += 1000000000ull;
    lockout();
    suspendedNs += 1000000000ull;
    if (daemon_standby_fan_stash_peek(ctl) != LINUX_STANDBY_FAN_STASH_REPLAY) return 14399;
    return 0;
}

} // namespace linux_standby_fan_stash_fixture

int run_linux_standby_fan_stash_tests() {
    int result = linux_standby_fan_stash_fixture::run_pure();
    if (!result) result = linux_standby_fan_stash_fixture::run_daemon();
    if (result) std::fprintf(stderr, "regression assertion failed: %d\n", result);
    return result;
}
