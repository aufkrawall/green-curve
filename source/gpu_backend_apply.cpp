// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
static bool reset_oc_before_gui_apply(char* result, size_t resultSize) {
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
    if (g_app.gpuClockOffsetkHz != 0 && !nvapi_set_gpu_offset(0)) {
        append_failure("GPU offset did not reset");
    }
    if (g_app.powerLimitPct != 100 && !nvapi_set_power_limit(100)) {
        append_failure("Power target did not reset");
    }
    // Do NOT reset memory offset here — abruptly dropping from +3000 to 0
    // while VRAM is under game load causes TDRs. The new profile's memory
    // offset will be applied directly in the main apply phase.
    if (hadCurveOffsets && !apply_curve_offsets_verified(resetOffsets, resetMask, 2)) {
        append_failure("VF curve offsets did not reset");
    }
    if (failures[0]) {
        set_message(result, resultSize, "Reset before apply failed: %s", failures);
        return false;
    }
    g_app.lastApplyUsedGpuOffset = false;
    read_live_curve_snapshot_settled(4, 25, nullptr);
    refresh_global_state(result, resultSize);
    debug_log("reset-before-apply: OC baseline reset succeeded\n");
    return true;
}

static bool apply_desired_settings_service(const DesiredSettings* desired, bool interactive, char* result, size_t resultSize) {
    if (!desired) {
        set_message(result, resultSize, "No desired settings");
        return false;
    }
    if (!validate_desired_fan_settings_for_apply(desired, result, resultSize)) {
        debug_log("apply_desired_settings: fan prevalidation failed: %s\n", result && result[0] ? result : "unknown");
        return false;
    }
    if (desired->resetOcBeforeApply) {
        if (!reset_oc_before_gui_apply(result, resultSize)) return false;
        Sleep(1000);
        debug_log("apply_desired_settings: 1s settle at stock baseline complete\n");
    }
    clear_last_operation_details();
    build_operation_intent_summary(desired, interactive, g_lastOperationIntent, sizeof(g_lastOperationIntent));
    capture_last_operation_snapshot(g_lastOperationBeforeSnapshot, sizeof(g_lastOperationBeforeSnapshot));
    set_last_apply_phase("apply: build intent and snapshots");
    const VfBackendSpec* activeBackend = g_app.vfBackend;
    debug_log("apply_desired_settings: hasGpuOffset=%d gpuOffsetMHz=%d gpuOffsetExcludeLowCount=%d family=%s backend=%s bestGuess=%d read=%d write=%d\n",
        desired->hasGpuOffset ? 1 : 0,
        desired->gpuOffsetMHz,
        desired->gpuOffsetExcludeLowCount,
        gpu_family_name(g_app.gpuFamily),
        activeBackend && activeBackend->name ? activeBackend->name : "<none>",
        activeBackend && activeBackend->bestGuessOnly ? 1 : 0,
        activeBackend && activeBackend->readSupported ? 1 : 0,
        activeBackend && activeBackend->writeSupported ? 1 : 0);
    int successCount = 0;
    int failCount = 0;
    char failureDetails[1024] = {};
    char curveVerifySummary[512] = {};
    auto append_failure = [&](const char* fmt, ...) {
        char part[256] = {};
        va_list ap;
        va_start(ap, fmt);
        StringCchVPrintfA(part, ARRAY_COUNT(part), fmt, ap);
        va_end(ap);
        if (!part[0]) return;
        if (failureDetails[0]) {
            StringCchCatA(failureDetails, ARRAY_COUNT(failureDetails), "; ");
        }
        StringCchCatA(failureDetails, ARRAY_COUNT(failureDetails), part);
        debug_log("apply failure: %s\n", part);
    };
    bool requestHasLock = desired->hasLock && desired->lockCi >= 0 && desired->lockMHz > 0;
    bool allowInteractiveStoredLock = interactive && !g_app.isServiceProcess;
    bool hasLock = requestHasLock
        || (allowInteractiveStoredLock && g_app.lockedVi >= 0 && g_app.lockedVi < g_app.numVisible);
    bool hasCurveEdits = false;
    int lockCi = -1;
    int lockVi = -1;
    unsigned int lockMhz = 0;
    bool shouldApplyMemOffset = false;
    int targetMemkHz = 0;
    bool memApplied = false;
    bool powerChanged = false;
    int currentAppliedGpuOffsetMHz = current_applied_gpu_offset_mhz();
    int currentActiveGpuOffsetExcludeLowCount = current_applied_gpu_offset_excludes_low_points() ? g_app.appliedGpuOffsetExcludeLowCount : 0;
    int targetGpuOffsetkHz = currentAppliedGpuOffsetMHz * 1000;
    bool gpuOffsetValid = true;
    bool shouldApplyGpuOffset = false;
    int desiredActiveGpuOffsetExcludeLowCount = 0;
    bool gpuPolicyViaCurveBatch = false;
    bool gpuPolicyChangeRequested = false;
    bool partialApplyRisk = false;
    int originalCurveOffsets[VF_NUM_POINTS] = {};
    int originalCurveFreqkHz[VF_NUM_POINTS] = {};
    unsigned int originalCurveVoltUv[VF_NUM_POINTS] = {};
    bool originalCurvePopulated[VF_NUM_POINTS] = {};
    int targetCurveOffsets[VF_NUM_POINTS] = {};
    bool targetCurveMask[VF_NUM_POINTS] = {};
    bool lockedTailMask[VF_NUM_POINTS] = {};
    bool explicitCurveMask[VF_NUM_POINTS] = {};
    bool haveNonZeroCurveOffsets = false;
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        originalCurveOffsets[ci] = g_app.freqOffsets[ci];
        originalCurveFreqkHz[ci] = (int)g_app.curve[ci].freq_kHz;
        originalCurveVoltUv[ci] = g_app.curve[ci].volt_uV;
        originalCurvePopulated[ci] = g_app.curve[ci].freq_kHz > 0;
        if (originalCurveOffsets[ci] != 0) haveNonZeroCurveOffsets = true;
        if (desired->hasCurvePoint[ci]) {
            hasCurveEdits = true;
            explicitCurveMask[ci] = true;
        }
    }
    // After reset-before-apply, the live curve base frequencies may have shifted
    // (e.g. due to temperature-dependent boost). Refresh the originals so that
    // subsequent offset computations use post-reset base frequencies rather than
    // stale pre-reset values. Otherwise points that should stay at their base
    // frequency end up at the wrong MHz because the computed offset targets the
    // old base frequency.
    if (desired->resetOcBeforeApply) {
        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
            originalCurveOffsets[ci] = g_app.freqOffsets[ci];
            originalCurveFreqkHz[ci] = (int)g_app.curve[ci].freq_kHz;
            originalCurveVoltUv[ci] = g_app.curve[ci].volt_uV;
            originalCurvePopulated[ci] = g_app.curve[ci].freq_kHz > 0;
        }
        debug_log("refresh originals after reset-before-apply: point 75 freq=%d kHz offset=%d\n",
            originalCurveFreqkHz[75],
            originalCurveOffsets[75]);
    }
    if (hasLock) {
        if (desired->hasLock && desired->lockCi >= 0 && desired->lockCi < VF_NUM_POINTS && desired->lockMHz > 0) {
            lockCi = desired->lockCi;
            lockMhz = desired->lockMHz;
        } else if (g_app.lockedCi >= 0 && g_app.lockedCi < VF_NUM_POINTS && g_app.lockedFreq > 0) {
            lockCi = g_app.lockedCi;
        } else {
            lockCi = g_app.visibleMap[g_app.lockedVi];
        }
        for (int vi = 0; vi < g_app.numVisible; vi++) {
            if (g_app.visibleMap[vi] == lockCi) {
                lockVi = vi;
                break;
            }
        }
        if (lockVi < 0) {
            hasLock = false;
        } else {
            if (lockMhz == 0) {
                lockMhz = desired->hasCurvePoint[lockCi] ? desired->curvePointMHz[lockCi] : get_edit_value(g_app.hEditsMhz[lockVi]);
            }
            for (int vi = lockVi; vi < g_app.numVisible; vi++) {
                int ci = g_app.visibleMap[vi];
                if (ci >= 0 && ci < VF_NUM_POINTS) lockedTailMask[ci] = true;
            }
        }
    }
    if (desired->hasMemOffset) {
        targetMemkHz = mem_driver_khz_from_display_mhz(desired->memOffsetMHz);
        shouldApplyMemOffset = (g_app.memClockOffsetkHz != targetMemkHz);
        if (g_app.memOffsetRangeKnown &&
            (desired->memOffsetMHz < g_app.memClockOffsetMinMHz || desired->memOffsetMHz > g_app.memClockOffsetMaxMHz)) {
            // F-DOM-1: intentionally NOT gated. NVIDIA's reported memory-offset
            // range is frequently conservative and the driver itself clamps or
            // rejects values it cannot honor, so we apply outside the reported
            // range by design (already absolute-bounded to +/-5000 MHz at the IPC
            // boundary). Logged explicitly so an auditor sees this is deliberate.
            debug_log("mem offset %d MHz is outside the driver-reported range %d..%d MHz; applying anyway by design (reported memory range is often conservative; the driver clamps/rejects unsupported values)\n",
                desired->memOffsetMHz, g_app.memClockOffsetMinMHz, g_app.memClockOffsetMaxMHz);
        }
    }
    if (desired->hasGpuOffset) {
        if (!g_app.gpuOffsetRangeKnown ||
            (desired->gpuOffsetMHz >= g_app.gpuClockOffsetMinMHz && desired->gpuOffsetMHz <= g_app.gpuClockOffsetMaxMHz)) {
            desiredActiveGpuOffsetExcludeLowCount = (desired->gpuOffsetExcludeLowCount > 0 && desired->gpuOffsetMHz != 0) ? desired->gpuOffsetExcludeLowCount : 0;
            targetGpuOffsetkHz = desired->gpuOffsetMHz * 1000;
            gpuPolicyChangeRequested =
                (targetGpuOffsetkHz != currentAppliedGpuOffsetMHz * 1000) ||
                (desiredActiveGpuOffsetExcludeLowCount != currentActiveGpuOffsetExcludeLowCount);
            shouldApplyGpuOffset = gpuPolicyChangeRequested && currentActiveGpuOffsetExcludeLowCount <= 0 && desiredActiveGpuOffsetExcludeLowCount <= 0;
            gpuPolicyViaCurveBatch = gpuPolicyChangeRequested && (currentActiveGpuOffsetExcludeLowCount > 0 || desiredActiveGpuOffsetExcludeLowCount > 0);
            debug_log("desired gpu offset mhz=%d current=%d shouldApply=%d viaCurve=%d desiredSelective=%d currentSelective=%d\n",
                desired->gpuOffsetMHz,
                currentAppliedGpuOffsetMHz,
                shouldApplyGpuOffset ? 1 : 0,
                gpuPolicyViaCurveBatch ? 1 : 0,
                desiredActiveGpuOffsetExcludeLowCount,
                currentActiveGpuOffsetExcludeLowCount);
        } else {
            gpuOffsetValid = false;
            debug_log("desired gpu offset mhz=%d rejected by range %d..%d\n",
                desired->gpuOffsetMHz, g_app.gpuClockOffsetMinMHz, g_app.gpuClockOffsetMaxMHz);
        }
    }
    if (!gpuOffsetValid) {
        failCount++;
        partialApplyRisk = true;
        append_failure("GPU offset %d MHz is outside the supported range %d..%d MHz",
            desired->gpuOffsetMHz, g_app.gpuClockOffsetMinMHz, g_app.gpuClockOffsetMaxMHz);
    }
    if (gpuOffsetValid && hasLock && lockMhz > 0 && (hasCurveEdits || hasLock || gpuPolicyViaCurveBatch)) {
        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
            if (!originalCurvePopulated[ci]) continue;
            if (lockedTailMask[ci]) break;

            unsigned int preTailTargetMHz = 0;
            if (desired->hasCurvePoint[ci] && !lockedTailMask[ci]) {
                preTailTargetMHz = desired->curvePointMHz[ci];
            } else if (desired->hasGpuOffset) {
                long long base = (long long)originalCurveFreqkHz[ci] - (long long)originalCurveOffsets[ci];
                if (base < 0) base = 0;
                long long targetKHz = base + (long long)gpu_offset_component_mhz_for_point(ci, desired->gpuOffsetMHz, desiredActiveGpuOffsetExcludeLowCount) * 1000LL;
                if (targetKHz < 0) targetKHz = 0;
                if (targetKHz > UINT_MAX) targetKHz = UINT_MAX;
                preTailTargetMHz = displayed_curve_mhz((unsigned int)targetKHz);
            } else {
                preTailTargetMHz = displayed_curve_mhz((unsigned int)originalCurveFreqkHz[ci]);
            }

            if (preTailTargetMHz > lockMhz) {
                char excludeHint[128] = {};
                if (desired->hasGpuOffset && desired->gpuOffsetMHz != 0 && gpuPolicyViaCurveBatch) {
                    StringCchPrintfA(excludeHint, ARRAY_COUNT(excludeHint),
                        " Enable 'exclude low VF points' to exclude pre-tail points from the GPU offset.");
                }
                set_message(result, resultSize,
                    "Curve lock %u MHz at point %d is below pre-tail point %d (%u MHz).%s Lower the preceding point or raise the lock target.",
                    lockMhz, lockCi, ci, preTailTargetMHz, excludeHint);
                debug_log("lock monotonicity validation failed before writes: lockCi=%d lockMHz=%u preTailCi=%d target=%u desiredGpu=%d exclude=%d explicit=%d\n",
                    lockCi, lockMhz, ci, preTailTargetMHz,
                    desired->hasGpuOffset ? desired->gpuOffsetMHz : currentAppliedGpuOffsetMHz,
                    desired->hasGpuOffset ? desiredActiveGpuOffsetExcludeLowCount : currentActiveGpuOffsetExcludeLowCount,
                    desired->hasCurvePoint[ci] ? 1 : 0);
                return false;
            }
        }
    }
    bool gpuApplied = false;
    // Apply GPU offset first via dedicated path (handles uniform offset reliably).
    // When combined with lock/curve edits, applying the GPU offset separately avoids
    // sending highly non-uniform offsets for all curve points in a single batch,
    // which can fail. After this, the curve batch only needs to adjust the lock tail.
    if (gpuOffsetValid && shouldApplyGpuOffset) {
        set_last_apply_phase("apply: dedicated GPU offset write");
        if (nvapi_set_gpu_offset(targetGpuOffsetkHz)) {
            successCount++;
            gpuApplied = true;
            g_app.appliedGpuOffsetMHz = desired->gpuOffsetMHz;
            g_app.appliedGpuOffsetExcludeLowCount = 0;
            g_app.lastApplyUsedGpuOffset = true;
            bool settledOffsetsOk = false;
            if (!read_live_curve_snapshot_settled(6, 25, &settledOffsetsOk)) {
                debug_log("apply gpu offset: settled refresh failed after dedicated GPU offset write\n");
            }
        } else {
            failCount++;
            partialApplyRisk = true;
            append_failure("GPU offset %d MHz was not accepted by the driver", desired->gpuOffsetMHz);
        }
    }
    // Refresh cached originals after GPU offset so subsequent curve computations
    // use the post-offset state
    if (gpuApplied) {
        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
            originalCurveOffsets[ci] = g_app.freqOffsets[ci];
            originalCurveFreqkHz[ci] = (int)g_app.curve[ci].freq_kHz;
            originalCurveVoltUv[ci] = g_app.curve[ci].volt_uV;
            originalCurvePopulated[ci] = g_app.curve[ci].freq_kHz > 0;
        }
    }
    // When transitioning between uniform and selective (exclude-low) offsets
    // on Blackwell, zero the existing per-curve-point offsets first.
    // This establishes a clean baseline (all offsets = 0) so that the new
    // per-point deltas are applied from a known state, rather than depending on
    // correct detection of the previous offset magnitude in the delta formula.
    if (gpuPolicyViaCurveBatch
        && currentAppliedGpuOffsetMHz != 0
        && vf_curve_global_gpu_offset_supported()) {
        set_last_apply_phase("apply: zero prior GPU offset");
        debug_log("selective offset: zeroing prior offset %d MHz before transition\n", currentAppliedGpuOffsetMHz);
        if (nvapi_set_gpu_offset(0)) {
            currentAppliedGpuOffsetMHz = 0;
            g_app.appliedGpuOffsetMHz = 0;
            g_app.appliedGpuOffsetExcludeLowCount = 0;
            for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                originalCurveOffsets[ci] = g_app.freqOffsets[ci];
                originalCurveFreqkHz[ci] = (int)g_app.curve[ci].freq_kHz;
                originalCurveVoltUv[ci] = g_app.curve[ci].volt_uV;
            }
        } else {
            debug_log("selective offset: failed to zero prior offset\n");
        }
    }
    bool preserveCurveAcrossMem = shouldApplyMemOffset && (haveNonZeroCurveOffsets || gpuApplied);
    bool userCurveRequest = hasCurveEdits || hasLock;
    bool curveRequest = userCurveRequest || gpuPolicyViaCurveBatch;
    {
        bool explicitNonTailMask[VF_NUM_POINTS] = {};
        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
            explicitNonTailMask[ci] = explicitCurveMask[ci] && !lockedTailMask[ci];
        }
        char explicitPoints[256] = {};
        char tailPoints[256] = {};
        build_point_list_from_flags(explicitNonTailMask, explicitPoints, sizeof(explicitPoints));
        build_point_list_from_flags(lockedTailMask, tailPoints, sizeof(tailPoints));
        StringCchPrintfA(g_lastOperationPlan, sizeof(g_lastOperationPlan),
            "GPU offset apply: requested=%d valid=%d shouldApply=%d viaCurve=%d desiredSelective=%d currentSelective=%d\r\n"
            "Memory offset apply: requested=%d shouldApply=%d\r\n"
            "Curve plan: userCurveRequest=%d curveRequest=%d hasLock=%d lockCi=%d lockMHz=%u lockTracksAnchor=%d preserveAcrossMem=%d\r\n"
            "Explicit curve points: %s\r\n"
            "Locked tail points: %s\r\n",
            desired->hasGpuOffset ? desired->gpuOffsetMHz : currentAppliedGpuOffsetMHz,
            gpuOffsetValid ? 1 : 0,
            shouldApplyGpuOffset ? 1 : 0,
            gpuPolicyViaCurveBatch ? 1 : 0,
            desiredActiveGpuOffsetExcludeLowCount,
            currentActiveGpuOffsetExcludeLowCount,
            desired->hasMemOffset ? desired->memOffsetMHz : mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz),
            shouldApplyMemOffset ? 1 : 0,
            userCurveRequest ? 1 : 0,
            curveRequest ? 1 : 0,
            hasLock ? 1 : 0,
            lockCi,
            lockMhz,
            desired->lockTracksAnchor ? 1 : 0,
            preserveCurveAcrossMem ? 1 : 0,
            explicitPoints,
            tailPoints);
    }
    if (curveRequest || preserveCurveAcrossMem) {
        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
            if (!originalCurvePopulated[ci]) continue;
            targetCurveOffsets[ci] = originalCurveOffsets[ci];
            if (preserveCurveAcrossMem) targetCurveMask[ci] = true;
        }
        if (gpuPolicyViaCurveBatch) {
            bool currentDetected = (currentAppliedGpuOffsetMHz != 0 || currentActiveGpuOffsetExcludeLowCount > 0);
            for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                if (!originalCurvePopulated[ci]) continue;
                int desiredPointGpuOffsetkHz = gpu_offset_component_mhz_for_point(ci, desired->gpuOffsetMHz, desiredActiveGpuOffsetExcludeLowCount) * 1000;
                int currentPointGpuOffsetkHz;
                if (currentDetected) {
                    currentPointGpuOffsetkHz = gpu_offset_component_mhz_for_point(ci, currentAppliedGpuOffsetMHz, currentActiveGpuOffsetExcludeLowCount) * 1000;
                } else {
                    currentPointGpuOffsetkHz = originalCurveOffsets[ci];
                }
                int targetOffset = clamp_freq_delta_khz(originalCurveOffsets[ci] - currentPointGpuOffsetkHz + desiredPointGpuOffsetkHz);
                targetCurveOffsets[ci] = targetOffset;
                targetCurveMask[ci] = true;
            }
            debug_log("selective offset: currentMHz=%d desiredMHz=%d currentExcl=%d desiredExcl=%d detected=%d hasLock=%d lockMHz=%d\n",
                currentAppliedGpuOffsetMHz, desired->gpuOffsetMHz,
                currentActiveGpuOffsetExcludeLowCount,
                desiredActiveGpuOffsetExcludeLowCount,
                currentDetected ? 1 : 0,
                hasLock ? 1 : 0, lockMhz);
        }
        if (hasLock && lockMhz > 0) {
            // When a selective offset is active, the selective path above already
            // set correct per-point deltas (+offset for included points, 0 for
            // excluded points). Using absolute profile targets for boost-region
            // points would overwrite those deltas with temperature-dependent
            // values that can blow out to 700+ MHz when the cold base curve is
            // elevated. Only the tail-flatten loop is needed in this case.
            if (!gpuPolicyViaCurveBatch) {
                for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                    if (!desired->hasCurvePoint[ci]) continue;
                    if (lockedTailMask[ci]) continue;
                    if (!originalCurvePopulated[ci]) continue;
                    long long base = (long long)originalCurveFreqkHz[ci] - (long long)originalCurveOffsets[ci];
                    if (base < 0) base = 0;
                    long long target = (long long)desired->curvePointMHz[ci] * 1000LL;
                    long long diff = target - base;
                    if (diff > INT_MAX) diff = INT_MAX;
                    if (diff < INT_MIN) diff = INT_MIN;
                    targetCurveOffsets[ci] = clamp_freq_delta_khz((int)diff);
                    targetCurveMask[ci] = true;
                }
            }
            // Determine the uniform floor offset for tail points.
            // Per-point tail offsets are ineffective on Blackwell: the
            // driver ignores individual tail-point deltas and the correction
            // loop cannot converge. The solution is to apply a single
            // uniform negative offset to ALL tail points, which floors
            // the tail and lets the lock point control the entire region.
            // Use the minimum supported driver offset for this purpose.
            int floorTailOffsetKHz = 0;
            {
                int minkHz = 0, maxkHz = 0;
                bool rangeOk = get_curve_offset_range_khz(&minkHz, &maxkHz);
                if (rangeOk && minkHz < 0) {
                    floorTailOffsetKHz = minkHz;
                } else {
                    floorTailOffsetKHz = -1000000; // conservative fallback
                }
            }
            for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                if (!lockedTailMask[ci]) continue;
                if (!originalCurvePopulated[ci]) continue;
                if (ci == lockCi) {
                    // Lock point: compute per-point offset normally
                    long long base = (long long)originalCurveFreqkHz[ci] - (long long)originalCurveOffsets[ci];
                    if (base < 0) base = 0;
                    long long target = (long long)lockMhz * 1000LL;
                    long long diff = target - base;
                    if (diff > INT_MAX) diff = INT_MAX;
                    if (diff < INT_MIN) diff = INT_MIN;
                    targetCurveOffsets[ci] = clamp_freq_delta_khz((int)diff);
                } else {
                    // All remaining tail points: use uniform floor offset.
                    // Per-point deltas are ineffective on Blackwell; the
                    // driver treats tail points as a group controlled by
                    // the lock point. A uniform minimum offset floors them
                    // reliably.
                    targetCurveOffsets[ci] = floorTailOffsetKHz;
                }
                targetCurveMask[ci] = true;
            }
        } else if (gpuPolicyViaCurveBatch && !hasLock) {
            // When the selective GPU offset is active without a lock, the explicit
            // curve point path is skipped because the selective offset already
            // handles all populated points. The locked tail above also handles
            // the case where both lock and selective offset are active.
        } else {
            // No lock, no selective offset — explicit curve points only.
            for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                if (!desired->hasCurvePoint[ci]) continue;
                if (!originalCurvePopulated[ci]) continue;
                long long base = (long long)originalCurveFreqkHz[ci] - (long long)originalCurveOffsets[ci];
                if (base < 0) base = 0;
                long long target = (long long)desired->curvePointMHz[ci] * 1000LL;
                long long diff = target - base;
                if (diff > INT_MAX) diff = INT_MAX;
                if (diff < INT_MIN) diff = INT_MIN;
                targetCurveOffsets[ci] = clamp_freq_delta_khz((int)diff);
                targetCurveMask[ci] = true;
            }
        }
    }
    if (desired->hasMemOffset) {
        if (shouldApplyMemOffset) {
            set_last_apply_phase("apply: memory offset write");
            debug_log("apply mem offset: display=%d MHz driver_kHz=%d nvml_mhz=%d\n",
                desired->memOffsetMHz,
                targetMemkHz,
                (targetMemkHz / 1000) * 2);
            if (nvapi_set_mem_offset(targetMemkHz)) {
                successCount++;
                memApplied = true;
                debug_log("apply mem offset: accepted by driver\n");
            } else {
                failCount++;
                partialApplyRisk = true;
                append_failure("Memory offset %d MHz was not accepted by the driver", desired->memOffsetMHz);
            }
        } else {
            debug_log("apply mem offset: skipped (current already matches target %d kHz)\n", targetMemkHz);
        }
    }
    bool curveBatchOk = true;
    bool curveBatchNeeded = false;
    bool curveTouched = gpuApplied;
    int selectiveOffsetApplied = 0;
    int selectiveOffsetFailed = 0;
    int flattenApplied = 0;
    int flattenFailed = 0;
    int userBoostApplied = 0;
    int userBoostFailed = 0;
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (targetCurveMask[ci]) {
            curveBatchNeeded = true;
            break;
        }
    }
    if ((gpuPolicyViaCurveBatch || hasLock) && curveBatchNeeded) {
        int boostPoints = 0, flattenPoints = 0;
        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
            if (!targetCurveMask[ci]) continue;
            if (hasLock && lockedTailMask[ci]) flattenPoints++;
            else boostPoints++;
        }
        debug_log("curve strategy: selective=%+d MHz%s boostRegionPoints=%d flattenTarget=%u MHz flattenRegionPoints=%d lockCi=%d%s\n",
            gpuPolicyViaCurveBatch ? desired->gpuOffsetMHz : 0,
            desiredActiveGpuOffsetExcludeLowCount > 0 ? " excl<N" : "",
            boostPoints,
            (hasLock && lockMhz > 0) ? lockMhz : 0,
            flattenPoints,
            hasLock ? lockCi : -1,
            (gpuPolicyViaCurveBatch && hasLock) ? " boost-via-selective" : "");
    }
    if (curveBatchNeeded && (curveRequest || memApplied)) {
        set_last_apply_phase("apply: VF curve batch write");
        curveTouched = true;
        int batchedCount = 0;
        int batchedMinCi = -1;
        int batchedMaxCi = -1;
        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
            if (!targetCurveMask[ci]) continue;
            batchedCount++;
            if (batchedMinCi < 0) batchedMinCi = ci;
            batchedMaxCi = ci;
        }
        // Warn about unusually large offsets, but only refuse values beyond the
        // driver-reported VF offset range. The former fixed 600 MHz cutoff was
        // too conservative for cold-boot baselines where a valid locked profile
        // can need larger transition-point deltas.
        int highOffsetWarnings = 0;
        int hardLimitOffsets = 0;
        int maxAbsOffsetKHz = 0;
        int maxAbsOffsetCi = -1;
        int firstHighOffsetCi = -1;
        int firstHighOffsetKHz = 0;
        int rangeMinKHz = 0;
        int rangeMaxKHz = 0;
        bool rangeKnown = get_curve_offset_range_khz(&rangeMinKHz, &rangeMaxKHz);
        // When the driver range is unknown, use a conservative fallback to
        // prevent dangerously large offsets from corrupted/malicious profiles.
        // Testing shows the driver accepts tail offsets beyond 300000 kHz;
        // the GPU offset range (±1000 MHz = ±1000000 kHz) is the authoritative
        // hardware capability.
        const int FALLBACK_VF_OFFSET_LIMIT_KHZ = 500000; // 500 MHz (up from 300 MHz)
        int hardLimitKHz = rangeKnown
            ? nvmax(abs(rangeMinKHz), abs(rangeMaxKHz))
            : FALLBACK_VF_OFFSET_LIMIT_KHZ;
        if (hardLimitKHz <= 0) hardLimitKHz = FALLBACK_VF_OFFSET_LIMIT_KHZ;
        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
            if (!targetCurveMask[ci]) continue;
            int absOffsetKHz = abs(targetCurveOffsets[ci]);
            if (absOffsetKHz > maxAbsOffsetKHz) {
                maxAbsOffsetKHz = absOffsetKHz;
                maxAbsOffsetCi = ci;
            }
            if (absOffsetKHz > 600000) { // > 600 MHz
                highOffsetWarnings++;
                if (firstHighOffsetCi < 0) {
                    firstHighOffsetCi = ci;
                    firstHighOffsetKHz = targetCurveOffsets[ci];
                }
            }
            if (absOffsetKHz > hardLimitKHz) {
                hardLimitOffsets++;
                debug_log("apply curve batch: refusing point %d target offset %d kHz beyond hard range limit %d kHz\n",
                    ci,
                    targetCurveOffsets[ci],
                    hardLimitKHz);
            }
        }
        char curveVerifyDetail[256] = {};
        bool curveRequestOk = true;
        if (hardLimitOffsets > 0) {
            set_last_apply_phase("apply: VF curve batch refused out-of-range offsets");
            debug_log("apply curve batch: refused %d point(s) beyond hard range limit %d kHz\n",
                hardLimitOffsets,
                hardLimitKHz);
            curveBatchOk = false;
            curveRequestOk = false;
            set_message(curveVerifyDetail, sizeof(curveVerifyDetail),
                "Refused VF curve batch because %d point(s) exceeded the driver VF offset range", hardLimitOffsets);
        } else {
            if (highOffsetWarnings > 0) {
                debug_log("apply curve batch: high offset warning summary count=%d firstPoint=%d firstOffset=%d maxAbsPoint=%d maxAbs=%d driverRange=%d..%d known=%d\n",
                    highOffsetWarnings,
                    firstHighOffsetCi,
                    firstHighOffsetKHz,
                    maxAbsOffsetCi,
                    maxAbsOffsetKHz,
                    rangeMinKHz,
                    rangeMaxKHz,
                    rangeKnown ? 1 : 0);
            }
            debug_log("apply curve batch: points=%d range=%d..%d passes=%d offsetRange=%d..%d known=%d highWarnings=%d maxAbsPoint=%d maxAbs=%d\n",
                batchedCount,
                batchedMinCi,
                batchedMaxCi,
                hasLock ? 3 : 2,
                rangeMinKHz,
                rangeMaxKHz,
                rangeKnown ? 1 : 0,
                highOffsetWarnings,
                maxAbsOffsetCi,
                maxAbsOffsetKHz);
            curveBatchOk = apply_curve_offsets_verified(targetCurveOffsets, targetCurveMask, hasLock ? 3 : 2);
            bool settledOffsetsOk = false;
            if (!read_live_curve_snapshot_settled(6, 25, &settledOffsetsOk)) {
                debug_log("apply curve: settled refresh failed after curve batch\n");
            }
            DesiredSettings verifyDesired = *desired;
            for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                if (hasLock && lockedTailMask[ci]) continue;
                if (!desired->hasCurvePoint[ci]) {
                    verifyDesired.hasCurvePoint[ci] = false;
                    verifyDesired.curvePointMHz[ci] = 0;
                }
            }
            if (gpuPolicyViaCurveBatch) {
                bool currentDetected = (currentAppliedGpuOffsetMHz != 0 || currentActiveGpuOffsetExcludeLowCount > 0);
                for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                    if (!originalCurvePopulated[ci] || verifyDesired.hasCurvePoint[ci]) continue;
                    long long currentPointGpuOffsetkHz;
                    if (currentDetected) {
                        currentPointGpuOffsetkHz = (long long)gpu_offset_component_mhz_for_point(ci, currentAppliedGpuOffsetMHz, currentActiveGpuOffsetExcludeLowCount) * 1000LL;
                    } else {
                        currentPointGpuOffsetkHz = originalCurveOffsets[ci];
                    }
                    long long targetFreqkHz = (long long)originalCurveFreqkHz[ci]
                        - currentPointGpuOffsetkHz
                        + (long long)gpu_offset_component_mhz_for_point(ci, desired->gpuOffsetMHz, desiredActiveGpuOffsetExcludeLowCount) * 1000LL;
                    if (targetFreqkHz < 0) targetFreqkHz = 0;
                    verifyDesired.hasCurvePoint[ci] = true;
                    if (hasLock && lockedTailMask[ci]) {
                        verifyDesired.curvePointMHz[ci] = lockMhz;
                    } else {
                        verifyDesired.curvePointMHz[ci] = displayed_curve_mhz((unsigned int)targetFreqkHz);
                    }
                }
                // Verify selective offset in the boost region (below the lock point).
                // Tail points are verified separately against the flatten target.
                // Some VF points may have hardware limits that prevent the selective
                // offset from taking effect (e.g. special max-clock limit points on
                // Blackwell, or rounding edge cases). Accept the actual live frequency
                // for points where the hardware didn't apply the expected offset, so the
                // overall operation isn't marked as failed for a single stubborn point.
                selectiveOffsetApplied = 0;
                selectiveOffsetFailed = 0;
                flattenApplied = 0;
                flattenFailed = 0;
                for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                    if (!verifyDesired.hasCurvePoint[ci]) continue;
                    if (g_app.curve[ci].freq_kHz == 0) continue;
                    unsigned int actualMHz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
                    if (hasLock && lockedTailMask[ci]) {
                        unsigned int deltaMHz = actualMHz > lockMhz ? (actualMHz - lockMhz) : (lockMhz - actualMHz);
                        if (deltaMHz <= 8) {
                            flattenApplied++;
                        } else {
                            flattenFailed++;
                            debug_log("flatten undervolt: point %d actual %u MHz != target %u MHz (delta=%u); keeping strict lock target\n",
                                ci, actualMHz, lockMhz, deltaMHz);
                        }
                        continue;
                    }
                    int desiredPointOffsetMHz = gpu_offset_component_mhz_for_point(ci, desired->gpuOffsetMHz, desiredActiveGpuOffsetExcludeLowCount);
                    int actualOffsetkHz = g_app.freqOffsets[ci];
                    int expectedOffsetkHz = desiredPointOffsetMHz * 1000;
                    bool userRelevantPoint = (expectedOffsetkHz != 0) && (actualMHz >= 500);
                    if (abs(actualOffsetkHz - expectedOffsetkHz) <= 12000) {
                        selectiveOffsetApplied++;
                        if (userRelevantPoint) userBoostApplied++;
                    } else {
                        selectiveOffsetFailed++;
                        if (userRelevantPoint) userBoostFailed++;
                        verifyDesired.curvePointMHz[ci] = actualMHz;
                        if (expectedOffsetkHz == 0 && actualOffsetkHz != 0) {
                            debug_log("selective offset: point %d excluded from selective, hardware applied residual offset %d kHz, accepting actual %u MHz\n",
                                ci, actualOffsetkHz, actualMHz);
                        } else if (expectedOffsetkHz != 0 && abs(actualOffsetkHz) < abs(expectedOffsetkHz) / 2) {
                            debug_log("selective offset: point %d hardware refused offset (expected %d kHz, got %d kHz), accepting actual %u MHz\n",
                                ci, expectedOffsetkHz, actualOffsetkHz, actualMHz);
                        } else {
                            debug_log("selective offset: point %d offset %d kHz != expected %d kHz, accepting actual %u MHz\n",
                                ci, actualOffsetkHz, expectedOffsetkHz, actualMHz);
                        }
                    }
                }
                debug_log("selective offset: boost applied=%d failed=%d\n", selectiveOffsetApplied, selectiveOffsetFailed);
                if (hasLock) {
                    debug_log("flatten undervolt: target=%u MHz applied=%d failed=%d\n", lockMhz, flattenApplied, flattenFailed);
                }
            }
            auto verify_curve_request = [&](char* detailOut, size_t detailOutSize) -> bool {
                if (!curveRequest) return true;
                if (gpuPolicyViaCurveBatch && selectiveOffsetApplied > 0 && selectiveOffsetApplied >= selectiveOffsetFailed) {
                    if (hasLock && lockMhz > 0) {
                        bool sawTailPoint = false;
                        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                            if (!lockedTailMask[ci]) continue;
                            if (g_app.curve[ci].freq_kHz == 0) continue;
                            sawTailPoint = true;
                            unsigned int actualLockMHz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
                            unsigned int tailTarget = lockMhz;
                            unsigned int toleranceMHz = curve_point_verify_tolerance_mhz(ci);
                            unsigned int deltaMHz = actualLockMHz > tailTarget ? (actualLockMHz - tailTarget) : (tailTarget - actualLockMHz);
                            if (deltaMHz > toleranceMHz) {
                                set_curve_target_mismatch_detail(ci, actualLockMHz, tailTarget, true, detailOut, detailOutSize);
                                debug_log("selective offset lock tail mismatch: ci=%d actual=%u target=%u tol=%u\n",
                                    ci, actualLockMHz, tailTarget, toleranceMHz);
                                return false;
                            }
                        }
                        if (!sawTailPoint) {
                            set_message(detailOut, detailOutSize, "No VF points were available to verify the curve lock");
                            return false;
                        }
                    }
                    debug_log("selective offset verified: boost applied=%d failed=%d, flatten applied=%d failed=%d\n", selectiveOffsetApplied, selectiveOffsetFailed, flattenApplied, flattenFailed);
                    return true;
                }
                return curve_targets_match_request(&verifyDesired, hasLock ? lockedTailMask : nullptr, lockMhz, detailOut, detailOutSize);
            };
            if (curveRequest) {
                curveRequestOk = verify_curve_request(curveVerifyDetail, sizeof(curveVerifyDetail));
                if (!curveRequestOk) {
                    // Use generous correction passes because writing large tail offsets
                    // can shift the base frequency of adjacent non-tail points (observed on
                    // Blackwell). Iterate until all points converge or the limit is reached.
                    int prevErrorKHz[VF_NUM_POINTS] = {};
                    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                        prevErrorKHz[ci] = INT_MAX;
                    }
                    // Uniform floor offset for tail points during correction.
                    // Per-point tail deltas are ineffective on Blackwell; the
                    // driver ignores them (see initial tail offset computation).
                    int correctionFloorTailOffsetKHz = 0;
                    {
                        int minkHz = 0, maxkHz = 0;
                        bool rangeOk = get_curve_offset_range_khz(&minkHz, &maxkHz);
                        if (rangeOk && minkHz < 0) {
                            correctionFloorTailOffsetKHz = minkHz;
                        } else {
                            correctionFloorTailOffsetKHz = -1000000;
                        }
                    }
                    for (int correctionPass = 0; correctionPass < 25; correctionPass++) {
                        int correctedCurveOffsets[VF_NUM_POINTS] = {};
                        bool correctedCurveMask[VF_NUM_POINTS] = {};
                        bool haveCorrections = false;
                        unsigned int lastTargetMHz = 0;
                        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                            if (g_app.curve[ci].freq_kHz == 0) {
                                lastTargetMHz = 0;
                                continue;
                            }

                            unsigned int targetMHz = 0;
                            bool isTail = (hasLock && lockedTailMask[ci] && lockMhz > 0);
                            
                            if (isTail) {
                                targetMHz = lockMhz;
                            } else if (verifyDesired.hasCurvePoint[ci]) {
                                targetMHz = verifyDesired.curvePointMHz[ci];
                            } else {
                                lastTargetMHz = 0;
                                continue;
                            }
                            debug_log("correction pass %d source: ci=%d isTail=%d targetMHz=%u verifyDesired=%u lockMhz=%u hasCP=%d origPop=%d\n",
                                correctionPass + 1, ci, isTail ? 1 : 0, targetMHz,
                                verifyDesired.hasCurvePoint[ci] ? verifyDesired.curvePointMHz[ci] : 0,
                                lockMhz, verifyDesired.hasCurvePoint[ci] ? 1 : 0,
                                originalCurvePopulated[ci] ? 1 : 0);

                            if (gpuPolicyViaCurveBatch && ci > 0 && lastTargetMHz > targetMHz && !isTail) {
                                targetMHz = lastTargetMHz;
                            } else if (gpuPolicyViaCurveBatch && ci > 0 && lastTargetMHz > targetMHz && isTail) {
                                debug_log("correction pass %d: strict tail ci=%d remains at lock target %u below previous target %u\n",
                                    correctionPass + 1, ci, targetMHz, lastTargetMHz);
                            }

                            lastTargetMHz = targetMHz;
                            verifyDesired.curvePointMHz[ci] = targetMHz;
                            verifyDesired.hasCurvePoint[ci] = true;

                            if (gpuPolicyViaCurveBatch && !isTail) {
                                correctedCurveOffsets[ci] = gpu_offset_component_mhz_for_point(ci, desired->gpuOffsetMHz, desiredActiveGpuOffsetExcludeLowCount) * 1000;
                                debug_log("correction pass %d branch: ci=%d viaCurvebatch path offset=%d\n",
                                    correctionPass + 1, ci, correctedCurveOffsets[ci]);
                            } else if (!gpuPolicyViaCurveBatch && !isTail && originalCurvePopulated[ci]) {
                                long long stockBase = (long long)originalCurveFreqkHz[ci] - (long long)originalCurveOffsets[ci];
                                if (stockBase < 0) stockBase = 0;
                                long long targetKHz = (long long)raw_curve_khz_from_display_mhz(targetMHz);
                                long long diff = targetKHz - stockBase;
                                if (diff > INT_MAX) diff = INT_MAX;
                                if (diff < INT_MIN) diff = INT_MIN;
                                correctedCurveOffsets[ci] = clamp_freq_delta_khz((int)diff);
                                debug_log("correction pass %d: ci=%d targetMHz=%u stockBase=%lld targetKHz=%lld diff=%lld live=%u offs=%d\n",
                                    correctionPass + 1, ci, targetMHz, stockBase / 1000, targetKHz, diff,
                                    displayed_curve_mhz(g_app.curve[ci].freq_kHz), g_app.freqOffsets[ci]);
                            } else if (isTail && ci != lockCi) {
                                // Tail points beyond the lock point: use uniform
                                // floor offset instead of per-point delta. On Blackwell
                                // the driver ignores per-point tail deltas, so the
                                // correction loop cannot converge with graduated offsets.
                                // A uniform minimum offset floors all tail points,
                                // letting the lock point control the entire region.
                                correctedCurveOffsets[ci] = correctionFloorTailOffsetKHz;
                                debug_log("correction pass %d branch: ci=%d tail uniform floor offset=%d target=%u live=%u offs=%d\n",
                                    correctionPass + 1, ci, correctedCurveOffsets[ci],
                                    targetMHz, displayed_curve_mhz(g_app.curve[ci].freq_kHz),
                                    g_app.freqOffsets[ci]);
                            } else {
                                correctedCurveOffsets[ci] = curve_delta_khz_for_target_display_mhz(ci, targetMHz);
                                debug_log("correction pass %d branch: ci=%d else path (live base) target=%u offset=%d live=%u offs=%d\n",
                                    correctionPass + 1, ci, targetMHz, correctedCurveOffsets[ci],
                                    displayed_curve_mhz(g_app.curve[ci].freq_kHz), g_app.freqOffsets[ci]);
                            }
                            correctedCurveMask[ci] = true;
                            haveCorrections = true;
                        }
                        if (!haveCorrections) break;
                        debug_log("curve correction pass %d: target point 75 live=%u MHz offset=%d desiredOffset=%d\n",
                            correctionPass + 1,
                            displayed_curve_mhz(g_app.curve[75].freq_kHz),
                            g_app.freqOffsets[75],
                            correctedCurveOffsets[75]);
                        set_last_apply_phase("apply: VF curve correction write");
                        bool correctionOk = apply_curve_offsets_verified(correctedCurveOffsets, correctedCurveMask, hasLock ? 3 : 2);
                        if (!correctionOk) {
                            debug_log("curve correction pass %d had an offset verification mismatch\n", correctionPass + 1);
                        }
                        // After writing correction offsets, check for non-tail points whose
                        // required correction delta exceeds the hardware range. This happens
                        // when tail offset writes shift adjacent point bases (observed on
                        // Blackwell), causing the correction to diverge: each pass writes a
                        // larger offset but the live frequency doesn't move, until the offset
                        // hits the range limit. Accept the actual MHz for such points to
                        // prevent runaway negative offset growth.
                        {
                            int divMinKHz = 0, divMaxKHz = 0;
                            bool divRangeKnown = get_curve_offset_range_khz(&divMinKHz, &divMaxKHz);
                            int converging = 0, worsening = 0, stuck = 0, outOfRange = 0;
                            for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                                if (!verifyDesired.hasCurvePoint[ci]) continue;
                                if (g_app.curve[ci].freq_kHz == 0) continue;
                                unsigned int actualMHz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
                                unsigned int targetMHz = (hasLock && lockedTailMask[ci] && lockMhz > 0)
                                    ? lockMhz : verifyDesired.curvePointMHz[ci];
                                if (actualMHz == targetMHz) continue;
                                int actualKHz = (int)g_app.curve[ci].freq_kHz;
                                int targetKHz = (int)targetMHz * 1000;
                                int errorKHz = actualKHz > targetKHz ? (actualKHz - targetKHz) : (targetKHz - actualKHz);
                                int requiredDeltaKHz = curve_delta_khz_for_target_display_mhz_unclamped(ci, targetMHz);
                                bool diverged = (divRangeKnown && (requiredDeltaKHz < divMinKHz || requiredDeltaKHz > divMaxKHz));
                                if (gpuPolicyViaCurveBatch && hasLock && lockedTailMask[ci] && lockMhz > 0) {
                                    if (!diverged && prevErrorKHz[ci] != INT_MAX && errorKHz < prevErrorKHz[ci]) {
                                        converging++;
                                        diverged = false;
                                    } else if (!diverged && prevErrorKHz[ci] != INT_MAX && errorKHz == prevErrorKHz[ci]) {
                                        stuck++;
                                        diverged = true;
                                        debug_log("correction pass %d: tail point %d stuck at actual=%u target=%u err=%dKHz requiredDelta=%dKHz range=[%d,%d]kHz known=%d\n",
                                            correctionPass + 1, ci, actualMHz, targetMHz, errorKHz,
                                            requiredDeltaKHz, divMinKHz, divMaxKHz, divRangeKnown ? 1 : 0);
                                    } else if (!diverged && prevErrorKHz[ci] != INT_MAX && errorKHz > prevErrorKHz[ci]) {
                                        worsening++;
                                        diverged = true;
                                    } else if (diverged) {
                                        outOfRange++;
                                        debug_log("correction pass %d: tail point %d out of range: actual=%u target=%u err=%dKHz requiredDelta=%dKHz range=[%d,%d]kHz\n",
                                            correctionPass + 1, ci, actualMHz, targetMHz, errorKHz,
                                            requiredDeltaKHz, divMinKHz, divMaxKHz);
                                    } else {
                                        converging++;
                                    }
                                } else if (diverged) {
                                    outOfRange++;
                                }
                                bool userExplicitPoint = explicitCurveMask[ci] && !lockedTailMask[ci];
                                // A non-tail point is "near the locked tail" if it is within 5
                                // VF points of any locked tail point.  Large flatten offsets on
                                // tail points (76-126) cause ~10-20 MHz hardware cross-talk on
                                // immediately adjacent non-tail points, preventing the correction
                                // loop from converging on the exact requested frequency.
                                bool nearLockedTail = false;
                                if (hasLock && lockMhz > 0) {
                                    for (int tailCi = 0; tailCi < VF_NUM_POINTS; tailCi++) {
                                        if (!lockedTailMask[tailCi]) continue;
                                        int dist = ci > tailCi ? (ci - tailCi) : (tailCi - ci);
                                        if (dist <= 5) {
                                            nearLockedTail = true;
                                            break;
                                        }
                                    }
                                }
                                bool acceptNonTailReadback = !gpuPolicyViaCurveBatch
                                    && hasLock
                                    && !lockedTailMask[ci]
                                    && lockMhz > 0
                                    && (!userExplicitPoint || nearLockedTail);
                                // Large tail offset writes can shift adjacent non-tail
                                // readback points. Verification-derived points always absorb
                                // that artifact. User-explicit points also absorb it when
                                // they are near the locked tail and the error is small
                                // (cross-talk, not a genuine apply failure). The locked tail
                                // itself remains strict.
                                if (acceptNonTailReadback && !diverged) {
                                    diverged = true;
                                }
                                if (diverged && acceptNonTailReadback) {
                                    debug_log("correction pass %d: non-tail %s point %d actual %u MHz != target %u MHz (tol=%u); accepting verification-only actual %u MHz (prevErr=%dKHz curErr=%dKHz)%s\n",
                                        correctionPass + 1,
                                        userExplicitPoint ? "explicit" : "readback",
                                        ci, actualMHz, targetMHz,
                                        curve_point_verify_tolerance_mhz(ci), actualMHz,
                                        prevErrorKHz[ci] == INT_MAX ? -1 : prevErrorKHz[ci], errorKHz,
                                        userExplicitPoint ? " (cross-talk near locked tail)" : "");
                                    verifyDesired.curvePointMHz[ci] = actualMHz;
                                } else if (diverged) {
                                    debug_log("correction pass %d: strict %s point %d actual %u MHz != target %u MHz (tol=%u); keeping requested target (prevErr=%dKHz curErr=%dKHz)\n",
                                        correctionPass + 1,
                                        lockedTailMask[ci] ? "tail" : (userExplicitPoint ? "explicit" : "derived"),
                                        ci, actualMHz, targetMHz,
                                        curve_point_verify_tolerance_mhz(ci),
                                        prevErrorKHz[ci] == INT_MAX ? -1 : prevErrorKHz[ci], errorKHz);
                                    prevErrorKHz[ci] = errorKHz;
                                } else {
                                    prevErrorKHz[ci] = errorKHz;
                                }
                            }
                            debug_log("correction pass %d convergence: converging=%d worsening=%d stuck=%d outOfRange=%d\n",
                                correctionPass + 1, converging, worsening, stuck, outOfRange);
                        }
                        if (verify_curve_request(curveVerifyDetail, sizeof(curveVerifyDetail))) {
                            curveRequestOk = true;
                            debug_log("curve correction pass %d converged to requested live MHz targets\n", correctionPass + 1);
                            break;
                        }
                    }
                }
                // After the correction loop, apply post-correction handling.
                // Log if the curve request converged but the batch verification
                // initially reported a mismatch (non-fatal).
                if (curveRequestOk && !curveBatchOk) {
                    debug_log("curve request matched live targets after offset verification mismatch\n");
                }
                // Check monotonicity after correction without raising the locked
                // tail target. If the driver cannot keep the user's flat tail, the
                // apply must fail rather than silently converting the lock into a
                // higher plateau.
                if (curveRequestOk && hasLock && lockMhz > 0) {
                    unsigned int lastTargetMHz = 0;
                    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                        if (!g_app.curve[ci].freq_kHz || !verifyDesired.hasCurvePoint[ci]) continue;

                        unsigned int currentTarget = (lockedTailMask[ci]) ? lockMhz : verifyDesired.curvePointMHz[ci];

                        if (lockedTailMask[ci] && ci > 0 && lastTargetMHz > currentTarget) {
                            curveRequestOk = false;
                            set_message(curveVerifyDetail, sizeof(curveVerifyDetail),
                                "Curve lock %u MHz at point %d would need to rise to %u MHz to stay monotonic; keeping the requested flat tail and failing apply",
                                lockMhz, ci, lastTargetMHz);
                            debug_log("monotonicity enforcement: strict tail violation ci=%d lockMhz=%u previousTarget=%u; not rewriting tail above lock\n",
                                ci, lockMhz, lastTargetMHz);
                            break;
                        }
                        lastTargetMHz = currentTarget;
                    }
                }
                // Log voltage consistency diagnostic: compare post-apply voltage
                // against the pre-apply snapshot to detect unexpected voltage drift.
                // Voltage is inherently immutable on NVIDIA VF tables, so any change
                // indicates a driver state shift or NVAPI read instability.
                {
                    const unsigned int VOLTAGE_DRIFT_TOLERANCE_uV = 10000; // 10 mV
                    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                        if (!originalCurvePopulated[ci]) continue;
                        unsigned int originalUv = originalCurveVoltUv[ci];
                        unsigned int currentUv = g_app.curve[ci].volt_uV;
                        if (originalUv == 0 || currentUv == 0) continue;
                        unsigned int driftUv = (originalUv > currentUv)
                            ? (originalUv - currentUv) : (currentUv - originalUv);
                        if (driftUv > VOLTAGE_DRIFT_TOLERANCE_uV) {
                            debug_log("voltage consistency: point %d drifted by %u uV (original=%u uV current=%u uV)\n",
                                ci, driftUv, originalUv, currentUv);
                        }
                    }
                }
                // Post-apply curve state dump: log all points with target vs actual
                // to detect weird shifts that differ from the intended VF curve shape.
                // Also always log first/last tail point and any tail drift > 2 MHz.
                {
                    int tailOff = 0, tailOK = 0, nonTailOff = 0, nonTailOK = 0;
                    int firstTail = -1, lastTail = -1;
                    unsigned int firstTailActual = 0, firstTailTarget = 0;
                    unsigned int lastTailActual = 0, lastTailTarget = 0;
                    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                        if (!verifyDesired.hasCurvePoint[ci]) continue;
                        if (g_app.curve[ci].freq_kHz == 0) continue;
                        unsigned int actualMHz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
                        unsigned int targetMHz = (lockedTailMask[ci] && lockMhz > 0) ? lockMhz : verifyDesired.curvePointMHz[ci];
                        unsigned int delta = actualMHz > targetMHz ? actualMHz - targetMHz : targetMHz - actualMHz;
                        int tol = (int)curve_point_verify_tolerance_mhz(ci);
                        bool isTail = (lockedTailMask[ci] && lockMhz > 0);
                        if (delta > (unsigned int)tol) {
                            debug_log("post-apply curve: ci=%d actual=%u target=%u delta=%u tol=%d freqOffs=%d %s\n",
                                ci, actualMHz, targetMHz, delta, tol, g_app.freqOffsets[ci],
                                isTail ? "TAIL" : "BOOST");
                            if (isTail) tailOff++; else nonTailOff++;
                        } else if (isTail && delta > 2) {
                            tailOK++;
                            debug_log("post-apply tail: ci=%d actual=%u target=%u delta=%u tol=%d freqOffs=%d\n",
                                ci, actualMHz, targetMHz, delta, tol, g_app.freqOffsets[ci]);
                        } else if (isTail) {
                            tailOK++;
                        } else {
                            nonTailOK++;
                        }
                        if (isTail) {
                            if (firstTail < 0) {
                                firstTail = ci;
                                firstTailActual = actualMHz;
                                firstTailTarget = targetMHz;
                            }
                            lastTail = ci;
                            lastTailActual = actualMHz;
                            lastTailTarget = targetMHz;
                        }
                    }
                    debug_log("post-apply tail bookends: first=ci%d actual=%u target=%u last=ci%d actual=%u target=%u\n",
                        firstTail, firstTailActual, firstTailTarget,
                        lastTail, lastTailActual, lastTailTarget);
                    if (tailOff > 0 || nonTailOff > 0) {
                        debug_log("post-apply curve summary: tail=%dOK+%dOFF boost=%dOK+%dOFF\n",
                            tailOK, tailOff, nonTailOK, nonTailOff);
                    }
                    if (hasLock && lockMhz > 0) {
                        flattenApplied = tailOK;
                        flattenFailed = tailOff;
                    }
                }
                // Success/failure counting and state persistence
                if (curveRequestOk) {
                    successCount++;
                    g_app.lastApplyUsedGpuOffset = gpuPolicyViaCurveBatch;
                    if (gpuPolicyViaCurveBatch) {
                        g_app.appliedGpuOffsetMHz = desired->gpuOffsetMHz;
                        g_app.appliedGpuOffsetExcludeLowCount = desiredActiveGpuOffsetExcludeLowCount;
                        if (desiredActiveGpuOffsetExcludeLowCount > 0) {
                            persist_runtime_selective_gpu_offset_request(desired->gpuOffsetMHz, desiredActiveGpuOffsetExcludeLowCount);
                        } else {
                            clear_runtime_selective_gpu_offset_request();
                        }
                    } else if (!desired->hasGpuOffset || desiredActiveGpuOffsetExcludeLowCount <= 0) {
                        clear_runtime_selective_gpu_offset_request();
                    }
                } else {
                    failCount++;
                    partialApplyRisk = true;
                    if (curveVerifyDetail[0]) {
                        append_failure("%s", curveVerifyDetail);
                    } else if (hasLock && lockMhz > 0) {
                        append_failure("Curve lock to %u MHz did not verify after apply", lockMhz);
                    } else if (gpuPolicyViaCurveBatch) {
                        append_failure("GPU offset %d MHz did not verify after apply", desired->gpuOffsetMHz);
                    } else {
                        append_failure("VF curve update did not verify after apply");
                    }
                }
            }
        }
        if (memApplied && preserveCurveAcrossMem && !curveRequest && !curveBatchOk) {
            curveRequestOk = false;
            failCount++;
            partialApplyRisk = true;
            append_failure("Restoring the existing VF curve after the memory offset did not verify");
        }
        if (curveRequestOk) {
            int pos = 0;
            if (gpuPolicyViaCurveBatch) {
                if (userBoostFailed == 0) {
                    pos += StringCchPrintfA(curveVerifySummary + pos, sizeof(curveVerifySummary) - pos,
                        " %+d MHz selective offset verified.", desired->gpuOffsetMHz);
                } else {
                    pos += StringCchPrintfA(curveVerifySummary + pos, sizeof(curveVerifySummary) - pos,
                        " %+d MHz selective offset: %d of %d boost points matched.",
                        desired->gpuOffsetMHz, userBoostApplied, userBoostApplied + userBoostFailed);
                }
            }
            if (hasLock && lockMhz > 0) {
                if (flattenFailed == 0) {
                    pos += StringCchPrintfA(curveVerifySummary + pos, sizeof(curveVerifySummary) - pos,
                        " Undervolt flatten to %u MHz verified (%d pts).", lockMhz, flattenApplied);
                } else {
                    pos += StringCchPrintfA(curveVerifySummary + pos, sizeof(curveVerifySummary) - pos,
                        " Undervolt flatten to %u MHz: %d of %d pts matched.",
                        lockMhz, flattenApplied, flattenApplied + flattenFailed);
                }
            }
        }
    }
    if (hasLock) {
        g_app.lockedVi = lockVi;
        g_app.lockedCi = lockCi;
        g_app.lockedFreq = lockMhz;
        g_app.appliedLockVi = lockVi;
        g_app.appliedLockCi = lockCi;
        g_app.appliedLockFreq = lockMhz;
        g_app.guiLockTracksAnchor = desired->lockTracksAnchor;
    }
    if (desired->hasGpuOffset && !gpuPolicyViaCurveBatch) {
        if (desiredActiveGpuOffsetExcludeLowCount > 0) {
            persist_runtime_selective_gpu_offset_request(desired->gpuOffsetMHz, desiredActiveGpuOffsetExcludeLowCount);
        } else {
            clear_runtime_selective_gpu_offset_request();
        }
    } else if (!desired->hasGpuOffset && curveTouched && failCount == 0) {
        clear_runtime_selective_gpu_offset_request();
    }
    if (desired->hasPowerLimit) {
        int currentPowerPct = g_app.powerLimitPct;
        if (desired->powerLimitPct != currentPowerPct) {
            if (desired->powerLimitPct < 50 || desired->powerLimitPct > 150) {
                append_failure("Power limit %d%% outside safe range 50..150%%", desired->powerLimitPct);
                failCount++;
                partialApplyRisk = true;
            } else {
                powerChanged = true;
                set_last_apply_phase("apply: power limit write");
                if (nvapi_set_power_limit(desired->powerLimitPct)) successCount++; else {
                    failCount++;
                    partialApplyRisk = true;
                    append_failure("Power limit %d%% was not accepted by the driver", desired->powerLimitPct);
                }
            }
        }
    }
    bool fanChanged = false;
    if (!apply_fan_settings(desired, failureDetails, sizeof(failureDetails), successCount, failCount, result, resultSize, fanChanged)) {
        if (successCount > 0) {
            partialApplyRisk = true;
            rollback_to_safe_defaults();
            g_app.gpuClockOffsetkHz = 0;
            g_app.memClockOffsetkHz = 0;
            g_app.powerLimitPct = 0;
            char rollbackDetail[128] = {};
            refresh_global_state(rollbackDetail, sizeof(rollbackDetail));
            debug_log("apply: fan failure triggered rollback of %d earlier hardware writes\n", successCount);
        }
        return false;
    }
    if (failCount > 0 && successCount > 0) {
        partialApplyRisk = true;
        rollback_to_safe_defaults();
        g_app.gpuClockOffsetkHz = 0;
        g_app.memClockOffsetkHz = 0;
        g_app.powerLimitPct = 0;
        char rollbackDetail[128] = {};
        refresh_global_state(rollbackDetail, sizeof(rollbackDetail));
    }
    char detail[128] = {};
    if (memApplied || powerChanged || fanChanged) {
        refresh_global_state(detail, sizeof(detail));
    } else if (!curveTouched) {
        detect_clock_offsets();
    }
    EnterCriticalSection(&g_appLock);
    // A post-apply global refresh may suppress live lock auto-detection (for example
    // on selective GPU offset profiles) and clear the GUI lock markers even though
    // this apply explicitly requested a lock. Restore the requested lock state so
    // the VF controls continue to match the just-applied profile.
    if (hasLock) {
        unsigned int displayedLockMHz = lockMhz;
        g_app.lockedVi = lockVi;
        g_app.lockedCi = lockCi;
        g_app.lockedFreq = displayedLockMHz;
        g_app.appliedLockVi = lockVi;
        g_app.appliedLockCi = lockCi;
        g_app.appliedLockFreq = displayedLockMHz;
        g_app.guiLockTracksAnchor = desired->lockTracksAnchor;
        debug_log("post-apply lock restore: ci=%d requested=%u displayed=%u trackAnchor=%d (preserving requested value)\n",
            lockCi,
            lockMhz,
            displayedLockMHz,
            desired->lockTracksAnchor ? 1 : 0);
    } else if (g_app.isServiceProcess && failCount == 0 && (curveTouched || desired->hasGpuOffset || hasCurveEdits)) {
        g_app.lockedVi = -1;
        g_app.lockedCi = -1;
        g_app.lockedFreq = 0;
        g_app.appliedLockVi = -1;
        g_app.appliedLockCi = -1;
        g_app.appliedLockFreq = 0;
        g_app.guiLockTracksAnchor = true;
        debug_log("post-apply lock clear: no lock requested; cleared stale service lock markers\n");
    }
    capture_last_operation_snapshot(g_lastOperationAfterSnapshot, sizeof(g_lastOperationAfterSnapshot));
    if (!g_app.isServiceProcess) {
        populate_global_controls();
        if (interactive) {
            populate_edits();
            invalidate_main_window();
        }
    }
    LeaveCriticalSection(&g_appLock);
    set_last_apply_phase(failCount == 0 ? "apply: complete" : "apply: failed");
    if (successCount == 0 && failCount == 0) {
        set_message(result, resultSize, "No setting changes needed.");
    } else if (failCount == 0) {
        set_message(result, resultSize, "Applied %d setting changes successfully.%s", successCount, curveVerifySummary);
    } else {
        char logErr[256] = {};
        bool logWritten = write_error_report_log("Setting apply reported one or more failures", failureDetails, logErr, sizeof(logErr));
        if (failureDetails[0]) {
            set_message(result, resultSize, "%sApplied %d OK, %d failed: %s%s%s",
                partialApplyRisk ? "Live state may now be a mixed partial apply. " : "",
                successCount,
                failCount,
                failureDetails,
                logWritten ? " See " : "",
                logWritten ? error_log_path() : "");
        } else {
            set_message(result, resultSize, "%sApplied %d OK, %d failed.%s%s",
                partialApplyRisk ? "Live state may now be a mixed partial apply. " : "",
                successCount,
                failCount,
                logWritten ? " See " : "",
                logWritten ? error_log_path() : "");
        }
        if (!logWritten && logErr[0]) {
            debug_log("failed to write error report: %s\n", logErr);
        }
    }
    return failCount == 0;
}
