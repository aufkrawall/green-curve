/*
SPDX-License-Identifier: MIT
Copyright (c) 2026 aufkrawall
*/

static bool service_active_desired_has_vf_intent(const DesiredSettings* desired) {
    if (!desired) return false;
    if (desired->hasLock && desired->lockCi >= 0 && desired->lockCi < VF_NUM_POINTS && desired->lockMHz > 0) {
        return true;
    }
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (desired->hasCurvePoint[ci] && desired->curvePointMHz[ci] > 0) return true;
    }
    return false;
}

static bool service_active_vf_intent_drifted(const DesiredSettings* desired, char* detail, size_t detailSize) {
    if (detail && detailSize > 0) detail[0] = 0;
    if (!desired || !service_active_desired_has_vf_intent(desired)) return false;

    if (desired->hasLock && desired->lockCi >= 0 && desired->lockCi < VF_NUM_POINTS && desired->lockMHz > 0) {
        int tailPoints = 0;
        int driftPoints = 0;
        int firstDriftCi = -1;
        int maxDriftCi = -1;
        unsigned int maxDeltaMHz = 0;
        for (int ci = desired->lockCi; ci < VF_NUM_POINTS; ci++) {
            if (!is_curve_point_visible_in_gui(ci)) continue;
            if (g_app.curve[ci].freq_kHz == 0) continue;
            tailPoints++;
            unsigned int actualMHz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
            unsigned int deltaMHz = actualMHz > desired->lockMHz
                ? actualMHz - desired->lockMHz
                : desired->lockMHz - actualMHz;
            if (deltaMHz <= SERVICE_VF_DRIFT_TOLERANCE_MHZ) continue;
            if (firstDriftCi < 0) firstDriftCi = ci;
            driftPoints++;
            if (deltaMHz > maxDeltaMHz) {
                maxDeltaMHz = deltaMHz;
                maxDriftCi = ci;
            }
        }
        if (driftPoints > 0) {
            set_message(detail, detailSize,
                "locked tail target=%uMHz points=%d drifted=%d first=ci%d max=ci%d/%uMHz",
                desired->lockMHz,
                tailPoints,
                driftPoints,
                firstDriftCi,
                maxDriftCi,
                maxDeltaMHz);
            return true;
        }
    }

    int explicitPoints = 0;
    int driftPoints = 0;
    int firstDriftCi = -1;
    int maxDriftCi = -1;
    unsigned int maxDeltaMHz = 0;
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (!desired->hasCurvePoint[ci] || desired->curvePointMHz[ci] == 0) continue;
        if (!is_curve_point_visible_in_gui(ci)) continue;
        if (g_app.curve[ci].freq_kHz == 0) continue;
        explicitPoints++;
        unsigned int actualMHz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
        unsigned int targetMHz = desired->curvePointMHz[ci];
        unsigned int deltaMHz = actualMHz > targetMHz ? actualMHz - targetMHz : targetMHz - actualMHz;
        if (deltaMHz <= SERVICE_VF_DRIFT_TOLERANCE_MHZ) continue;
        if (firstDriftCi < 0) firstDriftCi = ci;
        driftPoints++;
        if (deltaMHz > maxDeltaMHz) {
            maxDeltaMHz = deltaMHz;
            maxDriftCi = ci;
        }
    }
    if (driftPoints > 0) {
        set_message(detail, detailSize,
            "explicit VF points=%d drifted=%d first=ci%d max=ci%d/%uMHz",
            explicitPoints,
            driftPoints,
            firstDriftCi,
            maxDriftCi,
            maxDeltaMHz);
        return true;
    }
    return false;
}

