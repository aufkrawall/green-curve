// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The Windows half of F-APPLY-CEILING: the scope-bound transition clock clamp
// an Apply holds while it rewrites the VF curve, plus the peak-curve diagnostic
// that makes an uncapped transition visible in the log instead of only in a
// driver crash.
//
// Split out of gpu_backend_apply.cpp, which is already far over the project's
// file-size guidance; the pure decision it consumes lives in
// apply_clock_ceiling_policy.h, which also carries the measured incident this
// whole mechanism exists for.
#ifndef GREEN_CURVE_GPU_BACKEND_APPLY_CEILING_H
#define GREEN_CURVE_GPU_BACKEND_APPLY_CEILING_H

// F-APPLY-CEILING: the transition clock clamp, armed before the first
// clock-affecting write of an apply and held until the final lock state is
// established.  See apply_clock_ceiling_policy.h for the measured TDR this
// exists to prevent; the short version is that a profile switch used to write
// the new (raised) VF curve and only THEN pin the clock, leaving the GPU
// running a ~3637 MHz curve uncapped for 1.2 s under game load.
//
// Scope exit retains protection. Only an explicit verified finalization or
// recovery may release it; an early return can follow a partial driver write.
// The highest clock the OUTGOING state is entitled to run, which is what
// bounds the transition together with the incoming request.
//
// For a pinned outgoing profile this is its pin, NOT its live VF curve peak:
// a HARD profile's tail is deliberately left raw and high, and its safety
// comes entirely from the pin.  Reading the curve there would produce the
// 3637 MHz raw tail from the 2026-09-13 incident and "cap" the transition at a
// value that caps nothing.  For everything else the live curve peak IS the
// envelope -- the GPU is running it right now.
static inline unsigned int apply_outgoing_ceiling_mhz() {
    if (g_app.transitionClockCapActive && g_app.transitionClockCapMHz > 0)
        return g_app.transitionClockCapMHz;
    if (g_app.lockMode == LOCK_MODE_HARD && g_app.appliedLockFreq > 0)
        return g_app.appliedLockFreq;
    if (g_app.lockMode == LOCK_MODE_HARD && g_app.lockedFreq > 0)
        return g_app.lockedFreq;
    unsigned int peakMHz = 0;
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (g_app.curve[ci].freq_kHz == 0) continue;
        unsigned int mhz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
        if (mhz > peakMHz) peakMHz = mhz;
    }
    return peakMHz;
}

// Whether the outgoing state is being held DOWN by something a reset-to-stock
// removes.  When it is, the window between the reset and the new curve write
// runs the GPU at stock, which is above BOTH profiles -- the transition state
// neither the old nor the new profile ever validated.
//
// A purely positive outgoing offset fails this test on purpose: resetting it
// only lowers the GPU, and the subsequent raise goes no higher than the
// incoming profile's own intent.
static inline bool apply_outgoing_state_holds_clocks_down() {
    if (g_app.transitionClockCapActive) return true;
    if (g_app.lockMode != LOCK_MODE_NONE) return true;
    if (g_app.gpuClockOffsetkHz < 0) return true;
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (g_app.curve[ci].freq_kHz == 0) continue;
        if (g_app.freqOffsets[ci] < 0) return true;
    }
    return false;
}

struct ApplyClockCeilingGuard {
    ApplyClockCeilingPlan plan = {};
    bool armed = false;
    bool adopted = false;
    // A clamp write was issued to the driver, whether or not it took.  The
    // apply needs this even on the refusal path: a failed NVML write is still
    // a hardware write attempt, so automatic restoration must latch off and
    // the stability proof stays invalidated.
    bool writeAttempted = false;
    ApplyClockCeilingArmResult armResult = APPLY_CEILING_ARM_NOT_NEEDED;

