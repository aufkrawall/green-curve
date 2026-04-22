static bool apply_fan_settings(const DesiredSettings* desired, char* failureDetails, size_t failureDetailsSize, int& successCount, int& failCount, char* result, size_t resultSize, bool& outFanChanged) {
    outFanChanged = false;
    if (!desired->hasFan) return true;

    auto append_failure = [&](const char* fmt, ...) {
        char part[256] = {};
        va_list ap;
        va_start(ap, fmt);
        StringCchVPrintfA(part, ARRAY_COUNT(part), fmt, ap);
        va_end(ap);
        if (!part[0]) return;
        if (failureDetails[0]) {
            StringCchCatA(failureDetails, (int)failureDetailsSize, "; ");
        }
        StringCchCatA(failureDetails, (int)failureDetailsSize, part);
        debug_log("apply failure: %s\n", part);
    };

    int desiredFanMode = desired->fanMode;
    if (desiredFanMode < FAN_MODE_AUTO || desiredFanMode > FAN_MODE_CURVE) {
        desiredFanMode = desired->fanAuto ? FAN_MODE_AUTO : FAN_MODE_FIXED;
    }
    FanCurveConfig desiredCurve = desired->fanCurve;
    fan_curve_normalize(&desiredCurve);
    if (desiredFanMode == FAN_MODE_CURVE) {
        char validationErr[256] = {};
        if (!fan_curve_validate(&desiredCurve, validationErr, sizeof(validationErr))) {
            set_message(result, resultSize, "%s", validationErr);
            return false;
        }
    }

    bool fanChanged = false;
    if (!fan_setting_matches_current(desiredFanMode, desired->fanPercent, &desiredCurve)) {
        fanChanged = true;
        bool exact = false;
        char detail[128] = {};
        bool ok = false;
        if (desiredFanMode == FAN_MODE_AUTO) {
            stop_fan_curve_runtime();
            ok = nvml_set_fan_auto(detail, sizeof(detail));
            if (ok) {
                g_app.activeFanMode = FAN_MODE_AUTO;
            }
        } else if (desiredFanMode == FAN_MODE_FIXED) {
            stop_fan_curve_runtime();
            if (validate_manual_fan_percent_for_runtime(desired->fanPercent, detail, sizeof(detail))) {
                if (g_app.hMainWnd || g_app.isServiceProcess) {
                    g_app.activeFanFixedPercent = clamp_percent(desired->fanPercent);
                    start_fixed_fan_runtime();
                    ok = g_app.fanFixedRuntimeActive && g_app.fanRuntimeLastApplyTickMs != 0;
                    if (!ok) {
                        set_message(detail, sizeof(detail),
                            g_app.fanRuntimeConsecutiveFailures > 0
                                ? "Failed to verify the initial fixed fan apply"
                                : "Failed to start fixed fan maintenance");
                    }
                } else {
                    ok = nvml_set_fan_manual(desired->fanPercent, &exact, detail, sizeof(detail));
                }
            }
            if (ok) {
                g_app.activeFanMode = FAN_MODE_FIXED;
                g_app.activeFanFixedPercent = clamp_percent(desired->fanPercent);
            }
        } else {
            copy_fan_curve(&g_app.activeFanCurve, &desiredCurve);
            debug_log("apply fan curve: pollMs=%d hysteresis=%d firstEnabledPct=%d serviceProcess=%d\n",
                g_app.activeFanCurve.pollIntervalMs,
                g_app.activeFanCurve.hysteresisC,
                g_app.activeFanCurve.points[0].enabled ? g_app.activeFanCurve.points[0].fanPercent : 0,
                g_app.isServiceProcess ? 1 : 0);
            if (!validate_fan_curve_for_runtime(&desiredCurve, detail, sizeof(detail))) {
                ok = false;
            } else if (g_app.hMainWnd || g_app.isServiceProcess) {
                start_fan_curve_runtime();
                ok = g_app.fanCurveRuntimeActive && g_app.fanRuntimeLastApplyTickMs != 0;
                debug_log("apply fan curve runtime start: active=%d lastApply=%llu failures=%u\n",
                    g_app.fanCurveRuntimeActive ? 1 : 0,
                    g_app.fanRuntimeLastApplyTickMs,
                    g_app.fanRuntimeConsecutiveFailures);
                if (!ok) {
                    set_message(detail, sizeof(detail),
                        g_app.fanRuntimeConsecutiveFailures > 0
                            ? "Failed to verify the initial fan curve apply"
                            : "Failed to start fan curve maintenance");
                }
            } else {
                set_message(detail, sizeof(detail),
                    "Fan curve mode requires the resident Green Curve tray app. Start the program hidden to tray for long-running fan control.");
            }
        }
        if (ok) successCount++;
        else {
            failCount++;
            append_failure("Fan control change failed%s%s",
                detail[0] ? ": " : "",
                detail[0] ? detail : "");
        }
    }
    outFanChanged = fanChanged;
    return true;
}

