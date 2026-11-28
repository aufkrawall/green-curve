// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The standby window for a fan runtime handback
// (source/service_standby_fan_stash_policy.h) and the reducer's standby stash
// epoch, cases 14340-14349 and 14358-14365, driven through the real lifecycle
// reducer.  The production handback and restore bindings run in
// tests/fan_handback_runtime_tests.cpp (14350-14357).
#include "app_shared.h"
#include "fan_curve.h"
#include "desired_settings_ui_policy.h"
#include "service_standby_fan_stash_policy.h"

#include <cstdio>

namespace {

ServiceLifecycleDecision feed(ServiceLifecycleState* state, ServiceLifecycleEventType type,
                              bool success = false, bool writeAttempted = false) {
    ServiceLifecycleEvent event = {};
    event.type = type;
    event.success = success;
    event.writeAttempted = writeAttempted;
    return service_lifecycle_reduce(state, &event);
}

DesiredSettings curve_intent() {
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

bool window(const ServiceLifecycleState& state, gc_u64* epoch = nullptr) {
    gc_u64 ignored = 0;
    return service_standby_fan_handback_window(&state, epoch ? epoch : &ignored);
}

int run_window_cases() {
    // No suspend yet: no window.
    ServiceLifecycleState state = {};
    gc_u64 epoch = 99;
    if (window(state, &epoch) || epoch != 0) return 14340;
    // Suspended: open.
    feed(&state, SERVICE_LIFECYCLE_EVENT_SUSPEND);
    if (!window(state, &epoch) || epoch != 0) return 14341;
    // Resumed with the restore pending: still open; the duplicate resume
    // Windows sends for the same wake changes nothing.
    ServiceLifecycleDecision resumed = feed(&state, SERVICE_LIFECYCLE_EVENT_RESUME);
    ServiceLifecycleDecision duplicate = feed(&state, SERVICE_LIFECYCLE_EVENT_RESUME);
    if (!resumed.wakeWorker || !duplicate.coalesced || !state.standbyPending ||
        !window(state, &epoch) || epoch != 0)
        return 14342;
    // The restore's own write is outside the window: a fan that fails while
    // the restore starts it is a real failure.
    ServiceLifecycleDecision start = feed(&state, SERVICE_LIFECYCLE_EVENT_STANDBY_WRITE_STARTED);
    if (!start.authorizeStandbyWrite || window(state)) return 14343;
    // A refusal before any write keeps the restore pending, reopens the
    // window and keeps the epoch.
    feed(&state, SERVICE_LIFECYCLE_EVENT_STANDBY_WRITE_FINISHED, false, false);
    if (!state.standbyPending || !window(state, &epoch) || epoch != 0) return 14344;
    // A written restore closes it and moves the epoch.
    feed(&state, SERVICE_LIFECYCLE_EVENT_STANDBY_WRITE_STARTED);
    feed(&state, SERVICE_LIFECYCLE_EVENT_STANDBY_WRITE_FINISHED, true, true);
    if (state.standbyPending || window(state, &epoch) || epoch != 1) return 14345;
    // Lockout, driver recovery and an explicit supersede each close the window.
    ServiceLifecycleState locked = {};
    feed(&locked, SERVICE_LIFECYCLE_EVENT_SUSPEND);
    feed(&locked, SERVICE_LIFECYCLE_EVENT_LOCKOUT);
    ServiceLifecycleState driver = {};
    feed(&driver, SERVICE_LIFECYCLE_EVENT_SUSPEND);
    feed(&driver, SERVICE_LIFECYCLE_EVENT_RESUME);
    feed(&driver, SERVICE_LIFECYCLE_EVENT_DRIVER_RECOVERY);
    ServiceLifecycleState explicitApply = {};
    feed(&explicitApply, SERVICE_LIFECYCLE_EVENT_SUSPEND);
    feed(&explicitApply, SERVICE_LIFECYCLE_EVENT_RESUME);
    feed(&explicitApply, SERVICE_LIFECYCLE_EVENT_EXPLICIT_SUPERSEDE);
    if (window(locked) || window(driver) || window(explicitApply)) return 14346;
    return 0;
}

int run_epoch_cases() {
    // A second suspend before the restore writes (the same waking driver)
    // keeps the epoch, so a copy kept on the first wake is still replayable.
    ServiceLifecycleState state = {};
    gc_u64 epoch = 0;
    feed(&state, SERVICE_LIFECYCLE_EVENT_SUSPEND);
    feed(&state, SERVICE_LIFECYCLE_EVENT_RESUME);
    feed(&state, SERVICE_LIFECYCLE_EVENT_STANDBY_WRITE_STARTED);
    feed(&state, SERVICE_LIFECYCLE_EVENT_STANDBY_WRITE_FINISHED, false, false);
    feed(&state, SERVICE_LIFECYCLE_EVENT_SUSPEND);
    feed(&state, SERVICE_LIFECYCLE_EVENT_RESUME);
    if (!state.standbyPending || !window(state, &epoch) || epoch != 0 ||
        state.resumedSuspendGeneration != 2)
        return 14358;
    // Every event that must win over a kept copy moves the epoch: explicit
    // supersede, lockout, and an attempted logon / driver / standby write.
    ServiceLifecycleState s = {};
    feed(&s, SERVICE_LIFECYCLE_EVENT_EXPLICIT_SUPERSEDE);
    if (s.standbyStashEpoch != 1) return 14359;
    ServiceLifecycleState logon = {};
    ServiceLifecycleEvent logonEvent = {};
    logonEvent.type = SERVICE_LIFECYCLE_EVENT_WTS_LOGON;
    logonEvent.identity.valid = true;
    logonEvent.identity.sessionId = 1;
    std::snprintf(logonEvent.identity.sid, sizeof(logonEvent.identity.sid), "S-1-5-21-1");
    service_lifecycle_reduce(&logon, &logonEvent);
    logonEvent.type = SERVICE_LIFECYCLE_EVENT_LOGON_WRITE_STARTED;
    service_lifecycle_reduce(&logon, &logonEvent);
    logonEvent.type = SERVICE_LIFECYCLE_EVENT_LOGON_WRITE_FINISHED;
    logonEvent.writeAttempted = true;
    logonEvent.success = true;
    service_lifecycle_reduce(&logon, &logonEvent);
    if (logon.standbyStashEpoch != 1) return 14360;
    ServiceLifecycleState driver = {};
    ServiceLifecycleEvent driverEvent = {};
    driverEvent.type = SERVICE_LIFECYCLE_EVENT_DRIVER_RECOVERY;
    service_lifecycle_reduce(&driver, &driverEvent);
    driverEvent.type = SERVICE_LIFECYCLE_EVENT_DRIVER_WRITE_STARTED;
    driverEvent.driverProofReady = true;
    driverEvent.controlledRecoveryValidated = true;
    service_lifecycle_reduce(&driver, &driverEvent);
    driverEvent.type = SERVICE_LIFECYCLE_EVENT_DRIVER_WRITE_FINISHED;
    driverEvent.writeAttempted = true;
    service_lifecycle_reduce(&driver, &driverEvent);
    feed(&driver, SERVICE_LIFECYCLE_EVENT_LOCKOUT);
    if (driver.standbyStashEpoch != 2) return 14361;
    // A resume under lockout disarms the suspend: once an explicit Apply
    // clears the lockout the machine is awake, the window is closed, and a
    // late duplicate resume runs no standby restore for that old wake.
    ServiceLifecycleState awake = {};
    feed(&awake, SERVICE_LIFECYCLE_EVENT_SUSPEND);
    feed(&awake, SERVICE_LIFECYCLE_EVENT_LOCKOUT);
    ServiceLifecycleDecision lockedResume = feed(&awake, SERVICE_LIFECYCLE_EVENT_RESUME);
    awake.lockedOut = false;  // what a successful explicit Apply does
    ServiceLifecycleDecision late = feed(&awake, SERVICE_LIFECYCLE_EVENT_RESUME);
    if (awake.suspendArmed || lockedResume.wakeWorker || late.wakeWorker ||
        awake.standbyPending || window(awake))
        return 14362;
    return 0;
}

int run_stash_cases() {
    // Only a manual fan taken from an owned intent inside the window is kept.
    DesiredSettings curve = curve_intent();
    DesiredSettings fixed = curve;
    fixed.fanMode = FAN_MODE_FIXED;
    fixed.fanPercent = 55;
    DesiredSettings autoFan = curve;
    autoFan.fanAuto = 1;
    autoFan.fanMode = FAN_MODE_AUTO;
    DesiredSettings noFan = curve;
    noFan.hasFan = 0;
    if (!service_standby_fan_stash_should_capture(
            true, SERVICE_FAN_FAULT_REPEATED_FAILURES, true, &curve) ||
        !service_standby_fan_stash_should_capture(
            true, SERVICE_FAN_FAULT_REPEATED_FAILURES, true, &fixed) ||
        service_standby_fan_stash_should_capture(
            false, SERVICE_FAN_FAULT_REPEATED_FAILURES, true, &curve) ||
        service_standby_fan_stash_should_capture(
            true, SERVICE_FAN_FAULT_STALL, true, &curve) ||
        service_standby_fan_stash_should_capture(
            true, SERVICE_FAN_FAULT_NONE, true, &curve) ||
        service_standby_fan_stash_should_capture(
            true, SERVICE_FAN_FAULT_REPEATED_FAILURES, false, &curve) ||
        service_standby_fan_stash_should_capture(
            true, SERVICE_FAN_FAULT_REPEATED_FAILURES, true, &autoFan) ||
        service_standby_fan_stash_should_capture(
            true, SERVICE_FAN_FAULT_REPEATED_FAILURES, true, &noFan) ||
        service_standby_fan_stash_should_capture(
            true, SERVICE_FAN_FAULT_REPEATED_FAILURES, true, nullptr))
        return 14347;

    // The intent the handback left: the curve's OC with the fan released.
    ServiceStandbyFanStash stash = {};
    stash.valid = true;
    stash.epoch = 4;
    stash.replaced = curve;
    stash.replacedGpu.valid = true;
    stash.replacedGpu.pciBus = 3;
    stash.handbackHasIntent = true;
    stash.handbackIntent = noFan;
    stash.handbackIntent.fanAuto = 1;
    stash.handbackIntent.fanMode = FAN_MODE_AUTO;
    DesiredSettings current = stash.handbackIntent;
    if (service_standby_fan_stash_use(&stash, 4, true, &current) != SERVICE_STANDBY_FAN_STASH_REPLAY ||
        service_standby_fan_stash_use(nullptr, 4, true, &current) != SERVICE_STANDBY_FAN_STASH_NONE)
        return 14348;
    // A moved epoch, an intent that changed, or a released intent: no replay.
    DesiredSettings edited = current;
    edited.gpuOffsetMHz = 120;
    ServiceStandbyFanStash fanOnly = stash;
    fanOnly.handbackHasIntent = false;
    fanOnly.handbackIntent = DesiredSettings{};
    if (service_standby_fan_stash_use(&stash, 5, true, &current) !=
            SERVICE_STANDBY_FAN_STASH_SUPERSEDED ||
        service_standby_fan_stash_use(&stash, 4, true, &edited) !=
            SERVICE_STANDBY_FAN_STASH_INTENT_CHANGED ||
        service_standby_fan_stash_use(&stash, 4, false, &current) !=
            SERVICE_STANDBY_FAN_STASH_INTENT_CHANGED ||
        // A fan-only profile: the handback released everything, and nothing
        // has claimed anything since.
        service_standby_fan_stash_use(&fanOnly, 4, false, &current) !=
            SERVICE_STANDBY_FAN_STASH_REPLAY ||
        service_standby_fan_stash_use(&fanOnly, 4, true, &current) !=
            SERVICE_STANDBY_FAN_STASH_INTENT_CHANGED)
        return 14349;

    // The restore replays the kept intent and its GPU, even when nothing is
    // owned any more (the fan-only case); without a replay it restores the
    // owned intent, and with neither there is nothing to write.
    const DesiredSettings* chosen = nullptr;
    GpuAdapterInfo target = {};
    GpuAdapterInfo activeGpu = {};
    activeGpu.valid = true;
    activeGpu.pciBus = 7;
    if (!service_standby_restore_select(true, &fanOnly, false, &current, &activeGpu,
            &chosen, &target) ||
        chosen != &fanOnly.replaced || target.pciBus != 3)
        return 14363;
    if (!service_standby_restore_select(false, &fanOnly, true, &current, &activeGpu,
            &chosen, &target) ||
        chosen != &current || target.pciBus != 7 ||
        service_standby_restore_select(false, &fanOnly, false, &current, &activeGpu,
            &chosen, &target) ||
        chosen != nullptr || target.valid)
        return 14364;
    // The kept copy is used up only by a restore that reached the hardware.
    if (!service_standby_fan_stash_used_up(true, true, true) ||
        !service_standby_fan_stash_used_up(true, false, true) ||
        service_standby_fan_stash_used_up(true, false, false) ||
        service_standby_fan_stash_used_up(false, true, true))
        return 14365;
    // Saved profile attribution is restored only when a replayed restore
    // succeeded without an error-class post-apply severity.
    if (!service_standby_fan_stash_should_restore_profile(
            true, true, SERVICE_OUTCOME_SEVERITY_SUCCESS) ||
        !service_standby_fan_stash_should_restore_profile(
            true, true, SERVICE_OUTCOME_SEVERITY_WARNING) ||
        service_standby_fan_stash_should_restore_profile(
            true, true, SERVICE_OUTCOME_SEVERITY_ERROR) ||
        service_standby_fan_stash_should_restore_profile(
            false, true, SERVICE_OUTCOME_SEVERITY_SUCCESS) ||
        service_standby_fan_stash_should_restore_profile(
            true, false, SERVICE_OUTCOME_SEVERITY_SUCCESS))
        return 14416;
    return 0;
}

} // namespace

int run_standby_fan_stash_tests() {
    int result = run_window_cases();
    if (!result) result = run_epoch_cases();
    if (!result) result = run_stash_cases();
    if (result) std::fprintf(stderr, "standby fan stash regression failed: %d\n", result);
    return result;
}
