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
    if (baseline || (phaseMask & LINUX_MUTATION_LOCK) ||
        (phaseMask & LINUX_MUTATION_LOCK_CEILING)) {
        // NVML exposes no getter for the configured locked-clock range, so the
        // pre-transaction lock policy cannot be restored exactly and this
        // rollback is always reported as uncertain (`ok = false`).
        //
        // CT-07.  What it must NOT do is release the restriction regardless.
        // The pre-fix code called resetGpuLockedClocks() unconditionally on the
        // argument that a clamp left standing over restored offsets is an
        // invisible cap.  That argument holds only when the restore SUCCEEDED.
        // When it did not -- or when the state being restored is an outgoing
        // HARD profile whose raw VF tail is safe only because of its pin --
        // releasing hands the GPU a raised curve with nothing holding it down.
        // A cap the user can see and reset is strictly better than a TDR.
        const bool curveRestoreUncertain =
            (phaseMask & LINUX_MUTATION_CURVE) && snapshot->curveValid && !ok;
        const bool restoringPinnedCurve =
            snapshot->curveValid && (phaseMask & LINUX_MUTATION_CURVE) &&
            linux_snapshot_curve_needs_a_pin(snapshot);
        if (curveRestoreUncertain || restoringPinnedCurve) {
            lb_log("rollback: KEEPING the locked-clock restriction -- restore"
                   " uncertain=%d, restored curve needs a pin=%d. Releasing it"
                   " would uncap the curve this rollback just wrote back\n",
                   curveRestoreUncertain ? 1 : 0, restoringPinnedCurve ? 1 : 0);
        } else if (g->nvml.resetGpuLockedClocks) {
            g->nvml.resetGpuLockedClocks(g->nvmlDevice);
        }
        ok = false;
    }
    if (!ok) gc_strlcpy(err, errSize, "one or more GPU rollback phases failed");
    linux_backend_refresh(g);
    return ok;
}

#endif  // GREEN_CURVE_LINUX_BACKEND_ROLLBACK_H
