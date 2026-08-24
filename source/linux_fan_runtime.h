// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

static pl_thread_ret fan_reassert_thread(void*) {
    FanRuntimeState runtime = {};
    unsigned long long observedGeneration = 0;
    unsigned long long runtimeGeneration = 0;
    while (g_running) {
        unsigned int pollMs = 0;
        pl_mutex_lock(&g_lock);
        bool active = g_gpuReady && g_hasActiveDesired && !g_stateUncertain &&
                      linux_gpu_identity_matches(
                          &g_activeTarget, &g_gpu.selectedGpu) &&
                      g_activeDesired.hasFan &&
                      g_activeDesired.fanMode == FAN_MODE_CURVE;
        if (active) {
            pollMs = (unsigned int)g_activeDesired.fanCurve.pollIntervalMs;
            if (pollMs < 250) pollMs = 250;
        } else {
            memset(&runtime, 0, sizeof(runtime));
        }
        pl_mutex_unlock(&g_lock);

        pthread_mutex_lock(&g_fanWakeMutex);
        observedGeneration = g_fanWakeGeneration;
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
        active = g_gpuReady && g_hasActiveDesired && !g_stateUncertain &&
                 linux_gpu_identity_matches(
                     &g_activeTarget, &g_gpu.selectedGpu) &&
                 g_activeDesired.hasFan &&
                 g_activeDesired.fanMode == FAN_MODE_CURVE;
        if (active) {
            // Telemetry loss is escalated exactly like a refused write: either
            // way the curve stops tracking temperature while a manual duty
            // stays pinned.  Silently skipping the read left the fan stuck
            // forever with no journal entry.
            FanRuntimeOutcome outcome = FAN_RUNTIME_OUTCOME_TELEMETRY_FAILED;
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
                bool autoOk = linux_backend_set_fan_auto(&g_gpu);
                char stateErr[256] = {};
                g_stateUncertain = true;
                store_daemon_record(LINUX_DAEMON_RECORD_UNCERTAIN, &g_activeTarget,
                                    &g_activeDesired, stateErr, sizeof(stateErr));
                dlog("daemon: fan runtime locked out after repeated failures; auto=%d state=%s\n",
                     autoOk ? 1 : 0, stateErr[0] ? stateErr : "persisted");
                if (fan_runtime_escalation_after_auto_restore(autoOk) ==
                        FAN_RUNTIME_ESCALATION_EMERGENCY_MAX) {
                    bool emergencyOk = linux_backend_set_curve_fan_percent(
                        &g_gpu, (unsigned int)FAN_RUNTIME_EMERGENCY_PERCENT);
                    dlog("daemon: fan runtime emergency: driver auto restore failed, forced %d%% ok=%d\n",
                         FAN_RUNTIME_EMERGENCY_PERCENT, emergencyOk ? 1 : 0);
                }
            }
        }
        pl_mutex_unlock(&g_lock);
    }
    return PL_THREAD_RET_OK;
}
