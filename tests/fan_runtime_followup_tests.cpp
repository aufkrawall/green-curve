// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Execute the production Linux worker with a deterministic scheduler. No
// sleeps or real GPU calls: waits are explicit scheduling boundaries.
#include "app_shared.h"
#include "fan_curve.h"
#include "fan_intent_policy.h"
#include "fan_runtime_policy.h"
#include "fan_fixed_maintenance_policy.h"
#include "fan_stall_policy.h"
#include "service_scoped_apply_policy.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace fan_runtime_fixture {
struct LinuxGpuState {
    NvmlApi nvml = {};
    nvmlDevice_t nvmlDevice = nullptr;
    GpuAdapterInfo selectedGpu = {};
    int fanMinPct = 0, fanMaxPct = 100;
    bool fanRangeKnown = false;
};
// The LinuxGpuController members linux_fan_runtime.h reads, over this
// fixture's reduced LinuxGpuState; the worker receives it explicitly.
struct LinuxGpuController {
    LinuxGpuState gpu;
    DesiredSettings activeDesired;
    GpuAdapterInfo activeTarget;
    bool gpuReady, hasActiveDesired, stateUncertain;
    unsigned long long gpuGeneration = 1;
    unsigned int fanFailureCount;
    unsigned long long fanWakeGeneration;
    bool fanOwnershipHandbackDeferred;
    ServiceFanFault fanFault;
};
static LinuxGpuController fixture;
// A second GPU's controller: its wakes must not touch the fixture's fan.
static LinuxGpuController otherGpu;
static int g_lock, g_fanWakeMutex, g_fanWakeCondition, g_running;
static int deferredHandbacks;
enum Schedule { IDLE, APPLY_BEFORE_WAIT, APPLY_BEFORE_POLL, NORMAL, TELEMETRY_LOST,
                // Stall schedules (12893-12897) run many polls on a simulated clock.
                STALL_FIXED, STALL_UNSUPPORTED, STALL_ZERO_RPM, STALL_REAPPLY,
                STALL_OTHER_GPU_WAKE };
static Schedule schedule;
static bool injected, afterTimedWait, stateLocked, wakeLocked;
static bool lockOrderValid, autoRefused, lockoutAutoOk;
static int waits, polls, writes, lastDuty, iterations, lockouts;
static unsigned long long nowMs, lockoutAtMs;
static int stallSamples;

static bool stall_schedule() { return schedule >= STALL_FIXED; }
static void commit_fixed(int duty) {
    fixture.hasActiveDesired = true;
    fixture.activeDesired.hasFan = true;
    fixture.activeDesired.fanMode = FAN_MODE_FIXED;
    fixture.activeDesired.fanPercent = duty;
    ++fixture.fanWakeGeneration;
}