static bool apply_desired_settings(const DesiredSettings* desired, bool interactive, char* result, size_t resultSize) {
    if (!g_app.isServiceProcess) {
        refresh_background_service_state();
        if (!g_app.backgroundServiceAvailable) {
            set_message(result, resultSize,
                g_app.backgroundServiceInstalled
                    ? "Background service is not responding. Live GPU control is unavailable until it starts responding again."
                    : "Background service is not installed. Install it to enable live GPU control.");
            return false;
        }
        ServiceSnapshot snapshot = {};
        bool ok = service_client_apply_desired(desired, g_pendingOperationSource[0] ? g_pendingOperationSource : "client apply", interactive, result, resultSize, &snapshot);
        if (snapshot.initialized || snapshot.loaded) {
            apply_service_snapshot_to_app(&snapshot);
            if (desired && desired->hasLock && desired->lockCi >= 0 && desired->lockCi < VF_NUM_POINTS && snapshot.curve[desired->lockCi].freq_kHz > 0) {
                g_app.lockedCi = desired->lockCi;
                g_app.lockedFreq = displayed_curve_mhz(snapshot.curve[desired->lockCi].freq_kHz);
                g_app.guiLockTracksAnchor = desired->lockTracksAnchor;
                for (int vi = 0; vi < g_app.numVisible; vi++) {
                    if (g_app.visibleMap[vi] == desired->lockCi) {
                        g_app.lockedVi = vi;
                        break;
                    }
                }
                debug_log("service apply client lock sync: ci=%d liveMHz=%u trackAnchor=%d\n",
                    g_app.lockedCi,
                    g_app.lockedFreq,
                    g_app.guiLockTracksAnchor ? 1 : 0);
            }
            if (g_app.hMainWnd) {
                populate_global_controls();
                if (g_app.loaded) populate_edits();
                invalidate_main_window();
            }
        }
        return ok;
    }

    if (!desired) {
        set_message(result, resultSize, "No desired settings");
        return false;
    }

    clear_last_operation_details();
    build_operation_intent_summary(desired, interactive, g_lastOperationIntent, sizeof(g_lastOperationIntent));
    capture_last_operation_snapshot(g_lastOperationBeforeSnapshot, sizeof(g_lastOperationBeforeSnapshot));

    debug_log("apply_desired_settings: hasGpuOffset=%d gpuOffsetMHz=%d gpuOffsetExcludeLow70=%d\n",
        desired->hasGpuOffset ? 1 : 0, desired->gpuOffsetMHz, desired->gpuOffsetExcludeLow70 ? 1 : 0);

    int successCount = 0;
    int failCount = 0;
    char failureDetails[1024] = {};
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
    bool hasLock = (desired->hasLock && desired->lockCi >= 0 && desired->lockMHz > 0)
        || (interactive && g_app.lockedVi >= 0 && g_app.lockedVi < g_app.numVisible);
    bool hasCurveEdits = false;
    int lockCi = -1;
    int lockVi = -1;
    unsigned int lockMhz = 0;
    bool memOffsetValid = true;
    bool shouldApplyMemOffset = false;
    int targetMemkHz = 0;
    bool memApplied = false;
    bool powerChanged = false;
    int currentAppliedGpuOffsetMHz = current_applied_gpu_offset_mhz();
    bool currentActiveGpuOffsetExcludeLow70 = current_applied_gpu_offset_excludes_low_points();
    int targetGpuOffsetkHz = currentAppliedGpuOffsetMHz * 1000;
    bool gpuOffsetValid = true;
    bool shouldApplyGpuOffset = false;
    bool desiredActiveGpuOffsetExcludeLow70 = false;
    bool gpuPolicyViaCurveBatch = false;
    bool gpuPolicyChangeRequested = false;
    bool partialApplyRisk = false;
    int originalCurveOffsets[VF_NUM_POINTS] = {};
    int originalCurveFreqkHz[VF_NUM_POINTS] = {};
    bool originalCurvePopulated[VF_NUM_POINTS] = {};
    int targetCurveOffsets[VF_NUM_POINTS] = {};
    bool targetCurveMask[VF_NUM_POINTS] = {};
    bool lockedTailMask[VF_NUM_POINTS] = {};
    bool explicitCurveMask[VF_NUM_POINTS] = {};
    bool haveNonZeroCurveOffsets = false;

    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        originalCurveOffsets[ci] = g_app.freqOffsets[ci];
        originalCurveFreqkHz[ci] = (int)g_app.curve[ci].freq_kHz;
        originalCurvePopulated[ci] = g_app.curve[ci].freq_kHz > 0;
        if (originalCurveOffsets[ci] != 0) haveNonZeroCurveOffsets = true;
        if (desired->hasCurvePoint[ci]) {
            hasCurveEdits = true;
            explicitCurveMask[ci] = true;
        }
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
        if (!g_app.memOffsetRangeKnown ||
            (desired->memOffsetMHz >= g_app.memClockOffsetMinMHz && desired->memOffsetMHz <= g_app.memClockOffsetMaxMHz)) {
            targetMemkHz = mem_driver_khz_from_display_mhz(desired->memOffsetMHz);
            shouldApplyMemOffset = (g_app.memClockOffsetkHz != targetMemkHz);
        } else {
            memOffsetValid = false;
        }
    }

    if (desired->hasGpuOffset) {
        if (!g_app.gpuOffsetRangeKnown ||
            (desired->gpuOffsetMHz >= g_app.gpuClockOffsetMinMHz && desired->gpuOffsetMHz <= g_app.gpuClockOffsetMaxMHz)) {
            desiredActiveGpuOffsetExcludeLow70 = desired->gpuOffsetExcludeLow70 && desired->gpuOffsetMHz != 0;
            targetGpuOffsetkHz = desired->gpuOffsetMHz * 1000;
            gpuPolicyChangeRequested =
                (targetGpuOffsetkHz != currentAppliedGpuOffsetMHz * 1000) ||
                (desiredActiveGpuOffsetExcludeLow70 != currentActiveGpuOffsetExcludeLow70);
            shouldApplyGpuOffset = gpuPolicyChangeRequested && !currentActiveGpuOffsetExcludeLow70 && !desiredActiveGpuOffsetExcludeLow70;
            gpuPolicyViaCurveBatch = gpuPolicyChangeRequested && (currentActiveGpuOffsetExcludeLow70 || desiredActiveGpuOffsetExcludeLow70);
            debug_log("desired gpu offset mhz=%d current=%d shouldApply=%d viaCurve=%d desiredSelective=%d currentSelective=%d\n",
                desired->gpuOffsetMHz,
                currentAppliedGpuOffsetMHz,
                shouldApplyGpuOffset ? 1 : 0,
                gpuPolicyViaCurveBatch ? 1 : 0,
                desiredActiveGpuOffsetExcludeLow70 ? 1 : 0,
                currentActiveGpuOffsetExcludeLow70 ? 1 : 0);
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
    if (!memOffsetValid) {
        failCount++;
        partialApplyRisk = true;
        append_failure("Memory offset %d MHz is outside the supported range %d..%d MHz",
            desired->memOffsetMHz, g_app.memClockOffsetMinMHz, g_app.memClockOffsetMaxMHz);
    }

    bool gpuApplied = false;

    // Apply GPU offset first via dedicated path (handles uniform offset reliably).
    // When combined with lock/curve edits, applying the GPU offset separately avoids
    // sending highly non-uniform offsets for all curve points in a single batch,
    // which can fail. After this, the curve batch only needs to adjust the lock tail.
    if (gpuOffsetValid && shouldApplyGpuOffset) {
        if (nvapi_set_gpu_offset(targetGpuOffsetkHz)) {
            successCount++;
            gpuApplied = true;
            g_app.appliedGpuOffsetMHz = desired->gpuOffsetMHz;
            g_app.appliedGpuOffsetExcludeLow70 = false;
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
        debug_log("selective offset: zeroing prior offset %d MHz before transition\n", currentAppliedGpuOffsetMHz);
        if (nvapi_set_gpu_offset(0)) {
            currentAppliedGpuOffsetMHz = 0;
            g_app.appliedGpuOffsetMHz = 0;
            g_app.appliedGpuOffsetExcludeLow70 = false;
            for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                originalCurveOffsets[ci] = g_app.freqOffsets[ci];
                originalCurveFreqkHz[ci] = (int)g_app.curve[ci].freq_kHz;
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
            "Memory offset apply: requested=%d valid=%d shouldApply=%d\r\n"
            "Curve plan: userCurveRequest=%d curveRequest=%d hasLock=%d lockCi=%d lockMHz=%u lockTracksAnchor=%d preserveAcrossMem=%d\r\n"
            "Explicit curve points: %s\r\n"
            "Locked tail points: %s\r\n",
            desired->hasGpuOffset ? desired->gpuOffsetMHz : currentAppliedGpuOffsetMHz,
            gpuOffsetValid ? 1 : 0,
            shouldApplyGpuOffset ? 1 : 0,
            gpuPolicyViaCurveBatch ? 1 : 0,
            desiredActiveGpuOffsetExcludeLow70 ? 1 : 0,
            currentActiveGpuOffsetExcludeLow70 ? 1 : 0,
            desired->hasMemOffset ? desired->memOffsetMHz : mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz),
            memOffsetValid ? 1 : 0,
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
            bool currentDetected = (currentAppliedGpuOffsetMHz != 0 || currentActiveGpuOffsetExcludeLow70);

            for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                if (!originalCurvePopulated[ci]) continue;
                int desiredPointGpuOffsetkHz = gpu_offset_component_mhz_for_point(ci, desired->gpuOffsetMHz, desiredActiveGpuOffsetExcludeLow70) * 1000;

                int currentPointGpuOffsetkHz;
                if (currentDetected) {
                    currentPointGpuOffsetkHz = gpu_offset_component_mhz_for_point(ci, currentAppliedGpuOffsetMHz, currentActiveGpuOffsetExcludeLow70) * 1000;
                } else {
                    currentPointGpuOffsetkHz = originalCurveOffsets[ci];
                }

                int targetOffset = clamp_freq_delta_khz(originalCurveOffsets[ci] - currentPointGpuOffsetkHz + desiredPointGpuOffsetkHz);
                targetCurveOffsets[ci] = targetOffset;
                targetCurveMask[ci] = true;
            }
            debug_log("selective offset: currentMHz=%d desiredMHz=%d currentExcl=%d desiredExcl=%d detected=%d hasLock=%d lockMHz=%d\n",
                currentAppliedGpuOffsetMHz, desired->gpuOffsetMHz,
                currentActiveGpuOffsetExcludeLow70 ? 1 : 0,
                desiredActiveGpuOffsetExcludeLow70 ? 1 : 0,
                currentDetected ? 1 : 0,
                hasLock ? 1 : 0, lockMhz);
        }

        if (hasLock && lockMhz > 0) {
            for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                if (!desired->hasCurvePoint[ci]) continue;
                if (lockedTailMask[ci]) continue;
                if (!originalCurvePopulated[ci]) continue;
                long long base = (long long)originalCurveFreqkHz[ci] - (long long)originalCurveOffsets[ci];
                if (base < 0) base = 0;
                long long target = (long long)desired->curvePointMHz[ci] * 1000LL;
                targetCurveOffsets[ci] = clamp_freq_delta_khz((int)(target - base));
                targetCurveMask[ci] = true;
            }
            for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                if (!lockedTailMask[ci]) continue;
                if (!originalCurvePopulated[ci]) continue;
                long long base = (long long)originalCurveFreqkHz[ci] - (long long)originalCurveOffsets[ci];
                if (base < 0) base = 0;
                long long target = (long long)lockMhz * 1000LL;
                targetCurveOffsets[ci] = clamp_freq_delta_khz((int)(target - base));
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
                targetCurveOffsets[ci] = clamp_freq_delta_khz((int)(target - base));
                targetCurveMask[ci] = true;
            }
        }
    }

    if (desired->hasMemOffset) {
        if (memOffsetValid) {
            if (shouldApplyMemOffset) {
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
        } else {
            debug_log("apply mem offset: rejected (out of range %d..%d MHz)\n",
                g_app.memClockOffsetMinMHz, g_app.memClockOffsetMaxMHz);
        }
    }

    bool curveBatchOk = true;
    bool curveBatchNeeded = false;
    bool curveTouched = gpuApplied;
    int selectiveOffsetApplied = 0;
    int selectiveOffsetFailed = 0;
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (targetCurveMask[ci]) {
            curveBatchNeeded = true;
            break;
        }
    }
    if (curveBatchNeeded && (curveRequest || memApplied)) {
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
        // Sanity-check for extreme offsets that are likely to cause driver issues.
        int extremeOffsets = 0;
        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
            if (!targetCurveMask[ci]) continue;
            if (abs(targetCurveOffsets[ci]) > 600000) { // > 600 MHz
                extremeOffsets++;
                debug_log("apply curve batch: warning point %d target offset %d kHz exceeds 600 MHz sanity limit\n",
                    ci, targetCurveOffsets[ci]);
            }
        }
        if (extremeOffsets > 0) {
            debug_log("apply curve batch: %d points have extreme offsets (batch will proceed)\n", extremeOffsets);
        }
        debug_log("apply curve batch: points=%d range=%d..%d passes=%d\n",
            batchedCount, batchedMinCi, batchedMaxCi, hasLock ? 3 : 2);
        curveBatchOk = apply_curve_offsets_verified(targetCurveOffsets, targetCurveMask, hasLock ? 3 : 2);
        bool settledOffsetsOk = false;
        if (!read_live_curve_snapshot_settled(6, 25, &settledOffsetsOk)) {
            debug_log("apply curve: settled refresh failed after curve batch\n");
        }
        char curveVerifyDetail[256] = {};
        bool curveRequestOk = true;
        DesiredSettings verifyDesired = *desired;
        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
            if (hasLock && lockedTailMask[ci]) continue;
            if (!desired->hasCurvePoint[ci]) {
                verifyDesired.hasCurvePoint[ci] = false;
                verifyDesired.curvePointMHz[ci] = 0;
            }
        }

        if (gpuPolicyViaCurveBatch) {
            bool currentDetected = (currentAppliedGpuOffsetMHz != 0 || currentActiveGpuOffsetExcludeLow70);
            for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                if (!originalCurvePopulated[ci] || verifyDesired.hasCurvePoint[ci]) continue;

                long long currentPointGpuOffsetkHz;
                if (currentDetected) {
                    currentPointGpuOffsetkHz = (long long)gpu_offset_component_mhz_for_point(ci, currentAppliedGpuOffsetMHz, currentActiveGpuOffsetExcludeLow70) * 1000LL;
                } else {
                    currentPointGpuOffsetkHz = originalCurveOffsets[ci];
                }

                long long targetFreqkHz = (long long)originalCurveFreqkHz[ci]
                    - currentPointGpuOffsetkHz
                    + (long long)gpu_offset_component_mhz_for_point(ci, desired->gpuOffsetMHz, desiredActiveGpuOffsetExcludeLow70) * 1000LL;
                if (targetFreqkHz < 0) targetFreqkHz = 0;

                verifyDesired.hasCurvePoint[ci] = true;
                verifyDesired.curvePointMHz[ci] = displayed_curve_mhz((unsigned int)targetFreqkHz);
            }

            // Some VF points may have hardware limits that prevent the selective
            // offset from taking effect (e.g. special max-clock limit points on
            // Blackwell, or rounding edge cases). Accept the actual live frequency
            // for points where the hardware didn't apply the expected offset, so the
            // overall operation isn't marked as failed for a single stubborn point.
            selectiveOffsetApplied = 0;
            selectiveOffsetFailed = 0;
            for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                if (!verifyDesired.hasCurvePoint[ci]) continue;
                if (g_app.curve[ci].freq_kHz == 0) continue;
                unsigned int actualMHz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
                int desiredPointOffsetMHz = gpu_offset_component_mhz_for_point(ci, desired->gpuOffsetMHz, desiredActiveGpuOffsetExcludeLow70);
                int actualOffsetkHz = g_app.freqOffsets[ci];
                int expectedOffsetkHz = desiredPointOffsetMHz * 1000;
                if (abs(actualOffsetkHz - expectedOffsetkHz) <= 12000) {
                    selectiveOffsetApplied++;
                } else {
                    selectiveOffsetFailed++;
                    verifyDesired.curvePointMHz[ci] = actualMHz;
                    debug_log("selective offset: point %d offset %d kHz != expected %d kHz, accepting actual %u MHz\n",
                        ci, actualOffsetkHz, expectedOffsetkHz, actualMHz);
                }
            }
            debug_log("selective offset: applied=%d failed=%d\n", selectiveOffsetApplied, selectiveOffsetFailed);
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
                        unsigned int toleranceMHz = curve_point_verify_tolerance_mhz(ci);
                        unsigned int deltaMHz = actualLockMHz > lockMhz ? (actualLockMHz - lockMhz) : (lockMhz - actualLockMHz);
                        if (deltaMHz > toleranceMHz) {
                            set_curve_target_mismatch_detail(ci, actualLockMHz, lockMhz, true, detailOut, detailOutSize);
                            debug_log("selective offset lock tail mismatch: ci=%d actual=%u target=%u tol=%u\n",
                                ci, actualLockMHz, lockMhz, toleranceMHz);
                            return false;
                        }
                    }
                    if (!sawTailPoint) {
                        set_message(detailOut, detailOutSize, "No VF points were available to verify the curve lock");
                        return false;
                    }
                }
                debug_log("selective offset verified: %d applied, %d failed (accepted)\n", selectiveOffsetApplied, selectiveOffsetFailed);
                return true;
            }
            return curve_targets_match_request(&verifyDesired, hasLock ? lockedTailMask : nullptr, lockMhz, detailOut, detailOutSize);
        };

        if (curveRequest) {
            curveRequestOk = verify_curve_request(curveVerifyDetail, sizeof(curveVerifyDetail));
            if (!curveRequestOk) {
                for (int correctionPass = 0; correctionPass < 2; correctionPass++) {
                    int correctedCurveOffsets[VF_NUM_POINTS] = {};
                    bool correctedCurveMask[VF_NUM_POINTS] = {};
                    bool haveCorrections = false;

                    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                        unsigned int targetMHz = 0;
                        bool haveTarget = false;

                        if (hasLock && lockedTailMask[ci] && lockMhz > 0) {
                            targetMHz = lockMhz;
                            haveTarget = true;
                        } else if (verifyDesired.hasCurvePoint[ci]) {
                            targetMHz = verifyDesired.curvePointMHz[ci];
                            haveTarget = true;
                        }

                        if (!haveTarget || g_app.curve[ci].freq_kHz == 0) continue;

                        correctedCurveOffsets[ci] = curve_delta_khz_for_target_display_mhz(ci, targetMHz);
                        correctedCurveMask[ci] = true;
                        haveCorrections = true;
                    }

                    if (!haveCorrections) break;

                    bool correctionOk = apply_curve_offsets_verified(correctedCurveOffsets, correctedCurveMask, hasLock ? 3 : 2);
                    if (!correctionOk) {
                        debug_log("curve correction pass %d had an offset verification mismatch\n", correctionPass + 1);
                    }
                    if (verify_curve_request(curveVerifyDetail, sizeof(curveVerifyDetail))) {
                        curveRequestOk = true;
                        debug_log("curve correction pass %d converged to requested live MHz targets\n", correctionPass + 1);
                        break;
                    }
                }
            }
            if (curveRequestOk && !curveBatchOk) {
                debug_log("curve request matched live targets after offset verification mismatch\n");
            }
        }
        if (curveRequest) {
            if (curveRequestOk) {
                successCount++;
                if (gpuPolicyViaCurveBatch) {
                    g_app.appliedGpuOffsetMHz = desired->gpuOffsetMHz;
                    g_app.appliedGpuOffsetExcludeLow70 = desiredActiveGpuOffsetExcludeLow70;
                    persist_runtime_selective_gpu_offset_request(desired->gpuOffsetMHz, desiredActiveGpuOffsetExcludeLow70);
                } else if (!desired->hasGpuOffset || !desiredActiveGpuOffsetExcludeLow70) {
                    persist_runtime_selective_gpu_offset_request(0, false);
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
        if (memApplied && preserveCurveAcrossMem && !curveRequestOk && !curveRequest) {
            failCount++;
            partialApplyRisk = true;
            append_failure("Restoring the existing VF curve after the memory offset did not verify");
        }
    }

    if (hasLock) {
        g_app.lockedVi = lockVi;
        g_app.lockedCi = lockCi;
        g_app.lockedFreq = lockMhz;
        g_app.guiLockTracksAnchor = desired->lockTracksAnchor;
    }
    if (desired->hasGpuOffset && !gpuPolicyViaCurveBatch) {
        persist_runtime_selective_gpu_offset_request(desired->gpuOffsetMHz, desiredActiveGpuOffsetExcludeLow70);
    } else if (!desired->hasGpuOffset && curveTouched && failCount == 0) {
        persist_runtime_selective_gpu_offset_request(0, false);
    }
    if (desired->hasPowerLimit) {
        int currentPowerPct = g_app.powerLimitPct;
        if (desired->powerLimitPct != currentPowerPct) {
            powerChanged = true;
            if (nvapi_set_power_limit(desired->powerLimitPct)) successCount++; else {
                failCount++;
                partialApplyRisk = true;
                append_failure("Power limit %d%% was not accepted by the driver", desired->powerLimitPct);
            }
        }
    }

    bool fanChanged = false;
    if (!apply_fan_settings(desired, failureDetails, sizeof(failureDetails), successCount, failCount, result, resultSize, fanChanged)) {
        return false;
    }

    if (failCount > 0 && successCount > 0) {
        partialApplyRisk = true;
    }

    char detail[128] = {};
    if (memApplied || powerChanged || fanChanged) {
        refresh_global_state(detail, sizeof(detail));
    } else if (!curveTouched) {
        detect_clock_offsets();
    }

    // A post-apply global refresh may suppress live lock auto-detection (for example
    // on selective GPU offset profiles) and clear the GUI lock markers even though
    // this apply explicitly requested a lock. Restore the requested lock state so
    // the VF controls continue to match the just-applied profile.
    if (hasLock) {
        unsigned int displayedLockMHz = lockMhz;
        if (lockCi >= 0 && lockCi < VF_NUM_POINTS && g_app.curve[lockCi].freq_kHz > 0) {
            displayedLockMHz = displayed_curve_mhz(g_app.curve[lockCi].freq_kHz);
        }
        g_app.lockedVi = lockVi;
        g_app.lockedCi = lockCi;
        g_app.lockedFreq = displayedLockMHz;
        g_app.guiLockTracksAnchor = desired->lockTracksAnchor;
        debug_log("post-apply lock restore: ci=%d requested=%u displayed=%u trackAnchor=%d\n",
            lockCi,
            lockMhz,
            displayedLockMHz,
            desired->lockTracksAnchor ? 1 : 0);
    }

    capture_last_operation_snapshot(g_lastOperationAfterSnapshot, sizeof(g_lastOperationAfterSnapshot));
    populate_global_controls();
    if (interactive) {
        populate_edits();
        invalidate_main_window();
    }

    if (successCount == 0 && failCount == 0) {
        set_message(result, resultSize, "No setting changes needed.");
    } else if (failCount == 0) {
        set_message(result, resultSize, "Applied %d setting changes successfully.", successCount);
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

// ============================================================================
// NvAPI Interface
// ============================================================================

static void* nvapi_qi(unsigned int id) {
    typedef void* (*qi_func)(unsigned int);
    static qi_func qi = nullptr;
    if (!qi) {
        g_app.hNvApi = LoadLibraryA("nvapi64.dll");
        if (!g_app.hNvApi) {
            g_app.hNvApi = LoadLibraryA("nvapi.dll");
        }
        if (!g_app.hNvApi) return nullptr;
        qi = (qi_func)GetProcAddress(g_app.hNvApi, "nvapi_QueryInterface");
        if (!qi) return nullptr;
    }
    return qi(id);
}

static bool nvapi_init() {
    typedef int (*init_t)();
    auto init = (init_t)nvapi_qi(NVAPI_INIT_ID);
    if (!init) return false;
    return init() == 0;
}

static bool nvapi_enum_gpu() {
    typedef int (*enum_t)(GPU_HANDLE*, int*);
    auto enumGpu = (enum_t)nvapi_qi(NVAPI_ENUM_GPU_ID);
    if (!enumGpu) return false;
    int count = 0;
    GPU_HANDLE handles[64] = {};
    int ret = enumGpu(handles, &count);
    if (ret != 0 || count < 1) return false;
    g_app.gpuHandle = handles[0];
    return true;
}

static bool nvapi_get_name() {
    typedef int (*name_t)(GPU_HANDLE, char*);
    auto getName = (name_t)nvapi_qi(NVAPI_GET_NAME_ID);
    if (!getName) return false;
    return getName(g_app.gpuHandle, g_app.gpuName) == 0;
}

static bool nvapi_read_curve() {
    const VfBackendSpec* backend = g_app.vfBackend;
    if (!backend || !backend->readSupported) return false;

    auto getStatus = (NvApiFunc)nvapi_qi(backend->getStatusId);
    if (!getStatus) return false;

    unsigned char mask[32] = {};
    unsigned int numClocks = backend->defaultNumClocks;
    if (!nvapi_get_vf_info_cached(mask, &numClocks)) return false;

    unsigned char buf[0x4000] = {};
    if (backend->statusBufferSize > sizeof(buf)) return false;
    {
        const unsigned int version = (backend->statusVersion << 16) | backend->statusBufferSize;
        memcpy(&buf[0], &version, sizeof(version));
    }
    if (backend->statusMaskOffset + sizeof(mask) > backend->statusBufferSize) return false;
    memcpy(&buf[backend->statusMaskOffset], mask, sizeof(mask));
    if (backend->statusNumClocksOffset + sizeof(numClocks) <= backend->statusBufferSize) {
        memcpy(&buf[backend->statusNumClocksOffset], &numClocks, sizeof(numClocks));
    }

    int ret = getStatus(g_app.gpuHandle, buf);
    if (ret != 0) return false;

    g_app.numPopulated = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        unsigned int freq = 0, volt = 0;
        unsigned int entryOffset = backend->statusEntriesOffset + (unsigned int)i * backend->statusEntryStride;
        if (entryOffset + 8 > backend->statusBufferSize) {
            g_app.curve[i].freq_kHz = 0;
            g_app.curve[i].volt_uV = 0;
            continue;
        }
        memcpy(&freq, &buf[entryOffset], sizeof(freq));
        memcpy(&volt, &buf[entryOffset + 4], sizeof(volt));
        g_app.curve[i].freq_kHz = freq;
        g_app.curve[i].volt_uV = volt;
        if (freq > 0) g_app.numPopulated++;
    }
    g_app.loaded = true;
    return true;
}

static void read_nvidia_smi_max_clocks() {
    // Read nvidia-smi VBIOS default max clocks once, cache in AppData
    if (g_app.smiClocksRead) return;
    g_app.smiClocksRead = true;
    
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);
    
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    
    PROCESS_INFORMATION pi = {};
    WCHAR cmd[] = L"nvidia-smi -q -d CLOCK";
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hWrite);
        char buf[4096] = {};
        DWORD totalRead = 0;
        while (totalRead < sizeof(buf) - 1) {
            DWORD n = 0;
            if (!ReadFile(hRead, buf + totalRead, sizeof(buf) - 1 - totalRead, &n, nullptr) || n == 0) break;
            totalRead += n;
        }
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        
        bool inMaxSection = false;
        char* line = buf;
        while (line && *line) {
            char* nextLine = strchr(line, '\n');
            if (nextLine) { *nextLine = 0; nextLine++; }
            char* cr = strchr(line, '\r');
            if (cr) *cr = 0;
            while (*line == ' ' || *line == '\t') line++;
            
            if (strstr(line, "Max Clocks")) { inMaxSection = true; line = nextLine; continue; }
            if (inMaxSection && line[0] == '[') inMaxSection = false;
            
            if (inMaxSection) {
                char* vp = nullptr;
                if ((vp = strstr(line, "Memory")) && (vp = strchr(vp, ':')))
                    g_app.smiMemMaxMHz = (unsigned int)atoi(vp + 1);
            }
            line = nextLine;
        }
    } else {
        CloseHandle(hWrite);
    }
    CloseHandle(hRead);
}

