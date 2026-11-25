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

#include "linux_apply_ceiling_transaction.h"

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

static bool linux_outgoing_curve_has_negative_offsets(const LinuxGpuState* g) {
    if (!g) return false;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (g->curve[i].freq_kHz == 0) continue;
        if (g->freqOffsets[i] < 0) return true;
    }
    return false;
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
    return linux_outgoing_curve_has_negative_offsets(g);
}

template <class D>
static inline auto linux_apply_ceiling_scope_is_replace_profile(const D* d, int)
    -> decltype((void)d->applicationScope, bool()) {
    return desired_application_scope_kind(d->applicationScope) == DESIRED_SCOPE_REPLACE_PROFILE;
}
template <class D>
static inline bool linux_apply_ceiling_scope_is_replace_profile(const D*, long) { return false; }

static ApplyClockCeilingPlan linux_apply_clock_ceiling_plan(
    LinuxGpuState* g, const LinuxApplyCeilingTransaction& t, const DesiredSettings* d,
    const DesiredSettings* committed = nullptr) {
    if (!g || !d) return ApplyClockCeilingPlan{};
    const bool resetsToStock = d->resetOcBeforeApply != 0 || t.scopedCurveTransition;
    const bool replacesClockDomain = service_request_replaces_lock_domain(d) ||
        t.scopedCurveTransition ||
        (d->resetOcBeforeApply && linux_apply_ceiling_scope_is_replace_profile(d, 0));
    const bool outgoingHardPin = committed && committed->hasLock &&
        committed->lockMode == LOCK_MODE_HARD && committed->lockMHz > 0;
    const bool outgoingFlatten = (committed && committed->hasLock &&
        committed->lockMode == LOCK_MODE_FLATTEN) ||
        (!outgoingHardPin && linux_outgoing_curve_has_negative_offsets(g));
    const bool memoryMayDropFlatten = !resetsToStock && d->hasMemOffset && outgoingFlatten;
    return apply_clock_ceiling_plan(
        replacesClockDomain, d->hasLock != 0, (int)d->lockMode, d->lockMHz,
        g->nvml.setGpuLockedClocks != nullptr && g->nvml.resetGpuLockedClocks != nullptr,
        resetsToStock,
        linux_outgoing_state_holds_clocks_down(g, committed),
        linux_outgoing_ceiling_mhz(g, committed), memoryMayDropFlatten);
}

// The transaction's arm result, armed clamp, cached plan and outgoing facts
// live in LinuxApplyCeilingTransaction (linux_apply_ceiling_transaction.h),
// owned by the caller of linux_backend_apply()/linux_backend_reset() and
// passed to every function below that reads or writes them.

// Record what the transaction is leaving, before any phase runs.
static void linux_apply_ceiling_note_outgoing(LinuxApplyCeilingTransaction& t,
                                              const DesiredSettings* committed) {
    t.outgoingHadHardPin = committed && committed->hasLock &&
        committed->lockMode == LOCK_MODE_HARD && committed->lockMHz > 0;
}

// Transaction entry: start from a clean value, then record what is being left.
// The caller keeps the value after the transaction returns, because a failed
// commit rolls back through linux_backend_restore_snapshot() with it.
static void linux_apply_ceiling_begin(LinuxApplyCeilingTransaction& t,
                                      const DesiredSettings* outgoing) {
    t = LinuxApplyCeilingTransaction{};
    linux_apply_ceiling_note_outgoing(t, outgoing);
}

// Capture Reset's rollback bound before any offset, VF or lock write. The
// daemon may also need it if the successful hardware Reset cannot commit.
static void linux_reset_capture_rollback_ceiling(const LinuxGpuState* g,
                                                 LinuxApplyCeilingTransaction& t,
                                                 const DesiredSettings* previous) {
    linux_apply_ceiling_note_outgoing(t, previous);
    t.outgoingCeilingMHz = linux_outgoing_ceiling_mhz(g, previous);
}

