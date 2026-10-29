// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Driver-boundary VF writes, included after the atomic reader. Executed with
// fake driver dependencies in tests/linux_vf_write_tests.cpp.

#ifndef GREEN_CURVE_LINUX_BACKEND_VF_WRITE_H
#define GREEN_CURVE_LINUX_BACKEND_VF_WRITE_H

// Write verification needs a fresh CONTROL table before the full status read
// performed at transaction completion.
static bool nvapi_read_offsets(LinuxGpuState* g) {
    LinuxVfReadCandidate candidate = {};
    if (!nvapi_read_offsets_candidate(g, &candidate)) return false;
    memcpy(g->freqOffsets, candidate.offsets, sizeof(g->freqOffsets));
    return true;
}

// ports get_curve_offset_range_khz() / clamp_freq_delta_khz()
static void curve_offset_range_khz(LinuxGpuState* g, int* minKHz, int* maxKHz) {
    const int FALLBACK = 1000000;  // 1000 MHz (matches Windows)
    int mn = -FALLBACK, mx = FALLBACK;
    if (g->curveOffsetRangeKnown && g->curveOffsetMinKHz <= g->curveOffsetMaxKHz) {
        mn = g->curveOffsetMinKHz;
        mx = g->curveOffsetMaxKHz;
    } else if (g->gpuOffsetMinMHz <= g->gpuOffsetMaxMHz && (g->gpuOffsetMinMHz || g->gpuOffsetMaxMHz)) {
        mn = g->gpuOffsetMinMHz * 1000;
        mx = g->gpuOffsetMaxMHz * 1000;
    }
    if (minKHz) *minKHz = mn;
    if (maxKHz) *maxKHz = mx;
}

static int clamp_freq_delta_khz(LinuxGpuState* g, int freqDelta_kHz) {
    int mn = 0, mx = 0;
    curve_offset_range_khz(g, &mn, &mx);
    if (freqDelta_kHz > mx) { lb_log("clamp_freq_delta_khz: %d -> max %d\n", freqDelta_kHz, mx); return mx; }
    if (freqDelta_kHz < mn) { lb_log("clamp_freq_delta_khz: %d -> min %d\n", freqDelta_kHz, mn); return mn; }
    return freqDelta_kHz;
}

