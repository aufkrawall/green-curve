// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The Windows verified VF-curve writer: batch control-table writes with
// readback, then ~1 s per-point fallback writes for whatever the batch could
// not converge.  Split out of main_runtime_gpu.cpp (included from the position
// it occupied, so the amalgamated ordering is unchanged) so the regression
// harness can drive the production code against a fake driver
// (tests/windows_vf_verified_write_tests.cpp).
//
// Invariant: convergence is judged ONLY against a control table read inside
// this call.  The cached g_app.freqOffsets is the last snapshot the service
// took, and anything outside Green Curve -- another tool's Reset, a driver
// reset -- can change the hardware after it.  The writer used to read a fresh
// table for its batch comparison but judge the points left over against that
// cache: when the fresh table already matched every target the batch loop
// wrote nothing and read nothing back, so a stale cache sent every point it
// disagreed with through the per-point fallback.  A Reset after an external
// reset spent ~1 s per point (52 s for 51 points) re-writing a curve that was
// already at stock, outlasting the client's response deadline.

#ifndef GREEN_CURVE_GPU_BACKEND_CURVE_WRITE_H
#define GREEN_CURVE_GPU_BACKEND_CURVE_WRITE_H

#include "apply_correction_budget_policy.h"
#include "vf_control_offsets_decode.h"

static bool nvapi_read_control_table(unsigned char* buf, size_t bufSize) {
    const VfBackendSpec* backend = g_app.vfBackend;
    if (!backend) return false;
    if (!buf || bufSize < backend->controlBufferSize) return false;

    auto getFunc = (NvApiFunc)nvapi_qi(backend->getControlId);
    if (!getFunc) return false;

    unsigned char mask[32] = {};
    if (!nvapi_get_vf_info_cached(mask)) return false;

    memset(buf, 0, backend->controlBufferSize);
    const unsigned int version = (backend->controlVersion << 16) | backend->controlBufferSize;
    memcpy(&buf[0], &version, sizeof(version));
    if (backend->controlMaskOffset + sizeof(mask) > backend->controlBufferSize) return false;
    memcpy(&buf[backend->controlMaskOffset], mask, sizeof(mask));
    return getFunc(g_app.gpuHandle, buf) == 0;
}

