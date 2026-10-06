# Clock transition hardening

## Summary

Last verified: 2026-09-23 (source/API compatibility review; see the open
finding below). Targeted follow-up to
`b695583e278a758b0f1fd2ec35215de84c1d33a1`. The September 16 review found nine
remaining defects. The follow-up fixes those software paths and a verification
regression reported during user testing. It does not complete the original
42-scenario hardware acceptance plan.

Historical evidence and the broader plan are local, untracked artifacts
(`temp/stability-review-b695583.md`, `temp/stability-harden.md`) that are not
part of the repository. Their reviewed-commit findings are historical, not
descriptions of the current working tree.

## 2026-09-23 compatibility correction: unsupported locked clocks

The release-review exception in `source/apply_clock_ceiling_policy.h` was
incomplete. NVIDIA's NVML command reference says both
`nvmlDeviceSetGpuLockedClocks` and `nvmlDeviceResetGpuLockedClocks` are for
Volta-or-newer devices and return `NVML_ERROR_NOT_SUPPORTED` when unavailable.
The earlier policy let an unsupported transition through only for
`APPLY_CEILING_REASON_RESET_DROPS_CAP`; it classified VF-curve `LOCK_MODE_FLATTEN`
as an NVML HARD pin and refused it before writing. A lock-free transition that
did pass could still fail on final reset after curve writes, on both platforms.

The policy now distinguishes VF FLATTEN from HARD. `UNSUPPORTED` permits only
FLATTEN or no-lock end states; a transient refusal still vetoes. A shared pure
predicate accepts an absent NVML reset only for a known Pascal GPU with no
installed/retained clamp or outgoing HARD pin. Windows apply, stock recovery,
and explicit Reset use it. Linux baseline, final lock, explicit Reset, and
snapshot rollback use it; the rollback restores and verifies prior settings
without inventing a cap that the GPU cannot install. Real or uncertain pins
retain the prior fail-closed behavior. Logs identify each skipped reset and
unprotected restoration.

`tests/clock_transition_tests.cpp` now makes both set and reset return
`NOT_SUPPORTED`. It checks no-lock and FLATTEN applies, HARD refusal, capable
hardware refusal, retained-cap refusal, and Linux snapshot restoration. Source
gates pin the Windows and Linux call sites. This is software and API-contract
verification, not a measured Pascal run. The full apply orchestrators and a
live Pascal apply/Reset/recovery sequence remain unverified.

## 2026-09-23 apply time budget and Reset NVML gap

- **Correction budget.** `SERVICE_APPLY_HANDLER_BUDGET_MS` (20 s) was a promise
  nothing inside the apply enforced. [apply_correction_budget_policy.h](../source/apply_correction_budget_policy.h)
  now stops starting correction passes at `budget - APPLY_POST_CORRECTION_RESERVE_MS`
  (14 s after apply entry), estimating the next pass from the previous one;
  pass 1 always runs. `apply_curve_offsets_verified()` takes an optional
  fallback deadline that skips further ~1 s per-point writes; Reset and
  rollback pass 0 (unbounded). A stopped correction reuses the ordinary failed
  verification path (clamp retained, rollback), with the detail suffixed
  "correction stopped after N pass(es) to stay within the apply time budget".
  Diagnostics: `curve correction pass N wrote in X ms`, `NOT starting pass`,
  `skipped N per-point fallback write(s)`, and `elapsedMs=... OVER-BUDGET` on
  `apply outcome:`. Invariant: the budget is a bound on WORK STARTED, never a
  sleep; it does not change what is written, only when the loop gives up.
- **Reset with NVML not ready.** `service_reset_all()` skipped the locked-clock
  release silently when `nvml_ensure_ready()` failed and still said
  "Reset applied.". It is now a failure whenever `lockMode`, `appliedLockFreq`
  or a retained transition cap says a restriction may exist.
- Linux got the Windows placeholder rule (`vf_offset_zero_readback_is_benign`)
  in its VF writer; it previously had no correction loop to hide the difference.

## 2026-09-24 lock pre-tail refusal moved ahead of reset-to-stock

- **Defect.** The "Curve lock X MHz at point N is below pre-tail point M" check
  ran after `reset_oc_before_gui_apply()` and returned a bare `false`. Its log
  line even said "failed before writes". On a reset-before-apply request (every
  GUI FULL apply, profile switch, and lifecycle restore) that left the GPU on the
  stock curve with the transition clamp retained at the lock frequency (guard
  destructor), the previous profile gone, and a message that only asked the user
  to lower a point. Reachable because the load-time profile check covers only
  typed points, while the service also checks offset-derived and untouched
  points against the live stock base, which moves a bin with temperature.