// ports apply_curve_offsets_verified(): batch per-point writes with readback
// verification.  The set-control mask carries multiple bits per pass on Windows;
// on a driver that rejects multi-bit masks the verify loop converges via
// repeated single-changed-point passes.
static bool apply_curve_offsets_verified(LinuxGpuState* g, const int* targetOffsets,
                                         const bool* pointMask, int maxBatchPasses) {
    const VfBackendSpec* b = g->backend;
    if (!b || !b->writeSupported || !targetOffsets || !pointMask) return false;

    bool desiredMask[VF_NUM_POINTS] = {};
    int desiredOffsets[VF_NUM_POINTS] = {};
    bool pendingMask[VF_NUM_POINTS] = {};
    bool driverRefused[VF_NUM_POINTS] = {};
    int desiredCount = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!pointMask[i] || g->curve[i].freq_kHz == 0) continue;
        if (!(g->vfMask[i / 8] & (1u << (i % 8)))) {
            lb_log("curve: point %d not in vfMask, skipping\n", i);
            continue;
        }
        desiredMask[i] = pendingMask[i] = true;
        desiredOffsets[i] = clamp_freq_delta_khz(g, targetOffsets[i]);
        desiredCount++;
    }
    if (desiredCount == 0) return true;
    if (maxBatchPasses < 1) maxBatchPasses = 1;

    auto setFunc = (nvapi_buf_t)g->nvapiQi(b->setControlId);
    if (!setFunc) return false;
    if (b->controlBufferSize > LB_CONTROL_BUF_SIZE) return false;

    unsigned char* baseControl = (unsigned char*)calloc(1, LB_CONTROL_BUF_SIZE);
    unsigned char* batchBuf = (unsigned char*)calloc(1, LB_CONTROL_BUF_SIZE);
    bool ok = false;
    if (baseControl && batchBuf && nvapi_read_control_table(g, baseControl, LB_CONTROL_BUF_SIZE)) {
        for (int pass = 0; pass < maxBatchPasses; pass++) {
            memcpy(batchBuf, baseControl, b->controlBufferSize);
            unsigned char writeMask[32] = {};
            bool anyPendingWrite = false;
            int pointsInPass = 0;
            for (int i = 0; i < VF_NUM_POINTS; i++) {
                if (!pendingMask[i]) continue;
                int cur = 0;
                unsigned int off = b->controlEntryBaseOffset + (unsigned int)i * b->controlEntryStride + b->controlEntryDeltaOffset;
                if (off + sizeof(cur) > b->controlBufferSize) { anyPendingWrite = false; break; }
                memcpy(&cur, batchBuf + off, sizeof(cur));
                if (vf_graphics_offset_khz(b, cur) == desiredOffsets[i]) {
                    pendingMask[i] = false;
                    continue;
                }
                int rawDelta = 0;
                if (!vf_graphics_offset_driver_units(b, desiredOffsets[i], &rawDelta)) {
                    lb_log("curve: point %d offset %d kHz exceeds driver unit range\n",
                           i, desiredOffsets[i]);
                    free(baseControl);
                    free(batchBuf);
                    return false;
                }
                memcpy(batchBuf + off, &rawDelta, sizeof(rawDelta));
                writeMask[i / 8] |= (unsigned char)(1u << (i % 8));
                anyPendingWrite = true;
                pointsInPass++;
            }
            if (!anyPendingWrite) { ok = true; break; }
            memcpy(batchBuf + b->controlMaskOffset, writeMask, sizeof(writeMask));
            int setRet = setFunc(g->gpuHandle, batchBuf);
            lb_log("curve batch pass %d: points=%d ret=%d\n", pass + 1, pointsInPass, setRet);
            if (setRet != 0) break;

            bool readOk = false;
            for (int t = 0; t < 6; t++) {
                if (t > 0) pl_sleep_ms(10);
                if (nvapi_read_offsets(g)) { readOk = true; break; }
            }
            if (!readOk) break;

            bool anyPending = false;
            for (int i = 0; i < VF_NUM_POINTS; i++) {
                if (!desiredMask[i]) continue;
                pendingMask[i] = (g->freqOffsets[i] != desiredOffsets[i]);
                // Same rule as the Windows writer (main_runtime_gpu.cpp): an
                // idle placeholder entry below the operating range that the
                // driver pins at offset 0 can never converge.  Without this the
                // loop spent every remaining pass (~1 s each) rewriting it and
                // then failed -- and rolled back -- a curve that had landed.
                if (pendingMask[i] &&
                    vf_offset_zero_readback_is_benign(desiredOffsets[i],
                        g->freqOffsets[i], g->curve[i].freq_kHz,
                        VF_PLACEHOLDER_MAX_FREQ_MHz * 1000u)) {
                    if (!driverRefused[i]) {
                        driverRefused[i] = true;
                        lb_log("curve offset: driver refuses point %d (wrote %dkHz, readback pinned at 0, liveFreq=%ukHz); accepting as non-offsettable placeholder\n",
                               i, desiredOffsets[i], g->curve[i].freq_kHz);
                    }
                    pendingMask[i] = false;
                }
                if (pendingMask[i]) {
                    anyPending = true;
                    lb_log("curve batch pass %d unconverged: ci=%d wroteOffset=%dkHz readbackOffset=%dkHz\n",
                           pass + 1, i, desiredOffsets[i], g->freqOffsets[i]);
                }
            }
            if (!anyPending) { ok = true; break; }
            // Keep driver units in the raw preimage, including sub-kHz values.
            if (!nvapi_read_control_table(g, baseControl, LB_CONTROL_BUF_SIZE)) break;
        }
    }
    free(baseControl);
    free(batchBuf);
    return ok;
}

#endif // GREEN_CURVE_LINUX_BACKEND_VF_WRITE_H
