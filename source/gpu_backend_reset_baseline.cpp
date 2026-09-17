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
    char* result, size_t resultSize, bool* powerTargetAlreadyWrittenOut) {
    if (powerTargetAlreadyWrittenOut) *powerTargetAlreadyWrittenOut = false;
    if (!nvapi_read_curve()) {
        set_message(result, resultSize, "Cannot read the curve before baseline reset");
        return false;
    }
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
    set_last_apply_phase("apply: reset OC baseline");
    // Reset GPU offset first to avoid dangerous transient where VF curve tail
    // points snap to factory base frequencies (~3300+ MHz on modern GPUs) while
    // the GPU offset from the previous profile is still active — that spike
    // (e.g. 3300 base + 475 old offset = 3775 MHz effective) causes TDR/crashes.
    if (desired && desired->hasGpuOffset && !vf_curve_global_gpu_offset_supported()
        && !nvapi_set_gpu_offset(0, true)) {
        set_message(result, resultSize, "GPU offset did not reset before curve reset");
        return false;
    }
    // A reset-to-clean-VF-baseline is not ownership of unrelated controls.
    // Only write power when the incoming request itself owns power — and only
    // when this board actually HAS a power control surface.  A board whose driver
    // refuses the power limit (notebook boards whose TGP the OEM/EC owns do
    // this while still answering the constraints) publishes the neutral default
    // percentage, has never had its power target moved by Green Curve, and
    // cannot accept a write; issuing one anyway aborted the whole Apply before
    // the VF curve was touched.  See power_reset_before_apply_required().
    bool powerSurfaceAvailable = power_limit_surface_available(
        g_app.readback.powerLimit, g_app.powerLimitDefaultmW, g_app.powerLimitCurrentmW);
    if (desired && desired->hasPowerLimit && !powerSurfaceAvailable) {
        debug_log("reset-before-apply: skipping power reset — no power control surface"
                  " (readback=%d current=%d mW default=%d mW constraints %d..%d mW);"
                  " Green Curve cannot have moved this board's power target\n",
            g_app.readback.powerLimit ? 1 : 0, g_app.powerLimitCurrentmW,
            g_app.powerLimitDefaultmW, g_app.powerLimitMinmW, g_app.powerLimitMaxmW);
    }
    // Write the target this apply will END at, not the board default.  See
    // power_reset_before_apply_target_pct(): passing through 100% first left a
    // profile that LOWERS the power limit running at full TGP for the entire
    // apply, including the VF curve batch that raises the curve.
    bool requestOwnsPower = desired && desired->hasPowerLimit;
    int powerResetTargetPct = clamp_power_limit_pct(
        power_reset_before_apply_target_pct(requestOwnsPower,
            requestOwnsPower ? desired->powerLimitPct : POWER_LIMIT_DEFAULT_PCT));
    if (power_reset_before_apply_required(requestOwnsPower, powerSurfaceAvailable,
                                          g_app.powerLimitPct, powerResetTargetPct)) {
        debug_log("reset-before-apply: power target %d%% -> %d%% (writing the"
                  " apply's own target, never passing through %d%%)\n",
            g_app.powerLimitPct, powerResetTargetPct, POWER_LIMIT_DEFAULT_PCT);
        if (nvapi_set_power_limit(powerResetTargetPct)) {
            if (powerTargetAlreadyWrittenOut) *powerTargetAlreadyWrittenOut = true;
        } else {
            append_failure("Power target did not apply");
        }
    } else if (requestOwnsPower && powerSurfaceAvailable) {
        // Already at the requested target: the apply phase must still count it
        // as satisfied rather than re-issuing an identical write.
        if (powerTargetAlreadyWrittenOut) *powerTargetAlreadyWrittenOut = true;
        debug_log("reset-before-apply: power target already at %d%%; no write needed\n",
            powerResetTargetPct);
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
    if (!apply_curve_offsets_verified(resetOffsets, resetMask, 2)) {
        append_failure("VF curve offsets did not reset");
    }
    // CT-08.  The `if (failures[0]) return false` check used to live HERE,
    // ahead of the three advanced-clock blocks below.  Every append_failure()
    // in those blocks therefore wrote into a buffer nothing read again, and
    // the function returned true -- a failed XBAR, SYS or VIDEO reset was
    // reported to the caller as a successful baseline.  The check moved to the
    // end of the function; this early exit only survives for the core domains,
    // where stopping before the advanced writes is the point.
    if (failures[0]) {
        set_message(result, resultSize, "Reset before apply failed: %s", failures);
        debug_log("reset-before-apply: core reset failed (%s); not attempting the"
                  " advanced-clock resets\n", failures);
        return false;
    }
    // CT-08.  Ownership, not "any nonzero value I can see".
    //
    // These three blocks used to zero every nonzero probed XBAR/MSVDD/SYS/VIDEO
    // value regardless of whether the incoming request or a previous Green
    // Curve intent owned that domain.  Apply then restored only the fields the
    // request named, so a core-clock-only profile switch silently wiped an
    // advanced offset set by another tool -- or by the user through a different
    // path -- and never put it back.  A baseline reset cleans up what this
    // application owns; it is not a licence to clear the whole GPU.
    const bool requestOwnsXbar = desired && desired->hasXbarOffsetKhz;
    const bool requestOwnsMsvdd = desired && desired->hasXbarMsvddOffsetUv;
    const bool ownsXbar = requestOwnsXbar;
    const bool ownsMsvdd = requestOwnsMsvdd;
    const bool requestOwnsSysClk = desired && desired->hasSysClkOffsetKhz;
    const bool requestOwnsVideoClk = desired && desired->hasVideoClkOffsetKhz;
    // Full profile replacement explicitly names dropped owned fields as zero
    // in service_lifecycle_policy.h. A sparse request does not acquire them.
    const bool previouslyOwnedXbar = g_app.appliedAdvancedOwnedXbar;
    const bool previouslyOwnedSysClk = g_app.appliedAdvancedOwnedSysClk;
    const bool previouslyOwnedVideoClk = g_app.appliedAdvancedOwnedVideoClk;
    debug_log("reset-before-apply: advanced-clock ownership xbar=req%d/prev%d"
              " sys=req%d/prev%d video=req%d/prev%d (probed xbar=%d/%d sys=%d video=%d)\n",
        requestOwnsXbar ? 1 : 0, previouslyOwnedXbar ? 1 : 0,
        requestOwnsSysClk ? 1 : 0, previouslyOwnedSysClk ? 1 : 0,
        requestOwnsVideoClk ? 1 : 0, previouslyOwnedVideoClk ? 1 : 0,
        g_app.xbarFreqOffsetKhz, g_app.xbarMsvddOffsetUv,
        g_app.sysClkFreqOffsetKhz, g_app.videoClkFreqOffsetKhz);
    // Reset both owned XBAR fields through the same validated ClkDomains V2
    // transaction used by Apply.  A fresh GET preserves all unrelated fields.
    if ((ownsXbar || ownsMsvdd) && g_app.xbarProbeValid &&
        ((ownsXbar && g_app.xbarFreqOffsetKhz != 0) ||
         (ownsMsvdd && g_app.xbarMsvddOffsetUv != 0))) {
        auto xbarGetFunc = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_DOMAINS_GET_CONTROL);
        auto xbarSetFunc = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_DOMAINS_SET_CONTROL);
        auto xbarMeasure = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_MEASURE);
        if (xbarGetFunc && xbarSetFunc && xbarMeasure) {
            XbarControlSnapshot snap{};
            if (xbar_write(xbarGetFunc, xbarSetFunc, xbarMeasure,
                          g_app.gpuHandle, &snap, 0, 0, ownsXbar, ownsMsvdd)) {
                g_app.xbarFreqReadbackValid = true;
                g_app.xbarMsvddReadbackValid = true;
                g_app.xbarFreqOffsetKhz = snap.freqOffsetKhz;
                g_app.xbarMsvddOffsetUv = snap.msvddOffsetUv;
                g_app.xbarMeasuredClockKhz = snap.measuredKhz;
                // Back at stock: Green Curve no longer owns this domain.
                if (ownsXbar) g_app.appliedAdvancedOwnedXbar = false;
                if (ownsMsvdd) g_app.appliedAdvancedOwnedMsvdd = false;
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
    if (requestOwnsSysClk &&
        g_app.sysClkProbeValid && g_app.sysClkFreqOffsetKhz != 0) {
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
                g_app.appliedAdvancedOwnedSysClk = false;
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
    if (requestOwnsVideoClk &&
        g_app.videoClkProbeValid && g_app.videoClkFreqOffsetKhz != 0) {
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
                g_app.appliedAdvancedOwnedVideoClk = false;
                debug_log("reset-before-apply: VIDEO clock reset to %d kHz\n",
                          g_app.videoClkFreqOffsetKhz);
            } else {
                append_failure("VIDEO clock offset did not reset");
            }
        } else {
            // CT-08.  VIDEO was the one domain whose missing-function path had
            // no `else` at all: the reset was silently skipped and the caller
            // was told the baseline was clean.  XBAR and SYS both recorded it.
            append_failure("VIDEO clock reset functions unavailable");
        }
    }
    // CT-08.  THE check that the advanced-clock blocks above never had.  Every
    // append_failure() they issue now actually decides the result; before this
    // moved here, they wrote into a buffer whose only reader had already run.
    if (failures[0]) {
        set_message(result, resultSize, "Reset before apply failed: %s", failures);
        debug_log("reset-before-apply: advanced-clock reset failed (%s)\n", failures);
        return false;
    }
    g_app.lastApplyUsedGpuOffset = false;
    read_live_curve_snapshot_settled(4, 25, nullptr);
    refresh_global_state(result, resultSize);
    debug_log("reset-before-apply: OC baseline reset succeeded\n");
    return true;
}