    explicit ApplyClockCeilingGuard(const DesiredSettings* desired) {
        if (!desired) return;
        // Resolve lazy NVML initialisation BEFORE reading the entry points.
        // A null pointer that only means "NVML has not been loaded yet" used
        // to select the no-protection path on the first apply of a process,
        // which is exactly when a profile switch is most likely (logon
        // restore, resume, tray pick right after start).
        (void)nvml_ensure_ready();
        const bool resetsToStock = desired->resetOcBeforeApply != 0;
        const bool outgoingHoldsDown = apply_outgoing_state_holds_clocks_down();
        const unsigned int outgoingCeiling = apply_outgoing_ceiling_mhz();
        plan = apply_clock_ceiling_plan(
            service_request_replaces_lock_domain(desired),
            desired->hasLock != 0, (int)desired->lockMode, desired->lockMHz,
            g_nvml_api.setGpuLockedClocks != nullptr &&
                g_nvml_api.resetGpuLockedClocks != nullptr,
            resetsToStock, outgoingHoldsDown, outgoingCeiling);
        debug_log("apply ceiling: plan arm=%d required=%d reason=%s ceiling=%u MHz"
                  " finalPin=%d symFallback=%d (hasLock=%d mode=%s lockMHz=%u"
                  " resetToStock=%d outgoingHoldsDown=%d outgoingCeiling=%u"
                  " outgoingMode=%s nvmlSet=%d nvmlReset=%d)\n",
            plan.arm ? 1 : 0, plan.required ? 1 : 0,
            apply_clock_ceiling_reason_name(plan.reason), plan.ceilingMHz,
            plan.finalPinIsCeiling ? 1 : 0, plan.symmetricFallbackAllowed ? 1 : 0,
            desired->hasLock ? 1 : 0, lock_mode_name(desired->lockMode),
            desired->lockMHz, resetsToStock ? 1 : 0, outgoingHoldsDown ? 1 : 0,
            outgoingCeiling, lock_mode_name(g_app.lockMode),
            g_nvml_api.setGpuLockedClocks ? 1 : 0,
            g_nvml_api.resetGpuLockedClocks ? 1 : 0);
    }