static void commit_curve(int duty) {
    fixture.hasActiveDesired = true;
    fixture.activeDesired.hasFan = true;
    fixture.activeDesired.fanMode = FAN_MODE_CURVE;
    fan_curve_set_default(&fixture.activeDesired.fanCurve);
    for (auto& point : fixture.activeDesired.fanCurve.points)
        point.fanPercent = duty;
    ++fixture.fanWakeGeneration;
}
static void pl_mutex_lock(int*) {
    if (wakeLocked) lockOrderValid = false;
    stateLocked = true;
    if (++iterations > (stall_schedule() ? 400 : 8)) g_running = 0;
}
static void pl_mutex_unlock(int*) {
    stateLocked = false;
    if (schedule == APPLY_BEFORE_WAIT && !injected) {
        injected = true;
        commit_curve(70);
    }
    // A new Apply of the same duty lands while a stall window is open.
    if (schedule == STALL_REAPPLY && stallSamples == 3 && !injected) {
        injected = true;
        commit_fixed(40);
    }
}
static int fixture_mutex_lock(int*) { wakeLocked = true; return 0; }
static int fixture_mutex_unlock(int*) {
    wakeLocked = false;
    if (schedule == APPLY_BEFORE_POLL && afterTimedWait && !injected && polls == 1) {
        injected = true;
        commit_curve(70);
    }
    afterTimedWait = false;
    return 0;
}
static int fixture_cond_wait(int*, int*) {
    ++waits;
    g_running = 0;
    return 0;
}
// Waiting until the deadline is what moves the simulated monotonic clock.
static int fixture_cond_timedwait(int*, int*, const timespec* deadline) {
    afterTimedWait = true;
    // Another GPU's Apply broadcasts the shared condvar mid-wait without
    // advancing this controller's fanWakeGeneration: return 0 once so the
    // worker actually wakes and re-waits on the same deadline without
    // restarting its stall window.
    if (schedule == STALL_OTHER_GPU_WAKE && stallSamples == 3 && !injected) {
        injected = true;
        ++otherGpu.fanWakeGeneration;
        return 0;
    }
    const unsigned long long at = (unsigned long long)deadline->tv_sec * 1000ull +
        (unsigned long long)(deadline->tv_nsec / 1000000L);
    if (at > nowMs) nowMs = at;
    return ETIMEDOUT;
}
static int fixture_clock_gettime(int, timespec* value) {
    *value = {};
    value->tv_sec = (time_t)(nowMs / 1000ull);
    value->tv_nsec = (long)(nowMs % 1000ull) * 1000000L;
    return 0;
}
static int temperature(nvmlDevice_t, unsigned int, unsigned int* value) {
    ++polls;
    // Below the zero-RPM curve's 30C start: driver-automatic phase.
    *value = schedule == STALL_ZERO_RPM ? 20u : 80u;
    // Lost telemetry keeps failing until the worker locks itself out.
    if (schedule == TELEMETRY_LOST) return 3;
    if (stall_schedule()) return NVML_SUCCESS;
    if (schedule != APPLY_BEFORE_POLL || polls == 2) g_running = 0;
    return NVML_SUCCESS;
}
// Two fans; fan 2 turns for the first reading (proving its sensors), then
// stops.  The zero-RPM schedule keeps the driver reporting a manual 40% so
// only the worker's own driver-automatic phase keeps it from being judged.
static bool linux_backend_fan_stall_sample(LinuxGpuState*, FanStallFanSample* samples,
                                           unsigned int* fanCount) {
    *fanCount = 0;
    if (!stall_schedule()) return true;
    ++stallSamples;
    for (unsigned int fan = 0; fan < 2; ++fan) {
        FanStallFanSample s = {};
        s.policyKnown = s.policyManual = true;
        s.intentKnown = true;
        s.intentPercent = 40;
        const bool turning = fan == 0 || stallSamples == 1;
        s.rpmKnown = s.dutyKnown = schedule != STALL_UNSUPPORTED;
        s.rpm = s.rpmKnown && turning ? 1100u : 0u;
        s.reportedDuty = s.dutyKnown ? 41 : 0;
        samples[fan] = s;
    }
    *fanCount = 2;
    return true;
}
static bool linux_gpu_identity_matches(const GpuAdapterInfo*, const GpuAdapterInfo*) { return true; }
static FanFixedMaintenanceDecision linux_backend_fixed_fan_check(LinuxGpuState*, int) {
    return {false, false, "holding"};
}
static bool linux_backend_set_curve_fan_percent(LinuxGpuState*, unsigned int duty) {
    ++writes;
    lastDuty = (int)duty;
    return true;
}
static bool linux_backend_set_fan_auto(LinuxGpuState*) { return !autoRefused; }
static bool linux_backend_fans_are_auto(LinuxGpuState*) { return false; }
static bool daemon_fan_ownership_resume_deferred_handback(LinuxGpuController& ctl) {
    if (!ctl.fanOwnershipHandbackDeferred) return false;
    ctl.fanOwnershipHandbackDeferred = false;
    ++deferredHandbacks;
    return true;
}
static DesiredSettings storedUncertainDesired;
static int storeUncertainCalls;
static void dlog(const char*, ...) {}
static bool store_daemon_record(LinuxGpuController&, int, const GpuAdapterInfo*, const DesiredSettings* desired, char*, size_t) {
    ++storeUncertainCalls;
    if (desired) storedUncertainDesired = *desired;
    return true;
}
static void daemon_enter_uncertain_after_fan_lockout(LinuxGpuController& ctl, const char*, bool autoOk) {
    ++lockouts;
    lockoutAtMs = nowMs;
    lockoutAutoOk = autoOk;
    ctl.stateUncertain = true;
}
#define LINUX_DAEMON_RECORD_UNCERTAIN 1
#define pthread_mutex_lock fixture_mutex_lock
#define pthread_mutex_unlock fixture_mutex_unlock
#define pthread_cond_wait fixture_cond_wait
#define pthread_cond_timedwait fixture_cond_timedwait
#define clock_gettime fixture_clock_gettime
#if !defined(CLOCK_MONOTONIC)
#define CLOCK_MONOTONIC 1
#define GC_FIXTURE_DEFINED_MONOTONIC
#endif
#include "linux_fan_runtime.h"
#undef pthread_mutex_lock
#undef pthread_mutex_unlock
#undef pthread_cond_wait
#undef pthread_cond_timedwait
#undef clock_gettime
#undef LINUX_DAEMON_RECORD_UNCERTAIN
#if defined(GC_FIXTURE_DEFINED_MONOTONIC)
#undef CLOCK_MONOTONIC
#undef GC_FIXTURE_DEFINED_MONOTONIC
#endif

