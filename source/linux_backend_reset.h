// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included by linux_backend_mutation.cpp after the shared rollback helpers.
#pragma once

struct LinuxResetTransactionContext {
    LinuxGpuState* gpu;
    const LinuxHardwareSnapshot* snapshot;
};

static bool linux_reset_transaction_step(void* opaque, unsigned int phase) {
    LinuxResetTransactionContext* context = (LinuxResetTransactionContext*)opaque;
    LinuxGpuState* g = context->gpu;
    switch (phase) {
        case LINUX_MUTATION_LOCK: {
            bool noControl = linux_clock_control_proven_absent(g);
            bool ok = linux_finalize_named_graphics_reset(g, [&] {
                return noControl || (g->nvml.resetGpuLockedClocks &&
                    g->nvml.resetGpuLockedClocks(g->nvmlDevice) == NVML_SUCCESS);
            });
            if (noControl)
                lb_log("reset: NVML locked-clock domain absent on Pascal;"
                       " continuing with mutable domains\n");
            if (ok) g->retainedTransitionCeilingMHz = 0;
            return ok;
        }
        case LINUX_MUTATION_GPU_OFFSET:
            if (g->graphicsRecovery.covered) return linux_reset_named_graphics(g, false);
            return nvml_set_clock_offset(g, NVML_CLOCK_GRAPHICS, 0);
        case LINUX_MUTATION_MEM_OFFSET:
            return nvml_set_clock_offset(g, NVML_CLOCK_MEM, 0);
        case LINUX_MUTATION_POWER: {
            unsigned int expectedDefaultmW = g->powerLimitDefaultmW > 0
                ? (unsigned int)g->powerLimitDefaultmW : 0;
            bool ok = expectedDefaultmW > 0 && g->nvml.setPowerLimit &&
                g->nvml.setPowerLimit(g->nvmlDevice, expectedDefaultmW) == NVML_SUCCESS;
            unsigned int currentmW = 0;
            unsigned int defaultmW = 0;
            bool pairValid = ok &&
                linux_read_power_limit_pair(g, &currentmW, &defaultmW);
            g->powerLimitCurrentmW = currentmW > 0 ? (int)currentmW : 0;
            g->powerLimitDefaultmW = defaultmW > 0 ? (int)defaultmW : 0;
            return pairValid && currentmW == expectedDefaultmW &&
                   defaultmW == expectedDefaultmW;
        }
        case LINUX_MUTATION_CURVE: {
            int targets[VF_NUM_POINTS] = {};
            gc_i64 targetsHz[VF_NUM_POINTS] = {};
            bool mask[VF_NUM_POINTS] = {};
            (void)linux_vf_baseline_reset_mask(g->curve, nullptr, mask, g->vfMask);
            return apply_curve_offsets_verified(g, targets, mask, 25, targetsHz, mask);
        }
        case LINUX_MUTATION_FAN:
            return nvml_set_fan(g, FAN_MODE_AUTO, true, 0);
        case LINUX_MUTATION_XBAR:
            return linux_xbar_write_owned(g, 0, 0, true, true);
        case LINUX_MUTATION_SYS_CLK:
            return linux_xbar_write_entry(g, XBAR_PINNED_SYS_ENTRY_INDEX, 0);
        case LINUX_MUTATION_VIDEO_CLK:
            return linux_xbar_write_entry(g, XBAR_PINNED_VIDEO_ENTRY_INDEX, 0);
        default:
            return false;
    }
}

static bool linux_reset_transaction_rollback(void* opaque, unsigned int phases) {
    LinuxResetTransactionContext* context = (LinuxResetTransactionContext*)opaque;
    char rollback[256] = {};
    return linux_backend_restore_snapshot(context->gpu, context->snapshot, phases,
                                          rollback, sizeof(rollback));
}

LinuxMutationResult linux_backend_reset(LinuxGpuState* g, const DesiredSettings* previous,
                                        char* result, size_t resultSize) {
    linux_apply_ceiling_reset_state();
    LinuxMutationResult mutation = {};
    LinuxHardwareSnapshot snapshot = {};
    char detail[256] = {};
    linux_backend_refresh(g);
    if (g->graphicsRecovery.covered & ~g->graphicsRecovery.prepared) {
        if (result) gc_strlcpy(result, resultSize, "Reset requires durable preparation for every named graphics control");
        return mutation;
    }
    if (!linux_backend_capture_snapshot(g, &snapshot, detail, sizeof(detail)) ||
        !g->writeIdentityResolved) {
        if (result) gc_strlcpy(result, resultSize, detail[0] ? detail : "Reset preflight failed");
        return mutation;
    }
    if (!linux_graphics_recovery_snapshot_complete(g, &snapshot)) {
        if (result) gc_strlcpy(result, resultSize, "Reset cannot read every named graphics recovery control");
        lb_log("graphics-offset: Reset preimage incomplete covered=%u readable=%u\n",
            g->graphicsRecovery.covered, snapshot.graphicsPreimage.validMask);
        return mutation;
    }
    const bool fanKnownAbsent = snapshot.fanCountReported && snapshot.fanCount == 0;
    if (!linux_reset_preflight_domains_supported(
            snapshot.availableMutationDomains, g->powerLimitDefaultmW, fanKnownAbsent) ||
        !g->backend || !g->backend->writeSupported) {
        gc_strlcpy(detail, sizeof(detail),
                   "Reset preflight failed: every mutable domain must support snapshot and restore");
        if (result) gc_strlcpy(result, resultSize, detail);
        return mutation;
    }
    if (!(snapshot.availableMutationDomains & SERVICE_MUTATION_DOMAIN_POWER)) {
        lb_log("reset: skipping power reset — board exposes no power control surface\n");
    }
    linux_reset_capture_rollback_ceiling(g, previous);
    lb_log("reset: rollback bound=%u MHz outgoingHardPin=%d fanAbsent=%d\n",
        g_linuxOutgoingCeilingMHz, g_linuxOutgoingHadHardPin ? 1 : 0, fanKnownAbsent ? 1 : 0);
    LinuxResetTransactionContext context = {g, &snapshot};
    unsigned int requested = linux_reset_phases_for_available_domains(
        snapshot.availableMutationDomains);
    mutation = linux_execute_transaction(requested, linux_reset_transaction_step,
                                         linux_reset_transaction_rollback, &context);
    if (result) gc_strlcpy(result, resultSize, mutation.success ? "Reset to defaults" :
        mutation.rollbackSucceeded ? "Reset failed and was rolled back" : "Reset failed; rollback uncertain");
    linux_backend_refresh(g);
    return mutation;
}
