// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

static void log_locked_tail_drift_diagnostics() {
    // GUI-side diagnostic during normal runtime.  The service-side VF drift
    // monitor separately queues a conservative reapply only after confirmed
    // active-desired drift and recovery/backoff gates pass.
    if (g_app.lockedCi < 0 || g_app.lockedFreq == 0 || g_app.lockedCi >= VF_NUM_POINTS) return;
    if (!is_curve_point_visible_in_gui(g_app.lockedCi)) return;

    static const int TAIL_BOOKEND_COUNT = 5;
    int bookendCi[TAIL_BOOKEND_COUNT] = { g_app.lockedCi, g_app.lockedCi, g_app.lockedCi, g_app.lockedCi, g_app.lockedCi };
    int wantedVisibleOffsets[TAIL_BOOKEND_COUNT] = { 0, 10, 20, 30, 50 };
    int visibleTailOrdinal = 0;
    int lastVisibleTailCi = g_app.lockedCi;
    for (int ci = g_app.lockedCi; ci < VF_NUM_POINTS; ci++) {
        if (!is_curve_point_visible_in_gui(ci)) continue;
        if (g_app.curve[ci].freq_kHz == 0) continue;
        lastVisibleTailCi = ci;
        for (int i = 0; i < TAIL_BOOKEND_COUNT; i++) {
            if (visibleTailOrdinal == wantedVisibleOffsets[i]) {
                bookendCi[i] = ci;
            }
        }
        visibleTailOrdinal++;
    }
    if (visibleTailOrdinal == 0) return;
    for (int i = 0; i < TAIL_BOOKEND_COUNT; i++) {
        if (wantedVisibleOffsets[i] >= visibleTailOrdinal) bookendCi[i] = lastVisibleTailCi;
    }
    // curve tail bookends diagnostic (logged only when drift detected below)

    int tailPoints = 0;
    int driftPoints = 0;
    int firstDriftCi = -1;
    int lastDriftCi = -1;
    int maxDriftCi = -1;
    unsigned int maxDeltaMHz = 0;
    for (int ci = g_app.lockedCi; ci < VF_NUM_POINTS; ci++) {
        if (!is_curve_point_visible_in_gui(ci)) continue;
        if (g_app.curve[ci].freq_kHz == 0) continue;
        tailPoints++;
        unsigned int actualMHz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
        unsigned int deltaMHz = actualMHz > g_app.lockedFreq
            ? actualMHz - g_app.lockedFreq
            : g_app.lockedFreq - actualMHz;
        if (deltaMHz <= 2) continue;
        if (firstDriftCi < 0) firstDriftCi = ci;
        lastDriftCi = ci;
        driftPoints++;
        if (deltaMHz > maxDeltaMHz) {
            maxDeltaMHz = deltaMHz;
            maxDriftCi = ci;
        }
    }
    static bool s_tailDriftLastLoggedValid = false;
    static unsigned int s_tailDriftLastTargetMHz = 0;
    static unsigned int s_tailDriftLastMaxDeltaMHz = 0;
    static int s_tailDriftLastTailPoints = 0;
    static int s_tailDriftLastDriftPoints = 0;
    static int s_tailDriftLastFirstCi = -1;
    static int s_tailDriftLastLastCi = -1;
    static int s_tailDriftLastMaxCi = -1;
    if (driftPoints <= 0) {
        s_tailDriftLastLoggedValid = false;
        return;
    }

    bool driftShapeChanged = !s_tailDriftLastLoggedValid
        || s_tailDriftLastTargetMHz != g_app.lockedFreq
        || s_tailDriftLastTailPoints != tailPoints
        || s_tailDriftLastDriftPoints != driftPoints
        || s_tailDriftLastFirstCi != firstDriftCi
        || s_tailDriftLastLastCi != lastDriftCi
        || s_tailDriftLastMaxCi != maxDriftCi
        || s_tailDriftLastMaxDeltaMHz != maxDeltaMHz;
    if (!driftShapeChanged) return;

    s_tailDriftLastLoggedValid = true;
    s_tailDriftLastTargetMHz = g_app.lockedFreq;
    s_tailDriftLastTailPoints = tailPoints;
    s_tailDriftLastDriftPoints = driftPoints;
    s_tailDriftLastFirstCi = firstDriftCi;
    s_tailDriftLastLastCi = lastDriftCi;
    s_tailDriftLastMaxCi = maxDriftCi;
    s_tailDriftLastMaxDeltaMHz = maxDeltaMHz;
    debug_log("apply_service_snapshot_to_app: full tail live readback drift target=%u points=%d drift=%d first=ci%d last=ci%d max=ci%d/%uMHz temp=%d valid=%d (expected NVIDIA boost/temperature drift — diagnostic only, NO reapply)\n",
        g_app.lockedFreq,
        tailPoints,
        driftPoints,
        firstDriftCi,
        lastDriftCi,
        maxDriftCi,
        maxDeltaMHz,
        g_app.gpuTemperatureC,
        g_app.gpuTemperatureValid ? 1 : 0);
}
