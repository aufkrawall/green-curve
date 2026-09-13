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
static ApplyClockCeilingPlan linux_apply_clock_ceiling_plan(
    LinuxGpuState* g, const DesiredSettings* d) {
    if (!g || !d) return ApplyClockCeilingPlan{};
    return apply_clock_ceiling_plan(
        service_request_replaces_lock_domain(d), d->hasLock != 0,
        (int)d->lockMode, d->lockMHz,
        g->nvml.setGpuLockedClocks != nullptr &&
            g->nvml.resetGpuLockedClocks != nullptr);
}

// The LINUX_MUTATION_LOCK_CEILING phase body.  Always reports success: this is
// strictly an added safety net, and failing the whole apply because the net
// could not be hung would be worse than the pre-guard behaviour it replaces.
// The failure is logged at full volume instead, because it is the single most
// useful line in the journal if the driver falls over during a profile switch.
static bool linux_apply_arm_transition_ceiling(LinuxGpuState* g,
                                               const DesiredSettings* d) {
    ApplyClockCeilingPlan plan = linux_apply_clock_ceiling_plan(g, d);
    if (!plan.arm) return true;
    // Ceiling, not pin: a 0 minimum caps without forcing the clock up at idle
    // (what `nvidia-smi --lock-gpu-clocks=0,N` asks for).  A driver that refuses
    // it still accepts the symmetric form, which caps correctly.
    if (g->nvml.setGpuLockedClocks(g->nvmlDevice, 0, plan.ceilingMHz) ==
        NVML_SUCCESS) {
        lb_log("apply: transition clock ceiling armed 0..%u MHz before the"
               " reset/curve writes\n", plan.ceilingMHz);
        return true;
    }
    if (g->nvml.setGpuLockedClocks(g->nvmlDevice, plan.ceilingMHz,
                                   plan.ceilingMHz) == NVML_SUCCESS) {
        lb_log("apply: transition clock ceiling armed %u..%u MHz before the"
               " reset/curve writes\n", plan.ceilingMHz, plan.ceilingMHz);
        return true;
    }
    lb_log("apply: COULD NOT ARM transition clock ceiling at %u MHz; the curve"
           " write below runs uncapped until the lock phase\n", plan.ceilingMHz);
    return true;
}

// The Linux counterpart of the Windows apply-clock witness: one line recording
// the measured GPU clock next to the ceiling that is supposed to be capping it,
// plus the live load. Without the load, a crash report that only reproduces
// under 3D work cannot be told apart from an idle run that proves nothing.
// Sampled synchronously between phases -- never from another thread, which
// would add a concurrent NVML reader alongside an in-flight VF write.
static void linux_apply_log_clock_witness(LinuxGpuState* g,
                                          const DesiredSettings* d,
                                          const char* stage) {
    if (!g || !stage) return;
    ApplyClockCeilingPlan ceiling = linux_apply_clock_ceiling_plan(g, d);
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
    // disagree about what "the clamp held" means.  `arm` is both requested and
    // (best-effort) armed here: the phase never fails, and a refusal is already
    // logged loudly by linux_apply_arm_transition_ceiling().
    const char* verdict = apply_clock_witness_verdict_name(
        apply_clock_witness_verdict(ceiling.arm, ceiling.arm, ceiling.ceilingMHz,
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
    linux_apply_log_clock_witness(g, d, "post-lock");
    return lockOk;
}

// What LINUX_MUTATION_RESET_BASELINE does with the locked-clock domain.
// Releasing it there would undo the ceiling the phase before it just armed and
// re-open the exact window F-APPLY-CEILING closes -- which is precisely what
// the pre-fix code did, unconditionally, immediately before writing the new
// curve.  With no ceiling armed this still clears any stale pin, as before.
static bool linux_apply_reset_baseline_locked_clocks(LinuxGpuState* g,
                                                     const DesiredSettings* d) {
    ApplyClockCeilingPlan ceiling = linux_apply_clock_ceiling_plan(g, d);
    if (ceiling.arm) {
        lb_log("apply: reset baseline keeps the %u MHz transition ceiling\n",
               ceiling.ceilingMHz);
        return true;
    }
    return g->nvml.resetGpuLockedClocks &&
           g->nvml.resetGpuLockedClocks(g->nvmlDevice) == NVML_SUCCESS;
}

#endif