    // Idempotent: the apply has two entry points into its first hardware write
    // (with and without reset-to-stock) and both must be covered.
    //
    // Returns what actually happened, because the caller has to act on it.
    // This used to return void and merely log a refusal, so a driver that
    // rejected the clamp let the apply continue straight into reset-to-stock
    // and the curve batch with no protection at all (audit CT-01) -- the
    // uncapped sequence the whole mechanism exists to prevent.
    ApplyClockCeilingArmResult arm() {
        if (armed) return armResult;
        adopted = false;
        if (!plan.arm) {
            if (!plan.required) {
                armResult = APPLY_CEILING_ARM_NOT_NEEDED;
                return armResult;
            }
            // No entry points at all is a property of the driver, not of this
            // attempt; a plan with no defensible ceiling value is neither.
            armResult = plan.clampControlAbsent ? APPLY_CEILING_ARM_UNSUPPORTED
                                                : APPLY_CEILING_ARM_UNAVAILABLE;
            debug_log("apply ceiling: protection REQUIRED (%s) but no clamp can be"
                      " installed (ceiling=%u MHz nvmlSet=%d nvmlReset=%d);"
                      " armResult=%s\n",
                apply_clock_ceiling_reason_name(plan.reason), plan.ceilingMHz,
                g_nvml_api.setGpuLockedClocks ? 1 : 0,
                g_nvml_api.resetGpuLockedClocks ? 1 : 0,
                apply_clock_ceiling_arm_result_name(armResult));
            log_unprotected_if_proceeding();
            return armResult;
        }
        set_last_apply_phase("apply: arm transition clock ceiling");
        char detail[128] = {};
        writeAttempted = true;
        // Every permitted form has to answer NOT_SUPPORTED before the clamp
        // counts as absent from this GPU: one form being unsupported while
        // another is merely declined is still a clamp this hardware can hold.
        bool openNotSupported = false, symmetricNotSupported = false;
        // Ceiling, not pin: a 0 minimum caps without also forcing the clock up
        // at idle.  A driver that refuses it still accepts the symmetric form,
        // which caps correctly -- but that form adds a FLOOR, so it is only
        // used where the plan says both endpoints permit it.
        if (nvml_set_gpu_locked_clocks(0, plan.ceilingMHz, detail, sizeof(detail),
                                       &openNotSupported)) {
            armed = true;
            g_app.transitionClockCapActive = true;
            g_app.transitionClockCapMHz = plan.ceilingMHz;
            armResult = APPLY_CEILING_ARM_INSTALLED;
            apply_clock_witness_set_clamp(plan.ceilingMHz, true);
            debug_log("apply ceiling: armed 0..%u MHz before the first clock write\n",
                plan.ceilingMHz);
            apply_clock_witness_record_at_arming("ceiling armed");
            return armResult;
        }
        if (!plan.symmetricFallbackAllowed) {
            armResult = openNotSupported ? APPLY_CEILING_ARM_UNSUPPORTED
                                         : APPLY_CEILING_ARM_REFUSED;
            debug_log("apply ceiling: open-ended clamp refused (%s) and the symmetric"
                      " form is not permitted for a %s clamp -- it would add a clock"
                      " FLOOR this request never asked for; armResult=%s\n",
                detail[0] ? detail : "unknown error",
                apply_clock_ceiling_reason_name(plan.reason),
                apply_clock_ceiling_arm_result_name(armResult));
            log_unprotected_if_proceeding();
            return armResult;
        }
        debug_log("apply ceiling: open-ended clamp refused (%s); retrying symmetric\n",
            detail[0] ? detail : "unknown error");
        detail[0] = 0;
        if (nvml_set_gpu_locked_clocks(plan.ceilingMHz, plan.ceilingMHz, detail,
                                       sizeof(detail), &symmetricNotSupported)) {
            armed = true;
            g_app.transitionClockCapActive = true;
            g_app.transitionClockCapMHz = plan.ceilingMHz;
            armResult = APPLY_CEILING_ARM_INSTALLED;
            apply_clock_witness_set_clamp(plan.ceilingMHz, true);
            debug_log("apply ceiling: armed %u..%u MHz before the first clock write\n",
                plan.ceilingMHz, plan.ceilingMHz);
            apply_clock_witness_record_at_arming("ceiling armed");
            return armResult;
        }
        armResult = (openNotSupported && symmetricNotSupported)
            ? APPLY_CEILING_ARM_UNSUPPORTED
            : APPLY_CEILING_ARM_REFUSED;
        // The single most useful line in the log if the driver falls over
        // during a profile switch, so it is logged at full volume.  Unlike the
        // pre-fix version it is no longer followed by the write it is warning
        // about -- unless the clamp is one this GPU has never had, which the
        // line now names explicitly.
        debug_log("apply ceiling: COULD NOT ARM clamp at %u MHz (%s); armResult=%s;"
                  " protection was %s\n",
            plan.ceilingMHz, detail[0] ? detail : "unknown error",
            apply_clock_ceiling_arm_result_name(armResult),
            !plan.required ? "optional -- continuing"
                           : (must_refuse_transition()
                                  ? "REQUIRED -- refusing the transition before"
                                    " any clock write"
                                  : "REQUIRED but unavailable on this GPU"));
        log_unprotected_if_proceeding();
        return armResult;
    }

    // One line, at the only two places it can be true, so an unprotected
    // transition is never an absence in the log.
    void log_unprotected_if_proceeding() const {
        if (!apply_clock_ceiling_proceeds_unprotected(plan.required, armResult,
                                                      plan.reason))
            return;
        debug_log("apply ceiling: PROCEEDING UNPROTECTED -- a %u MHz transition"
                  " clamp was required (%s) but this GPU has no locked-clock"
                  " control (%s). The request names no lock of its own, so its"
                  " end state is uncapped by the user's own choice and this is"
                  " the behaviour this hardware has always had; a driver that"
                  " CAN hold a clamp and merely declined it would have refused"
                  " the transition instead\n",
            plan.ceilingMHz, apply_clock_ceiling_reason_name(plan.reason),
            apply_clock_ceiling_arm_result_name(armResult));
    }

    // Whether this apply must stop before mutating anything.
    bool must_refuse_transition() const {
        return apply_clock_ceiling_transition_must_refuse(plan.required, armResult,
                                                          plan.reason);
    }

