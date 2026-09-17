// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The Linux half of F-APPLY-CEILING: deriving the transition clock ceiling for
// an apply, and arming it.  Split out of linux_backend_mutation.cpp to keep
// that shard under the source-size ratchet; the pure decision it consumes lives
// in apply_clock_ceiling_policy.h, which also carries the measured incident
// (2026-09-13, nvlddmkm event 153) this mechanism exists for.
//
// Included from linux_backend_mutation.cpp, which is itself included by
// linux_backend.cpp after lb_log() and the NVML function table exist.
#ifndef GREEN_CURVE_LINUX_APPLY_CEILING_H
#define GREEN_CURVE_LINUX_APPLY_CEILING_H

// Same decision as Windows, same reason: the transaction order used to be
// reset -> offsets -> curve -> lock, so a switch from an unpinned profile to a
// pinned one ran the newly raised curve with no clamp at all between the curve
// phase and the lock phase.  Arming the requested target as a ceiling first
// costs one NVML call and can never exceed what the request itself asks for.

// The outgoing envelope, same rule as Windows: a pinned profile's entitlement
// is its pin, not its deliberately raw VF tail -- reading the curve there
// would return the raw tail and "cap" the transition at a value that caps
// nothing.  Everything else is bounded by the curve it is running right now.
//
// `committed` is the intent this daemon last applied; it is the only record of
// an outgoing pin, because NVML exposes no getter for the configured
// locked-clock range.  A null one means "unknown", which falls back to the
// live curve rather than inventing a pin.
static unsigned int linux_outgoing_ceiling_mhz(const LinuxGpuState* g,
                                               const DesiredSettings* committed) {
    if (!g) return 0;
    if (g->retainedTransitionCeilingMHz > 0)
        return g->retainedTransitionCeilingMHz;
    if (committed && committed->hasLock &&
        committed->lockMode == LOCK_MODE_HARD && committed->lockMHz > 0)
        return committed->lockMHz;
    unsigned int peakMHz = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (g->curve[i].freq_kHz == 0) continue;
        unsigned int mhz = g->curve[i].freq_kHz / 1000;
        if (mhz > peakMHz) peakMHz = mhz;
    }
    return peakMHz;
}

// Whether a reset-to-stock would RAISE this GPU on its way through: the
// outgoing state is being held down by something the reset removes.
static bool linux_outgoing_state_holds_clocks_down(const LinuxGpuState* g,
                                                   const DesiredSettings* committed) {
    if (!g) return false;
    if (g->retainedTransitionCeilingMHz > 0) return true;
    if (committed && committed->hasLock && committed->lockMode != LOCK_MODE_NONE)
        return true;
    if (committed && committed->hasGpuOffset && committed->gpuOffsetMHz < 0)
        return true;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (g->curve[i].freq_kHz == 0) continue;
        if (g->freqOffsets[i] < 0) return true;
    }
    return false;
}

static ApplyClockCeilingPlan linux_apply_clock_ceiling_plan(
    LinuxGpuState* g, const DesiredSettings* d,
    const DesiredSettings* committed = nullptr) {
    if (!g || !d) return ApplyClockCeilingPlan{};
    return apply_clock_ceiling_plan(
        service_request_replaces_lock_domain(d), d->hasLock != 0,
        (int)d->lockMode, d->lockMHz,
        g->nvml.setGpuLockedClocks != nullptr &&
            g->nvml.resetGpuLockedClocks != nullptr,
        d->resetOcBeforeApply != 0,
        linux_outgoing_state_holds_clocks_down(g, committed),
        linux_outgoing_ceiling_mhz(g, committed));
}

// What arming actually did, so the witness and the phase result can both stop
// guessing.  `linux_apply_ceiling_armed` is written by the phase body and read
// by the witness; the pre-fix witness used the PLAN's `arm` flag as evidence
// of installation, so a refused clamp still produced a HELD verdict in the
// journal (audit CT-07).
static ApplyClockCeilingArmResult g_linuxCeilingArmResult =
    APPLY_CEILING_ARM_NOT_NEEDED;