// Whether a curve this rollback is about to restore is one that needs a pin to
// be safe.
static bool linux_snapshot_curve_needs_a_pin(const LinuxApplyCeilingTransaction& t,
                                             const LinuxHardwareSnapshot* s) {
    if (!s || !s->curveValid) return false;
    return t.outgoingHadHardPin;
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
                                               LinuxApplyCeilingTransaction& t,
                                               const DesiredSettings* d,
                                               const DesiredSettings* committed, bool installedGuardRequired = false,
                                               const ApplyClockCeilingPlan* modeled = nullptr) {
    if (modeled && modeled->required && (!g || !d)) {
        lb_log("apply: modeled clock ceiling refused without a GPU and desired settings\n"); return false;
    }
    // Transaction entry already initialized state and recorded outgoing ownership.
    ApplyClockCeilingPlan plan = linux_apply_clock_ceiling_plan(g, t, d, committed);
    if (modeled) plan = apply_clock_ceiling_merge_modeled(plan, *modeled,
        g && g->nvml.setGpuLockedClocks && g->nvml.resetGpuLockedClocks);
    t.plan = plan;
    if (!plan.arm) {
        if (!plan.required && !installedGuardRequired) return true;
        // No entry points at all is a fact about the driver, not about this
        // attempt.  A plan with no defensible ceiling value is neither, and
        // stays UNAVAILABLE so it keeps refusing.
        t.armResult = plan.clampControlAbsent
            ? APPLY_CEILING_ARM_UNSUPPORTED
            : APPLY_CEILING_ARM_UNAVAILABLE;
        const bool refuse = apply_clock_ceiling_transition_must_refuse(
            plan.required, t.armResult, plan.reason, installedGuardRequired);
        // Two different facts reach this branch and they need different words:
        // missing entry points really are "no control on this driver", while a
        // plan with no defensible ceiling value has everything it needs except
        // a number, and saying otherwise would send the next reader hunting for
        // a driver problem that is not there.
        lb_log("apply: transition clock ceiling at %u MHz is REQUIRED (%s) but"
               " %s (%s); %s\n",
               plan.ceilingMHz, apply_clock_ceiling_reason_name(plan.reason),
               plan.clampControlAbsent
                   ? "this driver exposes no locked-clock entry points at all"
                   : "no defensible ceiling value could be derived",
               apply_clock_ceiling_arm_result_name(t.armResult),
               refuse ? "refusing the transition before any write"
                      : "PROCEEDING UNPROTECTED -- the final state needs no"
                        " NVML hard pin and this driver has no clamp to install");
        return !refuse;
    }
    t.writeAttempted = true;
    // Every permitted form has to answer NOT_SUPPORTED before the attempt
    // counts as unsupported: one form unsupported while another is merely
    // declined is still a clamp this request could have installed.
    bool openNotSupported = false, symmetricNotSupported = false;
    int armRc = NVML_SUCCESS;
    // Ceiling, not pin: a 0 minimum caps without forcing the clock up at idle
    // (what `nvidia-smi --lock-gpu-clocks=0,N` asks for).  A driver that refuses
    // it still accepts the symmetric form, which caps correctly -- but that
    // form adds a FLOOR, so it is only used where the plan permits it.
    armRc = g->nvml.setGpuLockedClocks(g->nvmlDevice, 0, plan.ceilingMHz);
    if (armRc == NVML_SUCCESS) {
        t.armed = true;
        g->retainedTransitionCeilingMHz = plan.ceilingMHz;
        t.armResult = APPLY_CEILING_ARM_INSTALLED;
        lb_log("apply: transition clock ceiling armed 0..%u MHz before the"
               " reset/curve writes\n", plan.ceilingMHz);
        return true;
    }
    openNotSupported = (armRc == NVML_ERROR_NOT_SUPPORTED);
    if (plan.symmetricFallbackAllowed) {
        armRc = g->nvml.setGpuLockedClocks(g->nvmlDevice, plan.ceilingMHz,
                                           plan.ceilingMHz);
        if (armRc == NVML_SUCCESS) {
            t.armed = true;
            g->retainedTransitionCeilingMHz = plan.ceilingMHz;
            t.armResult = APPLY_CEILING_ARM_INSTALLED;
            lb_log("apply: transition clock ceiling armed %u..%u MHz before the"
                   " reset/curve writes\n", plan.ceilingMHz, plan.ceilingMHz);
            return true;
        }
        symmetricNotSupported = (armRc == NVML_ERROR_NOT_SUPPORTED);
    }
    // A clamp form the driver does not support is not the same as one it
    // declined, and only the former may let a lock-less request through.  Note
    // what this does NOT establish: for a lock-less request only the open-ended
    // form is permitted (the symmetric one would add a clock floor nobody asked
    // for), so this can be true of a driver that does have locked-clock control
    // and rejects a 0 minimum.  The decision is the same -- nothing installable
    // exists for this request -- but the log below says only that.  See
    // apply_clock_ceiling_policy.h.
    const bool unsupported = openNotSupported &&
        (!plan.symmetricFallbackAllowed || symmetricNotSupported);
    t.armResult = unsupported ? APPLY_CEILING_ARM_UNSUPPORTED
                                          : APPLY_CEILING_ARM_REFUSED;
    const bool refuse = apply_clock_ceiling_transition_must_refuse(
        plan.required, t.armResult, plan.reason, installedGuardRequired);
    lb_log("apply: COULD NOT ARM transition clock ceiling at %u MHz (symmetric"
           " fallback %s, armResult=%s); protection was %s\n",
           plan.ceilingMHz,
           plan.symmetricFallbackAllowed ? "also refused" : "not permitted here",
           apply_clock_ceiling_arm_result_name(t.armResult),
           !plan.required ? "optional -- continuing"
                          : (refuse ? "REQUIRED -- refusing the transition before"
                                      " the reset and curve writes"
                                    : "REQUIRED, but the driver answered"
                                      " NOT_SUPPORTED for"
                                      " every clamp form this request may use"
                                      " -- PROCEEDING UNPROTECTED, the final"
                                      " state needs no NVML hard pin"));
    return !refuse;
}

