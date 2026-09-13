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
// Scope-bound rather than open-coded because the apply has validation paths
// that return between arming and the final lock step; every one of those is
// ahead of the first clock-RAISING write, so an abandoned clamp must be
// released (apply_clock_ceiling_release_on_abandon()).  The one path that
// reaches the end of the apply with a raised curve and no resolved lock calls
// retain() instead, which is what keeps that predicate honest.
struct ApplyClockCeilingGuard {
    ApplyClockCeilingPlan plan = {};
    bool armed = false;
    bool adopted = false;

    explicit ApplyClockCeilingGuard(const DesiredSettings* desired) {
        if (!desired) return;
        plan = apply_clock_ceiling_plan(
            service_request_replaces_lock_domain(desired),
            desired->hasLock != 0, (int)desired->lockMode, desired->lockMHz,
            g_nvml_api.setGpuLockedClocks != nullptr &&
                g_nvml_api.resetGpuLockedClocks != nullptr);
        debug_log("apply ceiling: plan arm=%d ceiling=%u MHz finalPin=%d"
                  " (hasLock=%d mode=%s lockMHz=%u nvmlSet=%d nvmlReset=%d)\n",
            plan.arm ? 1 : 0, plan.ceilingMHz, plan.finalPinIsCeiling ? 1 : 0,
            desired->hasLock ? 1 : 0, lock_mode_name(desired->lockMode),
            desired->lockMHz,
            g_nvml_api.setGpuLockedClocks ? 1 : 0,
            g_nvml_api.resetGpuLockedClocks ? 1 : 0);
    }

    // Idempotent: the apply has two entry points into its first hardware write
    // (with and without reset-to-stock) and both must be covered.
    void arm() {
        if (!plan.arm || armed) return;
        set_last_apply_phase("apply: arm transition clock ceiling");
        char detail[128] = {};
        // Ceiling, not pin: a 0 minimum caps without also forcing the clock up
        // at idle.  A driver that refuses it still accepts the symmetric form,
        // which caps correctly -- capping is the whole point.
        if (nvml_set_gpu_locked_clocks(0, plan.ceilingMHz, detail, sizeof(detail))) {
            armed = true;
            apply_clock_witness_set_clamp(plan.ceilingMHz, true);
            debug_log("apply ceiling: armed 0..%u MHz before the first clock write\n",
                plan.ceilingMHz);
            apply_clock_witness_record("ceiling armed");
            return;
        }
        debug_log("apply ceiling: open-ended clamp refused (%s); retrying symmetric\n",
            detail[0] ? detail : "unknown error");
        detail[0] = 0;
        if (nvml_set_gpu_locked_clocks(plan.ceilingMHz, plan.ceilingMHz, detail,
                                       sizeof(detail))) {
            armed = true;
            apply_clock_witness_set_clamp(plan.ceilingMHz, true);
            debug_log("apply ceiling: armed %u..%u MHz before the first clock write\n",
                plan.ceilingMHz, plan.ceilingMHz);
            apply_clock_witness_record("ceiling armed");
            return;
        }
        // Not fatal: the apply is no worse off than it was before this guard
        // existed.  It IS the single most useful line in the log if the driver
        // falls over during a profile switch, so it is logged at full volume.
        debug_log("apply ceiling: COULD NOT ARM clamp at %u MHz (%s); the VF curve"
                  " write below runs uncapped until the lock step\n",
            plan.ceilingMHz, detail[0] ? detail : "unknown error");
    }

    // The final lock step took ownership of the locked-clock domain, so the
    // guard must not touch it again.
    void adopt(const char* how) {
        if (!armed) return;
        adopted = true;
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
        if (!apply_clock_ceiling_release_on_abandon(armed, adopted)) return;
        char detail[128] = {};
        bool ok = nvml_reset_gpu_locked_clocks(detail, sizeof(detail));
        debug_log("apply ceiling: released abandoned clamp at %u MHz ok=%d %s\n",
            plan.ceilingMHz, ok ? 1 : 0, ok ? "" : (detail[0] ? detail : "unknown error"));
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
    }
}

#endif
