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
// These are a SEPARATE transaction from the core clock domains on purpose.
// They are independently verified through a fresh full-block
// read-modify-write (gpu_backend_xbar.h), they contribute to the aggregate
// result the caller reports, and neither their successes nor their failures
// trigger another core rollback -- rollback_to_safe_defaults() deliberately
// does not cover them.
//
// What is NOT separate is the direction of the dependency.  CT-08: none of
// these writes may run after core recovery has fired.  The pre-fix code walked
// straight from rollback_to_safe_defaults() into raising private clock and
// rail-voltage offsets on a GPU whose core state had just been declared
// untrustworthy, because the "advanced domains are independent" rule was read
// as independence in both directions.  It is not: a core failure invalidates
// the state these offsets would be layered on top of.
#ifndef GREEN_CURVE_GPU_BACKEND_APPLY_ADVANCED_H
#define GREEN_CURVE_GPU_BACKEND_APPLY_ADVANCED_H

static void apply_advanced_clock_domains(const DesiredSettings* desired,
                                         bool coreRecoveryRan,
                                         int& successCount, int& failCount,
                                         bool& partialApplyRisk,
                                         char* failureDetails,
                                         size_t failureDetailsSize) {
    // Advanced-clock transaction boundary.  XBAR/SYS/VIDEO are independently
    // verified writes and intentionally live outside rollback_to_safe_defaults().
    // They still contribute to the aggregate result reported to the caller, but
    // neither their successes nor their failures trigger another core rollback.
    //
    // CT-04.  New advanced tuning does NOT run after core recovery.  The old
    // code walked straight into XBAR/SYS/VIDEO writes moments after
    // rollback_to_safe_defaults() had just returned the core domain to stock
    // because something failed -- raising private clock and voltage offsets on
    // a GPU whose core state had just been declared untrustworthy.  These are
    // clock and rail offsets, not cosmetic settings.
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
            if (xbar_write(xbarGetCtrl, xbarSetCtrl, xbarMeasure,
                           g_app.gpuHandle, &snap, targetFreqKhz, targetMsvddUv,
                           true, true)) {
                g_app.xbarFreqReadbackValid = true;
                g_app.xbarMsvddReadbackValid = true;
                g_app.xbarFreqOffsetKhz = snap.freqOffsetKhz;
                g_app.xbarMsvddOffsetUv = snap.msvddOffsetUv;
                g_app.xbarMeasuredClockKhz = snap.measuredKhz;
                // CT-08.  Ownership is recorded on a VERIFIED write, so a
                // later baseline reset can tell a value this application put
                // there from one it must preserve.  Back at zero means the
                // domain is at stock and nothing owns it.
                g_app.appliedAdvancedOwnedXbar =
                    (snap.freqOffsetKhz != 0 || snap.msvddOffsetUv != 0);
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
    // SYS clock domain offset apply (second ClkDomains aux entry, identified
    // empirically).  Same full-block transaction discipline as XBAR; a
    // failure is reported rather than silently skipped.
    if (!coreRecoveryRan && desired && desired->hasSysClkOffsetKhz) {
        auto sysGetCtrl = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_DOMAINS_GET_CONTROL);
        auto sysSetCtrl = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_DOMAINS_SET_CONTROL);
        if (g_app.sysClkProbeValid && sysGetCtrl && sysSetCtrl) {
            XbarControlSnapshot snap{};
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
    // VIDEO clock offset apply.  Entry 4 identified by differential dump;
    // the engine's physical clock has no CLK_MEASURE id, so verification is
    // the exact readback itself.
    if (!coreRecoveryRan && desired && desired->hasVideoClkOffsetKhz) {
        auto vidGetCtrl = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_DOMAINS_GET_CONTROL);
        auto vidSetCtrl = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_DOMAINS_SET_CONTROL);
        if (g_app.videoClkProbeValid && vidGetCtrl && vidSetCtrl) {
            XbarControlSnapshot snap{};
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

#endif  // GREEN_CURVE_GPU_BACKEND_APPLY_ADVANCED_H