static bool g_linuxCeilingArmed = false;
static bool g_linuxCeilingWriteAttempted = false;
// The plan AS ARMED, cached for the rest of the transaction.
//
// Every later consumer used to recompute it from `g` and `d`.  That is wrong
// for two independent reasons now that the plan reads the outgoing state: the
// reset and curve phases CHANGE that state, so a recomputed plan describes a
// transition that is already half-done, and the witness would print a ceiling
// value that never existed.  The clamp that is physically installed is a fact,
// not something to re-derive.
static ApplyClockCeilingPlan g_linuxCeilingPlan = {};
// Whether the state this transaction is leaving was a HARD pin.
//
// CT-07.  Rollback needs this and cannot derive it: a HARD profile's VF tail
// is deliberately left raw and high because the pin -- not the curve -- is
// what makes it safe.  Restoring that curve and then releasing the lock is the
// one rollback outcome that is worse than doing nothing.  It is a transaction
// fact recorded at entry, not something the restore can infer from a snapshot
// full of positive offsets, which an ordinary unpinned overclock also has.
static bool g_linuxOutgoingHadHardPin = false;
static unsigned int g_linuxOutgoingCeilingMHz = 0;

static void linux_apply_ceiling_reset_state() {
    g_linuxCeilingArmResult = APPLY_CEILING_ARM_NOT_NEEDED;
    g_linuxCeilingArmed = false;
    g_linuxCeilingWriteAttempted = false;
    g_linuxCeilingPlan = ApplyClockCeilingPlan{};
    g_linuxOutgoingHadHardPin = false;
    g_linuxOutgoingCeilingMHz = 0;
}

// Record what the transaction is leaving, before any phase runs.
static void linux_apply_ceiling_note_outgoing(const DesiredSettings* committed) {
    g_linuxOutgoingHadHardPin = committed && committed->hasLock &&
        committed->lockMode == LOCK_MODE_HARD && committed->lockMHz > 0;
}

// Whether a curve this rollback is about to restore is one that needs a pin to
// be safe.
static bool linux_snapshot_curve_needs_a_pin(const LinuxHardwareSnapshot* s) {
    if (!s || !s->curveValid) return false;
    return g_linuxOutgoingHadHardPin;
}

