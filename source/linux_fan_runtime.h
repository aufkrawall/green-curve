// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Fixed duty is re-checked on the same cadence Windows re-asserts it.
static const unsigned int LINUX_FAN_FIXED_MAINTENANCE_INTERVAL_MS = 5000u;

// Capture under g_lock, in the same lock order as Apply's notification. The
// generation must describe the state snapshot, not the later start of a wait.
static unsigned long long fan_runtime_generation() {
    pthread_mutex_lock(&g_fanWakeMutex);
    unsigned long long generation = g_fanWakeGeneration;
    pthread_mutex_unlock(&g_fanWakeMutex);
    return generation;
}

// A committed fixed duty the driver no longer holds is written again; a read
// that cannot tell counts toward the same escalation as a curve failure.
// Caller holds g_lock.
static FanRuntimeOutcome daemon_fixed_fan_maintain(const char** reasonOut) {
    int target = g_activeDesired.fanPercent;
    if (target < 0) target = 0;
    if (target > 100) target = 100;
    FanFixedMaintenanceDecision d = linux_backend_fixed_fan_check(&g_gpu, target);
    if (reasonOut) *reasonOut = d.reason;
    if (d.failure) return FAN_RUNTIME_OUTCOME_TELEMETRY_FAILED;
    if (!d.write) return FAN_RUNTIME_OUTCOME_SUCCESS;
    bool ok = linux_backend_set_curve_fan_percent(&g_gpu, (unsigned int)target);
    dlog("daemon: fixed fan re-asserted at %d%% (%s) ok=%d\n", target, d.reason,
         ok ? 1 : 0);
    return ok ? FAN_RUNTIME_OUTCOME_SUCCESS : FAN_RUNTIME_OUTCOME_WRITE_FAILED;
}

static void daemon_fan_fault_clear(const char* why) {
    if (g_daemonFanFault.kind == SERVICE_FAN_FAULT_NONE) return;
    dlog("daemon fan fault: record kind=%u serial=%u cleared by %s\n",
         (unsigned int)g_daemonFanFault.kind, (unsigned int)g_daemonFanFault.serial,
         why ? why : "unspecified");
    g_daemonFanFault = ServiceFanFault{};
}

// A committed Apply or Reset ends any worker lockout: its failure count and its
// fault record are history.  Caller holds g_lock.
static void daemon_fan_runtime_note_commit(const char* why) {
    g_fanFailureCount = 0;
    daemon_fan_fault_clear(why);
}

// The v39 record clients announce (service_fan_fault.h).  Caller holds g_lock.
static void daemon_fan_fault_record(gc_u32 kind, gc_u8 handback, unsigned int fanIndex,
                                    int commandedPercent, unsigned long long stalledForMs,
                                    const char* detail) {
    ServiceFanFault record = {};
    record.kind = kind;
    record.handback = handback;
    if (kind == SERVICE_FAN_FAULT_STALL) {
        record.fanIndex = fanIndex < MAX_GPU_FANS ? fanIndex : 0u;
        record.commandedPercent = commandedPercent < 0 ? 0u
            : (commandedPercent > 100 ? 100u : (gc_u32)commandedPercent);
        record.stalledForMs = stalledForMs > 0xFFFFFFFFull ? 0xFFFFFFFFu
                                                           : (gc_u32)stalledForMs;
    }
    gc_strlcpy(record.detail, sizeof(record.detail), detail ? detail : "");
    if (g_daemonFanFaultSerial == 0xFFFFFFFFu) g_daemonFanFaultSerial = 0;
    record.serial = ++g_daemonFanFaultSerial;
    g_daemonFanFault = record;
    dlog("daemon fan fault: recorded for clients kind=%u serial=%u fan=%u pct=%u stalledMs=%u handback=%u\n",
         (unsigned int)record.kind, (unsigned int)record.serial,
         (unsigned int)record.fanIndex, (unsigned int)record.commandedPercent,
         (unsigned int)record.stalledForMs, (unsigned int)record.handback);
}