static int uniform_curve_offset_khz() {
    if (!vf_curve_global_gpu_offset_supported()) return 0;

    int values[VF_NUM_POINTS] = {};
    int counts[VF_NUM_POINTS] = {};
    int uniqueCount = 0;
    int populatedCount = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (g_app.curve[i].freq_kHz == 0) continue;
        populatedCount++;
        int delta = g_app.freqOffsets[i];
        bool found = false;
        for (int j = 0; j < uniqueCount; j++) {
            if (values[j] == delta) {
                counts[j]++;
                found = true;
                break;
            }
        }
        if (!found && uniqueCount < VF_NUM_POINTS) {
            values[uniqueCount] = delta;
            counts[uniqueCount] = 1;
            uniqueCount++;
        }
    }
    if (populatedCount == 0) return 0;

    int bestValue = 0;
    int bestCount = 0;
    for (int i = 0; i < uniqueCount; i++) {
        if (counts[i] > bestCount || (counts[i] == bestCount && abs(values[i]) > abs(bestValue))) {
            bestValue = values[i];
            bestCount = counts[i];
        }
    }

    if (bestCount * 2 < populatedCount) return 0;
    return bestValue;
}

static void detect_clock_offsets() {
    // Detect global offsets from generic driver-visible sources.
    // GPU: only use uniform VF control deltas on write-supported backends where
    // the control-table layout is validated. Probe-only/read-only backends may
    // expose non-user-visible baseline deltas that do not map to a global offset.
    // Memory: prefer public Pstates20 memory clocks vs VBIOS max clocks.
    // This reflects the currently active offset and avoids stale NVML/Pstates delta fields
    // surviving after another tool resets memory to default.

    read_nvidia_smi_max_clocks();

    int gpuOffsetkHz = uniform_curve_offset_khz();
    if (gpuOffsetkHz != 0 || g_app.pstateGpuOffsetkHz != 0) {
        if (gpuOffsetkHz == 0) gpuOffsetkHz = g_app.pstateGpuOffsetkHz;
        if (!g_app.gpuOffsetRangeKnown) {
            int gpuOffsetMHz = gpuOffsetkHz / 1000;
            g_app.gpuClockOffsetMinMHz = gpuOffsetMHz;
            g_app.gpuClockOffsetMaxMHz = gpuOffsetMHz;
        }
    }
    g_app.gpuClockOffsetkHz = gpuOffsetkHz;

    if (g_app.pstateMemMaxMHz > 0 && g_app.smiMemMaxMHz > 0) {
        int memOffsetkHz = ((int)g_app.pstateMemMaxMHz - (int)g_app.smiMemMaxMHz) * 1000;
        g_app.memClockOffsetkHz = memOffsetkHz;
        if (!g_app.memOffsetRangeKnown && memOffsetkHz != 0) {
            int memOffsetMHz = mem_display_mhz_from_driver_khz(memOffsetkHz);
            g_app.memClockOffsetMinMHz = memOffsetMHz;
            g_app.memClockOffsetMaxMHz = memOffsetMHz;
        }
    }
}