// The LINUX_MUTATION_LOCK_CEILING phase body.
//
// CT-01.  This used to `return true` on every path, including the one where
// both clamp forms were refused, with a comment arguing that failing the apply
// because the safety net could not be hung would be worse than the pre-guard
// behaviour.  That reasoning is wrong in the case that matters: the pre-guard
// behaviour is precisely the uncapped transition that produced the 2026-09-13
// TDR, so "no worse than before" means "still capable of crashing the driver".
// A protection the plan marks REQUIRED and that could not be installed now
// fails the phase, which the transaction turns into a refusal before the reset
// and curve phases run.
static bool linux_apply_arm_transition_ceiling(LinuxGpuState* g,
                                               const DesiredSettings* d,
                                               const DesiredSettings* committed) {
    // Transaction entry already initialized state and recorded outgoing ownership.
    ApplyClockCeilingPlan plan = linux_apply_clock_ceiling_plan(g, d, committed);
    g_linuxCeilingPlan = plan;
    if (!plan.arm) {
        if (!plan.required) return true;
        g_linuxCeilingArmResult = APPLY_CEILING_ARM_UNAVAILABLE;
        lb_log("apply: transition clock ceiling at %u MHz is REQUIRED (%s) but this"
               " driver exposes no usable locked-clock control; refusing the"
               " transition before any write\n",
               plan.ceilingMHz, apply_clock_ceiling_reason_name(plan.reason));
        return false;
    }
    g_linuxCeilingWriteAttempted = true;
    // Ceiling, not pin: a 0 minimum caps without forcing the clock up at idle
    // (what `nvidia-smi --lock-gpu-clocks=0,N` asks for).  A driver that refuses
    // it still accepts the symmetric form, which caps correctly -- but that
    // form adds a FLOOR, so it is only used where the plan permits it.
    if (g->nvml.setGpuLockedClocks(g->nvmlDevice, 0, plan.ceilingMHz) ==
        NVML_SUCCESS) {
        g_linuxCeilingArmed = true;
        g->retainedTransitionCeilingMHz = plan.ceilingMHz;
        g_linuxCeilingArmResult = APPLY_CEILING_ARM_INSTALLED;
        lb_log("apply: transition clock ceiling armed 0..%u MHz before the"
               " reset/curve writes\n", plan.ceilingMHz);
        return true;
    }
    if (plan.symmetricFallbackAllowed &&
        g->nvml.setGpuLockedClocks(g->nvmlDevice, plan.ceilingMHz,
                                   plan.ceilingMHz) == NVML_SUCCESS) {
        g_linuxCeilingArmed = true;
        g->retainedTransitionCeilingMHz = plan.ceilingMHz;
        g_linuxCeilingArmResult = APPLY_CEILING_ARM_INSTALLED;
        lb_log("apply: transition clock ceiling armed %u..%u MHz before the"
               " reset/curve writes\n", plan.ceilingMHz, plan.ceilingMHz);
        return true;
    }
    g_linuxCeilingArmResult = APPLY_CEILING_ARM_REFUSED;
    lb_log("apply: COULD NOT ARM transition clock ceiling at %u MHz (symmetric"
           " fallback %s); protection was %s\n",
           plan.ceilingMHz,
           plan.symmetricFallbackAllowed ? "also refused" : "not permitted here",
           plan.required ? "REQUIRED -- refusing the transition before the reset"
                           " and curve writes"
                         : "optional -- continuing");
    return !apply_clock_ceiling_transition_must_refuse(plan.required,
                                                       g_linuxCeilingArmResult);
}

// The Linux counterpart of the Windows apply-clock witness: one line recording
// the measured GPU clock next to the ceiling that is supposed to be capping it,
// plus the live load. Without the load, a crash report that only reproduces
// under 3D work cannot be told apart from an idle run that proves nothing.
// Sampled synchronously between phases -- never from another thread, which
// would add a concurrent NVML reader alongside an in-flight VF write.
static void linux_apply_log_clock_witness(LinuxGpuState* g,
                                          const DesiredSettings* d,
                                          const char* stage, bool finalState = false) {
    if (!g || !stage) return;
    // The plan as it was ARMED, not a fresh one: the phases between arming and
    // this sample have already changed the live state the plan is derived from,
    // so recomputing would print a ceiling that never existed.
    ApplyClockCeilingPlan ceiling = g_linuxCeilingPlan;
    bool armed = g_linuxCeilingArmed;
    if (finalState && d) {
        armed = d->lockMode == LOCK_MODE_HARD && d->lockMHz > 0;
        ceiling.required = ceiling.arm = armed;
        ceiling.ceilingMHz = armed ? d->lockMHz : 0;
    }
    unsigned int gpc = 0, sm = 0, mem = 0, tempC = 0, powerMw = 0;
    bool clockOk = g->nvml.getClock &&
        g->nvml.getClock(g->nvmlDevice, NVML_CLOCK_GRAPHICS,
                         NVML_CLOCK_ID_CURRENT, &gpc) == NVML_SUCCESS;
    if (clockOk) {
        g->nvml.getClock(g->nvmlDevice, NVML_CLOCK_SM, NVML_CLOCK_ID_CURRENT, &sm);
        g->nvml.getClock(g->nvmlDevice, NVML_CLOCK_MEM, NVML_CLOCK_ID_CURRENT, &mem);
    }
    bool tempOk = g->nvml.getTemperature &&
        g->nvml.getTemperature(g->nvmlDevice, NVML_TEMPERATURE_GPU, &tempC) == NVML_SUCCESS;
    nvmlUtilization_t util = {};
    bool utilOk = g->nvml.getUtilization &&
        g->nvml.getUtilization(g->nvmlDevice, &util) == NVML_SUCCESS;
    bool powerOk = g->nvml.getPowerUsage &&
        g->nvml.getPowerUsage(g->nvmlDevice, &powerMw) == NVML_SUCCESS;
    // Same shared rule the Windows verdict uses, so the two platforms cannot
    // disagree about what "the clamp held" means.
    //
    // CT-07.  This used to pass the PLAN's `arm` flag as BOTH the requested
    // and the armed argument, which made "we intended to arm" indistinguishable
    // from "the clamp is installed".  A driver that refused the clamp therefore
    // produced `HELD` in the journal whenever the GPU happened to be idle --
    // the single diagnostic whose entire job is to catch an unprotected
    // transition, reporting that the transition was protected.  The verdict now
    // reads the flag the arming call actually set.
    const char* verdict = apply_clock_witness_verdict_name(
        apply_clock_witness_verdict(ceiling.required || ceiling.arm,
                                    armed, ceiling.ceilingMHz,
                                    clockOk ? gpc : 0));
    lb_log("apply clock witness [%s]: gpc=%s%u MHz sm=%u mem=%u util=%s%u%%/%u%% "
           "power=%s%u.%01u W temp=%s%u C ceiling=%u MHz -> %s\n",
           stage, clockOk ? "" : "unknown:", gpc, sm, mem,
           utilOk ? "" : "unknown:", util.gpu, util.memory,
           powerOk ? "" : "unknown:", powerMw / 1000, (powerMw % 1000) / 100,
           tempOk ? "" : "unknown:", tempC, ceiling.ceilingMHz, verdict);
}

