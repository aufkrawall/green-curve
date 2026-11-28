// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The standby window for a fan runtime handback
// (source/service_standby_fan_stash_policy.h), cases 14340-14349, driven
// through the real lifecycle reducer.  The production handback and restore
// bindings run in tests/fan_handback_runtime_tests.cpp (14350-14357).
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

int run_window_cases() {
    // No suspend yet: no window.
    ServiceLifecycleState state = {};
    gc_u64 generation = 99;
    if (service_standby_fan_handback_window(&state, &generation) || generation != 0)
        return 14340;
    // Suspended: open, tagged with the generation the resume will restore.
    feed(&state, SERVICE_LIFECYCLE_EVENT_SUSPEND);
    if (!service_standby_fan_handback_window(&state, &generation) || generation != 1)
        return 14341;
    // Resumed with the restore pending: still open, same generation.
    ServiceLifecycleDecision resumed = feed(&state, SERVICE_LIFECYCLE_EVENT_RESUME);
    if (!resumed.wakeWorker || !state.standbyPending ||
        !service_standby_fan_handback_window(&state, &generation) || generation != 1)
        return 14342;
    // The restore's own write is outside the window: a fan that fails while
    // the restore starts it is a real failure.
    ServiceLifecycleDecision start = feed(&state, SERVICE_LIFECYCLE_EVENT_STANDBY_WRITE_STARTED);
    if (!start.authorizeStandbyWrite || service_standby_fan_handback_window(&state, &generation))
        return 14343;
    // A refusal before any write keeps the restore pending, and reopens the window.
    feed(&state, SERVICE_LIFECYCLE_EVENT_STANDBY_WRITE_FINISHED, false, false);
    if (!state.standbyPending || !service_standby_fan_handback_window(&state, &generation) ||
        generation != 1)
        return 14344;
    // A written restore closes it for good.
    feed(&state, SERVICE_LIFECYCLE_EVENT_STANDBY_WRITE_STARTED);
    feed(&state, SERVICE_LIFECYCLE_EVENT_STANDBY_WRITE_FINISHED, true, true);
    if (state.standbyPending || service_standby_fan_handback_window(&state, &generation))
        return 14345;
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
    if (service_standby_fan_handback_window(&locked, &generation) ||
        service_standby_fan_handback_window(&driver, &generation) ||
        service_standby_fan_handback_window(&explicitApply, &generation))
        return 14346;
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
    if (!service_standby_fan_stash_should_capture(true, true, &curve) ||
        !service_standby_fan_stash_should_capture(true, true, &fixed) ||
        service_standby_fan_stash_should_capture(false, true, &curve) ||
        service_standby_fan_stash_should_capture(true, false, &curve) ||
        service_standby_fan_stash_should_capture(true, true, &autoFan) ||
        service_standby_fan_stash_should_capture(true, true, &noFan) ||
        service_standby_fan_stash_should_capture(true, true, nullptr))
        return 14347;

    // The intent the handback left: the curve's OC with the fan released.
    ServiceStandbyFanStash stash = {};
    stash.valid = true;
    stash.suspendGeneration = 4;
    stash.replaced = curve;
    stash.handbackHasIntent = true;
    stash.handbackIntent = noFan;
    stash.handbackIntent.fanAuto = 1;
    stash.handbackIntent.fanMode = FAN_MODE_AUTO;
    DesiredSettings current = stash.handbackIntent;
    if (service_standby_fan_stash_use(&stash, 4, true, &current) != SERVICE_STANDBY_FAN_STASH_REPLAY ||
        service_standby_fan_stash_use(nullptr, 4, true, &current) != SERVICE_STANDBY_FAN_STASH_NONE)
        return 14348;
    // Another wake, an Apply that changed anything, or a released intent: no replay.
    DesiredSettings edited = current;
    edited.gpuOffsetMHz = 120;
    ServiceStandbyFanStash zeroGeneration = stash;
    zeroGeneration.suspendGeneration = 0;
    ServiceStandbyFanStash fanOnly = stash;
    fanOnly.handbackHasIntent = false;
    fanOnly.handbackIntent = DesiredSettings{};
    if (service_standby_fan_stash_use(&stash, 5, true, &current) !=
            SERVICE_STANDBY_FAN_STASH_STALE_GENERATION ||
        service_standby_fan_stash_use(&zeroGeneration, 0, true, &current) !=
            SERVICE_STANDBY_FAN_STASH_STALE_GENERATION ||
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
    return 0;
}

} // namespace

int run_standby_fan_stash_tests() {
    int result = run_window_cases();
    if (!result) result = run_stash_cases();
    if (result) std::fprintf(stderr, "standby fan stash regression failed: %d\n", result);
    return result;
}
