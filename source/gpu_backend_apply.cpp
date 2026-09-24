// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "service_apply_severity_policy.h"
#include "gpu_backend_reset_baseline.cpp"

#include "gpu_backend_apply_ceiling.h"
#include "gpu_backend_apply_failure.h"
// What the VF table actually looks like after the batch, for post-mortems.
#include "gpu_backend_apply_diagnostics.h"
// Intent plus a fresh live curve -> the per-point control offsets to write.
#include "gpu_backend_apply_targets.h"
#include "gpu_backend_apply_verify.h"
// The XBAR/SYS/VIDEO half of an apply, after the core clock transaction.
#include "gpu_backend_apply_advanced.h"
// How long VF correction may run inside the apply's own time budget.
#include "apply_correction_budget_policy.h"

static bool apply_desired_settings_service(const DesiredSettings* desired,
    bool interactive, char* result, size_t resultSize,
    bool* hardwareWriteAttemptedOut, gc_u32* outcomeSeverityOut) {
    if (hardwareWriteAttemptedOut) *hardwareWriteAttemptedOut = false;
    // Any early return below is a refusal or a failure, so ERROR is the correct
    // starting value: a caller that forgets to look at the bool still cannot
    // read a failed apply as a clean one.  Only the tail lowers it.
    if (outcomeSeverityOut)
        *outcomeSeverityOut = (gc_u32)SERVICE_OUTCOME_SEVERITY_ERROR;
    if (!desired) {
        set_message(result, resultSize, "No desired settings");
        return false;
    }
    // Every VF write below measures itself against this, so the apply cannot
    // outrun the budget its client deadline is derived from.
    const ULONGLONG applyStartTickMs = GetTickCount64();
    const ULONGLONG correctionDeadlineTickMs =
        applyStartTickMs + apply_correction_budget_ms();
    if (!validate_desired_fan_settings_for_apply(desired, result, resultSize)) {
        debug_log("apply_desired_settings: fan prevalidation failed: %s\n", result && result[0] ? result : "unknown");
        return false;
    }
    // CT-02, preflight half.  A request that EXPLICITLY names a lock point
    // this GPU's visible map cannot resolve used to have `hasLock` quietly
    // cleared halfway down this function -- after reset-to-stock had already
    // run -- and the rest of the profile was then applied as an unlocked one.
    // The user asked for a pinned or flattened profile and got an unpinned
    // overclock, reported as success.  Resolving it here, before anything is
    // written, turns that into an untouched refusal.
    //
    // Only an EXPLICIT request is validated: `hasLock` can also be inherited
    // from the stored interactive lock further down, and dropping an inherited
    // lock the current GPU cannot express is not a failure of this request.
    // The visible map is topology, not live state, so it is as valid here as
    // it is after the reset.
    if (desired->hasLock && desired->lockCi >= 0 && desired->lockMHz > 0) {
        bool lockPointVisible = false;
        for (int vi = 0; vi < g_app.numVisible; vi++) {
            if (g_app.visibleMap[vi] == desired->lockCi) { lockPointVisible = true; break; }
        }
        if (!lockPointVisible) {
            set_message(result, resultSize,
                "The requested %s lock point (curve index %d) is not available on this"
                " GPU, so the profile cannot be applied as requested. Nothing was"
                " changed. Re-pick the lock point on the curve.",
                lock_mode_name(desired->lockMode), desired->lockCi);
            debug_log("apply_desired_settings: REFUSED before any write -- requested"
                      " lock ci=%d mode=%s lockMHz=%u is not in the visible map"
                      " (numVisible=%d); refusing instead of silently applying an"
                      " unlocked profile\n",
                desired->lockCi, lock_mode_name(desired->lockMode),
                desired->lockMHz, g_app.numVisible);
            return false;
        }
    }
#ifdef GREEN_CURVE_SERVICE_BINARY
    bool proofInvalidatedForWrite = false;
#endif
    // F-APPLY-CEILING.  Declared before the first hardware write and destroyed
    // on every exit; armed only once the OC stability proof has been
    // invalidated, because arming is itself a hardware write.
    if (desired->hasLock && desired->lockMode == LOCK_MODE_FLATTEN &&
        !vf_offset_range_supports_flatten(vf_offset_range_current())) {
        set_message(result, resultSize, "This driver's VF offset range cannot flatten the curve");
        return false;
    }
    ApplyClockCeilingGuard clockCeiling(desired);
    // Records the measured clock and the live load across the whole apply, and
    // ends with the one line the next under-load test needs: did the clamp hold.
    // Destroyed after the guard is declared, so the verdict is emitted before
    // the guard's own release/retain line -- the clamp is still whatever the
    // apply left it as while the verdict is written.
    ApplyClockWitnessScope clockWitness(clockCeiling.plan.ceilingMHz, false);
    apply_clock_witness_record("apply entry");
    bool powerTargetWrittenByReset = false;
    if (desired->resetOcBeforeApply) {
#ifdef GREEN_CURVE_SERVICE_BINARY
        if (!service_invalidate_oc_apply_proof_before_write()) {
            set_message(result, resultSize,
                "Could not invalidate the previous stability proof; no hardware write was attempted");
            return false;
        }
        // Same boundary: a crash after this write must be handed back.
        if (!service_ownership_marker_ensure_before_write()) {
            set_message(result, resultSize,
                "Could not record GPU ownership before the write; no hardware write was attempted");
            return false;
        }
        proofInvalidatedForWrite = true;
#endif
        if (hardwareWriteAttemptedOut) *hardwareWriteAttemptedOut = true;
        // BEFORE the reset drops whatever was capping the clocks (a flatten
        // tail floor, an old pin) and before the settle runs the stock curve.
        clockCeiling.arm();
        // CT-01.  A required clamp that could not be installed stops the apply
        // HERE, ahead of reset_oc_before_gui_apply() -- which is the write that
        // removes the outgoing cap and hands the GPU a stock curve.  The
        // pre-fix code logged the refusal and then ran that reset anyway.
        if (clockCeiling.must_refuse_transition()) {
            clockCeiling.refusal_message(result, resultSize);
            debug_log("apply_desired_settings: REFUSED before reset-to-stock --"
                      " required transition clamp at %u MHz not installed"
                      " (armResult=%d)\n",
                clockCeiling.plan.ceilingMHz, (int)clockCeiling.armResult);
            set_last_apply_phase("apply: refused (transition clamp unavailable)");
            return false;
        }
        if (!reset_oc_before_gui_apply(desired, result, resultSize,
                                       &powerTargetWrittenByReset))
            return apply_recover_clock_failure(clockCeiling, result, result, resultSize);
        // TDR settle after reset-to-stock: let VRM/memory controllers stabilize
        // before the new aggressive clocks (commit 4b225e1). The 1s default is
        // deliberately conservative; make it tunable so rapid profile-switching can
        // trade some of it back after GPU verification. Default preserves the exact
        // current behaviour; clamp to a sane ceiling. Lowering it is a TDR/speed
        // trade-off that MUST be validated on the real GPU under game load.
        int settleMs = g_app.configPath[0]
            ? get_config_int(g_app.configPath, "apply", "reset_settle_ms", 1000)
            : 1000;
        if (settleMs < 0) settleMs = 0;
        if (settleMs > 5000) settleMs = 5000;
        if (settleMs > 0) Sleep((DWORD)settleMs);
        debug_log("apply_desired_settings: %dms settle at stock baseline complete\n", settleMs);
        apply_clock_witness_record("post-reset settle");
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
    bool allowInteractiveStoredLock = interactive && !app_is_service_process();
    bool hasLock = requestHasLock
        || (allowInteractiveStoredLock && g_app.lockedVi >= 0 && g_app.lockedVi < g_app.numVisible);
    LockMode lockMode = LOCK_MODE_NONE;
    if (hasLock) {
        if (desired->hasLock && desired->lockMode != LOCK_MODE_NONE) {
            lockMode = desired->lockMode;
        } else if (g_app.lockMode != LOCK_MODE_NONE) {
            lockMode = g_app.lockMode;
        } else {
            lockMode = LOCK_MODE_FLATTEN;
        }
    }
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
            // EXPLICIT means "the user typed this absolute MHz", which is what
            // earns a point the right to fail an apply on its absolute readback.
            // A point the request marks as offset-derived did not come from the
            // user in absolute form -- it is a stock base plus this request's own
            // offset component, and the base it was projected from is not the
            // base the driver reports now.  Those points keep offset authority
            // and are verified as offsets.  Per point: one request mixes both.
            explicitCurveMask[ci] = !desired->curvePointFromGpuOffset[ci];
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
            // CT-02.  Clearing `hasLock` alone left `lockMode` at FLATTEN or
            // HARD, and the release branch near the end of this function keys
            // off lockMode, not hasLock.  A FLATTEN whose anchor could not be
            // resolved therefore took the "release the locked-clock domain"
            // path and adopted the transition clamp as released -- over a
            // curve the selective offset had already raised -- while the
            // retain() fall-through that was supposed to catch exactly this
            // case became unreachable, because adopt() had already run.
            // The lock mode goes with the lock.
            debug_log("apply: requested lock point ci=%d is not in this GPU's"
                      " visible map; dropping the lock AND its mode (%s -> NONE)"
                      " so the release branch cannot claim a domain this apply"
                      " no longer owns\n",
                lockCi, lock_mode_name(lockMode));
            hasLock = false;
            lockMode = LOCK_MODE_NONE;
            lockMhz = 0;
        } else {
            if (lockMhz == 0) {
                lockMhz = desired->hasCurvePoint[lockCi] ? desired->curvePointMHz[lockCi] : displayed_curve_mhz(g_app.curve[lockCi].freq_kHz);
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
            // range by design (already absolute-bounded to +/-3000 MHz at the IPC
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
    // All validation that can reject the request without touching hardware is
    // complete.  From this point, any failing result is conservatively treated
    // as a real write attempt so automatic restoration is latched off.
#ifdef GREEN_CURVE_SERVICE_BINARY
    if (!proofInvalidatedForWrite &&
        !service_invalidate_oc_apply_proof_before_write()) {
        set_message(result, resultSize,
            "Could not invalidate the previous stability proof; no hardware write was attempted");
        return false;
    }
    if (!proofInvalidatedForWrite &&
        !service_ownership_marker_ensure_before_write()) {
        set_message(result, resultSize,
            "Could not record GPU ownership before the write; no hardware write was attempted");
        return false;
    }
#endif
    if (hardwareWriteAttemptedOut) *hardwareWriteAttemptedOut = true;
    // Second arm site: a request without reset-to-stock reaches its first
    // clock-affecting write here.  Idempotent when the reset path already armed.
    clockCeiling.arm();
    // Same CT-01 refusal as the reset path, for the request shapes that reach
    // their first clock write without a reset-to-stock.
    if (clockCeiling.must_refuse_transition()) {
        clockCeiling.refusal_message(result, resultSize);
        debug_log("apply_desired_settings: REFUSED before the first clock write --"
                  " required transition clamp at %u MHz not installed"
                  " (armResult=%d)\n",
            clockCeiling.plan.ceilingMHz, (int)clockCeiling.armResult);
        set_last_apply_phase("apply: refused (transition clamp unavailable)");
        return false;
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
            if (!read_live_curve_snapshot_settled(6, 25, &settledOffsetsOk) || !settledOffsetsOk) {
                return apply_recover_clock_failure(clockCeiling, "Fresh curve readback failed", result, resultSize);
            }
        } else {
            failCount++;
            partialApplyRisk = true;
            append_failure("GPU offset %d MHz was not accepted by the driver", desired->gpuOffsetMHz);
            return apply_recover_clock_failure(clockCeiling, failureDetails, result, resultSize);
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
            return apply_recover_clock_failure(clockCeiling,
                "Selective offset could not clear the previous offset", result, resultSize);
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
    if (!apply_build_curve_targets(desired, curveRequest, preserveCurveAcrossMem,
        hasLock, lockMode, lockCi, lockMhz, gpuPolicyViaCurveBatch,
        desiredActiveGpuOffsetExcludeLowCount, currentAppliedGpuOffsetMHz,
        currentActiveGpuOffsetExcludeLowCount, originalCurvePopulated,
        originalCurveOffsets, originalCurveFreqkHz, lockedTailMask,
        targetCurveOffsets, targetCurveMask))
        return apply_recover_clock_failure(clockCeiling,
            "A requested curve target is missing or outside the driver range", result, resultSize);
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
                return apply_recover_clock_failure(clockCeiling, failureDetails, result, resultSize);
            }
        } else {
            debug_log("apply mem offset: skipped (current already matches target %d kHz)\n", targetMemkHz);
        }
    }
    bool curveBatchOk = true;
    bool curveBatchNeeded = false;
    bool curveTouched = gpuApplied;
    // CT-02.  Transaction state, not batch-local state.  This used to be
    // declared inside the curve-write block, so it had gone out of scope by
    // the time the locked-clock release ran -- which is a large part of why
    // that release could not consult it and instead keyed off the lock mode
    // alone.  It starts true because a request that writes no curve has no
    // unverified curve to worry about; `curveTouched` is what distinguishes
    // the two cases at the release site.
    bool curveRequestOk = true;
    int selectiveOffsetApplied = 0;
    int selectiveOffsetFailed = 0;
    int flattenApplied = 0;
    int flattenFailed = 0;
    int userBoostApplied = 0;
    int userBoostFailed = 0;
    // CT-06.  Points the hardware put ABOVE what the request asked for.  They
    // are counted separately from selectiveOffsetFailed because the shortcut
    // in verify_curve_request() -- "most points matched, call it verified" --
    // is a legitimate tolerance for points that came up SHORT and must never
    // apply to points that came up OVER.




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
        debug_log("curve strategy: selective=%+d MHz%s boostRegionPoints=%d flattenTarget=%u MHz flattenRegionPoints=%d lockCi=%d lockMode=%s%s\n",
            gpuPolicyViaCurveBatch ? desired->gpuOffsetMHz : 0,
            desiredActiveGpuOffsetExcludeLowCount > 0 ? " excl<N" : "",
            boostPoints,
            (hasLock && lockMhz > 0) ? lockMhz : 0,
            flattenPoints,
            hasLock ? lockCi : -1,
            lock_mode_name(lockMode),
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
        // CT-03.  ONE range, from vf_offset_range_policy.h, shared with the
        // FLATTEN tail floor above and the write clamp below.  This site used
        // to carry its own 500,000 kHz fallback while the floor used
        // -1,000,000 kHz, so on any board whose range could not be probed the
        // planner generated a floor that this check then refused.
        const VfOffsetRange offsetRange = vf_offset_range_current();
        const int rangeMinKHz = offsetRange.minKHz;
        const int rangeMaxKHz = offsetRange.maxKHz;
        const bool rangeKnown = offsetRange.known;
        const int hardLimitKHz = vf_offset_range_hard_limit_khz(offsetRange);
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
        if (hardLimitOffsets > 0) {
            set_last_apply_phase("apply: VF curve batch refused out-of-range offsets");
            debug_log("apply curve batch: refused %d point(s) beyond hard range limit %d kHz"
                      " (range %d..%d known=%d)\n",
                hardLimitOffsets, hardLimitKHz, rangeMinKHz, rangeMaxKHz,
                rangeKnown ? 1 : 0);
            curveBatchOk = false;
            curveRequestOk = false;
            // CT-03.  This branch used to set the two flags and stop.  The
            // curve domain's only failCount++ / append_failure() lives in the
            // sibling branch below, so an apply that refused its ENTIRE VF
            // batch returned failCount == 0 -- success -- released the
            // transition clamp, and told the user a profile was applied that
            // had never reached the GPU.  A refused batch is a failed required
            // domain and is routed through exactly the same accounting as a
            // driver rejection.
            failCount++;
            partialApplyRisk = true;
            set_message(curveVerifyDetail, sizeof(curveVerifyDetail),
                "Refused VF curve batch because %d point(s) exceeded the driver VF offset range (%d..%d kHz)",
                hardLimitOffsets, rangeMinKHz, rangeMaxKHz);
            append_failure("%s", curveVerifyDetail);
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
            curveBatchOk = apply_curve_offsets_verified(targetCurveOffsets, targetCurveMask, hasLock ? 3 : 2,
                correctionDeadlineTickMs);
            // THE critical sample. The curve is now at its new (raised) shape and
            // the final lock step has not run; this is the exact instant that was
            // uncapped before F-APPLY-CEILING, and the 2026-09-13 TDR landed
            // roughly a second into it. The settle loop below samples the rest of
            // the window; this catches its leading edge.
            apply_clock_witness_record("post-curve-batch (pre-lock)");
            bool settledOffsetsOk = false;
            if (!read_live_curve_snapshot_settled(6, 25, &settledOffsetsOk) || !settledOffsetsOk) {
                return apply_recover_clock_failure(clockCeiling, "Fresh curve readback failed", result, resultSize);
            }
            apply_log_curve_peak_after_batch(clockCeiling, hasLock, lockMhz, lockMode);
            DesiredSettings verifyDesired = *desired;
            for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                if (hasLock && lockedTailMask[ci]) continue;
                if (!desired->hasCurvePoint[ci]) {
                    verifyDesired.hasCurvePoint[ci] = false;
                    verifyDesired.curvePointMHz[ci] = 0;
                    verifyDesired.curvePointFromGpuOffset[ci] = 0;
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
                    // Synthesized from the selective offset batch: an absolute
                    // preview, not a projection flag inherited from `desired`.
                    verifyDesired.curvePointFromGpuOffset[ci] = 0;
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
                    int actualOffsetkHz = g_app.freqOffsets[ci];
                    int expectedOffsetkHz = targetCurveOffsets[ci];
                    bool userRelevantPoint = (expectedOffsetkHz != 0) && (actualMHz >= 500);
                    if (abs(actualOffsetkHz - expectedOffsetkHz) <= 12000) {
                        selectiveOffsetApplied++;
                        if (userRelevantPoint) userBoostApplied++;
                    } else {
                        selectiveOffsetFailed++;
                        if (userRelevantPoint) userBoostFailed++;
                        // Derived MHz is a preview. Verify the immutable control
                        // target; absolute point/tail authority is checked below.
                        debug_log("selective offset: point %d actual=%u MHz offset=%d kHz expected=%d kHz explicit=%d; retaining requested intent for verification\n",
                            ci, actualMHz, actualOffsetkHz, expectedOffsetkHz,
                            explicitCurveMask[ci]);
                    }
                }
                debug_log("selective offset: boost applied=%d failed=%d\n", selectiveOffsetApplied, selectiveOffsetFailed);
                if (hasLock) {
                    debug_log("flatten undervolt: target=%u MHz applied=%d failed=%d\n", lockMhz, flattenApplied, flattenFailed);
                }
            }
            auto verify_curve_request = [&](char* detailOut, size_t detailOutSize) -> bool {
                return apply_verify_curve_targets(&verifyDesired, explicitCurveMask, targetCurveOffsets,
                    curveRequest, gpuPolicyViaCurveBatch, hasLock, lockMode,
                    lockedTailMask, lockMhz, detailOut, detailOutSize);
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
                    // CT-03.  The fourth copy of this decision, and it used the
                    // same hardcoded -1,000,000 kHz `else` branch as the
                    // planner -- against a batch pre-check that allowed only
                    // 500,000 kHz on unprobeable hardware.  One policy now, so
                    // the correction pass cannot generate a floor the refusal
                    // check rejects.
                    const int correctionFloorTailOffsetKHz =
                        vf_offset_range_flatten_floor_khz(vf_offset_range_current());
                    bool correctionReachedFixedPoint = false;
                    bool correctionBudgetExhausted = false;
                    int correctionPassesRun = 0;
                    ULONGLONG previousPassMs = 0;
                    for (int correctionPass = 0; correctionPass < 25; correctionPass++) {
                        const ULONGLONG passStartTickMs = GetTickCount64();
                        if (!apply_correction_pass_may_start(correctionPass,
                                passStartTickMs - applyStartTickMs, previousPassMs)) {
                            correctionBudgetExhausted = true;
                            debug_log("curve correction: NOT starting pass %d -- %llu ms since"
                                      " apply entry plus the previous pass's %llu ms would pass"
                                      " the %lu ms correction budget; verifying what landed\n",
                                correctionPass + 1,
                                (unsigned long long)(passStartTickMs - applyStartTickMs),
                                (unsigned long long)previousPassMs,
                                apply_correction_budget_ms());
                            break;
                        }
                        correctionPassesRun = correctionPass + 1;
                        int correctedCurveOffsets[VF_NUM_POINTS] = {};
                        bool correctedCurveMask[VF_NUM_POINTS] = {};
                        bool haveCorrections = false;
                        int tailFloorCount = 0;

                        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                            if (g_app.curve[ci].freq_kHz == 0) {
                                continue;
                            }

                            unsigned int targetMHz = 0;
                            bool isTail = (hasLock && lockedTailMask[ci] && lockMhz > 0);
                            
                            if (isTail) {
                                targetMHz = lockMhz;
                            } else if (verifyDesired.hasCurvePoint[ci]) {
                                targetMHz = verifyDesired.curvePointMHz[ci];
                            } else {
                                continue;
                            }
                            if (!isTail && originalCurvePopulated[ci]) {
                                // ONE answer, shared with the initial target
                                // build. This branch used to re-derive
                                // `absolute - live base` on its own, so a
                                // correction triggered by an unrelated point --
                                // the TAIL missing by one VF bin -- overwrote
                                // points that were already correct at offset 0
                                // with +495000 kHz, then failed the apply on
                                // the value it had just invented (2026-09-17,
                                // profile 1 under load).
                                CurvePointOffsetRequest want = {};
                                want.fromGpuOffset = desired->curvePointFromGpuOffset[ci];
                                want.gpuOffsetComponentKHz = gpu_offset_component_mhz_for_point(ci,
                                    desired->gpuOffsetMHz, desiredActiveGpuOffsetExcludeLowCount) * 1000;
                                want.absoluteMHz = targetMHz;
                                // The base must be the one the driver is
                                // reporting NOW, not the sample taken right
                                // after the reset.  A correction pass runs
                                // after a fresh settled readback precisely so
                                // it can see where the point actually landed;
                                // recomputing against the reset-time sample
                                // reproduces the same offset every pass and the
                                // point never moves.  That is why profile 1
                                // sat at `ci=74 actual=2932 target=2902` with
                                // `desiredOffset` equal to what was already
                                // programmed, while the locked tail -- which
                                // already used the live base via
                                // curve_delta_khz_for_target_display_mhz() --
                                // converged in one pass every time.
                                //
                                // Subtracting the CURRENTLY PROGRAMMED offset
                                // from the CURRENT frequency is what keeps this
                                // absolute rather than cumulative: it recovers
                                // the stock base, so each pass recomputes the
                                // whole offset instead of accumulating a delta.
                                want.liveBaseKHz = curve_point_stock_base_khz(
                                    g_app.curve[ci].freq_kHz, g_app.freqOffsets[ci]);
                                long long diff = curve_point_target_offset_khz(&want);
                                if (diff > INT_MAX) diff = INT_MAX;
                                if (diff < INT_MIN) diff = INT_MIN;
                                correctedCurveOffsets[ci] = clamp_freq_delta_khz((int)diff);
                            } else if (isTail && ci != lockCi) {
                                // Tail points beyond the lock point: use uniform
                                // floor offset instead of per-point delta. On Blackwell
                                // the driver ignores per-point tail deltas, so the
                                // correction loop cannot converge with graduated offsets.
                                // A uniform minimum offset floors all tail points,
                                // letting the lock point control the entire region.
                                correctedCurveOffsets[ci] = correctionFloorTailOffsetKHz;
                                tailFloorCount++;
                            } else {
                                correctedCurveOffsets[ci] = curve_delta_khz_for_target_display_mhz(ci, targetMHz);
                            }
                            correctedCurveMask[ci] = true;
                            haveCorrections = true;
                        }
                        if (!haveCorrections) break;
                        if (tailFloorCount > 0) {
                            debug_log("correction pass %d: %d tail points use tail uniform floor offset=%d\n",
                                correctionPass + 1, tailFloorCount, correctionFloorTailOffsetKHz);
                        }
                        debug_log("curve correction pass %d: target point 75 live=%u MHz offset=%d desiredOffset=%d\n",
                            correctionPass + 1,
                            displayed_curve_mhz(g_app.curve[75].freq_kHz),
                            g_app.freqOffsets[75],
                            correctedCurveOffsets[75]);
                        set_last_apply_phase("apply: VF curve correction write");
                        bool correctionOk = apply_curve_offsets_verified(correctedCurveOffsets, correctedCurveMask, hasLock ? 3 : 2,
                            correctionDeadlineTickMs);
                        previousPassMs = GetTickCount64() - passStartTickMs;
                        if (!correctionOk) {
                            debug_log("curve correction pass %d had an offset verification mismatch\n", correctionPass + 1);
                        }
                        debug_log("curve correction pass %d wrote in %llu ms (%llu ms since apply entry)\n",
                            correctionPass + 1, (unsigned long long)previousPassMs,
                            (unsigned long long)(GetTickCount64() - applyStartTickMs));
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
                            int acceptedNonTail = 0, strictDiverged = 0;
                            // Fixed-point detection for the whole pass, not just
                            // the tail.  The per-point `stuck` bookkeeping below
                            // is reachable only for locked tail points; a NON-tail
                            // point the driver will not move was reclassified every
                            // pass and never ended the loop, so the apply ran all
                            // 25 passes at ~1 s each while holding the hardware
                            // gate (2026-09-17: ci=70 read 2827 against a 2797
                            // target identically 12 times before an unrelated
                            // watchdog tore the service down).  A pass whose inputs
                            // are unchanged writes the same offsets and reads back
                            // the same frequencies, so once no point improves, no
                            // later pass can improve one either.
                            int unconvergedPoints = 0, improvedPoints = 0;
                            for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                                if (!verifyDesired.hasCurvePoint[ci]) continue;
                                if (g_app.curve[ci].freq_kHz == 0) continue;
                                unsigned int actualMHz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
                                unsigned int targetMHz = (hasLock && lockedTailMask[ci] && lockMhz > 0)
                                    ? lockMhz : verifyDesired.curvePointMHz[ci];
                                if (actualMHz == targetMHz) continue;
                                unconvergedPoints++;
                                int actualKHz = (int)g_app.curve[ci].freq_kHz;
                                int targetKHz = (int)targetMHz * 1000;
                                int errorKHz = actualKHz > targetKHz ? (actualKHz - targetKHz) : (targetKHz - actualKHz);
                                int requiredDeltaKHz = curve_delta_khz_for_target_display_mhz_unclamped(ci, targetMHz);
                                if (prevErrorKHz[ci] != INT_MAX && errorKHz < prevErrorKHz[ci]) improvedPoints++;
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
                                // Only derived, lower readbacks may be accepted.
                                bool acceptNonTailReadback = !gpuPolicyViaCurveBatch
                                    && hasLock
                                    && !lockedTailMask[ci]
                                    && lockMhz > 0
                                    && !userExplicitPoint
                                    && actualMHz <= targetMHz;
                                if (acceptNonTailReadback && !diverged) {
                                    diverged = true;
                                }
                                if (diverged && acceptNonTailReadback) {
                                    acceptedNonTail++;
                                    debug_log("correction pass %d: non-tail %s point %d actual %u MHz != target %u MHz (tol=%u); accepting verification-only actual %u MHz (prevErr=%dKHz curErr=%dKHz)%s\n",
                                        correctionPass + 1,
                                        userExplicitPoint ? "explicit" : "readback",
                                        ci, actualMHz, targetMHz,
                                        curve_point_verify_tolerance_mhz(ci), actualMHz,
                                        prevErrorKHz[ci] == INT_MAX ? -1 : prevErrorKHz[ci], errorKHz,
                                        userExplicitPoint ? " (cross-talk near locked tail)" : "");
                                    verifyDesired.curvePointMHz[ci] = actualMHz;
                                } else if (diverged) {
                                    strictDiverged++;
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
                            debug_log("correction pass %d convergence: converging=%d worsening=%d stuck=%d outOfRange=%d acceptedNonTail=%d strictDiverged=%d unconverged=%d improved=%d\n",
                                correctionPass + 1, converging, worsening, stuck, outOfRange, acceptedNonTail, strictDiverged,
                                unconvergedPoints, improvedPoints);
                            if (correctionPass > 0 && unconvergedPoints > 0 && improvedPoints == 0) {
                                debug_log("correction pass %d: no point improved and %d remain unconverged; the correction has reached a fixed point, stopping instead of rewriting identical offsets\n",
                                    correctionPass + 1, unconvergedPoints);
                                correctionReachedFixedPoint = true;
                            }
                        }
                        // Verification runs BEFORE the fixed-point exit, and the
                        // order is the whole point.  The convergence bookkeeping
                        // above counts a point as unconverged on exact equality
                        // (`actualMHz == targetMHz`), while the apply is verified
                        // against curve_point_verify_tolerance_mhz() -- so a pass
                        // can land the whole curve inside tolerance, report
                        // `unconverged>0 improved=0`, and reach a fixed point that
                        // is in fact the requested result.  Breaking first left
                        // curveRequestOk false, which fails the apply and rolls
                        // back a curve that had verified.  The check is a pure
                        // read of the latest readback (apply_verify_curve_targets
                        // takes a const DesiredSettings and writes no hardware),
                        // so running it first costs nothing and can only turn a
                        // spurious failure into the success it already was.
                        if (verify_curve_request(curveVerifyDetail, sizeof(curveVerifyDetail))) {
                            curveRequestOk = true;
                            debug_log("curve correction pass %d converged to requested live MHz targets%s\n",
                                correctionPass + 1,
                                correctionReachedFixedPoint
                                    ? " (on the pass that reached a fixed point:"
                                      " within tolerance, though not exact)"
                                    : "");
                            break;
                        }
                        if (correctionReachedFixedPoint) break;
                    }
                    if (correctionBudgetExhausted && !curveRequestOk) {
                        // The last verification detail says which point missed;
                        // say also why no further pass was tried.
                        char budgetDetail[256] = {};
                        set_message(budgetDetail, sizeof(budgetDetail),
                            "%s (correction stopped after %d pass(es) to stay within the apply time budget)",
                            curveVerifyDetail[0] ? curveVerifyDetail : "VF curve did not verify",
                            correctionPassesRun);
                        StringCchCopyA(curveVerifyDetail, ARRAY_COUNT(curveVerifyDetail), budgetDetail);
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
                if (curveRequestOk && hasLock && lockMhz > 0 && lockMode != LOCK_MODE_HARD) {
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
                apply_log_post_apply_curve_diagnostics(
                    &verifyDesired, originalCurvePopulated, originalCurveVoltUv,
                    lockedTailMask, hasLock, lockMhz, flattenApplied, flattenFailed,
                    gpuPolicyViaCurveBatch, explicitCurveMask, targetCurveOffsets);
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
        // curveRequestOk is still true only when no branch above has already
        // counted this batch as failed; the out-of-range refusal both clears it
        // and counts, so without it that one refusal was reported twice.
        if (memApplied && preserveCurveAcrossMem && !curveRequest && !curveBatchOk &&
            curveRequestOk) {
            curveRequestOk = false;
            failCount++;
            partialApplyRisk = true;
            append_failure("Restoring the existing VF curve after the memory offset did not verify");
        }
        if (curveRequestOk) {
            size_t pos = 0;
            if (gpuPolicyViaCurveBatch) {
                if (userBoostFailed == 0) {
                    pos = gc_appendf(curveVerifySummary, sizeof(curveVerifySummary), pos,
                        " %+d MHz selective offset verified.", desired->gpuOffsetMHz);
                } else {
                    pos = gc_appendf(curveVerifySummary, sizeof(curveVerifySummary), pos,
                        " %+d MHz selective offset: %d of %d boost points matched.",
                        desired->gpuOffsetMHz, userBoostApplied, userBoostApplied + userBoostFailed);
                }
            }
            if (hasLock && lockMhz > 0 && lockMode == LOCK_MODE_FLATTEN) {
                if (flattenFailed == 0) {
                    pos = gc_appendf(curveVerifySummary, sizeof(curveVerifySummary), pos,
                        " Undervolt flatten to %u MHz verified (%d pts).", lockMhz, flattenApplied);
                } else {
                    pos = gc_appendf(curveVerifySummary, sizeof(curveVerifySummary), pos,
                        " Undervolt flatten to %u MHz: %d of %d pts matched.",
                        lockMhz, flattenApplied, flattenApplied + flattenFailed);
                }
            } else if (hasLock && lockMhz > 0 && lockMode == LOCK_MODE_HARD) {
                pos = gc_appendf(curveVerifySummary, sizeof(curveVerifySummary), pos,
                    " Hard lock pinned at %u MHz.", lockMhz);
            }
            (void)pos;
        }
    }
    if (failCount > 0)
        return apply_recover_clock_failure(clockCeiling, failureDetails, result, resultSize);
    // Reset locked clocks only when this request intentionally owns/replaces
    // the VF/lock domain. A sparse fan/memory/power request must preserve an
    // external hard lock and an ad-hoc fan update must preserve Green Curve's
    // existing lock intent. Named HARD->NONE/FLATTEN transitions carry
    // resetOcBeforeApply, while direct curve/GPU/lock requests own the domain.
    bool replacesLockDomain =
        service_request_replaces_lock_domain(desired);
    if (lockMode != LOCK_MODE_HARD && replacesLockDomain &&
        apply_clock_control_proven_absent(
            g_app.gpuFamily == GPU_FAMILY_PASCAL,
            clockCeiling.armed || g_app.transitionClockCapActive,
            g_app.lockMode == LOCK_MODE_HARD ||
                g_app.appliedLockMode == LOCK_MODE_HARD)) {
        debug_log("apply: no NVML locked-clock reset on Pascal; no hard pin or"
                  " retained transition cap exists\n");
    } else if (lockMode != LOCK_MODE_HARD && replacesLockDomain &&
               g_nvml_api.resetGpuLockedClocks) {
        // CT-02.  The release used to be unconditional on anything except the
        // lock mode.  Its justifying comment -- "by now the flatten tail is the
        // ceiling" -- is only true when the flatten tail was actually written
        // AND verified.  When the curve failed, releasing handed the user the
        // partially-written raised curve with nothing capping it, which is the
        // precise shape of the 2026-09-13 incident.
        //
        // curveTouched is part of the test because a request that never wrote
        // the curve at all has no raised state to protect: releasing a stale
        // external pin there is correct and is long-standing behaviour.
        const bool curveStateProvenSafe = failCount == 0 && curveRequestOk;
        if (!curveStateProvenSafe) {
            clockCeiling.retain("the VF curve did not verify; releasing the clamp"
                                " would uncap a partially written curve");
            debug_log("apply: NOT releasing the locked-clock domain -- curve"
                      " verification failed (curveRequestOk=%d curveTouched=%d);"
                      " the transition clamp at %u MHz stays until recovery\n",
                curveRequestOk ? 1 : 0, curveTouched ? 1 : 0,
                clockCeiling.plan.ceilingMHz);
        } else {
            char resetDetail[128] = {};
            if (nvml_reset_gpu_locked_clocks(resetDetail, sizeof(resetDetail))) {
                successCount++;
                // This IS the release of any F-APPLY-CEILING transition clamp:
                // by now the flatten tail (or the absence of a lock) is the
                // ceiling, and it has been verified.
                clockCeiling.adopt("released with the locked-clock domain");
                debug_log("apply: reset NVML locked clocks (not requesting HARD mode)\n");
            } else {
                failCount++;
                partialApplyRisk = true;
                // The requested end state is unpinned and the release failed,
                // so a cap the user did not ask for is still in force.  Mark
                // the guard adopted-as-retained so the destructor does not try
                // the same failing release again and so recovery knows a
                // restriction may still exist.
                clockCeiling.retain("the locked-clock release was refused by NVML");
                append_failure("NVML locked clocks did not reset: %s",
                    resetDetail[0] ? resetDetail : "unknown error");
            }
        }
    }
    if (hasLock) {
        g_app.lockedVi = lockVi;
        g_app.lockedCi = lockCi;
        g_app.lockedFreq = lockMhz;
        g_app.lockMode = lockMode;
        g_app.appliedLockVi = lockVi;
        g_app.appliedLockCi = lockCi;
        g_app.appliedLockFreq = lockMhz;
        g_app.appliedLockMode = lockMode;
        g_app.guiLockTracksAnchor = desired->lockTracksAnchor;

        if (lockMode == LOCK_MODE_HARD) {
            // CT-02.  The final pin is the request's own ceiling, but the
            // TRANSITION clamp may be lower than it -- when the outgoing
            // profile had a lower pin, the plan deliberately bounds the whole
            // transition at the lower of the two so the old pin keeps holding
            // until the new curve exists.  Writing the higher requested pin
            // over a curve that did NOT verify would relax that bound onto a
            // partially written curve, which is the failure mode this whole
            // guard exists to prevent.  A verified curve is the prerequisite
            // for raising the ceiling to what was asked for.
            const bool curveStateProvenSafeForPin = failCount == 0 && curveRequestOk;
            if (!curveStateProvenSafeForPin) {
                failCount++;
                partialApplyRisk = true;
                clockCeiling.retain("the VF curve did not verify; the final hard pin"
                                    " would relax the transition clamp over it");
                append_failure("Hard lock at %u MHz was not applied because the VF curve"
                               " did not verify; the transition clamp at %u MHz is still"
                               " in force", lockMhz, clockCeiling.plan.ceilingMHz);
                debug_log("apply: NOT asserting the final hard pin at %u MHz --"
                          " curveRequestOk=%d curveTouched=%d; keeping the transition"
                          " clamp at %u MHz\n",
                    lockMhz, curveRequestOk ? 1 : 0, curveTouched ? 1 : 0,
                    clockCeiling.plan.ceilingMHz);
            } else {
                char lockedClockDetail[128] = {};
                // With F-APPLY-CEILING armed this is a RE-assertion of the same
                // ceiling in its final symmetric form (or a raise from the
                // lower transition bound to the requested pin), not the first
                // time the clock is capped in this apply.
                if (nvml_set_gpu_locked_clocks(lockMhz, lockMhz, lockedClockDetail, sizeof(lockedClockDetail))) {
                    successCount++;
                    clockCeiling.adopt("re-asserted as the final hard pin");
                    debug_log("apply: hard lock pinned at %u MHz via NVML\n", lockMhz);
                } else {
                    failCount++;
                    partialApplyRisk = true;
                    clockCeiling.retain("final hard pin was refused by NVML");
                    append_failure("Hard lock at %u MHz failed: %s", lockMhz, lockedClockDetail);
                }
            }
        }
    }
    // The catch-all for any path that reaches here with the clamp still armed
    // after something was raised.  It is a net, not the primary mechanism: the
    // release and pin branches above each make their own explicit
    // retain()/adopt() decision based on whether the curve verified.
    //
    // This comment used to claim that every OTHER unadopted exit is ahead of
    // the first raising write.  That was false, and the two counterexamples
    // are now fixed at their sources: an unresolvable lock anchor is refused
    // before any write, and the non-HARD release is conditional on a verified
    // curve rather than on the lock mode alone.
    if (!clockCeiling.adopted && (curveTouched || gpuApplied))
        clockCeiling.retain("the apply raised the curve without establishing a final lock");
    if (curveTouched || gpuApplied || hasLock)
        apply_clock_witness_record("post-lock");
    if (desired->hasGpuOffset && !gpuPolicyViaCurveBatch) {
        if (desiredActiveGpuOffsetExcludeLowCount > 0) {
            persist_runtime_selective_gpu_offset_request(desired->gpuOffsetMHz, desiredActiveGpuOffsetExcludeLowCount);
        } else {
            clear_runtime_selective_gpu_offset_request();
        }
    } else if (!desired->hasGpuOffset && curveTouched && failCount == 0) {
        clear_runtime_selective_gpu_offset_request();
    }
    if (desired->hasPowerLimit && powerTargetWrittenByReset) {
        // The reset-to-stock phase already put the board on this apply's own
        // power target instead of passing through the board default first, so
        // the domain is satisfied before the VF curve was ever raised.  It is
        // still counted and still refreshes state, exactly as the late write
        // used to -- only the timing moved.
        successCount++;
        powerChanged = true;
        debug_log("apply power limit: %d%% already established by the reset phase"
                  " (before the curve write), no second write\n",
            desired->powerLimitPct);
    } else if (desired->hasPowerLimit) {
        int currentPowerPct = g_app.powerLimitPct;
        if (desired->powerLimitPct != currentPowerPct) {
            if (desired->powerLimitPct != clamp_power_limit_pct(desired->powerLimitPct)) {
                append_failure("Power limit %d%% outside safe range %d..%d%%", desired->powerLimitPct, POWER_LIMIT_MIN_PCT, POWER_LIMIT_MAX_PCT);
                failCount++;
                partialApplyRisk = true;
            } else if (!power_limit_surface_available(g_app.readback.powerLimit,
                           g_app.powerLimitDefaultmW, g_app.powerLimitCurrentmW)) {
                // A real request for a target this board has no surface for.
                // It fails, loudly and by name — the silent-refusal path is the
                // one this domain was getting wrong, not the refusal itself.
                append_failure("Power limit %d%% cannot be set: this GPU's driver does not"
                               " report a power target (constraints %d..%d mW)",
                    desired->powerLimitPct, g_app.powerLimitMinmW, g_app.powerLimitMaxmW);
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
    // Fan request validity was checked before any writes.  The helper is void:
    // attempted fan writes contribute only to the same core success/failure
    // counters as VF/lock/memory/power, then return here for one rollback decision.
    apply_fan_settings(desired, failureDetails, sizeof(failureDetails),
        successCount, failCount, result, resultSize, fanChanged);

    // Snapshot the core domain before entering advanced clocks.  The aggregate
    // counters continue below for final reporting, but XBAR/SYS/VIDEO outcomes
    // must never become inputs to this core rollback decision.
    const int coreSuccessCount = successCount;
    const int coreFailCount = failCount;
    // CT-04.  Recovery now keys off whether a core domain was ATTEMPTED, not
    // off a success count.  A counter only moves after a driver call returns,
    // so the first core write of an apply -- a curve batch that partly applied
    // before the driver rejected a later point, a baseline reset that got
    // halfway, the transition clamp itself -- produced (success=0, fail=1),
    // which the old mixed-result rule classified as "nothing to roll back"
    // while the hardware sat in a partial state.
    //
    // `clockCeiling.writeAttempted` is in the disjunction for exactly that
    // reason: arming is a hardware write the success counters never saw.
    //
    // F-01-002 legacy source gate searches for "fan failure triggered rollback".
    // The executable guarantee is the typed core policy below: any fan or other
    // core failure after an earlier attempted core write enters rollback.
    const bool anyCoreDomainAttempted =
        coreSuccessCount > 0 || partialApplyRisk || curveTouched || gpuApplied ||
        memApplied || powerChanged || clockCeiling.writeAttempted ||
        desired->resetOcBeforeApply != 0;
    if (service_apply_core_requires_recovery(anyCoreDomainAttempted, coreFailCount))
        return apply_recover_clock_failure(clockCeiling, failureDetails, result, resultSize);
    apply_advanced_clock_domains(desired, false, successCount,
                                 failCount, partialApplyRisk, failureDetails,
                                 ARRAY_COUNT(failureDetails));
    char detail[128] = {};
    if (memApplied || powerChanged || fanChanged) {
        refresh_global_state(detail, sizeof(detail));
    } else if (!curveTouched) {
        detect_clock_offsets();
    }
    EnterCriticalSection(&g_appLock);
    // A post-apply global refresh may suppress live lock auto-detection (for example
    // on selective GPU offset profiles) and clear the GUI lock markers even though
    // this apply explicitly requested a lock. Restore the requested lock state only
    // when the core transaction itself had no failures. Advanced-clock failures are
    // outside the core domain and must not erase a successfully applied core lock;
    // conversely, a failed or rolled-back core apply must not resurrect lock markers.
    if (hasLock && coreFailCount == 0) {
        unsigned int displayedLockMHz = lockMhz;
        g_app.lockedVi = lockVi;
        g_app.lockedCi = lockCi;
        g_app.lockedFreq = displayedLockMHz;
        g_app.lockMode = lockMode;
        g_app.appliedLockVi = lockVi;
        g_app.appliedLockCi = lockCi;
        g_app.appliedLockFreq = displayedLockMHz;
        g_app.appliedLockMode = lockMode;
        g_app.guiLockTracksAnchor = desired->lockTracksAnchor;
        debug_log("post-apply lock restore: ci=%d requested=%u displayed=%u mode=%s trackAnchor=%d (preserving requested value)\n",
            lockCi,
            lockMhz,
            displayedLockMHz,
            lock_mode_name(lockMode),
            desired->lockTracksAnchor ? 1 : 0);
    } else if (app_is_service_process() && coreFailCount == 0 && (curveTouched || desired->hasGpuOffset || hasCurveEdits)) {
        g_app.lockedVi = -1;
        g_app.lockedCi = -1;
        g_app.lockedFreq = 0;
        g_app.lockMode = LOCK_MODE_NONE;
        g_app.appliedLockVi = -1;
        g_app.appliedLockCi = -1;
        g_app.appliedLockFreq = 0;
        g_app.appliedLockMode = LOCK_MODE_NONE;
        g_app.guiLockTracksAnchor = true;
        debug_log("post-apply lock clear: no lock requested; cleared stale service lock markers\n");
    }
    capture_last_operation_snapshot(g_lastOperationAfterSnapshot, sizeof(g_lastOperationAfterSnapshot));
    if (!app_is_service_process()) {
        populate_global_controls();
        if (interactive) {
            populate_edits();
            invalidate_main_window();
        }
    }
    LeaveCriticalSection(&g_appLock);
    set_last_apply_phase(failCount == 0 ? "apply: complete" : "apply: failed");
    // The verify counters are the only place the difference between "did what
    // was asked" and "committed something close to it" exists; the summary text
    // below already reports it to a human, and this reports it to the client.
    gc_u32 outcomeSeverity = service_apply_outcome_severity_for_lock_mode(
        lockMode == LOCK_MODE_HARD, failCount, userBoostFailed, flattenFailed);
    if (outcomeSeverityOut) *outcomeSeverityOut = outcomeSeverity;
    const ULONGLONG applyElapsedMs = GetTickCount64() - applyStartTickMs;
    debug_log("apply outcome: severity=%s failCount=%d successCount=%d boostUnmatched=%d flattenUnmatched=%d elapsedMs=%llu budgetMs=%lu%s\n",
        service_outcome_severity_name(outcomeSeverity), failCount, successCount,
        userBoostFailed, flattenFailed, (unsigned long long)applyElapsedMs,
        (unsigned long)SERVICE_APPLY_HANDLER_BUDGET_MS,
        applyElapsedMs > SERVICE_APPLY_HANDLER_BUDGET_MS
            ? " OVER-BUDGET (the client has already timed out)" : "");
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