// The worker gives the fan up: driver automatic control first, maximum cooling
// when the driver refuses it, an UNCERTAIN record, the v39 fault record, and
// uncertainty through the ownership helper (which retires the marker only when
// the driver took the fan back).  Every later fan write waits for an explicit
// Apply or Reset.  Caller holds g_lock.
static void daemon_fan_runtime_lockout(gc_u32 kind, const char* reason,
                                       unsigned int fanIndex, int commandedPercent,
                                       unsigned long long stalledForMs) {
    const char* why = reason && reason[0] ? reason : "fan runtime locked out";
    bool autoOk = linux_backend_set_fan_auto(&g_gpu);
    bool emergencyOk = false;
    if (fan_runtime_escalation_after_auto_restore(autoOk) ==
            FAN_RUNTIME_ESCALATION_EMERGENCY_MAX) {
        emergencyOk = linux_backend_set_curve_fan_percent(
            &g_gpu, (unsigned int)FAN_RUNTIME_EMERGENCY_PERCENT);
        dlog("daemon: fan runtime emergency: driver auto restore failed, forced %d%% ok=%d\n",
             FAN_RUNTIME_EMERGENCY_PERCENT, emergencyOk ? 1 : 0);
    }
    const gc_u8 handback = autoOk ? (gc_u8)SERVICE_FAN_HANDBACK_DRIVER_AUTO
        : (emergencyOk ? (gc_u8)SERVICE_FAN_HANDBACK_EMERGENCY_MAX
                       : (gc_u8)SERVICE_FAN_HANDBACK_FAILED);
    const bool hadIntent = g_hasActiveDesired;
    const int previousMode = fan_owned_intent_mode(
        hadIntent && g_activeDesired.hasFan, g_activeDesired.fanMode,
        g_activeDesired.fanAuto != 0);
    const GpuAdapterInfo recordTarget = g_activeTarget.valid
        ? g_activeTarget : g_gpu.selectedGpu;
    if (autoOk) {
        // The driver has the fan back: stop claiming the failed/stalled fan so
        // snapshots and later non-fan Applies cannot resurrect it.
        g_activeDesired.hasFan = 0;
        g_activeDesired.fanAuto = 1;
        g_activeDesired.fanMode = FAN_MODE_AUTO;
        g_activeDesired.fanPercent = 0;
    } else {
        // Driver refused auto: own the emergency duty, never the failed curve.
        if (!hadIntent) {
            g_activeDesired = {};
            g_activeDesired.lockCi = -1;
            g_activeDesired.powerLimitPct = POWER_LIMIT_DEFAULT_PCT;
            g_activeTarget = recordTarget;
        }
        g_activeDesired.hasFan = 1;
        g_activeDesired.fanAuto = 0;
        g_activeDesired.fanMode = FAN_MODE_FIXED;
        g_activeDesired.fanPercent = FAN_RUNTIME_EMERGENCY_PERCENT;
    }
    g_hasActiveDesired = service_scoped_has_owned_intent(g_activeDesired);
    if (!g_hasActiveDesired) g_activeTarget = {};
    char stateErr[256] = {};
    store_daemon_record(LINUX_DAEMON_RECORD_UNCERTAIN, &recordTarget,
                        &g_activeDesired, stateErr, sizeof(stateErr));
    dlog("daemon: %s; auto=%d handback=%u fanIntent=%d->%d owned=%d state=%s\n",
         why, autoOk ? 1 : 0, (unsigned int)handback, previousMode,
         fan_owned_intent_mode(g_hasActiveDesired && g_activeDesired.hasFan,
             g_activeDesired.fanMode, g_activeDesired.fanAuto != 0),
         g_hasActiveDesired ? 1 : 0,
         stateErr[0] ? stateErr : "persisted");
    daemon_fan_fault_record(kind, handback,
        fanIndex, commandedPercent, stalledForMs, why);
    daemon_enter_uncertain_after_fan_lockout(why, autoOk);
}

