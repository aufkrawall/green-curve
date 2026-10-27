// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The advanced-clock domains of an Apply: XBAR core clock, XBAR MSVDD, the
// pinned SYS clock entry and the pinned VIDEO clock entry.
//
// Split out of gpu_backend_apply.cpp, which is far over the project's
// file-size guidance, and included from the exact position it occupied so the
// amalgamated ordering is unchanged.
//
// Advanced writes share the apply's commit boundary. A failure resets every
// requested advanced field and enters core recovery before returning failure.
#ifndef GREEN_CURVE_GPU_BACKEND_APPLY_ADVANCED_H
#define GREEN_CURVE_GPU_BACKEND_APPLY_ADVANCED_H

static bool validate_advanced_clock_request(const DesiredSettings* desired,
                                            char* result, size_t resultSize) {
    const bool xbar = desired->hasXbarOffsetKhz || desired->hasXbarMsvddOffsetUv;
    const bool sys = desired->hasSysClkOffsetKhz != 0;
    const bool video = desired->hasVideoClkOffsetKhz != 0;
    if (!xbar && !sys && !video) return true;
    const bool control = nvapi_qi(XBAR_NVAPI_CLK_DOMAINS_GET_CONTROL) &&
                         nvapi_qi(XBAR_NVAPI_CLK_DOMAINS_SET_CONTROL);
    const bool available = control &&
        (!xbar || (g_app.xbarProbeValid && nvapi_qi(XBAR_NVAPI_CLK_MEASURE))) &&
        (!sys || g_app.sysClkProbeValid) && (!video || g_app.videoClkProbeValid);
    if (!available) {
        set_message(result, resultSize,
            "A requested advanced clock control is unavailable. Nothing was changed.");
        debug_log("apply: advanced preflight refused control=%d xbar=%d/%d sys=%d/%d video=%d/%d\n",
            control ? 1 : 0, xbar ? 1 : 0, g_app.xbarProbeValid ? 1 : 0,
            sys ? 1 : 0, g_app.sysClkProbeValid ? 1 : 0,
            video ? 1 : 0, g_app.videoClkProbeValid ? 1 : 0);
    }
    return available;
}