static bool nvapi_read_offsets() {
    const VfBackendSpec* backend = g_app.vfBackend;
    if (!backend || !backend->readSupported) return false;

    unsigned char buf[0x4000] = {};
    if (backend->controlBufferSize > sizeof(buf)) return false;
    if (!nvapi_read_control_table(buf, sizeof(buf))) return false;

    for (int i = 0; i < VF_NUM_POINTS; i++) {
        int delta = 0;
        unsigned int deltaOffset = backend->controlEntryBaseOffset + (unsigned int)i * backend->controlEntryStride + backend->controlEntryDeltaOffset;
        if (deltaOffset + sizeof(delta) > backend->controlBufferSize) {
            g_app.freqOffsets[i] = 0;
            continue;
        }
        memcpy(&delta, &buf[deltaOffset], sizeof(delta));
        g_app.freqOffsets[i] = delta;
    }
    return true;
}

static bool nvapi_set_point(int pointIndex, int freqDelta_kHz) {
    if (pointIndex < 0 || pointIndex >= VF_NUM_POINTS) return false;
    freqDelta_kHz = clamp_freq_delta_khz(freqDelta_kHz);

    const VfBackendSpec* backend = g_app.vfBackend;
    if (!backend || !backend->writeSupported) return false;

    auto func = (NvApiFunc)nvapi_qi(backend->setControlId);
    if (!func) return false;

    unsigned char buf[0x4000] = {};
    if (backend->controlBufferSize > sizeof(buf)) return false;
    if (!nvapi_read_control_table(buf, sizeof(buf))) return false;

    memset(&buf[backend->controlMaskOffset], 0, 32);
    buf[backend->controlMaskOffset + pointIndex / 8] = (unsigned char)(1 << (pointIndex % 8));
    unsigned int deltaOffset = backend->controlEntryBaseOffset + (unsigned int)pointIndex * backend->controlEntryStride + backend->controlEntryDeltaOffset;
    if (deltaOffset + sizeof(freqDelta_kHz) > backend->controlBufferSize) return false;
    memcpy(&buf[deltaOffset], &freqDelta_kHz, sizeof(freqDelta_kHz));

    int ret = func(g_app.gpuHandle, buf);
    debug_log("set_point idx=%d delta=%d ret=%d\n", pointIndex, freqDelta_kHz, ret);
    return ret == 0;
}