    // What to tell the user.  Names the specific transition and the specific
    // missing capability rather than reporting a generic failure, because the
    // whole point of refusing here is that everything else about this GPU
    // still works: fan, memory, power and any request that does not raise a
    // clock past the outgoing envelope are unaffected.
    void refusal_message(char* out, size_t outSize) const {
        const char* why = (armResult == APPLY_CEILING_ARM_REFUSED)
            ? "the driver refused it"
            : (armResult == APPLY_CEILING_ARM_UNSUPPORTED)
                ? "this GPU has no locked-clock control"
                : "this driver exposes no usable locked-clock control";
        set_message(out, outSize,
            "This profile switch needs a temporary %u MHz clock cap while the VF"
            " curve is rewritten (%s), but %s. No clock settings were changed."
            " Fan, memory and power settings are unaffected.",
            plan.ceilingMHz, apply_clock_ceiling_reason_name(plan.reason), why);
    }

    // The final lock step took ownership of the locked-clock domain, so the
    // guard must not touch it again.
    void adopt(const char* how) {
        if (!armed) return;
        adopted = true;
        armed = false;
        g_app.transitionClockCapActive = false;
        apply_clock_witness_finish_transition();
        debug_log("apply ceiling: clamp adopted by the final lock step (%s)\n",
            how ? how : "");
    }

    // The final lock step FAILED after the curve was already raised.  Releasing
    // the clamp here would hand the user exactly the uncapped raised curve this
    // guard exists to prevent, so the transition clamp stays -- it caps at the
    // value the request asked for, which is the closest thing to the intended
    // end state that still exists.  The apply reports the failure either way.
    void retain(const char* why) {
        if (!armed) return;
        adopted = true;
        debug_log("apply ceiling: KEEPING the transition clamp at %u MHz (%s);"
                  " releasing it would leave the raised curve uncapped\n",
            plan.ceilingMHz, why ? why : "");
    }

    ~ApplyClockCeilingGuard() {
        if (armed && !adopted)
            retain("scope exit without a verified final state");
    }
};

// The peak the live curve reaches after a batch write, and whether a clamp was
// holding while it got there.  The 2026-09-13 incident was only reconstructable
// afterwards by summing 51 per-point readback lines by hand; this is the one
// line that answers "did the curve go somewhere the lock was supposed to
// prevent, and was anything stopping it".
static inline void apply_log_curve_peak_after_batch(
    const ApplyClockCeilingGuard& ceiling, bool hasLock, unsigned int lockMhz,
    LockMode lockMode) {
    unsigned int peakMHz = 0;
    int peakCi = -1;
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (g_app.curve[ci].freq_kHz == 0) continue;
        unsigned int mhz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
        if (mhz > peakMHz) { peakMHz = mhz; peakCi = ci; }
    }
    debug_log("apply curve peak after batch: %u MHz at ci=%d;"
              " transition clamp armed=%d at %u MHz (lockMode=%s)\n",
        peakMHz, peakCi, ceiling.armed ? 1 : 0, ceiling.plan.ceilingMHz,
        lock_mode_name(lockMode));
    if (!ceiling.armed && hasLock && lockMhz > 0 && peakMHz > lockMhz) {
        debug_log("apply curve peak: WARNING curve peak %u MHz exceeds the"
                  " requested lock %u MHz with no clamp armed; this is the"
                  " uncapped-transition shape that produced the 2026-09-13 TDR\n",
            peakMHz, lockMhz);
    } else if (!ceiling.armed && !hasLock) {
        // F-03-004: an apply that raises the curve with NO lock requested arms
        // no clamp -- correctly, because the end state the user asked for is
        // itself uncapped, so the transition creates no operating point above
        // their own intent. But the shape on the wire is identical to the one
        // that produced the TDR, and until now the log went silent for it: the
        // warning above is gated on hasLock, so the single most useful line
        // during an unlocked profile switch was the one that was never written.
        // Stated plainly instead, so a post-mortem can tell "no clamp because
        // none was needed" from "no clamp because arming failed".
        debug_log("apply curve peak: no clamp was armed because the request"
                  " carries no lock target; the curve peaked at %u MHz and the"
                  " requested end state is uncapped by design\n", peakMHz);
    }
}

#endif