// `fallbackDeadlineTickMs` bounds the ~1 s per-point fallback writes for an
// apply (apply_correction_budget_policy.h); 0 = unbounded, for Reset/rollback.
static bool apply_curve_offsets_verified(const int* targetOffsets, const bool* pointMask, int maxBatchPasses,
    ULONGLONG fallbackDeadlineTickMs) {
    if (!targetOffsets || !pointMask) return false;

    const VfBackendSpec* backend = g_app.vfBackend;
    if (!backend || !backend->writeSupported) return false;

    bool desiredMask[VF_NUM_POINTS] = {};
    int desiredOffsets[VF_NUM_POINTS] = {};
    bool pendingMask[VF_NUM_POINTS] = {};
    bool driverRefused[VF_NUM_POINTS] = {};
    int desiredCount = 0;

    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!pointMask[i]) continue;
        if (g_app.curve[i].freq_kHz == 0) continue;
        unsigned char vfMaskByte = g_app.vfMask[i / 8];
        if (!(vfMaskByte & (1u << (i % 8)))) {
            debug_log("apply_curve_offsets_verified: point %d not in vfMask, skipping\n", i);
            continue;
        }
        desiredMask[i] = true;
        pendingMask[i] = true;
        desiredOffsets[i] = clamp_freq_delta_khz(targetOffsets[i]);
        desiredCount++;
    }
    if (desiredCount == 0) return false;

    if (maxBatchPasses < 1) maxBatchPasses = 1;

    auto setFunc = (NvApiFunc)nvapi_qi(backend->setControlId);
    if (!setFunc) return false;

    const size_t CONTROL_BUF_SIZE = 0x4000;
    HeapBuffer baseControl(CONTROL_BUF_SIZE);
    if (!baseControl) return false;
    if (backend->controlBufferSize > CONTROL_BUF_SIZE) return false;
    if (!nvapi_read_control_table(baseControl, CONTROL_BUF_SIZE)) return false;

    // Adopt the table just read as the cache this call judges against (see the
    // invariant at the top of this file).  A difference on a targeted point
    // means the hardware moved since the last snapshot; say so once, and
    // re-read the curve below even if no write turns out to be needed, so the
    // caller is not left holding a curve that disagrees with its offsets.
    int cachedOffsets[VF_NUM_POINTS] = {};
    memcpy(cachedOffsets, g_app.freqOffsets, sizeof(cachedOffsets));
    g_app.vfOffsetsHzValid = vf_control_offsets_decode(backend, baseControl,
        backend->controlBufferSize, g_app.freqOffsets, g_app.freqOffsetsHz);
    const VfControlAdoptionDiff entryDiff =
        vf_control_adoption_diff(cachedOffsets, g_app.freqOffsets, desiredMask);
    if (entryDiff.changed > 0) {
        debug_log("apply_curve_offsets_verified: entry readback replaced %d stale cached"
                  " offset(s) (first ci=%d cached=%dkHz live=%dkHz); the VF table changed"
                  " outside this process since the last read\n",
            entryDiff.changed, entryDiff.firstCi, entryDiff.firstCachedKHz,
            entryDiff.firstLiveKHz);
    }

    bool anyWrite = false;
    int batchedPoints = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (desiredMask[i]) batchedPoints++;
    }
    bool allowBatch = batchedPoints > 1;
    bool batchFailed = false;
    if (!allowBatch) maxBatchPasses = 0;
    HeapBuffer batchBuf(CONTROL_BUF_SIZE);
    if (!batchBuf) return false;
    for (int pass = 0; pass < maxBatchPasses; pass++) {
        unsigned char* buf = batchBuf;
        memcpy(buf, baseControl, backend->controlBufferSize);

        unsigned char writeMask[32] = {};
        bool anyPendingWrite = false;
        int pointsInPass = 0;
        int minPassOffsetKHz = 0, maxPassOffsetKHz = 0;
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            if (!pendingMask[i]) continue;
            int currentDelta = 0;
            unsigned int deltaOffset = backend->controlEntryBaseOffset + (unsigned int)i * backend->controlEntryStride + backend->controlEntryDeltaOffset;
            if (deltaOffset + sizeof(currentDelta) > backend->controlBufferSize) return false;
            memcpy(&currentDelta, &buf[deltaOffset], sizeof(currentDelta));
            int rawDelta = 0;
            if (!vf_graphics_offset_driver_units(backend, desiredOffsets[i], &rawDelta)) {
                debug_log("curve: point %d offset %d kHz exceeds driver unit range\n",
                    i, desiredOffsets[i]);
                return false;
            }
            if (currentDelta == rawDelta) {
                pendingMask[i] = false;
                continue;
            }
            memcpy(&buf[deltaOffset], &rawDelta, sizeof(rawDelta));
            writeMask[i / 8] |= (unsigned char)(1u << (i % 8));
            if (pointsInPass == 0) {
                minPassOffsetKHz = maxPassOffsetKHz = desiredOffsets[i];
            } else {
                if (desiredOffsets[i] < minPassOffsetKHz) minPassOffsetKHz = desiredOffsets[i];
                if (desiredOffsets[i] > maxPassOffsetKHz) maxPassOffsetKHz = desiredOffsets[i];
            }
            anyPendingWrite = true;
            pointsInPass++;
        }

        if (!anyPendingWrite) break;

        memcpy(&buf[backend->controlMaskOffset], writeMask, sizeof(writeMask));
        char phase[128] = {};
        StringCchPrintfA(phase, ARRAY_COUNT(phase), "VF curve batch pass: pass=%d points=%d", pass + 1, pointsInPass);
        set_last_apply_phase(phase);
        debug_log("curve batch pass %d begin: points=%d offsetRange=%d..%dkHz maskBytes=%02X%02X%02X%02X\n",
            pass + 1, pointsInPass, minPassOffsetKHz, maxPassOffsetKHz,
            writeMask[0], writeMask[1], writeMask[2], writeMask[3]);
        int setRet = setFunc(g_app.gpuHandle, buf);
        debug_log("curve batch pass %d: points=%d offsetRange=%d..%dkHz ret=%d maskBytes=%02X%02X%02X%02X\n",
            pass + 1, pointsInPass, minPassOffsetKHz, maxPassOffsetKHz, setRet,
            writeMask[0], writeMask[1], writeMask[2], writeMask[3]);
        if (setRet != 0) {
            batchFailed = true;
            break;
        }
        anyWrite = true;

        bool readOk = false;
        for (int verifyTry = 0; verifyTry < 6; verifyTry++) {
            if (verifyTry > 0) Sleep(10);
            if (nvapi_read_offsets()) {
                readOk = true;
                break;
            }
        }
        if (!readOk) {
            batchFailed = true;
            break;
        }

        if (!nvapi_read_curve()) { batchFailed = true; break; }
        bool anyPending = false;
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            if (!desiredMask[i]) continue;
            bool converged = vf_graphics_offset_readback_matches(g_app.freqOffsets[i],
                g_app.vfOffsetsHzValid, g_app.freqOffsetsHz[i], desiredOffsets[i]);
            // Driver-refusal: a populated-but-placeholder VF entry (marked editable in
            // vfMask, but an idle/non-operating point — e.g. a high-index entry that
            // reports ~400 MHz, below the operating range) that the driver silently
            // pins at offset 0 no matter what we write. Retrying it costs a ~1s NVAPI
            // setControl per pass plus a per-point fallback, and it can never converge
            // (readback stays exactly 0). Detect the refusal (we asked for a non-zero
            // offset, the write returned success, but the readback is still exactly 0)
            // and accept it as non-offsettable instead of looping. Accepting it is a
            // hardware no-op — the driver was never going to move it.
            if (!converged && vf_offset_zero_readback_is_benign(desiredOffsets[i],
                g_app.freqOffsets[i], g_app.curve[i].freq_kHz,
                VF_PLACEHOLDER_MAX_FREQ_MHz * 1000u)) {
                if (!driverRefused[i]) {
                    driverRefused[i] = true;
                    debug_log("curve offset: driver refuses point %d (wrote %dkHz, readback pinned at 0, liveFreq=%ukHz); accepting as non-offsettable placeholder\n",
                        i, desiredOffsets[i], g_app.curve[i].freq_kHz);
                }
                pendingMask[i] = false;
                continue;
            }
            pendingMask[i] = !converged;
            if (pendingMask[i]) {
                anyPending = true;
                // Offset-convergence diagnostic: the driver accepted the write (ret=0)
                // but reports a DIFFERENT offset than we wrote, so this point keeps
                // being retried (each retry costs a ~1s NVAPI setControl). Log the
                // exact gap so we can see if the driver snaps/rounds/clamps the offset.
                debug_log("curve batch pass %d unconverged: ci=%d wroteOffset=%dkHz readbackOffset=%dkHz driverDelta=%dkHz liveFreq=%ukHz\n",
                    pass + 1, i, desiredOffsets[i], g_app.freqOffsets[i],
                    g_app.freqOffsets[i] - desiredOffsets[i], g_app.curve[i].freq_kHz);
            }
        }
        if (!anyPending) break;
        if (!nvapi_read_control_table(baseControl, CONTROL_BUF_SIZE)) {
            batchFailed = true;
            break;
        }
    }

    bool allOk = !batchFailed;
    bool hasPending = false;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!desiredMask[i]) continue;
        if (driverRefused[i]) { pendingMask[i] = false; continue; }  // non-offsettable placeholder: don't fall back
        if (!vf_graphics_offset_readback_matches(g_app.freqOffsets[i],
            g_app.vfOffsetsHzValid, g_app.freqOffsetsHz[i], desiredOffsets[i])) {
            pendingMask[i] = true;
            hasPending = true;
        } else {
            pendingMask[i] = false;
        }
    }

    if (hasPending) {
        int fallbackSkipped = 0;
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            if (!pendingMask[i]) continue;
            if (!apply_fallback_write_may_start(GetTickCount64(), fallbackDeadlineTickMs)) {
                // Past the apply's correction budget: leave the point for
                // verification to judge instead of spending another ~1 s write.
                fallbackSkipped++;
                allOk = false;
                continue;
            }
            // Points the batch could not converge fall back to per-point writes.
            // Log the pre-fallback gap so the batch-vs-fallback behaviour is visible.
            bool pointOk = nvapi_set_point(i, desiredOffsets[i]);
            debug_log("curve fallback point %d target=%dkHz ok=%d (batch left readback=%dkHz, driverDelta=%dkHz)\n",
                i, desiredOffsets[i], pointOk ? 1 : 0,
                g_app.freqOffsets[i], g_app.freqOffsets[i] - desiredOffsets[i]);
            if (!pointOk) {
                allOk = false;
            } else {
                anyWrite = true;
            }
        }

        if (fallbackSkipped > 0) {
            debug_log("apply_curve_offsets_verified: skipped %d per-point fallback write(s);"
                      " the apply correction budget (%lu ms after entry) is exhausted\n",
                fallbackSkipped, apply_correction_budget_ms());
        }

        bool readOk = false;
        for (int verifyTry = 0; verifyTry < 6; verifyTry++) {
            if (verifyTry > 0) Sleep(10);
            if (nvapi_read_offsets()) {
                readOk = true;
                break;
            }
        }
        if (!readOk) {
            allOk = false;
        }
    }

    if (anyWrite || entryDiff.changed > 0) {
        if (!nvapi_read_curve()) allOk = false;
        rebuild_visible_map();
    }

    // Final offset-convergence summary: which points the driver still refuses to
    // honor exactly (these are what force the separate correction phase + extra ~1s
    // driver writes). Distinguishes a driver-rounded offset from a true write miss.
    int unconvergedCount = 0;
    int refusedCount = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!desiredMask[i]) continue;
        if (driverRefused[i]) { refusedCount++; continue; }  // accepted: driver won't move a placeholder, not a real miss
        if (!vf_graphics_offset_readback_matches(g_app.freqOffsets[i],
            g_app.vfOffsetsHzValid, g_app.freqOffsetsHz[i], desiredOffsets[i])) {
            allOk = false;
            unconvergedCount++;
            debug_log("curve offset unconverged (final): ci=%d wroteOffset=%dkHz readbackOffset=%dkHz driverDelta=%dkHz liveFreq=%ukHz\n",
                i, desiredOffsets[i], g_app.freqOffsets[i],
                g_app.freqOffsets[i] - desiredOffsets[i], g_app.curve[i].freq_kHz);
        }
    }
    debug_log("apply_curve_offsets_verified: done desired=%d unconverged=%d driverRefused=%d allOk=%d (unconverged points force the correction phase's extra ~1s writes; refused placeholders are accepted)\n",
        desiredCount, unconvergedCount, refusedCount, allOk ? 1 : 0);

    return allOk;
}

#endif  // GREEN_CURVE_GPU_BACKEND_CURVE_WRITE_H
