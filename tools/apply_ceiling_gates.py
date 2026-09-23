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


def check_all(ctx, require_text, forbid_text, require_order_in_operation):
    policy_h = _p(ctx, "apply_clock_ceiling_policy.h")
    # Bound once under a name the later `policy_h` rebinding cannot shadow: the
    # clock-ceiling rules are checked again after this function switches
    # `policy_h` to curve_point_offset_policy.h.
    ceiling_policy_h = policy_h
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
    require_order_in_operation(
        apply_cpp,
        "static bool apply_desired_settings_service(const DesiredSettings* desired",
        "clockCeiling.arm();",
        "if (!reset_oc_before_gui_apply(",
        "the clamp is armed BEFORE reset-to-stock drops the old ceiling")
    # CT-01.  Arming used to return void, so a driver that refused the clamp let
    # the apply walk straight into reset-to-stock and the curve batch with no
    # protection -- the uncapped sequence this whole mechanism exists to
    # prevent.  A REQUIRED protection that could not be installed is now a
    # refusal before any mutation.
    require_text(policy_h,
                 "static inline bool apply_clock_ceiling_transition_must_refuse(",
                 "whether an unprotectable transition must be refused is one "
                 "named rule, not an open-coded log line")
    require_text(guard_h, "ApplyClockCeilingArmResult arm() {",
                 "arming reports what actually happened instead of returning void")
    require_order_in_operation(
        apply_cpp,
        "static bool apply_desired_settings_service(const DesiredSettings* desired",
        "if (clockCeiling.must_refuse_transition()) {",
        "if (!reset_oc_before_gui_apply(",
        "an unprotectable transition is refused BEFORE reset-to-stock runs")
    require_text(apply_cpp,
                 "clockCeiling.adopt(\"released with the locked-clock domain\");",
                 "a non-HARD apply hands the clamp to the locked-clock release")
    # CT-02.  That release used to be unconditional on anything except the lock
    # mode, so a FLATTEN whose tail failed verification uncapped a partially
    # written curve -- the shape of the 2026-09-13 incident.  It is now gated on
    # the curve having verified, and the failing branch retains the clamp.
    require_text(apply_cpp,
                 "const bool curveStateProvenSafe = failCount == 0 && curveRequestOk;",
                 "the locked-clock release is conditional on a verified curve")
    require_order_in_operation(
        apply_cpp,
        "static bool apply_desired_settings_service(const DesiredSettings* desired",
        "const bool curveStateProvenSafe = failCount == 0 && curveRequestOk;",
        "clockCeiling.adopt(\"released with the locked-clock domain\");",
        "the verified-curve test precedes the release it guards")
    # The curve verdict has to OUTLIVE the batch scope for that test to be
    # possible at all; it used to be declared inside the curve-write block.
    require_order_in_operation(
        apply_cpp,
        "static bool apply_desired_settings_service(const DesiredSettings* desired",
        "bool curveRequestOk = true;",
        "if (curveBatchNeeded && (curveRequest || memApplied)) {",
        "the curve verdict is declared OUTSIDE the curve-write block, so the "
        "locked-clock release at the end of the apply can still consult it")
    # CT-02, the other half: an explicitly requested lock point this GPU cannot
    # resolve is refused up front rather than silently applied unlocked.
    require_text(apply_cpp,
                 "is not available on this",
                 "an unresolvable explicit lock anchor is refused before any write")
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
    require_text(witness_h,
                 "apply_clock_witness_counts_toward_verdict(w->clampArmed,",
                 "a sample taken before the clamp was armed, or in the same "
                 "instant as the arming write, cannot be judged against a "
                 "ceiling that was not yet in force")
    require_text(witness_h, "static void apply_clock_witness_record_at_arming(",
                 "the arming-instant sample has its own named entry point rather "
                 "than a bool at the call site")
    require_text(guard_h, 'apply_clock_witness_record_at_arming("ceiling armed")',
                 "the guard records its arming sample through the unjudged path")
    require_text(apply_cpp, "ApplyClockWitnessScope clockWitness(",
                 "the apply opens a clock witness for its whole duration")
    require_text(apply_cpp, 'apply_clock_witness_record("post-curve-batch (pre-lock)");',
                 "the apply samples the live clock at the instant that used to be "
                 "uncapped -- the curve is raised and the lock has not run")
    require_text(_p(ctx, "gpu_backend_snapshot.h"), 'apply_clock_witness_poll("curve settle");',
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

    # A rollback returns every other control to stock, so a locked-clock clamp
    # left standing behind that is an invisible cap -- WHEN the resets worked.
    # CT-04: when they did not, releasing hands the user a raised curve with
    # nothing holding it down, which is strictly worse.  The old LOCK_MODE_HARD
    # gate stays forbidden (it missed the FLATTEN transition clamp entirely);
    # what replaces it is a verified-recovery gate, not an unconditional release.
    rollback_h = _p(ctx, "main_gpu_rollback.h")
    forbid_text(rollback_h,
                "if (g_nvml_api.resetGpuLockedClocks && g_app.lockMode == LOCK_MODE_HARD)",
                "rollback must not gate the release on HARD mode, which misses "
                "a transition clamp armed for a FLATTEN request")
    require_text(policy_h, "static inline bool apply_recovery_permits_release(",
                 "when a restriction may be lifted after recovery is one named rule")
    require_text(rollback_h, "if (!apply_recovery_permits_release(recovery)) {",
                 "rollback keeps the restriction when it cannot prove stock")

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
    # CT-01 on Linux: the phase body used to `return true` on every path,
    # including the one where both clamp forms were refused.
    require_text(linux_ceiling_h,
                 "const bool refuse = apply_clock_ceiling_transition_must_refuse(",
                 "the Linux phase asks the shared rule whether to refuse rather "
                 "than deciding on its own")
    require_text(linux_ceiling_h, "return !refuse;",
                 "a refused REQUIRED Linux clamp fails its phase")
    # The phase must also be SCHEDULED when protection is required: gating the
    # request on `.arm` alone meant a GPU with no locked-clock control simply
    # omitted the phase, so the refusal inside it could never run.
    require_text(linux_mutation_cpp,
                 "if (ceilingPlan.arm || ceilingPlan.required)",
                 "the Linux ceiling phase is scheduled whenever protection is required")
    # CT-07: the witness used to pass the PLAN's arm flag as evidence that the
    # clamp was installed, so a refused clamp still printed HELD.
    require_text(linux_ceiling_h, "g_linuxCeilingArmed",
                 "the Linux witness reads the flag the arming call actually set")
    forbid_text(linux_ceiling_h,
                "apply_clock_witness_verdict(ceiling.arm, ceiling.arm,",
                "a planned clamp is not evidence of an armed one")
    linux_rollback_h = _p(ctx, "linux_backend_rollback.h")
    require_text(linux_rollback_h,
                 "if (phaseMask & clockPhases) {",
                 "Linux rollback accounts for a transition clamp it armed")
    # CT-07.  It must not release that clamp over a curve it just restored and
    # cannot vouch for -- in particular an outgoing HARD profile, whose raw VF
    # tail is safe only because of its pin.
    require_text(linux_rollback_h, "linux_snapshot_curve_needs_a_pin(snapshot)",
                 "Linux rollback keeps the restriction when the restored curve "
                 "needs a pin to be safe")
    require_text(linux_ceiling_h, "static void linux_apply_ceiling_note_outgoing(",
                 "what the Linux transaction is leaving is recorded at entry, "
                 "not inferred from a snapshot full of positive offsets")

    forbid_text(guard_h, "bool ok = nvml_reset_gpu_locked_clocks(detail, sizeof(detail));",
                "scope destruction cannot release unverified protection")
    require_text(apply_cpp, "return apply_recover_clock_failure(clockCeiling, result, result, resultSize);",
                 "baseline failure reaches guarded recovery")
    require_text(linux_mutation_cpp, "linux_apply_clock_ceiling_plan(g, d, previousIntent)",
                 "Linux plans protection from the actual previous intent")
    require_text(linux_mutation_cpp, "linux_apply_arm_transition_ceiling(g, d, context->previousIntent)",
                 "Linux arming receives the previous intent too")
    require_text(linux_mutation_cpp, "if (d->hasLock || (requested & LINUX_MUTATION_LOCK_CEILING))",
                 "temporary protection always receives final disposition")
    forbid_text(linux_rollback_h, "g->nvml.resetGpuLockedClocks(g->nvmlDevice);",
                "uncertain Linux rollback cannot erase clock protection")
    require_text(reset_cpp, "0, 0, ownsXbar, ownsMsvdd)",
                 "baseline preserves independently unowned XBAR siblings")

    require_text(rollback_h, "reset_core_clock_controls(vf_curve_global_gpu_offset_supported(), true,",
                 "rollback uses backend-aware reset sequencing")

    # The 2026-09-17 under-load profile switch (profile 3 -> 4 at 99% util).
    # Nothing here is about the ceiling itself -- it armed correctly and held
    # every sample -- but the apply that ran underneath it failed in a way that
    # ended in a driver recovery, so the rules live beside the other ordering
    # gates rather than in build.py.
    #
    # Profile 4 stores its curve as `base_plus_gpu_offset`: point 70 is the
    # stock base (2322 MHz) plus the request's own +475 MHz component. Loading
    # the profile reconstructs 2797 MHz from the base captured when the profile
    # was SAVED. Under load the driver reports that point's stock base one whole
    # VF bin higher, so the same correct +475000 kHz offset reads back as 2827
    # MHz -- a 30 MHz "miss" against a number no user ever typed.
    verify_h = _p(ctx, "gpu_backend_apply_verify.h")
    targets_h = _p(ctx, "gpu_backend_apply_targets.h")
    diagnostics_cpp = _p(ctx, "main_diagnostics.cpp")
    service_host_cpp = _p(ctx, "main_service_host.cpp")

    # Such a point carries OFFSET intent, in either offset routing.
    require_text(verify_h, "desired->curvePointFromGpuOffset[ci]",
                 "verification separates projected points from typed absolutes, PER POINT")
    require_text(verify_h, "(selective || fromOffset)",
                 "projected points take the offset branch whichever routing applies")
    # ...and writing must not re-derive them from the live base, which is the
    # very sample that moved.
    forbid_text(targets_h, "offset = (long long)desired->curvePointMHz[ci] * 1000 - base;",
                "projected points keep the requested offset, not absolute-minus-live-base")
    require_text(_p(ctx, "linux_curve_targets.h"), "desired->curvePointFromGpuOffset[i]",
                 "Linux target building honours the same provenance rule")
    forbid_text(targets_h, "desired->curvePointFromGpuOffset[ci] && gpuPolicyViaCurveBatch",
                "offset ownership is routing-independent: a profile with no GPU offset never sets that flag")

    # One answer to "what offset does this point want?", shared by the initial
    # build, the correction loop and Linux.  Four private copies is what made
    # each earlier fix cover only part of the system: the correction loop kept
    # deriving `absolute - live base` after the build site stopped, so a tail
    # miss under load sent it in to overwrite already-correct points.
    policy_h = _p(ctx, "curve_point_offset_policy.h")
    require_text(policy_h, "static inline long long curve_point_target_offset_khz(",
                 "the shared curve-point offset policy exists")
    for site, what in ((targets_h, "initial target build"),
                       (apply_cpp, "correction loop"),
                       (_p(ctx, "linux_curve_targets.h"), "Linux target build")):
        require_text(site, "curve_point_target_offset_khz(&want)",
                     f"the {what} asks the shared offset policy")
    forbid_text(apply_cpp, "long long targetKHz = (long long)raw_curve_khz_from_display_mhz(targetMHz);",
                "the correction loop may not re-derive a point's offset from the live base itself")

    # A point landing far below the requested curve is how a wrong result hides
    # behind severity=success: slot 1 reported a clean apply with points 74/75
    # sitting 465 and 360 MHz low. The count has to be in the log unconditionally.
    require_text(_p(ctx, "gpu_backend_apply_diagnostics.h"), "post-apply SHORTFALL:",
                 "an apply that lands far below the requested curve says so")
    require_text(_p(ctx, "gpu_backend_apply_diagnostics.h"), "refusedPlaceholder",
                 "the shortfall line excludes the driver-refused placeholder, so it is not permanently on")
    # The GUI Apply path is the one the first fix missed: a profile load sets the
    # flag, but clicking Apply rebuilds the request from the editor, whose VF
    # fields show the same projection and carry the same stale absolutes.
    require_text(_p(ctx, "main_runtime_control.cpp"), "desired->curvePointFromGpuOffset[ci] = gc_bool8_from_bool(",
                 "the GUI capture records curve-point provenance too")
    require_text(_p(ctx, "app_shared.h"), "static inline void gui_set_curve_point_origin(",
                 "editor ownership and provenance are recorded through one setter")
    forbid_text(_p(ctx, "ui_main.cpp"), "g_app.guiCurvePointExplicit[ci] = true;",
                "no site may claim editor ownership without stating provenance")
    # Provenance describes the VALUE it travels with, so every path that copies,
    # merges, clears or synthesizes a curve point has to carry it too.  The
    # active service intent and the profile/CLI merges are exactly the paths an
    # under-load automatic replay uses; a dropped flag silently turns a
    # projected point back into a typed absolute, which is the original defect.
    require_text(_p(ctx, "config_profiles_ui.cpp"),
                 "base->curvePointFromGpuOffset[i] = override->curvePointFromGpuOffset[i];",
                 "the Windows settings merge carries per-point provenance")
    require_text(_p(ctx, "linux_port_profiles.cpp"),
                 "base->curvePointFromGpuOffset[i] = incoming->curvePointFromGpuOffset[i];",
                 "the Linux settings merge carries per-point provenance")
    require_text(_p(ctx, "service_desired_mutation_policy.h"),
                 "merged.curvePointFromGpuOffset[i] = requested->curvePointFromGpuOffset[i];",
                 "durable service intent keeps per-point provenance across sparse merges")
    require_text(_p(ctx, "service_desired_mutation_policy.h"),
                 "desired->curvePointFromGpuOffset[i] = 0;",
                 "projecting a request to available domains clears dropped point provenance")
    require_text(_p(ctx, "desired_settings_ui_policy.h"),
                 "left->curvePointFromGpuOffset[i] !=",
                 "struct equality treats provenance as state, not metadata")
    require_text(_p(ctx, "linux_tui_actions.cpp"),
                 "desired.curvePointFromGpuOffset[index] = 0;",
                 "a Linux TUI hand edit makes that point an absolute target")
    forbid_text(_p(ctx, "ui_main.cpp"),
                "g_app.guiCurvePointExplicit[g_app.lockedCi] = true;",
                "the lock anchor edit goes through the ownership+provenance setter")

    # The correction loop must terminate on its own evidence. Its per-point
    # `stuck` bookkeeping is reachable only for locked tail points, so a non-tail
    # point the driver would not move was reclassified every pass and never ended
    # the loop: 25 passes at ~1 s each, all holding the hardware gate.
    require_text(apply_cpp, "correctionReachedFixedPoint",
                 "correction loop detects a pass in which no point improved")
    require_text(apply_cpp, "if (correctionReachedFixedPoint) break;",
                 "the fixed-point verdict actually leaves the correction loop")
    # ...but it must not leave before the pass has been VERIFIED. The
    # convergence bookkeeping counts a point unconverged on exact equality,
    # while the apply is verified against curve_point_verify_tolerance_mhz(), so
    # a pass can land the whole curve inside tolerance, report
    # `unconverged>0 improved=0`, and reach a fixed point that IS the requested
    # result. Breaking first left curveRequestOk false and rolled back a curve
    # that had verified.
    require_order_in_operation(
        apply_cpp,
        "static bool apply_desired_settings_service(const DesiredSettings* desired",
        "if (verify_curve_request(curveVerifyDetail, sizeof(curveVerifyDetail))) {",
        "if (correctionReachedFixedPoint) break;",
        "a correction pass is verified before the fixed-point exit, so a curve "
        "within tolerance is not failed for missing exact equality")

    # A clamp the GPU has never had is not a clamp that was declined. Refusing
    # on both broke every profile switch away from an undervolt on families
    # whose driver answers NOT_SUPPORTED to nvmlDeviceSetGpuLockedClocks (the
    # API is Volta and newer; Pascal is a fully supported family here).
    require_text(ceiling_policy_h, "APPLY_CEILING_ARM_UNSUPPORTED,",
                 "an absent locked-clock control is a distinct arm result")
    require_text(ceiling_policy_h, "if (result == APPLY_CEILING_ARM_UNSUPPORTED &&",
                 "only an UNSUPPORTED clamp may let a transition through; a "
                 "REFUSED one still refuses")
    require_text(ceiling_policy_h, "reason == APPLY_CEILING_REASON_RESET_DROPS_CAP ||",
                 "an absent clamp may permit a lock-free transition")
    require_text(ceiling_policy_h, "reason == APPLY_CEILING_REASON_REQUESTED_FLATTEN))",
                 "a VF flatten remains usable without NVML hard-lock support")
    require_text(ceiling_policy_h, "static inline bool apply_clock_control_proven_absent(",
                 "reset and rollback use one conservative absent-control rule")
    require_text(_p(ctx, "gpu_backend_apply.cpp"),
                 "apply_clock_control_proven_absent(",
                 "Windows skips an inapplicable Pascal reset only without a retained pin")
    require_text(rollback_h, "apply_clock_control_proven_absent(",
                 "Windows recovery recognizes an absent Pascal lock domain after stock verifies")
    require_text(_p(ctx, "main_service_apply_runtime.cpp"),
                 "apply_clock_control_proven_absent(",
                 "explicit Windows Reset recognizes an absent Pascal lock domain")
    require_text(linux_ceiling_h, "linux_clock_control_proven_absent(g)",
                 "Linux baseline and final lock use the absent-control rule")
    require_text(linux_rollback_h, "const bool noClockControl = linux_clock_control_proven_absent(g);",
                 "Pascal rollback restores prior settings when no cap can exist")
    require_text(linux_mutation_cpp, "bool noControl = linux_clock_control_proven_absent(g);",
                 "explicit Linux Reset skips an absent locked-clock domain")
    require_text(ceiling_policy_h,
                 "static inline bool apply_clock_ceiling_proceeds_unprotected(",
                 "the log line for an unprotected transition shares the rule "
                 "that permits it")
    require_text(guard_h, "log_unprotected_if_proceeding();",
                 "the Windows guard says so out loud when it proceeds without "
                 "the clamp it wanted")
    require_text(linux_ceiling_h, "PROCEEDING UNPROTECTED",
                 "the Linux arming phase says so out loud too")
    # Both of those lines must diagnose the ATTEMPT, not the GPU. A lock-less
    # request may only use the open-ended clamp form -- the symmetric one would
    # add a clock floor nobody asked for -- so NOT_SUPPORTED there is also what
    # a driver with working locked-clock control returns when it rejects a 0
    # minimum. "This GPU has no locked-clock control" would be a wrong diagnosis
    # in that case and would send the reader hunting the wrong fault. The
    # stronger claim stays legal in refusal_message(), which is only reachable
    # when the request names its own lock and BOTH forms were therefore tried.
    for shard in (guard_h, linux_ceiling_h):
        require_text(shard, "every clamp form this request may use",
                     "an unprotected-transition log line states what was "
                     "actually established -- no permitted form was supported "
                     "-- rather than diagnosing the GPU")
    require_text(_p(ctx, "main_runtime_nvml.cpp"),
                 "*notSupportedOut = (r == NVML_ERROR_NOT_SUPPORTED);",
                 "the Windows clamp write reports NOT_SUPPORTED distinctly from "
                 "any other refusal")

    # And the wedge watchdog must not read a queued fan pulse's age as a driver
    # hang. The pulse stamps its heartbeat BEFORE queuing on the runtime lock, so
    # waiting behind a slow apply looked identical to hanging inside nvml.dll;
    # the service killed a working driver, broke the client's apply and locked
    # out automatic restore. A wedge stops every stamp, so progress is the signal.
    require_text(service_host_cpp, "progressAgeMs > SERVICE_FAN_PULSE_WEDGE_TIMEOUT_MS",
                 "the wedge verdict also requires the hardware gate to have stopped moving")
    require_text(diagnostics_cpp, "service_note_hardware_progress();",
                 "an apply advancing a phase stamps gate progress, so slow is not read as wedged")
    require_order_in_operation(
        _p(ctx, "clock_reset_policy.h"),
        "static ApplyRecoveryResult reset_core_clock_controls(",
        "result.gpuOffset.verified = resetScalar();",
        "result.curve.verified = resetCurve();",
        "independent scalar reset verifies before the curve reset")