// The LINUX_MUTATION_LOCK phase body: establish the authoritative final lock
// state (a hard pin, or the release that also hands over any F-APPLY-CEILING
// transition clamp), then witness the result.  The witness runs either way --
// a failed lock write is exactly when knowing the live clock matters most.
static bool linux_apply_write_final_lock(LinuxGpuState* g,
                                         const DesiredSettings* d) {
    bool lockOk;
    if (d->lockMode == LOCK_MODE_HARD && d->lockMHz > 0)
        lockOk = g->nvml.setGpuLockedClocks &&
                 g->nvml.setGpuLockedClocks(g->nvmlDevice, d->lockMHz,
                                            d->lockMHz) == NVML_SUCCESS;
    else
        lockOk = g->nvml.resetGpuLockedClocks &&
                 g->nvml.resetGpuLockedClocks(g->nvmlDevice) == NVML_SUCCESS;
    if (lockOk) {
        g->retainedTransitionCeilingMHz = 0;
        // Post-handoff clocks obey the final state, not the old lower cap.
        linux_apply_log_clock_witness(g, d, "post-lock final state", true);
    } else {
        linux_apply_log_clock_witness(g, d, "post-lock failure");
    }
    return lockOk;
}

// What LINUX_MUTATION_RESET_BASELINE does with the locked-clock domain.
// Releasing it there would undo the ceiling the phase before it just armed and
// re-open the exact window F-APPLY-CEILING closes -- which is precisely what
// the pre-fix code did, unconditionally, immediately before writing the new
// curve.  With no ceiling armed this still clears any stale pin, as before.
static bool linux_apply_reset_baseline_locked_clocks(LinuxGpuState* g,
                                                     const DesiredSettings* d) {
    (void)d;
    // Keyed off the clamp that is ACTUALLY installed, not off a freshly
    // recomputed plan.  Recomputing here would consult a live state the arming
    // phase has already begun changing, and would keep the pin standing on the
    // strength of an intention rather than a fact.
    if (g_linuxCeilingArmed) {
        lb_log("apply: reset baseline keeps the %u MHz transition ceiling\n",
               g_linuxCeilingPlan.ceilingMHz);
        return true;
    }
    return g->nvml.resetGpuLockedClocks &&
           g->nvml.resetGpuLockedClocks(g->nvmlDevice) == NVML_SUCCESS;
}

#endif
