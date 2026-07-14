// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

static pl_thread_ret fan_reassert_thread(void*) {
    FanRuntimeState runtime = {};
    unsigned long long observedGeneration = 0;
    while (g_running) {
        unsigned int pollMs = 0;
        pl_mutex_lock(&g_lock);
        bool active = g_gpuReady && g_hasActiveDesired && !g_stateUncertain &&
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
        struct timespec deadline = {};
        clock_gettime(CLOCK_REALTIME, &deadline);
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
                 g_activeDesired.hasFan &&
                 g_activeDesired.fanMode == FAN_MODE_CURVE;
        if (active && g_gpu.nvml.getTemperature) {
            unsigned int t = 0;
            if (g_gpu.nvml.getTemperature(g_gpu.nvmlDevice, NVML_TEMPERATURE_GPU, &t) == NVML_SUCCESS) {
                FanRuntimeDecision decision = fan_runtime_next_action(&runtime,
                    &g_activeDesired.fanCurve, (int)t, false);
                int pct = decision.targetPercent;
                if (linux_backend_set_curve_fan_percent(&g_gpu, (unsigned int)pct)) {
                    g_fanFailureCount = 0;
                } else {
                    ++g_fanFailureCount;
                    dlog("daemon: fan reassert failed (%u/3) target=%d%%\n", g_fanFailureCount, pct);
                    if (g_fanFailureCount >= 3) {
                        bool autoOk = linux_backend_set_fan_auto(&g_gpu);
                        char stateErr[256] = {};
                        g_stateUncertain = true;
                        store_daemon_record(LINUX_DAEMON_RECORD_UNCERTAIN, &g_activeTarget,
                                            &g_activeDesired, stateErr, sizeof(stateErr));
                        dlog("daemon: fan runtime locked out after repeated failures; auto=%d state=%s\n",
                             autoOk ? 1 : 0, stateErr[0] ? stateErr : "persisted");
                    }
                }
            }
        }
        pl_mutex_unlock(&g_lock);
    }
    return PL_THREAD_RET_OK;
}