static void apply_advanced_clock_domains(const DesiredSettings* desired,
                                         bool coreRecoveryRan,
                                         int& successCount, int& failCount,
                                         bool& partialApplyRisk,
                                         char* failureDetails,
                                         size_t failureDetailsSize,
                                         bool recovering = false) {
    const int failuresBefore = failCount;
    if (coreRecoveryRan) {
        const bool advancedRequested = desired &&
            (desired->hasXbarOffsetKhz || desired->hasXbarMsvddOffsetUv ||
             desired->hasSysClkOffsetKhz || desired->hasVideoClkOffsetKhz);
        if (advancedRequested) {
            failCount++;
            partialApplyRisk = true;
            StringCchCatA(failureDetails, failureDetailsSize,
                failureDetails[0]
                    ? "; advanced clock offsets skipped after core rollback"
                    : "Advanced clock offsets were not applied because the core"
                      " clock apply failed and was rolled back");
            debug_log("apply: skipping ALL advanced-clock writes (xbar=%d msvdd=%d"
                      " sys=%d video=%d) -- core recovery ran\n",
                desired->hasXbarOffsetKhz ? 1 : 0, desired->hasXbarMsvddOffsetUv ? 1 : 0,
                desired->hasSysClkOffsetKhz ? 1 : 0, desired->hasVideoClkOffsetKhz ? 1 : 0);
        }
    }
    if (!coreRecoveryRan && desired && (desired->hasXbarOffsetKhz || desired->hasXbarMsvddOffsetUv)) {
        auto xbarGetCtrl = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_DOMAINS_GET_CONTROL);
        auto xbarSetCtrl = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_DOMAINS_SET_CONTROL);
        auto xbarMeasure = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_MEASURE);
        bool functionsAvailable = xbarGetCtrl && xbarSetCtrl && xbarMeasure;
        if (g_app.xbarProbeValid && functionsAvailable) {
            XbarControlSnapshot snap{};
            int targetFreqKhz = desired->hasXbarOffsetKhz
                ? desired->xbarOffsetKhz : g_app.xbarFreqOffsetKhz;
            int targetMsvddUv = desired->hasXbarMsvddOffsetUv
                ? desired->xbarMsvddOffsetUv : g_app.xbarMsvddOffsetUv;
            if (desired->hasXbarOffsetKhz) g_app.appliedAdvancedOwnedXbar = true;
            if (desired->hasXbarMsvddOffsetUv) g_app.appliedAdvancedOwnedMsvdd = true;
            if (xbar_write(xbarGetCtrl, xbarSetCtrl, xbarMeasure,
                           g_app.gpuHandle, &snap, targetFreqKhz, targetMsvddUv,
                           desired->hasXbarOffsetKhz, desired->hasXbarMsvddOffsetUv)) {
                g_app.xbarFreqReadbackValid = true;
                g_app.xbarMsvddReadbackValid = true;
                g_app.xbarFreqOffsetKhz = snap.freqOffsetKhz;
                g_app.xbarMsvddOffsetUv = snap.msvddOffsetUv;
                g_app.xbarMeasuredClockKhz = snap.measuredKhz;
                // CT-08.  Ownership is recorded on a VERIFIED write, so a
                // later baseline reset can tell a value this application put
                // there from one it must preserve.  Back at zero means the
                // domain is at stock and nothing owns it.
                if (desired->hasXbarOffsetKhz)
                    g_app.appliedAdvancedOwnedXbar = snap.freqOffsetKhz != 0;
                if (desired->hasXbarMsvddOffsetUv)
                    g_app.appliedAdvancedOwnedMsvdd = snap.msvddOffsetUv != 0;
                successCount++;
                debug_log("apply: XBAR offset %d kHz, MSVDD %d uV, measured %u kHz\n",
                          snap.freqOffsetKhz, snap.msvddOffsetUv,
                          snap.measuredKhz);
            } else {
                failCount++;
                partialApplyRisk = true;
                StringCchCatA(failureDetails, failureDetailsSize,
                              failureDetails[0] ? "; XBAR offset" : "XBAR offset");
                debug_log("apply: XBAR offset write FAILED requested=(%d kHz, %d uV)"
                          " probeValid=%d\n", targetFreqKhz, targetMsvddUv,
                          g_app.xbarProbeValid ? 1 : 0);
            }
        } else {
            failCount++;
            partialApplyRisk = true;
            StringCchCatA(failureDetails, failureDetailsSize,
                          failureDetails[0] ? "; XBAR unavailable" : "XBAR unavailable");
            debug_log("apply: XBAR requested but unavailable probeValid=%d functions=%d\n",
                      g_app.xbarProbeValid ? 1 : 0, functionsAvailable ? 1 : 0);
        }
    }
    if (!recovering && failCount != failuresBefore) return;
    // SYS clock domain offset apply (second ClkDomains aux entry, identified
    // empirically).  Same full-block transaction discipline as XBAR; a
    // failure is reported rather than silently skipped.
    if (!coreRecoveryRan && desired && desired->hasSysClkOffsetKhz) {
        auto sysGetCtrl = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_DOMAINS_GET_CONTROL);
        auto sysSetCtrl = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_DOMAINS_SET_CONTROL);
        if (g_app.sysClkProbeValid && sysGetCtrl && sysSetCtrl) {
            XbarControlSnapshot snap{};
            g_app.appliedAdvancedOwnedSysClk = true;
            if (xbar_write_entry_freq(sysGetCtrl, sysSetCtrl, g_app.gpuHandle,
                                      &snap, XBAR_PINNED_SYS_ENTRY_INDEX,
                                      desired->sysClkOffsetKhz)) {
                unsigned int sysField = snap.entryBase +
                    XBAR_PINNED_SYS_ENTRY_INDEX * snap.entryStride +
                    g_xbarSchemas[0].freqOffsetField;
                g_app.sysClkFreqReadbackValid = true;
                g_app.sysClkFreqOffsetKhz =
                    (int)xbar_get_u32(snap.buf, sysField);
                // CT-08: ownership recorded on a verified write.
                g_app.appliedAdvancedOwnedSysClk =
                    (g_app.sysClkFreqOffsetKhz != 0);
                successCount++;
                debug_log("apply: SYS clock offset %d kHz\n",
                          g_app.sysClkFreqOffsetKhz);
            } else {
                failCount++;
                partialApplyRisk = true;
                StringCchCatA(failureDetails, failureDetailsSize,
                              failureDetails[0] ? "; SYS clock offset"
                                                : "SYS clock offset");
                debug_log("apply: SYS clock offset write FAILED requested=%d kHz"
                          " probeValid=%d\n", desired->sysClkOffsetKhz,
                          g_app.sysClkProbeValid ? 1 : 0);
            }
        } else {
            failCount++;
            partialApplyRisk = true;
            StringCchCatA(failureDetails, failureDetailsSize,
                          failureDetails[0] ? "; SYS clock unavailable"
                                            : "SYS clock unavailable");
            debug_log("apply: SYS clock requested but unavailable probeValid=%d\n",
                      g_app.sysClkProbeValid ? 1 : 0);
        }
    }
    if (!recovering && failCount != failuresBefore) return;
    // VIDEO clock offset apply.  Entry 4 identified by differential dump;
    // the engine's physical clock has no CLK_MEASURE id, so verification is
    // the exact readback itself.
    if (!coreRecoveryRan && desired && desired->hasVideoClkOffsetKhz) {
        auto vidGetCtrl = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_DOMAINS_GET_CONTROL);
        auto vidSetCtrl = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_DOMAINS_SET_CONTROL);
        if (g_app.videoClkProbeValid && vidGetCtrl && vidSetCtrl) {
            XbarControlSnapshot snap{};
            g_app.appliedAdvancedOwnedVideoClk = true;
            if (xbar_write_entry_freq(vidGetCtrl, vidSetCtrl, g_app.gpuHandle,
                                      &snap, (unsigned int)XBAR_PINNED_VIDEO_ENTRY_INDEX,
                                      desired->videoClkOffsetKhz)) {
                unsigned int videoField = snap.entryBase +
                    XBAR_PINNED_VIDEO_ENTRY_INDEX * snap.entryStride +
                    g_xbarSchemas[0].freqOffsetField;
                g_app.videoClkFreqReadbackValid = true;
                g_app.videoClkFreqOffsetKhz =
                    (int)xbar_get_u32(snap.buf, videoField);
                // CT-08: ownership recorded on a verified write.
                g_app.appliedAdvancedOwnedVideoClk =
                    (g_app.videoClkFreqOffsetKhz != 0);
                successCount++;
                debug_log("apply: VIDEO clock offset %d kHz\n",
                          g_app.videoClkFreqOffsetKhz);
            } else {
                failCount++;
                partialApplyRisk = true;
                StringCchCatA(failureDetails, failureDetailsSize,
                              failureDetails[0] ? "; VIDEO clock offset"
                                                : "VIDEO clock offset");
                debug_log("apply: VIDEO clock offset write FAILED requested=%d kHz"
                          " probeValid=%d\n", desired->videoClkOffsetKhz,
                          g_app.videoClkProbeValid ? 1 : 0);
            }
        } else {
            failCount++;
            partialApplyRisk = true;
            StringCchCatA(failureDetails, failureDetailsSize,
                          failureDetails[0] ? "; VIDEO clock unavailable"
                                            : "VIDEO clock unavailable");
            debug_log("apply: VIDEO clock requested but unavailable probeValid=%d\n",
                      g_app.videoClkProbeValid ? 1 : 0);
        }
    }
}

