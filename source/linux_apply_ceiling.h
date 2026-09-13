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
