// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Execute the production Linux worker with a deterministic scheduler. No
// sleeps or real GPU calls: waits are explicit scheduling boundaries.
#include "app_shared.h"
#include "fan_curve.h"
#include "fan_runtime_policy.h"
#include "fan_fixed_maintenance_policy.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace fan_runtime_fixture {
struct LinuxGpuState {
    NvmlApi nvml = {};
    nvmlDevice_t nvmlDevice = nullptr;
    GpuAdapterInfo selectedGpu = {};
};
static LinuxGpuState g_gpu;
static DesiredSettings g_activeDesired;
static GpuAdapterInfo g_activeTarget;
static bool g_gpuReady, g_hasActiveDesired, g_stateUncertain;
static int g_lock, g_fanWakeMutex, g_fanWakeCondition, g_running;
static unsigned long long g_fanWakeGeneration;
static unsigned int g_fanFailureCount;
enum Schedule { IDLE, APPLY_BEFORE_WAIT, APPLY_BEFORE_POLL, NORMAL, TELEMETRY_LOST };
static Schedule schedule;
static bool injected, afterTimedWait, stateLocked, wakeLocked;
static bool lockOrderValid, autoRefused, lockoutAutoOk;
static int waits, polls, writes, lastDuty, iterations, lockouts;

static void commit_curve(int duty) {
    g_hasActiveDesired = true;
    g_activeDesired.hasFan = true;
    g_activeDesired.fanMode = FAN_MODE_CURVE;
    fan_curve_set_default(&g_activeDesired.fanCurve);
    for (auto& point : g_activeDesired.fanCurve.points)
        point.fanPercent = duty;
    ++g_fanWakeGeneration;
}
static void pl_mutex_lock(int*) {
    if (wakeLocked) lockOrderValid = false;
    stateLocked = true;
    if (++iterations > 8) g_running = 0;
}
static void pl_mutex_unlock(int*) {
    stateLocked = false;
    if (schedule == APPLY_BEFORE_WAIT && !injected) {
        injected = true;
        commit_curve(70);
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
static int fixture_cond_timedwait(int*, int*, const timespec*) {
    afterTimedWait = true;
    return ETIMEDOUT;
}
static int fixture_clock_gettime(int, timespec* value) { *value = {}; return 0; }
static int temperature(nvmlDevice_t, unsigned int, unsigned int* value) {
    ++polls;
    *value = 80;
    // Lost telemetry keeps failing until the worker locks itself out.
    if (schedule == TELEMETRY_LOST) return 3;
    if (schedule != APPLY_BEFORE_POLL || polls == 2) g_running = 0;
    return NVML_SUCCESS;
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
static void dlog(const char*, ...) {}
static bool store_daemon_record(int, const GpuAdapterInfo*, const DesiredSettings*, char*, size_t) { return true; }
static void daemon_enter_uncertain_after_fan_lockout(const char*, bool autoOk) {
    ++lockouts;
    lockoutAutoOk = autoOk;
    g_stateUncertain = true;
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
    g_gpu = {};
    g_gpu.nvml.getTemperature = temperature;
    g_activeDesired = {};
    g_activeTarget = {};
    g_gpuReady = true;
    g_hasActiveDesired = g_stateUncertain = false;
    g_running = 1;
    g_fanWakeGeneration = 0;
    g_fanFailureCount = 0;
    injected = afterTimedWait = stateLocked = wakeLocked = false;
    lockOrderValid = true;
    autoRefused = lockoutAutoOk = false;
    waits = polls = writes = lastDuty = iterations = lockouts = 0;
    schedule = next;
}
static int run() {
    initialize(IDLE);
    fan_reassert_thread(nullptr);
    if (waits != 1 || polls || writes || !lockOrderValid) return 7520;
    initialize(APPLY_BEFORE_WAIT);
    fan_reassert_thread(nullptr);
    if (!injected || waits || polls != 1 || writes != 1 || lastDuty != 70 || !lockOrderValid) return 7521;
    initialize(APPLY_BEFORE_POLL);
    commit_curve(30);
    fan_reassert_thread(nullptr);
    if (!injected || waits || polls != 2 || writes != 2 || lastDuty != 70 || !lockOrderValid) return 7522;
    initialize(NORMAL);
    commit_curve(30);
    fan_reassert_thread(nullptr);
    if (waits || polls != 1 || writes != 1 || lastDuty != 30 || !lockOrderValid) return 7523;
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
    fan_reassert_thread(nullptr);
    if (lockouts != 1 || !lockoutAutoOk || !g_stateUncertain || writes ||
        polls != (int)FAN_RUNTIME_DEFAULT_FAILURE_LIMIT || !lockOrderValid) return 7546;
    // A refused handback forces the emergency duty first and keeps the marker.
    initialize(TELEMETRY_LOST);
    autoRefused = true;
    commit_curve(30);
    fan_reassert_thread(nullptr);
    if (lockouts != 1 || lockoutAutoOk || writes != 1 ||
        lastDuty != FAN_RUNTIME_EMERGENCY_PERCENT) return 7547;
    return 0;
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