- **Fix.** [apply_lock_pretail_policy.h](../source/apply_lock_pretail_policy.h)
  is the one rule; [gpu_backend_apply_lock_pretail.h](../source/gpu_backend_apply_lock_pretail.h)
  runs it before the clamp is armed, on a fresh curve read, using each point's
  stock base (live minus programmed offset), which is what the post-reset
  readback reports. The post-reset copy stays as a backstop for a base that
  moved between the two reads and now returns through
  `apply_recover_clock_failure()` when a reset ran. A failed pre-check read
  defers to that backstop.
- **Also.** `reset_oc_before_gui_apply()` discarded its post-reset curve
  readback result and wrote `refresh_global_state()` detail into the caller's
  result buffer; a failed readback now fails the reset (same criterion as the
  apply's own reads), so no target is computed from a pre-reset sample.
- Tests 6430-6443 (`tests/apply_profile_followup_tests.cpp`); gates at the end
  of `tools/apply_ceiling_gates.py` (verified failing with the pre-check
  removed). Software only; not reproduced on hardware.

## Reset and failure sequencing

- [clock_reset_policy.h](../source/clock_reset_policy.h) distinguishes VF-global
  backends from a separate scalar offset. Reset VF-global backends through the
  VF table directly: inferred-global scalar reset can mistake a majority flatten
  floor for a global offset and raise the prefix by subtracting it. With a
  separate scalar, force and freshly verify zero before resetting the curve.
  Explicit Reset and recovery share this rule; baseline reset follows it too.
- [gpu_backend_apply_failure.h](../source/gpu_backend_apply_failure.h) handles
  prerequisite failures immediately. It re-establishes required protection
  before recovery, including after a final setter that might have changed the
  restriction before reporting failure. GPU/memory/baseline/snapshot/target
  failures cannot fall through to normal finalization.
- [main_gpu_rollback.h](../source/main_gpu_rollback.h) releases only after both
  scalar and curve stock controls verify. Unattempted work alone is not proof.
  The Windows guard destructor never unlocks abandoned state.
- [gpu_backend_apply_ceiling.h](../source/gpu_backend_apply_ceiling.h) records
  retained protection separately from desired/applied lock markers. A subsequent
  apply must not infer a higher bound from a raw curve after failed recovery.
  Verified finalization/recovery and GPU selection reset clear this ownership.

## Target authority and the point-69 regression

[gpu_backend_apply_targets.h](../source/gpu_backend_apply_targets.h) constructs
immutable per-point control targets. An explicit non-tail MHz target overrides
selective offset policy. Missing requested points and out-of-range targets fail
rather than being silently discarded or clamped.

[gpu_backend_apply_verify.h](../source/gpu_backend_apply_verify.h) distinguishes:

1. Explicit non-tail points: fresh absolute MHz must match the requested value
   within the existing point/bin tolerance, including selective/HARD mode.
2. FLATTEN tail: fresh MHz must match the requested lock target.
3. HARD tail: raw VF MHz is diagnostic; the final NVML pin owns its frequency.
   Missing requested readback still fails.
4. Offset-derived non-tail points: verify the immutable control offset, not a
   MHz projection from an earlier curve sample. Higher offsets beyond the
   existing 12,000 kHz control tolerance fail; lower driver-limited offsets are
   logged. A matching derived offset never excuses an explicit MHz violation.
   Corrections preserve the original derived control target.

The user reported `VF point 69 verified at 2407 MHz instead of requested 2295
MHz`. Local logs show `explicit=0`, exclusion count 70, GPU +475 MHz, and point
69 offset 0. The driver changed the readback shape after the batch; 2295 was a
pre-write preview, not an absolute user target. The first follow-up verifier
incorrectly treated that preview as a ceiling. Checking the requested zero offset
fixes the semantic error without adding a 112 MHz exception or loosening explicit
and tail checks. Diagnostics now distinguish previews from absolute targets.
The user must retest the rebuilt binary; the log establishes the original
regression, not hardware acceptance of its fix.

[gpu_backend_snapshot.h](../source/gpu_backend_snapshot.h) keeps the newest
coherent curve/offset sample, even at equal population. A final read failure
cannot reuse earlier success as current proof. Verification reevaluates current
readback on each correction. The low-level zero-offset placeholder exception
requires a positive request, zero readback and a fresh populated frequency below
the operating minimum; it cannot hide a refused negative offset or missing point.

## Linux and auxiliary controls

- [linux_backend_mutation.cpp](../source/linux_backend_mutation.cpp) passes the
  actual previous intent to planning/arming, separately from the merged
  destination intent. Arming preserves recorded outgoing HARD ownership.
- [linux_backend_rollback.h](../source/linux_backend_rollback.h) protects before
  restoring potentially modified core state, including failures before CURVE
  and after a higher final pin. Refused protection cannot unlock the old pin.
  Uncertain core rollback retains protection and reports uncertainty.
- `LinuxGpuState.retainedTransitionCeilingMHz` survives transaction cleanup and
  is scoped to the selected GPU. No-lock requests that armed protection schedule
  final disposition too. Successful finalization or explicit Reset clears it.
- [gpu_backend_reset_baseline.cpp](../source/gpu_backend_reset_baseline.cpp) and
  [gpu_backend_apply_advanced.h](../source/gpu_backend_apply_advanced.h) preserve
  XBAR frequency and MSVDD independently through fresh masked read-modify-write.
  Sparse requests do not inherit unrelated advanced fields. Full replacement
  already materializes dropped fields as zero in
  [service_lifecycle_policy.h](../source/service_lifecycle_policy.h).
- [vf_offset_range_policy.h](../source/vf_offset_range_policy.h) uses signed
  endpoints, overflow-safe magnitude arithmetic and one flatten floor. Known
  nonnegative ranges cannot flatten and are refused before mutation. Unknown
  GPU family read/write support remains enabled by default.

## Diagnostics and validation

Windows witness evidence freezes at successful handoff, so a higher final pin
cannot be judged against the old lower transition cap. Recovery rearming resumes
the window. Linux post-finalization diagnostics use the final state while keeping
the saved transition plan intact for rollback.

[tests/clock_transition_tests.cpp](../tests/clock_transition_tests.cpp), linked
by [build.py](../build.py), includes actual production helper headers with fake
driver dependencies. It covers repeated mode pairs, rejected arm/rollback,
late/partial final writes, reset composition/failure combinations, retained-cap
reuse, explicit target authority, the point-69 regression, negative-offset
refusal, missing/final-failed snapshots and range boundaries. It does not execute
the entire Windows or Linux apply orchestrator. Source gates guard that wiring;
neither layer proves driver atomicity.

Commands: `python build.py --test`, `python build.py --test --asan`,
`python build.py --tidy`, `python build.py`. See the newest recent-log entry for
final results. Windows-host Linux checks cross-link the Linux fixtures; they do
not constitute native Linux runtime or hardware testing.

## 2026-09-17 under-load failure (profile 3 -> 4 at 99% util)

First genuine under-load test of the hardening. **The ceiling itself passed**:
plan `outgoingHoldsDown=1 outgoingCeiling=2962 outgoingMode=flatten` -> min ->
2957, armed before the first clock write, and every witness sample stayed under
it (entry 2880, post-reset settle 2760 at 250 W/64 C, post-curve-batch 2910)
while the raw curve tail sat at 3622 MHz. The apply underneath it failed, in
three compounding ways, all fixed here.

**1. Projected curve points were held to a stale absolute MHz.**
`[profile4_curve]` uses `curve_semantics=base_plus_gpu_offset`:
`point70_mhz=2322` is the STOCK base and
`restore_curve_points_from_base_plus_gpu_offset()`
([config_profile_repair.cpp](../source/config_profile_repair.cpp)) rebuilds
2322+475 = 2797 at load time. Under load the driver reports point 70's stock
base one whole VF bin higher (2352), so the correct +475000 kHz offset read back
as 2827 MHz: `curve verification failed: ci=70 actual=2827 target=2797
explicit=1 tail=0`. Indistinguishable from a typed absolute because
`explicitCurveMask[ci] = true` for every `hasCurvePoint`.

**The file format moved after this incident (2026-09-17, second review).**
`base_plus_gpu_offset` is read-only now; every save writes
`curve_semantics=absolute_with_origin` — absolute MHz plus a per-point
`pointN_from_gpu_offset` — because the whole-section marker could not describe a
profile that MIXES a typed point with projected neighbours, and saving one
flattened it back to all-projected. The incident above is unchanged as history;
see [config-profiles](config-profiles.md) "Curve semantics" for what a profile
written today looks like.

**Propagation rule (review 2026-09-17):** the flag describes the value it
travels with, so every path that copies, merges, clears or synthesizes a curve
point must carry it. `merge_desired_settings()` (both platforms),
`service_merge_desired_after_mutation()` (durable intent, including the
reset-baseline clear), `service_project_desired_to_available_domains()`, profile
load/repair clear paths, the Windows verify snapshot, the CLI live/`--pointN`
fallbacks, Linux TUI hand edits and the Windows lock-anchor edit all do now;
`desired_settings_equal()` treats a different origin for identical digits as a
different request (the profile-vs-active match deliberately stays numeric: the
locked tail's flag legitimately differs between a loaded profile and the
request that applied it). Source gates in `tools/apply_ceiling_gates.py` pin
the merge/equality/TUI sites.

`DesiredSettings.curvePointFromGpuOffset[VF_NUM_POINTS]` now travels on the wire
(protocol 27). The apply keeps flagged points OUT of `explicitCurveMask`;
[gpu_backend_apply_verify.h](../source/gpu_backend_apply_verify.h) verifies their
offset in either routing, [gpu_backend_apply_targets.h](../source/gpu_backend_apply_targets.h)
stops re-deriving them as `absolute - live base` (`offsetOwnsThisPoint`), and
[linux_curve_targets.h](../source/linux_curve_targets.h) carries the same rule.
Linux verification was already offset-based and needed no change. Typed points
keep absolute authority and still fail on their own readback.

**Two corrections to the first attempt (4cb08a0), both found by the 14:01 retest:**

- *It was a single per-request flag.* Wrong granularity: load a base+offset
  profile and hand-edit one field, and that point is a real absolute while its
  neighbours are projections. Now per point. (Protocol 26 -> 27 rather than
  reusing 26, because build 192 shipped v26 to the user's machine and a silent
  layout change under the same version is exactly what the version prevents.)
- *It only covered the profile-LOAD path.* The retest's failing apply came from
  `ui action: Apply clicked` -> `capture_gui_desired_settings`, which rebuilds
  the request from the editor and never touched the flag: `reconstructed=0` in
  the log while the profile-load applies at 14:00:50 showed `reconstructed=1`.
  The editor's VF fields show the same projection for any point it did not get
  from the user, and `preTailInferred` points exist in the request *only*
  because the GPU offset put them there. `g_app.guiCurvePointFromGpuOffset[]`
  and `g_app.appliedCurveFromGpuOffset[]` now shadow `guiCurvePointExplicit` /
  `appliedCurveMHz`, written through `gui_set_curve_point_origin()` /
  `applied_set_curve_point_origin()` in [app_shared.h](../source/app_shared.h) so
  a new call site cannot record ownership without provenance; a hand edit in
  [ui_main_window.cpp](../source/ui_main_window.cpp) clears it. A `forbid_text`
  gate on the raw `guiCurvePointExplicit[ci] = true` keeps that true.

**1b. Profile 1 (WITHDRAWN rule -- read this before touching the loader again).**
Slot 1 failed with `curve verification failed: ci=74 actual=2932 target=2902
explicit=1 tail=0 reconstructed=0 selective=0` -- 30 MHz, one VF bin, with
`gpu_offset_mhz=0` and `point74_offset_khz=0`. The fix attempted in `2423f05`
read that zero as "this point is a stock recording, it wants nothing" and
flagged such points offset-authoritative.

**That was wrong, and it produced a silent wrong result**: slot 1 then applied
cleanly with points 74/75 left at stock, `actual=2430 target=2902` and
`actual=2572 target=2932` -- 465 and 360 MHz below the profile -- reported as
`severity=success`. The disproof is in the same file: `[profile1_curve]` stores
`offset_khz=0` for EVERY point including the flatten tail at 2962, which
unambiguously requires a large negative offset. So `pointN_offset_khz` is
hardware state sampled at save time, carrying no intent at all for this
profile; **`pointN_mhz` is the intent.** The rule was reverted.

The original 30 MHz miss is base movement between the target computation and
the readback, and the correction loop is the mechanism for exactly that -- it
recomputes against a fresh base and rewrites. It could not do its job while it
was also corrupting other points (1c below).

**1c. The correction loop kept its own copy of the rule, and undid the fix.**
Slot 1 still failed on build 194, and the log shows the whole mechanism:

```
apply curve peak after batch: 2992 MHz at ci=76 ...
curve verification failed: ci=76 actual=2992 target=2962 explicit=1 tail=1
curve correction pass 1: target point 75 live=2602 MHz offset=0 desiredOffset=502000
curve offset verification failed: ci=74 actual=495000 target=0 kHz reconstructed=1
post-apply tail bookends: first=ci76 actual=2962 ... last=ci126 actual=2962
post-apply curve summary: tail=51OK+0OFF boost=0OK+2OFF
apply failure: VF point 74 offset verified at 495000 kHz above requested 0 kHz
```

Points 74/75 were written correctly at offset 0. The TAIL then missed by one VF
bin because the base moved, which sent the apply into the correction loop --
and the correction loop still derived `absolute - live base` in its own private
branch, overwriting those correct points with +495000 and +502000 kHz. The
apply then failed on a value the corrector had just invented. Note the tail
itself converged (`tail=51OK+0OFF` at exactly 2962): the only surviving failure
was self-inflicted.

Four sites answered "what offset does this point want?" independently: the
initial target build, the correction loop, the Linux target build and
verification. Fixing them one at a time is why each earlier attempt covered
only part of the system. They now share
[curve_point_offset_policy.h](../source/curve_point_offset_policy.h):
`fromGpuOffset` returns the request's own offset component and never looks at a
base; only a genuinely typed absolute consults the live base. Gates require all
three write sites to call it and forbid the old private derivations.

**The governing rule, which every one of these failures is an instance of: the
absolute MHz of a VF point is not a controllable quantity.** Green Curve writes
offsets; the driver owns the stock base and moves it with load and temperature.
Any stored or projected absolute is a sample of one moment. Only the tail lock
keeps absolute authority, because it is what bounds peak clocks and is written
as its own target.

**1d. The correction loop recomputed against a base sampled at reset time.**
The last one, and the reason slot 1 still failed after 1b was withdrawn:

```
curve verification failed: ci=74 actual=2932 target=2902 ... delta=30
curve correction pass 1: target point 75 live=2962 MHz offset=502000 desiredOffset=502000
correction pass 2: no point improved and 2 remain unconverged
post-apply curve: ci=74 actual=2932 target=2902 delta=30 freqOffs=495000 BOOST
post-apply tail bookends: first=ci76 actual=2962 target=2962 last=ci126 actual=2962 target=2962
```

`desiredOffset` equals what is already programmed, so the corrector writes
nothing and the point cannot move. It derived the base from
`originalCurveFreqkHz`/`originalCurveOffsets` -- the snapshot taken right after
the reset. Under load the driver had since moved the base 30 MHz, so
`2902000 - 2407000 = 495000` reproduced the same wrong offset every pass. The
fresh base is `2932 - 495 = 2437`, which asks for 465000, and that is the
offset that lands the point.

The asymmetry in the log is the proof: the locked TAIL converged on pass 1
every single time, because it already went through
`curve_delta_khz_for_target_display_mhz()` -> `curve_base_khz_for_point()`,
which reads `g_app.curve[ci].freq_kHz - g_app.freqOffsets[ci]` -- the live
base. Only the non-tail branch used the stale snapshot.

Subtracting the CURRENTLY PROGRAMMED offset from the CURRENT frequency is what
keeps this absolute rather than cumulative, which is the property
`BUILD` gates as the "cumulative offset bug": it recovers the stock base, so
each pass recomputes the whole offset instead of accumulating a delta. Feeding
a landed result back in is a fixed point.

**2. The correction loop could not detect its own fixed point.** The per-point
`stuck` bookkeeping in [gpu_backend_apply.cpp](../source/gpu_backend_apply.cpp)
is reachable only under `gpuPolicyViaCurveBatch && hasLock && lockedTailMask[ci]`
— tail points only. A non-tail point the driver would not move was reclassified
every pass, `prevErrorKHz` refreshed, and nothing broke the loop: all 25 passes
at ~1.03 s, identical offsets written and identical frequencies read
(`ci=70 actual=2827 target=2797` twelve times in the log). Now a pass in which
no unconverged point improves sets `correctionReachedFixedPoint` and leaves the
loop. The loop bound stays 25; the exit is on evidence, not on a timer.

**3. The wedge watchdog read waiter age instead of progress.**
`service_fan_runtime_thread` stamps `g_serviceFanPulseHeartbeatMs` BEFORE
`lock_service_runtime()`, so time spent queued behind a legitimately slow apply
was counted as "wedged inside nvml.dll". `SERVICE_FAN_PULSE_WEDGE_TIMEOUT_MS`
is 12000 while the correction loop's worst case is ~26 s, so the watchdog always
won: `fan pulse wedged for 14656 ms`, controlled recovery, client transport dead
(`error 109, bytes 0/7112`), restarted service with no GPU backend
(`control state readback validity: gpu=0 mem=0 power=0` — a controlled-recovery
start skips the driver probe), and `auto-restore lockout`. New
`g_serviceHardwareProgressMs` ([service_gate_progress.h](../source/service_gate_progress.h))
is stamped by `set_last_apply_phase()`, `unlock_service_runtime()` and the fan
pulse once it owns the gate; the wedge verdict now requires BOTH the pulse and
the gate to have stopped. A real wedge stops every stamp; a slow apply does not.
The deferral is logged rather than silent.

Wire cost: `SERVICE_PROTOCOL_VERSION` 25 -> 27, `DesiredSettings` 836 -> 964,
`ServiceRequest` 1424 -> 1552, `ServiceResponse` 7112 -> 7368. Provenance cannot
be recomputed service-side — both a typed 2797 and a projected 2322+475 arrive
as `curvePointMHz[70] = 2797` — so it had to travel.

Splits forced by the size ratchets, which were LOWERED, not raised:
`validate_desired_settings_for_ipc` -> [desired_settings_ipc.h](../source/desired_settings_ipc.h)
(gpu_core.h 910 -> 829), wedge-watchdog heartbeats ->
[service_gate_progress.h](../source/service_gate_progress.h) (main.cpp 801 -> 796).
New gates live in [apply_ceiling_gates.py](../tools/apply_ceiling_gates.py), not
build.py, which is also on its ratchet.

Verified on hardware for defects 2 and 3: the 14:01 retest (`loadMeaningful=1`,
100% util, 253 W, 59 C) stopped at `correction pass 2: no point improved and 59
remain unconverged`, with no wedge, no controlled recovery and no service
restart — a clean fast failure where the first run had torn the driver down.
Defect 1 was still `reconstructed=0` there and is fixed only in the second pass.

Coverage limits: the fixtures in
[clock_transition_tests.cpp](../tests/clock_transition_tests.cpp) prove the
verify and target-build rules per point, including a mixed request where a typed
neighbour keeps absolute authority, with the pre-fix absolute-minus-live-base
rule re-simulated as a negative control. The `explicitCurveMask` construction,
the GUI capture and the correction loop live in the apply orchestrator and the
GUI, which no test executes; they are held by source gates only. **The GUI-path
provenance fix has no hardware retest.**

## 2026-09-17 release review (0.26.0)

A read of the last two weeks of commits before the 0.26.0 release found two
defects in the clock-transition work itself. Both are software-only fixes with
gates and tests; neither has had a hardware replay.

### The transition-clamp veto refused every Pascal profile switch

`apply_clock_ceiling_transition_must_refuse()` refused on every non-INSTALLED
arm result. `nvmlDeviceSetGpuLockedClocks` is a **Volta-and-newer** control;
Pascal (a family `vf_backends.cpp` fully supports, with its own VF spec) answers
`NVML_ERROR_NOT_SUPPORTED` to every form. Because
`APPLY_CEILING_REASON_RESET_DROPS_CAP` fires for any apply that resets to stock
while the outgoing state holds the clocks down -- which is the definition of an
undervolt -- the veto turned *every profile switch away from an undervolt* into
a refusal on those cards, for a cap that hardware has never had. This collides
directly with the project's "keep read and write support for unsupported GPUs"
constraint; nothing about the transition changed in 0.26.0, only the veto.

The rule now separates "declined this time" from "absent on this GPU":

- New `APPLY_CEILING_ARM_UNSUPPORTED`, set only when every *permitted* clamp
  form answers `NOT_SUPPORTED`, or when the entry point is missing
  (`ApplyClockCeilingPlan::clampControlAbsent`). NVML merely failing to come up
  is deliberately NOT unsupported -- that is an environment failure a later
  attempt can fix.
- `APPLY_CEILING_ARM_REFUSED` (permission, reservation, transient driver state)
  still refuses, so capable hardware keeps CT-01 protection in full.
- UNSUPPORTED still refuses when the request needs an NVML HARD pin: that apply
  would fail on the same call at its final lock step, so refusing first reports
  the real reason and writes nothing. The 2026-09-23 correction permits VF
  FLATTEN because that end state does not need an NVML pin.
- What is left -- a lock-free or VF FLATTEN request, no usable clamp -- proceeds, and
  `apply_clock_ceiling_proceeds_unprotected()` drives one explicit log line on
  both platforms so it is never a silent absence.

**What UNSUPPORTED does and does not establish** (wording corrected
2026-09-17, after the first pass overstated it). It means *no clamp form this
request is permitted to use was accepted*, which is not the same claim as "this
GPU has no locked-clock control". A lock-less request may only use the
open-ended `(0, ceiling)` form -- the symmetric one would add a clock FLOOR
nobody asked for -- so a driver that DOES have locked-clock control and rejects
a 0 minimum with NOT_SUPPORTED lands here too. The decision is unaffected
(nothing installable exists for this request either way), but a log line that
diagnosed the GPU would send the next reader hunting a driver fault that is not
there, so none of them do. `refusal_message()` is the one place the stronger
claim is earned and made: UNSUPPORTED only refuses for a HARD pin, which is
exactly when the symmetric form is permitted, so both forms
were tried. A gate pins the qualifier in both unprotected log lines.

Sources: [apply_clock_ceiling_policy.h](../source/apply_clock_ceiling_policy.h),
[gpu_backend_apply_ceiling.h](../source/gpu_backend_apply_ceiling.h),
[linux_apply_ceiling.h](../source/linux_apply_ceiling.h),
`nvml_set_gpu_locked_clocks(..., bool* notSupportedOut)` in
[main_runtime_nvml.cpp](../source/main_runtime_nvml.cpp). Executing coverage in
[clock_transition_tests.cpp](../tests/clock_transition_tests.cpp) (the fake
driver now returns a selectable refusal code); gates in
[apply_ceiling_gates.py](../tools/apply_ceiling_gates.py).

### The correction loop's fixed-point exit skipped verification

The fixed-point exit added on 2026-09-17 ran **before** the pass was verified.
The two use different yardsticks: the convergence bookkeeping counts a point
unconverged on exact equality (`actualMHz == targetMHz`), while the apply is
verified by `apply_verify_curve_targets()` against
`curve_point_verify_tolerance_mhz(ci)`. A pass that brought the whole curve
inside tolerance without landing any point exactly therefore reported
`unconverged>0 improved=0`, broke out with `curveRequestOk` still false, and
failed + rolled back a curve that had in fact landed. `prevErrorKHz[ci]` starts
at `INT_MAX` and is not updated for a point that converged exactly, so a point
drifting off an exact match is never counted as "improved", which makes the
shape reachable rather than theoretical.

`verify_curve_request()` takes a `const DesiredSettings*` and writes no
hardware, so running it first is free; the order is pinned by a
`require_order_in_operation` gate.

**Executed since 2026-09-24.** The correction loop's decisions moved out of
`apply_desired_settings_service()` into the pure
[apply_correction_policy.h](../source/apply_correction_policy.h):
`apply_correction_plan_pass()` (absolute offsets from live - programmed, the
shared `curve_point_target_offset_khz()` rule, uniform tail floor beyond the
anchor, range clamp reported), `apply_correction_classify_pass()` (converging /
stuck / worsening / out-of-range / accepted-readback / strict-diverged, the
fixed-point verdict, per-point reports for the log lines), the loop driver
`apply_run_correction_loop()` (budget -> plan+write -> classify -> VERIFY ->
fixed-point exit) and `apply_correction_tail_monotonic_violation()`. The apply
supplies a per-pass readback snapshot, the write and the log text (same
wording; clamping is now one summary line per pass instead of one line per
clamped point, and a final `curve correction: passes=N verified=..
fixedPoint=.. budgetExhausted=.. nothingToCorrect=..` line was added).
`tests/apply_correction_tests.cpp` (6500-6531) drives it against a simulated
driver (exact, drifting base, ignored point, creeping point, VF-bin
quantisation, monotonic tail collapse). Mutation-checked: fixed-point exit
before verify -> 6527; cumulative base -> 6501; fixed point disabled -> 6503;
provenance ignored -> 6506. Still untested: the surrounding orchestrator
(reset, batch, lock, power, fan, rollback) and real driver readbacks.
Observed while writing the fixture, not changed: a requested point whose
readback is 0 never counts as unconverged, so it cannot trigger the fixed-point
exit; the loop then rewrites unchanged offsets until the time budget. Only
reachable for a requested but unpopulated VF point.

## Open questions / release acceptance

- 0.26.0 release review (2026-09-17): build 202 ran clean on hardware -- 11
  applies 15:49:45..15:52:37, all `severity=success failCount=0`, 11 witness
  verdicts all HELD (7 with `loadMeaningful=1`, 100% util, 220-255 W, 55-70 C),
  7 corrections all converging on pass 1, zero fixed-point exits, zero
  SHORTFALL, zero rollbacks/wedges/dropped lines, and no `nvlddmkm` events at
  all that day. The HARD-pin applies are the direct control for the 2026-09-13
  TDR: clamp armed 15:50:08.582 -> reset-to-stock 15:50:09.624 -> curve batch
  peaking at **ci126 = 3667 MHz with `armed=1`** -> `min=2957 max=2957 ok` ->
  `clamp adopted by the final lock step`. `tail=0OK+51OFF` with
  `flattenUnmatched=51` on those applies is the deliberate raw HARD tail, not a
  failure.
- **But neither 0.26.0 fix is actually exercised by that run.** All 11 applies
  logged `reason=requested lock target` and installed the clamp, so
  `APPLY_CEILING_REASON_RESET_DROPS_CAP` never fired and no
  `PROCEEDING UNPROTECTED` line exists -- correct on Blackwell, and zero
  coverage for the UNSUPPORTED path. The Pascal claim still rests on NVML's
  documented Volta-and-newer support, not a measurement. A live GTX 10-series
  run remains useful if hardware becomes available.
- The verify-before-fixed-point reorder has its precondition on record but was
  not decisive: six applies logged `correction pass 1 convergence: ...
  unconverged=55 improved=0` while the tail sat at 2962 against a 2957 target
  (5 MHz, inside `tol=8`) and verification passed on the same pass. That is
  exactly the within-tolerance-but-not-exact shape the old ordering would have
  failed -- but it landed on pass 1, where the guard is inactive by
  construction (`correctionPass > 0`), so the old build would have passed these
  too. Needs a run that reaches pass 2.
- Not exercised on 202: an explicit Reset, a NONE (unpinned -> unpinned)
  transition, and rapid back-to-back switching under load.

- DONE 2026-09-17 on build 197: 12 applies across slots 1/2/3/4, idle and under
  a real workload (100% util, 220-255 W, 60-72 C), all `severity=success`,
  every correction converging on pass 1, ceiling HELD on every sample, no
  driver events. The 1a-1d defects above are all confirmed fixed on hardware.
  Still not exercised: a NONE (unpinned -> unpinned) transition and an explicit
  Reset under load, and rapid back-to-back switching.
- The broad executor seam, all SH-01..SH-42 lifecycle/fault scenarios and complete
  voltage/memory/power acceptance remain deferred to keep this patch targeted.
  Existing settle/retry delays were not added as fixes and do not prove safety.
- **Known accepted gap (2026-09-24 review): Pascal FLATTEN + uniform GPU
  offset has an uncapped transient.** In `apply_desired_settings_service()` the
  dedicated `nvapi_set_gpu_offset()` write runs BEFORE the curve batch that
  floors the FLATTEN tail. On non-Blackwell it is the NVML graphics offset, a
  scalar that shifts the whole curve, tail included, so between the two writes
  the top VF points run at stock + offset at full voltage (scalar write +
  settled readback + batch, roughly 0.2-1.5 s). Every GPU that installs the
  clamp is capped at `min(lock, outgoing)` there; Pascal cannot, and FLATTEN is
  deliberately let through unprotected, so on Pascal the window is uncapped.
  Selective offsets (exclude-low) and Blackwell are unaffected (single batch /
  clamp); Linux composes the offset into the curve for locked profiles
  (`desired_gpu_offset_uses_curve`) and is unaffected.
  **Rejected fix:** a Pascal-only "tail-first" pre-write (floor the tail, anchor
  pre-lowered by the rise, then raise the scalar). After the scalar write the
  apply refreshes originals and derives the anchor base as `freq - offset`;
  the FLATTEN verifier expects floored tail points to read back AT the lock,
  i.e. readback appears to be the driver's effective (monotonic) curve, so a
  pre-lowered anchor or a higher explicit pre-tail point can be read back
  clamped and the anchor base mis-derived. The default order never reads the
  anchor with a non-zero offset. Unverifiable without Pascal hardware, and the
  maintainer judged it not worth risking; reverted before commit. Revisit only
  with a Pascal board and a readback trace (raw vs effective per-point MHz).
- Hot-applied FLATTEN/explicit points keep their offset when the GPU cools and
  can run thermal bins above the typed MHz; project rule is not to fight this.
- Opaque driver atomicity, voltage/frequency behavior, other clock-writing tools,
  device loss and inherently unstable OC remain limits. Software checks cannot
  guarantee every transient or make arbitrary user clocks stable.