// Pstates20 struct size and version for Blackwell
// NVML-based OC/PL functions
static bool nvml_read_power_limit() {
    if (!nvml_ensure_ready()) return false;
    if (!g_nvml_api.getPowerLimit || !g_nvml_api.getPowerDefaultLimit) return false;

    unsigned int cur = 0, def = 0;
    if (g_nvml_api.getPowerLimit(g_app.nvmlDevice, &cur) != NVML_SUCCESS) return false;
    if (g_nvml_api.getPowerDefaultLimit(g_app.nvmlDevice, &def) != NVML_SUCCESS) def = cur;

    g_app.powerLimitCurrentmW = (int)cur;
    g_app.powerLimitDefaultmW = def > 0 ? (int)def : (int)cur;
    g_app.powerLimitMinmW = 0;
    g_app.powerLimitMaxmW = 0;

    if (g_nvml_api.getPowerConstraints) {
        unsigned int mn = 0, mx = 0;
        if (g_nvml_api.getPowerConstraints(g_app.nvmlDevice, &mn, &mx) == NVML_SUCCESS) {
            g_app.powerLimitMinmW = (int)mn;
            g_app.powerLimitMaxmW = (int)mx;
        }
    }

    if (g_app.powerLimitDefaultmW > 0)
        g_app.powerLimitPct = (g_app.powerLimitCurrentmW * 100 + g_app.powerLimitDefaultmW / 2) / g_app.powerLimitDefaultmW;
    else
        g_app.powerLimitPct = 100;

    if (g_app.powerLimitPct < 0) g_app.powerLimitPct = 0;

    return true;
}