// Install or tighten a post-reset modeled transition ceiling before scalar/VF
// writes. Never loosen an already-armed tighter ceiling; on failure while
// armed, preserve existing ceiling state so rollback stays protected.
static bool linux_apply_arm_modeled_post_reset(LinuxGpuState* g, LinuxApplyCeilingTransaction& t,
    const DesiredSettings* d,
    const ApplyClockCeilingPlan& modeled, char* why, size_t size) {
    if (!modeled.required && !modeled.arm) return true;
    if (!g || !d) {
        if (why && size) snprintf(why, size, "%s", "modeled transition ceiling requires GPU and desired settings");
        return false;
    }
    const bool cap = g->nvml.setGpuLockedClocks != nullptr &&
        g->nvml.resetGpuLockedClocks != nullptr;
    const ApplyClockCeilingPlan merged = apply_clock_ceiling_merge_modeled(
        t.plan, modeled, cap);
    if (!merged.arm) {
        if (!t.armed) {
            t.plan = merged;
            t.armResult = merged.clampControlAbsent
                ? APPLY_CEILING_ARM_UNSUPPORTED
                : APPLY_CEILING_ARM_UNAVAILABLE;
        }
        if (why && size)
            snprintf(why, size, "Post-reset modeled transition clock ceiling is unavailable (%s)",
                apply_clock_ceiling_arm_result_name(t.armResult));
        lb_log("apply: post-reset modeled ceiling unavailable (armResult=%s)\n",
            apply_clock_ceiling_arm_result_name(t.armResult));
        return false;
    }
    if (t.armed && t.plan.ceilingMHz > 0 &&
        t.plan.ceilingMHz <= merged.ceilingMHz) {
        t.plan.required = merged.required;
        t.plan.finalPinIsCeiling = merged.finalPinIsCeiling;
        lb_log("apply: post-reset modeled cap (%u MHz) covered by armed %u MHz\n",
            merged.ceilingMHz, t.plan.ceilingMHz);
        return true;
    }
    t.writeAttempted = true;
    bool openNotSupported = false, symmetricNotSupported = false;
    int armRc = g->nvml.setGpuLockedClocks(g->nvmlDevice, 0, merged.ceilingMHz);
    if (armRc == NVML_SUCCESS) {
        t.plan = merged;
        t.armed = true;
        g->retainedTransitionCeilingMHz = merged.ceilingMHz;
        t.armResult = APPLY_CEILING_ARM_INSTALLED;
        lb_log("apply: post-reset modeled transition ceiling armed 0..%u MHz\n", merged.ceilingMHz);
        return true;
    }
    openNotSupported = (armRc == NVML_ERROR_NOT_SUPPORTED);
    if (merged.symmetricFallbackAllowed) {
        armRc = g->nvml.setGpuLockedClocks(g->nvmlDevice, merged.ceilingMHz, merged.ceilingMHz);
        if (armRc == NVML_SUCCESS) {
            t.plan = merged;
            t.armed = true;
            g->retainedTransitionCeilingMHz = merged.ceilingMHz;
            t.armResult = APPLY_CEILING_ARM_INSTALLED;
            lb_log("apply: post-reset modeled transition ceiling armed %u..%u MHz\n",
                merged.ceilingMHz, merged.ceilingMHz);
            return true;
        }
        symmetricNotSupported = (armRc == NVML_ERROR_NOT_SUPPORTED);
    }
    const bool unsupported = openNotSupported &&
        (!merged.symmetricFallbackAllowed || symmetricNotSupported);
    if (!t.armed) t.plan = merged;
    t.armResult = unsupported ? APPLY_CEILING_ARM_UNSUPPORTED : APPLY_CEILING_ARM_REFUSED;
    if (why && size)
        snprintf(why, size, "Post-reset modeled transition clock ceiling at %u MHz could not be armed (%s)",
            merged.ceilingMHz, apply_clock_ceiling_arm_result_name(t.armResult));
    lb_log("apply: post-reset modeled ceiling at %u MHz failed (armResult=%s, retainedArmed=%d)\n",
        merged.ceilingMHz, apply_clock_ceiling_arm_result_name(t.armResult),
        t.armed ? 1 : 0);
    return false;
}