static void service_check_active_vf_drift_monitor(const char* source) {
    if (!g_app.isServiceProcess) return;
    ULONGLONG now = GetTickCount64();
    if (g_serviceVfDriftLastCheckTickMs != 0 &&
        now - g_serviceVfDriftLastCheckTickMs < SERVICE_VF_DRIFT_CHECK_INTERVAL_MS) {
        return;
    }
    g_serviceVfDriftLastCheckTickMs = now;

    if (InterlockedExchangeAdd(&g_serviceGpuRecovering, 0) != 0 ||
        InterlockedExchangeAdd(&g_serviceReapplyInProgress, 0) != 0 ||
        InterlockedExchangeAdd(&g_serviceRecoveryReapplyPending, 0) != 0 ||
        nvml_crash_recovery_active()) {
        debug_log_on_change("vf drift monitor: skipped during recovery/reapply state recovering=%ld inProgress=%ld pending=%ld\n",
            (long)InterlockedExchangeAdd(&g_serviceGpuRecovering, 0),
            (long)InterlockedExchangeAdd(&g_serviceReapplyInProgress, 0),
            (long)InterlockedExchangeAdd(&g_serviceRecoveryReapplyPending, 0));
        return;
    }

    DesiredSettings desired = {};
    bool hasDesired = false;
    EnterCriticalSection(&g_appLock);
    if (g_serviceHasActiveDesired) {
        desired = g_serviceActiveDesired;
        hasDesired = service_active_desired_has_vf_intent(&desired);
    }
    LeaveCriticalSection(&g_appLock);
    if (!hasDesired) {
        g_serviceVfDriftConsecutiveSamples = 0;
        return;
    }

    bool drifted = false;
    char driftDetail[256] = {};
    char hwDetail[160] = {};
    lock_service_runtime();
    bool ready = !g_app.deviceRemoved && hardware_initialize(hwDetail, sizeof(hwDetail)) && g_app.numPopulated > 0 && g_app.loaded;
    if (ready) {
        bool offsetsOk = false;
        if (!read_live_curve_snapshot_settled(1, 0, &offsetsOk)) {
            debug_log_on_change("vf drift monitor: live VF read failed%s%s\n",
                hwDetail[0] ? ": " : "",
                hwDetail[0] ? hwDetail : "");
        } else {
            drifted = service_active_vf_intent_drifted(&desired, driftDetail, sizeof(driftDetail));
            if (!offsetsOk) {
                debug_log_on_change("vf drift monitor: live VF offsets readback unavailable; drift=%d detail=%s\n",
                    drifted ? 1 : 0,
                    driftDetail[0] ? driftDetail : "<none>");
            }
        }
    }
    unlock_service_runtime();

    if (!ready) {
        debug_log_on_change("vf drift monitor: skipped, hardware not ready%s%s\n",
            hwDetail[0] ? ": " : "",
            hwDetail[0] ? hwDetail : "");
        g_serviceVfDriftConsecutiveSamples = 0;
        return;
    }
    if (!drifted) {
        if (g_serviceVfDriftConsecutiveSamples != 0) {
            debug_log("vf drift monitor: drift cleared after %u sample(s)\n",
                g_serviceVfDriftConsecutiveSamples);
        }
        g_serviceVfDriftConsecutiveSamples = 0;
        g_serviceVfDriftLastDetail[0] = 0;
        return;
    }

    g_serviceVfDriftConsecutiveSamples++;
    bool detailChanged = strcmp(g_serviceVfDriftLastDetail, driftDetail) != 0;
    if (detailChanged) {
        StringCchCopyA(g_serviceVfDriftLastDetail, ARRAY_COUNT(g_serviceVfDriftLastDetail), driftDetail);
    }
    if (detailChanged || g_serviceVfDriftConsecutiveSamples == 1) {
        debug_log("vf drift monitor: detected drift sample %u/%u from %s: %s\n",
            g_serviceVfDriftConsecutiveSamples,
            SERVICE_VF_DRIFT_CONFIRM_SAMPLES,
            source && source[0] ? source : "service",
            driftDetail[0] ? driftDetail : "unknown drift");
    }
    if (g_serviceVfDriftConsecutiveSamples < SERVICE_VF_DRIFT_CONFIRM_SAMPLES) return;

    if (g_serviceVfDriftWindowStartTickMs == 0 ||
        now - g_serviceVfDriftWindowStartTickMs > SERVICE_VF_DRIFT_QUEUE_WINDOW_MS) {
        g_serviceVfDriftWindowStartTickMs = now;
        g_serviceVfDriftQueueCount = 0;
    }
    if (g_serviceVfDriftQueueCount >= SERVICE_VF_DRIFT_MAX_QUEUES_PER_WINDOW) {
        debug_log_on_change("vf drift monitor: reapply cap reached (%u queues in %llu ms), not queuing: %s\n",
            SERVICE_VF_DRIFT_MAX_QUEUES_PER_WINDOW,
            (unsigned long long)SERVICE_VF_DRIFT_QUEUE_WINDOW_MS,
            driftDetail[0] ? driftDetail : "unknown drift");
        return;
    }
    if (g_serviceVfDriftLastQueueTickMs != 0 &&
        now - g_serviceVfDriftLastQueueTickMs < SERVICE_VF_DRIFT_MIN_REQUEUE_MS) {
        debug_log_on_change("vf drift monitor: drift confirmed but requeue backoff active (%llums remaining): %s\n",
            (unsigned long long)(SERVICE_VF_DRIFT_MIN_REQUEUE_MS - (now - g_serviceVfDriftLastQueueTickMs)),
            driftDetail[0] ? driftDetail : "unknown drift");
        return;
    }

    g_serviceVfDriftLastQueueTickMs = now;
    g_serviceVfDriftQueueCount++;
    g_serviceVfDriftConsecutiveSamples = 0;
    debug_log("vf drift monitor: confirmed active VF drift; queueing conservative reapply (%u/%u in window): %s\n",
        g_serviceVfDriftQueueCount,
        SERVICE_VF_DRIFT_MAX_QUEUES_PER_WINDOW,
        driftDetail[0] ? driftDetail : "unknown drift");
    service_queue_recovery_reapply("VF drift monitor", 0);
}