static bool nvapi_read_pstates() {
    // Read clock data from public NvAPI Pstates20.
    auto func = (NvApiFunc)nvapi_qi(0x6FF81213u);
    g_app.pstateGpuOffsetkHz = 0;
    g_app.pstateMemMaxMHz = 0;
    if (!func) return false;

    nvapiPerfPstates20Info_t info = {};
    info.version = NVAPI_PERF_PSTATES20_INFO_VER3;
    int ret = func(g_app.gpuHandle, &info);
    if (ret != 0) {
        info = {};
        info.version = NVAPI_PERF_PSTATES20_INFO_VER2;
        ret = func(g_app.gpuHandle, &info);
    }
    if (ret != 0) return false;

    unsigned int numPstates = info.numPstates;
    if (numPstates > NVAPI_MAX_GPU_PSTATE20_PSTATES) numPstates = NVAPI_MAX_GPU_PSTATE20_PSTATES;
    unsigned int numClocks = info.numClocks;
    if (numClocks > NVAPI_MAX_GPU_PSTATE20_CLOCKS) numClocks = NVAPI_MAX_GPU_PSTATE20_CLOCKS;
    bool curveRangeAnyFound = false;
    bool curveRangeP0Found = false;
    int curveRangeAnyMinkHz = 0;
    int curveRangeAnyMaxkHz = 0;
    int curveRangeP0MinkHz = 0;
    int curveRangeP0MaxkHz = 0;

    auto update_curve_range = [](bool* found, int* minOut, int* maxOut, int minValue, int maxValue) {
        if (!found || !minOut || !maxOut) return;
        if (!*found) {
            *minOut = minValue;
            *maxOut = maxValue;
            *found = true;
            return;
        }
        if (minValue < *minOut) *minOut = minValue;
        if (maxValue > *maxOut) *maxOut = maxValue;
    };

    for (unsigned int pi = 0; pi < numPstates; pi++) {
        const nvapiPstate20Entry_t* pstate = &info.pstates[pi];
        for (unsigned int ci = 0; ci < numClocks; ci++) {
            const nvapiPstate20ClockEntry_t* clock = &pstate->clocks[ci];
            unsigned int maxFreq_kHz = 0;
            if (clock->typeId == NVAPI_GPU_PERF_PSTATE20_CLOCK_TYPE_SINGLE) {
                maxFreq_kHz = clock->data.single.freq_kHz;
            } else if (clock->typeId == NVAPI_GPU_PERF_PSTATE20_CLOCK_TYPE_RANGE) {
                maxFreq_kHz = clock->data.range.maxFreq_kHz;
            }

            if (clock->domainId == NVAPI_GPU_PUBLIC_CLOCK_GRAPHICS) {
                if (abs(clock->freqDelta_kHz.value) > abs(g_app.pstateGpuOffsetkHz)) {
                    g_app.pstateGpuOffsetkHz = clock->freqDelta_kHz.value;
                }
                if (clock->bIsEditable) {
                    if (pstate->pstateId == NVML_PSTATE_0) {
                        update_curve_range(&curveRangeP0Found, &curveRangeP0MinkHz, &curveRangeP0MaxkHz,
                            clock->freqDelta_kHz.valueRange.min, clock->freqDelta_kHz.valueRange.max);
                    } else {
                        update_curve_range(&curveRangeAnyFound, &curveRangeAnyMinkHz, &curveRangeAnyMaxkHz,
                            clock->freqDelta_kHz.valueRange.min, clock->freqDelta_kHz.valueRange.max);
                    }
                }
            } else if (clock->domainId == NVAPI_GPU_PUBLIC_CLOCK_MEMORY) {
                unsigned int mhz = maxFreq_kHz / 1000;
                if (mhz > g_app.pstateMemMaxMHz) g_app.pstateMemMaxMHz = mhz;
            }
        }
    }

    if (curveRangeP0Found) {
        set_curve_offset_range_khz(curveRangeP0MinkHz, curveRangeP0MaxkHz);
    } else if (curveRangeAnyFound) {
        set_curve_offset_range_khz(curveRangeAnyMinkHz, curveRangeAnyMaxkHz);
    }

    return true;
}

