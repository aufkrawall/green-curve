// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

static void log_locked_tail_drift_diagnostics() {
    // Diagnostics-only during normal runtime; NVUV must not silently reapply
    // the VF curve outside explicit apply/reset or resume/update recovery flows.
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
    if (driftPoints > 0) {
        debug_log("apply_service_snapshot_to_app: full tail drift detected target=%u points=%d drift=%d first=ci%d last=ci%d max=ci%d/%uMHz temp=%d valid=%d (diagnostic only; no automatic VF reapply)\n",
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
}