// Monotonic milliseconds: the clock the worker's waits use.
static unsigned long long daemon_fan_monotonic_ms() {
    struct timespec now = {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (unsigned long long)now.tv_sec * 1000ull +
           (unsigned long long)(now.tv_nsec / 1000000L);
}

// After a successful poll: the shared stall check (fan_stall_policy.h), on the
// readings it takes at most every 2 s.  A confirmed stall locks the worker out
// exactly like repeated failures and is reported as a stall.  `commandKey` is
// the wake generation, so every Apply or Reset starts the windows over; sensor
// proof belongs to the selected GPU's generation.  Caller holds g_lock.
static void daemon_fan_stall_check(FanStallTracker* tracker, FanStallLogLimiter* limiter,
                                   unsigned long long commandKey, bool manualCommanded,
                                   int requestedPercent) {
    const unsigned long long now = daemon_fan_monotonic_ms();
    if (!fan_stall_sample_due(tracker, now)) return;
    FanStallFanSample samples[MAX_GPU_FANS] = {};
    unsigned int fanCount = 0;
    if (!linux_backend_fan_stall_sample(&g_gpu, samples, &fanCount)) {
        if (fan_stall_note_sample_unavailable(tracker, now))
            dlog("daemon fan stall: fan read failed; open stall windows closed\n");
        return;
    }
    FanStallCommand command = {};
    command.manualCommanded = manualCommanded;
    command.rangeKnown = g_gpu.fanRangeKnown;
    command.minPercent = g_gpu.fanMinPct;
    command.commandedPercent = fan_manual_effective_percent(requestedPercent,
        g_gpu.fanMinPct, g_gpu.fanMaxPct, g_gpu.fanRangeKnown);
    const FanStallReport r = fan_stall_observe(tracker,
        (unsigned long long)g_daemonGpuGeneration, commandKey, now, &command,
        samples, fanCount);
    if (r.reset == FAN_STALL_RESET_DEVICE)
        dlog("daemon fan stall: tracking restarted for another GPU; sensor proof dropped\n");
    for (unsigned int fan = 0; fan < MAX_GPU_FANS; ++fan) {
        const unsigned int bit = 1u << fan;
        const FanStallFanSample* s = fan < fanCount ? &samples[fan] : nullptr;
        if ((r.proofGainedMask & bit) && s)
            dlog("daemon fan stall: fan %u sensors proven (rpm=%u%s measured=%d%%%s); "
                 "stall detection armed\n", fan, s->rpm, s->rpmKnown ? "" : "?",
                 s->measuredPercent, s->measuredKnown ? "" : "?");
        if (r.verdict == FAN_STALL_VERDICT_CONFIRMED && r.fanIndex == fan) continue;
        if (!((r.windowStartedMask | r.windowRestartedMask | r.windowClearedMask) & bit))
            continue;
        unsigned int suppressed = 0;
        if (!fan_stall_log_allow(limiter, fan, now, &suppressed)) continue;
        char line[192] = {};
        const int judged = s ? fan_stall_judged_percent(&command, s) : command.commandedPercent;
        if (fan_stall_format_window_event(&r, fan, s, judged, suppressed, line, sizeof(line)))
            dlog("daemon fan stall: %s\n", line);
    }
    if (r.verdict != FAN_STALL_VERDICT_CONFIRMED) return;
    const FanStallFanSample& s = samples[r.fanIndex];
    char rpmText[24] = "n/a", measuredText[24] = "n/a";
    if (r.rpmEvidence) snprintf(rpmText, sizeof(rpmText), "%u RPM", s.rpm);
    if (r.measuredEvidence) snprintf(measuredText, sizeof(measuredText), "%d%%", s.measuredPercent);
    char summary[SERVICE_FAN_FAULT_DETAIL_CHARS] = {};
    snprintf(summary, sizeof(summary), "Fan %u did not turn at %d%% for %llu s (tach %s, measured %s)",
             r.fanIndex + 1, r.judgedPercent, (r.stalledForMs + 500ull) / 1000ull,
             rpmText, measuredText);
    dlog("daemon FAN STALL: %s over %u readings; handing the fan back to the driver\n",
         summary, r.observations);
    daemon_fan_runtime_lockout(SERVICE_FAN_FAULT_STALL, summary, r.fanIndex,
                               r.judgedPercent, r.stalledForMs);
}

static pl_thread_ret fan_reassert_thread(void*) {
    FanRuntimeState runtime = {};
    FanStallTracker stall = {};
    FanStallLogLimiter stallLog = {};
    unsigned long long observedGeneration = 0;
    unsigned long long runtimeGeneration = 0;
    while (g_running) {
        unsigned int pollMs = 0;
        pl_mutex_lock(&g_lock);
        if (g_fanOwnershipHandbackDeferred && g_gpuReady) {
            const GpuAdapterInfo before = g_gpu.selectedGpu;
            if (daemon_fan_ownership_resume_deferred_handback()) {
                const GpuAdapterInfo& after = g_gpu.selectedGpu;
                const bool changed = before.valid != after.valid ||
                    (before.valid && after.valid &&
                     !linux_gpu_identity_matches(&before, &after));
                if (changed) {
                    ++g_daemonGpuGeneration;
                    dlog("daemon fan ownership: deferred handback changed the"
                         " selected GPU; generation=%llu\n",
                         (unsigned long long)g_daemonGpuGeneration);
                }
            }
        }
        observedGeneration = fan_runtime_generation();
        bool active = linux_daemon_fan_runtime_active(g_gpuReady,
            g_hasActiveDesired, g_stateUncertain,
            linux_gpu_identity_matches(&g_activeTarget, &g_gpu.selectedGpu),
            g_activeDesired.hasFan != 0, g_activeDesired.fanMode,
            g_activeDesired.fanAuto != 0);
        if (active && g_activeDesired.fanMode == FAN_MODE_FIXED) {
            pollMs = LINUX_FAN_FIXED_MAINTENANCE_INTERVAL_MS;
        } else if (active) {
            pollMs = (unsigned int)g_activeDesired.fanCurve.pollIntervalMs;
            if (pollMs < 250) pollMs = 250;
        } else {
            memset(&runtime, 0, sizeof(runtime));
        }
        pl_mutex_unlock(&g_lock);

        pthread_mutex_lock(&g_fanWakeMutex);
        if (observedGeneration != g_fanWakeGeneration) {
            pthread_mutex_unlock(&g_fanWakeMutex);
            dlog("daemon: fan runtime state changed before wait; refreshing generation=%llu\n",
                 observedGeneration);
            continue;
        }
        if (active && runtimeGeneration != observedGeneration) {
            // A successful Apply already established the new curve's initial
            // hardware policy/target.  Drop the previous curve's hysteresis
            // memory so the next poll observes that state instead of
            // reasserting an old percentage or auto/manual sub-state.
            memset(&runtime, 0, sizeof(runtime));
            runtimeGeneration = observedGeneration;
        }
        if (!g_running) {
            pthread_mutex_unlock(&g_fanWakeMutex);
            break;
        }
        if (!active) {
            while (g_running && observedGeneration == g_fanWakeGeneration)
                pthread_cond_wait(&g_fanWakeCondition, &g_fanWakeMutex);
            pthread_mutex_unlock(&g_fanWakeMutex);
            continue;
        }
        // CLOCK_MONOTONIC, paired with pthread_condattr_setclock on
        // g_fanWakeCondition.  A backwards wall-clock step — NTP correcting a
        // machine without a battery-backed RTC, a manual date change, a VM
        // resume — would otherwise stall this wait for the size of the step
        // while a manual fan duty stays pinned.
        struct timespec deadline = {};
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += (time_t)(pollMs / 1000u);
        deadline.tv_nsec += (long)(pollMs % 1000u) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        int waitResult = 0;
        while (g_running && observedGeneration == g_fanWakeGeneration &&
               waitResult != ETIMEDOUT) {
            waitResult = pthread_cond_timedwait(&g_fanWakeCondition,
                &g_fanWakeMutex, &deadline);
        }
        bool pollDue = waitResult == ETIMEDOUT && g_running &&
            observedGeneration == g_fanWakeGeneration;
        pthread_mutex_unlock(&g_fanWakeMutex);
        if (!pollDue) continue;

        pl_mutex_lock(&g_lock);
        if (observedGeneration != fan_runtime_generation()) {
            pl_mutex_unlock(&g_lock);
            dlog("daemon: fan runtime state changed before poll; discarding generation=%llu\n",
                 observedGeneration);
            continue;
        }
        active = linux_daemon_fan_runtime_active(g_gpuReady,
            g_hasActiveDesired, g_stateUncertain,
            linux_gpu_identity_matches(&g_activeTarget, &g_gpu.selectedGpu),
            g_activeDesired.hasFan != 0, g_activeDesired.fanMode,
            g_activeDesired.fanAuto != 0);
        if (active && g_activeDesired.fanMode == FAN_MODE_FIXED) {
            const char* reason = "";
            FanRuntimeOutcome outcome = daemon_fixed_fan_maintain(&reason);
            FanRuntimeFailureDecision failure = fan_runtime_observe_result(
                g_fanFailureCount, outcome, FAN_RUNTIME_DEFAULT_FAILURE_LIMIT);
            g_fanFailureCount = failure.failureCount;
            if (failure.shouldLogFailure) {
                dlog("daemon: fixed fan maintenance failed (%u/%u) target=%d%%: %s\n",
                     failure.failureCount, FAN_RUNTIME_DEFAULT_FAILURE_LIMIT,
                     g_activeDesired.fanPercent, reason ? reason : "unknown");
            }
            if (failure.escalation == FAN_RUNTIME_ESCALATION_RESTORE_AUTO) {
                daemon_fan_runtime_lockout(SERVICE_FAN_FAULT_REPEATED_FAILURES,
                    "fixed fan maintenance locked out", 0, 0, 0);
            } else if (outcome == FAN_RUNTIME_OUTCOME_SUCCESS) {
                daemon_fan_stall_check(&stall, &stallLog, observedGeneration, true,
                                       g_activeDesired.fanPercent);
            }
        } else if (active) {
            // Telemetry loss is escalated exactly like a refused write: either
            // way the curve stops tracking temperature while a manual duty
            // stays pinned.  Silently skipping the read left the fan stuck
            // forever with no journal entry.
            FanRuntimeOutcome outcome = FAN_RUNTIME_OUTCOME_TELEMETRY_FAILED;
            bool manualCommanded = false;
            int pct = -1;
            unsigned int t = 0;
            const char* reason = "temperature source unavailable";
            if (!g_gpu.nvml.getTemperature) {
                reason = "nvmlDeviceGetTemperature is unavailable";
            } else {
                nvmlReturn_t tempStatus = g_gpu.nvml.getTemperature(
                    g_gpu.nvmlDevice, NVML_TEMPERATURE_GPU, &t);
                if (tempStatus != NVML_SUCCESS) {
                    reason = "temperature read failed";
                    dlog("daemon: fan reassert temperature read failed (nvml=%d)\n",
                         (int)tempStatus);
                } else {
                    if (!runtime.initialized) {
                        runtime.initialized = true;
                        runtime.lastTemperatureC = (int)t;
                        runtime.lastPercent = fan_curve_interpolate_percent(
                            &g_activeDesired.fanCurve, (int)t);
                        runtime.driverAutoActive =
                            linux_backend_fans_are_auto(&g_gpu);
                    }
                    FanRuntimeDecision decision = fan_runtime_next_action(&runtime,
                        &g_activeDesired.fanCurve, (int)t, false);
                    pct = decision.targetPercent;
                    bool writeOk = false;
                    if (decision.useDriverAuto) {
                        writeOk = linux_backend_fans_are_auto(&g_gpu) ||
                            linux_backend_set_fan_auto(&g_gpu);
                    } else {
                        writeOk = linux_backend_set_curve_fan_percent(
                            &g_gpu, (unsigned int)pct);
                    }
                    if (writeOk) {
                        outcome = FAN_RUNTIME_OUTCOME_SUCCESS;
                        manualCommanded = !decision.useDriverAuto;
                        if (decision.controlModeChanged) {
                            dlog("daemon: fan curve transition temp=%uC control=%s "
                                 "target=%d%% curveDownshift=%dC "
                                 "zeroRpmGap=%dC start=%dC stop=%dC\n",
                                 t,
                                 decision.useDriverAuto
                                    ? "driver-auto/native-zero-RPM"
                                    : "manual",
                                 pct,
                                 g_activeDesired.fanCurve.hysteresisC,
                                 fan_curve_zero_rpm_hysteresis(
                                     &g_activeDesired.fanCurve),
                                 fan_curve_first_enabled_temperature(
                                     &g_activeDesired.fanCurve),
                                 fan_curve_first_enabled_temperature(
                                     &g_activeDesired.fanCurve) -
                                     fan_curve_zero_rpm_hysteresis(
                                         &g_activeDesired.fanCurve));
                        }
                    } else {
                        outcome = FAN_RUNTIME_OUTCOME_WRITE_FAILED;
                        reason = decision.useDriverAuto
                            ? "driver auto handback failed"
                            : "fan write failed";
                    }
                }
            }

            FanRuntimeFailureDecision failure = fan_runtime_observe_result(
                g_fanFailureCount, outcome, FAN_RUNTIME_DEFAULT_FAILURE_LIMIT);
            g_fanFailureCount = failure.failureCount;
            if (failure.shouldLogFailure) {
                dlog("daemon: fan reassert failed (%u/%u) target=%d%% temp=%uC: %s\n",
                     failure.failureCount, FAN_RUNTIME_DEFAULT_FAILURE_LIMIT,
                     pct, t, reason);
            }
            if (failure.escalation == FAN_RUNTIME_ESCALATION_RESTORE_AUTO) {
                daemon_fan_runtime_lockout(SERVICE_FAN_FAULT_REPEATED_FAILURES,
                    "fan curve runtime locked out", 0, 0, 0);
            } else if (outcome == FAN_RUNTIME_OUTCOME_SUCCESS) {
                daemon_fan_stall_check(&stall, &stallLog, observedGeneration,
                                       manualCommanded, pct);
            }
        }
        pl_mutex_unlock(&g_lock);
    }
    return PL_THREAD_RET_OK;
}
