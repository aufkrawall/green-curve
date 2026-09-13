"""Source gates for F-APPLY-CEILING -- the transition clock ceiling.

Split out of build.py so the build script stays under its size ratchet; build.py
owns the CLI and passes its paths and check helpers in through `ctx`. Nothing
here imports build.py, so the dependency runs one way only.

Every rule here guards an ORDERING, which is the worst kind of property to lose
silently: the apply still succeeds, the clocks still end up where the profile
asked, the log still says "severity=success", and the only symptom is a driver
that falls over during a profile switch on a machine under load.

The measured incident (2026-09-13 13:43, greencurve_debug.txt + nvlddmkm event
153): profile 3 held its 2957 MHz ceiling with a FLATTEN tail, so no NVML pin
was armed. Switching to profile 4 (HARD pin, same 2957 MHz) reset the curve to
stock, then wrote the new curve with a +475 MHz selective offset all the way to
point 126 -- tail readback 2947..3637 MHz -- and pinned the clock 1.21 s later.
The control case is in the same log: the HARD -> HARD switch a minute earlier
kept the previous pin armed across the identical curve batch and survived.

`ctx` is any object exposing SOURCE_DIR.
"""
import os


def _p(ctx, name):
    return os.path.join(ctx.SOURCE_DIR, name)


