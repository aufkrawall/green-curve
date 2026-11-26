// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included by clock_transition_tests.cpp after the production Linux ceiling,
// rollback and baseline shards (assertion codes 14220-14227).
//
// F-APPLY-CEILING state is one explicit LinuxApplyCeilingTransaction per
// apply/reset. Two transactions -- here on two GPUs sharing one fake driver --
// must never observe each other's clamp. With the former file-scope state,
// GPU B's baseline kept GPU A's ceiling instead of releasing its own stale pin
// (14222) and B's rollback armed A's bound (14224).
static int linux_ceiling_transaction_cases() {
    LinuxGpuState a{}, b{};
    a.nvml.setGpuLockedClocks = b.nvml.setGpuLockedClocks = setCap;
    a.nvml.resetGpuLockedClocks = b.nvml.resetGpuLockedClocks = resetCap;
    a.curve[0].freq_kHz = b.curve[0].freq_kHz = 3600000;
    DesiredSettings old{}, next{};
    old.hasLock = next.hasLock = true;
    old.lockMode = next.lockMode = LOCK_MODE_HARD;
    old.lockMHz = 2500; next.lockMHz = 3000; next.resetOcBeforeApply = true;
    refuseArm = refuseReset = false; resetCap(0);

    LinuxApplyCeilingTransaction ta = {}, tb = {};
    linux_apply_ceiling_begin(ta, &old);
    ta.outgoingCeilingMHz = linux_outgoing_ceiling_mhz(&a, &old);
    if (!linux_apply_arm_transition_ceiling(&a, ta, &next, &old) || !ta.armed ||
        ta.armResult != APPLY_CEILING_ARM_INSTALLED || cap != 2500) return 14220;
    linux_apply_ceiling_begin(tb, nullptr);
    if (tb.armed || tb.plan.ceilingMHz || tb.outgoingHadHardPin ||
        tb.armResult != APPLY_CEILING_ARM_NOT_NEEDED) return 14221;

    // B never armed: its baseline releases B's locked clocks instead of
    // keeping the clamp that belongs to A's transaction.
    unsigned int before = resets;
    if (!linux_apply_reset_baseline_locked_clocks(&b, tb, &next) || resets != before + 1) return 14222;
    resetCap(0); cap = 2500;  // A's clamp is still physically installed.
    before = resets;
    if (!linux_apply_reset_baseline_locked_clocks(&a, ta, &next) || resets != before || cap != 2500) return 14223;

    // A rollback of B's clock phases has no bound of its own, so it refuses
    // rather than installing A's 2500 MHz ceiling on B.
    LinuxHardwareSnapshot snapshot{}; snapshot.valid = snapshot.curveValid = true;
    char err[160]{};
    before = armCalls;
    if (linux_backend_restore_snapshot(&b, tb, &snapshot, LINUX_MUTATION_LOCK, err, sizeof(err)) ||
        armCalls != before || tb.armed || b.retainedTransitionCeilingMHz) return 14224;
    // A's own rollback re-establishes protection at A's bound.
    before = armCalls;
    (void)linux_backend_restore_snapshot(&a, ta, &snapshot, LINUX_MUTATION_LOCK, err, sizeof(err));
    if (armCalls != before + 1 || cap != 2500 || a.retainedTransitionCeilingMHz != 2500) return 14225;

    // Reusing a value for a new transaction starts clean and records only the
    // new outgoing state.
    linux_apply_ceiling_begin(ta, &old);
    if (ta.armed || ta.writeAttempted || ta.plan.ceilingMHz || ta.outgoingCeilingMHz ||
        ta.armResult != APPLY_CEILING_ARM_NOT_NEEDED || !ta.outgoingHadHardPin) return 14226;
    linux_apply_ceiling_begin(ta, nullptr);
    if (ta.outgoingHadHardPin || linux_snapshot_curve_needs_a_pin(ta, &snapshot)) return 14227;
    resetCap(0);
    return 0;
}