static bool nvapi_set_gpu_offset(int offsetkHz) {
    if (!vf_curve_global_gpu_offset_supported()) {
        if (g_app.gpuClockOffsetkHz == offsetkHz) return true;

        bool exact = false;
        char detail[128] = {};
        bool ok = nvml_set_clock_offset_domain(NVML_CLOCK_GRAPHICS, offsetkHz / 1000, &exact, detail, sizeof(detail));
        if (!ok) return false;

        nvml_read_clock_offsets(detail, sizeof(detail));
        nvapi_read_pstates();
        detect_clock_offsets();
        nvapi_read_offsets();
        if (nvapi_read_curve()) rebuild_visible_map();
        return exact || g_app.gpuClockOffsetkHz == offsetkHz;
    }

    int currentGlobalkHz = uniform_curve_offset_khz();
    if (currentGlobalkHz == offsetkHz) return true;

    debug_log("set_gpu_offset current=%d target=%d\n", currentGlobalkHz, offsetkHz);

    int targetOffsets[VF_NUM_POINTS] = {};
    bool pointMask[VF_NUM_POINTS] = {};
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (g_app.curve[i].freq_kHz == 0) continue;
        targetOffsets[i] = clamp_freq_delta_khz(g_app.freqOffsets[i] - currentGlobalkHz + offsetkHz);
        pointMask[i] = true;
    }
    bool exactOk = apply_curve_offsets_verified(targetOffsets, pointMask, 2);
    int uniformkHz = uniform_curve_offset_khz();
    detect_clock_offsets();
    bool functionalOk = (uniformkHz == offsetkHz) || (g_app.gpuClockOffsetkHz == offsetkHz);
    debug_log("set_gpu_offset result exact=%d uniform=%d detected=%d\n",
        exactOk ? 1 : 0, uniformkHz, g_app.gpuClockOffsetkHz);
    return exactOk || functionalOk;
}

static bool nvapi_set_mem_offset(int offsetkHz) {
    if (g_app.memClockOffsetkHz == offsetkHz) return true;
    int nvmlValueMHz = (offsetkHz / 1000) * 2;
    debug_log("nvapi_set_mem_offset: target_driver_kHz=%d nvml_value_MHz=%d current_driver_kHz=%d\n",
        offsetkHz, nvmlValueMHz, g_app.memClockOffsetkHz);
    bool exact = false;
    char detail[128] = {};
    bool ok = nvml_set_clock_offset_domain(NVML_CLOCK_MEM, nvmlValueMHz, &exact, detail, sizeof(detail));
    if (!ok) {
        debug_log("nvapi_set_mem_offset: NVML rejected offset (nvml=%d MHz) detail=%s\n",
            nvmlValueMHz, detail[0] ? detail : "unknown");
        return false;
    }

    nvml_read_clock_offsets(detail, sizeof(detail));
    nvapi_read_pstates();
    detect_clock_offsets();
    bool verified = (g_app.memClockOffsetkHz == offsetkHz);
    debug_log("nvapi_set_mem_offset: apply ok, readback_driver_kHz=%d verified=%d\n",
        g_app.memClockOffsetkHz, verified ? 1 : 0);
    return verified;
}

static bool nvapi_set_power_limit(int pct) {
    if (pct < 50 || pct > 150) return false;
    if (g_app.powerLimitDefaultmW <= 0) return false;

    int watts = (g_app.powerLimitDefaultmW * pct + 50000) / 100000;
    if (watts < 1) return false;
    unsigned int targetmW = (unsigned int)watts * 1000u;

    if (g_app.powerLimitMinmW > 0 && targetmW < (unsigned int)g_app.powerLimitMinmW) return false;
    if (g_app.powerLimitMaxmW > 0 && targetmW > (unsigned int)g_app.powerLimitMaxmW) return false;

    if (nvml_ensure_ready() && g_nvml_api.setPowerLimit) {
        nvmlReturn_t r = g_nvml_api.setPowerLimit(g_app.nvmlDevice, targetmW);
        if (r == NVML_SUCCESS) {
            nvml_read_power_limit();
            return true;
        }
        debug_log("Power limit via NVML failed: %s\n", nvml_err_name(r));
    }

    char exePath[MAX_PATH] = {};
    DWORD pathLen = SearchPathA(nullptr, "nvidia-smi.exe", nullptr, ARRAY_COUNT(exePath), exePath, nullptr);
    if (pathLen == 0 || pathLen >= ARRAY_COUNT(exePath)) {
        StringCchCopyA(exePath, ARRAY_COUNT(exePath), "nvidia-smi.exe");
    }

    char cmdLine[256] = {};
    StringCchPrintfA(cmdLine, ARRAY_COUNT(cmdLine), "\"%s\" -pl %d", exePath, watts);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessA(nullptr, cmdLine, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        debug_log("Power limit via nvidia-smi failed to launch (error %lu)\n", GetLastError());
        return false;
    }

    WaitForSingleObject(pi.hProcess, 5000);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode == 0) {
        nvml_read_power_limit();
    }
    else debug_log("Power limit via nvidia-smi failed with exit code %lu\n", exitCode);
    return exitCode == 0;
}

static void rebuild_visible_map() {
    g_app.numVisible = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        // Keep the editable VF grid stable across applied offsets and curve edits.
        // Visibility should follow the baseline point position, not the current live target.
        unsigned int freq_mhz = (unsigned int)(curve_base_khz_for_point(i) / 1000);
        unsigned int volt_mv = g_app.curve[i].volt_uV / 1000;
        if (volt_mv >= MIN_VISIBLE_VOLT_mV && freq_mhz >= MIN_VISIBLE_FREQ_MHz) {
            g_app.visibleMap[g_app.numVisible++] = i;
        }
    }
}

