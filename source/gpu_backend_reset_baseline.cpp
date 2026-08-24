// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The reset-to-stock-baseline step every profile-switching Apply runs first.
// Split out of gpu_backend_apply.cpp, which had reached its size ratchet, and
// included from the position it occupied so the amalgamated ordering is
// unchanged.  The long comment inside is the whole point of the file: the
// ordering of these writes, and the fact that the per-point VF reset is NOT
// optional, are both measured behaviour that has been re-litigated before.
static bool reset_oc_before_gui_apply(const DesiredSettings* desired,
    char* result, size_t resultSize) {
    int resetOffsets[VF_NUM_POINTS] = {};
    bool resetMask[VF_NUM_POINTS] = {};
    char failures[512] = {};
    auto append_failure = [&](const char* text) {
        if (!text || !text[0]) return;
        if (failures[0]) StringCchCatA(failures, ARRAY_COUNT(failures), "; ");
        StringCchCatA(failures, ARRAY_COUNT(failures), text);
        debug_log("reset-before-apply failure: %s\n", text);
    };
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (g_app.curve[ci].freq_kHz != 0) resetMask[ci] = true;
    }
    bool hadCurveOffsets = false;
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (g_app.freqOffsets[ci] != 0) {
            hadCurveOffsets = true;
            break;
        }
    }
    set_last_apply_phase("apply: reset OC baseline");
    // Reset GPU offset first to avoid dangerous transient where VF curve tail
    // points snap to factory base frequencies (~3300+ MHz on modern GPUs) while
    // the GPU offset from the previous profile is still active — that spike
    // (e.g. 3300 base + 475 old offset = 3775 MHz effective) causes TDR/crashes.
    if (desired && desired->hasGpuOffset &&
        g_app.gpuClockOffsetkHz != 0 && !nvapi_set_gpu_offset(0)) {
        append_failure("GPU offset did not reset");
    }
    // A reset-to-clean-VF-baseline is not ownership of unrelated controls.
    // Only reset power when the incoming request itself owns power and will
    // immediately write its requested target in the apply phase.
    if (desired && desired->hasPowerLimit &&
        g_app.powerLimitPct != 100 && !nvapi_set_power_limit(100)) {
        append_failure("Power target did not reset");
    }
    // Do NOT reset memory offset here — abruptly dropping from +3000 to 0
    // while VRAM is under game load causes TDRs. The new profile's memory
    // offset will be applied directly in the main apply phase.
    //
    // The per-point VF-curve reset-to-zero is REQUIRED, not just a ~1s cost. The
    // selective-offset / boost apply is deliberately DELTA-based for temperature
    // independence (see the "blow out to 700+ MHz" note below): each boost point is
    // written as originalOffset - currentGpuComponent + desiredGpuComponent. For
    // EXCLUDED / stock points the current & desired GPU components are both 0, so the
    // target collapses to originalCurveOffsets[ci] — i.e. whatever is CURRENTLY on the
    // point. This reset-to-zero is what makes that "current" a clean stock baseline;
    // skipping it leaves the PREVIOUS profile's offset on any point the new profile
    // doesn't re-boost (e.g. +475 MHz stranded on excluded points 70-75 when switching
    // to a milder profile — observed 2026-07-04, build 355 `skip_reset_curve_write`
    // experiment). It cannot be cheaply removed without reworking the boost to absolute
    // targets, which the delta design exists to avoid. So it always runs.
    if (hadCurveOffsets && !apply_curve_offsets_verified(resetOffsets, resetMask, 2)) {
        append_failure("VF curve offsets did not reset");
    }
    if (failures[0]) {
        set_message(result, resultSize, "Reset before apply failed: %s", failures);
        return false;
    }
    // Reset both owned XBAR fields through the same validated ClkDomains V2
    // transaction used by Apply.  A fresh GET preserves all unrelated fields.
    if (g_app.xbarProbeValid &&
        (g_app.xbarFreqOffsetKhz != 0 || g_app.xbarMsvddOffsetUv != 0)) {
        auto xbarGetFunc = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_DOMAINS_GET_CONTROL);
        auto xbarSetFunc = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_DOMAINS_SET_CONTROL);
        auto xbarMeasure = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_MEASURE);
        if (xbarGetFunc && xbarSetFunc && xbarMeasure) {
            XbarControlSnapshot snap{};
            if (xbar_reset_to_stock(xbarGetFunc, xbarSetFunc, xbarMeasure,
                                    g_app.gpuHandle, &snap)) {
                g_app.xbarFreqReadbackValid = true;
                g_app.xbarMsvddReadbackValid = true;
                g_app.xbarFreqOffsetKhz = snap.freqOffsetKhz;
                g_app.xbarMsvddOffsetUv = snap.msvddOffsetUv;
                g_app.xbarMeasuredClockKhz = snap.measuredKhz;
                debug_log("reset-before-apply: XBAR reset to %d kHz, %d uV, measured %u kHz\n",
                          snap.freqOffsetKhz, snap.msvddOffsetUv, snap.measuredKhz);
            } else {
                append_failure("XBAR offset did not reset");
            }
        } else {
            append_failure("XBAR reset functions unavailable");
        }
    }
    // SYS clock entry rides the same validated block.
    if (g_app.sysClkProbeValid && g_app.sysClkFreqOffsetKhz != 0) {
        auto sysGetFunc = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_DOMAINS_GET_CONTROL);
        auto sysSetFunc = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_DOMAINS_SET_CONTROL);
        if (sysGetFunc && sysSetFunc) {
            XbarControlSnapshot snap{};
            if (xbar_write_entry_freq(sysGetFunc, sysSetFunc, g_app.gpuHandle,
                                      &snap, XBAR_PINNED_SYS_ENTRY_INDEX, 0)) {
                unsigned int sysField = snap.entryBase +
                    XBAR_PINNED_SYS_ENTRY_INDEX * snap.entryStride +
                    g_xbarSchemas[0].freqOffsetField;
                g_app.sysClkFreqReadbackValid = true;
                g_app.sysClkFreqOffsetKhz = (int)xbar_get_u32(snap.buf, sysField);
                debug_log("reset-before-apply: SYS clock reset to %d kHz\n",
                          g_app.sysClkFreqOffsetKhz);
            } else {
                append_failure("SYS clock offset did not reset");
            }
        } else {
            append_failure("SYS clock reset functions unavailable");
        }
    }
    // VIDEO clock entry rides the same validated block.
    if (g_app.videoClkProbeValid && g_app.videoClkFreqOffsetKhz != 0) {
        auto vidGetFunc = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_DOMAINS_GET_CONTROL);
        auto vidSetFunc = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_DOMAINS_SET_CONTROL);
        if (vidGetFunc && vidSetFunc) {
            XbarControlSnapshot snap{};
            if (xbar_write_entry_freq(vidGetFunc, vidSetFunc, g_app.gpuHandle,
                                      &snap, (unsigned int)XBAR_PINNED_VIDEO_ENTRY_INDEX, 0)) {
                unsigned int videoField = snap.entryBase +
                    XBAR_PINNED_VIDEO_ENTRY_INDEX * snap.entryStride +
                    g_xbarSchemas[0].freqOffsetField;
                g_app.videoClkFreqReadbackValid = true;
                g_app.videoClkFreqOffsetKhz = (int)xbar_get_u32(snap.buf, videoField);
                debug_log("reset-before-apply: VIDEO clock reset to %d kHz\n",
                          g_app.videoClkFreqOffsetKhz);
            } else {
                append_failure("VIDEO clock offset did not reset");
            }
        }
    }
    g_app.lastApplyUsedGpuOffset = false;
    read_live_curve_snapshot_settled(4, 25, nullptr);
    refresh_global_state(result, resultSize);
    debug_log("reset-before-apply: OC baseline reset succeeded\n");
    return true;
}
