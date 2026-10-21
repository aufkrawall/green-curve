# Recent Log

Newest-first rolling memory. Archive older entries to `archive-YYYY-Www.md` when this file grows too long.

This log is PUBLIC (tracked since 2026-10-22): every entry follows the same
rules as the topic pages (`secret-leak-prevention.md`, "Public wiki safety").

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
