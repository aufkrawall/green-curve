// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_GPU_BACKEND_SNAPSHOT_H
#define GREEN_CURVE_GPU_BACKEND_SNAPSHOT_H
// Latest complete sample wins. A final read failure invalidates the proof,
// even when an earlier successful sample remains available for diagnostics.
static bool read_live_curve_snapshot_settled(int attempts, DWORD delayMs, bool* lastOffsetsOkOut) {
    if (!g_app.isServiceProcess && g_app.usingBackgroundService) {
        char err[256] = {};
        ServiceResponse stateResponse = {};
        if (!service_client_get_ready_state(&stateResponse, 2000,
                "settled curve state", err, sizeof(err))) {
            debug_log("service snapshot failed: %s\n", err);
            if (lastOffsetsOkOut) *lastOffsetsOkOut = false;
            return false;
        }
        apply_ready_service_envelope_to_app(&stateResponse);
        if (lastOffsetsOkOut) *lastOffsetsOkOut = true;
        return stateResponse.snapshot.loaded;
    }
    if (lastOffsetsOkOut) *lastOffsetsOkOut = false;
    if (attempts < 1) attempts = 1;
    bool lastSampleOk = false;
    bool bestValid = false;
    bool bestOffsetsOk = false;
    int bestNumVisible = -1;
    int bestNumPopulated = -1;
    VFCurvePoint bestCurve[VF_NUM_POINTS] = {};
    int bestFreqOffsets[VF_NUM_POINTS] = {};
    for (int attempt = 0; attempt < attempts; attempt++) {
        if (attempt > 0 && delayMs > 0) Sleep(delayMs);
        // F-APPLY-CEILING witness. Inert unless an apply is in progress; it adds
        // one NVML read to an iteration this loop was already making, and no
        // sleep, attempt or branch that can change what the loop returns. This
        // is the only place that samples the middle of the post-curve-write
        // window rather than just its two ends.
        apply_clock_witness_poll("curve settle");
        bool curveOk = nvapi_read_curve();
        bool offsetsOk = nvapi_read_offsets();
        lastSampleOk = curveOk && offsetsOk && g_app.numPopulated > 0;
        if (!lastSampleOk) continue;
        rebuild_visible_map();
        detect_locked_tail_from_curve();
        bool betterSnapshot = lastSampleOk;
        if (betterSnapshot) {
            memcpy(bestCurve, g_app.curve, sizeof(bestCurve));
            memcpy(bestFreqOffsets, g_app.freqOffsets, sizeof(bestFreqOffsets));
            bestNumVisible = g_app.numVisible;
            bestNumPopulated = g_app.numPopulated;
            bestOffsetsOk = offsetsOk;
            bestValid = true;
        }
    }
    if (!bestValid) {
        if (lastOffsetsOkOut) *lastOffsetsOkOut = false;
        return false;
    }
    memcpy(g_app.curve, bestCurve, sizeof(g_app.curve));
    memcpy(g_app.freqOffsets, bestFreqOffsets, sizeof(g_app.freqOffsets));
    g_app.numPopulated = bestNumPopulated;
    g_app.loaded = true;
    rebuild_visible_map();
    detect_locked_tail_from_curve();
    debug_log("read_live_curve_snapshot_settled: selected visible=%d populated=%d offsetsOk=%d attempts=%d\n",
        bestNumVisible,
        bestNumPopulated,
        bestOffsetsOk ? 1 : 0,
        attempts);
    if (lastOffsetsOkOut) *lastOffsetsOkOut = bestOffsetsOk && lastSampleOk;
    return lastSampleOk;
}
#endif