static bool restore_locked_tail_from_curve_index_exact(int preferredCi) {
    if (preferredCi < 0 || preferredCi >= VF_NUM_POINTS) return false;
    if (g_app.numVisible < 2) return false;
    if (g_app.curve[preferredCi].freq_kHz == 0) return false;

    int preferredVi = -1;
    for (int vi = 0; vi < g_app.numVisible; vi++) {
        if (g_app.visibleMap[vi] == preferredCi) {
            preferredVi = vi;
            break;
        }
    }
    if (preferredVi < 0 || preferredVi >= g_app.numVisible - 1) return false;

    unsigned int lockFreqkHz = g_app.curve[preferredCi].freq_kHz;
    bool hasTail = false;
    for (int j = preferredVi + 1; j < g_app.numVisible; j++) {
        int cj = g_app.visibleMap[j];
        if (g_app.curve[cj].freq_kHz == 0) return false;
        hasTail = true;
        if (g_app.curve[cj].freq_kHz != lockFreqkHz) return false;
    }
    if (!hasTail) return false;

    g_app.lockedVi = preferredVi;
    g_app.lockedCi = preferredCi;
    g_app.lockedFreq = displayed_curve_mhz(lockFreqkHz);
    g_app.guiLockTracksAnchor = true;
    return true;
}

static bool restore_locked_tail_from_curve_index_tolerant(int preferredCi, int minTailPoints) {
    if (preferredCi < 0 || preferredCi >= VF_NUM_POINTS) return false;
    if (g_app.numVisible < 2) return false;
    if (g_app.curve[preferredCi].freq_kHz == 0) return false;

    int preferredVi = -1;
    for (int vi = 0; vi < g_app.numVisible; vi++) {
        if (g_app.visibleMap[vi] == preferredCi) {
            preferredVi = vi;
            break;
        }
    }
    if (preferredVi < 0 || preferredVi >= g_app.numVisible - 1) return false;

    static const int LOCK_TAIL_TOLERANCE_MHZ = 1;
    unsigned int anchorMHz = displayed_curve_mhz(g_app.curve[preferredCi].freq_kHz);
    unsigned int summedMHz = anchorMHz;
    int pointCount = 1;
    int tailPoints = 0;

    for (int j = preferredVi + 1; j < g_app.numVisible; j++) {
        int cj = g_app.visibleMap[j];
        if (g_app.curve[cj].freq_kHz == 0) return false;

        unsigned int pointMHz = displayed_curve_mhz(g_app.curve[cj].freq_kHz);
        if (abs((int)pointMHz - (int)anchorMHz) > LOCK_TAIL_TOLERANCE_MHZ) return false;

        summedMHz += pointMHz;
        pointCount++;
        tailPoints++;
    }

    if (tailPoints < minTailPoints) return false;

    g_app.lockedVi = preferredVi;
    g_app.lockedCi = preferredCi;
    g_app.lockedFreq = (summedMHz + (unsigned int)(pointCount / 2)) / (unsigned int)pointCount;
    g_app.guiLockTracksAnchor = true;
    return true;
}

static void detect_locked_tail_from_curve() {
    int preferredCi = (g_app.lockedFreq > 0 && g_app.lockedCi >= 0 && g_app.lockedCi < VF_NUM_POINTS)
        ? g_app.lockedCi
        : -1;

    g_app.lockedVi = -1;
    g_app.lockedCi = -1;
    g_app.lockedFreq = 0;
    g_app.guiLockTracksAnchor = true;

    if (g_app.numVisible < 2) return;
    if (!should_auto_detect_locked_tail_from_live_curve()) return;
    if (preferredCi >= 0) {
        if (restore_locked_tail_from_curve_index_exact(preferredCi)) return;
        if (restore_locked_tail_from_curve_index_tolerant(preferredCi, 1)) return;
    }

    for (int vi = 0; vi < g_app.numVisible - 1; vi++) {
        int ci = g_app.visibleMap[vi];
        unsigned int lockFreqkHz = g_app.curve[ci].freq_kHz;
        if (lockFreqkHz == 0) continue;

        bool hasTail = false;
        bool allSame = true;
        for (int j = vi + 1; j < g_app.numVisible; j++) {
            int cj = g_app.visibleMap[j];
            unsigned int freqkHz = g_app.curve[cj].freq_kHz;
            if (freqkHz == 0) {
                allSame = false;
                break;
            }
            hasTail = true;
            if (freqkHz != lockFreqkHz) {
                allSame = false;
                break;
            }
        }

        if (hasTail && allSame) {
            g_app.lockedVi = vi;
            g_app.lockedCi = ci;
            g_app.lockedFreq = displayed_curve_mhz(lockFreqkHz);
            return;
        }
    }

    // Some drivers report a flattened tail with tiny per-point kHz drift even though
    // the visible curve is effectively locked. Accept a long suffix that stays within
    // 1 MHz of the anchor so startup detection does not jump to a later voltage point.
    for (int vi = 0; vi < g_app.numVisible - 1; vi++) {
        int ci = g_app.visibleMap[vi];
        if (restore_locked_tail_from_curve_index_tolerant(ci, 3)) {
            return;
        }
    }
}

static bool read_live_curve_snapshot_settled(int attempts, DWORD delayMs, bool* lastOffsetsOkOut) {
    if (!g_app.isServiceProcess && g_app.usingBackgroundService) {
        char err[256] = {};
        ServiceSnapshot snapshot = {};
        if (!service_client_get_snapshot(&snapshot, err, sizeof(err))) {
            debug_log("service snapshot failed: %s\n", err);
            if (lastOffsetsOkOut) *lastOffsetsOkOut = false;
            return false;
        }
        apply_service_snapshot_to_app(&snapshot);
        DesiredSettings activeDesired = {};
        if (service_client_get_active_desired(&activeDesired, nullptr, err, sizeof(err))) {
            apply_service_desired_to_gui(&activeDesired);
        }
        if (lastOffsetsOkOut) *lastOffsetsOkOut = true;
        return snapshot.loaded;
    }

    if (lastOffsetsOkOut) *lastOffsetsOkOut = false;
    if (attempts < 1) attempts = 1;

    bool anyCurveOk = false;
    bool bestValid = false;
    bool bestOffsetsOk = false;
    int bestNumVisible = -1;
    int bestNumPopulated = -1;
    VFCurvePoint bestCurve[VF_NUM_POINTS] = {};
    int bestFreqOffsets[VF_NUM_POINTS] = {};
    for (int attempt = 0; attempt < attempts; attempt++) {
        if (attempt > 0 && delayMs > 0) Sleep(delayMs);

        bool curveOk = nvapi_read_curve();
        bool offsetsOk = nvapi_read_offsets();
        if (!curveOk) continue;

        anyCurveOk = true;
        rebuild_visible_map();
        detect_locked_tail_from_curve();

        bool betterSnapshot = !bestValid
            || g_app.numVisible > bestNumVisible
            || (g_app.numVisible == bestNumVisible && g_app.numPopulated > bestNumPopulated)
            || (g_app.numVisible == bestNumVisible && g_app.numPopulated == bestNumPopulated && offsetsOk && !bestOffsetsOk);
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
        return anyCurveOk;
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

    if (lastOffsetsOkOut) *lastOffsetsOkOut = bestOffsetsOk;
    return true;
}