def check_all(ctx, require_text, forbid_text):
    policy_h = _p(ctx, "apply_clock_ceiling_policy.h")
    apply_cpp = _p(ctx, "gpu_backend_apply.cpp")
    guard_h = _p(ctx, "gpu_backend_apply_ceiling.h")
    reset_cpp = _p(ctx, "gpu_backend_reset_baseline.cpp")
    transaction_h = _p(ctx, "linux_transaction.h")
    linux_mutation_cpp = _p(ctx, "linux_backend_mutation.cpp")
    linux_ceiling_h = _p(ctx, "linux_apply_ceiling.h")

    front_cpp = _p(ctx, "main_gpu_front.cpp")

    # One decision, shared. Two independent copies would drift, and the drift
    # would be invisible until a platform-specific TDR report.
    require_text(policy_h, "static inline ApplyClockCeilingPlan apply_clock_ceiling_plan(",
                 "the transition-ceiling decision exists exactly once")
    require_text(policy_h, "static inline bool apply_clock_ceiling_release_on_abandon(",
                 "the abandoned-clamp rule is named rather than open-coded")

    # Windows: the guard must exist, must be armed on BOTH paths into the first
    # hardware write, and must be adopted by the final lock step in both modes.
    require_text(guard_h, "struct ApplyClockCeilingGuard",
                 "the Windows apply owns a scope-bound transition clamp")
    require_text(apply_cpp, "ApplyClockCeilingGuard clockCeiling(desired);",
                 "the Windows apply declares the clamp before its first write")
    require_text(apply_cpp, "clockCeiling.arm();\n        if (!reset_oc_before_gui_apply(",
                 "the clamp is armed BEFORE reset-to-stock drops the old ceiling")
    require_text(apply_cpp,
                 "clockCeiling.adopt(\"released with the locked-clock domain\");",
                 "a non-HARD apply hands the clamp to the locked-clock release")
    require_text(apply_cpp,
                 "clockCeiling.adopt(\"re-asserted as the final hard pin\");",
                 "a HARD apply re-asserts the clamp as the authoritative pin")
    require_text(apply_cpp,
                 "clockCeiling.retain(\"final hard pin was refused by NVML\");",
                 "a refused final pin keeps the clamp instead of uncapping the "
                 "curve the apply just raised")
    require_text(guard_h, "apply curve peak after batch:",
                 "the apply logs the peak the written curve reaches")
    require_text(apply_cpp, "apply_log_curve_peak_after_batch(clockCeiling,",
                 "the apply calls the peak diagnostic after its curve batch")

    # The clock witness. Without it a test run can only report "nothing
    # crashed", which the pre-fix code also did most of the time -- and the
    # 2026-09-13 post-fix run turned out to have been idle, which was only
    # discoverable afterwards by inferring load from fan telemetry.
    witness_h = _p(ctx, "apply_clock_witness.h")
    backend_cpp = _p(ctx, "gpu_backend.cpp")
    require_text(policy_h, "static inline ApplyClockWitnessVerdict apply_clock_witness_verdict(",
                 "the held/exceeded verdict is one shared pure rule")
    require_text(policy_h, "static inline bool apply_clock_witness_load_is_meaningful(",
                 "the log states whether the run carried enough load to mean anything")
    require_text(witness_h, "struct ApplyClockWitnessScope",
                 "the witness is scope-bound so no apply exit leaves it armed")
    require_text(witness_h, "apply_clock_witness_verdict(",
                 "the Windows verdict line uses the shared rule")
    require_text(policy_h,
                 "static inline bool apply_clock_witness_counts_toward_verdict(",
                 "which samples the verdict may judge is a named rule")
    require_text(witness_h, "apply_clock_witness_counts_toward_verdict(w->clampArmed)",
                 "a sample taken before the clamp was armed cannot be judged "
                 "against a ceiling that was not yet in force")
    require_text(apply_cpp, "ApplyClockWitnessScope clockWitness(",
                 "the apply opens a clock witness for its whole duration")
    require_text(apply_cpp, 'apply_clock_witness_record("post-curve-batch (pre-lock)");',
                 "the apply samples the live clock at the instant that used to be "
                 "uncapped -- the curve is raised and the lock has not run")
    require_text(backend_cpp, 'apply_clock_witness_poll("curve settle");',
                 "the settle loop samples the middle of the post-curve window, not "
                 "only its two ends")
    # The witness must never grow a sampling thread: a concurrent NVML reader
    # alongside an in-flight NvAPI VF write is the exact class of racy behaviour
    # under investigation here.
    forbid_text(witness_h, "CreateThread",
                "the clock witness never samples from another thread")
    require_text(linux_ceiling_h, "static void linux_apply_log_clock_witness(",
                 "Linux records the same witness line")
    require_text(linux_ceiling_h, "apply_clock_witness_verdict(",
                 "Linux uses the shared verdict rule rather than its own comparison")

    # The sibling transient: power must not bounce through the board default.
    require_text(reset_cpp, "power_reset_before_apply_target_pct(",
                 "reset-before-apply writes the apply's own power target")
    forbid_text(reset_cpp, "nvapi_set_power_limit(POWER_LIMIT_DEFAULT_PCT)",
                "reset-before-apply never routes an owned power target through "
                "the board default first")

    # A rollback returns every other control to stock; a locked-clock clamp left
    # standing behind that is an invisible cap.
    forbid_text(front_cpp,
                "if (g_nvml_api.resetGpuLockedClocks && g_app.lockMode == LOCK_MODE_HARD)",
                "rollback releases locked clocks unconditionally, including a "
                "transition clamp armed for a FLATTEN request")

    # Linux: same contract, expressed in the pure phase order.
    require_text(transaction_h, "LINUX_MUTATION_LOCK_CEILING",
                 "Linux has a dedicated transition-ceiling phase")
    require_text(transaction_h,
                 "LINUX_MUTATION_LOCK_CEILING,\n        LINUX_MUTATION_RESET_BASELINE,",
                 "the Linux ceiling phase runs before the reset baseline")
    require_text(linux_ceiling_h, "static ApplyClockCeilingPlan linux_apply_clock_ceiling_plan(",
                 "Linux derives its ceiling from the shared policy")
    require_text(linux_ceiling_h, "static bool linux_apply_arm_transition_ceiling(",
                 "Linux names the ceiling-arming step once")
    require_text(linux_mutation_cpp, "case LINUX_MUTATION_LOCK_CEILING:",
                 "the Linux transaction implements the ceiling phase")
    require_text(linux_ceiling_h,
                 "static bool linux_apply_reset_baseline_locked_clocks(",
                 "the Linux reset baseline does not release a clamp it is "
                 "supposed to be running under")
    require_text(linux_mutation_cpp,
                 "if (!linux_apply_reset_baseline_locked_clocks(g, d)) return false;",
                 "the Linux reset baseline routes locked clocks through the "
                 "ceiling-aware rule")
    require_text(linux_mutation_cpp,
                 "(phaseMask & LINUX_MUTATION_LOCK_CEILING)) {",
                 "Linux rollback releases a transition clamp it armed")