// Execute the same boundary in production and hardware-free driver tests.
// Cleanup includes failed setters: their cached readback may still be stale.
template<class RecoverCore>
static bool apply_advanced_clock_transaction(const DesiredSettings* desired,
    int& successCount, int& failCount, bool& partialApplyRisk,
    char* failureDetails, size_t failureDetailsSize, RecoverCore recoverCore) {
    const int failuresBefore = failCount;
    apply_advanced_clock_domains(desired, false, successCount, failCount,
        partialApplyRisk, failureDetails, failureDetailsSize);
    if (failCount == failuresBefore) return true;
    DesiredSettings stock = {};
    stock.hasXbarOffsetKhz = desired->hasXbarOffsetKhz;
    stock.hasXbarMsvddOffsetUv = desired->hasXbarMsvddOffsetUv;
    stock.hasSysClkOffsetKhz = desired->hasSysClkOffsetKhz;
    stock.hasVideoClkOffsetKhz = desired->hasVideoClkOffsetKhz;
    int recovered = 0, failed = 0;
    bool risk = false;
    char detail[256] = {};
    apply_advanced_clock_domains(&stock, false, recovered, failed, risk,
                                 detail, sizeof(detail), true);
    debug_log("apply: advanced recovery verified=%d failed=%d; entering core recovery\n",
              recovered, failed);
    if (failed) StringCchCatA(failureDetails, failureDetailsSize,
        "; advanced defaults could not be verified; cleanup ownership retained");
    recoverCore();
    return false;
}

#endif  // GREEN_CURVE_GPU_BACKEND_APPLY_ADVANCED_H