static void initialize(Schedule next) {
    fixture.gpu = {};
    fixture.gpu.nvml.getTemperature = temperature;
    fixture.activeDesired = {};
    fixture.activeTarget = {};
    fixture.gpuReady = true;
    fixture.hasActiveDesired = fixture.stateUncertain = false;
    g_running = 1;
    fixture.fanWakeGeneration = 0;
    fixture.gpuGeneration = 1;
    fixture.fanFailureCount = 0;
    fixture.fanOwnershipHandbackDeferred = false;
    fixture.fanFault = {};
    g_daemonFanFaultSerial = 0;
    deferredHandbacks = 0;
    injected = afterTimedWait = stateLocked = wakeLocked = false;
    lockOrderValid = true;
    autoRefused = lockoutAutoOk = false;
    waits = polls = writes = lastDuty = iterations = lockouts = 0;
    nowMs = lockoutAtMs = 0;
    stallSamples = 0;
    storedUncertainDesired = {};
    storeUncertainCalls = 0;
    schedule = next;
}

// The worker's stall check, 12893-12897.
static int run_stall() {
    // A proven fan stops under a fixed 40% duty: locked out as a stall on the
    // reading that completes 20 s (5 s cadence: proof at 5 s, stopped from 10 s).
    initialize(STALL_FIXED);
    commit_fixed(40);
    fan_reassert_thread(&fixture);
    if (lockouts != 1 || !lockoutAutoOk || lockoutAtMs != 30000 || writes ||
        fixture.fanFault.kind != SERVICE_FAN_FAULT_STALL || fixture.fanFault.fanIndex != 1 ||
        fixture.fanFault.commandedPercent != 40 || fixture.fanFault.stalledForMs != 20000 ||
        fixture.fanFault.handback != SERVICE_FAN_HANDBACK_DRIVER_AUTO ||
        !std::strstr(fixture.fanFault.detail, "Fan 2 did not turn at 40%") ||
        !service_fan_fault_wire_valid(&fixture.fanFault) || !lockOrderValid) return 12893;
    // No RPM channel: never judged, however long.
    initialize(STALL_UNSUPPORTED);
    commit_fixed(40);
    fan_reassert_thread(&fixture);
    if (lockouts || stallSamples < 20) return 12894;
    // A zero-RPM curve below its start temperature commands no manual duty.
    initialize(STALL_ZERO_RPM);
    commit_curve(40);
    fixture.activeDesired.fanCurve.zeroRpmEnabled = true;
    fan_reassert_thread(&fixture);
    if (lockouts || stallSamples < 20) return 12895;
    // A new Apply restarts the window: confirmed 20 s after its first stopped
    // reading (20 s), not 20 s after the first stopped reading overall (10 s).
    initialize(STALL_REAPPLY);
    commit_fixed(40);
    fan_reassert_thread(&fixture);
    if (!injected || lockouts != 1 || lockoutAtMs != 40000 ||
        fixture.fanFault.kind != SERVICE_FAN_FAULT_STALL) return 12896;
    // 14234: another GPU's wake does not restart this fan's stall window; the
    // handback lands at 30 s exactly as without it (a process-wide command
    // generation delayed it to 40 s).
    initialize(STALL_OTHER_GPU_WAKE);
    commit_fixed(40);
    fan_reassert_thread(&fixture);
    if (!injected || lockouts != 1 || lockoutAtMs != 30000 ||
        fixture.fanFault.kind != SERVICE_FAN_FAULT_STALL) return 14234;
    // The driver refuses the fan back: forced to 100%, reported as such, and
    // the owned intent switches to emergency 100% fixed rather than keeping
    // the stalled curve/duty.
    initialize(STALL_FIXED);
    autoRefused = true;
    commit_fixed(40);
    fan_reassert_thread(&fixture);
    if (lockouts != 1 || writes != 1 || lastDuty != FAN_RUNTIME_EMERGENCY_PERCENT ||
        fixture.fanFault.handback != SERVICE_FAN_HANDBACK_EMERGENCY_MAX ||
        !fixture.hasActiveDesired || !fixture.activeDesired.hasFan ||
        fixture.activeDesired.fanMode != FAN_MODE_FIXED ||
        fixture.activeDesired.fanPercent != FAN_RUNTIME_EMERGENCY_PERCENT ||
        storedUncertainDesired.fanMode != FAN_MODE_FIXED ||
        storedUncertainDesired.fanPercent != FAN_RUNTIME_EMERGENCY_PERCENT) return 12897;
    // Driver-auto lockout drops fan ownership (and drops fixture.hasActiveDesired
    // when fan was the last domain) so a subsequent non-fan Apply cannot
    // resurrect the stalled curve.
    initialize(STALL_FIXED);
    commit_curve(40);
    fixture.activeDesired.hasPowerLimit = 1;
    fixture.activeDesired.powerLimitPct = 90;
    fan_reassert_thread(&fixture);
    if (lockouts != 1 || !fixture.hasActiveDesired || fixture.activeDesired.hasFan ||
        fixture.activeDesired.fanMode != FAN_MODE_AUTO || !fixture.activeDesired.hasPowerLimit ||
        storedUncertainDesired.hasFan || !storedUncertainDesired.hasPowerLimit)
        return 12898;
    // Shared worker/snapshot runtime activity predicate: uncertainty, missing
    // target match, or dropped fan ownership all report inactive.
    if (!linux_daemon_fan_runtime_active(true, true, false, true, true, FAN_MODE_CURVE, false) ||
        linux_daemon_fan_runtime_active(true, true, true, true, true, FAN_MODE_CURVE, false) ||
        linux_daemon_fan_runtime_active(true, true, false, false, true, FAN_MODE_CURVE, false) ||
        linux_daemon_fan_runtime_active(true, true, false, true, false, FAN_MODE_CURVE, false) ||
        linux_daemon_fan_runtime_active(true, true, false, true, true, FAN_MODE_AUTO, true))
        return 12899;
    return 0;
}
static int run() {
    initialize(IDLE);
    fan_reassert_thread(&fixture);
    if (waits != 1 || polls || writes || !lockOrderValid) return 7520;
    initialize(APPLY_BEFORE_WAIT);
    fan_reassert_thread(&fixture);
    if (!injected || waits || polls != 1 || writes != 1 || lastDuty != 70 || !lockOrderValid) return 7521;
    initialize(APPLY_BEFORE_POLL);
    commit_curve(30);
    fan_reassert_thread(&fixture);
    if (!injected || waits || polls != 2 || writes != 2 || lastDuty != 70 || !lockOrderValid) return 7522;
    initialize(NORMAL);
    commit_curve(30);
    fan_reassert_thread(&fixture);
    if (waits || polls != 1 || writes != 1 || lastDuty != 30 || !lockOrderValid) return 7523;
    // Backend discovery may finish outside a request. The worker performs the
    // deferred fan handback once before normal fan maintenance.
    initialize(NORMAL);
    commit_curve(30);
    fixture.fanOwnershipHandbackDeferred = true;
    fan_reassert_thread(&fixture);
    if (deferredHandbacks != 1 || fixture.fanOwnershipHandbackDeferred ||
        polls != 1 || writes != 1 || !lockOrderValid) return 7631;
    if (fan_manual_write_confirmed(80, 80, 30, true)) return 7524;
    if (fan_manual_write_confirmed(0, 0, 30, true)) return 7525;
    if (!fan_manual_write_confirmed(80, 0, 80, true)) return 7526;
    if (!fan_manual_write_confirmed(80, 80, 0, false)) return 7527;
    if (fan_manual_write_confirmed(80, 80, 0, true)) return 7528;
    if (!fan_manual_write_confirmed(80, 80, 30, false)) return 7529;
    // Repeated telemetry loss locks the worker out: the fan goes back to the
    // driver and uncertainty is entered through the ownership helper, which
    // retires a marker the driver took the fan back for.
    initialize(TELEMETRY_LOST);
    commit_curve(30);
    fan_reassert_thread(&fixture);
    if (lockouts != 1 || !lockoutAutoOk || !fixture.stateUncertain || writes ||
        polls != (int)FAN_RUNTIME_DEFAULT_FAILURE_LIMIT || !lockOrderValid ||
        fixture.activeDesired.hasFan || fixture.hasActiveDesired ||
        storedUncertainDesired.hasFan) return 7546;
    // Clients are told (protocol v39): which kind of handback, and its outcome.
    if (fixture.fanFault.kind != SERVICE_FAN_FAULT_REPEATED_FAILURES ||
        fixture.fanFault.handback != SERVICE_FAN_HANDBACK_DRIVER_AUTO ||
        fixture.fanFault.serial != 1 || fixture.fanFault.fanIndex ||
        std::strcmp(fixture.fanFault.detail, "fan curve runtime locked out") != 0 ||
        !service_fan_fault_wire_valid(&fixture.fanFault)) return 12890;
    // A refused handback forces the emergency duty first and keeps the marker.
    initialize(TELEMETRY_LOST);
    autoRefused = true;
    commit_curve(30);
    fan_reassert_thread(&fixture);
    if (lockouts != 1 || lockoutAutoOk || writes != 1 ||
        lastDuty != FAN_RUNTIME_EMERGENCY_PERCENT) return 7547;
    if (fixture.fanFault.handback != SERVICE_FAN_HANDBACK_EMERGENCY_MAX ||
        !service_fan_fault_wire_valid(&fixture.fanFault)) return 12891;
    // A committed Apply or Reset ends the lockout's bookkeeping.
    fixture.fanFailureCount = 2;
    daemon_fan_runtime_note_commit(fixture, "test");
    if (fixture.fanFailureCount || fixture.fanFault.kind != SERVICE_FAN_FAULT_NONE) return 12892;
    // 14250: process-wide fault serials stay distinct across controllers so a
    // client observer deduplicating on (serial, kind) announces each card's
    // fault; null worker arg exits cleanly.
    g_daemonFanFaultSerial = 0;
    otherGpu = {};
    daemon_fan_fault_record(fixture, SERVICE_FAN_FAULT_STALL,
        SERVICE_FAN_HANDBACK_DRIVER_AUTO, 0, 40, 20000, "gpu0");
    daemon_fan_fault_record(otherGpu, SERVICE_FAN_FAULT_STALL,
        SERVICE_FAN_HANDBACK_DRIVER_AUTO, 0, 40, 20000, "gpu1");
    if (fixture.fanFault.serial != 1 || otherGpu.fanFault.serial != 2) return 14250;
    fan_reassert_thread(nullptr);
    return run_stall();
}
} // namespace fan_runtime_fixture

int run_fan_runtime_followup_tests() {
    int result = fan_runtime_fixture::run();
    if (result) std::fprintf(stderr, "regression assertion failed: %d\n", result);
    return result;
}
#ifdef GC_FAN_RUNTIME_FIXTURE_MAIN
int main() { return run_fan_runtime_followup_tests(); }
#endif