// The Linux counterpart of the Windows apply-clock witness: one line recording
// the measured GPU clock next to the ceiling that is supposed to be capping it,
// plus the live load. Without the load, a crash report that only reproduces
// under 3D work cannot be told apart from an idle run that proves nothing.
// Sampled synchronously between phases -- never from another thread, which
// would add a concurrent NVML reader alongside an in-flight VF write.
static void linux_apply_log_clock_witness(LinuxGpuState* g,
                                          const LinuxApplyCeilingTransaction& t,
                                          const DesiredSettings* d,
                                          const char* stage, bool finalState = false) {
    if (!g || !stage) return;
    // The plan as it was ARMED, not a fresh one: the phases between arming and
    // this sample have already changed the live state the plan is derived from,
    // so recomputing would print a ceiling that never existed.
    ApplyClockCeilingPlan ceiling = t.plan;
    bool armed = t.armed;
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
static bool linux_clock_control_proven_absent(const LinuxGpuState* g,
                                              const LinuxApplyCeilingTransaction& t) {
    return g && apply_clock_control_proven_absent(
        g->family == GPU_FAMILY_PASCAL,
        t.armed || g->retainedTransitionCeilingMHz != 0,
        t.outgoingHadHardPin);
}

static bool linux_apply_write_final_lock(LinuxGpuState* g,
                                         const LinuxApplyCeilingTransaction& t,
                                         const DesiredSettings* d) {
    bool lockOk;
    if (d->lockMode == LOCK_MODE_HARD && d->lockMHz > 0)
        lockOk = g->nvml.setGpuLockedClocks &&
                 g->nvml.setGpuLockedClocks(g->nvmlDevice, d->lockMHz,
                                            d->lockMHz) == NVML_SUCCESS;
    else if (linux_clock_control_proven_absent(g, t)) {
        lb_log("apply: final NVML locked-clock reset is inapplicable on Pascal;"
               " no hard pin or retained cap exists\n");
        lockOk = true;
    } else
        lockOk = g->nvml.resetGpuLockedClocks &&
                 g->nvml.resetGpuLockedClocks(g->nvmlDevice) == NVML_SUCCESS;
    if (lockOk) {
        g->retainedTransitionCeilingMHz = 0;
        // Post-handoff clocks obey the final state, not the old lower cap.
        linux_apply_log_clock_witness(g, t, d, "post-lock final state", true);
    } else {
        linux_apply_log_clock_witness(g, t, d, "post-lock failure");
    }
    return lockOk;
}

// What LINUX_MUTATION_RESET_BASELINE does with the locked-clock domain.
// Releasing it there would undo the ceiling the phase before it just armed and
// re-open the exact window F-APPLY-CEILING closes -- which is precisely what
// the pre-fix code did, unconditionally, immediately before writing the new
// curve.  With no ceiling armed this still clears any stale pin, as before.
static bool linux_apply_reset_baseline_locked_clocks(LinuxGpuState* g,
                                                     const LinuxApplyCeilingTransaction& t,
                                                     const DesiredSettings* d) {
    (void)d;
    // Keyed off the clamp that is ACTUALLY installed, not off a freshly
    // recomputed plan.  Recomputing here would consult a live state the arming
    // phase has already begun changing, and would keep the pin standing on the
    // strength of an intention rather than a fact.
    if (t.armed) {
        lb_log("apply: reset baseline keeps the %u MHz transition ceiling\n",
               t.plan.ceilingMHz);
        return true;
    }
    if (linux_clock_control_proven_absent(g, t)) {
        lb_log("apply: baseline NVML locked-clock reset is inapplicable on Pascal\n");
        return true;
    }
    return g->nvml.resetGpuLockedClocks &&
           g->nvml.resetGpuLockedClocks(g->nvmlDevice) == NVML_SUCCESS;
}

#endif
