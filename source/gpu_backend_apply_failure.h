// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_GPU_BACKEND_APPLY_FAILURE_H
#define GREEN_CURVE_GPU_BACKEND_APPLY_FAILURE_H

#include "apply_recovery_text.h"

// An unverified memory or power write is repaired only by a request that owns
// that domain (or Reset). Any other Apply would succeed while the GPU keeps a
// value nobody proved, which recovery already told the user to Reset first.
static bool apply_write_uncertainty_precheck(const DesiredSettings* desired,
    char* result, size_t resultSize) {
    if (!desired) return true;
    const bool power = g_app.powerLimitWriteUncertain && !desired->hasPowerLimit;
    const bool memory = g_app.memOffsetWriteUncertain && !desired->hasMemOffset;
    if (!power && !memory) return true;
    set_message(result, resultSize, "%s",
        power && memory
            ? "Previous memory-offset and power-limit writes are still unverified."
              " Run Reset before applying settings that do not repair them."
            : memory
                ? "A previous memory-offset write is still unverified. Run Reset"
                  " before applying settings that do not repair the memory offset."
                : "A previous power-limit write is still unverified. Run Reset before"
                  " applying settings that do not repair the power limit.");
    set_last_apply_phase(power && memory
        ? "apply: refused (unverified memory offset and power limit)"
        : memory ? "apply: refused (unverified memory offset)"
                 : "apply: refused (unverified power limit)");
    debug_log("apply: refused before writes; unverified memory=%d power=%d"
              " (request owns memory=%d power=%d)\n",
        g_app.memOffsetWriteUncertain ? 1 : 0,
        g_app.powerLimitWriteUncertain ? 1 : 0,
        desired->hasMemOffset ? 1 : 0, desired->hasPowerLimit ? 1 : 0);
    return false;
}

// All prerequisite failures leave normal finalization immediately. Recovery
// owns the only permitted release; scope destruction cannot release again.
static bool apply_recover_clock_failure(ApplyClockCeilingGuard& ceiling,
    const char* reason, char* result, size_t resultSize) {
    char failure[512] = {};
    StringCchCopyA(failure, ARRAY_COUNT(failure), reason);
    // Finalization may have released/raised the cap, even if its setter failed
    // after partial side effects. Re-establish protection before recovery.
    if (ceiling.plan.required) {
        ceiling.armed = false;
        ceiling.arm();
        if (ceiling.must_refuse_transition()) {
            set_message(result, resultSize, "%s. Recovery stopped: clock protection could not be established", failure);
            return false;
        }
    }
    ceiling.retain("clock prerequisite failed; recovering under protection");
    ApplyRecoveryResult recovery = rollback_to_safe_defaults();
    if (recovery.restrictionReleased) ceiling.adopt("verified recovery released cap");
    invalidate_scalar_readbacks(&g_app.readback);
    char refreshDetail[128] = {};
    refresh_global_state(refreshDetail, sizeof(refreshDetail));
    // GC_APPLY_RECOVERY_VERIFIED_TEXT is matched by the service to tell the
    // user the clocks are back at stock (auto_apply_failure_notice_policy.h).
    // Both outcomes are reported.  An unverified memory offset withholds the
    // verified marker (the clocks are not all back at stock), but must not
    // hide whether core protection is still holding.
    const bool coreReleased =
        apply_recovery_permits_release(recovery) && recovery.restrictionReleased;
    const bool memoryUncertain =
        apply_recovery_domain_is_uncertain(recovery.memory);
    const bool powerUncertain =
        apply_recovery_domain_is_uncertain(recovery.power);
    const char* recoveryText = nullptr;
    if (memoryUncertain || powerUncertain) {
        if (coreReleased) {
            recoveryText = memoryUncertain && powerUncertain
                ? "Recovery restored core clocks but could not verify memory or"
                  " power defaults; run Reset before applying again"
                : memoryUncertain
                    ? "Recovery restored core clocks but could not verify memory"
                      " defaults; run Reset before applying again"
                    : "Recovery restored core clocks but could not verify the"
                      " power limit; run Reset before applying again";
        } else {
            recoveryText = memoryUncertain && powerUncertain
                ? "Recovery could not verify core, memory or power defaults;"
                  " clock protection retained; run Reset before applying again"
                : memoryUncertain
                    ? "Recovery could not verify core or memory defaults; clock"
                      " protection retained; run Reset before applying again"
                    : "Recovery could not verify core clocks or the power limit;"
                      " clock protection retained; run Reset before applying again";
        }
    } else {
        recoveryText = coreReleased
            ? GC_APPLY_RECOVERY_VERIFIED_TEXT
            : "Recovery could not verify core defaults; clock protection retained";
    }
    set_message(result, resultSize, "%s. %s", failure, recoveryText);
    debug_log("apply prerequisite failed: %s; recovery scalar=%d curve=%d released=%d"
              " memoryAttempted=%d memoryVerified=%d powerAttempted=%d"
              " powerVerified=%d\n",
        failure, recovery.gpuOffset.verified, recovery.curve.verified,
        recovery.restrictionReleased, recovery.memory.attempted,
        recovery.memory.verified, recovery.power.attempted,
        recovery.power.verified);
    return false;
}
#endif
