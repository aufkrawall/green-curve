// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Linux rollback: restoring the hardware snapshot a failed transaction
// captured, and the one decision that makes it safe -- whether the clock
// restriction holding the restored curve down may be released.
//
// Split out of linux_backend_mutation.cpp for the file-size guidance and
// included from the exact position it occupied, so definition order in the
// amalgamated translation unit is unchanged.
#ifndef GREEN_CURVE_LINUX_BACKEND_ROLLBACK_H
#define GREEN_CURVE_LINUX_BACKEND_ROLLBACK_H

bool linux_backend_restore_snapshot(LinuxGpuState* g, const LinuxHardwareSnapshot* snapshot,
                                    unsigned int phaseMask, char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!g || !snapshot || !snapshot->valid) {
        gc_strlcpy(err, errSize, "rollback snapshot is invalid");
        return false;
    }
    const unsigned int clockPhases = LINUX_MUTATION_RESET_BASELINE |
        LINUX_MUTATION_GPU_OFFSET | LINUX_MUTATION_CURVE |
        LINUX_MUTATION_LOCK | LINUX_MUTATION_LOCK_CEILING;
    const bool noClockControl = linux_clock_control_proven_absent(g);
    if (phaseMask & clockPhases) {
        unsigned int bound = g_linuxOutgoingCeilingMHz;
        if (g_linuxCeilingPlan.ceilingMHz > 0 &&
            (bound == 0 || g_linuxCeilingPlan.ceilingMHz < bound))
            bound = g_linuxCeilingPlan.ceilingMHz;
        // A rejected first arm must leave the old restriction alone. Failed
        // writes may have side effects, so require protection before restoring
        // whenever this GPU has a locked-clock control. Pascal cannot install
        // one; restore and verify its prior snapshot without inventing a pin.
        bool protectedRestore = !noClockControl && bound > 0 &&
            g->nvml.setGpuLockedClocks &&
            g->nvml.setGpuLockedClocks(g->nvmlDevice, 0, bound) == NVML_SUCCESS;
        if (!protectedRestore && bound > 0 && g_linuxOutgoingHadHardPin &&
            g->nvml.setGpuLockedClocks)
            protectedRestore = g->nvml.setGpuLockedClocks(
                g->nvmlDevice, bound, bound) == NVML_SUCCESS;
        if (!protectedRestore && !noClockControl) {
            gc_strlcpy(err, errSize, "Rollback cannot establish clock protection; existing restriction preserved");
            lb_log("rollback: refusing clock restore without protection at %u MHz\n", bound);
            return false;
        }
        if (protectedRestore) {
            g_linuxCeilingArmed = true;
            g->retainedTransitionCeilingMHz = bound;
            lb_log("rollback: clock protection established at %u MHz before restore\n", bound);
        } else {
            lb_log("rollback: Pascal exposes no NVML clock clamp; restoring"
                   " the previous snapshot without a transition pin\n");
        }
    }
    bool ok = true;
    bool baseline = (phaseMask & LINUX_MUTATION_RESET_BASELINE) != 0;
    if ((baseline || (phaseMask & LINUX_MUTATION_GPU_OFFSET)) && snapshot->gpuOffsetValid)
        ok &= nvml_set_clock_offset(g, NVML_CLOCK_GRAPHICS, snapshot->gpuOffsetMHz);
    if ((baseline || (phaseMask & LINUX_MUTATION_MEM_OFFSET)) && snapshot->memOffsetValid) {
        // Snapshot is display MHz; the NVML wire unit is effective MHz.
        int restoreEffectiveMHz =
            nvml_mem_effective_mhz_from_display_mhz(snapshot->memOffsetMHz);
        lb_log("offset: rollback domain=%u display=%d effective=%d\n",
               (unsigned int)NVML_CLOCK_MEM, snapshot->memOffsetMHz,
               restoreEffectiveMHz);
        ok &= nvml_set_clock_offset(g, NVML_CLOCK_MEM, restoreEffectiveMHz);
    }
    if (((baseline && (snapshot->availableMutationDomains &
                       SERVICE_MUTATION_DOMAIN_XBAR)) ||
         (phaseMask & LINUX_MUTATION_XBAR)) &&
        snapshot->xbarValid) {
        ok &= linux_xbar_write_owned(g, snapshot->xbarOffsetKhz,
                                     snapshot->xbarMsvddOffsetUv,
                                     true, true);
    }
    if (((baseline && (snapshot->availableMutationDomains &
                       SERVICE_MUTATION_DOMAIN_SYS_CLK)) ||
         (phaseMask & LINUX_MUTATION_SYS_CLK)) &&
        snapshot->sysClkValid) {
        ok &= linux_xbar_write_entry(g, XBAR_PINNED_SYS_ENTRY_INDEX,
                                     snapshot->sysClkOffsetKhz);
    }
    if (((baseline && (snapshot->availableMutationDomains &
                       SERVICE_MUTATION_DOMAIN_VIDEO_CLK)) ||
         (phaseMask & LINUX_MUTATION_VIDEO_CLK)) &&
        snapshot->videoClkValid) {
        ok &= linux_xbar_write_entry(g, XBAR_PINNED_VIDEO_ENTRY_INDEX,
                                     snapshot->videoClkOffsetKhz);
    }
    if ((phaseMask & LINUX_MUTATION_POWER) && snapshot->powerValid && g->nvml.setPowerLimit) {
        bool powerOk = g->nvml.setPowerLimit(g->nvmlDevice, snapshot->powerLimitmW) == NVML_SUCCESS;
        if (powerOk) {
            unsigned int currentmW = 0;
            unsigned int defaultmW = 0;
            powerOk = linux_read_power_limit_pair(g, &currentmW, &defaultmW) &&
                      currentmW == snapshot->powerLimitmW;
            g->powerLimitCurrentmW = currentmW > 0 ? (int)currentmW : 0;
            g->powerLimitDefaultmW = defaultmW > 0 ? (int)defaultmW : 0;
        }
        ok &= powerOk;
    }
    if ((phaseMask & LINUX_MUTATION_CURVE) && snapshot->curveValid)
        ok &= apply_curve_offsets_verified(g, snapshot->curveOffsets, snapshot->curveMask, 25);
    if ((phaseMask & LINUX_MUTATION_FAN) && snapshot->fanValid) {
        for (unsigned int i = 0; i < snapshot->fanCount; ++i) {
            bool fanOk = false;
            if (snapshot->fanPolicy[i] == NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW) {
                if (g->nvml.setDefaultFanSpeed)
                    fanOk = g->nvml.setDefaultFanSpeed(g->nvmlDevice, i) == NVML_SUCCESS;
                else if (g->nvml.setFanControlPolicy)
                    fanOk = g->nvml.setFanControlPolicy(g->nvmlDevice, i,
                        NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW) == NVML_SUCCESS;
            } else if (g->nvml.setFanSpeed) {
                fanOk = g->nvml.setFanSpeed(g->nvmlDevice, i, snapshot->fanTargetPercent[i]) == NVML_SUCCESS;
            }
            if (fanOk && snapshot->fanPolicy[i] == NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW) {
                unsigned int verifyPolicy = 0;
                fanOk = g->nvml.getFanControlPolicy &&
                        g->nvml.getFanControlPolicy(g->nvmlDevice, i, &verifyPolicy) == NVML_SUCCESS &&
                        verifyPolicy == NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW;
            } else if (fanOk) {
                // Same trap as the forward write: the measured duty is not a
                // readback.  Confirm the restored *intent* instead.
                int intended = 0;
                bool intendedKnown = nvml_read_fan_intent(g, i, &intended);
                int measured = nvml_read_fan_measured(g, i);
                fanOk = fan_manual_write_confirmed((int)snapshot->fanTargetPercent[i],
                    measured < 0 ? 0 : measured, intended, intendedKnown);
                if (!fanOk) {
                    lb_log("fan: rollback readback mismatch for fan %u "
                           "(want=%u intent=%s%d measured=%d)\n",
                           i, snapshot->fanTargetPercent[i],
                           intendedKnown ? "" : "unknown:", intended, measured);
                }
            }
            if (!fanOk) lb_log("fan: rollback verification failed for fan %u\n", i);
            ok &= fanOk;
        }
    }
    if ((phaseMask & clockPhases) && !noClockControl) {
        // A restored HARD curve needs its restriction even when failure happened
        // before the CURVE phase. Keep protection on all uncertain rollbacks;
        // the caller already reports rollback uncertainty to the user.
        lb_log("rollback: retaining clock protection (outgoing hard=%d, restore ok=%d)\n",
            linux_snapshot_curve_needs_a_pin(snapshot), ok);
        ok = false;
    }
    if (!ok) gc_strlcpy(err, errSize, "one or more GPU rollback phases failed");
    linux_backend_refresh(g);
    return ok;
}

#endif  // GREEN_CURVE_LINUX_BACKEND_ROLLBACK_H
