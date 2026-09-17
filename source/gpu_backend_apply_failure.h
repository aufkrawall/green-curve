// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_GPU_BACKEND_APPLY_FAILURE_H
#define GREEN_CURVE_GPU_BACKEND_APPLY_FAILURE_H

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
    set_message(result, resultSize, "%s. Recovery %s", failure,
        apply_recovery_permits_release(recovery) && recovery.restrictionReleased
            ? "verified core defaults" : "could not verify core defaults; clock protection retained");
    debug_log("apply prerequisite failed: %s; recovery scalar=%d curve=%d released=%d\n",
        failure, recovery.gpuOffset.verified, recovery.curve.verified,
        recovery.restrictionReleased);
    return false;
}
#endif
