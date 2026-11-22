# Recent Log

Newest-first rolling memory. Archive older entries to `archive-YYYY-Www.md` when this file grows too long.

This log is PUBLIC (tracked since 2026-10-22): every entry follows the same
rules as the topic pages (`secret-leak-prevention.md`, "Public wiki safety").

## 2026-11-22: Renew the 0.31.0 update-freshness envelope

- Ran the documented dry run and then the first live
  `release_post.py --renew-freshness`, after independently checking the latest
  release, local/remote tag identity, signing-key/ACL preflight, GitHub clock
  agreement, and a verified rollback copy.
- The tool revalidated the published v1 signature and both installers'
  release-workflow provenance, then replaced only
  `greencurve-update-v2.txt` and `.sig`. The new envelope carries the exact
  475-byte v1 manifest, was issued `2026-11-22T20:41:43Z`, and expires
  `2026-12-22T20:41:43Z`.
- Postflight proved all 21 release assets remained present and every non-v2
  asset kept its identity, size, digest, and release metadata. Anonymous
  delivery verified the new signature and v2-to-v1 binding; manual freshness
  workflow [45067937146](https://github.com/aufkrawall/green-curve/actions/runs/45067937146)
  passed with 30.0 days remaining.
- The renewal itself required no source, installer, tag, release-body, or
  changelog change. The temporary rollback copy was removed after success.
  The remaining risks are the already documented non-atomic two-asset
  replacement and concurrent remote publication.

## 2026-11-21: Review follow-ups across Windows fan handback, clock ceiling, explicit-release profiles, and Linux upgrade rollback

- **Windows fan Auto handback & Reset guard:** cleared `g_app.fanIsAuto` on failed reads and operational policy-read errors (`main_runtime_nvml_fans.cpp`), added per-fan `policyUnsupported` provenance (`control_readback_policy.h`) so older drivers/GPUs lacking `nvmlDeviceGetFanControlPolicy_v2` still confirm Auto from verified per-fan writes + consistent fan count, gated `service_reset_fan_to_driver()` on `app_fan_control_known_absent()`, complete positive policy readback, and `!service_owned_fan_is_non_auto()`, and extended `service_shutdown_owned_fan()` and Reset failure retention to cover emergency-owned manual fan intent (`tests/fan_handback_runtime_tests.cpp` `12903-12910`, `12920-12926`).
- **Post-Apply fan fault retention:** added `fan_fault_should_clear_after_apply()` (`fan_intent_policy.h`) so `service_apply_desired_settings()` never clears an active fan fault when post-Apply fan-intent invariant repair fails (`tests/fan_intent_tests.cpp` `12737`).
- **Clock ceiling `memoryMayDropFlatten` scoping:** scoped `memoryMayDropFlatten` to non-resetting memory writes (`!resetsToStock`) across `apply_clock_ceiling_policy.h`, `gpu_backend_apply.cpp`, and `linux_apply_ceiling.h`, restored `REQUESTED_LOCK`/`REQUESTED_FLATTEN`/`RESET_DROPS_CAP` precedence (preserving Pascal FLATTEN/stock switches and HARD-pin symmetric fallback), and added live negative-offset curve detection on Linux when `committed == nullptr` (`tests/clock_transition_tests.cpp`).
- **Explicit-release legacy documents:** resolved `PROFILE_SCOPE_EXPLICIT_RELEASE` dead-end in `profile_slot_runtime.h` so empty-curve releases always require destination topology while indexed explicit curve/lock releases convert offline (`tests/profile_slot_runtime_tests.cpp` `12710-12712`).
- **Linux upgrade rollback & probes:** added pure `linux_service_rollback_plan()` (`linux_service_install_policy.h`), `systemctl reenable` for previously enabled units after file restore + `daemon-reload`, and `--quiet` on pre-upgrade `is-active`/`is-enabled` probes (`tests/regression_main.cpp` `1276-1279`).

## 2026-11-21: Daemon startup and GPU transition review

- Linux boot classifies incoming records before automatic writes and resolves
  prior ownership before transferring control to a startup profile.
- A lifetime instance lock protects startup and socket ownership. Every
  post-replay exit attempts fan handback before unloading the backend.
- Memory writes that can clear an outgoing flatten require maximum-only clock
  protection. Flatten itself cannot install a fixed-clock fallback.
- Linux Reset keeps the outgoing power limit through curve verification and
  independently verifies both available memory controls.

## 2026-11-21: Review follow-ups for Linux exports and upgrades

- Linux create-only exports use `linkat` when `renameat2(RENAME_NOREPLACE)`
  is unavailable. The operation requires one of these atomic mechanisms.
- The upgrade rollback rewrite had dropped the explicit root owner and group on
  staged files and the post-install ownership checks; both are back
  (verification runs on the live paths before activation).
- `systemctl enable` runs again for an already enabled unit.

## 2026-11-21: Cooling, Reset and Linux upgrade recovery

- Use RPM for stall decisions and keep the live Windows cooling runtime when
  Reset cannot verify driver Auto.
- Capture the Linux Reset rollback limit before changing clocks. Skip the
  fan phase only when the driver confirms there are no controllable fans.
- Stage and back up Linux service files before publishing the upgrade.
  Activation failure rolls back the files and service state.
- Windows regression and source checks passed. The Linux file fixture ran
  under WSL. The full build produced 12 packages with matching payloads and hashes.

## 2026-11-20: Review-range hardening across fan handback, Linux VF masks/modeled review, GPU selection, and Linux INI/CLI loading

- Verified and resolved seven code-review findings across `9f7e493..HEAD`:
  1. **Linux fan lockout parity:** `daemon_fan_runtime_lockout()` now updates `g_activeDesired` / `g_hasActiveDesired` / `g_activeTarget` before writing `LINUX_DAEMON_RECORD_UNCERTAIN`; `populate_snapshot()` gates fan mode and runtime flags on `intentMatchesSelected`, `!g_stateUncertain`, and shared `linux_daemon_fan_runtime_active()` (`fan_intent_policy.h`); non-fan `SERVICE_CMD_APPLY` preserves `g_daemonFanFault` (`tests/fan_runtime_followup_tests.cpp` `12898-12899`).
  2. **Windows fan handback state sync:** `service_fan_runtime_handback_publish()` initializes canonical defaults on `!hadIntent` emergency handback, clears/populates `g_serviceActiveDesiredGpu`, updates profile metadata (`NONE/0` vs `AD_HOC/0`), and clears/re-persists the restart-reapply snapshot outside `g_appLock` (`tests/fan_handback_runtime_tests.cpp` `12857/12859/12860/12869`).
  3. **Linux VF `vfMask` intersection:** baseline reset, full Reset, snapshot capture, and rollback intersect exact masks with `g->vfMask` via `linux_vf_baseline_reset_mask()` and standalone `linux_vf_rollback_mask_policy.h` while preserving sub-450 MHz rollback placeholders (`tests/linux_vf_write_tests.cpp` `12689-12691`).
  4. **Scoped Linux `vfBaselineReset` in modeled review:** `linux_authoring_scalar_transition_precheck()`, `authoring_modeled_transition_preflight()`, and `linux_apply_prepare_scoped_vf_before_scalar()` (`linux_apply_modeled_review.h`) run scoped VF baseline reset + post-reset modeled review before named scalar writes and mark `vfBaselineResetAttempted` for rollback (`tests/linux_clock_offset_tests.cpp` `9006-9008`).
  5. **Windows GPU selection recovery:** `service_prepare_requested_gpu()` restores the previous selection on init failure/identity mismatch (`main_service_path_containment.h` split out to keep `main_service_request_policy.cpp` under 800 lines); `gpu_selector_combo_enabled()` keeps the combo usable in `DEGRADED`/`DEVICE_MISSING`; transient busy/transition refusals (`gpu_selection_refusal_is_transient()`) no longer latch reconciliation; and GPU switches enqueue a single full sync (`tests/gpu_selection_sync_tests.cpp` `7100-7101`).
  6. **Linux CLI lazy GPU target resolution:** `linux_cli_profile_load.h` resolves the configured GPU target only when `needsLive` is true, enforcing `linux_gpu_target_for_write()` for mutating verbs and `linux_gpu_target_for_read()` for read-only verbs.
  7. **Linux INI flat-tail repair parity:** `linux_profile_document_load.h` and `linux_port_profiles.cpp` skip `PROFILE_CURVE_DECODE_BASE_PLUS_GPU_OFFSET` and pass `pointN_offset_khz` into `profile_desired_has_legacy_flat_tail_offset_artifact()`.
- Also fixed two `clang-tidy` findings (`linux_port.cpp` explicit `existingDestination != nullptr`, `main_state_sync.cpp` `(void)fromFullSync;` in service build).
- Verification: `python build.py --gates`, `python build.py --test` (all pure tests, Windows pipe/loader/CLI fixtures, Linux `x86_64` cross-links, `wiki_public_gates`, and source gates), and `python build.py --tidy` (0 new findings, 34 baselined) all passed; Linux native runtime tests were cross-linked on Windows rather than executed on a Linux GPU host.

## 2026-11-20: Fan stall detection and reported fan handbacks

- Follow-up to the profile-switch report: neither platform noticed a fan that
  accepts its manual duty and does not turn. Both runtimes now run the shared
  `fan_stall_policy.h` after each successful tick and hand a confirmed stall
  back to the driver (else 100%) through their existing handback.
- Found on the way: the Windows service's repeated-failure handback kept the
  curve in the owned intent, so the GUI and tray kept showing it and the next
  Apply's fan invariant would have restarted it. Protocol v39 adds the
  `ServiceFanFault` snapshot record and the service now stops claiming a fan the
  driver took back. Linux's two worker lockout copies became one helper.
- Size ratchet: `service_gpu_health_reason_name()` moved to
  `service_gpu_health_names.h`, `nvml_read_fans()` to
  `main_runtime_nvml_fans.cpp`, and the v29 record check into its own header.
- Details and limits: `fan-control.md` (top section). Fixture-verified with
  per-gate mutations; no stalled fan on hardware.

## 2026-11-20: Profile switch released a running fan curve

- Reported: after switching Profile 4 -> Profile 3 (both fan curve, zero-RPM
  off) the fan stood still at idle while the GUI showed Profile 3 and its curve
  as active. The debug log showed the GUI's reset-before-apply request without
  the unchanged fan, `apply-scope: scope=2`, a driver-Auto fan write with the
  curve runtime stopped, and then `service response APPLY: ... fanMode=2`.
- Root causes and fix are in `fan-control.md` (top section): the shared legacy
  scope upgrade read an omitted fan as a release (now keeps omitted fan,
  memory and power for non-replacement reset requests, Windows and Linux); the
  GUI now carries an owned Fixed/Curve fan in full requests; the Windows
  service's fan mode follows its runtime and owned intent instead of a mirror
  re-derived from the outgoing intent; only an Auto profile may match an
  unclaimed fan; and every successful service Apply enforces the fan
  runtime/intent invariant (logged, repaired, reported as warning/error).
- Tests: `tests/fan_intent_tests.cpp` 12720-12769, relaxation cases
  1364-1368 + 12737-12739 (12421 inverted), `tools/fan_gates.py`
  (`check_fan_intent_consistency()`).
- Verification: native Windows regression suite and source gates passed;
  three scratch-copy negative controls failed at the new assertions; plain
  `python build.py` refreshed all twelve 0.31.0 packages. Not replayed on
  hardware.

## 2026-11-20: Curve correction, profile I/O and GPU binding

- Linux VF correction requires a fresh snapshot, a retry-eligible write policy and convergence of projected offsets. Windows retargeting disambiguates identical boards, and Reset preserves fractional memory readback.
- Both editors import the reviewed source. Linux exports require consent before replacement. Complete voltage bindings survive transfer and domain-only releases use the destination's writable points.
- Startup comparison uses topology from the bound GPU. A mapped refresh carries the service instance, GPU generation and topology signature used to resolve the slot.

- Verification: the Windows regression suite and source gates passed. The Linux regression harness and filesystem fixture ran under WSL. Build 405 produced twelve packages whose binary payloads and SHA-256 sidecars matched. GPU failures were exercised with fixtures.

## 2026-11-20: Release-range review fixes verified and packaged

- Completed the seven review corrections: planner-fixture request lifetime;
  scoped Linux VF baseline writes; hard-pin tail isolation during correction;
  explicit replacement scope and named graphics controls across profile
  transfer; untouched advanced-field projection in the Windows editor; and
  exact milliwatt comparison and verification during Windows power Reset.
- Source/test anchors: `tests/curve_plan_tests.cpp`,
  `tests/linux_vf_write_tests.cpp` (12683-12684),
  `tests/profile_slot_runtime_tests.cpp` (12685-12688),
  `tests/gui_editor_base_tests.cpp` (12681-12682),
  `tests/external_oc_change_tests.cpp` (12680). Production invariants and
  diagnostics are in `clock-transition-audit.md`, `profile-document.md`,
  `config-profiles.md` and `gpu-backend.md`.
- Verification: native Windows `python build.py --test` and
  `python build.py --test --asan` passed, including installer and named-pipe
  fixtures, Linux cross-link checks and source/Python checks.
  `python build.py --tidy` passed with no new findings. The profile codec
  completed 5,000 fuzz runs. Six scratch-copy negative controls independently
  restored the faulty behavior and failed at the intended new assertions;
  each fixed fixture passed.
- Plain `python build.py` completed the full Windows/Linux x64/ARM64 matrix.
  All 12 current packages have fresh timestamps and changed content digests;
  checksum files, archive contents, installer payloads and packaged executable
  bytes match the rebuilt outputs. New diagnostics are present in the binaries.
  Hardware Apply/Reset behavior was verified with simulated driver data only;
  this task did not perform a physical GPU replay.
- Reconciled stale captured-replay and advanced-field projection claims in the
  topic pages. Condensed the unreleased notes while retaining all 49 topics.

## 2026-11-20: Linux TUI flatten-tail delta display & Linux -> Windows profile export/capture parity

- Reported / audited:
  1. In the Linux TUI VF table, flatten-tail rows after the knee (`ci=77..126`)
     showed `-5` in `DELTA` (`targetMHz 2957 - clampedBaseMHz 2962`) even though
     the driver was correctly programmed with `-1000 MHz` (verified via live
     `--json-live` readback across all flattened profiles).
  2. Auditing Linux -> Windows export/import uncovered three real gaps:
     - **Unlocked profiles failed export on both Linux and Windows:**
       `linux_port_profiles.cpp`, `config_profiles.cpp`, and
       `main_runtime_capture.cpp` wrote `lock_tracks_anchor=1` alongside
       `lock_ci=-1, lock_mhz=0, lock_mode=0`, which `profile_legacy_ini.h`
       rejected with `legacy lock needs an explicit anchor, frequency and mode`.
     - **Raw Linux `config.ini` import on Windows failed without manual format
       selection:** `profile_editor_transfer_probe()` defaulted every legacy INI
       to `PROFILE_LEGACY_WINDOWS_INI` even when `[meta] linux_mem_migrated=1`
       or bare `pointN=` keys identified Linux.
     - **Rejected approach (verified live on RTX 5070 Linux driver 620.73.04):**
       wiring `hardware_capture_memory_clock` into `linux_backend_capture.h`
       caused Linux capture to report `memory=0Hz(running-clock)` under a
       `+3000 MHz` OC because on Linux `nvmlDeviceGetMaxClockInfo(NVML_CLOCK_MEM)`
       returns the OC-adjusted max (`17001 MHz == Pstates20 17001 MHz`), unlike
       Windows (`14001 MHz` board stock). Reverted and locked behind a
       `forbid_text` gate in `tools/linux_gates.py`.
- Fixes (`2a154e6`, `bc5dfe6`, follow-up revert of Linux `memoryClockReader`):
  1. `linux_tui_layout.{h,cpp}` / `linux_tui_layout_vf.cpp`: compute
     `TUI_POINT_FLATTEN_TAIL` `deltaMHz` from `vf_offset_range_flatten_floor_khz`
     (typically `-1000 MHz`), report live offsets on `TUI_POINT_LIVE` and the
     selected-point header, and keep `TUI_POINT_EXCLUDED` at `0 MHz`.
  2. Gate `lock_tracks_anchor=1` on active lock presence in Linux/Windows INI
     savers and accept the explicit no-lock triple with a leftover
     `lock_tracks_anchor=1` in `profile_legacy_ini.h` (while keeping bare or
     contradictory lock records refused).
  3. Add `profile_editor_detect_legacy_ini_format()` in
     `profile_editor_transfer.h` so legacy Linux `config.ini` files auto-detect
     as `PROFILE_LEGACY_LINUX_INI`.
  4. Keep `linux_backend_capture.h` on the truthful NVML memory offset getter
     with a `forbid_text` gate against wiring `hardware_capture_memory_clock`.
- Verified: regression cases `12652-12656` (`tests/regression_main.cpp`),
  `12660-12663` (`tests/profile_legacy_ini_tests.cpp`), `12664-12667`
  (`tests/profile_tools_tests.cpp`), unlocked save/export in
  `tests/linux_gpu_selection_store_regression.cpp`, `tools/linux_gates.py`,
  `python3 build.py --test`, `python3 build.py --check --target all`, plain
  deliverable `python3 build.py`, and live hardware CLI/TUI/capture verification.

## 2026-11-20: Windows-exported profiles failed on Linux (curve convergence, shaped-baseline transitions, voltage-bound CLI/startup loading)

- Reported: five profiles exported from Windows and imported into Green Curve
  on Linux (same multi-boot RTX 5070 Blackwell GPU, NVIDIA Linux driver
  620.73.04) worked on Windows, but multiple profiles and cross-profile
  transitions failed on Linux.
- Root causes (confirmed live on hardware and from daemon/code inspection):
  1. **Missing post-write curve correction loop on Linux.** Applying a
     selective `+475 MHz` offset together with a `-1000 MHz` flattened tail
     pulled the anchor (`ci=76`) to `2947 MHz` (`10 MHz` below the `2957 MHz`
     target, outside the `8 MHz` verification tolerance). Windows converged in
     pass 1 (`+475 -> +485 MHz`), whereas Linux stopped after one VF write and
     rolled back.
  2. **Shaped-VF baseline pollution and missing replacement preparation.**
     Switching between flattened/captured curves and hard-locked or legacy
     profiles without resetting the active VF baseline caused target building to
     subtract offsets from monotonically clamped tail bases (`-1005 MHz` out of
     range) or cross-talked pre-tail points, and caused legacy slots upgraded by
     `service_upgrade_legacy_scalar_scope()` to inherit prior explicit-scope
     captured points when `desired_prepare_replacement_request()` was omitted.
  3. **Offline-only profile loader rejected voltage-bound slots.** Linux CLI
     (`--dump`, `--json`, `--apply-config`, `--startup-profile`), startup sync,
     and TUI startup bind always called `load_document_section()` with a null
     destination topology, failing voltage-bound profiles and even blocking
     `--tui`/`--probe`/`--reset` when `selected_slot` pointed to one.
- Fixes (three commits: `63a6d04`, `7c22054`, `2bf64e6`):
  1. Added `source/linux_backend_curve_correction.h` and
     `source/linux_backend_curve_correction_runtime.h` implementing the
     budget-bounded post-write correction loop for both legacy (reusing
     `apply_correction_policy.h`) and exact authoring paths, with flat-tail
     monotonicity verification and actual-Hz mismatch diagnostics.
  2. Added `source/linux_vf_baseline_policy.h` to reset shaped VF baselines
     inside `LINUX_MUTATION_CURVE` under an armed `LINUX_MUTATION_LOCK_CEILING`
     with a post-baseline `curve_plan_evaluate()` backstop, and aligned Linux
     CLI and TUI to call `desired_prepare_replacement_request()` on profile
     replacement.
  3. Added destination-aware loaders (`linux_profile_document_load.h`,
     `linux_port_profiles.cpp`), live-daemon topology fallback
     (`linux_cli_profile_load.h`), deferred slot materialization in
     `linux_main.cpp`, lock-mode/captured-point output in
     `linux_profile_output.cpp`, and captured/projected VF point preview in
     `linux_tui_layout.cpp`.
- Verified:
  - Regression tests `12630-12648` (`tests/linux_vf_write_tests.cpp`),
    `12650-12651` (`tests/regression_main.cpp`), native fixture
    `test_voltage_bound_profile_load_and_output()`
    (`tests/linux_gpu_selection_store_regression.cpp`), and
    `tools/linux_gates.py::check_profile_interchange_and_curve_correction()`.
  - `python3 build.py --test`, `python3 build.py --check --target all`,
    `python3 build.py --gates`, and plain deliverable `python3 build.py`.
  - Live hardware on RTX 5070 Linux: all five imported profiles, 13-step
    cross-profile CLI sequence, interactive TUI load/apply cycle, and
    `--startup-profile` bind succeeded with 0 failed phases and pass-1
    correction in 10-22 ms.

## 2026-11-20: Loading an older saved flatten profile failed with "graphics offset (-1927 MHz) is out of range"

- Reported: clicking Load on Profile 1 raised
  `Profile 1: graphics offset (-1927 MHz) is out of range (-1000..1000 MHz)`.
- Root cause (confirmed from stored profile structure and code):
  1. Profile 1 had originally been saved by a pre-`5d80beb` Green Curve build
     while an external tool's flatten at `ci=76` (`2962 MHz`) was active; the
     old selective VF offset detector misread the flattened tail (`ci=77..126`
     at `-1927 MHz`, anchor `ci=76` at `+480 MHz`, `0..75` at `0 MHz`) as
     `gpu_offset_mhz=-1927` with `gpu_offset_exclude_low_count=76` alongside
     `lock_mode=flatten`, `lock_ci=76`, `lock_mhz=2962`, `lock_tracks_anchor=1`,
     absolute tail points `76..126` (`2962 MHz`), and zero projected points.
  2. In ordinary INI form, `validate_desired_settings_for_ipc()` silently
     clamped `-1927` to `-1000` while leaving `exclude_low_count=76` (and with
     `lock_tracks_anchor=1` could corrupt the lock target). Once exported and
     re-imported as a portable indexed document, `profile_desired_from_indexed_document()`
     rejected `-1927 MHz` outright against `-1000..1000 MHz`, making the slot
     impossible to load even in the editor.
- Fix:
  1. Added pure `source/profile_legacy_flat_tail_offset_policy.h` recognizing
     only that provably dead pre-fix flat-tail artifact (`< -1000 MHz`,
     `LOCK_MODE_FLATTEN` with absolute tail points above `lockCi` matching
     `lockMHz`, zero projected/captured points, and either
     `excludeLowCount >= lockCi` corroborated by any recorded tail observed
     offsets or `excludeLowCount == 0` with zero pre-tail points, at least four
     matching observed tail offsets within `30 MHz`, and an observed anchor
     offset differing from the tail delta).
  2. Neutralized the artifact to stock `0 MHz` (`gpuOffsetMHz=0`,
     `gpuOffsetExcludeLowCount=0`, `lockTracksAnchor=0`) identically across
     `profile_desired_from_indexed_document()`, Windows ordinary INI load
     (`config_profile_repair.cpp` / `config_profiles.cpp` before IPC clamping),
     Linux ordinary INI load (`linux_profile_document_load.h` /
     `linux_port_profiles.cpp`), and `profile_document_summary()`, with
     one-line repair logs across all Windows/Linux document and INI loaders.
     All genuinely out-of-range offsets continue to be refused with exact MHz
     and bounds.
- Verified: regression cases `12600-12622` (`tests/profile_slot_runtime_tests.cpp`),
  `tools/ui_gates.py`, `--test`, `--check --target all`, `--tidy`, and full
  deliverable `python build.py`. Live GUI reload of Profile 1 pending user
  install of the freshly built setup.

## 2026-11-20: Upgrade restore of an imported profile lost its tray checkmark (zero-MHz projected points dropped on INI export)

- Reported: installing a new Green Curve version on top restored the running
  settings from Profile 3, but the tray right-click menu still showed no
  checkmark for slot 3.
- Root cause (confirmed from debug log and code):
  1. Profile 3 is stored as a portable indexed document whose 57 curve points
     (`ci=70..126`) are `kind=projection` without `observed_frequency_hz`, so
     `profile_desired_from_indexed_document()` produces valid zero-MHz
     projected points (`hasCurvePoint=1, curvePointFromGpuOffset=1,
     curvePointMHz=0`).
  2. During `--export-active-settings`, `save_profile_to_config()` called
     `profile_curve_point_record_for_save()`, which rejected `record.mhz == 0`
     unconditionally and silently dropped all 57 projected points (same check
     in Linux `linux_profile_write_curve_points()`). The restored apply still
     wrote the exact same selective GPU offset (`+475 MHz`, exclude 70) and
     flatten lock (`ci=76 @ 2957 MHz`) to the GPU, but `g_serviceActiveDesired`
     now held `curvePoints=0` (`ABSENT`) while slot 3 on disk decoded
     `PROJECTED/0`.
  3. Both service claimed-slot confirmation and the GUI `AD_HOC` personal-slot
     scan failed at `ci=70` (`curve intent differs at ci70 (profile=projected/0
     active=absent/0)`), leaving `applied_slot=0`.
- Fix (two commits):
  1. Preserve zero-MHz projected curve points across INI save/load and upgrade
     transfer on Windows, Linux, and `profile_legacy::points()` via shared
     `profile_curve_point_should_emit()` and
     `profile_curve_point_zero_mhz_allowed()` (`source/profile_curve_semantics.h`,
     `source/profile_curve_origin_io.h`, `source/linux_profile_curve_codec.h`),
     skipping zero-MHz projected points in stored-MHz monotonicity checks, and
     reading transfer files with `PROFILE_READ_FOR_OWNERSHIP` with projected
     point count logging.
  2. Add `profile_ownership_projected_point_mismatch_allowed()` in
     `source/profile_ownership_policy.h` (wired into
     `desired_settings_match_active_service_intent()`) so relaxed ownership
     reads (post-write confirmation and the GUI tray indicator) treat
     `PROJECTED` vs `ABSENT` as equivalent when both records agree on
     `hasGpuOffset`, offset/exclusions, authoring, and curve-replacing scope
     (preserving the tray tick even when upgrading directly from an
     already-installed build whose exporter dropped zero-MHz projected points).
     Also record a concise slot mismatch reason in
     `sync_applied_profile_from_service_metadata()` when an `AD_HOC` scan finds
     no matching slot.
- Verified: regression cases `12560-12595`, `tools/ui_gates.py`, `--test`,
  `--tidy`, and full deliverable `python build.py`. Live in-place installer
  replay pending user install of the freshly built setup.

## 2026-11-19: Windows GPU selector snapped back to the service's GPU

- Confirmed from code (deterministic, not timing): Windows state reads
  named no GPU, the service ignored a target on them, and snapshot adoption
  took the service's `selectedAdapterIndex`. A selector choice was saved
  to `[gpu]` and then replaced on screen by the service's GPU.
- Config-driven actions resolve the saved GPU through
  `validate_configured_gpu_selection_for_client()`. Auto-profile and
  app-start Applies were therefore stamped with the shown GPU's
  generation but targeted the saved one, and were refused as stale every
  time. Shared-profile publication bound slots to the saved GPU, although
  their values came from the shown one.
- Fix: Linux's existing rule. A full state read names the GPU the client
  wants, and the service switches under the retarget guard and the
  shared-only policy. A choice is saved only once shown, a refused saved
  GPU is held per attempt key, and client validation refuses an unshown
  saved GPU. The CLI asks once, synchronously, before acting.
- Verified: regression 7090-7099 and source gates. Not run on multi-GPU
  Windows hardware; this is also the per-request GPU routing a future
  per-GPU controller design needs.

## 2026-11-19: Windows service could retarget away from the GPU that owns the active intent

- Found while reviewing two community multi-GPU proposals (both Linux-only).
  Linux already refuses a GPU switch while an intent belongs to another GPU
  (`linux_gpu_switch_preserves_active_intent()`). The Windows service had no
  equivalent check.
- Root cause: `service_prepare_requested_gpu()` (Apply, Reset, reviewed and
  shared-binding Apply) and `service_lifecycle_select_context_gpu()` (logon)
  call `reset_gpu_runtime_selection()` and point `g_app.selectedGpu` at the
  requested GPU whenever it differs. They do this without consulting
  `g_serviceActiveDesiredGpu`. Consequences, from code reading:
  - the owner's clocks and manual fan duty stay in hardware untracked;
  - a non-replacing Apply merges the owner's remaining fields into the new
    GPU's intent, so later restores write them to the wrong card;
  - the fan runtime follows `g_app.nvmlDevice` with the owner's curve;
  - Reset of the other GPU drops the owner's tracking.
- Fix: the pure `gpu_intent_owner_policy.h` (on top of the PCI predicates
  moved from `gpu_backend.cpp` into `gpu_adapter_identity.h`), bound in
  `main_service_intent_owner.cpp` and asked by both sites before they
  retarget. Absent owners can be released only by Reset. Logon refusal is
  terminal (`FAILED`, no write, no retry).
- Verified: regression 7060-7072 and source gates in `gpu_support_gates.py`.
  Not run on multi-GPU Windows hardware.
- Open: the fan runtime has no per-tick owner check (gpu-backend.md, "GPU
  identity and selection"). The suspected selector snap-back was confirmed
  and fixed in the entry above.

## 2026-11-19: Previewing an offset profile over a flatten spiked point 75 to 3047 MHz (driver cross-talk) and zero-MHz projected drafts failed capture

- Reported: after applying Profile 5 (flatten at ci=76 / 925 mV -> 2962 MHz,
  `offset=0` on 45..75, `+480 MHz` at ci=76, `-1000 MHz` on 77..126) and
  loading Profile 3 (`+475 MHz`, exclude first 70, flatten 2957 MHz @ ci=76)
  without clicking Apply, the preview showed `3047 MHz` for point 75 (920 mV)
  instead of `~2932 MHz` (above the 2957 MHz lock at point 76).
- Root cause (two interacting defects confirmed from live capture + debug log):
  1. On Blackwell, raising ci=76 by `+480 MHz` next to untouched ci=75
     (`freqOffsets[75] == 0`) triggers driver VF neighbor cross-talk that pulls
     `curve[75]` up from stock ~2457 MHz to 2572 MHz while `freqOffsets[75]`
     stays `0` (and tail points 77..126 are monotonically clamped at 2962 MHz
     with `-1000 MHz` offsets). Both Windows (`pending_curve_mhz_for_gui_point`
     -> `curve_base_khz_for_point`) and Linux TUI (`tui_point_values`) computed
     stock as raw `live - freqOffsets`, yielding `2572 + 475 = 3047 MHz` at
     ci=75 (and `3962 MHz` on clamped tails when releasing a lock).
  2. Profile 3's portable indexed document stored `kind=projection` points
     without `observed_frequency_hz`, so `profile_desired_from_indexed_document()`
     produced `hasCurvePoint=1, curvePointFromGpuOffset=1, curvePointMHz=0`.
     `gui_draft_capture_desired()` copied `0` / `curveValueValid=false` into
     `g_app.guiDraft`, while `sync_vf_curve_field_values()` hid it in the Win32
     edit boxes -- causing `capture_gui_desired_settings()` (Apply, Save, and
     reload comparison) to fail with `Invalid MHz value for point 70`.
- Fix:
  1. Added pure `source/vf_stock_base_policy.h` combining a topology-bound
     unshaped stock curve cache (`vf_stock_curve_cache_observe()`, thermally
     aligned via median delta over clean zero-offset reference points) with a
     monotonic + concave-down-safe (chord + left-tangent) shaped-curve
     estimator (`vf_estimate_stock_base()`) that interpolates cross-talked
     neighbors by voltage and bounds monotonically clamped tails while leaving
     unshaped curves and clean points untouched. Wired into Windows GUI
     snapshot adoption (`main_state_sync.cpp`, `main_runtime_gpu.cpp` with
     change-gated `gui stock base:` diagnostics, `ui_main_graph.cpp`) and Linux
     TUI (`linux_tui_layout.cpp`).
  2. Extended `gui_draft_desired_point_mhz()` (`source/gui_draft_desired_policy.h`)
     and `resolved_desired_point_mhz_for_gui()` (`main_runtime_gpu.cpp`, wired
     into `gui_draft_capture_desired()` and `populate_desired_into_gui()`) so
     zero-MHz projected pre-tail points resolve from the crosstalk-safe stock
     base + GPU offset component and zero-MHz locked-tail points resolve to
     `lockMHz`; updated `maybe_confirm_profile_load_replace()` to treat matching
     projected points semantically.
- Coverage: `tests/vf_stock_base_tests.cpp` (`12520-12542`, `12544-12550`),
  `tests/regression_main.cpp` (`12543`), and `tools/ui_gates.py`
  (`check_preview_stock_base_crosstalk_protection()`, updated
  `check_loaded_lock_reaches_the_draft()`). Software-verified via `--test`,
  `--check --target all`, `--tidy`, and full deliverable build; live GUI
  re-test pending rebuilt install.

## 2026-11-19: Importing a 5-profile export with slot 5 empty swapped Profile 1 into Slot 5 ("graphics offset is out of range")

- Reported: after exporting all 5 slots to a `.gcprofile` file, clearing slot 5
  (a captured flatten + memory + power profile), and re-importing the file,
  loading Slot 5 failed with `Profile 5: graphics offset is out of range`.
- Root cause (two interacting defects):
  1. Both `gui_transfer_fill_import_rows()` (`source/gui_profile_transfer.cpp`)
     and `tui_transfer_probe()` (`source/linux_tui_transfer.h`) assigned default
     import slots greedily in row order (`empty slots first`). When slots 1..4
     were occupied and only slot 5 was empty, Profile 1 (`i=0`) claimed empty
     Slot 5, and Profile 5 (`i=4`) wrapped around to Slot 1 -- silently swapping
     Profile 1 and Profile 5.
  2. Profile 1 carried a legacy `-1927 MHz` selective graphics offset saved
     before the flat-tail offset-detection fix (`vf_live_offset_policy.h`). The
     ordinary INI loader had silently clamped it via
     `validate_desired_settings_for_ipc()`, whereas portable document loading
     (`profile_desired_from_indexed_document()`) properly refuses offsets
     outside `-1000..1000 MHz`. Because `gui_transfer_import_note()` skipped
     indexed records (`!profile_document_requires_destination(document)`), the
     Import dialog gave no warning before committing, and `profile_slot_runtime.h`
     also lacked a range check on legacy-scope memory offsets and omitted the
     offending MHz value and bounds from the error message.
- Fix: extracted pure `profile_transfer_default_slots()` in
  `source/profile_transfer_slot_policy.h` (shared by Windows GUI and Linux TUI):
  when all records fit in empty slots, no saved slot is overwritten; when saved
  slots must be replaced, records first claim their own empty preferred slots
  (so a 5-profile import always maps `1..5 -> 1..5` across all 32 occupancy
  masks) and remaining empty slots match monotonically to the closest unassigned
  rows. Wired both `gui_transfer_import_note()` (enlarging import note height to
  54 dp) and `tui_transfer_refresh()` to pre-check both indexed and
  voltage-bound checked records via `profile_slot_document_runtime()`, added
  exact MHz/bounds wording plus legacy-scope memory bounds in
  `source/profile_slot_runtime.h`, and logged default/queued `sources` and
  `slots` mappings on both platforms.
- Coverage: `tests/profile_tools_tests.cpp` (`9992-9996`),
  `tests/profile_slot_runtime_tests.cpp` (`12500-12504`),
  `tests/tui_profile_tools_tests.cpp` (`9792-9793`), and
  `check_import_transfer_slots_and_runtime_check()` in `tools/ui_gates.py`.
  Software-verified via fixtures and full Windows deliverable build; live GUI
  re-import check pending rebuilt install.

## 2026-11-19: Near-limit flatten target (2450 MHz @ 820 mV) gave generic range error instead of planner diagnostic

- Reported: with a 1425 MHz stock base at 820 mV and `+1000` MHz offset limit,
  applying a 2600 MHz flatten (+1175 MHz) refused before touching hardware with
  the full planner message (`2600 MHz at 820 mV requires a +1175 MHz point
  adjustment... Nothing was changed.`), whereas 2450 MHz (+1025 MHz) reset to
  stock and reported `"A requested curve target is missing or outside the driver
  range. Recovery verified core defaults"`.
- Root cause: `curve_plan_evaluate()` (`source/curve_feasibility_plan.h`) gated
  hard offset-range bounds (`ANCHOR_ABOVE_RANGE`, `ANCHOR_BELOW_RANGE`,
  `POINT_OUT_OF_RANGE`, `TAIL_ABSOLUTE_TARGET`) behind `beyondBin()` (the local
  neighbouring-point frequency spacing, ~30 MHz at 820 mV). The +25 MHz overrun
  stayed `CURVE_PLAN_NEEDS_FRESH_BASELINE` on both `UNSHAPED_LIVE` and the
  post-reset `TRANSACTION_BASELINE`, so both pre-check and backstop skipped the
  refusal and fell through to `apply_build_curve_targets()`, which strictly
  rejects any offset outside `[min, max]` and returned the generic range error.
- Fix: removed `beyondBin()` from hard offset-range checks so any out-of-range
  offset is proven whenever `baselineProves` is true (`UNSHAPED_LIVE` or
  `TRANSACTION_BASELINE`), while keeping `beyondBin()` for frequency-vs-plateau
  comparisons (`PREFIX_ABOVE_PLATEAU` and uniform-min FLATTEN
  `TAIL_UNREACHABLE`) and keeping `LIVE_SHAPED_ESTIMATE` conditional. Also
  floored `anchorOnlyMaxKHz` display (`curve_plan_floor_mhz()`), appended exact
  Hz on sub-MHz rounded range collisions, added `CurveTargetBuildFailure` +
  `curve_plan_format_target_failure()` / `apply_format_curve_target_build_failure()`
  so post-plan target-builder rejections (e.g. non-reset delta applies or
  post-scalar curve shifts) emit the same physical wording instead of the
  generic range string, and wired planner lead-issue formatting into authoring
  and Linux target-build failure fallbacks.
- Coverage: `tests/curve_plan_tests.cpp` (`12480-12494`),
  `tests/curve_plan_precision_cases.h` (`8765`),
  `tests/clock_transition_tests.cpp` (`CurveTargetBuildFailure`), and
  `tools/apply_ceiling_gates.py`. Hardware GUI re-test pending rebuilt install.

## 2026-11-19: External flatten tail detected as -1927 MHz selective GPU offset (exclude 76)

- Reported: applying a tail flatten at point 76 (925 mV -> 2962 MHz) in another
  curve editor without touching lower VF points or setting a global GPU offset
  displayed `GPU Offset (MHz): -1927` and `Exclude first 76 VF points` in Green
  Curve alongside the projected lock at point 76.
- Root cause: another tool flattens a tail by writing a large constant negative
  delta (e.g. `-1927` MHz) to every tail point (`77..126`) and relying on the
  driver's monotonic clamp. Because the service intentionally leaves
  `g_app.lockedCi == -1` for unowned external flattens
  (`gpu_lock_ownership_policy.h`), `detect_live_selective_gpu_offset_state()`
  (`is_locked_tail_detection_point()`) stopped skipping the flattened tail and
  matched `0..75` (~0) vs `77..126` (`-1927` MHz) as a selective offset;
  `uniform_curve_offset_khz()` also included tail points, so a flatten below the
  midpoint would similarly report `-1927` MHz as a uniform offset.
- Fix: shared `vf_recognize_programmed_flat_tail()` in
  `source/vf_flat_tail_recognition.h` (reused by `gui_observed_lock_policy.h`)
  and extracted pure `source/vf_live_offset_policy.h`
  (`vf_offset_detection_tail_start()`, `vf_uniform_curve_offset_khz()`,
  `vf_detect_live_selective_gpu_offset()`,
  `vf_live_selective_gpu_offset_matches_shape()`), wired into
  `source/main_gpu_state.cpp` and `source/gpu_backend.cpp` (lowering both file
  ratchets below 800 lines) plus actual detected exclude-count logging in
  `source/main_gpu_front.cpp`. Stock level tops remain unexcluded; owned locks
  and genuine uniform/selective offsets (including combined with an external
  flatten) are preserved.
- Coverage: `tests/vf_live_offset_tests.cpp` (`12450-12474`),
  `check_live_offset_excludes_programmed_flat_tail()` in `tools/ui_gates.py`.
  Hardware live-curve verification pending rebuild install.

## 2026-11-19: GUI Load -> Apply of a legacy locked profile lost the tray slot tick

- Reported: loading and applying a saved locked profile slot from the main
  window succeeded, but the tray context menu showed no active-slot checkmark
  and the tooltip reported "Manual settings".
- Root cause: the slot was saved in the legacy `curve_semantics=base_plus_gpu_offset`
  format with an explicit lock target. `restore_curve_points_from_base_plus_gpu_offset()`
  (and Linux's matching loop) reconstructed the locked tail (`ci >= lockCi`)
  back to `lockMHz` but marked every restored point `curvePointFromGpuOffset=1`
  (`DESIRED_CURVE_PROJECTED`), whereas `capture_gui_desired_settings()` emits
  locked-tail points as `DESIRED_CURVE_ABSOLUTE` (`curvePointFromGpuOffset=0`).
  Both pre-write and post-write service identity checks rejected the slot at
  the lock anchor (`curve intent differs at ci76`) and recorded `AD_HOC`.
  Tray/logon applies had stayed unaffected because they sent the loaded record
  verbatim without passing through GUI capture.
- Fix: shared provenance helpers (`desired_base_plus_offset_point_is_projected()`,
  `desired_clear_explicit_lock_tail_projection_flags()` in
  `source/desired_curve_intent.h`) keep explicit locked-tail points matching
  `lockMHz` absolute in Windows/Linux legacy restore and after inferred-lock
  detection, while preserving pre-tail projections and `lockMHz == 0` anchor
  provenance (`gpu_backend_apply_targets.h` also skips explicit lock-tail
  points when counting provenance points). Relaxed ownership reads
  (`profile_ownership_lock_tail_mismatch_allowed()` in
  `source/profile_ownership_policy.h`, `source/desired_settings_helpers.cpp`)
  also accept lock-consistent tail representations (`absent`, `absolute` at
  `lockHz`, or `projected` at `0`/`lockMHz`) once both records agree on the
  explicit lock and authoring, and curve-mismatch diagnostics now report both
  sides' kind and value.
- Coverage: `tests/profile_lock_tail_ownership_tests.cpp` (`12400-12427`,
  `12433-12447`, executing production `desired_settings_match_active_service_intent()`),
  `tests/regression_main.cpp` (`12428-12432`, executing production
  `restore_curve_points_from_base_plus_gpu_offset()`), and gates in
  `tools/ui_gates.py`.
- Follow-up (same day): after upgrading from the pre-fix build, the settings
  transfer re-applied the active intent with the old build's `AD_HOC` identity,
  so the tray still showed no tick even though the active intent matched slot 3.
  Both `service_confirm_profile_metadata_from_active_intent()` and
  `sync_applied_profile_from_service_metadata()` now scan personal slots
  `1..CONFIG_NUM_SLOTS` drift-free (`PROFILE_READ_FOR_OWNERSHIP`) when ownership
  would otherwise stay `AD_HOC` (`service_profile_identity_unique_user_slot_match()`,
  `APPLIED_PROFILE_REASON_MATCHED_AD_HOC_SLOT` / `AMBIGUOUS_AD_HOC_SLOT`),
  resolving any unique personal-slot match (upgrades, Save of active settings,
  CLI applies, combo moved before Apply) while keeping ambiguous duplicates
  unticked and shared/machine banks isolated. Hardware upgrade re-test pending.

## 2026-11-19: Reset after another tool's reset stalled for ~50 s

- Reported: apply a profile in another tool, start the GUI, reset in that
  tool, then Reset in Green Curve: the Reset stalled and the GUI reported
  "outcome is unknown after transport timeout". The debug log showed 51
  single-point VF writes at ~1 s each (52 s total) for points whose
  `batch left readback` was the other tool's old flatten, while the hardware
  was already at stock; the batch pass had written nothing.
- Root cause: `apply_curve_offsets_verified()` compared targets with a fresh
  control table for the batch, but judged leftovers against the service's
  cached snapshot, which the external reset had invalidated. Fix: the writer
  adopts its entry table into the cache before any convergence check
  (`gpu_backend_curve_write.h`, `vf_control_offsets_decode.h`) and logs how
  many cached offsets were stale. Coverage 12350-12366 runs the production
  writer against a fake control table.
- Lesson: a convergence check must read the same moment it compares against;
  a cache that only this process updates is not evidence about hardware other
  tools can also write.
- Same class, second fix: Reset decided memory and power from the snapshot,
  so a memory offset raised elsewhere after it survived Reset. Reset now
  re-reads those controls (and the ClkDomains values) first
  (`reset_live_readback_policy.h`, 12380-12385).
- Requested follow-up: register external changes without pressing Refresh,
  but with no background polling. Chosen: re-read on `WM_ACTIVATEAPP` only,
  gated and throttled (`gui_activation_resync_policy.h`, 12386-12397).
  Changing settings in another tool means leaving this window, so returning
  is when the snapshot may be stale; a timed poll was rejected because a
  full read holds the service's hardware lock for ~90 ms.

## 2026-11-19: Editor Load->Apply/Save keep an imported record's intent

- The editor rebuilt Apply/Save requests from its whole-MHz fields, dropping
  captured offsets, exact Hz, power ratios, topology binding and the explicit
  scope of imported/captured slots; ownership read "Manual settings". With an
  explicit active intent, a later lock edit put retained captured points into
  a legacy request the service refuses.
- Fix: the editor keeps a base record (loaded explicit slot, else an explicit
  active intent) and overlays only edited domains and typed points
  (`gui_editor_base_policy.h`, `gui_editor_base.cpp`); an unedited Load->Apply
  equals the tray request. Coverage 12320-12333 plus a ui gate.
- Lesson: once a record holds intent the fields cannot show, keep the record
  and send the edits as a delta instead of re-deriving it from the fields.

## 2026-11-19: Imported flatten applied the live clock; imported memory read as stock

- Reported: Import recognized another tool's 925 mV / 2962 MHz flatten and
  saved it to a slot (`lock_mhz=2962`, no explicit anchor point), but Load then
  Apply from the editor locked 2482 MHz, the stock clock at that point. Load's
  `set_gui_state_dirty(true)` rebuilds the draft from the applied/live curve
  and `gui_draft_capture_desired()` copied only explicit points, while Apply
  reads the lock target from the draft anchor. The copy now writes the lock
  target at the anchor (`gui_draft_desired_point_mhz()`).
- Reported: the same import showed memory as stock although the other tool had
  set +3000 MHz. The debug log had it: capture read the NVML getter (0 on P0
  and global) while the live state's Pstates20 derivation held 3000000 kHz.
  Capture now uses that derivation (`memory_clock_evidence.h`, schema 5
  `memoryEvidence`); the live state calls the same helper.
- Coverage: 12300-12319 plus `check_loaded_lock_reaches_the_draft()`.
- Follow-up (done the same day, entry above): the editor model could not hold
  captured offsets or explicit scope, so Load->Apply of an imported slot
  dropped captured points and never claimed slot ownership. Hardware check
  with another tool's memory offset still pending.
- Lesson: when two readers of the same control exist (live state and capture),
  give them one helper; the divergence was already visible in the log.

## 2026-11-19: Editor projects another tool's flatten as its lock

- Reversed the 2026-11-15 rule **for the Windows editor only** (`gui_observed_lock_policy.h`,
  `main_observed_lock.cpp`, `main_state_sync.cpp`): when the service reports no
  lock, the editor projects a live external flatten (anchor checkbox, greyed tail,
  plateau target) and mirrors it into `g_app.appliedLock*` so it is not a
  pending change. The service still never adopts external locks.
- Stock level tops and uniform VF-table offsets are excluded by requiring a tail
  point with an offset different from the anchor's. Unsaved drafts are never
  overwritten, and the projection withdraws when the flatten leaves the GPU or
  Green Curve's own lock takes over.
- Save (`profile_save_policy.h`, `ui_profile_save_notice.cpp`,
  `gui_profile_commands.cpp`) labels the slot as an external flatten, reports
  unsaved lower points, and offers `Import current GPU settings`.
- Coverage: `tests/gui_observed_lock_tests.cpp` (`12290-12299`) and a source
  gate in `tools/ui_gates.py`; live GPU/GUI verification pending hardware check.

## 2026-11-19: Import current GPU settings missed another tool's flatten

- Reported: a curve flattened at 925 mV / 2482 MHz by another tool was only
  offered as "copy every point exactly", and lock detection asked the user to
  start a game first. The CLI capture recognized the flatten (anchor, target,
  tail 50, deviation 0); the GUI did not, because its default domains add the
  advanced clocks, whose capture scalars had no setter flag, so the control
  plan of every lock proposal was "unavailable". Representations are now
  judged on the curve, core offset and lock only; advanced scalars carry the
  setter flag.
- The other tool flattens by writing -2045 MHz to every tail point (beyond
  the +/-1000 MHz range Green Curve uses) and relying on the driver's
  monotonic clamp. An exact copy is therefore refused at Apply; it is now
  labeled so, and a selectable flatten is recommended instead.
- Load: read-only probes with the user holding each lock in turn (another
  tool's VF point lock, NVML 1500/1500, NVML 0/1500) showed the NvAPI client
  lock table reports all three idle; NVML range/mode getters reported none.
  A pin or voltage lock also holds the idle clock high, a ceiling does not.
  Capture now reads the table (schema 4); no workload or load generator needed.
- Follow-ups: validate the table on other generations, Linux and memory locks;
  decide whether the per-point VF range should follow what the driver accepts.
  The installed service was not updated in this pass, so the GUI flow is
  verified by fixtures plus a live decoder read, not end to end.

## 2026-11-19: Reset left the old lock in the GUI and tray

- After a successful Reset of a flatten-lock profile the driver was at stock,
  but the RESET envelope still reported the lock, so the GUI re-adopted it
  (lock checkbox, flattened tail, "Lock:" header) and the tray showed OC.
  Cause: the live-lock ownership gate (2026-11-15) made detection return
  early for unowned state without clearing the markers, and Reset relied on
  detection to clear them after dropping the intent; the snapshot then read
  the stale markers. The regression never reached a release.
- Fix: Reset clears the markers itself once the lock domain verified and the
  restriction released (`service_reset_lock_policy.h`), before its post-reset
  refresh; a retained cap keeps them. The snapshot lock projection moved into
  the same header. Coverage: `tests/service_reset_lock_tests.cpp`
  (12240-12252) plus order gates in `tools/apply_ceiling_gates.py`.
- Lesson: when a gate stops a refresh from touching state it does not own,
  audit every caller that relied on that refresh to CLEAR the state.

## 2026-11-19: TUI source-document mode removed

- Nothing set the TUI's source-document state after Load began converting
  imported slots, so Check, mapped draft, conversion, tail restore, curve
  assistance and decimal source-field input were unreachable. Deleted with
  `linux_tui_source_edit.h`, `_source_check.h`, `_mapped_draft.h`,
  `_document_view.h`, `_review_lifetime.h` and the orphaned
  `profile_editor_document.h` / `_source_edit.h` / `_source_rows.h`. The TUI
  pending-load candidate is a plain `DesiredSettings`.
- Coverage was kept, not dropped: tests that exercised shared engines through a
  TUI wrapper now call the engine directly (`profile_editor_check_tests.cpp`,
  `profile_mapped_draft_tests.cpp`, `profile_document_slot_tests.cpp`,
  `tui_capture_context_tests.cpp`); the capture-expiry half of the lifetime cases
  stays. `check_canonical_tui_documents()` now forbids the deleted files and
  tokens instead of pinning them. Details: `config-profiles.md`, "Profile tools".
- Lesson: a removed header can carry an include other code relied on; the Linux
  compile check (`--check --target linux`) caught the missing capture-transport
  include, the Windows suite did not.

## 2026-11-19: Profile capture, import and export rebuilt around ordinary slots

- Imported and captured profiles were stored as portable documents that every
  slot reader refused, so they never worked from the tray, hotkeys, auto-switch,
  app start or logon, and the dialogs exposed engine internals. Slot readers now
  convert documents (`profile_slot_runtime.h`); capture saves an ordinary record;
  import/export dialogs were rebuilt on a probe and explicit record/slot choices,
  for Windows and the TUI. Details: `config-profiles.md`, "Profile tools".
- `DrawText` without `DT_NOPREFIX` rendered "Profiles & tools..." with an
  underline; a ui gate now requires the flag on every call.
- Found by the Linux compile check, not the Windows suite: the Linux document
  loader missed an include. Windows `--test` does not compile Linux-only `.cpp`
  files, so run `--check --target linux` after touching them.
- Export failed for real Windows slots ("legacy points require an explicit
  source curve-semantics choice", and the curve section's exclusion copy was an
  unsupported key), and slots saved by the new capture dialog would not have
  exported either (extension keys). Ordinary INI slots now decode as their runtime
  record (`profile_ini_slot.h`, `config-profiles.md`). Caught only by a desktop
  pass and a CLI export of real slots: fixtures had used minimal INI text, so
  tests now write slots in the writers' real layout.
- The capture dialog's core-clock row said "Not readable" when the offset was read
  but its relation to the curve is unknown; it now shows the value and that it
  cannot be saved separately. The export hint is no longer drawn as a warning.
- Open: the Windows Import dialog still needs a desktop pass. (The unreachable
  TUI source-document editing code was removed the same day; entry above.)

## 2026-11-18: CI Linux jobs pin the runner image

- Every Linux job (`linux`, `release-packaging`, `secrets`) and the release and
  freshness workflows used `ubuntu-latest`. It moved to Ubuntu 26.04, where
  `actions/setup-python` has no 3.12.10, so each job died in "Set up Python"
  and main went red with no repository change involved.
- Fix: `ubuntu-24.04` everywhere, plus a source gate
  (`_workflow_runner_errors()` in `tools/security_gates.py`) that rejects
  floating `ubuntu-*` labels, with a self-test fixture for both forms. Details in
  `build.md`, "CI release gates".
- Not changed: `windows-latest` (still green).
- Follow-up from the first run on the pinned image: the Linux job had not run
  past "Set up Python" since 2026-11-05, and its ASan step then failed to compile
  `tests/tui_capture_proposals_tests.cpp` (`std::strcmp` without `<cstring>`;
  only libc++ supplied it transitively). Fixed with explicit standard includes;
  the full regression suite, fuzzing of the other targets and Linux packaging
  pass in a WSL-native clone. The `profile_codec` fuzz abort seen there came
  from that machine's clang 22; CI's clang 18 passed every Linux step
  (`testing.md`, "Host-clang libstdc++ parity").

## 2026-11-18: GPU support warning tiers judge the service snapshot

- Found while adding the CMP tier: the unrecognized-family and reduced-surface
  predicates began with `g_app.vfBackend`, which the GUI never sets while it
  runs through the service, so neither could fire in the GUI (GUI sessions in
  the local debug log show no backend selection).
- Fixing only that would have warned every board without a pinned ClkDomains
  schema: the CMP 40HX snapshot carries XBAR/SYS/VIDEO refused
  (`missingMask=0x380`). The reduced-surface trigger and its text now consider
  board-level domains only (`gpu_capability_board_domains()`).
- `source/gpu_support_warning_policy.h`; tests 12230-12237; gates in
  `tools/gpu_support_gates.py`.
- Not changed: the self-test's surface class and verdict still count the
  advanced domains, so such boards report `PARTIAL` there.

## 2026-11-18: Skippable startup warning for NVIDIA CMP mining boards

- Requested with the CMP 40HX support (issue 13): guard it behind a warning the
  user can skip once or for good. New third GUI tier between the family and
  reduced-surface tiers, abortable, opt-out `[warnings] hide_cmp_board_warning`.
- Identity: confirmed id `0x1F0B` or the `CMP <n>HX` name
  (`source/gpu_cmp_policy.h`); reads the snapshot's adapter identity, not the
  VF backend. Self-test prints a NOTE. Tests 12220-12227.
- Found while wiring it: the two older tiers keyed on `g_app.vfBackend`, which
  the service-mode GUI does not set; fixed in the entry above.

## 2026-11-18: VF status requests stop at the 128-point table

- Report (issue 13): an NVIDIA CMP 40HX on a modified driver loaded no curve.
  The reporter's prototype source (based on 0.26.0) and debug log show the
  architecture query answering `TU100` (so the backend was already Turing) and
  a 131-point VF-info mask; status refused the request naming points 128..130.
- Fix: `source/vf_point_mask_policy.h`; every VF status request names points
  0..127 only (Windows, Linux reader/probe, capture, self-test). The VF-info
  cache moved to `source/main_vf_info_cache.cpp` (NVML shard ratchet lowered to
  827) and logs the mask extent; the self-test compares bounded and full status
  reads.
- Not adopted from the prototype: the PCI device-id family fallback (not
  needed, unconfirmed ids) and a two-attempt status read.
- Tests 12200-12208; gates `tools/gpu_support_gates.py`. RTX 5070 self-test:
  132 advertised points, bounded and full reads identical. No CMP hardware run.

## 2026-11-18: Linux snapshot gates honour the inert power request

- Follow-up to F-FANLESS: the Linux daemon precondition gate and the TUI gates
  (draft binding, edit staging, Apply, profile-load status) never applied the
  F-POWER-SURFACE inert rule, so on a board without a readable power target a
  saved profile (mandatory `power_limit_pct=100`) stayed a detached TUI draft.
  Found by code reading, not a report.
- Fix: `service_snapshot_missing_mutation_domains()` exempts board-default
  power when POWER is absent from the published domains; the predicate
  `desired_power_request_is_board_default()` is shared with
  `linux_power_request_is_inert()`. The helper header was renamed to
  `source/service_inert_request_domains.h`.
- Tests 12155-12159 (TUI binding on a powerless and fanless snapshot); gates
  in `tools/readback_gates.py`. Not run on hardware.

## 2026-11-18: Apply works on GPUs without a controllable fan (F-FANLESS)

- Report: on an RTX 4080 Laptop GPU the fan controls were disabled at Auto and
  no undervolt could be applied.
- Windows root cause: `fan_setting_matches_current()` returned false whenever no
  fan was reported, so every GUI Apply carried a fan-auto request; the service
  attempted `nvml_set_fan_auto()`, which refuses without a fan, and that core
  failure rolled back the whole Apply. Linux refused any fan-owning request as
  an unavailable domain, so saved profiles (always carrying `fan_mode`) failed.
- Fix: shared pure rule `source/fan_request_policy.h` (Auto inert, Fixed/Curve
  refused before writes, positive evidence required), wired into the new
  Windows shard `source/main_fan_apply.h` (extracted from
  `main_fan_runtime.cpp` / `main_runtime_nvml.cpp`, size ratchets lowered), the
  GUI capture/profile-load paths, Linux preflight, transaction phase and daemon
  gate. Linux now classifies a reported zero-fan board ABSENT like Windows.
- Linux TUI: draft binding, edit staging, Apply and profile-load status now use
  `source/service_inert_request_domains.h`, the same snapshot-based missing-domain
  mask as the daemon gate; Auto stays selectable on a fanless GPU. The Linux fan
  setters moved to `source/linux_backend_fan_control.h` (size limit).
- Tests: `tests/fan_request_apply_tests.cpp` (12100-12136, executes the Windows
  fan step with a scripted driver; mutation-checked: the pre-fix predicate
  fails 12120), `tests/linux_scope_preflight_tests.cpp` (12140-12145),
  `tests/regression_main.cpp` TUI binding (12150-12154), `tools/fan_gates.py`
  `check_fanless_request_rule()`.
- Unverified: no notebook hardware run.

## 2026-11-16: Fix Win32 `WndProc` stack overflow (`0xC00000FD`) under ThinLTO

- Root cause: under release `-Oz -flto`, ThinLTO inlined single-caller UI/review helpers directly into `WndProc` (`ui_main_window.cpp`), where value-reset assignments (`review = {}`, `output = {}`, `g_app.guiSource = {}`) materialized ~180–192 KB stack temporaries for `ProfileDestinationReview`, `ProfileConversionPlan`, and `ProfileEditorDocument` (`WndProc` frame reached `0x31788` = 202,632 bytes per message). Minimizing to tray (`ShowWindow(SW_HIDE)`) triggered synchronous Win32 re-entrancy through `WndProc` (`WM_SYSCOMMAND` -> `WM_SHOWWINDOW` -> `WM_IME_SETCONTEXT` -> `WM_IME_NOTIFY`) and overflowed the 1 MiB UI thread stack.
- Added in-place `profile_*_reset()` helpers (`source/profile_document.h`, `source/profile_control_feasibility.h`, `source/profile_conversion_batch.h`, `source/profile_domain_conversions.h`, `source/profile_destination_review.h`, `source/profile_editor_document.h`) and moved large `ProfileMappingReport`, `ProfileDestinationTopology`, `ProfileDocument`, `CurvePrefixRepair`, `DesiredSettings`, and path buffers onto `std::make_unique` in UI/review/assistance/proposal paths (`source/profile_runtime_resolution.h`, `source/profile_check_commands.h`, `source/profile_curve_assistance.h`, `source/profile_capture_proposals.h`, `source/profile_transfer_window.h`, `source/ui_curve_prepare.cpp`, `source/ui_main_apply_dispatch.h`, `source/gui_profile_commands.cpp`, `source/gui_profile_source.cpp`, `source/gui_profile_source_review.cpp`).
- Added `GC_NOINLINE` in `source/platform.h` and extracted heavy message/command branches out of `WndProc` (`source/ui_main_window.cpp`), `GuiUpdateDialogProc` (`source/gui_update_dialog.cpp`), `AutoProfileDialogProc` (`source/auto_profile_dialog.cpp`), `apply_changes` / `reset_curve` (`source/ui_main_apply_dispatch.h`, `source/ui_main_apply.cpp`), and capture/transfer handlers (`source/gui_profile_capture.cpp`), reducing all `WNDPROC` / `DLGPROC` stack frames below 2 KB.
- Added regression checks in `tools/ui_gates.py` (`check_wndproc_stack_discipline()`), `tests/profile_conversion_tests.cpp` (`8485-8491`), and `tests/profile_source_window_tests.cpp`, plus a concise bullet under `### Fixed` in `CHANGELOG.md`.

## 2026-11-16: Final acceptance fuzzing, wiki audit, and deliverable build (build 373)

- Extended `tests/profile_codec_fuzz.h` with structured `curve_plan_evaluate()`
  and `profile_pin_observation_analyze()` property checks (extreme ranges,
  overflows, invalid masks, non-mutation, and bounded outputs) and added
  sanitized seeds (`mixed-kinds.gcprofile`, `malformed-relations.gcprofile`)
  under `tests/fuzz-corpus/profile_codec/`.
- Ran all 9 libFuzzer targets (`--fuzz --fuzz-runs 5000`) under ASan + UBSan with
  zero findings (`profile_codec` reached cov 2085 / ft 4006).
- Completed semantic audit across `llm-wiki/*.md` (no orphan pages; cleared
  stale `pending`/`gated` notes in top summaries against completed authoring,
  replay, assistance, and Tier 3 pin observation docs) and verified `CHANGELOG.md`
  `## Unreleased` stays under the ~400-word budget.
- Completed plain deliverable `build.py` (build 373) and verified all 12
  expected packages (Windows MSVC-ABI + release x64/arm64 installers/7z and
  Linux x64/arm64 tarballs/Arch packages) and all 20 packaged application/service
  binaries against fresh timestamps, SHA-256 sidecars, and extracted bytes.
- Hardware boundaries remain explicitly unverified on physical hardware
  (RTX 5080 and Pascal live replays, live hardware burst sampling, live
  multi-GPU/driver-error injection, and physical native ARM64 runs).

## 2026-11-16: User-started workload pin observation (Tier 3)

- Added pure analyzer `source/profile_pin_observation.h` and isolated read-only
  NVML burst sampler `source/nvml_pin_observation_sampler.h` (bound by exact
  captured `HardwareCaptureBinding` PCI address, no sleeps/writers, frozen wire
  protocols unchanged), plus Tier 1/2 supremacy and confirmation helpers
  (`profile_capture_pin_has_stronger_tier()`,
  `profile_capture_apply_pin_observation()`,
  `profile_capture_report_observation_result()`) in
  `source/profile_capture_proposals.h`.
- Wired explicit user-triggered observation and MHz confirmation/editing across
  CLI (`--observe-pin`, `--confirm-pin-mhz` in `profile_capture_commands.h`,
  `profile_commands_windows.h`, `profile_commands_linux.h`, `main_cli_help.cpp`,
  `linux_cli_options.cpp`), Windows GUI (`ProfileSourceWindow`,
  `gui_profile_capture.cpp`, `gui_profile_source.cpp`), and Linux TUI
  (`linux_tui_capture_draft.h`, `linux_tui_layout_fan_profiles.cpp`,
  `linux_tui_actions.cpp`, `linux_tui_layout.h`, `linux_tui_internal.h`), keeping
  passive capture idle-safe and preserving `user_confirmed` vs `user`
  provenance.
- Verification: `--gates`, `--test`, `--test --asan`, `--check --target all`
  (all 10 Windows/Linux targets), `--tidy` (0 new findings), and WSL Linux UBSan
  regressions passed, with new regression coverage in
  `tests/profile_pin_observation_tests.cpp` (`9900-9918`),
  `tests/profile_capture_commands_tests.cpp` (`9920-9925`),
  `tests/gui_profile_capture_tests.cpp` (`9930-9935`),
  `tests/profile_source_window_tests.cpp` (`9936-9939`), and
  `tests/tui_capture_proposals_tests.cpp` (`9940-9946`). Live physical-GPU
  burst sampling remains unverified without hardware.

## 2026-11-16: Compatible-curve assistance (prefix repair + closest-target search)

- Extended `curve_prefix_repair_prepare()` (`convertOffsetIntent`) to retain
  exact captured-offset intent under proven baselines, convert `PROJECTED`
  prefix points explicitly to typed values, and refuse shaped live baselines
  with `CURVE_PREFIX_REPAIR_BASELINE_REQUIRED`.
- Created `source/profile_curve_assistance.h` implementing `PROFILE_ASSIST_PREFIX`,
  `PROFILE_ASSIST_CLOSEST_TARGET`, and `PROFILE_ASSIST_COMBINED`: extended-GCD
  CRT grid intersection (`1 MHz` pin grid + anchor `vfGrid`), bounded `<= 65536`
  descending search via the shared complete planner (without binary-search
  monotonicity assumptions), distinct `NO_COMPATIBLE_TARGET` vs `INDETERMINATE` vs
  `ESTIMATE` vs `BASELINE_REQUIRED` statuses, and revision/capture binding.
- Wired `--prepare-curve prefix|closest-target|combined` into
  `profile_check_commands.h` + CLI help/options, confirmation flow into Windows
  `ProfileSourceWindow` (`gui_profile_source_review.cpp`,
  `gui_profile_source.cpp`), and two-step prepare+accept controls into Linux TUI
  (`linux_tui_layout_fan_profiles.cpp`, `linux_tui_mapped_draft.h`,
  `linux_tui_render.cpp`, `linux_tui_actions.cpp`, `linux_tui_source_check.h`).
- Verification: `--gates`, `--test`, `--test --asan`, `--check --target all`,
  `--tidy` (0 new findings), and WSL Linux UBSan regressions passed, with new
  tests in `profile_curve_assistance_tests.cpp` (`9800-9825`),
  `profile_check_commands_cases.h` (`9830-9836`),
  `gui_profile_source_review_tests.cpp` (`9840-9845`),
  `profile_source_window_tests.cpp` (`9850-9853`),
  `tui_profile_check_tests.cpp` (`9860-9861`), and
  `profile_mapped_draft_tests.cpp` (`9870-9874`). Hardware replay remains
  unverified.

## 2026-11-16: Per-domain batch conversions and captured-tail restore

- Added `source/profile_domain_conversions.h` with shared per-domain batch
  conversion planning/acceptance (`profile_conversion_plan_build()`,
  `profile_conversion_accept_item()`, refusing voltage batches, unknown grids
  and constraint-breaking rounding) and no-recapture captured-tail restoration
  (`profile_document_restore_captured_tail()`).
- Wired `PROFILE_COMMAND_CONVERSION_REQUIRED` (`4`), `--convert-domain`,
  `--restore-captured-tail`, and `--output` into `profile_check_commands.h` +
  CLI help text, preserving unmodified source files and leaving editor preview
  decisions intact.
- Exposed conversion review/acceptance and exact captured-tail restore in Windows
  `ProfileSourceWindow` (`gui_profile_source_review.cpp`,
  `gui_profile_source.cpp`) and Linux TUI (`linux_tui_layout_fan_profiles.cpp`,
  `linux_tui_mapped_draft.h`, `linux_tui_render.cpp`, `linux_tui_actions.cpp`),
  fixing a post-reset `ProfileDestinationReview` use-after-free caught by ASan.
- Verification: `--gates`, `--test`, `--test --asan`, `--check --target all`,
  `--tidy` (0 new findings), and WSL Linux UBSan regressions passed, with new
  assertions in `profile_conversion_tests.cpp` (`8475-8484`),
  `profile_check_commands_cases.h` (`9120-9127`),
  `gui_profile_source_review_tests.cpp` (`9394-9397`),
  `profile_source_window_tests.cpp` (`9411-9416`),
  `tui_profile_check_tests.cpp` (`9275-9276`), and
  `profile_mapped_draft_tests.cpp` (`9678-9680`). Hardware replay remains
  unverified.

## 2026-11-16: Activate exact/captured replay, extended INI persistence, and backend fixes

- Activated runtime authoring/captured replay (`desired_authoring_runtime_ready()`,
  `service_protocol_validation.h`, `service_scoped_apply_policy.h`,
  `service_lifecycle_policy.h`, `profile_check_commands.h`) and switched
  replacement callers (`desired_prepare_replacement_request()`) so scoped
  profiles promote cleanly to `DESIRED_SCOPE_REPLACE_PROFILE` without forcing
  `resetOcBeforeApply`, while legacy configs retain reset-before-apply.
- Added shared `source/profile_desired_ini_ext.h` wired across Windows and Linux
  INI persistence (`config_profiles.cpp`, `linux_port_profiles.cpp`,
  `main_shell.cpp`, `main_runtime_capture.cpp`, `config_profiles_ui.cpp`),
  keeping scoped unowned domains omitted, skipping legacy coercions on scoped
  profiles, validating before IPC canonicalization, and resolving
  `PROFILE_SCOPE_EXPLICIT_RELEASE` in `profile_runtime_resolution.h` and
  `profile_control_feasibility.h`.
- Fixed Windows `authoringCurveWrite` pre-write evidence (`apply_authoring_curve_evidence`),
  `apply_authoring_prepare_verify_desired`, `apply_authoring_flatten_tally`
  (`gpu_backend_apply_authoring.h`, `gpu_backend_apply_diagnostics.h`,
  `gpu_backend_apply.cpp`), and Linux daemon rollback preimage capture on
  released/switched named graphics surfaces (`linux_graphics_offset_control.h`,
  `linux_backend_mutation.cpp`, `linux_daemon.cpp`).
- Verification: `--gates`, `--check --target all`, `--tidy`, `--test`,
  `--test --asan`, and WSL Linux UBSan suites passed cleanly, with new cases in
  `captured_intent_runtime_tests.cpp` (`8061-8071`),
  `profile_control_feasibility_tests.cpp` (`9115-9117`),
  `windows_vf_exact_tests.cpp` (`8783-8788`), and
  `linux_clock_offset_tests.cpp` (`9003-9005`). Hardware replay remains
  unverified. See
  [persistence-schema.md](../persistence-schema.md#exact-authoring--captured-replay-activation-and-ini-persistence-2026-11-16).

## 2026-11-15: Exclude unowned external state from Save and live lock detection

- Isolated live lock adoption and Save pre-tail preservation from unowned
  external GPU state (`source/gpu_lock_ownership_policy.h`, `source/gpu_backend.cpp`,
  `source/main_gpu_state.cpp`, `source/main_state_sync.cpp`,
  `source/main_runtime_control.cpp`, `source/main_runtime_capture.cpp`,
  `source/main_shell.cpp`). Unowned live flat tails no longer populate
  `g_app.locked*` / `g_app.appliedLock*` (`ownership=external` logged once),
  removed dormant `captureAllCurvePoints`, and constrained `preTailInferred`
  exclusively to Green Curve's own previously applied/active-service intent
  (including `curvePointCaptured`).
- Updated Windows GUI omission confirmation (`source/ui_profile_save_notice.cpp`,
  `source/gui_profile_commands.cpp`) to offer opening Capture after Save on
  `IDYES`, and updated Linux TUI Save omission status to count
  `curvePointCaptured` and direct the user to Capture (`source/linux_tui_actions.cpp`).
- Regressions (`tests/curve_plan_tests.cpp`, assertions `7894-7899`), source
  gates (`tools/ui_gates.py`), `--gates`, `--test`, `--check --target all`, and
  WSL Linux UBSan regressions pass. Hardware live-curve Save/capture runs remain
  unverified. See [config-profiles.md](../config-profiles.md#save-names-what-it-leaves-out-2026-11-15).

## 2026-11-15: Backend modeled lockless transition integration

- Connected shared modeled lockless transition preflight and post-reset review
  (`source/authoring_modeled_transition_review.h`) to both Windows
  (`gpu_backend_apply_authoring.h`, `gpu_backend_apply.cpp`,
  `ApplyClockCeilingGuard::apply_modeled_plan`) and Linux
  (`linux_graphics_offset_control.h`, `linux_apply_modeled_review.h`,
  `linux_apply_arm_modeled_post_reset`, `linux_backend_mutation.cpp`) apply paths.
- Bound `scalarBeforeCurve` to nonzero incoming named graphics surface across
  preflight and post-reset review; deferred modeled planning across shaped
  `resetOcBeforeApply` transitions until fresh stock readback while preserving
  outgoing ceiling entitlement (`outgoingCeilingMHz`, `g_linuxOutgoingCeilingMHz`)
  and `UPDATE_INCLUDED` pin authority. Updated `tools/apply_ceiling_gates.py`.
- Fake-driver regressions (`8986-8996` Windows, `8997-9002` Linux, plus
  `tests/clock_transition_tests.cpp`), `--gates`, `--test`, `--test --asan`,
  `--check --target all`, `--tidy` (34 baselined, 0 new), and WSL Linux UBSan
  regressions pass. `desired_authoring_runtime_ready()` and live graphics replay
  remain gated; hardware composition/replay remain unverified. See
  [clock-transition-audit.md](../clock-transition-audit.md#modeled-lockless-transition-protection-2026-11-15).

## 2026-11-15: Native capture representation choices

- Windows and the TUI select frozen exact/flatten/pin proposals with separate
  omission and unfinished-draft decisions. Session/ownership/target drift
  expires TUI capture without changing previous source or input; telemetry
  refresh keeps the snapshot. Fresh discard review prevents late dirty input
  from being replaced silently.
- Cases 9720-9763 cover production controller/transport/lifetime helpers,
  unavailable choices, keyboard/DPI behavior and compact layouts. An offscreen
  native render confirms representation/report geometry. See
  [config-profiles.md](../config-profiles.md#capture-lock-proposals-2026-11-15).
- Windows normal/ASan/UBSan, all-target checks, source gates and native Linux
  fixtures pass; static analysis adds no findings. A plain build freshly
  produced all 12 packages, with all 20 packaged application/service images
  independently compared to the outputs. Physical replay remains unverified.

## 2026-11-15: Reviewed CLI capture lock proposals

- The shared capture proposal layer reuses tolerant physical-voltage recognition
  and the complete planner. Explicit CLI representation choices retain original
  tail controls as observations and never imply Apply or ownership.
- Pin sources distinguish validated fixed getters, retained applied intent and
  unknown state. Matching generation/ownership revisions bind retained intent;
  absent passive snapshot capability stays unknown. Cases 9680-9715 cover exact
  preservation, hidden anchors, evidence/constraint failures and bounded topology.
  Native choice controls now consume these proposals; user observation and
  physical replay remain unverified. See [config-profiles.md](../config-profiles.md#capture-lock-proposals-2026-11-15).

## 2026-11-15: Reviewed scalar source submission

- Windows/TUI source checks retain complete control revisions. A separate Use
  mapped draft decision stages exact supported scalar intent; Apply passes the
  frozen value and token through existing transports. Graphics/curve replay
  stays gated until transition integration is complete.
- Windows dispatch rechecks after modal confirmations, preserves manual warning
  gates and submits ad hoc intent without borrowing a previous shared-slot
  reference. Executing fixtures cover stale controls, cancellation, exact
  payloads, independent draft binding and native/compact action reachability.
  See [config-profiles.md](../config-profiles.md#reviewed-source-submission-2026-11-15).

## 2026-11-15: Exact baseline-to-transition factory

- The model factory binds complete physical topology and authoritative point
  intent to fresh zero-control/reset evidence, checking writable membership,
  range and exact grid before calculating prospective offsets. Absolute
  targets account for the future scalar without modifying source intent.
- Cases 9600-9623 execute target rebasing, binding drift, missing evidence,
  authority and exact-grid failures, plus safe-order adoption with real peaks.
  The Windows ASan/UBSan suite passes. Runtime callers remain pending; no
  authoring replay or physical composition claim is activated.

## 2026-11-15: Modeled lockless transition guard foundation

- Shared exact-Hz planning computes monotonic final and intermediate peaks,
  separates baseline/composition/authority requirements and preserves the
  lockless single-batch path. Modeled protection cannot relax an outgoing cap
  or use a symmetric clock-floor fallback.
- Windows/Linux guard consumers accept the typed review descriptor and recheck
  current control availability. Cases 9550-9596 exercise pure planning and both
  production guard helpers with scripted drivers; omitting collapse fails 9550.
  Fresh model factories and replay activation remain pending. See
  [clock-transition-audit.md](../clock-transition-audit.md#modeled-lockless-transition-protection-2026-11-15).
- Windows normal/ASan/UBSan and native Linux UBSan pass; all-target checks and
  static analysis pass with no new findings. Model adoption stays separate from
  the lightweight review types used by isolated helper fixtures.

## 2026-11-15: Windows offline transfer pane

- Profiles & tools provides native file selection, exact preview and confirmed
  atomic import/export without a connected GPU. Existing source input survives
  cancel, errors, exports and import-only confirmation; loading imported intent
  requires a separate discard decision.
- Immutable file jobs use the existing coordinator and a retained completion
  slot; failed wake notifications cannot lose a result. Cases 9510-9541 execute
  the production controller with scripted
  files and native controls, including stale replies, conflicts, DPI and caret
  preservation. See [config-profiles.md](../config-profiles.md#windows-reviewed-profile-transfer-2026-11-15).
- Windows normal/ASan/UBSan, all-target checks and gates pass; static analysis
  adds no findings. Discard/keyboard/focus bypasses fail 9523/9538/9540, and the
  native options/review panes were rendered and inspected offscreen.

## 2026-11-15: Reviewed TUI profile transfer

- Transfer previews frozen file bytes and an atomic bank edit before any
  storage mutation. Import-only confirmation preserves unfinished editor input;
  loading an imported source has a separate discard decision. No GPU writes
  or daemon startup-snapshot changes occur.
- Shared preparation and TUI controller cases 9440-9491 cover exact units,
  one/five-profile transfer, conflicts, Unicode paths, independent decisions
  and minimum-size controls. Full Windows normal/ASan/UBSan, native Linux UBSan
  and all-target checks pass. Discard bypass fails 9465; static analysis adds no
  findings. See [config-profiles.md](../config-profiles.md#tui-reviewed-profile-transfer-2026-11-15).

## 2026-11-14: Windows capture into a source draft

- Profiles & tools exposes a temporary capture pane with independent omission
  and draft-discard decisions. Frozen acceptance installs an unsaved exact
  source; cancel/failure preserve canonical and legacy invalid input.
- The shared passive queue supports matching cancellation; active reads finish
  and stale results are freed. Cases 9394-9433 exercise cancellation, native
  controls/DPI/focus and production controller acceptance with scripted reads.
  See [config-profiles.md](../config-profiles.md#windows-capture-into-an-unsaved-source-draft-2026-11-14).
- Full Windows normal/ASan/UBSan, native Linux UBSan and all-target checks pass.
  Bypassing discard fails 9417; hiding the leading checkbox fails 9433.
  Static analysis adds no findings; the native pane was rendered offscreen.

## 2026-11-14: Explicit Windows preview controls

- Native source checks offer named graphics/memory controls without changing
  saved source meaning. Choice changes expire pending and completed reviews;
  loading another source resets choices to Auto.
- Cases 9387-9393 cover native selection, job binding, absent domains, reload
  defaults, keyboard focus scrolling and measured caption fit at high DPI.
  See [config-profiles.md](../config-profiles.md#windows-read-only-destination-checks-2026-11-14).
- Focused native ASan/UBSan tests and all-target builds pass. Dropping a chosen
  API fails 9389; omitting combo focus scrolling reproduces 9393.

## 2026-11-14: Windows passive destination checks

- The native source editor queues a named passive read through the existing
  coordinator and displays the shared mapping/control report. Source input,
  ownership and session changes expire in-flight or completed reviews.
- Cases 9350-9386 cover request/session policy, native report controls and
  production GUI callbacks with scripted transport. See
  [config-profiles.md](../config-profiles.md#windows-read-only-destination-checks-2026-11-14).
- Full Windows ASan/UBSan regressions, native Linux UBSan fixtures and all-target
  builds pass. Omitting input invalidation fails 9376; omitting ownership
  comparison fails 9356. Static analysis adds no findings. Hardware is unverified.

## 2026-11-14: Windows source profile editing

- Canonical personal slots open a separate source editor with shared precise
  editing and atomic storage; offline legacy loads keep unresolved identities.
  Source drafts remain independent of live selection, reconnect and Reset.
- Cases 9330-9349 execute native hidden-window input, Save, DPI, scrolling,
  read-only fields and discard lifetimes. Source Apply stays gated pending
  destination acceptance. See [config-profiles.md](../config-profiles.md#windows-canonical-source-editor-2026-11-14).
- Full Windows normal/sanitized regressions and all-target builds pass.
  Bypassing unfinished-input validation fails case 9336. Static analysis reports
  no new findings. Physical GPU writes were not performed.

## 2026-11-14: TUI capture-to-draft workflow

- Capture exposes domain selection and a bounded snapshot review through the
  shared CLI engine. Explicit exact/partial/discard choices stage an unsaved
  source without file writes, a second read or applied ownership.
- Cases 9295-9329 cover modal visibility, keyboard/focus, strict omissions,
  preserved precision, owned zero and responsive report layouts. See
  [config-profiles.md](../config-profiles.md#tui-capture-into-an-unsaved-source-draft-2026-11-14).
- Focused ASan/UBSan, full Windows regressions, native Linux UBSan and all-target
  builds passed. Omitting unsaved state fails 9305; allowing an unseen read
  fails 9301. Static analysis added no findings. Physical GPU writes were not
  performed; flatten/pin conversion and mapped replay remain pending.

## 2026-11-14: TUI scalar edits replace precise intent

- Explicit memory/power edits update precise authority as well as display
  values, preserve named memory controls and keep unrelated intent intact.
- Cases 9286-9294 include edits whose displayed integer does not change but
  whose precise meaning does. Accepted edits emit authoritative diagnostics.
  See [config-profiles.md](../config-profiles.md#explicit-scalar-edits-over-precise-intent-2026-11-14).
- Focused ASan/UBSan, full Windows regressions, native Linux UBSan and
  all-target builds passed. Keeping the previous exact memory/power authority
  fails 9287/9288 respectively. Static analysis added no findings.

## 2026-11-14: Destination-check session lifetime

- TUI Check names the shown GPU and rejects changed service/GPU/ownership
  evidence. Cached previews expire on lifetime changes while preserving the
  source; ordinary telemetry keeps the report readable.
- Portable source drafts survive GPU selection. Legacy draft and active-GPU
  ownership guards remain in force. Cases 9275-9285 drive production Check,
  refresh and selector helpers through scripted transport. See
  [config-profiles.md](../config-profiles.md#tui-review-session-lifetime-2026-11-14).
- Focused ASan/UBSan, full Windows regressions, native Linux UBSan and
  all-target builds passed. Omitting the named target fails 9275; omitting
  reconciliation fails 9280. The extracted hash body is unchanged after
  newline normalization. Static analysis added no findings.

## 2026-11-14: Typed destination review results

- CLI/editor checks share staged source, capture, mapping, runtime and planner
  results. The TUI consumes typed fields, binds its local revision and clears
  stale reviews on source/control changes. No Apply authority is granted.
- Memory grid conversion and application-range refusals have distinct reasons;
  exact source values are preserved. Cases 9265-9274 cover the result stages,
  refusals and reset contract. See
  [config-profiles.md](../config-profiles.md#structured-destination-review-2026-11-14).
- Full Windows regressions, focused ASan/UBSan, native Linux UBSan and
  all-target builds passed. Omitting result reset fails 9266; static analysis
  retained 34 baseline findings and added none.

## 2026-11-14: Offline TUI source editing

- Source points and included clock offsets accept exact decimal MHz without a
  daemon. Typing explicitly converts a point to absolute intent; untouched
  source values, rational power and lock targets remain unchanged.
- Cases 9241-9264 cover numeric limits, refusal preservation, hidden/read-only
  source identities, slot round-trip and editable layouts. Apply stays detached.
  See [config-profiles.md](../config-profiles.md#offline-tui-source-edits-2026-11-14).
- Full Windows regressions, focused ASan/UBSan, native Linux UBSan and
  all-target builds passed. Retaining a captured kind after typing fails 9247.
  Static analysis retained 34 baseline findings and added none.

## 2026-11-14: TUI source destination checks

- The TUI uses the shared CLI engine to check its in-memory source through
  passive Capture. Control preview choices preserve source intent; reports
  wrap and scroll without hidden trailing bounds.
- Cases 9230-9240 cover precise API selection, uncertainty/refusal, malformed
  evidence and compact report layouts. No Apply authority is published. See
  [config-profiles.md](../config-profiles.md#read-only-tui-destination-checks-2026-11-14).
- Full Windows regressions, focused ASan/UBSan, native Linux UBSan, all-target
  builds and source gates passed. Omitting report wrapping fails 9240.
  Static analysis retained the baseline and added no findings.

## 2026-11-14: Canonical source profiles in the TUI

- The TUI retains canonical source documents independently of display values;
  Load/Save preserve hidden and precise intent without a GPU. A source viewer
  separates exact stored intent from the live destination graph.
- CLI and TUI share synchronized atomic slot I/O. Source candidates include the
  full document during discard confirmation; dirty state includes persisted
  metadata. Mapped editing and Apply activation remain pending. See
  [config-profiles.md](../config-profiles.md#canonical-source-slots-in-the-tui-2026-11-14).
- Cases 9210-9226 cover offline Save/restart/Load/export and conflict behavior.
- Full Windows ASan, native Linux UBSan, both production CLI fixtures,
  all-target builds and source gates passed; static analysis added no findings.
  Replacing document dirty comparison with control equality fails 9221.

## 2026-11-14: Unsaved profile draft replacement

- Linux Load freezes a candidate, renders explicit Keep/Discard confirmation
  and blocks unrelated input until resolved. Cancellation preserves invalid
  edit text and no confirmation can act before it is visible.
- Windows asks before replacing dirty values when comparison fails rather
  than treating missing comparison as permission to discard them. See
  [config-profiles.md](../config-profiles.md#confirming-draft-replacement-2026-11-14).
- Cases 9190-9205 exercise pending-load and layout behavior without a daemon.
- Full native regressions, focused ASan/UBSan, native Linux UBSan and all-target
  builds passed. Static analysis added no findings; omitting the dirty guard
  fails 9190. Native Windows dialog interaction remains unverified.

## 2026-11-14: Windows named graphics handoff

- Passive preflight and the actual release bind outgoing/incoming APIs, require
  installed protection for nonzero handoff and retain durable coverage until
  successful completion. VF routing does not clear an unrelated preferred API.
- Cases 9160-9183 cover drift, missing protection, preparation failure, coupled
  owned zero, baseline review updates and post-handback curve reads. See
  [clock-transition-audit.md](../clock-transition-audit.md#windows-named-control-handoff-2026-11-14).
- Focused/full Windows ASan regressions, native Linux UBSan and all-target
  builds passed. Omitting the actual preflight's handoff review fails 9178;
  a read-only refusal preserves ownership of zero.
- Incoming scalar composition is measured from the fresh post-handback sample,
  independently of the clock increase caused by releasing an outgoing offset.
- Static analysis retained 34 baseline findings and added none. Keeping the
  old pre-handoff measurement sample fails 9182.

## 2026-11-14: Linux named graphics handoff

- Apply releases the outgoing named API under reviewed protection, retains
  exact preimages for both APIs and verifies their combined final state before
  clock handover. A failed incoming submission preserves earlier attempt
  evidence; owned zero participates in coupled-API recovery.
- Cases 9122-9151 exercise drift refusals, absent protection, exact handback,
  VF routing, baseline review updates and coupled failures. See
  [clock-transition-audit.md](../clock-transition-audit.md#linux-named-control-handoff-2026-11-14).
- Full native Windows tests, focused ASan/UBSan, native Linux UBSan fixtures
  and all-target checks passed; static analysis retained 34 baseline findings
  and added none. Removing the handoff fails 9124.

## 2026-11-14: Both named clock controls in passive review

- Capture schema 3 includes both graphics/memory APIs. Protocol 37 carries
  compiler-checked observation/response sizes of 4960/19568 bytes; requests,
  snapshots and persisted authoring layouts do not change.
- Both values bind consistency and fresh review. Nonzero alternatives remain
  partial observations; exact control previews never borrow another API's
  values. CLI choices affect a derived preview and preserve saved intent.
- Cases 9100-9121 and service-response fuzz seeds cover dual reads, malformed
  metadata, drift, stale review, explicit choices and source preservation. See
  [profile-document.md](../profile-document.md#named-clock-controls-in-passive-review-2026-11-14).
- Full Windows ASan regressions, native Linux UBSan fixtures and both platforms'
  production CLI checks passed, as did all-target checks, 5000 service-response
  fuzz runs and static analysis (34 baseline findings, zero new).

## 2026-11-14: Final combined graphics recovery verification

- Named APIs are verified together after scalar recovery; subsequent VF writes
  must preserve that result before protection or recovery coverage retires.
  A mismatch retains uncertainty even if every individual setter returned a
  matching immediate readback.
- Cases 9089-9097 and the production Linux rollback fixture exercise coupled
  APIs, late curve clobbering, refusal before unlock and successful retry. See
  [clock-transition-audit.md](../clock-transition-audit.md#final-named-control-recovery-verification-2026-11-14).
- Native Windows/Linux regressions, focused ASan/UBSan, all-target compile
  checks and static analysis passed. Removing final readback fails 9089.

## 2026-11-14: Durable Linux named graphics recovery

- Active records freeze the previous authoring layout and add named-control
  coverage. Every named write, including automatic replay, requires a durable
  PREPARED record and a fresh operation permission.
- Exact snapshots and rollback preserve both APIs, owned zero and attempted
  controls. Failed rollback after Reset retains recovery coverage. Pending
  records bind explicit Reset to their stored GPU even with startup replay off.
- Cases 9060-9088 and the transition fixture cover preparation refusal, precise
  recovery, no-write ownership rollback, target mismatch, migration and old
  padding. Cross-surface replay activation remains pending. See
  [persistence-schema.md](../persistence-schema.md#linux-graphics-control-recovery-2026-11-14).
- Native Linux file tests passed with real root ownership and ordinary-user
  identity adaptation. ASan/UBSan, all-target checks and static analysis passed;
  removing preparation fails 9060. Positive UUID matches preserve recovery
  across PCI movement; incompatible replacements refuse before transfer.

## 2026-11-14: Native Linux production profile-transfer verification

- The verified production ELF passed the complete offline transfer fixture in
  native Linux scratch storage under WSL, including Unicode-bank replacement
  and exact restart/export. Each client invocation is bounded; temporary files
  are removed, and no GPU is accessed.
- The fixture now explicitly selects Windows format for foreign INI export
  and separately checks each platform's native legacy layout. The previous
  platform assumption failed on Linux; the corrected workflow passes. See
  [config-profiles.md](../config-profiles.md#portable-documents-and-offline-transfer-2026-11-13).

## 2026-11-13: Durable Windows named graphics-control recovery

- The ownership marker freezes its 216-byte predecessor and adds validated
  control coverage in version 2 (224 bytes). New named control coverage is
  persisted before submission; a failed store refuses without a scalar setter.
- Owned zero retains its control identity. Reset/rollback repair all outstanding
  named controls, and a same-boot handback restores coverage after target/epoch
  checks. Cases 9033-9059 cover migration, padding, failed preparation, zero,
  restarted coverage, target-bound takeover, unrelated refusal and exact repair. See
  [persistence-schema.md](../persistence-schema.md#windows-graphics-control-recovery-marker-2026-11-13).
- Native Windows sanitizers and Linux execution passed; ignoring durable
  preparation failure fails 9043. All-target builds passed and static analysis
  added no findings. Replay activation still requires the remaining transition
  and cross-surface recovery work.

## 2026-11-13: Unicode document-bank preservation

- Document-bank edits and Windows native section inspection share a bounded
  encoding adapter. UTF-16LE stays UTF-16LE, unrelated code units survive, and
  conflict detection compares the original raw generation. Malformed Unicode
  refuses without replacement; existing BOM-less bytes remain opaque.
- Cases 9018-9032 cover exact encoding, surrogate pairs, retained preferences,
  stale edits, malformed data and bounds. The executable transfer fixture and
  codec fuzz boundary also exercise encoding; Windows production section reads
  and wide INI getters consume the resulting file. The pre-fix NUL rejection
  fails 9019. ASan/UBSan, native Linux, all-target builds, executable workflows
  and 5000 codec fuzz runs passed. See
  [config-profiles.md](../config-profiles.md#portable-documents-and-offline-transfer-2026-11-13).

## 2026-11-13: Explicit legacy profile transfer

- Source-format adapters preserve Windows/Linux point semantics, exact memory
  units, explicit locks and fan intent without live repair. Ambiguous units or
  curve meaning require choices; unresolved indices never acquire destination
  voltages. Source records and destination slots have separate selections.
- Native legacy export uses the raw decoder. Cases 8986-9008 and 9010-9017 cover
  strict parsing, output preservation, selection, persistence and exact values;
  executable workflows and the codec fuzz boundary also exercise conversion.
  Native Windows/Linux suites, all-target checks and 5000 codec fuzz runs passed;
  an incorrect memory conversion factor fails 8992. Static analysis added no findings.
  See [config-profiles.md](../config-profiles.md#explicit-legacy-transfer-2026-11-13).

## 2026-11-13: Native Linux regression execution

- The sanitizer-enabled complete C++ harness and all three Linux kernel/store
  fixtures passed under WSL, using the pinned Windows cross-compiler and declared
  link units. Native scratch storage and bounded process-group cleanup were
  verified; no GPU was accessed. Windows source/Python gates remain separate.
- These x86-64 results cover native execution rather than link checks; driver ABI,
  workload behavior and native ARM64 remain unverified. See
  [testing.md](../testing.md#windows-hosted-native-linux-execution-2026-11-13).

## 2026-11-13: Named scalar submission protection

- Bound scalar-to-curve requests classify raising submissions before mutation.
  Risky splits require an actually installed guard; unreadable lockless pin or
  composition evidence cannot become a guessed clamp.
- Both guards preserve strict refusal, and submission rechecks the selected
  scalar. A refusal before any write skips scalar/VF recovery; already
  installed ceilings retain recovery ownership. Cases 8948-8985 and production
  guard fixtures count driver calls, including retained-cap refusals. Removing
  the rollback phase filter fails 8979. Replay remains gated; see
  [profile-document.md](../profile-document.md#named-scalar-submission-protection-2026-11-13).

## 2026-11-13: Passive profile control feasibility

- Profile checking now evaluates exact curve constraints and scalar grids/ranges
  against passive capture, with bounded typed issues and explicit uncertainty.
  Whole-operation review remains conditional; the command still writes nothing.
- Bound projected prefixes use offset authority and baseline evidence rather
  than stored MHz previews. Cases 8915-8947 cover the checker, production CLI
  path and the shared planner; removing the prefix rule fails 8919. See
  [profile-document.md](../profile-document.md#passive-profile-control-feasibility-2026-11-13).

## 2026-11-13: Fresh controls and ownership review

- Protocol 36 binds explicit scoped Apply to a fresh passive all-domain capture
  and outgoing owned intent, including zero. Physical observation fingerprints
  remain separate from the service review revision.
- Refusal precedes refresh and automatic-work supersession. Reviewed paths keep
  the existing GPU session; shared binding resolution uses cached adapters.
  Cases 8884-8914 cover tokens, controls, ownership and actual daemon handling;
  replay and client integration remain gated. See
  [profile-document.md](../profile-document.md#fresh-controls-and-ownership-review-2026-11-13).

## 2026-11-13: Bounded authoring VF submission

- An explicit single-batch policy caps unprotected authoring submissions at one
  setter and prevents per-point fallback or later Windows correction writes.
  Exact Linux membership loss refuses before writing; legacy strategies remain.
- Cases 8870-8883 count actual production writer calls, including partial/ignored
  batches, missing points and the unknown-family backend. Removing the policy
  exit fails 8870. Full transition/recovery review still gates replay; see
  [profile-document.md](../profile-document.md#explicit-vf-submission-limits-2026-11-13).

## 2026-11-13: Windows named graphics routing and repair

- Reviewed graphics routes retain physical units, including sub-MHz VF
  components and zero. Named scalar writes retain the selected control surface,
  record uncertainty before mutation, and repair that surface before VF reset.
- Exact bound curve targets are compiled before the first transition setter;
  only an authorized baseline reset may defer a shaped-estimate range failure.
  Cases 8843-8869 cover production setters, rollback, preflight and routing.
  Runtime authoring remains gated pending transition, durable recovery and
  client integration; see
  [profile-document.md](../profile-document.md#windows-graphics-routing-and-scalar-repair-2026-11-13).

## 2026-11-13: Named Linux graphics replay and rollback

- A shared control plan distinguishes named scalars, VF components and unresolved
  routes. Named scalars validate exact physical units and independent current
  and range evidence without family assumptions.
- Linux Apply binds the selected scalar's preimage and keeps its surface through
  baseline, write, verification and rollback. Cases 8828-8842 plus the production
  transition fixture check refusal and recovery contracts. Replay remains gated;
  see [profile-document.md](../profile-document.md#named-linux-graphics-controls-2026-11-13).

## 2026-11-13: Windows exact authoring targets and correction

- The Windows Apply path consumes the shared compiler's physical-Hz targets.
  Corrections preserve captured values and unowned preimages across memory
  changes, recheck the reviewed write set, and avoid the legacy MHz classifier.
- Cases 8810-8827 execute the production builder, writer and repeated correction
  path with fake driver calls, including exact verification boundaries and
  zero-write refusals. Existing runtime gates remain closed pending route,
  transition and client integration. See
  [profile-document.md](../profile-document.md#windows-authoring-targets-and-corrections-2026-11-13).

## 2026-11-13: Shared exact targets and Linux authoring compilation

- A shared compiler keeps physical offsets and absolute frequency targets in
  separate channels, with full voltage binding, writable membership and raw-grid
  checks. Linux consumes its exact metadata through construction, baseline
  rebuild, driver submission and readback verification.
- Authorized baseline preparation can defer a shaped-estimate range failure;
  unresolved placeholders never reach a writer. Rebuilds recheck feasibility
  and publish matching exact metadata. Cases 8783-8809 include a production
  factory/rebuild fixture; removing the metadata assignment fails at 8800.
- Target factories moved into a focused header to keep the Linux mutation shard
  within its size gate. Runtime replay remains gated pending Windows authoring
  and transition integration. See
  [profile-document.md](../profile-document.md#shared-exact-target-compiler-and-linux-adapter-2026-11-13).

## 2026-11-13: Captured precision in Windows curve corrections

- Windows target construction retains captured physical-Hz authority beside
  legacy projections and checks exact range bounds. Exact-mode correction passes
  use the capture-preserving overlay and exact writer, so correcting another
  absolute point cannot round away a captured remainder.
- Fake-driver cases 8780-8782 check signed half-kHz offsets, owned zero, an
  absolute correction, untouched controls and zero-write refusals. The target
  builder regression fails before the correction. Runtime replay remains gated.
  See [profile-document.md](../profile-document.md#captured-remainders-through-windows-corrections-2026-11-13).

## 2026-11-13: Exact-Hz feasibility planning

- The shared planner uses exact physical baselines and components for precise
  points, locks and curve bounds. It distinguishes missing exact samples from
  representable intent, retains first/worst remainders in diagnostics and checks
  actual VF and named hard-pin grids. Legacy verification/bin rules remain.
- Backend prechecks and the Windows original-state backstop provide matching
  exact arrays. Half-kHz controls cannot be classified as stock through a zero
  kHz projection. Cases 8760-8779 include a before/after grid regression.
  Writer and transition review still gate runtime replay. See
  [profile-document.md](../profile-document.md#exact-hz-feasibility-and-baseline-evidence-2026-11-13).

## 2026-11-13: Exact frequency samples through backend snapshots

- Windows and Linux retain physical-Hz VF frequencies before kHz projection.
  Capture shares the checked conversion, and settled Windows samples keep
  matching exact frequency/offset arrays. Final read failure, service display
  projection and selection/freshness reset invalidate precision evidence.
- Focused fixtures cover half-kHz samples, malformed/empty tables, raw bounds,
  foreign-domain isolation and failed reads. Exact authoring remains gated until
  Apply integration is complete. See
  [profile-document.md](../profile-document.md#exact-frequency-snapshots-2026-11-13).

## 2026-11-13: Stale installer names and shared readers

- Stale transaction leaves are discarded through the existing no-follow
  handle. The single recovery attempt uses a fresh exclusive staging name,
  avoiding names kept pending by shared readers. No stale entry is renamed;
  target replacement and backup metadata preservation are unchanged.
- Native cases 8751-8758 cover shared-reader recovery, staging cleanup,
  strict owned names, non-sharing refusal and mapped-image refusal. Folder
  consent and uninstall cleanup recognize only complete payload staging names.
  No image-deletion bypass or delayed retry is introduced. See
  [installer.md](../installer.md#exclusive-temporary-file-creation-verified-2026-11-13).

## 2026-11-13: Passive constraints and partial-profile authority

- Protocol 35 carries capture schema 2 with explicit VF grid, range provenance
  and backend write-support facts in the normalized control fingerprint. Capture
  summaries distinguish reported graphics bounds from application fallback.
- Read-only point observations cannot grant a curve domain. Point replacement,
  semantic equality and mapping requirements respect omitted VF domains, keeping
  scalar-only captures from clearing retained points. Cases 8720-8737 cover
  wire boundaries, fresh fallback provenance, read-only capture and the actual
  scoped-plan handoff. See [profile-document.md](../profile-document.md#passive-constraint-provenance-and-omitted-curve-authority-2026-11-13).

## 2026-11-13: Exact runtime resolution and mapping preview

- Both CLI clients expose passive `--check-profile` mapping review. Parsing and
  bank selection precede Capture; exact runtime resolution retains point kinds,
  scalar precision, exclusions and destination voltage bindings without editing
  source files or publishing partial results. Missing getters do not prove absent
  write capability.
- Cases 8700-8717 exercise the product command and compiler; executable checks
  cover early invalid-input rejection, with structured runtime-resolution checks
  added to the portable-codec fuzz target. Range/grid/transition planning and
  current-ownership release resolution remain pending before replay is enabled.
  See [profile-document.md](../profile-document.md#exact-runtime-resolution-and-cli-mapping-preview-2026-11-13).

## 2026-11-13: Visible fixed floors remain read-only

- The shared target-status rule now distinguishes listed floor entries from
  writable points at every voltage. Both planners refuse explicit floor targets
  and anchors; passive Capture retains their observations without writable
  authority. Existing listing and supported rising points remain available.
- Cases 8691-8697 cover the planner and real getter boundary. The new Capture
  case failed before the correction; the full native suite passes afterward.
  Installer rollback fixture failures also report numeric operation/result
  fields without paths. See [gpu-backend.md](../gpu-backend.md).

## 2026-11-13: Runtime voltage identity binding

- Both scoped Apply planners remap bound incoming and outgoing authority before
  merge/cleanup, retaining exact values and exclusions. Full-table identity is
  required to disambiguate equal voltages; otherwise matching is unique by exact
  voltage. Refusals publish no partial plan, and summaries count remapped entries.
- Native cases 8680-8690 cover shifted tables, overlapping moves, ambiguity,
  missing/non-writable targets, replacement cleanup and fan-only retention.
  Runtime authoring replay still awaits destination review and exact adapters.
  See [profile-document.md](../profile-document.md#runtime-topology-binding-during-scoped-apply-2026-11-13).

## 2026-11-13: Bound authoring runtime migration

- Schema 5 and protocol 34 retain exact graphics/point/lock values, identity
  exclusions and resolved destination voltages across current durable records.
  Real loaders now widen the frozen exact-scalar predecessors after validating
  their original bytes. Explicit refusal prevents premature rounded replay.
- Native regression cases 8650-8676 cover tags, precision, historical/current
  round trips, checksum failures, projection and fan-only ownership retention.
  Destination review and exact adapter activation remain the next work. See
  [persistence-schema.md](../persistence-schema.md#bound-authoring-runtime-capacity-2026-11-13).

## 2026-11-13: Frozen exact-scalar runtime predecessors

- Froze the current desired payload and enclosing active/startup/restart records
  before the next authoring extension. Fieldwise converters preserve exact scalar
  and captured authority, with fixed offsets and original-byte checksum checks.
- Cases 8640-8644 pass alongside existing historical/current record fixtures.
  Live layouts, disk dispatch and current versions are unchanged. See
  [persistence-schema.md](../persistence-schema.md#frozen-precise-scalar-predecessor-2026-11-13).

## 2026-11-13: Exact Linux curve preservation across memory writes

- The Linux memory phase now holds fresh exact VF controls, verifies its binding
  after the memory setter and restores raw values under the declared write budget.
  No VF surface still permits independent memory tuning.
- Transaction preflight rejects unavailable preservation without setters. Memory
  rollback restores the prior curve even when no curve phase started. Cases
  8610-8623 and the actual rollback fixture cover fractional and partial failures.
- No physical GPU mutation or native Linux execution claimed. See
  [profile-document.md](../profile-document.md#linux-vf-preservation-across-memory-writes-2026-11-13).

## 2026-11-13: Exact Windows curve preservation across memory writes

- Memory preservation now recognizes sub-kHz controls and retains exact outgoing
  values through target construction. Fresh evidence and representability checks
  run before mutation; replay verifies raw controls with the original voltages.
- The bounded exact writer preserves unselected entries and uses the existing
  deadline. Cases 8580-8600 execute production precheck/writer helpers, including
  a memory-clobbered 500 Hz control, with no driver loading or physical writes.
- All declared production targets compile. General precise document compilation
  and Linux memory-triggered preservation remain pending. See
  [profile-document.md](../profile-document.md#windows-vf-preservation-across-memory-writes-2026-11-13).

## 2026-11-13: Reviewed CLI capture to portable intent

- Both clients report domain completeness and pin/composition knowledge before
  an exact portable save. Partial results require an explicit choice; unavailable
  sessions, malformed results and failed writes preserve existing output.
- Cases 8540-8551 execute review, decisions, exact zero/fractional/ratio retention
  and offline local restart decoding. Invalid executable commands refuse before
  service access. All declared production targets compile; native Windows CLI
  fixtures pass. GUI/TUI and flatten/pin proposals remain pending.
- See [config-profiles.md](../config-profiles.md#passive-capture-cli-2026-11-13).

## 2026-11-13: Dedicated passive capture IPC

- Protocol 33 carries a separate bounded observation, preserving the desired
  settings and durable record layouts. Capture rejects settings, file paths,
  mutation flags and restore fields.
- Windows uses its resolved driver interface; Linux skips the normal hardware
  snapshot during response stamping. Existing sessions only: no initialization,
  selection, recovery or deferred fan handback. Wire and actual daemon-handler
  fixtures cover rejection without capture calls and unchanged ownership.
- All declared production Windows/Linux targets compile. Client capture review
  remains pending. See [persistence-schema.md](../persistence-schema.md).

## 2026-11-13: Getter-only consistent capture foundation

- Added a bounded passive collector with exact VF/memory controls, independent
  power observations, explicit omissions/composition and unknown pin knowledge.
- Three immediate read pairs exclude moving frequency samples and publish only
  a coherent result; no initialization, recovery, cache adoption or setter path
  is called. Cases 8480-8501 exercise the production helpers with fake getters.
- Portable graphics observations retain unresolved controls without inventing
  replay intent. Service/client capture integration remains pending. See
  [profile-document.md](../profile-document.md#passive-capture-foundation-2026-11-13).

## 2026-11-13: Exact topology mapping and reviewed grid batches

- Added GPU-free exact-voltage mapping, complete-topology index proof, explicit
  historical binding review and distinct missing/ambiguous/read-only outcomes.
- Added checked lower-clock grids and whole-surface batch previews bound to
  semantic source revision, destination generation/revision, API grid and power
  default. A supplied complete planner can refuse the whole proposal; source
  documents and changed observations remain separate from derived intent.
- Cases 8420-8444 and 8460-8474 cover identity, signed-grid arithmetic, ratios,
  complete-plan refusal and stale acceptance. Product destination collection and
  editor bindings remain pending. See
  [profile-document.md](../profile-document.md#destination-mapping-and-conversion-primitives).

## 2026-11-13: Flat local document storage and offline CLI transfer

- Added flat per-slot document serialization with exact numbers and encoded
  UTF-8 text; offline fixtures retain hidden points, infeasible values, metadata
  and observations through full-bank replacement and subsequent export.
- Both clients prepare/confirm atomic profile import and export before driver
  initialization. Assignments are reported and retained, stale bank revisions
  and failed writes leave prior contents intact, and import never applies tuning.
- Windows historical Save/Clear preserves other document sections raw. Linux
  profile/GPU writers share a stable lock across read-modify-write. Historical
  Apply readers require destination review for new records rather than treating
  them as defaults; GUI/TUI mapping and capture remain pending.
- See [config-profiles.md](../config-profiles.md#portable-documents-and-offline-transfer-2026-11-13).

## 2026-11-13: Shared bounded portable-profile codec

- Implemented the published v1 table with atomic parse/encode results, checked
  exact numbers, strict UTF-8 and field/count/reference validation. Canonical
  output preserves observations and dormant fan data without consulting a GPU.
- Native cases 8340-8356 cover one/five-profile round trips, malformed input,
  section/key ordering, size boundaries and unchanged output on failure. A
  shared fuzz target exercises raw parsing and structured round-trip invariants.
- This is the shared serialization layer; product file/storage commands and
  destination resolution remain pending. See
  [profile-document.md](../profile-document.md#portable-codec).

## 2026-11-13: Transfer document identities and structural contract

- Added point-set policy, topology evidence/unresolved legacy markers, exclusion
  identities, memory-surface intent, complete optional fan configuration and
  source/lock observations. Captured tail controls can become labeled
  observations without retaining point authority after a reviewed conversion.
- Structural validation rejects invalid domains, references, enum values,
  projections, exclusions, fan data and bounded UTF-8 text without imposing
  current hardware limits. Exact ratios and exclusion sets compare by meaning.
- Published the exhaustive v1 field table beside the regression fixtures before
  codec implementation. Cases 8280-8321 cover the model; product producers and
  codec/storage integration remain pending. See
  [profile-document.md](../profile-document.md).

## 2026-11-13: Independent clock-control and range observation

- Windows keeps a valid programmed offset when its range is unavailable,
  with separate range confidence and checked physical-kHz cache scaling.
- Shared observation serves both platforms and retains selected API provenance;
  named exact verification still cannot use an alternate getter. Driver status
  and semantic range validity remain separate, including malformed ranges.
- Production reader cases 8265-8271 cover partial and refused observations.
  See [profile-document.md](../profile-document.md). No hardware write performed.

## 2026-11-13: Precise scalar runtime and frozen-record evolution

- Appended exact memory Hz/surface and reduced power-ratio authority, with
  strict previews and protocol 32. Frozen outgoing daemon/restart records
  retain historical bytes and widen without inventing precise intent.
- Scoped Apply resolves scalars exactly, preserves outgoing memory-surface
  cleanup and retires selected ownership. Legacy profile transitions after
  precise ownership use the scoped compiler; matching compares exact intent.
- Windows preflight and the extracted power phase bind a fresh denominator;
  Linux retains it in the transaction snapshot and verifies it after writes.
  Fixtures 8220-8263 cover the new contracts and historical records.
- Local document slots, capture and portable producers remain pending. See
  [persistence-schema.md](../persistence-schema.md) and
  [profile-document.md](../profile-document.md). No physical replay claimed.

## 2026-11-13: Shared exact memory control verification

- Windows strict memory writes now bind the API surface observed by their
  fresh getter. Invalid modern control metadata cannot become a readback,
  and a failed setter cannot fall through to another control surface.
- The same exact helper serves Linux recovery. Legacy whole-MHz behavior,
  conservative memory-range handling and observation fallback remain intact.
- Production fake-driver cases 8200-8205 cover isolation, failed/missing APIs,
  malformed metadata and conservative ranges. See
  [profile-document.md](../profile-document.md). No GPU write performed.

## 2026-11-13: Exact power planning and backend verification

- Added shared rational-power resolution against fresh destination controls,
  with explicit conversion/range refusals and stable-default verification.
  Linux percentage writes use it and preflight bounds before transaction writes;
  Windows precise writes share existing selected-GPU and uncertainty handling.
- Regression fixtures 8170-8199 cover fractional targets, unavailable controls,
  changing defaults/bounds, failed or rounded writes and actual backend helpers.
  Native regressions and all declared Windows/Linux compiler targets pass.
- Ratio IPC/editor producers remain pending. No hardware write performed.
  See [profile-document.md](../profile-document.md).

## 2026-11-13: Exact memory recovery binds its control surface

- Linux snapshots retain the API surface that supplied memory control.
  Exact restoration requires the same getter/setter and an equal control;
  a grid snap, another getter or missing surface cannot prove recovery.
- A failed precise write does not try a second setter. Legacy tuning retains
  its documented snap tolerance and observation fallback. Offset helpers moved
  to a focused, production-tested header; malformed modern metadata never
  becomes a valid P0 observation.
- Fixtures 8150-8163 and the rollback fixture cover exact/fractional values,
  failed setters that mutated, invalid metadata, unavailable getters and
  per-surface isolation. Full native regressions and the Windows/Linux compiler
  matrix pass. No physical replay claimed. See [profile-document.md](../profile-document.md).

## 2026-11-12: Explicit runtime scopes and ownership compilation

- Added the shared scoped transition compiler and Windows/Linux call sites.
  Partial updates preserve omitted domains and locks; replacement/release
  cleanup is separate from durable ownership, including owned zero and a VF
  point-set replacement policy. Final release retires ownership.
- Per-intent graphics routing preserves omitted independent scalars and keeps
  selective-to-uniform cleanup from changing the new scalar's write route.
- Preserved the transition ceiling when cleanup removes an owned flatten
  without a global reset. Scoped values refuse numeric changes that legacy
  IPC normalization would make. Protocol 31 separates the new semantics.
- Extracted Linux read-only preflight and removed a leftover memory-surface
  requirement for curve-only baseline work. Fixtures execute that production
  preflight and the scoped target/compiler/raw-writer path without hardware.
- See [profile-document.md](../profile-document.md). Document/editor/capture
  integration and native Linux hardware execution remain pending.

## 2026-11-12: Exact VF readback, raw replay and recovery

- Retained physical-Hz control values alongside legacy kHz caches. Linux raw
  targets and rollback snapshots preserve fractional controls; reset and cleanup
  compare exact zero. Windows captured verification rejects fractional drift
  hidden by the kHz projection.
- Added exact surface-grid/range planning and bounded per-pass failure summaries.
  Production fake-driver cases cover half steps of either sign, ignored writes,
  untouched raw entries and refusal before mutation. IPC capture replay remains
  disabled pending document/scope integration. No physical GPU replay claimed.
- See [profile-document.md](../profile-document.md) for current boundaries.

## 2026-11-12: Exact memory recovery and checked ratio arithmetic

- Added physical-Hz/effective-MHz conversion helpers and a strict programmed
  memory-control setter. The legacy setter also preserves half-MHz requests;
  unrepresentable requests or an unavailable exact getter issue no write.
- Linux rollback snapshots keep odd effective-MHz controls instead of rebuilding
  them from truncated display MHz. Added production setter/rollback fixtures.
- Signed minimum, checked scaling and ratio arithmetic are bounded without an
  overflowing intermediate product. Units 8070-8086 pass; 11,210 edge/random
  arithmetic cases also matched an independent arbitrary-precision calculation.
- Full native regressions/source gates and the Windows/Linux check matrix pass.
  No physical GPU replay claimed. See [profile-document.md](../profile-document.md).

## 2026-11-12: Shared captured-point runtime interpretation

- Added a shared point adapter used by Windows/Linux planning, target builds,
  verification, ownership and merge paths. Captured zero is owned independently
  of MHz; raw offsets never inherit graphics projection/scalar compensation.
- Captured readback is checked against the exact programmed offset. The
  correction view preserves offset authority; malformed point/lock combinations
  and sub-grid values refuse explicitly. Restore records validate point tags.
- Added 8040-8060 and production Windows target/verify cases; source gates follow
  the central adapter. See [profile-document.md](../profile-document.md).
- IPC replay stays disabled pending scope/editor/document integration. The
  legacy write interfaces still require exact kHz; no hardware replay claimed.

## 2026-11-12: Linux baseline preparation preserves memory tuning

- Removed the transient memory reset and its baseline-only rollback write;
  retained memory ownership during baseline intent merging. Baseline capability
  no longer depends on memory controls. Explicit memory Apply/Reset stays in
  its own phase. See [linux-scaffold.md](../linux-scaffold.md).
- A failed baseline can already have zeroed VF offsets before the curve phase
  begins; rollback now restores that captured curve as well.
- Production-helper fixtures pass with counted driver functions. Restoring each
  old baseline-memory, rollback-memory or skipped-curve-restore behavior makes
  the fixture fail. Full native Windows regressions, source/security gates and
  Linux cross-links pass; physical Linux execution remains unverified.

## 2026-11-12: Captured-offset schema capacity and historical record migration

- Appended independent signed-Hz VF offset presence/payload arrays without
  reinterpreting old absolute/projected settings. Froze outgoing desired,
  restart and UUID-bearing daemon records; widened old bytes with their
  original checksums and identity intact. See
  [persistence-schema.md](../persistence-schema.md).
- Protocol 30 rejects old/new peers and explicitly refuses captured replay
  until its runtime consumers are connected. No capture UI or producer yet.
- Added historical-layout, precision, owned-zero and strict-wire fixtures
  (8010-8030). Full native Windows regression tests and the Windows/Linux check
  matrix pass. Linux loaders are cross-linked; native Linux execution and GPU
  replay remain unverified.

## 2026-11-12: New authoring document (ProfileDocument), point intent and unit types

- Added `source/profile_document.h`, `source/profile_point_intent.h` and
  `source/tuning_units.h`: a from-scratch, platform-neutral, GPU-free
  document type (`ProfileDocument`) with four mutually exclusive per-point
  kinds (absent, absolute frequency, projection, captured offset), explicit
  application scope and domain presence, and an exact reduced power ratio
  of board default instead of raw mW. See
  [profile-document.md](../profile-document.md) for the full summary.
- This is scaffolding only: nothing in the live runtime, IPC or disk layer
  builds, persists or transmits a `ProfileDocument` yet. No user-visible
  behavior changed; no CHANGELOG entry was added for this reason.
- Verified: `python build.py --test` (new 7960-8000 in
  `tests/profile_document_tests.cpp`; mutation check disabling the
  captured-offset comparison fails 7988), `--gates`, `--check`, `--tidy` (no
  new findings).

## 2026-11-12: Linux VF baseline reset and post-write curve/plateau verification parity

- `LINUX_MUTATION_RESET_BASELINE` cleared the NVML graphics/memory scalars on
  every profile switch but never the per-point VF curve offsets, so a point
  the new profile does not re-boost kept the previous profile's raw offset
  (Windows has reset this unconditionally since `reset_oc_before_gui_apply()`
  was written). `linux_apply_reset_baseline_vf_curve()`
  (`linux_backend_apply_context.h`) now zeroes every populated point whenever
  `resetOcBeforeApply` is requested and re-reads the settled curve before the
  next build; `rebuildCurveTargets` is forced regardless of the NVML scalar's
  prior value.
- `apply_curve_offsets_verified()` (`linux_backend_vf_write.h`) silently
  clamped an out-of-range write target into the surface's own range instead
  of refusing; removed. The shared planner already proves representability
  before any setter runs, so a write-time mismatch now surfaces instead of
  landing on a value nothing validated.
- Offset convergence alone never proved a curve landed on its requested
  frequency: the driver can move a point's stock base between the write and
  the check (one VF bin under load on Blackwell). `linux_build_curve_targets()`
  now tags each written point absolute- or offset-intent (hard-lock tails stay
  untagged: the pin is the contract, not a flat raw table), and
  `linux_apply_verify_curve_write()` re-reads the curve and checks every
  absolute-intent point's frequency with the same quantization-aware
  tolerance Windows uses. Extracted the tolerance formula itself to a new
  shared, platform-neutral header (`curve_point_verify_policy.h`) that both
  Windows (`main_runtime_gpu.cpp`, pure refactor, no behavior change) and
  Linux call, so the two platforms cannot drift onto different answers.
  `LINUX_MUTATION_LOCK` (which releases the F-APPLY-CEILING transition
  ceiling) only runs if the curve phase's stronger check passes.
- Verified: `python build.py --test` (new 7940-7948 in
  `tests/linux_vf_write_tests.cpp`), `--gates`, `--check`, `--tidy` (no new
  findings), `--test --asan`, `--fuzz --fuzz-runs 5000`, plain `python
  build.py` (fresh dist, strings for the new log lines confirmed present in
  the Linux binary). Not covered: no fake-driver harness exercises the
  complete `linux_backend_apply()` transaction end to end; no Linux hardware
  run under load.

## 2026-11-12: Prepare compatible curve (operation A) on the GUI Apply path

- A manual Apply whose lock sits below earlier points (the plan proves a
  prefix refusal) now offers to lower those points in the editor draft
  (`ui_curve_prepare.cpp`, `curve_prefix_repair.h`). The repair reuses the
  planner's per-point values and the service's pre-write input builder, so
  it cannot disagree with Apply; accepted values are typed through the
  normal edit path and nothing is written until the next Apply. Projected
  points, shaped baselines, floor and range limits and independent tail
  failures are reported instead of repaired.
- Verified: `python build.py --test` (7900-7939; dropping the floor test from
  the repair fails 7914), `python build.py --check`. Not covered: the dialog
  on a real window; HELP-03..08.

## 2026-11-12: Shared curve planner, listed sub-700 mV points, flatten anchor fix

- Issue 17 (Pascal): an externally flattened 1000 MHz curve, saved and
  reloaded after a reset, was refused at hidden point 22 (662 mV, stock
  1050 MHz). Two causes addressed here: the live detector's exact-first pass
  anchored the flatten at point 67 (1125 mV) because the plateau carried
  sub-MHz bin jitter and only the top run was exact; and the editor could not
  list or save points below 700 mV. The detector now walks ascending
  candidates over listed non-floor points (`vf_flat_tail_recognition.h`), and
  the editor lists every writable graphics point (`vf_writable_point_policy.h`,
  floor frozen per topology binding). The implicit adoption of external
  pre-anchor offsets is unchanged and waits for a reviewed Capture.
- One pure planner (`curve_feasibility_plan.h`) now judges every curve request
  before any write on Windows and Linux, with evidence classes, a one-bin
  quantization gate and bounded structured issues. Behaviour change: unowned
  prefix points over a shaped outgoing curve are conditional instead of
  refused; the post-reset backstop decides.
- Save names an omitted external curve (GUI warning, TUI status line).
- An evidence request for the Pascal reporter (`--json` and `--probe`,
  applied and after Reset) was drafted for the maintainer and not posted.
  Pascal behaviour remains unverified on hardware; fixtures are synthetic.
- Verified: `python build.py --test` (new suite 7800-7899; mutation check
  fails 7821 as expected), `python build.py --check` (Windows MSVC and clang,
  Linux x64/arm64). Not covered: a fake-driver apply harness, a real window
  at 80 listed points, Blackwell writes between the floor and 700 mV.

## 2026-11-05: Themed message box clipboard copy and VF flatten field edit fix

- Themed message boxes (`ui_message_box.cpp`, `message_box_policy.h`) now support
  Ctrl+C, Ctrl+Insert, and `WM_COPY` to copy the complete dialog text to the
  clipboard in standard user32 format (27-hyphen dividers, normalized CRLF endings,
  and 3-space-separated button labels). Cases 1472-1475 verify exact layout and
  sizing across single- and multi-button dialogs.
- Fixed a caret reset regression in the VF curve editor: editing an active flatten
  anchor's MHz field triggered draft re-evaluation and programmatic text updates
  before anchor tracking was decoupled, which reset the caret to index 0 on each
  character. `sync_vf_curve_field_values()` now preserves focused controls,
  `set_edit_value()` elides identical writes, and `WndProc` clears
  `guiLockTracksAnchor` immediately upon user input.

## 2026-10-29: Decode GP102 legacy domain relationships and limits

- The complete format-2 Linux report accepts all four advanced INFO requests.
  Independent decoding agrees on nine domains, legacy clock model 3.0 and
  the GPC master with XBAR/SYS children; both children advertise zero delta
  bounds. Entry 4 is HUB rather than the modern VIDEO mapping; ID 31 reports
  PCIe generation rather than a frequency. Fixed records need separate typing.
- Successful metadata or target readback does not demonstrate independent
  physical adjustment. The existing strict write validation stays unchanged;
  another unchanged passive capture is not needed. Scope is this GP102/driver
  sample, with other Pascal configurations and Windows hardware unverified.
- Cross-version decoding and consistency checks passed, as did source,
  security and public-wiki gates. Documentation only; the application and
  capture protocol are unchanged. [Backend findings](../gpu-backend.md#gp102-capture-findings-2026-10-29)
  record the field geometry, evidence and remaining verification limits.

## 2026-10-29: Separate advanced-domain INFO from client tuning ranges

- Validation: native regressions, source/public-wiki gates and clang-tidy
  (no new findings) passed. Plain build 367 refreshed all 12 packages; hashes,
  sidecars and 20 embedded application/service images were verified. Both
  native Windows x64 variants accepted all four added INFO reads and wrote
  complete 602911-byte reports. Every test process exited. GP102/Linux format 2
  remains hardware-unverified pending the reporter's next capture.
- Parsed the complete stock GP102 capture: the normal CONTROL read succeeds
  with the known version and entry geometry but different wire types; IDs
  15–18 provide measurements while modern IDs 0–2 are refused. Stock zeros do
  not establish writable fields or independent controls.
- Read-only NvAPI implementation inspection identified a separate advanced
  INFO interface. The initial capture's client INFO only describes tuning
  ranges. Format 2 captures both families explicitly, with four pinned inline
  advanced INFO versions and a 1 MiB report cap, retaining every read status.
- Regression fixtures cover the reported wire-type vector, distinct INFO
  routing, zeroed retries, raw tails and untrusted returned version words.
  Tuning validation and write behavior are unchanged. The next hardware request
  is one format-2 report; [backend details](../gpu-backend.md#gp102-capture-findings-2026-10-29).

## 2026-10-29: Preserve unknown clock-domain responses for diagnosis

- Validation: native regressions and source/public-wiki gates passed;
  clang-tidy had no new findings. Plain build 366 produced all 12 packages;
  fresh timestamps, changed hashes, SHA-256 sidecars and 20 packaged
  application/service images matched the build outputs. Both native Windows
  x64 variants produced complete read-only reports; all test processes exited.
- The live Blackwell run accepted the normal mask and refused the broad mask
  with -1; the capture retained both and exited 0. A mixed diagnostic command
  exited 1 without rewriting the report. Pascal/Linux hardware capture is the
  next verification step, using one complete text file from the reporter.
- Added the regular Linux `--clk-domain-dump` command using the shared
  collector, scoped GPU selection and unconditional backend release. A single
  redirected stdout report also carries NVML driver metadata.
- Both parsers reject mixing the capture with another operational command;
  narrow/wide argument fixtures cover the read-only command contract. User
  instructions are in README and the GPU backend topic.
- Captures try normal and all-domain read masks separately, so modern mask
  assumptions cannot hide Pascal domain bits. A refused broad read preserves
  the accepted normal response. Production Linux wrapper fixtures verify
  initialization, unresolved selection, driver metadata and teardown on errors.
- Added the shared bounded raw collector and wired the existing Windows
  `--clk-domain-dump` through it, retaining successful layout mismatches and
  failed reads with their exact statuses. Normal tuning validation is unchanged.
- Fake-driver fixtures cover unknown and known responses, zero/tail retention,
  request isolation, missing interfaces, bounded output and read-only resolution.
- Updated [GPU backend guidance](../gpu-backend.md#raw-clock-domain-capture-2026-10-29):
  Pascal may reuse the modern version word with different entry geometry;
  advanced domain identities and units need raw evidence before any writes.

## 2026-10-29: Route restricted Windows deliverable builds explicitly

- Extended [build guidance](../build.md#windows-agent-build-execution-verified-2026-10-29)
  to reuse the approved native-test execution route for the complete plain
  build and packaging matrix. Both Zig caches were already workspace-local.
- A single-source ARM64 Zig compile failed with `error: Unexpected` under a
  restricted token even with fresh local caches and temporary files, then
  passed unchanged through the approved route. The exact refused OS operation
  is unverified. No compiler flags, cache-repair rules or validation changed.
- Plain build 365 passed through that route and refreshed all 12 packages;
  packaging showed no separate fault. Static analysis passed in the sandbox.
- The full `--gates` run also needs that route: the Bash-backed Linux setup
  escaping fixture's child failed to initialize under the restricted token.
  The approved rerun passed all source/Python gates.

## 2026-10-29: Explain service diagnostic dumps after recovery

- The service captures a driver-fault dump before requesting recovery, so a
  dump can remain after a successful driver restart or installation. The
  notice now explains this before listing every dump kind. Filenames alone
  do not establish recovery outcome and no dumps are filtered out.
- Case 7150 covers the explanation for service-only, WER-only and mixed
  inventories. Existing bounded-formatting, literal-command and native
  scan/write tests remain relevant. Existing count/write/failure diagnostics
  are sufficient because only presentation changed; no runtime logging added.
- README corrects the dump location and recommends keeping the WER service
  available and LocalDumps registration intact, while distinguishing local
  capture from reporting to Microsoft. Disabling the WER service itself is
  untested. Sources: [Windows architecture](../windows-architecture.md),
  [tests](../testing.md).
- Full native regressions, Linux cross-links and source/Python gates pass;
  static analysis reports no new findings. Mutating the recovery explanation
  fails case 7150. Plain build 365 refreshed all 12 current packages; timestamps,
  changed content and SHA-256 sidecars were checked, and the four Windows
  service variants carry the new wording in both archives and installers.

## 2026-10-29: Log a repeated ClkDomains refusal once

- The Pascal hardware report's journal repeated the ClkDomains layout-mismatch
  line on every daemon state read. The shared reader now records the refusal
  without logging; one-shot callers keep logging every refusal, while the Linux
  per-refresh read uses a per-GPU steady-state tracker. The read cadence,
  capability classification and the unknown-version latch are unchanged.
- Cases 7730-7739 drive the production reader and logger with a fake driver
  that answers like the reported Pascal card. Source anchors:
  [debug tools](../debug-tools.md#steady-state-summaries-and-verbose-mode-added-2026-10-17),
  [GPU backend](../gpu-backend.md), [Linux scaffold](../linux-scaffold.md),
  [tests](../testing.md).
- Open question: whether Pascal exposes an XBAR domain under another layout is
  unverified; the domain stays refused there.

## 2026-10-29: Physical VF graphics units on Pascal

- A hardware report confirms successful Pascal VF reads after the point-mask
  correction and shows double-rate graphics values. The shared backend now
  records Pascal's scale 2; readers normalize only the leading graphics domain,
  writers apply the inverse conversion, and partial batches preserve raw
  control blocks. Other families and NVML scalar offsets keep their units.
- Probe, self-test and binding/curve diagnostics identify the scale. Cases
  7717-7728 execute raw reads, signed writes, partial passes, Reset and target
  planning with fake driver dependencies. Corrected values and physical
  per-point writes still require Pascal hardware confirmation. Source anchors:
  [GPU backend](../gpu-backend.md), [tests](../testing.md).
- Full regressions, Linux cross-links, source/Python gates and tidy pass.
  Plain build 363 refreshed all 12 current packages; checksums and all 20
  packaged application/service images match the fresh outputs. The Windows
  read-only self-test on an RTX 5070 reports FULL with 127 graphics points.
  Previous reader/writer fixtures fail at 7682/7718; corrected fixtures pass.

## 2026-10-29: Accept explicit backend-retry state reads

- Snapshot/telemetry reads now admit the interactive retry flag that their
  client and handler already use. Other read verbs remain flag-free; mutation
  fields and shared-slot bits still refuse. Malformed daemon requests report
  the validator's fixed reason in the reply and debug log.
- Cases 7710-7716 execute the flag builder and validator together. The
  standalone fixture fails at 7710 with the previous protocol headers and
  passes after correction. Source anchors: [Linux](../linux-scaffold.md),
  [tests](../testing.md).

## 2026-10-28: VF layout in Linux probe and self-test

- Review of the Pascal VF-info fix: it removes the cause of the reported
  refusal, but status/control on Pascal, the graphics-run split and Pascal
  writes remain unverified on hardware. A refused snapshot published only
  `0 points` and one reason, too little to diagnose a new family's table.
- Each Linux VF read now records a layout summary, kept when validation
  refuses; the probe report, `--self-test` and the health-transition debug log
  print it as one line. The console probe names the graphics-run peak instead
  of the last point in the read window. `linux_vf_validation.h` now includes
  `platform.h` itself instead of relying on include order.
- Cases 7698-7709; mutation checks fail 7708 and 7698. Full regressions pass.
  Plain build 362 refreshed all 12 current packages with matching SHA-256
  sidecars; both Linux binaries contain the new layout output and the x64
  archive binary matches the build output. No GPU was read or written; the
  layout line on real Pascal hardware awaits a user report.

## 2026-10-28: Valid probe telemetry queries

- The probe uses supported `nvidia-smi` display categories for power and
  clocks, and a separate fan-speed property query. The old `FAN` display
  category reproduces exit 2; both corrected commands return exit 0 with the
  installed CLI. The diagnostic captures retain their existing size bounds.
- Full regressions, source gates and static analysis pass. Plain build 361
  refreshed all 12 current packages; checksums and 20 embedded application or
  service images were independently compared with the fresh outputs. The
  Windows x64 read-only self-test on an RTX 5070 reports FULL with 128 VF
  points and a readable control table. Pascal hardware verification remains
  pending; these checks perform no GPU writes.

## 2026-10-28: VF point-mask validation on Pascal

- VF-info `0x14` is part of the point mask, not a clock count. Valid Pascal
  responses can have zero there. Removed the count assumption from shared
  tables, Windows/Linux readers and probe diagnostics; status reserved words
  remain zero. Existing snapshot structure checks still apply.
- The production atomic read helpers now have an executing fake-driver
  fixture, cases 7680-7697. The Pascal case fails with the previous reader and
  passes after correction. Diagnostics distinguish skipped reads from missing
  functions and report mask acceptance on transitions. Hardware confirmation
  on Pascal remains pending.

## 2026-10-28: Isolated two-hop updater release simulation for 0.31.0

- Autonomously executed the prescribed isolated two-hop updater simulation
  (`update-procedure.md` Section 10, `tools/simulate_release.py`) for candidate
  0.31.0 against throwaway test repository `aufkrawall/green-curve-update-test`
  using an offline simulation key.
- Baseline 0.30.0 was built from git tag `0.30.0` repointed to the test repo,
  installed silently, and seeded with the client's active settings.
- Hop 1 published candidate 0.31.0 to the test channel; baseline 0.30.0
  discovered, verified, staged, and installed 0.31.0, preserving all 334
  settings fields through `pending-update-restore.ini` handoff replay.
- Hop 2 published follow-up patch 0.31.1 to the test channel; the newly
  installed 0.31.0 client verified and applied the update cleanly, confirming
  updater non-regression.
- Reinstalled production 0.31.0 client with active profile 1 restored.
  Teardown deleted test releases and tags from the clone repository, reverted
  repo visibility to private, and restored the machine's `highest_seen_version`
  watermark back to 0.30.0. The production repository remains untouched at 0.30.0.
- Machine-readable evidence and report recorded in
  `dist/test-simulation/20261028-b65935e/`.

## 2026-10-28: Review follow-up: memory plan evidence and recipe digest drift

- An external review of `f9c23ba..6f2bd10` found that the selection reset
  left the memory Apply planner comparing against the previous GPU's cached
  offset: with failed memory reads on the new GPU, an equal target skipped the
  setter and reported success. The planner now requires fresh readback
  (`mem_offset_write_required()`), cases 7676-7678; the earlier setter-level
  test could not see the planner.
- The same review noted that both Arch recipes name 0.31.0 with 0.30.0 remote
  digests. That is inherent until publication; recipes now name the release
  their remote digests belong to, and every build prints a NOTICE while it
  differs from `pkgver`.

## 2026-10-28: Prepare the 0.31.0 release

- `VERSION`, README install examples, both Arch recipes and the wiki index
  name 0.31.0. The recipes keep the 0.30.0 archive digests until the release
  publishes its inputs; pin them from the published bytes afterwards, as for
  0.30.0.
- The changelog section became `## 0.31.0` (387 words, related bullets
  merged) and `tools/release_prep.py` accepts it against `VERSION`.
- Cases 5944-5946 cover exact-version restore handoff for 0.31.0; cases
  7670-7675 (`tests/update_release_031_tests.cpp`) cover manifest
  eligibility from every public release, including both 0.30 spellings.
- The maintainer reported normal behaviour in hardware smoke tests on Windows
  and Linux before the bump. The two-hop updater simulation was subsequently
  performed and verified before publication (see entry above).

## 2026-10-28: Review follow-up: per-GPU write evidence and memory gating

- A review of the 2026-10-27 commits found that a GPU selection change cleared
  power write evidence but kept the memory uncertainty flag and the fresh
  scalar readbacks. `reset_selected_gpu_write_evidence()` now clears all of
  them in one place and logs what it discarded.
- An unverified memory write now refuses an Apply that does not set memory,
  like power already did; one precheck names every unverified domain.
- The Linux output-directory error names the requested path again.
- Linux read-time backend discovery logs its duration and warns when it
  exceeds the state refresh budget; the in-request design is recorded as a
  limit (it matches targeted GPU switching).
- Cases 7660-7669. Sources: [clock recovery](../clock-transition-audit.md),
  [Linux](../linux-scaffold.md), [tests](../testing.md).

## 2026-10-27: Power uncertainty follows actual write attempts

- Missing power writers, unresolved fallback targets and failed process
  creation preserve prior readback. An invoked NVML setter or started fallback
  retains uncertainty until its target verifies; failed launch restores the
  prior state without clearing uncertainty from an earlier write.
- Cases 7650-7657 execute those boundaries and the unrelated-Apply precheck.
  Case 7650 fails under the previous assignment; the standalone fixture passes.
  Process creation is fake and no physical GPU writes are performed.
- Full native regressions, Linux cross-links, source gates and static analysis
  pass. Plain build 358 refreshed six archives, four installers and two Linux
  packages; timestamps, changed content, checksum sidecars and all 20 packaged
  application/service binary payloads were independently verified.
- Sources: [clock recovery](../clock-transition-audit.md),
  [tests](../testing.md).

## 2026-10-27: Memory recovery requires memory-domain evidence

- The Windows memory setter accepts the P-state fallback only when that
  response supplies a usable memory clock and the stock maximum is readable.
  Graphics-only responses retain uncertainty; diagnostics show both inputs.
- Cases 7640-7645 execute missing, matching and contradictory evidence,
  repeated stock recovery and a missing denominator. Case 7640 fails with
  the previous predicate. The standalone fixture passes without GPU writes.
- Sources: [clock recovery](../clock-transition-audit.md),
  [tests](../testing.md).

## 2026-10-27: Failed writes and late Linux recovery keep clear ownership

- Windows core recovery now accounts for a first fan-setting attempt even when
  no success counter moved. Power writes stay recoverable until their exact
  target verifies; rollback, Reset and a power-owning Apply can force a repair
  from the last proven surface, while an unrelated sparse Apply refuses.
- Linux state reads may rediscover a backend without performing fan hardware
  or ownership-record work. Deferred handback runs in the worker, and automatic
  restore rechecks its guard, uncertainty, pending ownership and selected fan
  target after backend preparation. Startup no longer adds a second
  uncertainty transition.
- Cases 5446-5448, 7607, 7621-7626 and 7630-7633 cover the new boundaries.
  Full native Windows regressions, Linux cross-links, source gates and tidy
  passed. Build 357 produced six fresh archives, four installers and two Linux
  packages; every packaged application and Windows service payload matched the
  built binary. No physical GPU writes were performed.
- Sources: [clock recovery](../clock-transition-audit.md),
  [fan control](../fan-control.md), [GPU backend](../gpu-backend.md),
  [Linux](../linux-scaffold.md), [tests](../testing.md).

## 2026-10-27: Review follow-up for recovery and portability

- Portable advanced profile fields, deferred Linux backend bring-up, bounded
  trusted output-directory links, fan ownership and durable uncertain intent
  now have focused coverage and diagnostics. Physical GPU replay is unverified.
- The native and Linux cross-link harnesses share their test-source list.
- Sources: [Linux](../linux-scaffold.md), [fan control](../fan-control.md),
  [GPU backend](../gpu-backend.md), [tests](../testing.md).

## 2026-10-27: Windows ClkDomains readers use the response schema

- Windows SYS/VIDEO readback after apply, reset-before-apply, Reset and the
  capability probe now uses `xbar_read_entry_freq()` like Linux and telemetry.
  The clock probe and dump take their geometry from the response schema; the
  probe aborts on a schema change. No behaviour change with today's single
  schema row; a second row would have mis-read and mis-written fields.
- Gated in `tools/xbar_gates.py`; built and tested once with the batch.
- Sources: [GPU backend](../gpu-backend.md).

## 2026-10-27: Windows NVML binding compares device IDs

- Windows matched NVML devices to the selected NvAPI GPU with a clause that
  compared the low words of packed IDs, which is the NVIDIA vendor ID on every
  board; identity then rested on subsystem ID and location alone.
- Both platforms now share one extractor (`gpu_selection_policy.h`); each
  rejected NVML candidate is logged with its device and bus ID.
- Cases 7610-7613. Not run on multi-GPU hardware.
- Sources: [GPU backend](../gpu-backend.md#gpu-identity-and-selection),
  [tests](../testing.md).

## 2026-10-27: Windows power writes target and verify one GPU

- The `nvidia-smi` power fallback passes `-i` with the selected GPU's PCI bus
  ID and refuses without one; without `-i` it changed every GPU, including in
  rollback. The ID is validated before reaching the command line.
- Both write routes now require the reported limit to be within 1 W of the
  target instead of accepting any readback.
- Cases 7600-7606 and readback gates. Not run on multi-GPU hardware.
- Sources: [GPU backend](../gpu-backend.md), [tests](../testing.md).

## 2026-10-27: Deferred Linux fan handback runs when the GPU comes up

- A start that found inherited fan ownership but no usable GPU backend now
  runs the counted handback once a later request or automatic-restore
  re-initialization brings the backend up, before that path selects its GPU.
- Cases 7595-7599; case 7596 fails without the deferral. No GPU writes.
- Sources: [fan control](../fan-control.md), [tests](../testing.md).

## 2026-10-27: Failed Linux operations keep existing uncertainty

- A failed explicit Apply/Reset that started uncertain stays uncertain after
  a clean rollback, and its record restore writes UNCERTAIN instead of the last
  ACTIVE intent, so no later start replays unproven settings.
- The mutation deadline now counts the fan ownership marker writes.
- Cases 7590-7594; case 7592 fails under the previous rule. No GPU writes.
- Sources: [Linux](../linux-scaffold.md#durable-state-is-part-of-automatic-success),
  [tests](../testing.md).

## 2026-10-27: Linux uncertain state returns the owned fan

- Failures that make the daemon state uncertain after a hardware write now
  return an owned manual fan to the driver before the worker idles, with
  emergency duty if the driver refuses. Windows rollback already did this.
- Ownership binding is respected; refusals are logged and change nothing.
- Cases 7580-7589 execute the helper; case 7581 fails without the release.
  Fan gates pin every post-write call site. No physical GPU writes.
- Sources: [fan control](../fan-control.md), [tests](../testing.md).

## 2026-10-27: Linux TUI selection advances across refused adapters

- Explicit selector choices advance before daemon adoption, so forward cycling
  reaches a held GPU even through refused intermediate adapters. Writes still
  require that the chosen GPU is shown.
- Explicit and automatic switches share refusal tracking. A busy response or
  disconnect retains the choice for adoption; answered refusals wait for a
  changed daemon state, another choice or explicit Refresh.
- Cases 7571-7574 execute three-adapter recovery, refusal suppression, busy and
  reconnect paths. The held-state case fails without the selector change.
- Sources: [Linux TUI](../linux-scaffold.md), [tests](../testing.md).

## 2026-10-27: Loaded Linux fan ownership remains target-bound

- Retained startup ownership keeps its full adapter, role and UUID binding in
  memory. Apply and Reset validate it before mutation; only matching verified
  recovery retires it. A matching manual-fan Apply durably assumes ownership.
- Pending inherited recovery survives shutdown and exhausted automatic attempts.
  Sparse writes without fan recovery retain the restore lockout. Legacy explicit
  Reset selection and optional-identity compatibility remain supported.
- Executing cases 7560-7570 cover retained ownership across later operations;
  the first case rejects the prior behavior. Integration gates cover mutation
  ordering, shutdown routing and lockout preservation. Full native Windows
  regressions pass. No physical GPU writes.
- Sources: [fan control](../fan-control.md), [tests](../testing.md).

## 2026-10-27: Fan runtime and recovery evidence

- Linux worker state and wake generations are captured coherently, and a
  completed Apply invalidates an already-due poll's old hysteresis state.
- Fan verification treats a known driver target as authoritative. Windows
  carries target validity independently of zero or stale numeric values.
- Linux fan ownership v2 retains adapter, location role and optional UUID.
  Startup resolves that evidence before handback; ambiguous legacy records
  retain ownership and publish the recovery action without writing.
- Memory setters retain uncertainty until fresh evidence verifies the target.
  Rollback and explicit Reset force writes past cached equality; memory
  recovery is reported separately from graphics-core clamp release.
- Executing fixtures cover these boundaries and fail under negative controls.
  Full native Windows regressions pass, the new fixtures execute under Linux,
  and native Linux marker storage preserves identity and legacy compatibility
  while rejecting invalid permissions and truncation. No real GPU writes.
- Static analysis reports no new findings. The plain build passes; all six
  release archives, four installers and two Linux packages are fresh, with
  payload binaries matched against their build outputs. Staged and committed
  changes pass redacted secret scanning.
- Sources: [fan control](../fan-control.md),
  [clock recovery](../clock-transition-audit.md), [tests](../testing.md).

## 2026-10-27: Agent workflow baseline alignment

- Synchronized `AGENTS.md` with upstream template updates:
  - Added universal harness sandboxing and auto-approval denial guidance: agents
    must adapt execution within sandbox, environment, or auto-approval constraints
    (using project-local paths, workspace temporary directories, or approved
    non-interactive flags) and must never skip or weaken validation checks.
  - Added explicit pre-commit workflow rule requiring task-owned changelog-worthy
    changes to be recorded in the `## Unreleased` section of `CHANGELOG.md`.
- Sources: `AGENTS.md`, `llm-wiki/changelog-style.md`.

## 2026-10-27: Apply recovery and output identity invariants

- Advanced controls share the apply commit boundary. Requested surfaces are
  checked before mutation; failed writes reset requested advanced fields and
  enter core recovery. Cleanup ownership covers partial setter effects and is
  released only after verified zero. SYS editing follows its own probe.
- Pascal FLATTEN plus a nonzero uniform scalar write is refused before reset.
  Existing selective VF and ordinary unsupported-family operations remain
  available. This is a transition restriction, not hardware validation of a
  replacement scalar route.
- Privileged atomic text output walks/creates every directory by descriptor
  without following links; ordinary users retain redirected configuration
  directories. Client-identity restoration has a fatal, non-returning outcome.
- Focused tests execute the advanced boundary and real ClkDomains helper,
  partial setter effects and cleanup, scalar-refusal policy, directory walking
  and the fatal identity scope in a child. Full apply orchestration and live
  Pascal/advanced hardware remain unverified.
- Validation: complete native Windows regressions pass, including filesystem
  security and pipe fixtures; static analysis reports no new findings. The
  plain deliverable build passes for both Windows toolchains and both Windows
  and Linux architectures. All six release archives match fresh built binaries;
  all four installers and both current Linux packages have fresh timestamps.
  Native Linux output tests pass under ordinary and root identities. Negative
  controls reject omitted advanced recovery, scalar refusal, identity termination
  and directory no-follow enforcement. No real GPU writes were performed.
- Sources: [clock transitions](../clock-transition-audit.md),
  [Linux output](../linux-scaffold.md#clitui-atomic-text-writer-verified-2026-10-27),
  [service identity](../windows-architecture.md), [testing](../testing.md).

## 2026-10-24: Optional physical GPU identity for Linux restore

- State v7/startup v6 store an optional physical UUID. Frozen schema-3 loaders
  preserve every preceding role migration, including the first-generation
  compatibility re-read; old records need no manual rebind on upgrade.
- UUID evidence follows the same card across bus moves and distinguishes an
  identical replacement. Missing API support or failed optional reads retain
  the old resolver and do not disable unsupported GPU support. An unavailable
  query preserves the backend fast path and any stored UUID.
- Apply, startup bind, automatic restore, resume and startup content refresh
  carry the binding. A legacy boot profile captures it after success even
  without a bus move. Diagnostics expose verdicts/presence only.
- Fixtures 7460-7481 cover policy, query failures and record binding; 7450-7459
  use the actual frozen predecessor sizes and checksums. First-restore legacy
  ambiguity remains an explicit compatibility limit.

## 2026-10-24: The Linux regression suite runs in an agent sandbox

- Inside the sandbox the suite failed at 6731 (a hard-coded `/tmp` path on a
  read-only `/tmp`), then at the transport fixture (`socket(AF_UNIX)` refused
  by seccomp). Fixture scratch paths now follow `$TMPDIR`
  (`tests/posix_test_environment.h`); the transport fixture's daemon socket
  path moved under it too, so parallel runs no longer share a fixed path.
- The transport fixture reports a refused AF_UNIX socket as blocked (exit
  120) after its socketpair cases; build.py keeps going and finishes with
  `Regression tests INCOMPLETE`, non-zero. The named-socket cases still need
  an unsandboxed run (testing.md, "Linux agent sandbox execution").
- Every Linux test run raised a KDE crash notification: the crash-report
  fixture's deliberate SIGSEGV reached systemd-coredump despite
  `RLIMIT_CORE=0`. The child is now non-dumpable; no coredump entry since.

## 2026-10-24: Legacy Linux restore records widen as `unrecorded`, not `required`

- Reported on Linux: the boot profile stopped applying after updating past
  "Linux automatic restore retains GPU location authority". The daemon
  journal showed the cause: the v3 startup record migrated as
  `role=required` at bus 07, the only GPU now sat at bus 06, and the boot
  apply refused before any write ("the stored GPU is not present"). The
  previous build had applied the same record on the same boot.
- `required` was stricter than any build had read these records; Windows
  reads a role-less `[gpu]` record as `unrecorded`, and so did every earlier
  Linux restore. All five widening paths (schema-1 state v2/v3, schema-1
  state v1, schema-1 startup, schema-2 state and startup) now use
  `LINUX_DAEMON_LEGACY_LOCATION_ROLE` (`unrecorded`). The two schema-1 state
  paths and the schema-1 startup path had reached `required` silently through
  the initializers' default argument.
- Follow-up the same day: records those unreleased builds had already
  rewritten as state v5 / startup v4 `required` kept failing after the
  update (journal: same refusal on the fixed build). State v6 / startup v5
  now re-read that generation's `required` as `unrecorded` on load, so no
  user has to re-bind anything. Fixtures 7450-7459; keeping the stored role
  fails 7451. This supersedes the "Considered and kept" note below.
- Open gap: the TUI showed `PROFILE 1` while the boot apply had been refused.
  A pre-write failure sets no lockout (by design) and reaches no client.
- New diagnostic: an aborted automatic restore logs
  `[stored=<bdf> role=<role> remedy=<...>]`; migration lines log the real
  role. Fixtures 7423/7424/7426 changed, 7440-7447 added; the pre-fix
  widening fails 7423.

## 2026-10-24: Restore-role review follow-ups

- A committed Reset now clears `g_activeTargetRole` together with the active
  target (back to the fail-closed REQUIRED default); every reader already
  checked `g_hasActiveDesired` first, so this is state hygiene, source-gated.
- The restore-role suite's failure now prints the harness's
  `regression assertion failed: code` line like every other suite, and its
  7420-7429 range is listed in `tests/regression_main.cpp`.
- Considered and kept: a migrated record stays REQUIRED across automatic
  restores. Promoting it after an exact-location restore would need a marker
  for "migrated, ambiguity unknown" versus "a twin existed at selection";
  one explicit Apply or startup bind records the real role.

## 2026-10-24: Saving the Linux GPU selection never rewrites an unreadable config.ini

- Review follow-up. `save_linux_gpu_selection()` started from an empty
  document when config.ini could not be read, so the rewrite kept only
  `[gpu]` and replaced every profile. Since the same day an unreadable
  config.ini loads as a MALFORMED selection whose remedy is `--gpu`, i.e.
  exactly that save. It now fails with "it was not overwritten".
- The record I/O moved to `linux_gpu_target_config.cpp` first (separate
  commit), which brought `linux_port_profiles.cpp` under 800 lines.
- New native fixture `linux_gpu_selection_store_regression` (Linux hosts run
  it, others cross-link it); run under WSL here, it passes, and the pre-fix
  save fails its five unreadable-file assertions. Log line: `gpu selection:
  not saved; config.ini cannot be read`.

## 2026-10-24: The Linux TUI hold is recoverable, and refusals are not retried per tick

- Review follow-up to "The Linux TUI writes only to the selected GPU". A hold
  (the daemon keeps GPU A for its active settings while B is selected) was a
  dead end inside the TUI: Reset to A was blocked (A not selected), and the
  selector stepped from the daemon's selection, so it could only offer B,
  which the daemon refuses. The selector now steps from the selection; one
  step selects A, Reset is allowed, the next step selects B.
- A switch the daemon ANSWERED with a refusal for another reason was asked
  again every second and reported as "Daemon is busy". The transport now
  reports `answered`; refusals say so, and a refused switch waits for a
  change in the daemon's instance, generation, topology, adapter count or
  active settings, a new selection, or a user Refresh.
- New adoption steps DEFERRED (a staged draft: the selector's own rule) and
  BLOCKED (unresolved, including malformed: now announced after an offline
  start and after every reconnect, replacing the start-only special case).
- Fixtures 7409, 7430-7436; negative controls fail at 7431 (selector from the
  daemon's selection) and 7432 (refusals not recorded). Log lines: `tui: GPU
  selector step=`, `tui: switch to the selected GPU refused:`.
- Checked and left as is: the review's "edits to sub-500 MHz points are
  silently accepted". The batch writer accepts a pinned-zero readback there,
  but the final apply verification judges an explicit (absolute) point by its
  MHz and fails the apply; only offset-intent points accept a lower offset,
  which is the global-offset contract.

## 2026-10-24: Profile editor projection shares the VF grid visibility rule

- The profile loader filters saved points through the shared population and
  voltage predicate. Zero-frequency live entries are removed even above the
  voltage threshold; visible floor points and ownership-read intent remain.
- Fixtures 7390-7392 cover mask/MHz/provenance clearing and retained points;
  a source guard keeps the loader on that helper behind its editor gate.
  Diagnostics report a bounded dropped-point count. The lock-tail comment
  now describes current visibility instead of the former frequency rule.

## 2026-10-24: Linux automatic restore retains GPU location authority

- State v5 and startup v4 carry the selected location role under their
  checksum. Restore-last, profile boot apply, resume, backend replacement
  and startup content refresh retain it; a required selection stays exact
  when only the other identical card remains.
- Freeze state v4/startup v3 and DesiredSettings schema 2. Older records
  widen as required without changing settings or operation identity. They
  still restore at the exact location; moving needs explicit selection.
- Fixtures 7420-7429 and runtime forwarding guards; dropping the backend
  role is detected by 7421. Migration and restore logs include the role.
  See persistence-schema.md and linux-scaffold.md for upgrade behavior.

## 2026-10-24: A malformed Linux config.ini GPU record stops client writes

- Closes the 2026-10-23 "left as is": an unparsable BDF or role spelling in
  `[gpu]` loaded as no record, so CLI and TUI writes went untargeted and
  reached a lone remaining twin. Now MALFORMED (also a config.ini that exists
  but cannot be read): writes stop, reads go untargeted with a warning,
  `--gpu` replaces it; the TUI says so at start. Same as Windows.
- Checked first: every writer since v0.19 stores a live adapter's non-zero
  BDF, so no legitimate record is location-less.
- Fixtures 7410-7414; `gpu selection: loaded record=` and malformed-record
  log lines (key lengths only).

## 2026-10-24: The Linux TUI writes only to the selected GPU once it is shown

- Review follow-up. `--gpu B --tui` showed and wrote the daemon's current GPU
  A (an EXPLICIT selection was never switched to), and a config.ini record
  marked RESOLVED stayed so when its switch was refused for A's active
  settings or failed as busy, so Apply/Reset/bind reached A.
- The TUI now switches to the selected GPU after every answered untargeted
  refresh until it is shown, and writes need the selection to BE the shown
  GPU (`linux_tui_shown_gpu_write()`). See linux-scaffold.md, "GPU selection
  across PCI bus renumbering".
- Fixtures 7400-7409 (testing.md, "Linux TUI write target"); 7403 fails on
  the previous code. `tui: selected GPU adoption` log line per transition;
  blocked writes log the verdict with both locations.
- Kept: the daemon's exact mutation-identity check. A review suggested
  normalizing a stale request target there, but the daemon checks mutation
  preconditions before selecting the request GPU, and the CLI and TUI attach
  the live identity from their state read, so no client path sends a stale
  target to a mutation.

## 2026-10-24: Native Windows agent tests use approved execution

- Added an always-on agent instruction pointing to testing.md's Windows
  sandbox launch procedure. The full suite starts through the execution
  tool's approval route, with process-local temporary files in the workspace,
  so filesystem-security fixtures can exercise normal Windows permissions.
- Verified the unchanged full suite passes through that route; the restricted
  run failed at protected-directory assertion 6606. Assertions, fixture
  coverage and global sandbox settings are unchanged.

## 2026-10-24: The VF grid no longer shifts between a cold and a warm read

- Report: after opening the window from the tray, the lock checkbox and the
  points around it sat one column too far left until Refresh. The log showed
  76 visible points from the logon reads (GPU at 22 C) and 78 after Refresh
  (30 C); a CLI `--dump` confirmed a flat 180 MHz floor and a steep low ramp
  whose position moves with temperature, so the `>= 500 MHz` visibility term
  cut through the drifting ramp.
- Visibility is now voltage only (`vf_point_visibility_policy.h`), shared by
  the grid map and `is_curve_point_visible_in_gui()`. Visible change: the
  floor points at or above 700 mV (ci 40-48 on an RTX 5070) are in the grid
  and the graph, whose frequency axis may now start at 0. The tray-reopened
  window still shows the snapshot from its last full read until Refresh; only
  the layout no longer depends on that read's temperature.
- Fixtures 7380-7389 (testing.md); `check_vf_point_visibility_is_drift_free()`
  in `tools/ui_gates.py`; `vf visibility:` change log line.

## 2026-10-23: An unresolved Linux config.ini GPU stops client writes

- Review of the post-0.30.0 commits found that a config.ini selection that
  failed to resolve was still sent stale (CLI) or adopted as the TUI target.
  The daemon resolves requests without the role, so a `required` twin record
  whose card was removed could reach the remaining identical card; dropping
  the target instead would hit the untargeted lone-adapter rule.
- Client writes (CLI Apply/Reset/startup bind, TUI Apply/Reset/bind) now stop
  unless the selection resolved or was chosen explicitly; reads go
  untargeted. See linux-scaffold.md, "GPU selection across PCI bus
  renumbering".
- Fixtures 7371-7379 (testing.md); `check_gpu_location()` guards extended.
  Left as is: a malformed `[gpu]` record (bad BDF or role spelling) is still
  ignored, so it writes untargeted like no record; Windows blocks it instead.

## 2026-10-22: Linux follows a renumbered GPU too

- Windows fix verified on the reporting system after updating: the service
  followed the model from bus 07 to 06 and the GUI rewrote `[gpu]` with bus 6
  and `role=advisory`.
- Linux stored targets (config.ini, ACTIVE and startup records, requests) now
  resolve through `linux_resolve_stored_gpu_target()`. The daemon no longer
  refuses a stale request for the active GPU, a one-GPU init no longer
  resolves writes for a different model, the startup record re-binds after
  booting on a moved GPU, and the client resolves and refreshes config.ini
  with its recorded role before naming it. See linux-scaffold.md, "GPU
  selection across PCI bus renumbering".
- Fixtures 7340-7370 (testing.md); `check_gpu_location()` source guards.

## 2026-10-22: A PCI bus renumbering no longer orphans the selected GPU

- Disabling an on-board NIC in the firmware setup moved a single RTX 5070 from
  bus 07 to bus 06. The `[gpu]` record required an exact bus match, so logon
  apply stayed pending ("configured GPU identity is not present") and the GUI
  refused Apply/Save with "missing or ambiguous" on a one-GPU system.
- The location is now a tiebreaker between identical adapters; a new
  `selected_pci_location_role` records whether it was one at selection time.
  Role-less records move only with exactly one adapter present. The GUI
  refreshes a moved or role-less record after the service snapshot. See
  config-profiles.md, "The PCI location is a tiebreaker, not the identity".
- Fixtures 7300-7329 (testing.md); 7300 fails on the previous resolver.
  Resolution diagnostics now name stored and live locations in one line.
- Linux had a related but different limitation (stale BDF target, no heal
  path); fixed in the entry above.

## 2026-10-22: Stable 0.30.0 publication verified

- Published `bba83d97bb4e11da517855f1d9076088cd7a7a35` through CI `39830049166` and release workflow `39832643745`.
  The signed tag, all 21 assets, program/toolchain provenance, package digests
  and release notes were verified; see build.md, "Last Verified".
- Both updater metadata formats verify through anonymous client URLs. The
  floorless manifest passes the oldest public client's policy and the actual
  0.29.0-to-0.30.0 Windows x64 update preserved all 334 captured settings and
  profile-identity fields; see updates.md, "Last verified", for coverage limits
  and signed freshness expiry.
- Repinned both Arch recipes to the delivered release bytes after comparing
  the published source archive against the reviewed Git tree.

## 2026-10-22: Stable 0.30.0 preparation and two-hop update verification

- Versioned notes and installation examples use the exact 0.30.0 asset spelling.
  The release remains floorless and retains existing GPU support and profiles.
- Fixtures 7290-7295 cover all public updater-bearing installed versions, both
  Windows architectures, current/newer version decisions and exact asset names;
  see testing.md, "Stable 0.30.0 update eligibility".
- An isolated stable 0.29.0 baseline completed two signed update hops. CLI and
  service-protocol commands exercised download, verification, installation and
  production settings replay; all 334 captured fields matched after both hops
  and normal-client restoration. Successful teardown restored the seen version
  and applied settings, and removed the test publications. Update-dialog button
  capture was not automated; see updates.md, "Testing a release without publishing one".

## 2026-10-22: GPU-family wire validation is defined for every input

- Linux CI's response fuzzer stopped in the envelope validator while reading an
  out-of-range `GpuFamily`. The range check could not safely reject it because
  the enum lacked a fixed underlying type.
- `GpuFamily : int` follows the existing `LockMode` rule, preserving its
  four-byte wire layout and the validators' semantic rejection rules.
- New raw-byte fixtures 7280-7284 cover snapshot and adapter fields, refusal
  payload checks, invalid integer extremes and all accepted families. They run
  in both native and Linux cross-linked regression harnesses. The standalone
  Linux sanitizer fixture failed before the fix and passed afterward; see
  testing.md, "GPU-family wire representation".

## 2026-10-22: The MinGW CFG shim and its linker warning are gone

- Every llvm-mingw x64 link (GUI, service, setup, uninstaller) warned
  `duplicate symbol: __guard_check_icall_fptr`, documented as cosmetic. It was
  not: x64 clang uses CFG *dispatch* (374 call sites, 0 references to the
  check pointer), so the `cfg_glue.cpp` override was never called, and the
  load config's check slot pointed at it in `.text`; a running GUI showed the
  loader's validator address written over the function's first 8 bytes.
- Removed the shim, `--allow-multiple-definition` and the audit exemption;
  any duplicate symbol now fails the link. New `pe_verify.verify_cfg_pointer_slots()`
  refuses a CFG slot in code (failed on the pre-fix binaries, passes on the
  MSVC-ABI ones); self-tests in `pe_verify_tests.py`.
- Corrected build.md ("CFG on llvm-mingw x64"), windows-architecture.md and
  testing.md, which described the shim as the CFG mechanism.

## 2026-10-22: Applied-profile tick flickered at every client start

- Symptom: the tray read "Manual settings" for about a second after the GUI
  started, and the log showed `applied_slot 3 -> 0 reason=service published
  no active intent` then `0 -> 3` at nearly every GUI/CLI start that day.
  Reproduced on demand with `--export-active-settings`.
- Cause: `apply_service_snapshot_to_app()` syncs the indicator as its last
  step, but three callers stored the envelope's active intent only after it
  returned, so the new identity was judged against the previous intent.
- Fix (config-profiles.md, "Identity and intent come from one envelope"):
  the projection takes the intent and stores it with the identity before the
  sync. Source gate `check_applied_profile_projection_order`, verified failing
  with one post-hoc assignment restored. `main_state_sync.cpp` ratchet 915 ->
  919.

## 2026-10-22: Simulation teardown restores the seen version for real

- Symptom: after the 2-hop release simulation, the real build warned on every
  check that the update channel offers an older release. The machine config
  still held the hop-2 version (`0.29.1`) while the public channel advertised
  `0.29.0`; the pre-simulation record from the simulation was never consumed.
- Causes: an unelevated teardown only printed an elevated command (exit 2,
  easy to miss, and the runbook's teardown step did not mention it), and an
  elevated one edited the file under a running service, which writes its
  in-memory value back on every settings save.
- Fix (updates.md, "Testing a release without publishing one"): teardown
  stops `GreenCurveService`, writes and reads back, and starts it again; from
  a normal prompt it asks for elevation once and runs that as a child. New
  `tools/windows_service_control.py` ports the product's SCM wait protocol.
  Runbook step 5 rewritten (gitignored local file).
- Live check: one `teardown --restore-seen-only` from a normal prompt took
  one UAC prompt, stopped and restarted the running service, restored the
  recorded value and removed the record (exit 0). A full teardown including
  the GitHub steps has not run since the change.
- Follow-up the same day: that restart returned the GPU to stock (a clean
  service stop resets the intent it owns) and the new instance applied
  nothing, so the next install found no active settings to carry over and
  restored none. Teardown now carries the applied settings across its own
  restart exactly as setup does (`tools/windows_settings_transfer.py`:
  export before the stop, re-apply after the start; an export that fails
  leaves the service running). Fakes only; the carry-over has not run live.

## 2026-10-22: Stall rationing counts successful exchanges and both phases

- Review follow-up to F-LNX-STALL (linux-scaffold.md). The hold was decided
  only on a failed phase, from that phase's time: a request that completed
  just inside its read deadline earned one token and no hold (the loop was
  busy 100% of the time), and a write timeout after a slow read was held for
  the write alone (~49%).
- Fix: `gc_daemon_exchange_account()` (`linux_daemon_stall_policy.h`) settles
  every admitted exchange once; the outcome prices the tokens, read + write
  transport time decides the hold. Handler time is still excluded.
- Tests 7258-7270 (`tests/peer_stall_tests.cpp`), with the simulated peers now
  settled through the daemon's own function. Mutation check: with the old rule
  restored the suite fails at 7259 and the simulated shares are 1000 and
  494 per mille; with the fix all three shapes are 333.
- Not verified against a live daemon.

## 2026-10-22: `llm-wiki/private/` retired

- Reviewed in full before removal. The uncurated log copy is superseded by
  the public log: every dated entry is there, and only masked detail differs.
  The older write-ups of already fixed issues and the dated antivirus work
  log are superseded by the current topic pages. Machine and key-custody notes stay out of the repository, and
  operator steps moved into the gitignored runbook. Nothing that the tracked
  tree needs was lost.
- Fixed rather than recorded: Linux serve-loop stall rationing (F-LNX-STALL,
  linux-scaffold.md, 7240-7257), and a daily freshness-expiry reminder
  workflow (`tools/freshness_watch.py`, updates.md / build.md).
- Recorded as design limits on the topic pages: another account's redirected
  known folder (installer.md, service-install.md), per-account pipe leases
  (windows-architecture.md), unrationed Linux handler time
  (linux-scaffold.md), and no high-water mark of the envelope's issue time
  (updates.md). The elevated CLI log location was reviewed: logs go to the
  running account's own LocalAppData (debug-tools.md).
- Policy: there is no private wiki any more. Unfixed security findings go to
  a draft GitHub security advisory until fixed (`secret-leak-prevention.md`,
  `AGENTS.md`). The path stays gitignored, and `wiki_public_gates.py`
  unchanged, as a guard.
- Not verified: the reminder workflow has not run on GitHub (nothing pushed;
  the same check was run locally against the published release), and stall
  rationing was not exercised against a live daemon (simulated peer and
  mutation checks only).

## 2026-10-22: Driver-recovery flow under test

- Follow-up to the coverage review below. The fresh process's arming,
  queueing and single restore write moved from three shards into
  `driver_recovery_flow.h` (an `Ops` object per side effect), bound by
  `main_service_driver_recovery.cpp`. Behaviour is unchanged except the
  outcome log now says `proof=commit-failed-lockout` when the proof commit
  fails after a successful write (it latched before too, but the outcome line
  still claimed `preserved-mature`/`restarted`).
- `tests/driver_recovery_sequence_tests.cpp` (7210-7229) scripts the whole
  recovery against a simulated service with the real reducer and policies.
  A failed-write expectation first assumed result FAILED; the latch reduces
  LOCKOUT before the finish, so the service records LOCKED_OUT (unchanged).
  Seven deliberate flow breaks each fail a named code.
- `_check_driver_recovery_wiring()` pins the shard side file by file; ten
  deliberate breaks each fail a gate. Two existing gates moved with the code
  (`log_gates.py`). The unused reducer event `DRIVER_PROOF_SIGNAL` is gone;
  tests 429/594 now assert what the worker's re-attempt does.
- Not covered: the runtime bindings are text-pinned, not executed.

## 2026-10-22: Live driver-update restore; driver and build versions in the log

- Live test on x64 with a 0.30 dev build: the standard NVIDIA installer run
  twice (older over newer, then back), each about 16 minutes after the last
  Apply or restore. Both times a service thread faulted in `nvml.dll`, the VEH
  ended it, the controlled restart replayed the full intent after a real DXGI
  readiness signal (`proof=preserved-mature`), and the device restarted once
  per install, never again after the restore. That answers the OPEN item
  below for this installer: the replay's stamp was not challenged by a second
  restart, so no waiver change is made. The immature-proof waiver path stays
  unobserved live.
- The log could not say which driver a recovery crossed: versions were only
  printed by the immature-proof branch. The NvAPI loader now prints the mapped
  image's version and release (7189), the controlled-restart authorization the
  outgoing process's driver, and the recovery outcome the driver it restored
  under. Source gates in `tools/crash_artifacts.py`.
- Setup logs nothing on success, so an install was visible only as a changed
  session header. The service's first start of a new build now logs
  `version change: ... changed from <old> build <n> to <new> build <m>`
  against a one-line record in its admin-only data folder (7200-7206, after
  RUNNING; gate fails if the call is dropped, checked by removing it).
- Coverage review of the restore chain (see "Open questions" in
  [auto-restore-policy](../auto-restore-policy.md)): per-step policy tests
  exist, but the service wiring is held by source-text gates only, some of
  which cannot see a dropped call, and there is no sequence test. Follow-up.

## 2026-10-22: Prepare version 0.30

- VERSION is exactly `0.30`, without a trailing newline. Installation examples,
  Arch recipe versions, release notes and exact-version updater-restore fixtures
  agree. Compatibility and downloads sections follow the release-note format.
- Remote Arch recipe verification is pending published 0.30 inputs; locally
  generated Arch packages do not consume those remote inputs.
- Final normal and ASan/UBSan regressions, source gates, static-analysis ratchet
  (no new findings), release-note validation and 5000 runs per fuzz target pass.
  Plain `python build.py` produces the full matrix. Independent checks establish
  12 fresh installers/archives/packages, matching checksum sidecars, installer
  footer/entry CRCs and runtime bytes, archive content/modes and Arch metadata.
  Live driver-update, SCM and native ARM64 acceptance remain unverified.

## 2026-10-22: Path redaction retains command context explicitly

- Unix root entries, including first components containing spaces and names
  resembling switches, are redacted. Recognized unquoted command fragments
  preserve their switches and diagnostic fields; prose, arguments, quotes and
  separators end that context. Tests 7040/7049/7050 cover both contracts.
- Diagnostics are assessed at the formatter boundary: preserve existing exit
  and error fields, and avoid recursive logging from the redactor itself.
- Full UBSan regression and source gates pass. The focused production-header
  fixture fails against the previous header and passes with the current rules.

## 2026-10-22: Snapshot retirement binds to the authorized file object

- Recovery cleanup proves the snapshot's volume/file identity, content, length
  and timestamp independently of its nonce-bound authorization. Verification and
  deletion share one handle; delete sharing permits replacement by a newer
  service generation. Transient authorization format v4 carries file identity;
  saved profile and snapshot payload formats are unchanged.
- Native production-helper fixtures 7190-7195 cover replacement before and after
  snapshot open, including identical bytes and timestamps, input bounds and
  observable deletion failure. Helper diagnostics report snapshot retirement
  separately from authorization retirement. The full UBSan regression run passes;
  omitting file identity fails fixture 7191. Live SCM orchestration is unverified.

## 2026-10-22 (later): CI 7087 fix and review follow-ups for 0ac74a7..31b78d4

- CI run 39685286761 (Windows, UBSan step) stopped at 7087: the shell-link
  fixture compared the link target with an 8.3-spelled `%TEMP%` path by text
  (runners use `RUNNER~1`; the shell returns the long form). Now compared by
  file identity. Reproduced locally by pointing `TEMP`/`TMP` at an 8.3
  spelling; every later CI step had never run on Windows for these commits,
  so `--test`, `--test --asan`, `--tidy`, `--check --target all` and `--fuzz`
  were all rerun that way.
- Crash-dump notice: the elevated copy command now takes every path as a
  PowerShell single-quoted literal and is all or nothing (7148-7149; verified
  end to end in pwsh with `$()`, backtick, quote and bracket folder names).
  The same class existed in setup's icacls remedy (double-quoted install
  folder, `D:\Apps$Old` -> `D:\Apps`); both use `powershell_literal.h` now
  (5688-5689, AST-checked in pwsh).
- Driver-update waiver: the disk side compared the native ARM64 image's
  recorded version with `System32\nvapi64.dll` (x64 emulation copy). Loader
  and proof now share `nvapi_module_first()` (2189). The "in-process" version
  was a path read, so after an in-place replace both sides read the new file;
  it is read from the mapped image's resource directory now (7180-7188, hand
  walk, no new resource-API imports in the service). Helpers moved to the
  new shard `main_service_driver_version.cpp` (identity shard was at 800).
- From a second review pass: a fault/wedge inside a FIRST apply restarted
  plainly (no intent, no snapshot yet) and re-applied every logon; the plan
  now weighs the ownership marker (7163). The recovery helper's failure
  cleanup could delete a newer generation's snapshot; it is nonce-bound now
  (source gate only). Log redaction: `/delete`-style switches no longer
  swallow the rest of the line (7049); wide and narrow tokens agree (7048).
- OPEN (maintainer decision): an exempted driver-update replay records a
  fresh stamp with the NEW driver version, so a second device restart from
  the same install within 10 minutes latches UNSTABLE_APPLY. Whether driver
  installs restart the device twice is unconfirmed; a time-bounded waiver
  would need a stamp format change.
- None of the runtime paths is live-verified (no ARM64 hardware, no real
  driver update, no SCM race reproduced).

## 2026-10-22: The engineering log became public; private/ frozen

Policy decided by the maintainer: `llm-wiki/log/` (this file and its archives) is
tracked and published; `llm-wiki/private/` stays gitignored as a frozen
historic archive that nothing reads, writes or depends on.

- The whole log (four files, about 16.8k lines) was curated against the
  public-wiki rules: third-party tool names and analysis of other products,
  personal paths and the account name, key-custody and release-operator
  detail, incident narration, open security items, and pointers to untracked
  files were removed or replaced with "(details omitted)". All dated entry
  headings were kept. The uncurated original is not part of the repository.
- Residual product fingerprints were generalized too: a distinctive tail
  offset value (also in `gpu-backend.md`), a quoted button label and a
  daemon description.
- `tools/wiki_public_gates.py`: `private/` must stay ignored and untracked;
  `llm-wiki/log/recent.md` must be tracked and the log must never be
  ignored; log pages get the public-text checks (email, host, IPv4, tokens,
  account name) and every tracked file keeps the third-party denylist.
  Self-tests updated. `AGENTS.md`, `secret-leak-prevention.md`, `index.md`
  and six topic pages that pointed to "the local-only log" updated.
- Open: the release-operator runbook is still a gitignored file that
  releases depend on.

## 2026-10-21 (late): Remaining persistence items implemented; main at b6f1b1b

- 15c4919: UNSTABLE_APPLY / TDR_SPAM lockouts announced (GUI, TUI, Linux CLI); refused validated recovery drops its
  provisional intent (no stuck "GPU reconnecting"). Tests 7160-7162 (old 6752 TDR_SPAM-silent leg removed).
- ca21dc2: exact-device STARTED without REMOVED + applied intent -> driver recovery, except while a validated recovery
  write is pending. Tests 7135-7137.
- e89204f: proof stamp v4 records the NvAPI (System32) image version at apply; immature proof accepted when the on-disk
  version differs. v3 stamps fail closed until the next apply. Tests 7138-7139. README/policy table updated.
- b6f1b1b: SCM failure-action start (RESTART_ON_FAILURE) yields to a live, validated helper (STOPPED + ERROR_RETRY, no
  hardware, authorization kept); helper waits through an intermediate generation with a real PID. Tests 7170-7171;
  654/763 updated, 766 kept (pid 0 running state rejected).
- Item 6 (10-17 point-45 cold-logon failure): already fixed 10-17 (correction loop offset intent, tests 6532-6542); no
  recurrence in the log through 10-21. Left alone per maintainer caution about cold-boot behaviour.
- None of the new paths is live-verified (needs a real driver update / controlled restart). main fast-forwarded; 44 dist
  files copied to the main checkout (cmp + 12 sha256 OK).

## 2026-10-21 (later): Audit of the persistence promise, two follow-up fixes, dump notice; merged to main

- Field dump pid164: faulting thread = fan runtime (`service_fan_runtime_thread_proc` -> `apply_fan_curve_tick` ->
  `nvmlDeviceGetTemperature`); 10 VEH dumps since 09-11 at the same nvml.dll offset (driver installs 09-11/09-23/10-06/10-21).
  WER LocalDumps wrote `WerDumps\greencurve-service.exe.164.dmp` even with WER Disabled=1 (user re-enabled WER).
- 9fdb69c: VEH never ends ServiceMain/dispatcher threads (else START/STOP_PENDING hang); poisoned runtime with no snapshot and
  no live intent restarts plainly instead of latching AUTOMATIC_APPLY_FAILED. Tests 7107/7108, 7130-7134.
- de2af9a: `greencurve_service_crash_dumps.txt` notice in the user's folder (scan as SYSTEM, write under user token). Tests 7140-7147.
- main fast-forwarded to de2af9a; 44 fresh dist files copied to the main checkout's dist (cmp + sha256 verified).
- OPEN (need maintainer decision, verified in code):
  3. UNSTABLE_APPLY latched by ONE driver event within 10 min of any successful apply (incl. fan-only/app-profile applies);
     GUI announces only HANDBACK_INCOMPLETE/AUTOMATIC_APPLY_FAILED, so UNSTABLE_APPLY/TDR_SPAM are silent. Proposal: exempt a
     verified driver-version change, announce both reasons.
  5. Driver restart without REMOVED (driver update = STARTED only) is "diagnostic/readiness only"
     (main_service_logon_coordinator.cpp ~221); recovery depends on an NVML AV. No fan curve + GUI closed -> OC stays wiped until
     next logon. Proposal: STARTED + changed driver identity with intent -> request_service_restart.
  6. 10-17 10:48 cold logon: VF point 45 (excluded low region, requested 0) read +22 MHz -> verification failure -> rollback +
     AUTOMATIC_APPLY_FAILED; GPU at stock 41 min. Needs a tolerance rule for non-owned points.
  b. Controlled exit leaves STOP_PENDING; SCM failure action (2 s) races the helper's StartServiceW; helper normally wins (timing).
  4. Arm failure branch leaves REAPPLYING only if the reducer declines without a lockout (narrow).

## 2026-10-21: Driver update left profile 3 at stock -- VEH fast-fail under CET

- User report: NVIDIA driver update at 21:59:17, profile 3 applied 21:47:39 (proof mature, 11.6 min), not re-applied.
- Evidence: Application log 1000 at 21:59:19, greencurve-service.exe 0.29.0.329, 0xC0000409 in ntdll+0x124446; SCM 7031 restart;
  next instance `ownership handback: previous instance left GPU state it owned; verdict=run scope=full` -> reset to stock, no replay.
  Another process's monitoring code AV'd in nvmlDeviceGetPowerUsage in the same second. No service log line between 21:59:18.4 and the restart.
- Root cause (verified by disassembly of ntdll 10.0.26100.9711 via cdb -z on the image): +0x124446 is `int 29h` with ecx=0x30
  (FAST_FAIL_SET_CONTEXT_DENIED) in RtlRestoreContext right after NtContinue returned 0xC000060A; KiUserExceptionDispatcher continues a
  handled exception through RtlGuardRestoreContext. Our VEH rewrote Rip to kernel32!ExitThread -> refused under the /cetcompat strict
  shadow stacks (in place since 0.25.0, 34bd64b). Explains the open 10-06 0.28 fast-fail during a driver reinstall too.
- Fix: direct ExitThread from inside the handler (RtlpCallVectoredHandlers releases its SRW lock before the call; verified by disassembly).
  veh_driver_fault_win32.h + tests 7100-7129 incl. CET-forced child and legacy negative control; mutation (old rewrite) fails 7120.
- Not verified: live driver update with the fix; which thread faulted (WER dump in the admin-only WerDumps folder not read).

## 2026-10-21: Compatible diagnostics, loader and desktop hardening

- Five implementation commits through 7bebd89 plus a documentation-only clarification at bede194; no push. Preserve chosen installation locations, GPU support, service protocol and update settings.
- Identity-bound service log routes retain shared user logs; native mutation, link, route-reuse and rotation tests pass. Static import restrictions validated by a harmless sibling-DLL baseline and applied/gated on all Windows images. Persisted path redaction retains console/export contracts. Desktop writes bind to pinned directories; Linux deletion uses account privileges.
- Final plain build, regression/ASan/gates/tidy passed. Independent artifact verification confirms 12 fresh outputs, 16 Windows images, installer CRC/runtime bytes, archive/package bytes and Linux modes. Both x64 GUI variants pass startup/help. Native ARM and real endpoint upgrades were not run.
- Full-range secret scan passed; tracked tree clean; no fixture processes remain.

## 2026-10-19: Official 0.29.0 release published and verified

- CI run `39247438156` passed on commit `0ac74a70293d36f44bf46dc154bf2fb4c3167437` across all matrix jobs (`secrets`, `release-packaging`, `linux`, `windows`), validating the elevated directory claim test owner fix, the 16 MB ASan stack reserve, and shallow clone handling in release simulation tests.
- Release workflow `release.yml` dispatched on `main` (run ID `39249109401`); built all Windows + Linux (x64 and arm64) targets, verified clean tree, attested artifacts to exact commit `0ac74a70293d36f44bf46dc154bf2fb4c3167437`, created signed tag and release `0.29.0` with 17 initial assets, and posted `success` under commit status `Provenance & Attestation`.
- Updater assets published (floorless v1 + v2 freshness manifests, 4 assets, 21 release assets total) and verified anonymously via `releases/latest/download` (operator steps omitted).
- Anonymous verification confirmed valid P-256 signatures over network bytes against `GC_UPDATE_PUBLIC_KEY_ACTIVE`, matching installer payload sizes and hashes, allowlisted redirect hosts, and release notes matching `CHANGELOG.md`.

## 2026-10-19: Bump version to 0.29.0 for QA and 2-hop release simulation

- Bumped `VERSION` to `0.29.0` (maintaining no-trailing-newline invariant).
- Synchronized `README.md` Windows and Arch package installation examples to `0.29.0`.
- Updated `packaging/arch/PKGBUILD` and `packaging/arch/PKGBUILD.bin` `pkgver` to `0.29.0`.
- Converted `CHANGELOG.md` `## Unreleased` section into `## 0.29.0` with full `Compatibility notes`, `Downloads and verification`, and compare link `[0.28...0.29.0]`.
- Added update restore decision regression coverage for `0.29.0` (assertions 5941-5943).
- Updated `llm-wiki/index.md` version mapping table.

## 2026-10-19: Fix CI test failures (elevated claim owner, ASan stack size, shallow clone tag handling)

- Elevated directory claim owner test: in an elevated process (e.g. CI Windows runner), creating an object with `requireAdminOwner=false` assigns `BUILTIN\Administrators` by default. When the second pass checked `requireAdminOwner=true`, the directory was already owned by `Administrators`, causing adoption instead of replacement (assertion 6617). Added `claim_test_set_user_owner()` to explicitly assign `TokenUser` before the required-admin claim so replacement semantics are tested regardless of harness elevation.
- Windows AddressSanitizer stack size: ASan instrumentation and stack redzones across the pure regression suite exceeded the default 1 MB PE stack size, causing an ASan stack overflow (`STATUS_STACK_OVERFLOW`). Linked the Windows test binary with `-Wl,--stack,16777216` (16 MB reserve).
- Release simulation tag discovery: `simulate_release_tests.py` now uses an isolated temporary git repository fixture to test tag filtering and numerical sorting, eliminating dependency on clone depth, and handles shallow repository checkouts gracefully.
- CI configuration: added `fetch-depth: 0` to Windows and Linux checkout steps in `.github/workflows/ci.yml`.

## 2026-10-17: Implemented both commit-review fixes

- 540e2d4: Linux curve rebuilds retain the exact write mask (including cleanup) and populated voltage topology. Equal-count replacement regression failed at 6892 before the fix; frequency-only drift remains allowed.
- 9a0662b: retain per-point NVML-shift evidence for the current GPU session, exact scalar and voltage topology. Fresh scalar/VF preflight refuses unproven no-reset composition before writes; post-write evidence failure recovers. GPU selection/teardown/enumeration and real scalar writes invalidate the cache. Production-header tests 6900-6923; disabling retained shift reuse fails at 6903.
- 40865bf: clear the two unbaselined clang-tidy warnings from the earlier changes (pointer-sized launch-name indexing and explicit bank in/out-handle validation); native null-argument tests 6925-6926.
- Final python build.py --test and --tidy passed; plain python build.py produced all six targets. Independently verified fresh timestamps, all four installer payload CRCs and runtime bytes, archive contents/modes, and both Arch packages.
- Gitleaks 8.30.1 staged and exact-created-commit checks passed for all three commits. No push. No GPU writes or live installation; Pascal..Ada hardware replay remains outstanding.

## 2026-10-17: Review of the day's commits; profile-switch pre-tail regression fixed

- User report: on the RTX 5070, switching from profile 1 (typed points 74/75,
  flatten lock 2962) to profiles 3/4 (+475 exclude 70, lock 2957) was always
  refused: `apply lock precheck: REFUSED ... preTailCi=73 target=2987
  stockBase=2512000 ... requested=1 fromOffset=1`. Profile 3 saved point 73
  over a 2397 MHz base; the reset at 16:09:53 read stock point 75 at 2460 MHz.
  Introduced by 1f60e00 (provenance pre-tail points judged over the live base),
  kept for Blackwell by 0dd7787. Fixed: AGREEMENT mode for pre-reset bases.
  Mechanism of the inflated live readback is inferred, not verified. Needs a
  hardware re-run of the 1 -> 3 switch.
- Same session: failure-record clear order, Linux rebuild shape check, shared
  lockout observer (TUI reconnect), fan-log race, verbose config cache,
  required batch-offset parameter, lossless seen-version decode, `--dump`
  (always failed with error 5: target was the open CLI log), NVML/VF
  composition evidence log.
- Pre-Blackwell provenance points: Option 1 (VF carries the whole offset) was
  rejected after reading the detection/display/persistence paths (new carrier
  state, sparse/CLI no-reset stacking risk). Implemented 1b instead: component
  minus the per-point shift measured across the apply's own NVML write, bounded
  to [0, written]; full component when not measurable. Needs a Pascal..Ada log
  (`nvml shift:` line) to confirm on hardware.

## 2026-10-17: Pre-Blackwell selective offset state retention, NVML readback preservation, and uniform/selective routing

- Gated selective GPU offset detection/retention (`main_gpu_state.cpp`) on VF read/write capability (`vf_curve_selective_gpu_offset_supported()` / `vf_backend_supports_selective_gpu_offset()`) instead of `vf_curve_global_gpu_offset_supported()`, preventing Pascal..Ada GPUs from wiping `appliedGpuOffsetMHz` and `appliedGpuOffsetExcludeLowCount` on refresh after applying an exclude-low offset.
- Updated `detect_clock_offsets()` (`gpu_backend.cpp`) via `resolve_detected_scalar_gpu_offset_khz()` and `gpu_offset_readback_after_detection(..., nvmlRead)` (`control_readback_policy.h`) so a fresh non-Blackwell NVML graphics-offset reading is preserved when `nvapi_read_pstates()` reports `0` or is unavailable.
- Added `curve_point_request_uses_vf_table_offset()` and `curve_point_plan_gpu_offset_routing()` (`curve_point_offset_policy.h`) and wired them into `gpu_backend_apply.cpp`, `gpu_backend_apply_lock_pretail.h`, and `gpu_backend_apply_targets.h`:
  - Pre-Blackwell selective (`excludeLowCount > 0`) and zero-offset requests use VF-table live-base projection in pre-tail, flat-tail, and stored-lock anchor checks; only the non-Blackwell uniform NVML route retains stored-MHz checks.
  - Pre-Blackwell uniform->selective transitions without `resetOcBeforeApply` clear the prior scalar NVML offset (`nvapi_set_gpu_offset(0)`) before the VF curve batch; selective->uniform transitions write the NVML scalar offset and strip the prior selective VF curve offsets (`curveBatchGpuOffsetMHz = 0`).
  - `lastApplyUsedGpuOffset` remains `true` whenever a request owns or preserves a non-zero uniform or selective GPU offset across curve/lock applies.
- Added unit tests 1752-1760 (`tests/regression_main.cpp`) and 6852-6857 (`tests/offset_intent_tests.cpp`) plus source gates in `tools/readback_gates.py` and `tools/apply_ceiling_gates.py`.

## 2026-10-17: Defer unmeasured Windows NVML/VF compensation

- No Pascal..Ada hardware is available to confirm independent additivity or
  readback composition. Restore full component targets and remove scalar
  subtraction from pre-tail/anchor bases. Keep requested-point lock projection
  on the VF-table route; the separate NVML route keeps stored MHz.
- Retain the cold-GPU correction predicate, strict verifier, transition guards
  and recovery. Replace the additive fake driver with software-contract tests.
- Remove the definitive doubled-offset release claim; scope the two lock
  projection release notes to RTX 50. Hardware validation remains unavailable.

## 2026-10-17: Merged the three release-readiness branches into main

- 7dad7ad security hardening (claim-or-create machine dirs, private setup launch dir, simulate_release key/guards),
  2fd25b6 diagnostics volume + failed-auto-apply notice (protocol v29), 79993d2 offset-intent follow-ups
  (pre-Blackwell scalar, pre-tail provenance, boost tally, anchor, Linux stale scalar).
- Offset branch codes renumbered 6570-6604 -> 6800-6834 (collided with WER 6570-6582 and hardening 6600+);
  apply_correction_tests' internal runner renamed run_correction_offset_intent_tests.
- Final tree: python build.py, --test, --gates green; offset suite proven to run (forced 6800 failure).
- Still unverified on hardware/elevated: all three branches. Not pushed.

## 2026-10-17: Release-readiness review for the version after 0.28

- All build modes green: --test, --test --asan, --tidy (no new; 31 stale baseline lines), --fuzz, --check-cet,
  --target linux --check, --verify-toolchain, --gates. CI green on origin/main (36608a3).
- Fixed+committed: correction-loop offset intent (9ec84b5, c3069c6), diagnostics/SHORTFALL predicate + test 6520
  (73c43cf), Arch source PKGBUILD cd into green-curve-$pkgver (98aa9fc; verified against the published archive).
- Open (apply): non-Blackwell flagged (fromGpuOffset) points get component per point ON TOP of the NVML global
  offset (targets.h writes component unconditionally; nvapi_set_gpu_offset uses NVML global off Blackwell) ->
  double offset, verification blind. Pre-tail policy (apply_lock_pretail_policy.h) and monotonic check judge
  flagged points by stored projected MHz. Boost counters computed before correction. Interactive stored-lock
  anchor from projected point. Unverified on hardware (maintainer has Blackwell only).
- Open (diagnostics): ~97% of log bytes are 8 per-tick lines (telemetry read, service envelope + GUI state with
  revision in the on_change key, xbar refresh, fan worker pulse lines) -> 32 MiB cap holds ~2 days; a 10-06
  service 0xc0000409 fast-fail during an NVIDIA driver reinstall (no dump, log rotated out) is unexplained.
  Logon handoff/parse-error paths overwrite greencurve_cli_log.txt (write_text_file_atomic) instead of appending.
  A failed automatic logon apply is silent in the GUI (only HANDBACK_INCOMPLETE is announced).
- Open (tooling/docs): edbdcc0 carried functional PE-gate/shim/pe_layout changes under a docs title (verified safe
  for release assets); xbar comment now claims driver behaviour that was not measured; public-wiki gate and
  documentation follow-ups (details omitted);
  .gitignore no longer ignores CLAUDE.md. Stale local test tag 0.29 -> release_post commit mismatch.

## 2026-10-17: Cold-boot logon apply failed; correction loop vs verification provenance

- Boot 10:48: logon handoff accepted, service applied slot 3 (+475/excl70, FLATTEN 2957), correction loop ran 6
  passes, failed `VF point 45 offset verified at 22000 kHz above requested 0 kHz`, rolled back to stock, latched
  auto-restore lockout (durable). Same profile succeeded 10-15 and 10-16. GPU at 25 C.
- Root cause: correction plan used `desired->curvePointFromGpuOffset` (0 for selective-synthesized points 0..69) so it
  chased absolute previews after the low base shifted one VF bin; verification judges those points by offset.
- Fix: shared `curve_point_offset_is_intent()`; correction carries `targetOffsetKHz = targetCurveOffsets`; classify
  judges offset-intent points by programmed offset. Tests 6532-6542, mutation-checked (6532). Gates updated.
- Not hardware-reproduced. User must press Apply once to clear the lockout latched by this boot.
- See clock-transition-audit.md "2026-10-17 correction loop chased synthesized selective points".

## 2026-10-08: Added Support and Other projects sections to README.md

- Added Support section with GitHub Sponsors and Buy Me a Coffee badges.
- Added Other projects section listing capture-engine, testsmem4u, Shader-Stress, and obs-indicator, mirroring capture-engine's README style.

## 2026-10-08: Removed CLAUDE.md and adopted upstream AGENTS.md baseline updates

- Removed redundant `CLAUDE.md` symlink; `AGENTS.md` suffices as the single public instruction file.
- Updated `tools/wiki_public_gates.py`, `tools/security_gates.py`, `.gitignore`, `llm-wiki/index.md`, `llm-wiki/secret-leak-prevention.md`, `llm-wiki/changelog-style.md`, and `llm-wiki/testing.md` to remove `CLAUDE.md` requirements and checks, while keeping the regular-file tracking requirement on `AGENTS.md`.
- Backported latest improvements from upstream `https://github.com/aufkrawall/llm-prompt-templates`:
  - Always-on rule placement principle atop `AGENTS.md`.
  - Toolchain/environment discipline (prohibits silent tool substitution, mandates stop-and-report).
  - Concrete verification hierarchy ladder beyond exit codes.
  - Reviewable commit granularity (concise title, bulleted body, small self-contained commits).
  - Economical diagnostics/logging guidelines (token-efficient formatting, rate-limited repeats, capped collections).
  - Test apps and computer use lifecycle (bounded runs, full process tree shutdown/verification, dependency readiness signals).
- Verified with `python build.py --test` (all test gates and self-tests pass).

## 2026-10-06: AGENTS.md, CLAUDE.md and the wiki topic pages became tracked (public)

Policy decided by the maintainer: `AGENTS.md`, `CLAUDE.md` (a symlink to AGENTS.md, tracked as mode 120000) and
`llm-wiki/*.md` are tracked and published; `llm-wiki/log/` (this file) and the new `llm-wiki/private/` stay local.

- Every topic page was read in full. Incident/remediation narration, open security items, key-custody and
  release-operator detail, machine-specific notes and tone issues were removed from the public pages
  (details omitted).
- New gate module `tools/wiki_public_gates.py` (wired through `security_gates.check_public_wiki`, one shared
  tracked-file listing in `build.py`): fails if anything under `llm-wiki/log/` or `llm-wiki/private/` is tracked,
  if `.gitignore` loses those two rules or re-ignores a public page, if `CLAUDE.md` diverges from `AGENTS.md`,
  or if a public page contains an email, `DESKTOP-`/`LAPTOP-` host, non-documentation IPv4, secret-shaped token
  or the developer's own OS account name (derived at run time, skipped on CI). Self-tested in `--test`
  (`wiki_public_gates self-tests passed`); `--gates` runs the real-tree check.
- `check_no_developer_profile_paths` already scanned every tracked text file, so it now covers the wiki; the old
  `secret-leak-prevention.md` manual-fallback regex (a drive-letter users-folder pattern) would have tripped it
  and was reworded.
- Note: plain `python build.py` does NOT run the source gates; `--test` and `--gates` do (CI runs `--test`, the
  release job runs `--gates`).
- `git log --all -- llm-wiki AGENTS.md CLAUDE.md` is empty: these files were never tracked before this change.

## 2026-10-05: Released 0.28 and verified updater delivery to 0.27 and older

Published stable `0.28` end to end, with the
integrated-updater delivery guarantee verified against the live artifacts.

- Release-prep commits (all pushed to `main`, no tags pushed): `6a43892`
  (`Add 0.28 release notes`), `3ac4bf1` (`Prepare 0.28 version`: VERSION
  without trailing newline, README install examples, both Arch `pkgver`
  values -- `tools/linux_gates.py` pins those to `APP_VERSION`), `358793a`
  (0.28 release-manifest and update-restore regression fixtures; this is the
  release commit). Post-release follow-up `baad3e1` repinned the Arch recipe
  digests from the published0.28 bytes (the prep commit flagged them as
  pending; the tag-archive digest cannot exist before the tag does).
- The release version is spelled `0.28` (short form, per the maintainer's
  request). New fixtures pin `0.28` == `0.28.0` ordering equality both for the
  offer decision and the settings-handoff restore decision, AVAILABLE for
  every public installed version incl. the `0.27` no-patch spelling on both
  architectures with no floor, REJECTED above the release, and the literal
  asset-name spelling `release.yml` produces. Codes 5923-5938 in
  `tests/regression_main.cpp`.
- Gates: local `python build.py --test` green; `python tools/release_prep.py`
  and `--preflight` green (notes structure; key preflight details omitted); CI `37356433779` green
  at `358793a`; secret-leak checks around every commit (local gitleaks 8.30.1
  `protect --staged` + `detect --log-opts` over the outgoing range, no leaks;
  note `tools/secret_scan.py` downloads a Linux gitleaks binary and cannot run
  on Windows -- use local gitleaks per `secret-leak-prevention.md`).
- Release run `37358341063` green: tag `0.28` (SSH-signed, `Verified` badge)
  and non-draft/non-prerelease release at `358793a`, 17 program assets,
  `Provenance & Attestation` commit status `success`.
- Updater publication (first live use of `tools/release_post.py`): both
  installers' checksums and release-workflow provenance verified at
  `358793a`, all FOUR updater assets published (21 assets total) and
  anonymous delivery verified (operator steps omitted). Manifest:
  `format=1`, `version=0.28`, floorless. v2 freshness envelope issued
  `2026-10-05T18:53:55Z`, expires `2026-11-04T18:53:55Z`. Policy harness
  against the actual fetched
  manifest: AVAILABLE for 0.23..0.27.0 + `0.26`/`0.27` spellings on both
  arches, UP_TO_DATE for 0.28/0.28.0, REJECTED for 0.29. Updater policy
  headers re-verified byte-identical 0.23..HEAD, so the harness answer is the
  installed client's. `gh attestation verify` passed for the published x64
  setup.
- Stale-tag hazard realized: the local test-cycle tag `0.28` (-> `3225bad`)
  shadows the new release tag, so tag-resolution-based tool runs for this
  release fail closed on commit mismatch. Used `--source-commit <full SHA>`
  (the tool verifies GitHub's tag resolves to it). Stale-tag clean-up: operator steps omitted.
- Not rerun for this release (checklist items recorded as risk): the
  disposable-elevated upgrade/move/uninstall pass (the earlier 2-hop simulation
  live-verified upgrade + settings handoff + service lifecycle against the
  test clone), live post-stop rollback fault injection, and GPU-hardware
  power-only Apply / crash handback (no GPU-clock changes since 0.27.0).

## 2026-10-05: Implement 2-hop release simulation on clone repository

Formalized and automated the end-to-end in-app updater release simulation procedure on a throwaway clone repository (`aufkrawall/green-curve-update-test`):

- Added `tools/simulate_release.py`:
  - `init`: validates gh auth and signing preconditions (details omitted), and ensures test repository visibility is public for anonymous WinHTTP fetches.
  - `build-baseline`: builds setup from released stable tag (e.g. `0.27.0`) in an isolated detached git worktree with repointed repo constants, for manual installation by the operator.
  - `publish-hop1` / `publish-hop2`: build the candidate setup from HEAD and a version-bumped follow-up setup in isolated worktrees and publish them to the test repo, so the *new* updater implementation is also proven not to have regressed (operator steps omitted).
  - `teardown`: removes simulation releases and tags from test repo, and reverts visibility to private.
- Safety invariants:
  - Repointed repository constants are modified strictly inside detached git worktrees (`git worktree add --detach`) with NTFS junctions for compilers (`llvm-mingw`, `seven-zip`), keeping the main working tree pristine.
- Testing and runbook documentation:
  - Added unit test suite `tools/simulate_release_tests.py` covering repo parsing, header patching, SHA-256 generation, and CLI subcommands; wired into `release_post.run_self_tests()` and verified via `python build.py --gates`.
  - Updated `llm-wiki/updates.md` under "Testing a release without publishing one".
- Live verification run completed:
  - Executed full 2-hop simulation cycle against `aufkrawall/green-curve-update-test` (baseline `0.27.0` -> candidate `0.28.0` -> follow-up `0.28.1`).
  - Confirmed signature and freshness envelope verification, binary installation, service lifecycle, settings handoff, and hardware state preservation across both hops.
  - Teardown completed: purged test releases/tags and reverted test repository visibility back to private.

## 2026-10-02: Fix Windows tidy ratchet and Linux test compilation CI job failures

Resolved the 2 CI job failures reported in GitHub Actions run 36899383344:

- Windows static analysis ratchet (`python build.py --tidy`): failed on two
  unbaselined findings in `source/linux_daemon_state.cpp` (lines 153 and 165)
  for `bugprone-assignment-in-if-condition`. Root cause was the inline
  `if ((count = gc_read_record(...)) == ...)` introduced in commit `96cbca6`.
  Separated `count = gc_read_record(...)` before the conditional evaluation,
  preserving identical error propagation and semantics without assignment inside
  the branch condition.
- Linux CI test job (`python build.py --test`): compilation of
  `tests/regression_main.cpp` failed with 8 undeclared identifier errors for
  `StringCchCopyA`. Root cause was `StringCchCopyA` used in IPC log-safety tests
  (5958-5965) without a Linux definition in `source/win32_compat.h`.
  Provided complete `HRESULT`, `S_OK`, `S_FALSE`, `SUCCEEDED`, `FAILED`,
  `STRSAFE_E_INSUFFICIENT_BUFFER`, `STRSAFE_E_INVALID_PARAMETER`, and an
  in-line conforming `StringCchCopyA` implementation in `source/win32_compat.h`.
- Hardened cross-platform regression test coverage:
  - Added unit test suite `string_copy_shim_tests` (6270-6277) in
    `tests/review_followup_tests.cpp` to verify `StringCchCopyA` across success,
    truncation, exact fit, zero capacity, and SUCCEEDED/FAILED evaluation.
  - Added regression test harness cross-linking for `x86_64-linux-gnu` via Zig
    in `tools/security_gates.py` (`run_linux_fixtures`) when running on non-Linux
    hosts. Any future Linux build regression in `regression_main.cpp` or shared
    headers is caught on the Windows host during `--test` before commit/push.

## 2026-10-01: Review the nine commits after the 0.27.0 release

Reviewed `0.27.0..5f6a3da`, including the previous review's fixes. Baseline
regressions passed; five remaining release/packaging issues were established:

- Both Arch recipes retained the old resume-unit checksum after sandboxing,
  so source verification failed. Pins now match the actual unit; portable gates
  hash every local recipe asset and reject missing/unpinned remote checksums.
- Source PKGBUILD's new guard refused its own SKIP placeholder. Replaced it
  with the published 0.27.0 archive digest after comparing its 505 regular files
  with the reviewed tag's `git archive` (`2acdcad`). No remote code was executed.
- Binary PKGBUILD pins disagreed with BOTH published Linux archives. Fetched
  bytes match their release sidecars and release-workflow provenance for the
  reviewed commit; recipe pins are corrected. Native WSL Arch makepkg source
  verification now passes for source and both binary architectures. No install
  ran. Remote digests use this reproducible check rather than a networked unit.
- Initial updater publication targeted old stable releases, signed and uploaded
  before latest-delivery checks failed (dry-run missed this). Latest-release
  guards now run before download and directly before signing/upload, including
  renewal. Concurrent remote publication is still not atomically excluded.
- Signer version input accepted leading zeros/oversized components that native
  clients reject; renewal validated only marker text. Shared Python version and
  manifest validation mirrors native bounds/keys/asset binding, rejects malformed
  input before key reads, and preserves exact signed bytes. Generic raw sign and
  verify helpers retain their contracts.

Regression evidence: new signing-input units failed against old code (8 failures,
1 error); new publication checks failed three cases; both original recipes failed
the new input gate. Focused suites pass after fixes. Tests use only mocked keys,
network, provenance and publication. The documentation review corrected stale
installer-mitigation and Arch filename claims. No native GPU contract, unsupported
GPU defaults, global setting, real private key, running service or release changed.
Validation completed: `python build.py --test` (UBSan), `--test --asan`,
`--gates`, and final full `python build.py` Windows MSVC/release x64/ARM64 plus
Linux x64/ARM64 build/package matrix. Actual resume-unit bytes/mode and native
makepkg verification passed. Wiki quality checked 21 existing unique routes,
changed-page links and current source anchors, plus semantic stale-claim review.
Diagnostics are tool refusal reasons and gate summaries; no native runtime
logging change was needed for these tooling/input corrections.

Local commit: `29ba3d9fba6d4c5839e2a751c4c64d3108309a11` (policy-only message:
"Validate release inputs and pin verified Arch sources"). Complete staged
inventory/patch and planned message were reviewed; gitleaks 8.30.1 staged scan
passed. Exact created patch byte-matched the reviewed staged patch; public
author/committer metadata and full commit were inspected. Latest-commit and
full-history (307 commits) gitleaks scans passed. Working tree is clean; nothing
was pushed, signed with a real key, or published.

> Historical note: this first implementation was corrected by the follow-up above. Refusals no longer read a frame or cost tokens; admission now refills; identity is UID only and unknown credentials fail closed.

## 2026-10-01: Code review of the post-0.27.0 commits (e57b098..96cbca6)

Local commit `5f6a3da` (staged + post-commit gitleaks clean; not pushed).
Reviewed all eight commits since the 0.27.0 tag. Most changes held up; these
did not, and were fixed (details on the topic pages):

| Finding | Fix |
|---|---|
| Installer refused any existing `.gcnew`/`.gcrestore`, so a setup interrupted between create and rename wedged every later upgrade AND rollback | Discard the stale name once through a no-follow DELETE handle, retry the exclusive create once (installer.md) |
| Shared-bank startup proof gated `resolve_machine_config_path()` itself and latched for the service lifetime: one failed proof also disabled the updater cache/staging dir and ignored a later admin rewrite until restart | Gate content readers only, re-prove on the next content read under the config lock; admin writers hold that lock across write+harden (config-profiles.md) |
| Linux `write_text_file_atomic` rejected bare file names, refused symlinked config dirs for ordinary users, and retried I/O errors 16 times | Pure path policy + root-only dir `O_NOFOLLOW` + collision-only retries + dir fsync (linux-scaffold.md) |
| Linux client reported a daemon busy refusal as a transport fault when the daemon's close beat the request write | Read the queued refusal after EPIPE/ECONNRESET/EIO (linux-scaffold.md) |
| 30-day v2 freshness had no renewal tooling (manual ritual, `gh release upload` without `--clobber` fails) and every refusal told users to check their clock | `release_post.py --renew-freshness` + `update_signing.py renew-freshness`; `GcUpdateFreshStatus` separates expired vs clock (updates.md) |
| Staging dir verified its DACL but not its owner | Prove Administrators owner via a no-follow handle |
| Stale ACCESS_DENIED pipe text ("another user is the active controller") contradicted the AU pipe ACL | Reworded |

Reviewed and left as is: the per-account two-lease pipe cap (a single GUI
uses at most coordinator + update worker concurrently; a third same-account
connection -- e.g. a polling CLI script during an Apply -- is disconnected
before the header and may surface as "outcome unknown". Recorded as an open
question rather than changed without evidence). Verified: `--gates`, `--test`,
full `build.py`; Linux transport fixture and an atomic-writer probe executed
under WSL Arch (user + root). No live elevated install, service, or release run.

## 2026-10-01: Verify follow-up audit and close trust-boundary gaps

Local commit: `96cbca674dba31be6788b4ef88c45bec88607ae2`. The exact created
patch (including binary fixtures) matched the reviewed staged patch; metadata
was inspected, staged/history/latest-commit gitleaks scans passed, and the tracked
worktree is clean. Nothing was pushed or published.

Verified the supplied audit against the original code and targeted native/pure
fixtures; the recommendations below were implemented rather than accepting the
report's severity labels or its blanket shipping verdict. No release was published.

| Finding | Verified disposition |
|---|---|
| F11-001 | Silent peers could occupy all six Windows workers. Account-bound pre-read leases now allow two per account; actual six-silent-pipe regression plus independent-account policy fixture. Process SID is availability only; captured impersonation token still authorizes. Global anonymous throttling at accept was rejected because it would deny honest accounts too. |
| F07-001 | Genuine old signatures had no freshness boundary. Separate signed v2 envelope expires after at most 30 days; no v1 fallback for new clients. Release tooling emits both formats and maintainers must renew v2 metadata. Replay within its lifetime and compromised clocks remain possible. |
| F02-001 | ACL reclamation could preserve planted shared-bank content. Startup pins parent and a singly linked file, proves prior exact config DACL/admin owner or truncates and flushes before reclaim. Failure disables service machine-bank resolution. |
| F04-001 | Archive fuzzer consumed its steering/footer bytes and could read beyond short zero-count data. Independent regions and bounds now exercised through the real harness under ASAN. |
| F03-001 | Reissued minimum-version policy did not stop staged installation. AVAILABLE and freshness now gate recovery, UI, install and final launch without changing the frozen wire refusal enum. |
| F08-001 | Existing staged filenames could be followed/replaced. Exclusive no-reparse creation, pinned source, bounded copy, flush and owned-file-only cleanup; native collision regression. |
| F08-002 | Interrupted/short Linux reads could delete valid state. Shared read helper retries EINTR/assembles fragments; real I/O errors preserve protected records. |
| F12-001 | Resume unit lacked daemon sandbox. Generated and packaged units share tested restrictions; resume intentionally owns no runtime directory. |
| F11-002 | Refused Linux peers could stall the single-threaded daemon. Refusal sends once nonblocking before any request read, with zero token charge. Missing credentials fail closed; quota keyed by UID. |
| F10-001 | History secret CI and release test gates were missing. Pinned checksum-verified read-only scanner covers full history; release separately runs regression and fuzz before build. |
| F07-002 | Rare ECDSA zero-r/s retry restarted RFC6979. Nonce generator advances its HMAC state; forced-retry fixture verifies different next candidate and signature. |
| F12-002 | x64 release CET metadata, fortify level and stack-clash protection improved and artifact-checked. Pinned Zig ARM64 CFG passes indirect target in x15, incompatible with existing x0 shim: deferred pending ABI-compatible runtime. ELF property notes likewise remain toolchain-limited. Existing MSVC ARM64 CFG retained. |
| F02-002 | ACL diagnostics missed named principals/delete-child/null DACL. Conservative all-non-admin evaluation and native fixtures added. |
| F03-002 | Group membership persists GPU-admin boot trust. Intended functionality retained and explicitly documented, including revocation not clearing saved boot policy. |

Additional recommendations implemented: reserved console/superscript device names,
locked updater decision capture, strict signed installer basename, broader private
path gates, narrow corpus scanner scope, release credential EXIT cleanup, persisted
message termination, crash breadcrumb O_NOFOLLOW, desktop Exec metacharacter escaping,
and corrected stale comments. Primary-spec verification additionally corrected
the native AND Bash desktop Exec second escaping layer; C++ regressions and an
executing Bash assignment fixture cover this (the report missed the native gap).
Review also found collision-chain idle eviction could
reset a later live identity's quota: acquire now searches for the resident first and
has a deterministic regression. Refills occur at admission; insufficient outcome
charges drain tokens rather than leaving a residual budget.

Retained limitations: unsigned setup executable is protected by the authenticated
updater's digest/signature but lacks publisher Authenticode; power-loss atomicity
and first-install/bootstrap trust need broader product/tooling contracts (details
omitted). Unsupported GPU read/write defaults
remain unchanged. No new sleeps, timing workarounds, or protocol-version change.

Validation: full Windows (MSVC/release x64/ARM64) and Linux x64/ARM64 build/package
matrix; ASAN regression; eight fuzz targets with 5,000 runs each; all-target syntax
checks; x64 CET audit; Bash syntax; staged/commit secret gates. Linux runtime fixtures
are cross-built here and execute in Linux CI; no systemd, elevated SYSTEM, ARM64 or
GPU hardware replay on this host. See current topic pages for source anchors.

## 2026-10-01: Meter the Linux daemon's peers with the shared admission policy

Closes the last named abuse-control gap from the security audit. The Linux
serve loop had no rate limit or connection cap: one connection is
accepted, read, handled to completion and answered before the next `accept()`,
with a backlog of 8, so one busy peer could starve every other client.

- `source/linux_daemon_serve.h` now runs the SAME pure policy the Windows pipe
  transport already ships: `service_ipc_throttle_policy.h`, which is
  host-neutral by construction (a monotonic millisecond clock in, a decision
  out, no OS types, no allocation). This is a **port of an existing tested
  control, not a second design** — the same reason `linux_daemon.cpp` now
  includes that header.
- Metered **before** the request is read, so a flood spends its own budget
  rather than the shared backlog. Charged **after** the outcome is known, with
  the same weighting the Windows transport uses: success costs 1, a
  rate-refusal costs 5, and a peer that connected without delivering a frame
  costs 10. A stall-and-abandon flood is therefore more expensive than real
  work, and a refused peer is charged 5 — which is also what lets its bucket
  climb back (see below).
- A refused peer is **answered** with a `busy` status rather than dropped. A
  dropped connection looks like a transport fault and is retried immediately,
  which is precisely the behaviour being bounded; an answer lets a client back
  off.
- Identity is the peer **uid and primary group** from the `ucred`
  `log_peer()` already reads, deliberately NOT a bare pid — a client that forks
  would otherwise get a fresh, unmetered bucket on every request. An
  unidentifiable peer is admitted unmetered, the same trade the Windows
  transport makes; the request is still clamped and still serialized.

**A subtle property of the shared policy, recorded because the regression
test had to discover it.** The token refill is materialised by
`service_ipc_charge()`, not by `service_ipc_decide_admission()`: decide reads
`tokensMilli` without refilling, and a zero-cost refusal charge returns early.
So a fully drained peer is NOT served again by the passage of time alone — it
is served again because the serve loop charges it for the refusal, and that
non-zero charge is what runs `refill()`. The new test drives that exact
sequence rather than asserting a mechanism the code does not have. Two earlier
versions of the assertion failed for exactly this reason before being corrected.

Regression coverage (codes 5980-5987) asserts the whole property, not just
"it throttles": a flood is bounded at roughly the documented burst, a **different
peer is still served while the flooder is being refused** (without this it would
be a global rate limit and a self-inflicted DoS for every other user on the
machine), a refused peer is not charged for its own refusal, and the refusal is
temporary rather than a ban. `tools/linux_gates.py` gained six wiring
assertions so the port cannot be silently removed.

Verified: `--test` (UBSan), `--test --asan`, `--gates`, and a full `build.py`
across all four targets.

> Historical note: installer steering/footer reachability and zero-count bounds were corrected and exercised by the follow-up above.

## 2026-10-01: Fuzz the two uncovered trust boundaries (response validator, installer container)

Follow-up to the remediation pass below. The security audit's most useful
finding about the fuzz suite was not a bug in the code but the two untrusted
boundaries that had **no fuzz target at all**, despite both being large,
security-relevant and already carrying deterministic unit tests.

- `service_response` (target 7) drives `validate_service_response_for_ipc()`,
  the mirror of the already-covered request validator. This is the biggest
  function in the protocol validator and the one the GUI projects and trusts to
  decide a mutation was applied. Post-conditions: the exact magic/version pair,
  in-range status/operationState, `outcomeSeverityReserved == 0`, the
  status/severity agreement rule, both wire strings terminated, and idempotence
  (a second pass must neither reject nor mutate). A steer bit drives the
  payload-free-refusal arm directly, because that shape is what every refused
  request actually produces.
- `installer_archive` (target 8) drives `gc_archive_validate()` and
  `gc_payload_validate_footer()`. This parser runs in the ADMINISTRATOR setup
  program, and its most interesting field is the caller-derived `dataOffset`.
  Post-conditions on the accepted shape: every name NUL-terminated within its
  own field AND `gc_archive_name_is_safe()` (the `..`/separator/ADS/wildcard/
  DOS-device rules), every range inside the container in the overflow-safe form,
  and a real `gc_crc32` read of each range so ASan would catch a miss.
  Store-only, so there is no decompression-bomb surface to fuzz.
- Both build on Linux hosts for the same reason `update_manifest` does: header-
  only pure policy with no Win32 dependency, so they are in
  `FUZZ_LINUX_TARGETS` and get cross-linked everywhere.
- Seed corpora committed under `tests/fuzz-corpus/{service_response,
  installer_archive}/`, including deliberately bad-CRC and bad-magic variants
  so the refusal arms are seeded too, not only the accepted one.

**The new target found a real bug on its first run — in the harness.** A fuzzed
`fileCount` puts `dataStart` past the end of the buffer, `capacity - dataStart`
underflows in `gc_u64`, and `gc_crc32` is handed an enormous length over a wild
pointer. That is the harness's own arithmetic, not a defect in
`gc_archive_validate()` (it never sees those values), but an unguarded
subtraction in a fuzzer is still a fuzzer bug. Fixed with two explicit guards
and a comment naming why they are there.

**A second, subtler harness defect the numbers exposed.** The first revision
demanded the full header + 16-entry directory + 256 data bytes before doing
anything, so short inputs returned immediately. The measured `lim: 1721` was
below the 1723 bytes the footer path needed — half the target was unreachable,
and the low coverage (57 cov) was the symptom. Sizing the container FROM THE
INPUT instead makes short inputs useful (the validator simply reports a range
error, which is worth covering too), lifted feature coverage to 113, and made
the footer reachable.

Verified: `build.py --fuzz --fuzz-runs 5000` runs all EIGHT targets clean.

## 2026-10-01: Security-audit remediation pass (privileged boundary, supply chain, Linux abuse controls)

An end-to-end security audit of the whole repository produced findings across
six subsystems. This entry records what was CHANGED; the audit report itself was
delivered in chat and deliberately not written to the tree.

**Windows privileged boundary and diagnostics**

- `source/installer_main.cpp`: `initialize_process_mitigations()` is now the
  FIRST statement in `WinMain`. The setup program and the uninstaller were the
  only two shipped binaries that never called it, and they are the two that run
  with `requireAdministrator`. Static imports are bound by the loader before any
  of our code runs; `SetDefaultDllDirectories` covers every `LoadLibrary`
  afterwards and drops the CWD. The in-app update path was already safe: it stages into the hardened
  machine-config directory.
- The process-lifetime critical sections are no longer deleted. Detached
  workers (the Updates command worker, the logon-sync thread, the service update
  worker) can still call `debug_log()` and take `g_configLock`/`g_appLock`/
  `g_debugLogLock` after shutdown begins, so deleting them was a heap
  use-after-free in the exiting process. Touched
  `gui_process_cleanup.cpp`, `main_service_host.cpp`, `entry.cpp`, `main.cpp`.
  This matches what the debug-log writer's own event already did.
- `source/main_diagnostics.cpp`: `debug_log()` gained
  `__attribute__((format(printf, 1, 2)))`. It is the most-called logger in the
  product and had no attribute, so `-Wformat=2` was inert at every call site.
  Adding it immediately exposed four REAL latent bugs — `%u` against a `gc_u32`
  in `gui_mutation_worker.cpp`, `main_service_state_envelope.cpp`,
  `entry.cpp` and `gui_service_state.cpp`. `gc_u32` is `unsigned long`, which
  is 64-bit under MinGW/LP64 and 32-bit under clang-cl/LLP64, so those were
  genuine LP64 format bugs. Fixed with explicit `(unsigned int)` casts.
- Two real data races closed: `service_capture_owner_identity()` and the
  `g_app.configPath` mutations now run under `g_appLock`. The owner-identity
  writer is on the LIFECYCLE worker and the reader is on any of the six pipe
  workers inside `populate_service_snapshot()` — genuinely concurrent. The
  configPath publisher resolves into a LOCAL buffer and copies under the lock
  rather than holding `g_appLock` across `resolve_data_paths()` and a legacy
  `CopyFile`, so no disk I/O happens under the app lock.

**Log injection at the wire boundary**

- `source/service_protocol_wire_validation.h` gained
  `service_wire_string_is_log_safe()`, and
  `service_request_reject_reason()` applies it to `source`, `path` and
  `targetGpu.name`. The pipe ACL admits every authenticated local user, and
  `request->source` reaches the service log AND, via
  `set_pending_operation_source()`, the crash breadcrumb — the primary artifact
  for telling "the driver died under us" from "we handed the driver an
  invalidated handle". A CR/LF in it forged whole lines. TAB and bytes >= 0x80
  stay allowed so a non-ASCII profile path is not refused; DEL is refused.
  Empty strings are safe — a zero-initialised request carries them and the
  dispatch site already reads `x[0] ? x : "<default>"`.

**Authorization**

- `service_update_staging_dir()` now HARDENS and VERIFIES the staging
  directory's DACL instead of only checking attributes and reparse points.
  The parent's protected DACL only reaches a child the service itself creates,
  so a directory that already existed could keep a weaker DACL.
  `service_update_clear_staging()` also removes planted
  directory entries now — it used to `continue` past them, which is exactly the
  shape that makes `CREATE_NEW` fail forever.
  `tools/update_gates.py:189` asserted "update staging inherits the hardened
  machine-config DACL"; that is now true rather than aspirational.
- `service_command_requires_medium_integrity()` returns true for EVERY tier,
  READ included. The pipe ACL is `GRGW` for Authenticated Users — a strictly
  wider set than medium integrity — and READ is the command that delivers the
  authoritative state envelope, so a low-integrity process in the active
  session used to be handed GPU model, PCI IDs, fan RPM, applied OC state and
  the update posture. `main_service_pipe.cpp:291-299` already claimed
  low-integrity callers lose it; the tier table did not implement that.
- The updater now pins the staging DIRECTORY (no `FILE_SHARE_DELETE`,
  `FILE_FLAG_OPEN_REPARSE_POINT`) across `CreateProcessW`, copying the pattern
  `main_secure_write.cpp` already used for its own parent. The pinned file
  handle proves the BYTES; the directory handle keeps the NAME un-swappable.

**Supply chain**

- `ZIG_EXE` used to bypass ALL Zig verification while `--verify-toolchain`
  still reported green, because that check reads `ZIG_DIR`. An override is now
  verified against the pinned digest via the new
  `toolchain.verify_pinned_executable()` and refused on mismatch.
- Toolchain digests fail CLOSED: `resolve_archive()` and `archive_record()`
  refuse a missing pin instead of skipping verification, and `verify_tree()`
  refuses a manifest containing any unpinned entry.
- `packaging/arch/PKGBUILD.bin` pins the payload tarball (was
  `sha256sums_x86_64/aarch64=('SKIP')` for a binary that becomes
  `/usr/bin/greencurve` and runs as root). `PKGBUILD`'s source tarball is a
  GitHub-generated MUTABLE-tag archive whose digest cannot be derived offline,
  so the `SKIP` is now FATAL with instructions rather than a silent skip.
- The release workflow now runs `python build.py --gates` before building, and
  its checkout uses `persist-credentials: false` with the write token
  configured only inside the two publishing steps.

**Linux**

- `write_text_file_atomic()` (the CLIENT helper) now uses a CSPRNG temp name
  with `O_CREAT|O_EXCL|O_NOFOLLOW` and `renameat()` on a dirfd, matching
  `linux_daemon_state.cpp`. It used a fixed `<path>.tmp` with
  `fopen("wb")`, which follows a symlink and truncates its target.
- `LINUX_AUTO_RESTORE_TRIGGER_RESUME` is still exempt from the PER-BOOT
  counter (a laptop must not stop restoring after the third lid open) but now
  spends a separate short-window budget: 8 per 60 s, counted before the
  decision. The exemption was written assuming the trigger is a machine event;
  it is a repeatable socket command, so it needs its own budget.

**Build decomposition (forced by the size ratchet)**

- `--gates` was added as a cheap, no-compile run of the ~3000-line source gate
  suite, which previously ran only under `--test` and never in the release job.
  It lives in `security_gates.py` (`add_gates_argument`, `run_source_gates`) so
  build.py barely grows.
- The ratchet (5330, shrink-only) forced real decomposition:
  `get_windows_gui_compile_command`/`get_windows_service_compile_command`
  merged into one `get_windows_compile_command(service=...)`, and
  `linux_flags_for_arch` moved to `tools/build_variants.py`. build.py went
  5339 -> 5328.
- Moving `linux_flags_for_arch` broke a `require_text` gate that asserted the
  literal `flags.remove("-fcf-protection=full")` appears in build.py. Repointed
  at the file that now holds it; the behavioural check on the returned flag list
  was already present and is the one that matters.

**Verification.** `python build.py --test` (UBSan) passes, `python build.py
--gates` passes, and full `python build.py` builds Windows x64/ARM64 (both
toolchain variants) and Linux x64/ARM64 with every artifact gate. New regression
coverage: resume budget (5950-5957), wire log-safety (5958-5969), READ-tier
integrity (5262 inverted + 5970-5976). Two of the new assertions failed first
and were real: the log-safe predicate rejected empty strings, and the resume
budget's `>=` made it mean "seven allowed". Both fixed. `build.py --gates` also
gained `check_audit_finding_gates()` so none of the above can silently regress.

## 2026-09-30: Enforce release provenance before signing and publication

- Addressed the three review findings below in `tools/release_post.py`:
  both architectures require exact source-commit and release-workflow provenance
  before `prepare` or upload. Completeness/checksum checks precede provenance;
  dry runs use the same prerequisites. Per-installer diagnostics identify the
  expected source/workflow and verified result.
- The reviewed commit defaults to the local release tag, with full-SHA
  `--source-commit` for operators without that local tag. GitHub's actual tag
  must peel to the same commit; a moving release `targetCommitish` is not used.
  The release workflow now enforces source/workflow constraints for its own
  artifacts, and `release_manifest.py` guards those arguments.
- Added 22 deterministic orchestration tests in `tools/release_post_tests.py`,
  integrated into the existing self-test/build gates. Initial 17 tests failed
  against the original implementation (11 failures, 4 errors). Completed suite,
  `python build.py --test`, and full `python build.py` pass. Full build verified
  the Windows variant/architecture outputs and Linux packages. No extra C++
  diagnostics or runtime tests needed: only release tooling/workflow changed.
- Live read-only validation: local and published 0.27.0 tags match; both public
  installers pass the real installed CLI with exact source/workflow constraints.
  Incorrect source and workflow inputs are rejected. CLI TUF cache writes were
  denied in the sandbox's ordinary cache location; a child-process-only
  `LOCALAPPDATA` override kept the public trust cache in an ignored folder.
  No real signing key, remote write, or installer execution.
- Updated release procedure, source anchors, coverage, diagnostics, and stale-risk
  routing. Full corrected
  signing/publication remains a future authorized-release acceptance check.
- Commit: `04314df`. Staged `gitleaks protect` and a second scan of the exact
  28,097-byte staged patch passed. Exact-commit history scanning passed (one
  commit), and its file inventory/blob IDs matched the fully reviewed staged
  content; metadata uses the established public noreply identity. Wiki quality
  checks passed. No push; tracked worktree clean. Sources: [updates](../updates.md#release-automation-provenance-gates-2026-09-30),
  [testing](../testing.md), [build](../build.md).

## 2026-09-30: Focused updater and release code reviews

Historical review; its three provenance findings are addressed by the follow-up
above.

- Reviewed current clean HEAD (`de3cd54`), recent updater handoff changes,
  settings-transfer compatibility/replay boundaries, the release workflow,
  and post-release tooling. No additional confirmed bug in the reviewed
  service-parent handoff decision; live SCM/elevated integration remains open.
- Three unresolved provenance findings in `tools/release_post.py`: publication
  precedes provenance rejection, repository-only verification accepts a source
  commit other than the intended release, and ARM64 provenance is never checked.
  Missing ARM64 also publishes before failing; release-note mismatch is only a
  warning. See [updates](../updates.md#release-automation-provenance-gates-2026-09-30).
- Reproduction: a probe (details omitted) uses production
  manifest construction with mocked commands/network/signatures. Five probes
  passed by reproducing the current failures; no real key or remote mutation.
- `python build.py --test`, `python tools/release_post.py --self-test`,
  `python tools/release_prep.py`, and `python tools/update_signing.py self-test`
  passed. Optional cryptography cross-check unavailable. No crash dumps found
  in the workspace.
- Production source and tracked files unchanged; no commit or push. Updated
  release-tooling memory, corrected the obsolete claim that uploads have no
  automation, and checked source anchors, relevant wiki links, and index routing.

## 2026-09-27 (review follow-up): Establish legacy updater origin before skipping capture

- Corrected the two overbroad skips below: `--launch-session` can also be a manual silent command, and Session 0 does not prove the absence of active settings or a GUI handoff.
- `installer_main.cpp` now verifies a Session 0 parent against the live SCM service, pins the parent handle, and rejects creation times at/after setup to exclude PID reuse. Both legacy relaunch modes remain supported; explicit handoff still avoids the compatibility probe. Query failures retain ordinary capture ownership.
- Unhandled Session 0 capture retains FAILED and a failure log explaining that settings were not reapplied. Diagnostics distinguish explicit, legacy-service, and unconfirmed ownership.
- Regression evidence: standalone manual-command probe failed against the reviewed implementation, then passed after the fix; compiled cases 6680-6686 and source gates added. `python build.py --test`, full `python build.py`, and `python build.py --tidy` passed (no new tidy findings). Both Session 0 diagnostic gates reject the reviewed source and accept the fix. All four Windows setup artifacts contain the new diagnostics. No live elevated installer/update was run.
- Commit: `de3cd54`; staged and exact-commit gitleaks scans passed with no findings, and patch/metadata were reviewed. Wiki links, index routing, source anchors, and relevant claims were checked; corrected the stale payload-only export description. No push.
- Sources: [installer](../installer.md#settings-transfer), [updates](../updates.md#upgrade-settings-capture-backward-compatibility), [testing](../testing.md).

## 2026-09-27 (update): Updater settings handoff backward compatibility & Session 0 guard

Historical implementation, superseded by the review follow-up above.

- **Root-cause analysis:** When upgrading from 0.26.0 to 0.27.0 via the integrated updater, the 0.26.0 background service launched setup with `/S --launch --launch-session 1 --dir "..."` without the newer `--settings-captured-by-gui` switch (introduced in commit `b3231f7`). 0.27.0 setup attempted fallback settings export from Session 0, which the service correctly refused under `service_captured_identity_passes_session_rule` (`callerSessionId == 0 != activeSessionId`). Setup emitted `FAILED: capture: could not confirm or save the previous active settings`, which caused setup to rename its log to `greencurve-setup-error.log` despite the GUI handoff (`pending-update-restore.ini`) having already saved and successfully restored all 8 GPU settings upon relaunch.
- **Backward-compatible handoff inference:** Added `gc_installer_settings_capture_handled_by_gui()` in `source/installer_plan_policy.h` and hooked it into `source/installer_main.cpp`. Setup now automatically infers that settings capture was handled by the GUI whenever `--launch-session` is present, eliminating false-alarm capture attempts for updates initiated by older services.
- **Session 0 capture guard:** In `source/installer_apply.cpp`, `gc_capture_active_settings()` now checks `ProcessIdToSessionId()` and explicitly logs that settings export is skipped in non-interactive Session 0, preventing futile IPC attempts, error status, and false `greencurve-setup-error.log` generation.
- **Regression test units:** Added assertions 6474..6479 in `tests/regression_main.cpp` verifying `gc_installer_settings_capture_handled_by_gui()` with explicit handoff, legacy 0.26.0 updater arguments, default options, and null inputs.

## 2026-09-27: Stable 0.27.0 release published, attested, and updater assets verified

- **Release workflow execution:** Manual workflow `release.yml` dispatched on `main` (run `36274844164`). Successfully built the complete Windows + Linux matrix (x64 and arm64), generated GitHub build provenance attestations for all 8 packages and `greencurve-0.27.0-toolchain.json`, extracted release notes from `CHANGELOG.md`, created stable tag `0.27.0` and GitHub Release `0.27.0` targeting commit `2acdcad`, verified attestations, and published commit status `Provenance & Attestation` (`state: success`).
- **Updater assets signed & uploaded:** The manifest covers the published setup executables (`greencurve-0.27.0-windows-x64-setup.exe` [1953615 bytes, SHA-256 `bcf93c4251a7937b88e461dc51a8b7b41f60bdad9134970edcbaca283072d0d9`] and `greencurve-0.27.0-windows-arm64-setup.exe` [2933583 bytes, SHA-256 `534196bff535fd81bd52a73ad0f621664ce959f9fd725581b9032eb50c39b624`]); `greencurve-update-manifest.txt` and `greencurve-update-manifest.sig` published, bringing the release to 19 assets (operator steps omitted).
- **Anonymous post-publication verification:**
  - Fetched manifest and signature from anonymous `releases/latest/download/` URLs; verified signature against `GC_UPDATE_PUBLIC_KEY_ACTIVE` in `source/update_verify_keys.h`.
  - Downloaded installers via signed manifest URLs; confirmed exact byte lengths and SHA-256 hashes match. Confirmed redirect host lands on allowlisted `release-assets.githubusercontent.com`.
  - `gh attestation verify` confirmed artifact provenance. Release body verified identical to `CHANGELOG.md` extraction.
- **Tag signing:** tag `0.27.0` is SSH-signed and shows GitHub's `Verified` badge (operator steps omitted).
- **Future release automation:** Added `tools/release_post.py` to automate post-workflow publication and verification. Enhanced `tools/release_prep.py` with a `--preflight` signing-readiness check and hardened `release.yml` with a non-interactive signing-key check (details omitted). Corrected attestation badge link in `CHANGELOG.md` to `https://github.com/aufkrawall/green-curve/attestations` (was 404 `/actions/attestations`).

## 2026-09-26 (update 4): GitHub release attestation visual indicators & tag signing support

- **Release attestation commit status check:** Updated `.github/workflows/release.yml` with `statuses: write` permissions and added a post-verification step publishing a commit status under context `Provenance & Attestation` via GitHub Statuses API. Upon release artifact attestation verification (`gh attestation verify`), GitHub renders the green checkmark circle (`✔`) on the release commit and tag ref, linking directly to the attestation workflow run.
- **SSH tag signing integration:** Added support in `release.yml` for optional maintainer SSH signing key (`TAG_SIGNING_KEY` secret). When configured, the release step creates an annotated, SSH-signed tag (`git tag -s`) and pushes to `refs/tags/${TAG}`, displaying GitHub's native green `Verified` badge next to the release tag. If absent, lightweight tag creation continues gracefully.
- **Visual attestation badge in release notes:** Added the GitHub Attestation Verified badge (`[![Attestation](...)]`) and `gh attestation verify` command to `CHANGELOG.md` under `### Downloads and verification` for `## 0.27.0`. Validated with `tools/release_prep.py` and `security_gates.py`.

## 2026-09-26 (update 3): Changelog topic-lead bolding overhaul & legacy uninstaller cleanup clarification

- **Changelog scannability overhaul:** Adopted topic-lead bolding (`- **Topic:** description`) across `CHANGELOG.md` (`## 0.27.0`) for `### New`, `### Improved`, and `### Fixed`. Resolved uniform "wall of bold" readability defect where 18 out of 19 bug fix entries were 100% bolded single-line sentences, eliminating contrast between bold anchors and regular text.
- **Legacy uninstaller upgrade copy clarified:** Rephrased the misleading entry "Upgrading now removes the old uninstall.exe when it is left behind" to "**In-place upgrades:** now remove the legacy `uninstall.exe` left behind by older versions." Clarified that the uninstaller rename to `greencurve-uninstall.exe` left behind orphaned pre-rename `uninstall.exe` binaries when upgrading over pre-0.27.0 installations, which `gc_remove_stale_payload_leaves()` reconciles post-commit.
- **Rule harmonization:** Updated `CHANGELOG.md` header instructions, `AGENTS.md`, and `llm-wiki/changelog-style.md` to reflect the topic-lead bolding standard. Verified `tools/release_prep.py` passes cleanly.

## 2026-09-26 (update 2): GitHub release rollout dry-run & 0.27.0 auto-update transition simulation

Comprehensive dry-run and simulation of the GitHub release rollout procedure (`.github/workflows/release.yml`, `tools/update_signing.py`):

- **Release notes & metadata validation:** Verified `VERSION` (0.27.0), `CHANGELOG.md` (`## 0.27.0`), `README.md` references, `tools/release_prep.py`, and the exact `awk` extraction used by `release.yml`. Confirmed remote tag pre-conditions via `git ls-remote --tags origin refs/tags/0.27.0` (tag absent on remote).
- **Packaging pipeline verification:** Built and verified all 16 Windows variant artifacts (MSVC and release toolchains, x64 and arm64) in `dist/windows-<arch>/<variant>/` and all 8 Linux packages and checksums in `dist/linux-<arch>/`. Confirmed `release.yml` artifact paths, provenance attestation patterns, and release upload globs.
- **Signing setup & manifest dry-run:** Signing-key readiness verified; the derived public key matches `GC_UPDATE_PUBLIC_KEY_ACTIVE` in `source/update_verify_keys.h`, and a manifest signing dry-run on the 0.27.0 setup binaries produced a detached signature that verified (operator steps omitted).
- **Auto-update policy & URL verification:** Tested compiled C++ policy harness with `update_manifest_policy.h` and `update_url_policy.h`. Confirmed public installed versions 0.23, 0.23.1, 0.24.0, 0.25.0, 0.25.1, 0.25.2, and 0.26.0 all receive `GC_UPDATE_DECISION_AVAILABLE` on both architectures; 0.27.0 receives `GC_UPDATE_DECISION_UP_TO_DATE`; 0.28.0 receives `GC_UPDATE_DECISION_REJECTED`. Confirmed URL builders and installer CLI argument parsing (`/S`, `--launch`, `--launch-session`, `--settings-captured-by-gui`, `--dir`).
- **Regression test units added:** In `tests/regression_main.cpp`, added 0.27.0 update restore decision assertions (5907..5910, 5922) and 0.27.0 release candidate manifest parsing and transition verification (5911..5921).

## 2026-09-26 (update): Installer move copy correction & SAST leak-tooling readiness

- **Installer move accuracy:** In `source/installer_ui_pages.cpp`, corrected the folder page note to "Green Curve %s is already installed in %s. Keep that folder to upgrade in place, or choose a different folder to move the installation." The prior parenthetical "(existing files will not be deleted)" was factually inaccurate: under `gc_retire_previous_directory()` (`source/installer_move_cleanup.cpp`), moving a setup-managed installation explicitly deletes setup-owned leaves (`greencurve.exe`, `greencurve-service.exe`, etc.) from the previous directory and removes the folder when empty.
- **Security audit / leak tools configuration:**
  - Resolved local executables into `<tool-dir>` (`gitleaks.exe` 8.30.1, `osv-scanner.exe` 2.4.0), making `gitleaks`, `trufflehog`, and `osv-scanner` directly accessible on `PATH` without mutating system environment variables.
  - Verified `semgrep` 1.168.0 and `flawfinder` 2.0.20 are available on `PATH`.
  - Added repository `.gitleaks.toml` with `[extend] useDefault = true` and `[allowlist]` for known public fixtures: RFC 6979 deterministic ECDSA P-256 test vector (`tools/update_signing.py`), `source/update_verify_keys.h`, `tests/fuzz-corpus/`, and `libnvidia-api.so.1`. Full 293-commit history scans and staged commits now run cleanly with 0 false positives.

## 2026-09-26: User-facing copy and language modernization across installer, GUI, uninstaller, and CLI

Comprehensive review and cleanup of user-facing language applying principles similar to `changelog-style.md` (user-first, scannable, free of internal code jargon, professional, and consistent with Windows desktop conventions):

- **Path consistency:** Fixed `%ProgramFiles%\greencurve` -> `%ProgramFiles%\Green Curve` in `service_status_notice_policy.h`. Added regression assertion 5906 in `tests/regression_main.cpp`.
- **Colloquialism & tone cleanup:**
  - `installer_ui_pages.cpp`: "Read it, then continue." -> "Please review the terms before continuing."
  - `installer_ui_pages.cpp`: "...leave the old files for you to delete." -> "...(existing files will not be deleted)."
  - `installer_ui_pages.cpp`: "...left alone." -> "...preserved."
  - `installer_ui.cpp`: "...says why." -> "For details, see greencurve_cli_log.txt in %LOCALAPPDATA%\Green Curve." and "could NOT be applied again" -> "could not be reapplied." Pinned requirement `is deleted at the next restart` preserved.
  - `gui_update_first_run.cpp`: "reveals your IP address..." -> "checks GitHub once a day using your current version and architecture. Nothing is downloaded or installed without your confirmation."
- **Internal jargon removed from UI dialogs:**
  - `ui_main_apply.cpp`: "VF topology" -> "configuration".
  - `main_gpu_state.cpp`: "fallback VF backend layout" -> "fallback clock and voltage controls".
  - `ui_main_context_menus.cpp` & `main_cli_help.cpp`: "machine-wide bank" -> "for all users" / "shared profile".
  - `xbar_dialog.cpp`: "Requires driver support (validated schema)." -> "Requires driver support. Background service required."
- **Ambiguous modal semantics resolved:**
  - `main_capability_warning.cpp`: Added clear question before Yes/No buttons ("Show this warning again on next launch? (Select No to disable this warning)").
  - `main_gpu_state.cpp`: Clear bulleted action descriptions for fallback Yes/No/Cancel dialog.
- **Terminology harmonization:** Standardized on "Click Apply Changes" and "at Windows logon" across `entry.cpp`, `installer_ui.cpp`, `xbar_dialog.cpp`, and menus.

Older entries: [September 13 through September 25 archive](archive-2026-09-13-to-2026-09-25.md).
