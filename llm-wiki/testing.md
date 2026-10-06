# Testing

## Release and packaging input review (2026-10-01)

Source anchors: `tools/update_manifest_tools_tests.py`,
`tools/release_post_tests.py`, `tools/release_renew_tests.py`,
`tools/arch_recipe_checks_tests.py`.
Signing-input regressions execute through the signer's self-tests (`--test`),
and publication/renewal fixtures execute through release-manifest gates.
Non-latest publication fails before download/sign/upload, including dry-run;
channel changes during signing prevent upload for both publisher paths.
Canonical native version rules and manifest duplicate/unknown-key/asset bounds
are covered. Malformed renewal rejects before reading a private key. All keys,
network calls and release publication in orchestration fixtures are mocked.

Arch recipe checks run through Linux packaging source gates on every host.
They verify actual local source bytes and real remote checksum pins, with
fixtures for changed resume-service bytes and missing/unpinned source digests.
Both checksum drift and the source recipe's SKIP failed before their fixes.
Native WSL Arch `makepkg --verifysource --nodeps` additionally established
incorrect published binary pins. Both published tarballs were provenance-
verified against the reviewed release/workflow before correcting the pins;
source plus both binary architecture verification now passes. This does not
install a package or replay GPU operations. Final produced Arch packages were
inspected for the exact resume-unit bytes and mode 0644.

## Post-0.27.0 review follow-ups (2026-10-01)

Source anchors: `tests/review_followup_tests.cpp` (6250-6277, linked into the
regression harness from `build.py`'s harness list), `tests/service_install_tests.cpp`
(6222-6241), `tests/linux_transport_regression.cpp` (67-75),
`tools/release_renew_tests.py` (run by `release_post.run_self_tests()`, hence
by `--gates`/`--test`). They cover the freshness refusal classification, the
Linux atomic writer's path/temp-name rules, recovery from leftover installer
temporaries (and that a non-empty directory obstruction is still refused), the
client reading a busy refusal in both close/write orders, and the renewal
command's refusal arms (non-latest release, unverified v1, installer mismatch,
provenance, altered v1, envelope over other bytes, bad new signature, dry run,
anonymous corruption). The Linux fixture and an ad-hoc writer probe were also
EXECUTED under WSL Arch on the Windows dev host (user and root); CI remains the
standing Linux runner. The shared-bank re-proof state machine is covered by
source gates only (`check_audit_finding_gates`), not an executing test.
The StringCchCopyA/HRESULT compatibility shim suite (6270-6277) exercises
safe copy, empty string, exact-fit buffer, truncation to NUL, zero capacity,
and SUCCEEDED/FAILED evaluation under both platforms. In addition,
`tools/security_gates.py` cross-links the full regression test harness
(`regression_main.cpp` and test units) for `x86_64-linux-gnu` via Zig on
non-Linux hosts during `--test`, preventing Linux build regressions from
reaching CI unnoticed.

## Audit regressions (2026-10-01)

Source anchors: `tests/security_audit_tests.cpp`,
`tests/installer_fuzz_harness_tests.cpp`, `tests/service_install_tests.cpp`,
`tests/linux_transport_regression.cpp`, `tools/update_freshness.py`.
The archive harness regression executes the ACTUAL fuzzer entry point with
parser-observation wrappers. It requires both archive/footer accepted paths,
footer execution on short inputs, and the formerly masked zero-file-count
heap overflow case under ASan. This specifically detects dead harness coverage,
which ordinary parser tests cannot detect. Independent tail steering and footer
bytes are seeded; the update-manifest target also fuzzes the freshness parser.

Other focused regressions exercise strict expiry and skew, mandatory-envelope
refusal, account leases before any pipe read, named-principal/null DACLs,
unproven bank content removal, hard-link refusal, exclusive temporary collisions
(refusal since replaced by discard-once recovery, see the section above), EINTR/partial
record reads, saturating quota charges and refill on admission. Signer self-tests
force an r=0 retry with a distinct second RFC 6979 nonce. Release publication
fixtures cover all four assets and dry-run prerequisites. Linux refusal uses
the production nonblocking busy helper in a native socket-pair fixture.
Desktop escaping tests assert both required decoding layers; Linux source gates
execute the actual Bash substitutions with controlled quotes/backticks/dollars/
backslashes/percent and spaces, without running installation.
Platform limits remain explicit: Linux fixtures cross-link only on Windows;
SYSTEM/elevated updater/installer and real GPU behavior were not exercised.

## Test approach

The 2026-09-23 Pascal locked-clock fixture in
`tests/clock_transition_tests.cpp` makes both set and reset return
`NVML_ERROR_NOT_SUPPORTED`. It covers VF FLATTEN and no-lock completion, HARD
and transient-refusal vetoes, retained-cap refusal, and verified Linux snapshot
rollback without an impossible clamp. `tools/apply_ceiling_gates.py` pins the
Windows and Linux integration sites. The full orchestrators and live Pascal
hardware remain untested.

The 2026-09-23 settings-transfer result suite (5790-5795) covers the pure
three-way setup decision: captured snapshot, service-confirmed no active intent,
or failed/ambiguous capture. `tools/installer_build.py` gates the CLI exit-code
handoff, setup completion warning, updater warning, and confirmed-handoff capture skip.
Cases 5796-5801 verify the updater handoff marker in both relaunch forms and
the installer parser's silent-only acceptance.
The 2026-09-27 follow-up adds 6680-6686: manual `--launch-session` keeps capture,
verified legacy service launches transfer ownership in both relaunch modes,
and null/non-silent inputs cannot infer legacy ownership. The focused manual
probe was observed failing before the change and passing after. Native service
parent identity/lifetime checks and Session 0 failure-log retention are source
gated in `tools/installer_build.py`; no live SCM/update launch was performed.
It does not execute the Windows setup or updater GUI; an interactive failed
capture and an elevated setup click-through remain live acceptance cases.

The [clock transition hardening](clock-transition-audit.md) includes a separate
`tests/clock_transition_tests.cpp` translation unit linked into the regular
harness. It executes production transition/rollback/verification helper headers
with fake driver dependencies, including the point-69 derived-target regression.
This improves on pure decision tests but does not execute complete apply
orchestrators. Their wiring remains covered by source gates; the complete
per-driver-call fault/lifecycle matrix and hardware acceptance remain open.
Do not equate passing policy/helper tests with proof of driver atomicity.

No external test framework. Tests are built and run via `python build.py --test`.

The 2026-09-30 release-tooling fixes add 22 deterministic tests in
`tools/release_post_tests.py`, called by `release_post.run_self_tests()` through
the normal build gates. They execute production manifest construction with
mocked commands/network/signatures and enforce both installers' exact source
and workflow verification before signing/publication, tag identity and peeling,
complete assets/checksums, equivalent dry-run prerequisites, and delivery
corruption refusal. The initial 17-test suite failed against the original code;
the completed suite passes. Both real published installers also passed the
installed CLI's source/workflow constraints; incorrect constraints were
rejected. The ignored review probe is historical evidence, not an acceptance
test. See [updates.md](updates.md#release-automation-provenance-gates-2026-09-30)
for diagnostics and the remaining future-release integration limit.

Two layers:
1. **Compiled regression tests** — a test harness compiled on the fly with the
   bundled LLVM-MinGW on Windows or Zig elsewhere
2. **Source regression checks** — text-based assertions on source file content

## Running on a Linux host

`--test` runs natively on Linux as of 2026-07-27. It previously could not run
there at all: the harness was fed Win32-only translation units
(`config_utils.cpp`, `service_acl.cpp`, `platform_win32.cpp`) and failed at
`windows.h`. No extra toolchain is needed — the Zig that `build.py` already
downloads is a complete native compiler.

What made it portable:

- Suites that exercise **Win32 implementations** are `#if defined(_WIN32)` in
  `tests/regression_main.cpp`: config-INI storage and its named mutex, the
  F-SEC/F-15 DACL round-trips, Task Scheduler XML, and the WCHAR CLI parse.
  Everything platform-neutral — fan-curve math, all layout/lock/lifecycle/
  auto-profile policies, and the whole `linux_*` **policy-header** family —
  runs on both hosts.

  Read that last clause precisely (corrected 2026-09-15 by the code audit): it
  is the pure `linux_*.h` headers that the harness compiles and asserts. The
  `linux_*.cpp` IMPLEMENTATION files are not linked into it — see
  "What the compiled harness actually links" below.
- `win32_compat.h` is force-included on Linux (never `#include`d by the
  harness, so Windows never sees it) and supplies the `BN_*`/`VK_*` wire
  constants the pure policies name.
- Portable helpers were moved out of Win32 files: `trim_ascii`,
  `streqi_ascii`, `parse_int_strict` and `set_message` to
  `config_text_utils.cpp`; `gpu_family_uses_best_guess_backend` to
  `vf_backends.cpp`. `fan_curve.cpp` uses `gc_snprintf`, not `StringCchPrintfA`.
- The harness reports its assertion code on stderr and maps failures to a
  single non-aliasing exit status, because a POSIX exit status is only the low
  8 bits — assertion 1670 would otherwise surface as 134, itself a real
  assertion number.

**Known gap: CLOSED 2026-07-28.** `linux_port.cpp` used to duplicate both the
text helpers and the fan-curve math, so the harness validated `fan_curve.cpp`
while the Linux daemon ran a different copy. Both are now deduplicated;
`config_text_utils.cpp` and `fan_curve.cpp` are in `LINUX_SOURCE_FILES`.

The fan-curve half was **not** benign. The two copies had diverged in the
degenerate branch: the Linux one wrote past the end of the 8-point array and
produced a 2-point curve capped at 35% where the tested one produces the 5-point
default reaching 90%. Full analysis in [fan-control.md](fan-control.md);
assertions 1930-1946 cover it and fail against the old implementation.

The text-helper half was benign. `linux_port.cpp` used to duplicate
`trim_ascii`, `streqi_ascii`, `parse_int_strict`, `set_message` **and**
`parse_fan_value` verbatim; `config_text_utils.cpp` is now in
`LINUX_SOURCE_FILES` and those five duplicates are gone, so both platforms link
one definition. The five implementations were byte-equivalent in behaviour
before the merge (`gc_vsnprintf` is a thin `vsnprintf` wrapper on Linux, and
`StringCchCopyA` and `gc_snprintf("%s")` truncate at the same boundary), so this
changed no behaviour — it changed *what is tested*. A source guard in
`check_fuzz_harness_in_sync()` fails the build if `linux_port.cpp` defines any
of the five again; negative control performed.

## What the compiled harness actually links

Recorded 2026-09-15 by the code audit, because the sentence above was being
read more broadly than it is true.

`run_regression_tests()` in `build.py` links `tests/regression_main.cpp` plus
exactly these production translation units:

- `fan_curve.cpp`, `config_text_utils.cpp`, `app_shared.cpp`, `vf_backends.cpp`
- Windows host only: `config_utils.cpp`, `service_acl.cpp`, `service_path_chain.cpp`,
  `service_acl_handle.cpp`, `service_install_location.cpp`, `platform_win32.cpp`
- POSIX host only: `platform_posix.cpp`

That is 10 of ~340 files in `source/` (plus the extra test translation units
`tests/clock_transition_tests.cpp`, `tests/service_install_tests.cpp`,
`tests/apply_profile_followup_tests.cpp`, `tests/apply_correction_tests.cpp`
(6500-6531: the apply's VF correction loop against a simulated driver, see
[clock-transition-audit](clock-transition-audit.md)) and
`tests/fan_worker_lock_tests.cpp` (6550-6569: native Windows threads/events/
mutex fixture for the fan worker's cancellable lock wait, see
[fan-control](fan-control.md)); `service_install_tests.cpp` owns
assertions 5720-5786: SCM checkpoint-wait policy, the location content rule,
the exact-DACL predicate, handle-bound hardening with propagation to existing
files, the release-to-inherited contract, and the native location verdicts;
`apply_profile_followup_tests.cpp` owns 6430-6499: the lock/pre-tail refusal
policy and Linux fixed-fan maintenance). The 2026-09-24 audit follow-ups also
added 6400-6424 and 6470-6473 inside `regression_main.cpp` (INI encoding round
trip, auto-profile failure backoff, legacy repair gating). Everything else the harness "covers" it
covers through **header-only policy code** it `#include`s — which is a lot,
and is why there are ~2,500 assertions — but no `.cpp` implementation outside
that list is executed by a test. In particular **`gpu_backend_apply.cpp`
(~1,380 lines, the hardware write sequence; since 2026-09-24 its correction
loop's decisions are executed through `apply_correction_policy.h`, the rest is
not), `main_service_pipe.cpp` (the
privileged trust boundary), `main_state_sync.cpp`, the whole GUI, the whole
installer and the entire Linux daemon have no executable coverage.** They are
guarded by `require_text`/`forbid_text` source assertions, which verify that a
line of code is PRESENT, not that it BEHAVES.

This is a deliberate architecture (the pure/impure split is what makes the
policy layer testable at all), not an oversight — but it bounds what a green
`--test` run means, and two 2026-09-15 findings (a duplicated NvAPI VF read
that had diverged, and a missing enumeration clamp) were in exactly that
uncovered surface. The audit's recommendation, not yet done: extract the apply
ORDERING decision from `gpu_backend_apply.cpp` and the command→authorization
tier mapping from `main_service_pipe.cpp` behind pure seams, the way
`service_lifecycle_policy.h` already does for the lifecycle state machine. The
tier mapping half of that WAS done — see `service_command_authority_policy.h`.

## Compiled regression tests

The harness lives in **`tests/regression_main.cpp`** as a real translation unit
and is compiled/run by `run_regression_tests()` in `build.py`.

The updater policy block carries an exact next-stable-release case in addition
to its generic parser/order coverage. For 0.25.0 it parses the floorless,
two-architecture manifest shape and proves that an installed 0.24.0 client
selects `AVAILABLE` plus the exact version-bound x64/ARM64 setup names. The
0.24.0 tag's version, manifest, and embedded-key headers are byte-identical to
the current copies, so this case exercises the policy shipped to 0.24 users;
the Windows half of the harness also compiles the real CNG verifier.

It used to be a ~4000-line raw string literal inside `build.py`, which meant it
had no LSP, no clang-format, no clang-tidy and no syntax highlighting, and
adding a test meant editing a Python string. It was moved out verbatim, so
every exit code is unchanged; `compile_commands.json` now includes it, so it
gets the same tooling as production code. A `forbid_text` guard stops it
migrating back into a string literal.

`build.py` went from 8994 to ~5600 lines as a result and is now itself covered
by `BUILD_SCRIPT_SIZE_RATCHET`, having previously been exempt from the size
rule it enforces on `source/`. After the parallel-build split (build 533) it is
5223 lines; the ratchet still lives in `tools/build_state.py`, so the check
that would catch a future regrowth is itself outside `build.py`.

The parallel-build scheduler (`tools/build_scheduler.py`) has its own
deterministic self-tests, called from
`security_gates.run_build_script_regression_tests()` so `--test` runs them on
every host with no toolchain: `compile_only_flags()` keeps `-flto` only when
asked and strips every linker-only argument (including `-target`'s value),
`run_parallel()` preserves task order and propagates failures,
`compile_objects()` runs every compile and fails on the first non-zero
returncode, `auto_job_count()` never returns zero, and `serialized_output()`
restores the original `print` after the parallel section. The same self-tests
pin the build.py wiring: `--jobs` must exist, `run_parallel` must be used, and
the object-first Windows x64 mode string must remain.

The MSVC-ABI toolchain module (`tools/msvc_toolchain.py`, used by the native
Windows `msvc` variant) has the same kind of self-tests in

`run_self_tests()`: the hardened flag sets must carry `-c`,
`-D_CRT_SECURE_NO_WARNINGS`, `/GS`-equivalent `-GS`, `-guard:cf`, `-sdl`,
`-EHs-c-`, `-GR-`, and `-ftrivial-auto-var-init=pattern`; service/arm64
variations must add exactly their own flags; no GNU-style linker flag may leak
into a cl-style compile; x64 links must carry `-cetcompat` while arm64 links
must not; `-l` library lists must map to `.lib` and refuse anything else.
Discovery itself is fail-closed: a candidate toolchain is only accepted after a
compile+link probe for every requested architecture with the real flag set
(`resolve_requested()`), so a half-installed MSVC stack cannot poison a build.
The PE artifact gates are toolchain-aware in `tools/pe_verify.py` (x64+clang-cl
requires the CET_COMPAT debug entry; arm64+clang-cl requires nonempty GFIDS),
and `verify_windows_private_symbols` dispatches on the PDB file magic so the
same check covers clang-cl PDBs and the Zig arm64 DWARF image.

`tools/build_variants.py` self-tests the native-Windows `auto` plan (MSVC plus
release), explicit single-toolchain plans, Linux-host release selection, and
unique payload/package/symbol paths. Native x64 and ARM64 builds verify both
variants; native x64 packaging additionally verified both archives and setup
executables under their variant directories.

The Zig cache module (`tools/zig_cache.py`, added 2026-08-28 after a poisoned
shared global cache failed all four Zig-driven links on native Windows) has
the same kind of deterministic self-tests in the same runner: real-error-line
parser fixtures (quoted/unquoted lld diagnostics, forward/backslash roots,
dir-only forms, non-cache paths ignored), lock exclusivity proven with
non-blocking acquires (no timing), wipe/refusal rules, and retry-once repair
with an injected fake runner — including the negative paths (a non-cache
failure and a stale diagnostic whose artifact still exists must never wipe a
cache root). Source guards require the lock/repair wrapper at every Zig link
call site (build.py's link functions, the `--jobs 1` fallback, the Linux
fixture link, the arm64 installer link, the fuzz fixture link).

### Native Windows named-pipe fixture (added 2026-08-23)

`tests/windows_pipe_regression.cpp` is compiled AND executed by `--test` on
win32 hosts only (owned by `security_gates.run_windows_pipe_fixture`,
mirroring the Linux-only fixtures that cross-compile elsewhere). It drives
real message-mode pipes in-process to cover what pure tests cannot see:

- the 12-byte `ERROR_MORE_DATA` prefix probe is a valid first read;
- `ImpersonateNamedPipeClient()` succeeds AFTER that probe (the 2026-08-22
  error-1368 incident regression -- fails on any pre-read design);
- the remainder reads exactly into `ServiceRequest`, hostile tail bytes drain
  without delaying the response (the request and tail are one message-mode
  `WriteFile`, never several scheduling-dependent messages);
- a stalled client's header probe times out cleanly while a second client
  completes a full round trip in parallel;
- bounded `WaitNamedPipeW` retry covers transient `ERROR_PIPE_BUSY`;
- (added 2026-08-29) the server disconnects only after the client consumed
  the response — `DisconnectNamedPipe` DISCARDS unread buffered pipe data, so
  an unsequenced disconnect handed a descheduled client a spurious
  `ERROR_BROKEN_PIPE` (CI: client exit 907, server exit 0). The server waits
  bounded on a response-consumed event before disconnecting; the client
  signals it on BOTH terminal paths. The client also verifies the read itself
  (a completed-but-FAILED overlapped read signals the event too, so
  `GetOverlappedResult` must be checked — new exit 909) and the pong content
  (magic/version/status — new exit 910); the stalled case deliberately
  accepts any clean END of its pending read instead. Failure paths print the
  concrete `GetLastError` code.

Sequencing is event-driven (server signals instance-ready and
client-connected; main starts clients off those events, no `Sleep`s), and
the "healthy beats the stall" property is asserted by ORDERING — the healthy
exchange completes before the stalled connection's own probe deadline
(`kFixtureProbeTimeoutMs`) can fire — instead of a wall-clock bound. Joins
are bounded and treat timeout as failure. Failed joins print both client and
server wait/exit codes instead of collapsing all detail into main's exit code.

The write-capable `--clk-domain-probe` selector is separately covered by pure
regression codes 4800-4808 through `clk_probe_entry_policy.h`: exact endpoints
and surrounding ASCII whitespace pass; empty, signed, junk-suffixed,
out-of-range, and zero-domain inputs fail. This prevents malformed environment
or marker-file text from degrading to entry zero through `atoi`.

The same fixture session exposed a real production defect: the first drain
helper blocked for its whole deadline on clients that sent exactly one
message; it now Peek-gates and returns instantly on an empty queue.

The pure suite gained tests 4600+ for `service_ipc_throttle_policy.h`
(classification of every command, wrap-safe time, burst/refill/cost
boundaries, handoff reserve independence, fresh-LUID freshness, idle expiry,
full-table eviction without lockout, unknown-command and anonymous-budget
behavior, plus the `service_ipc_connection_cost_tokens()` outcome table with
its zero-cost-refusal-is-a-no-op property) and tests 4700+ for
`debug_log_rotation_policy.h` (cap boundaries, non-positive-cap guard,
marker-line contract).

`security_gates.check_ipc_transport_and_probe_gates()` (2026-08-23 review)
pins four silent-failure shapes no test could see: the single shared
admission-table accessor (two function-local tables once made rate limiting
inert), the exactly-once charge sites, the `~ServiceClientIdentity()` token
destructor (a per-connection SYSTEM token-handle leak), and both
`g_app.gpuCapability = probe;` publication exits. Since 2026-08-29 it also
pins the fixture's response-consumed ordering (server pre-disconnect wait,
both client notification paths, read/result and pong-content verification),
so the disconnect race cannot quietly return.

### What is tested

- **Fan curve math:**
  - Default curve has 5 active points
  - Interpolation produces expected values (e.g., 30C → 20%)
  - Mid-range interpolation stays within bounds
  - Invalid curves (non-monotonic fan%) are rejected
  - Normalization clamps poll interval to 250ms minimum
  - Degenerate configs with fewer than two enabled points normalize to the safe default curve without writing past the 8-point array

- **CLI point parsing (21-25):**
  - `--point0` → index 0
  - `--point127` → index 127
  - `--point128` → rejected (out of range)
  - `--pointabc` → rejected (not a number)
  - `--point-1` → rejected (negative)
  - Ran only on Windows until 2026-07-28 — `parse_cli_point_arg_w` lived in the
    Win32-only shard, so the block was `#if defined(_WIN32)` and could not link
    on Linux. Now platform-neutral.

- **Shared text parsers (F-LNX-DEDUP, 523-527 and 1900-1920):** all three moved
  out of `config_utils.cpp` into `config_text_utils.cpp`, so these now run on
  both hosts and cover what the Linux daemon executes.
  - Section headers: `[profiles]`/`[Profiles]`/`[PROFILES]` all match
    case-insensitively; `[profiles_old]` and `[profile]` do not (523-527, moved
    here out of a Win32-only block).
  - `_strnicmp` was replaced by an explicit ASCII fold, so the separating case
    is pinned: a line **shorter** than the section name (`"[prof"`, `"["`,
    `"[]"`) must stop at the NUL rather than read past the buffer — ASan/UBSan
    turn a regression into a hard failure. Empty section name, a non-header
    line, and both null arguments are covered too.
  - `parse_fan_value` had **no** regression coverage at all before this change,
    on either platform, despite the Linux daemon running its own copy: `auto`
    case-insensitively, empty and null input, surrounding whitespace, the
    `0`/`100` bounds, `101` and `-1` rejected, non-numeric text rejected, and
    both null out-parameters rejected rather than dereferenced.
  - `StringCchCopyA` became `gc_snprintf("%s")`; an 79-character `9` run must
    still be rejected rather than parsing whatever fitted in the 64-byte buffer.

- **Automatic restore policy:**
  - A configured logon profile is permitted only when the persistent safety
    lockout is clear.
  - Typed-origin classification identifies GUI Apply, explicit CLI, hotkey, and
    tray selection as explicit; app-launch, foreground, logon, standby, and
    driver recovery are automatic. Source guards pin the server-side rule that
    only explicit successful Apply may clear lockout/history.
  - Standby restoration allows complete active intended settings immediately
    and once per suspend generation unless locked out.
  - Driver recovery is rejected without current-boot proof, at 9:59 awake
    time, and after lockout; it is allowed exactly at 10:00. Sleep does not
    advance the fake unbiased clock.
  - Standby preserves proof age only at or beyond the exact 10:00 boundary.
    A 9:59, invalid, missing, or cross-boot proof restarts after successful
    restore; source-order guards require capture before write invalidation and
    republication only after successful hardware apply.
  - Current-boot proof compares both 64-bit halves of the Windows
    `BootIdentifier`; zero, high-half mismatch, low-half mismatch, and legacy
    proof formats fail closed. Source guards forbid the former adjustable
    `SystemTimeOfDayInformation.BootTime` authority.
  - Invalid lifecycle/profile/lockout IPC metadata is canonicalized safely;
    invalid lockout data remains locked out.
  - Full-restore construction retains combined, curve-only, lock-only, and
    fan-only intent without resetting unrelated domains. Named-profile
    transitions reset only previously Green Curve-owned omitted controls while
    publishing the exact new profile; ad-hoc sparse ownership remains mergeable.
  - Exact selected-GPU PnP policy accepts complete matching NVIDIA PCI identity
    and rejects partial, ambiguous, wrong-device, or wrong-BDF identities.

- **GPU family best-guess dispatch:**
  - Pascal, Turing, Ampere, Lovelace, Blackwell → tested known backend
  - Unknown/future family → best-effort fallback
  - Shared `vf_backend_for_architecture()` maps Blackwell/Ada/Ampere to non-best-effort backends and unknown architectures to the best-effort future backend

- **F-CAP service carriage and self-test verdicts (2178-2205):**
  - All seven two-bit domain states round-trip exactly through the compact
    `ServiceGpuHealth` representation, alongside unified-memory topology.
  - Protocol validation rejects out-of-range topology and unused packed bits.
    A legacy v16 producer's five zero reserve bytes decode to conservative
    `UNKNOWN`/`UNPROBED`, preserving the never-subtract-unprobed invariant.
  - The Windows driver pre-flight reaches `FULL` only when NVML, the curve read,
    the private CONTROL read, VF writability, and a full surface all agree.
    CONTROL/NVML failures, monitor-only hardware, unelevated private-read
    failures, and unusable initialization have distinct verdicts and nonzero
    process exit status.

- **Config I/O roundtrip:**
  - `get_config_int` / `set_config_int` roundtrip
  - `config_section_has_keys` detection
  - The `Global\\GreenCurveConfigMutex-v2` lock opens with only
    `SYNCHRONIZE | MUTEX_MODIFY_STATE`; source guards pin its cross-session name,
    least-privilege DACL/medium-integrity label, and fail-closed setup/wait path.
  - `parse_int_strict` accepts exact 32-bit bounds and rejects overflow/underflow values instead of accepting C library saturation
  - The logon selection transaction updates `logon_slot` and
    `logon_shared_slot` together, preserves unrelated `[profiles]` keys, rejects
    two simultaneous choices, flushes the Win32 INI cache before locked
    readback, and leaves both old values intact under an injected commit
    failure.
  - Mixed-case `[Profiles]`/`[PROFILES]` headers match the production atomic
    section replacement helper; source guards require whole-file profile
    save/clear to retain the cross-session storage guard through the final write
    and forbid profile-clear buffer truncation from reaching the atomic replace.
  - Tagged combo item data round-trips personal and shared choices independently
    of list index; applied-slot mapping accepts only a service-owned personal
    slot, never shared/machine/ad-hoc ownership or live VF equality.
  - The shared GPU serializer/parser round-trips a `[profile2_gpu]` stable PCI
    identity including BDF, proving machine slots use the same schema as the
    per-user `[gpu]` selection.

- **Profile startup editor source (pure decisions, returns 722–726):**
  - Disabled, negative, and out-of-range app-start assignments select the live
    service snapshot for the editor.
  - A valid enabled app-start slot selects explicit saved profile intent.
  - Logon launches remain service-owned observers and suppress independent
    app-start profile handling.
  - Source/order guards forbid the former selected-slot restore and require the
    snapshot to be received before the complete GUI repaint.

- **Scheduled-task XML fixtures:**
  - Canonical XML with omitted or empty Task Scheduler defaults is accepted.
  - SID principals and quoted executable/config paths are parsed by the
    production classifier.
  - Valid delay, `HighestAvailable`, old `PT0S`, and omitted safe defaults are
    compatible legacy.
  - Explicit disablement, wrong user, stale executable/config, extra triggers
    or actions, `PT1S`, battery/idle gating, restart/repetition, and unsafe
    multiple-instance policies are broken.
  - Source guards pin the separate `--tray-start` HKCU Run entry and generation-
    checked UI readback so a bounded handoff cannot own the resident tray and a
    stale repair cannot overwrite a newer combo/task choice.

- **IPC sanitization:**
  - Power, GPU, memory, fan, and VF curve values clamp to protocol-safe bounds
  - Protocol bool flags use `gc_bool8` and are canonicalized to `0/1` for
    `DesiredSettings`, `ServiceSnapshot`, nested GPU adapter info, fan curves,
    and service responses
  - Memory offset currently clamps to `-3000..3000` MHz
  - Extreme edge cases: zero, negative, overflow values all clamp correctly
  - `lockMode` clamps to the tri-state range (999 → HARD, -5 → NONE) — relies on `enum LockMode : int`
  - ServiceRequest/ServiceResponse size invariants
  - SERVICE_PROTOCOL_MAGIC and VF_NUM_POINTS constants verified
  - Protocol v13 rejects zero service instance/revision/generation, invalid GPU
    phases, health reasons/sources/domain masks or validity-mask bits,
    noncanonical booleans, unterminated wire strings, nonzero reserved fields,
    partial mutation preconditions, malformed topology/identity, and a `READY`
    envelope missing required sections or fresh VF health
  - Valid numeric defaults (including zero) remain authoritative when their
    section bit is present and overwrite prior custom model values

- **Protocol-v24 wire compatibility (returns 81, 1243, 4521):**
  - Exact sizes for `ServiceRequest`, `ControlState`, `DesiredSettings`,
    `ServiceSnapshot`, and `ServiceResponse` are both static-asserted and
    regression-pinned at 1552/188/964/4248/7368 bytes (protocol 27; 1424/836/7112
    before `DesiredSettings.curvePointFromGpuOffset`), so a field addition
    cannot silently reuse the current version. v24 assigns one former reserved
    byte in every embedded `FanCurveConfig` to the independent zero-RPM fan-off
    gap; the fixed sizes therefore remain unchanged while the version bump
    prevents v23 peers from interpreting the byte with old semantics.
  - The immediately previous wire version is explicitly rejected at the fixed
    eight-byte prefix before either transport attempts a mismatched body read.

- **Diagnostics privacy (returns 4522–4532):**
  - Path/account/SID/LUID tokens are stable fingerprints, do not contain raw
    input, distinguish empty from populated values, and distinguish distinct
    inputs.

- **Updater worker recovery (returns 4527–4530):**
  - A failed check preserves READY only when staging, available decision,
    valid manifest, and fresh package match are all true.
  - A successful check keeps staged bytes only when the manifest is valid and a
    fresh hash matches; otherwise it discards them.

- **Client mutation preconditions (F-SYNC-STAMP, returns 2006–2020):** the
  producer-side half of the wire contract, which 1801-1805 does not reach.
  - `service_client_identity_adopt()` accepts only a coherent READY envelope
    (a recovering phase, a missing required section, a zero instance/generation/
    topology are all refused) and **clears** a previously adopted identity when
    it refuses, so a client that watched the service go away cannot keep
    stamping a dead generation.
  - Envelope → identity → stamped request → validator, end to end: the exact
    unstamped APPLY the synchronous path used to build is refused and
    `service_request_reject_reason()` names the missing preconditions; the same
    request validates after stamping. The RESET built by `--service-remove`
    travels the same two steps.
  - A refused stamp leaves the request unsendable rather than partly filled, so
    `service_client_mutation_is_stamped()` is a single sufficient transport gate.
  - `validate_service_response_for_ipc()` accepts an error response whose whole
    payload is absent — the answer a service gives a caller it refused before
    authorization — while a stateless `SERVICE_STATUS_OK`, a stray envelope
    field, a stray snapshot/desired/control field, a wrong version, and an
    unterminated message are all still refused.

- **Intended-vs-actual GPU readback (F-INTENT-READBACK, returns 1710–1718):**
  - The configured NVML offset slot is always P0.
  - Matching GPU/memory offsets, power, and fixed-fan policy/target produce a
    complete match; replacing only memory with zero reports only the memory
    domain as overridden and leaves configured intent unchanged.
  - A missing hardware read marks the domain unavailable/partial, never
    matching.
  - Selective GPU offset compares owned populated points only: excluded points
    are ignored, an applied 225 MHz point matches, and a reset point reports a
    225 MHz VF divergence.
  - Absolute VF targets tolerate up to 30 MHz of ordinary boost/temperature
    movement but disclose reset-sized departures.
  - No active intent yields no comparison rather than inventing ownership.
  - Source guards pin modern P0 reads/writes on both platforms, Linux modern
    snapshot capture with legacy fallback, visible TUI/export override status,
    copied `strerror()` diagnostics, and transition-only VF boundary logging.

- **Fan policy takeover (F-INTENT-READBACK-FAN, returns 1730–1734):**
  - An external controller putting the fans back on the automatic curve is an
    override, not an unknown. Windows only reads the target duty while a fan is
    manual, so requiring a target readback before reporting a policy takeover
    would have hidden exactly the case the feature exists to disclose; the
    comparator therefore checks policy first and only then demands a duty.
  - A matching policy with an unreadable duty is unavailable, never a match.
  - An unreadable policy leaves the whole domain unavailable.
  - An AUTO intent needs no target readback at all.

- **Windows readback provenance (F-WIN-READBACK, returns 1740–1751):**
  - Covers the Windows `ControlState` producer through the pure
    `source/control_readback_policy.h`. This is the producer side of the same
    contract the Linux daemon implements; before it existed the Windows service
    shipped all-zero validity while still substituting intent for failed reads.
  - One silent fan makes the whole fan domain unknown rather than selectively
    matching; no fans present is never a fan readback.
  - A power percentage derived from a missing default is arithmetic, not a
    reading.
  - Intent-derived GPU offsets are never published as readback.
  - `invalidate_scalar_readbacks()` clears the four scalar flags and leaves the
    per-fan block alone, because the fan read refreshes that wholesale.
  - `gpu_offset_readback_after_detection()` requires a populated curve on a
    global-offset backend or a successful Pstates20 read; neither means the
    published 0 is bookkeeping, not a reading.

- **Protocol-v14 reconnect and mutation state machines:**
  - Envelope layout and topology hashing cover nonzero per-process instance,
    monotonic revision/generation, explicit active-intent presence, telemetry-
    independent READY validity, every visible-map entry, stable GPU identity,
    adapter reorder, same-count/different-map topology, and valid `0 -> N`
    topology recovery.
  - Pure GUI reducer cases cover READY → removal → recovering/reapplying → READY,
    service restart plus late retired-instance responses, lower/equal revisions,
    generation changes, partial validity, local PnP generation fences, and tray/
    status/action decisions for every phase. The observed restart ordering is
    explicit: a fresh instance may be accepted in RECOVERING at generation 1
    before the GUI receives `DEVICEINSTANCESTARTED`; that late cue must not arm
    a generation-2 fence, and the same instance's later READY generation 1 must
    be accepted.
  - Draft reconciliation covers dirty preservation on exact GPU/topology,
    detachment on either mismatch, clean automatic rebase, and first coherent
    binding of an unkeyed startup/reconnect draft. Source/order guards also pin
    the pre-`READY` sparse-desired overlay rebase so omitted fields come from
    the accepted live baseline rather than startup defaults.
  - Bounded tracker cases cover first execution, duplicate in-progress and
    completed IDs, restart conversion to `OUTCOME_UNKNOWN`, and 16-entry
    eviction without a second backend write for a retained ID.
  - Pure coordinator queue cases cover Apply→Apply latest-wins, pending
    Apply→Reset, Reset precedence over later Apply, one active/one pending,
    mutation invalidation across GPU epochs, full-sync coalescing, and telemetry
    dropping behind writes/full sync.
  - Pure presentation cases cover mutually exclusive version-4 versus legacy
    tray activation callbacks, the tray menu's foreground-owner neutrality
    (1155-1158: the main window fails however it is styled, so does a visible,
    taskbar-listed or Alt-Tab-visible owner, and so does a `WS_EX_NOACTIVATE`
    one that could never hold the foreground the popup needs), durable
    hidden-intent suppression of unsolicited
    top-level show requests, postcondition correction when `WS_VISIBLE` changes
    without `SWP_SHOWWINDOW`, recursion suppression during corrective hiding,
    stable-telemetry data-only adoption, and the disconnected-failure render
    gate. An identical background failure must be
    inert; phase, live-authority, visible service-state, or displayed-error
    changes must render. Authority, active-intent, topology, forced-refresh, or
    missing-control changes promote an envelope to a full redraw-suppressed
    transaction. Source/order guards pin the whole READY projection inside one
    programmatic-edit transaction and require exact selected-GPU recovery to
    reassert tray residency. The native hidden-HWND fixture sends a real
    `ShowWindow(SW_SHOW)` and separately force-adds `WS_VISIBLE` without
    `SWP_SHOWWINDOW`; both paths must end hidden while intent is set, and the
    window becomes visible only after explicit activation clears it. The same
    fixture proves a raw hidden-owner `WM_SETREDRAW(FALSE/TRUE)` pair adds
    `WS_VISIBLE`, while the replacement transaction sends neither half, updates
    child state, defers painting, and remains hidden. A second native fixture
    (`run_native_visible_projection_taskbar_presence_test`) covers the other
    direction on a real off-screen *visible* top-level window: the pair clears
    `WS_VISIBLE` — the bit the shell tracks windows by, which is why the taskbar
    button vanished for the length of a Refresh — while the replacement
    projection leaves it set from beginning to end and still lands its control
    updates. Pure cases pin `gui_redraw_toggle_is_visibility_safe()` (only a
    child may be toggled), `gui_top_level_redraw_may_paint_synchronously()`, and
    `gui_window_invalidation_must_defer()`, plus
    `gui_service_resync_decide()`: a READY model with live authority re-read
    against the same GPU/service preserves its presentation, and anything else
    (identity change, non-READY, no live authority) transitions.
  - Source guards isolate tray/hotkey/automatic mutation completion in a
    presentation-silent handler. The background apply/completion operations may
    not call dialog/window creation or showing, activation/focus/flash, console,
    or child-process APIs, and hidden main-window invalidation may not request
    synchronous paint.
  - Source/order guards pin secure 64-bit ID generation, timeout-then-query of
    the original ID, Windows/Linux operation persistence, GUI-thread-only state
    adoption, immutable publication, precondition checks before and after the
    service runtime lock, response-time authority loss, pending session/GPU-
    epoch revalidation, coordinator shutdown cancellation, and diagnostics.
  - Hidden Win32 fixtures project fake envelopes through the GUI-state policy
    without hardware or sleeps, covering overlays, control/tray capability,
    same-count topology replacement, draft preservation/detachment, and one
    coherent redraw transaction. A separate top-down memory-DIB fixture proves
    select/blit behavior and source guards forbid `CreateCompatibleBitmap` for
    the retained backbuffer, physical-window clears before the completed blit,
    erase/frame invalidation during coherent state adoption, presentation-
    changing reconnect timer probes, unconditional service-control setters,
    and forced child `UpdateWindow` calls.

- **Unicode-safe Windows paths:**
  - A compiled fixture creates an INI path containing Chinese, Cyrillic, and an
    emoji outside the active ANSI code page; profile write/read,
    canonicalization, open, and delete must round-trip through strict UTF-16
    wrappers.
  - Malformed UTF-8 is rejected. Source guards forbid path-taking ANSI Win32
    calls in production source and require ordinal UTF-16 containment checks.

- **Lock mode persistence:**
  - `lock_mode` round-trips through the profile INI (HARD/FLATTEN write → read)
  - Linux also round-trips `lock_tracks_anchor`, merges `lockMode`, migrates
    legacy/missing or contradictory enabled-`NONE` lock modes to `FLATTEN`, and
    preserves explicit `HARD`. Source checks pin both keys at save/load.
  - Source checks assert `merge_desired_settings`/`capture_gui_config_settings`
    carry `lockMode` and `lockTracksAnchor`, the right-click lock menu, and the
    lock tooltip.

- **VF lock checkbox input policy (compiled, returns 624–635):**
  - Pure transition tests prove new point → FLATTEN, FLATTEN → HARD, and HARD → NONE.
  - Notification tests prove `BN_SETFOCUS` and owner-draw `BN_DBLCLK` are inert; unarmed `BN_CLICKED` remains available to accessibility/`BM_CLICK`.
  - Armed gestures accept exactly once, reject a duplicate, reject a command for another control, and reject an exact lock-model stamp change between press and release.
  - A real hidden Win32 `BS_OWNERDRAW | WS_TABSTOP` button fixture observes exactly one `BN_CLICKED` from `BM_CLICK` and the distinct `BN_DBLCLK` notification from `WM_LBUTTONDBLCLK`; it does not touch GPU hardware. Since 2026-08-01 it also drives the full DOWN/UP/DBLCLK/UP sequence and pins that a fast double-click is `BN_CLICKED` + `BN_DBLCLK` with no second `BN_CLICKED` — the fact that made the installer's click-only filter swallow fast double-clicks.

- **Overclock range hints and the high-overclock confirmation (pure, F-OC-HINT /
  F-OC-WARN, returns 1400–1435):**
  - Driver windows wider than the IPC caps are clipped to `+/-1000` MHz (GPU) and
    `+/-3000` MHz (memory); a narrower window is reported verbatim.
  - An unreported or inverted driver window reports *unknown* rather than a
    fabricated interval, so the caption never invents a limit.
  - Power percent rounds the minimum up and the maximum down, clips to the
    enforced `50..150` gate, and falls back to that gate when the driver reports
    no constraints or a zero default (no divide-by-zero).
  - The caption stays ASCII (the GDI window writes through the ANSI entry
    points), fits its buffer, keeps the memory *advisory* marker, and every
    field tooltip stays NUL-terminated when given a deliberately tiny buffer.
  - Warn cases pin the threshold boundary in both directions (199/200,
    1999/2000), that lowering an already-running high clock is silent while
    raising past the applied value warns, the per-field profile-load exemption,
    a request that carries no offset at all, per-domain threshold disabling via
    `0`, the single message naming both domains, and that a non-warning decision
    produces an empty message so no caller can show a blank dialog.

- **Outcome severity, wire side (pure, protocol v18, returns 2290-2304 and
  2330-2333):**
  - `validate_service_response_for_ipc()` accepts `OK` + `SUCCESS` and
    `OK` + `WARNING`, and rejects `OK` + `ERROR`, an out-of-range severity, and
    a nonzero `outcomeSeverityReserved`. The check runs *before* the
    payload-free refusal shortcut, so a stateless refusal claiming a clean
    outcome is rejected too (assertion inside the 2017 refusal suite).
  - `service_response_resolve_outcome_severity()` is exhaustive over every
    status × recorded-severity pair: non-OK always resolves to `ERROR`, OK
    resolves to `SUCCESS` or `WARNING` and never to `ERROR`. The suite also
    asserts it is **idempotent** and that its output always satisfies
    `service_outcome_severity_matches_status()` — the mutation guard resolves
    before persisting and the write-out stamp resolves again over the same
    response, so a non-idempotent rule would corrupt one of the two.
  - `service_apply_outcome_severity()`: a failed step outranks everything, and
    a committed-but-unmatched boost/flatten point is a `WARNING` rather than a
    silent success.
  - `service_apply_outcome_severity_for_lock_mode()` (2330-2333): a hard NVML
    pin ignores VF tail readback (NVML itself verifies min=max), so tail
    mismatches stay `SUCCESS`; boost-region partials still warn and failed
    steps still error. Without the hard-pin flag the same tail count warns.
  - The operation tracker (1010-1012, 1034-1035) records and replays severity:
    a completed warning stays a warning for a deduplicated retry, and a caller
    handing in a severity the status cannot carry gets the resolved one.
- **Manual Apply/Reset result presentation (pure, F-RESULT, returns
  2305-2326):**
  - `gui_mutation_result_severity()`: a result the window could not fully adopt
    is an `ERROR` whatever the envelope claims, **including** the zeroed
    response a transport failure leaves behind — that response carries the
    `SUCCESS` value by default, so this rule, not the field, is what stops a
    lost transport from reading as a silent success.
  - `gui_mutation_result_needs_prompt()`: `SUCCESS` alone raises nothing.
  - The profile label reuses the tray vocabulary (`Profile 3`,
    `Shared profile 2`, `Machine profile 1`, `Manual settings`), and a personal
    slot outside the configured range degrades to `Manual settings` rather than
    announcing a slot that does not exist.
  - Status wording per kind × severity, including that a reset names no profile
    and that an empty label still yields a sentence rather than a leading
    space; plus the queued line, bounded truncation, and null-safety.

- **Themed message box (pure, F-THEMED-DIALOG, returns 1440-1471):**
  - Every stock button set is reproduced with the right ids and order, and an
    unknown type degrades to a plain OK box rather than to zero buttons.
  - Escape/close resolves to the safest available answer: `Cancel` where one
    exists, `No` for Yes/No. Dismissing a confirmation can never mean yes.
  - `MB_DEFBUTTON2` / `MB_DEFBUTTON3` select the right button, and a default
    index past the end of the set falls back to the first rather than indexing
    off the array. Icon flags never change which buttons appear.
  - Geometry: the icon gutter appears only with an icon, the button group is
    right-aligned inside the margin, a wide button row widens the dialog rather
    than pushing buttons off the left edge, margins scale with DPI while the
    caller-supplied icon size is used verbatim, and a null input yields an
    empty plan instead of a crash.

- **Unapplied-change presentation (pure, F-PENDING, returns 1480-1549 and
  4000-4004):**
  - Scalars: equal is unchanged, different is changed, and re-typing the applied
    value is *not* a change (this is what replaces the old "No changes to apply"
    error box). Negative offsets and a zero baseline are ordinary values.
  - **The anti-lockout rule, pinned twice.** Unparseable draft text always counts
    as a pending change, both as a single case and as a Cartesian sweep over
    `{-5000,-1,0,1,100,5000}²`: `gui_pending_scalar_changed` never returns false
    for invalid input, so a half-typed value can never grey out Apply and strand
    the user before the real validation error.
  - Curve points: changed when the editor owns the point, the driver reported
    it, and the value differs from the drift-free applied baseline. Not visible
    / not owned by either side / no live readback all report unchanged. An
    owned point with **no** applied baseline is changed (a freshly typed or
    freshly loaded point). New ownership-release cases (4024-4026) pin the
    inverse direction too: a point the APPLIED state owns but the editor does
    not is a pending change, because reset-before-apply releases it back to
    stock -- even when the stale draft still holds the old applied value. The
    ownership gate still wins over invalid text when neither side owns the
    point, so stale draft contents cannot manufacture a change.
  - Lock: added, removed, moved to another point, retargeted to another MHz, and
    FLATTEN↔HARD all register; an identical lock does not; two inactive locks
    are equal no matter what their stale ci/MHz/mode fields hold.
  - Summary: an empty summary (and a null one) disables Apply; each of the nine
    domain bits independently enables it; `mutationReady == false` disables it
    regardless of the mask, so pending changes only ever *remove* the button.
    The nine bits are asserted pairwise distinct so one domain cannot mask
    another. `gui_pending_summary_equal` distinguishes both a changed mask and a
    changed point count, which is what keeps the refresh from repainting on a
    stable telemetry tick.
  - Lock frequency tolerance (1580-1589): a requested 2957 against a settled 2962
    is not a change at a tolerance of 8, symmetrically in both directions;
    exactly at the tolerance is on target and one beyond is a change; a zero
    tolerance keeps exact behaviour (what an unpopulated point reports); the
    tolerance covers the frequency ONLY, so a moved anchor, a changed mode, and
    adding or removing a lock all still register.
  - Lock-target resolution (1552-1557): the draft value at the anchor wins over
    `g_app.lockedFreq` whenever a draft is attached and the anchor is valid, and
    only then; it wins even when it is the *smaller* value (a preference for the
    fresher source, not the higher clock); a valid zero anchor still wins, so
    callers keep their own "is there a lock at all" gate.
  - Graph run splitting (1520-1537): no changed points yields no run at all; an
    interior run expands by one neighbour on each side; runs at the array edges
    do not expand past the ends; two separate stretches are reported separately
    and a contiguous stretch as one run; an all-changed array yields exactly one
    spanning run (the case the original full-length draw was accidentally right
    about); and null / zero-count / negative / past-the-end inputs terminate
    instead of walking off the array or looping.
  - Global GPU-offset shift (1540-1549): a point whose offset component moves is
    pending in both directions; equal components (excluded on both sides, or an
    exclude-count change past this point) are not; under a **uniform** offset an
    editor-owned point is exempt because Apply writes it as an absolute target,
    while under a **selective** offset (`selectiveOffsetActive`, either side
    excludes low points) the curve batch re-places owned points too and they
    stay pending (new cases 4000-4004); the locked tail is exempt because the
    lock pins it, and the tail rule wins even for a point that is also owned;
    negative offsets are ordinary values. New pure cases 4010-4016 pin the
    selective-mode mirror itself: `gui_pending_offset_mode_selective()` treats
    the applied exclude count as already effective (matching the backend's
    resolver read, including the remembered-request edge) and normalizes the
    pending side like the backend's desired settings.
  - Graph preview record (3195-3207, 4006-4009, 4027-4030):
    `gui_graph_preview_point()` resolves the editor half of a plotted point in
    the apply path's order -- locked tail, then a moved global-offset
    component on an unowned point (or on an owned point when
    `offsetMovesOwnedPoints` is set for a selective offset, case 4005), then
    release-to-stock, then the draft value -- with an unresolved lock target
    of 0 falling through rather than plotting 0 MHz, an owned point exempt
    from a *uniform* offset (it is written absolute), the tail outranking the
    offset, and an unmoved component claiming nothing so live readback stands.
    New hard-pin cases (4006-4009) pin that `hardPinned` outranks draft and
    offset and answers the lock target for a pre-anchor point too. The
    ownership-release cases (4027-4030) pin that an applied-owned point the
    editor releases projects from the stock base even when the stale draft
    still holds the old applied value, carries the pending offset component
    when an offset projection is valid, is not used for an owned point, and
    yields to a pending hard pin.
    **The regression the repaint gate missed** is pinned three ways, all cases
    where the changed-point *set* is identical but the plotted *value* moved:
    the global offset retyped +100 -> +150, an already-pending point retyped
    2900 -> 2950, and a lock retargeted over the tail. Each must compare
    unequal. Equality is also asserted to ignore the struct padding, so the
    gate cannot fire at random.
  - Graph axis suite (3215-3227, 4020-4023): `gui_graph_frequency_axis()` /
    `gui_graph_voltage_axis()` fallback bands, high/low data sizing, minimum
    spans, floor clamps, single-point sizing, cap behaviour, and the true
    floor/ceil grid rounding helpers (`gui_grid_floor()` / `gui_grid_ceil()`)
    including negative pre-clamp values.

- **Control actionability (pure, F-ACTIONABLE, returns 1600-1632):** a table over
  eight service/draft states × every capability.
  - The escape-hatch invariant: `RECOVERY` is true in *every* state, including
    not-installed. Refresh and the service checkbox are the only ways out.
  - The clearable-when-set rule:
    `gui_service_config_control_actionable(state, false)` tracks reachability
    exactly, while `(state, true)` is true in every state. That second half is
    what keeps a logon assignment removable — it holds a Task Scheduler entry
    open, and `clear_profile_from_config()` preserves `logon_shared_slot`, so
    deleting the profile is not an escape.
  - `PROFILE_EDIT` / `AUTOMATION` exactly `installed && available &&
    !toggleInFlight`; `EDITOR` exactly `ready && attached && !detached`;
    `HARDWARE_MUTATION` = `EDITOR && loaded`, and it implies `EDITOR` in every
    state. The two helpers the imperative code calls directly are checked against
    the same table.
  - The reported bug: not-installed denies the whole profile row (Load, Save,
    Clear, slot combo, shared picker, Auto-Profiles) while recovery stays live and
    an already-set assignment stays clearable. The deliberate asymmetry:
    available-but-SYNCING **keeps** profile editing (the pre-READY overlay is
    rebased later) while `EDITOR` is false. An in-flight uninstall denies profile
    editing even though both service flags still read true.
  - Null input denies rather than crashing a projection pass. There is
    deliberately no out-of-range-capability case: loading a value outside the enum
    is itself UB and the sanitizer build rejects it.
  - 1620-1622 pin the property the re-gate coverage silently rests on —
    `gui_service_model_disconnect()` zeroes `topologySignature`, so the first
    envelope after any disconnect always forces a full render and can never be
    mistaken for stable telemetry.

- **Tray active-profile label (pure, returns 1560-1570):** `applied_slot` wins
  outright; shared and machine banks are named explicitly and never folded into
  the same-numbered personal slot; a hand-typed Apply and a user slot whose
  saved intent no longer matches both read "Manual settings"; nothing active
  reads "No profile"; out-of-range personal and source slots never fabricate a
  name; a tiny buffer truncates but stays NUL-terminated and a null one is
  ignored rather than crashing the tray update.

- **Logon auto-apply policy (pure decisions):**
  - `resolve_logon_profile_source()` — shared-bank / per-user / machine-default /
    none resolution incl. the restricted-user shared-only rule
  - A task-only lifecycle event authorizes one fake write after prerequisites;
    WTS plus task handoff coalesces to one write for
    `{session,SID,authentication LUID}`. Profile-source resolution is tested
    separately; the harness is not a full WTS/token/config/pipe integration test.
  - Logoff cancels pending intent; the same session/SID with a new
    authentication LUID is a new login.
  - A zero-initialized ordinary-start reducer state and driver recovery without
    validated controlled-recovery capability authorize zero fake writes.
    Stale/corrupt snapshots, wrong nonce, helper failure, SCM start-reason
    validation, and Task Manager behavior are pinned by source/order guards;
    they are not executable SCM/helper integration fixtures.
  - Controlled-restart SCM transition policy has executable pure coverage:
    `STOPPED` proceeds; `STOP_PENDING` accepts the API-documented zero/stale PID;
    another non-stopped generation or a missing expected parent identity is
    rejected. Actual SCM notification/start calls remain source/order guarded.
  - Null/global devnode events are readiness cues only and cannot authorize a
    write.
  - Operation-scoped source guards require shared/machine-default sources to
    bind the published slot GPU and require per-user GPU config only after the
    resolver actually chooses a personal profile. Restricted manual applies
    must replace both client settings and target with the authoritative bank
    copy.

- **Auto-profiles (pure, F-AUTO-PROFILE, returns 220–272):**
  - `resolve_auto_profile_slot()` — exe/title/class/fullscreen matching,
    ordered first-match-wins, `require_focus` honored (running-but-not-foreground
    exe does not match a focus-required rule; focus-optional matches on presence),
    default-slot fallback, `auto_profile_text_contains_ci`, match-type name
    round-trip
  - Controller state machine — coalescing (A→B→A within debounce = no B apply;
    sustained B = one apply after debounce), cooldown defers until
    `minSwitchIntervalMs`, suppression, manual-pin (hotkey N → pin+apply, same N
    again → resume auto, different slot → repin), enable/disable revert-to-default
  - **Enable is a transition, not a value (253–255, 268–272).** A pick while auto
    is disabled applies without recording a pin; enabling afterwards through
    `ap_apply_config_change()` (the configuration dialog's path) returns
    `RESUME_AUTO` and the controller drives again; a pin taken while enabled does
    not survive a dialog disable→enable; an unchanged enable adopts edited values
    (default slot/debounce/cooldown) while preserving a deliberate pin. Both
    assertions were verified to fail (253 and 254) against a pre-fix controller
    compiled in a scratch include path.
  - `auto_profile_config_load/save` round-trip through the INI helpers
  - `hotkey_parse`/`hotkey_format` — `ctrl+alt+f2` round-trip, case-insensitive,
    reject no-key / unknown token, bare key parses (dialog rejects modifier-less)
  - Harness `#include`s `auto_profile_rules.cpp`, `auto_profile_controller.cpp`,
    `hotkeys.cpp`
  - Source guards: `F-NO-INJECT` (no `WriteProcessMemory`/`CreateRemoteThread`/
    `VirtualAllocEx` **and no `OpenProcess`** in the detect/driver — the foreground
    exe name comes from `CreateToolhelp32Snapshot`; hook is `WINEVENT_OUTOFCONTEXT`)
    and `F-AUTO-PROFILE` wiring (init/shutdown/WM_HOTKEY/unity include)

- **Security regressions:**
  - Profile repair handles a saved `-2147483648` offset under UBSan (guards the
    former `abs(INT_MIN)` overflow)
  - Elevated-helper argument quoting round-trips through `CommandLineToArgvW`
  - Bounded formatted appends preserve earlier fields and saturate at the final
    writable byte; source guards forbid treating `StringCchPrintf*` HRESULTs as
    character counts in crash breadcrumbs and GPU apply summaries
  - The subprocess fixture emits more data than its capture buffer and must be
    drained to a clean zero exit; a second child exits 7 and must be reported as
    failure on both the Windows and POSIX implementations
  - Controlled-restart process/handle arguments are source-guarded as nonzero,
    digits-only decimal values with C-library overflow rejection
  - Service binary, service install directory, and machine shared-bank DACL
    helpers produce protected DACLs in temp-path tests
  - Build-script cache verification rejects a modified binary even when its
    adjacent `.sha256` marker was changed to match the modified bytes

- **Linux TUI layout (F-LNX-TUI):**
  - All layout shards are built headlessly into the exact terminal cell grid.
    A matrix of 72x24, compact, medium, wide, tall, and ultrawide dimensions is
    exercised across VF, Fan, and Profiles tabs.
  - Every layout has exactly `width * height` cells; every action is in bounds,
    pairwise non-overlapping, and present on the expected tab. Apply, Reset GPU,
    Quit, and all tab selectors remain reachable. 71x23 fails closed with no
    actions.
  - Pure VF composition checks excluded low points, global GPU offset, flatten
    knee/tail, absolute target, and hard-pin rules from base/live/desired state.
    Graph selection resolves the nearest populated voltage point.
  - `°` counts as one display column (`tui_display_columns` /
    `tui_column_to_byte_offset`) and the same cell grid drives mouse hit testing.
  - **VF graph axes (2069-2081).** Asserted against the rendered *cells*, not
    the drawing code, so a layout change that pushes a label out of the panel
    fails here rather than on someone's terminal: the row under the plot starts
    at the lowest voltage and carries `<max> mV`, the left margin holds a
    descending MHz scale, the units are present at every size that draws a
    graph, and a tick names the same point a click on that column selects.
    Negative controls run: removing the X axis exits 2071, the Y axis 2073.
  - **Numeric-field editing (2082-2099, 2206-2209).** The pure rule in
    `linux_tui_edit_policy.h`: first keystroke replaces a pre-selected value,
    the selection is consumed by that one character, a second click into the
    open field makes typing append, a rejected keystroke changes nothing, a
    sign is accepted only in the first position, backspace over a selection
    clears the whole value, and a full buffer refuses input. Negative controls
    run: reverting to unconditional append exits 2083, and clearing the
    selection on a *rejected* keystroke exits 2094.
    A truncated three-byte UTF-8 lead is rendered as one byte without consuming
    bytes beyond the NUL terminator.
  - Source guards pin separate wheel handling, left-button-only activation,
    spatial keyboard navigation, changed-row rendering, and generation/topology-
    checked Apply/Reset.

- **Linux terminal selection (F-LNX-TERM, returns 1810–1822):**
  - `linux_terminal_select()` against a fake PATH probe: KDE picks konsole, a
    colon-separated `ubuntu:GNOME` still matches its real token, an absent
    desktop-preferred terminal falls through to the generic list, an unknown
    desktop uses the generic list, and an empty system fails closed with no
    command rather than one `execvp()` would reject.
  - `linux_terminal_style_prefix()` returns the exact fixed argv words per style
    (0 for bare, `-x`, `start --`), because the launcher passes a vector and
    never a shell string.
  - `linux_terminal_should_relaunch()`: a real terminal, a headless session and
    an already-relaunched process all decline; **one** redirected stream is
    still a terminal launch, so `greencurve --tui > log` is not hijacked.

- **Linux debug log (F-LNX-DEBUGLOG, returns 1830–1837):**
  - Path resolution: client logs land beside `config.ini`, the daemon lands in
    its state directory, and the daemon role without a state directory fails
    rather than silently writing to the working directory.
  - `linux_debug_log_enabled_for()` mirrors Windows: on by default, `[debug]
    enabled=0` disables, `GREEN_CURVE_DEBUG=0` beats the config, any other env
    value forces it on.

- **Linux startup-apply policy (F-LNX-STARTUP, returns 1840–1859):**
  - Record round trip; a `none` record provably carries no slot, GPU, name or
    settings; a `profile` record without an exact GPU is rejected; out-of-range
    slot, unknown mode, wrong size and a single tampered byte are all rejected
    by the bounds/checksum pair.
  - Wire request: `profile` requires a slot and an exact GPU, and the other
    modes must carry neither, so a stale target cannot ride along on a request
    that ignores it. `startupMode` must be zero on every other command.
  - Envelope coherence: a published slot is valid **only** with the `profile`
    mode, in both directions. The block first asserts a minimally valid envelope
    so the negative cases cannot pass for the wrong reason.

- **Boot-apply snapshot stays equal to its profile (F-LNX-STARTUP-SYNC, returns
  1970–2005):** the reported failure was a 4 °C hysteresis and a 2000 ms poll
  interval that reverted to the 2 °C/1000 ms defaults after every reboot,
  because `profile 1` boot-apply replayed the snapshot captured when the policy
  was bound. The fixture is exactly that pair, and every assertion fails on the
  pre-fix code.
  - `startup_snapshot_sync_after_profile_write()`: writing the bound slot owes a
    refresh; another slot and the non-`profile` modes owe nothing; clearing the
    bound slot unbinds instead of leaving a deleted profile booting; an offline
    daemon yields `UNREACHABLE` rather than a silent success; out-of-range slots
    are never a binding.
  - `startup_snapshot_state_for()`: the reported pair reads as `DIVERGED`, equal
    settings as `IN_SYNC`, a snapshot that was never read back as `UNKNOWN` (it
    must not claim agreement it did not check), an unloadable slot as
    `PROFILE_MISSING`, and a non-`profile` policy as `NOT_APPLICABLE`.
  - `startup_snapshot_refresh_allowed()`: only a stored `profile` policy naming
    the same slot may be refreshed, so the command cannot create a boot-apply or
    move one to another slot.
  - Record identity across a refresh: mode, slot, name and the exact GPU binding
    are unchanged while only the settings advance, and the refreshed record then
    compares `IN_SYNC` with the profile.
  - `validate_desired_settings_for_ipc()` leaves a normalized curve untouched —
    if it did not, a refreshed snapshot could never compare equal to the profile
    the client just saved.
  - Wire contract for `SERVICE_CMD_REFRESH_STARTUP_PROFILE`: a slot is required
    and bounded; a `targetGpu`, a `startupMode`, an operation id or live-state
    preconditions are all rejected; and the command range still ends at this
    command.
  - **2026-2035 — how the snapshot reaches the client.** Everything above
    asserts the *decisions* with hand-supplied snapshots, and all of it passed
    while the product was broken: v15 returned the snapshot in `desired`, the
    member the daemon rewrites with the active intent on every response, so the
    detector compared the applied settings against the profile. This is the
    carriage half the suite was missing, and it is the same lesson as
    1801-1805 — *a decision suite with no counterpart for how the inputs
    arrive proves nothing.*
    - The bug's exact shape: one response holding a different active intent and
      boot snapshot, where reading `startupProfile` says `IN_SYNC` and reading
      `desired` says `DIVERGED`. The two members must stay independent storage.
    - Coherence at the trust boundary: a published snapshot is rejected unless
      the envelope's policy really is `profile N` with a nonzero slot; an unset
      flag must mean byte-zero settings; the flag is a boolean and the reserved
      bytes must be zero.
    - A response carrying no snapshot stays valid, and a stateless refusal is
      still recognized as payload-free now that the member exists.
    - Source-gated in `tools/linux_gates.py`: `startupProfile` exists,
      `daemon_publish_startup_snapshot()` is called, and neither
      `resp->desired = g_startupPolicy.desired` nor
      `report.snapshot = policy.desired` may come back.

- **Linux mem stored-unit migration (F-MEM-MIGRATION, returns 5000–5017):**
  INI bank conversion over all five slots + legacy `[controls]` mirror
  (+2500→+1250, -2501→-1250 trunc-toward-zero, +5000→+2500, +1→0, 0→0
  no-rewrite count, unrelated sections preserved), idempotence on the marked
  bank, a pre-marked display bank keeping large values, hand-edited garbage
  left for the strict parse, empty docs untouched, and halve-THEN-clamp
  ordering (+5000 effective → +2500 display survives the IPC clamp).
  Source-gated in `tools/linux_gates.py::check_mem_offset_migration`; the
  Windows surfaces are forbidden from learning the `linux_mem_migrated` marker.

- **On-disk DesiredSettings layouts (F-PERSIST-SCHEMA, returns 5018–5057),
  in `run_persistence_schema_tests()`:** byte-exact fixtures of what 0.25.2
  really wrote, built through `desired_settings_narrow_to_schema1()` — the
  frozen sizes themselves (836 / 1048 / 1036 / 1108 / 1032) are assertions,
  because the suite this replaced constructed the CURRENT structs and merely
  relabelled their version numbers, which is how a layout change reached a
  release candidate unnoticed. Covers: schema-1 startup v1 (halve once) and v2
  (do NOT halve again), curve points and target GPU surviving the widening,
  provenance widening to zero, mode=NONE staying a write-nothing record, tamper
  refusal, a version the layout never carried being refused, the same for the
  state record at v2/v3, the narrower v1 state record (no operation identity,
  halved once), and the schema-1 round trip losing provenance but keeping every
  number. **Verified failing (5030) when the widening forges provenance.** The
  block is its own function because these records overflow `main()`'s frame
  under ASan. Source-gated in `tools/persistence_gates.py::check_all`.

- **Saved-curve provenance (F-CURVE-PROVENANCE, returns 5060–5067 and
  5070–5089):** the platform-neutral half asserts
  `profile_curve_decode_from_marker()` over every marker including the
  downgrade-safety rule (an unknown marker reads as ABSOLUTE, never as
  base+offset — reading it as base+offset would write the curve high by the
  whole GPU offset on a machine that just rolled back). The Windows half writes
  a real INI and reads it back through
  `restore_curve_point_origins_from_section()`: a MIXED section (points 70 and
  72 projected, 71 hand-typed) must come back with exactly those three answers,
  every flag pre-set to 1 first so a reader that never writes the array cannot
  pass by accident, a point with no value never picking up provenance from a
  stale key, and the legacy/unknown/absent markers each routing elsewhere.
  **Verified failing (5077) with the per-point read disabled.** Source-gated in
  `tools/persistence_gates.py::check_profile_curve_format`.

- **Curve-provenance wire validation (returns 2016–2018):** the strict
  pre-canonicalization pass must refuse `curvePointFromGpuOffset[i] > 1` with
  "non-boolean desired settings flags", and the test also pins WHY the omission
  was invisible: canonicalization folds the value to 1 and the same request then
  validates. **Verified failing (2016) with the check removed.**

- **Linux VF/fan scroll clamp (F-LNX-VFSCROLL, returns 1860–1894):**
  - `tui_vf_max_first_visible()` over a 40-point table: 10 rows stop at 30, one
    row stops at 39, a list that fits gives 0, and a degenerate zero-row layout
    gives 0 rather than a negative offset.
  - Holes in the table are skipped when drawing, so they must not count toward a
    page either — punching five out moves the last page five entries back.
  - No populated points (daemon offline) pins scrolling to 0.
  - `tui_selection_needs_reveal()`: the first frame reveals (`revealedPoint`
    starts at -1), a scroll that moved the view away from an already-revealed
    selection does **not** re-reveal, a genuine selection change does, and an
    absent selection never reveals. This is the rule that stopped the wheel from
    snapping back — clamping alone was not enough, because two other paths
    (the per-frame reveal and the 1 Hz refresh clamp) were also dragging the
    offset back to the selected point.
  - `tui_vf_reveal_first_visible()`: the selection lands `visibleRows / 4`
    populated points from the top (48 rows/point 76 -> 64; 16 rows -> 72), is
    clamped to 0 near the start of the list and to the last page near its end
    (point 126, 16 rows -> 111), and degenerate inputs (no selection, zero
    rows, a one-row table) never produce an out-of-range offset. Bottom-
    aligning it was what filled a tall terminal with the 45 identical 180 MHz
    points of the driver's low-voltage floor.
  - `tui_vf_first_listed_point()` / `tui_vf_hidden_low_point_count()`: a 45-point
    180 MHz floor followed by a rising curve yields bound 45 and hidden 45, and
    neither scroll helper can walk back below it. A selection inside the floor
    lowers the bound to the selection. An ordinary rising curve (leading run of
    one) and a run shorter than `TUI_VF_MIN_FLAT_FLOOR_RUN` are left untouched,
    a wholly flat curve does not produce an empty table, and an offline view
    model yields no bound and no hidden count.

- **Fan failure escalation and daemon lifecycle (returns 1300–1339):**
  - `fan_runtime_observe_result()`: success resets the counter; failures 1 and 2
    continue; failure 3 requests auto restore. A failed **temperature read**
    walks the identical ladder as a failed write, and the two share one counter.
    A zero limit falls back to `FAN_RUNTIME_DEFAULT_FAILURE_LIMIT` instead of
    escalating on the first failure, and the counter saturates at `UINT_MAX`
    rather than wrapping back into an apparently healthy state.
  - `fan_runtime_escalation_after_auto_restore()`: a successful handback ends
    escalation; a refused handback demands `EMERGENCY_MAX` (100%).
  - `daemon_accept_disposition()` / `daemon_accept_error_is_fatal()`:
    `EINTR`/`ECONNABORTED`/`EAGAIN`/`EPROTO`/`ENOBUFS`/`ENOMEM`/`EPERM` retry,
    `EMFILE`/`ENFILE` reclaim a reserved descriptor, and
    `EBADF`/`EINVAL`/`ENOTSOCK`/`EOPNOTSUPP` are fatal.
  - `hotkey_parse()`: `f24` accepts and `f25`/`f0` reject; a 21-digit function
    key and `ctrl+f2147483648` reject *without* the former signed-overflow UB
    (the old accumulator tripped `-fno-sanitize-recover`); a 200-character
    input is rejected rather than silently truncated into a different valid
    binding; ordinary `ctrl+alt+f2` and `f1` still parse.
  - Source guards forbid the realtime-clock token in `linux_fan_runtime.h`,
    require `pthread_condattr_setclock`/`CLOCK_MONOTONIC` in
    `linux_daemon_lifecycle.h`, require `sigaction(SIGTERM`/`sigaction(SIGINT`,
    require the fan worker to be joined *before* `daemon_release_fan_to_driver()`,
    forbid the old `if (errno == EINTR) continue; break;` accept form, and
    require a non-zero fatal exit so `Restart=on-failure` applies.
  - The native fixture adds a real listener with one silent peer and one active
    peer: `poll()` must report the second connection immediately while the
    first is still unread, the second must complete a full response exchange,
    and a drained listener's `EAGAIN` must classify as non-fatal. No sleeps.
  - On non-Linux hosts the fixture is **cross-LINKED** to a real
    `x86_64-linux-gnu` ELF with warnings-as-errors (it cannot run without Linux
    filesystem sockets), so a break in it can no longer stay invisible until
    someone tests on Linux. It was only cross-*compiled* (`-c`, object only)
    until 2026-09-15; a compile cannot see an undefined symbol, which is exactly
    how a header-inline call into an unlinked shard passed a Windows host and
    failed only the Linux CI job. See "Linux fixture link lines" below.

- **Linux daemon access onboarding:** source guards require the successful
  installer output and Unix-socket `EACCES`/`EPERM` diagnostic to instruct the
  user to join `greencurve`; the TUI may not replace that detail with a false
  "daemon not running" message.

- **Linux mutation/fan/boundary policies:**
  - Pure curve-target matrices cover truly global offset, selective offset,
    populated-point low exclusion (including sparse input), explicit-point
    precedence, flatten tail, and hard-lock tail.
  - VF replacement matrices cover previous-versus-committed ownership: an
    omitted previously owned point receives a zero-offset cleanup write, while
    a point still owned by a composed policy receives that policy's final
    target rather than being cleared (2051-2053).
  - Transaction fixtures retain ordered phase verification and rollback;
    source guards keep unsupported-family writes enabled by default.
  - Pure fan reducer cases cover initial interpolation, configured interval,
    immediate rising demand, held/accepted falling hysteresis boundaries, and
    forced refresh. Source guards require condition-variable wakeup on desired
    state/reset/shutdown and repeated-failure return to automatic control.
  - Native zero-RPM cases (4720-4755) pin strict reserved-byte/flag validation,
    wire canonicalization, first-enabled-point threshold discovery, the
    independent 2-30C anti-cycle fan-off gap, automatic below 18C, manual start
    at 30C, history-preserving behavior inside that band, forced reassert,
    disabled-feature compatibility, summaries/equality, v23 profile migration,
    and temperature-aware readback. The same fixture proves that a 12C
    zero-RPM gap does not delay an ordinary curve downshift configured for 1C.
    Outside the zero-RPM band the wrong policy is override, and missing
    temperature is unavailable. A wide fan-tab layout must expose distinct
    `Curve downshift` and `Off gap` controls plus the computed thresholds. The
    Linux CLI cases also pin field-masked gap-only overrides, partial-override
    preservation, and transactional rejection of a duplicate-temperature edit;
    the Windows geometry test pins themed controls and explicit, unclipped
    `Fan ON`/`Fan OFF` lines. The curve-summary regression requires
    `zero-RPM enabled` plus `fan stop` wording and rejects the ambiguous
    `zero-RPM off` phrase used by the main-window button previously.
  - `tools/fan_gates.py` additionally pins both platform automatic-policy
    handoffs, the Linux initial Apply path, GUI/TUI/CLI/profile carriage, and
    forbids manual 0% as a zero-RPM implementation.
  - Socket guards require restrictive umask plus descriptor owner/group/type/
    mode verification and root-only fallback. TUI guards require the parent
    `waitpid` supervisor and forbid stdio/terminal work in fatal handlers.
  - Exact GPU identity tests require an active intent to retain its owning
    adapter, and source guards pin the daemon switch rejection plus the fan
    worker's independent owner check. Telemetry fallback is not published as a
    selected identity. Pure first-selection cases cover forward/backward
    endpoints, wraparound, and an empty adapter list.
  - Linux profile clearing tests/policy checks clear selected, applied, app-
    launch, and logon references to the removed slot.

- **Linux daemon upgrade, health, and recovery:**
  - Injected install-runner cases pin `daemon-reload -> enable -> restart ->
    is-active -> filesystem socket authorization -> protocol/build verify`,
    reject every command failure, forbid `enable --now`, and validate the
    bounded dependency-free `READY=1` payload.
  - Header-first transport policy classifies old short requests/responses,
    same-version truncation, timeout, and EOF with byte counts. Permission
    formatting covers socket owner/group/mode plus primary and supplementary
    group membership, and TUI diagnostics retain the first actionable error.
  - Native Linux builds additionally run `tests/linux_transport_regression.cpp`
    against `socketpair()` for real old-version, truncation, timeout, EOF, and
    response-write behavior without sleeps. The fixture also proves the Linux
    root cause where `fchmod(socketFd)` changes only the sockfs descriptor inode
    while the bound pathname remains `0700`, then verifies relative pathname
    configuration/inspection produces the required mode.
  - Native Linux builds also run `tests/linux_crash_report_regression.cpp`,
    which asserts the crash artifact's create-on-crash invariant from both
    sides: arming, re-arming and closing leave the directory empty and an empty
    emit creates nothing, while a fatal write creates exactly one `0600`
    non-empty report whose descriptor later fragments reuse. It also covers the
    sticky failed-open state and its clearing on re-arm, rotation trimming to
    `GC_CRASH_ARTIFACT_MAX_KEEP` by embedded stamp without touching a foreign
    file, and — end to end — a forked child that `raise(SIGSEGV)`s leaving one
    report containing the breadcrumb, phase and maps block, against children
    that `execve()` or take `SIGKILL` leaving nothing. All ordering comes from
    fork/raise/waitpid, so there are no sleeps or timing assumptions.
  - **The published automatic-restore lockout** (`3180`–`3194`, added 0.22.1)
    pins the field the snapshot exports. The regression it locks down: the
    daemon derived `autoRestoreLockoutReason` from `g_stateUncertain`, which the
    crash-loop arm never sets, so a permanently locked-out daemon published
    `LOCKOUT_NONE`. Cases cover a clean guard, an uncertain state with a clean
    guard still reporting a lockout, attempts-exhausted reporting
    `UNSTABLE_APPLY` versus a failed write reporting `AUTOMATIC_APPLY_FAILED`,
    first-cause-wins on a second latch, an incoherent (locked-out, reasonless
    or out-of-range) guard still refusing to publish `NONE`, an unexplained
    latch being recorded as the generic reason, and both null arms. The
    persistence cases additionally prove the reason survives a restart, that a
    record is coherent or rejected in both directions, that an out-of-range
    reason is refused, and that a guard latched by assigning the flag alone
  still serializes into a record the validator accepts. Source guards require
  the pure decision, the record field and `LINUX_DAEMON_GUARD_VERSION = 2`,
  and forbid the old `g_stateUncertain` spelling in the snapshot.
  **3228-3235 (added 2026-08-01)** pin the uncertain flag as a real gate:
  `linux_auto_restore_decide()` now takes `g_stateUncertain` and refuses every
  automatic trigger — including the unrationed resume — with the new
  `LINUX_AUTO_RESTORE_DENY_STATE_UNCERTAIN` verdict, a latched guard outranks
  it, and an exhausted boot is reported only after it. Source guards require
  the flag to reach the decide call and mark the pre-write GPU-not-available
  arm that must NOT set it (`F-PREP-NO-UNCERTAIN`).
  Source gates added in the final pre-ship pass also require the ACTIVE record
  store to precede `outcome.success`, require rollback plus an UNCERTAIN record
  on that store's failure, and require explicit Apply/Reset to check the
  transactional guard re-arm result. These are runtime/storage sequences with
  no hardware-free pure equivalent; the pure guard transition remains covered
  by 3140-3164.
  - Binding policy covers exact, ambiguous, and mismatched multi-GPU sets;
    compatible/conflicting sole-GPU fallback; both NVIDIA vendor/device word
    orders, internal/external/device-only and compatible subsystem forms; precise device versus
    subsystem conflicts; deterministic same-PCI rebind; transient optional
    metadata; one-time architecture retention; NVML Blackwell fallback; cached
    known backends; and future best-guess dispatch.
  - Atomic VF validation covers valid non-monotonic live frequencies, malformed
    voltage topology, implausible values, missing control reads, mask/count and
    returned-version mismatches, fresh-to-stale transitions, and stale rollback
    rejection.
  - Server-derived domains cover degraded NVML-only success; VF, mixed, and
    full-Reset preflight failure; requested-domain replacement plus unrelated-
    domain active-intent preservation; and exact
    instance/generation/GPU/topology authority by domain. TUI cases cover
    offline, degraded, recovering, ready, and draft detachment behavior.

- **Version source:** `require_app_version_fallback_in_sync()` asserts that
  `VERSION` is injected through `COMMON_FLAGS` and every source fallback is the
  neutral `dev` string. The pure-test command receives the same define.
- **Audit hardening (2026-08-22):** build-script self-tests validate both
  workflow structures and CI's read-only token permission. Source gates pin
  Linux INI bounds, owner-only Linux logs, and fingerprinted auto-profile
  diagnostics. An earlier active-session pipe SDDL suite was removed with its
  unsafe implementation; the live failure is recorded in
  [windows-architecture](windows-architecture.md).
- **Security-audit remediation gates (2026-10-01):**
  `security_gates.check_audit_finding_gates()` keeps the properties whose loss is
  invisible on the happy path. It asserts, among others, that the elevated
  installer's **first statement** in `WinMain` is
  `initialize_process_mitigations()` (checked structurally, by walking the body
  and taking the first non-comment line — not by a substring window, so the
  comment above it may be any length); that no translation unit deletes
  `g_debugLogLock`/`g_appLock`/`g_configLock`, since detached workers can still
  take them during shutdown; that `source`, `path` and `targetGpu.name` are each
  passed to `service_wire_string_is_log_safe()` (proximity-checked, so the call
  may be wrapped either way); that the update staging directory is hardened AND
  verified rather than assumed to inherit; and that
  `service_command_requires_medium_integrity()` returns unconditionally true.
  The behavioural half lives in the regression harness.

### Fuzz targets (eight as of 2026-10-01)

`tests/fuzz_main.cpp` builds once per `GC_FUZZ_TARGET`, and
`check_fuzz_harness_in_sync()` fails the build if the `#define` table and
`FUZZ_TARGETS` disagree, if a target has no `#if` body, or if it has no seed
corpus. The two newest close boundaries that previously had **no** target
despite carrying unit tests:

- **`service_response` (7)** — `validate_service_response_for_ipc()`, the
  mirror of the request validator and the function the GUI trusts to decide a
  mutation was applied. Asserts the magic/version pair, in-range
  status/operationState, `outcomeSeverityReserved == 0`, the status/severity
  agreement rule, both wire strings terminated, and idempotence. A steer bit
  drives the payload-free-refusal arm directly.
- **`installer_archive` (8)** — `gc_archive_validate()` and
  `gc_payload_validate_footer()`, the container the **administrator** setup
  program unpacks. Asserts that every accepted name is NUL-terminated in its own
  field and `gc_archive_name_is_safe()`, that every range is inside the
  container in the overflow-safe form, and reads each range with a real
  `gc_crc32` so ASan catches a miss. Store-only, so no decompression-bomb
  surface exists to fuzz.

Both are header-only pure policy with no Win32 dependency, so they are in
`FUZZ_LINUX_TARGETS` and cross-link on every host like `update_manifest`.

**Two harness lessons worth keeping.** The archive target found a real bug on
its first run: a fuzzed `fileCount` puts `dataStart` past the buffer,
`capacity - dataStart` underflows in `gc_u64`, and `gc_crc32` gets an enormous
length over a wild pointer. That was the harness's own arithmetic, not a
validator defect — but an unguarded subtraction in a fuzzer is a fuzzer bug.
And the first revision demanded the full header + 16-entry directory + 256 data
bytes before doing anything, so short inputs returned at once; the measured
`lim: 1721` was below the 1723 bytes the footer path needed, and low coverage
was the symptom. **Size the buffer from the input, not the other way round** —
a small container is a valid input that produces a range error, which is itself
worth covering.

### Linux fixture link lines (added 2026-09-15)

`tools/security_gates.py` owns both native-Linux fixtures:

- `LINUX_FIXTURES` — the `(stem, label)` table build.py loops over.
- `LINUX_FIXTURE_EXTRA_SOURCES` — the extra translation units each fixture must
  **link** beyond its own `.cpp`. An empty tuple means "link nothing extra"; a
  fixture with no entry at all is a hard error, so a link line is always
  declared rather than implied. Same contract as `FUZZ_LINUX_EXTRA_SOURCES`.
- `run_linux_fixtures()` — builds both on every host (native compile+run on
  Linux, `-target x86_64-linux-gnu` cross-link elsewhere) through
  `build.py::_run_zig_link`, so the shared Zig cache lock, poisoned-cache
  repair, and the duplicate-symbol audit apply to fixtures too.

**Why the link matters.** The fixtures `#include` real product shards
(`linux_daemon_transport.cpp`, `linux_crash_report.cpp`) and reach `gpu_core.h`,
whose *static-inline* `validate_desired_settings_for_ipc()` calls the
*out-of-line* `fan_curve_normalize_for_ipc()` in `source/fan_curve.cpp`, which
calls `set_message()` in `source/config_text_utils.cpp`. `gpu_core.h` states
that every consumer of that boundary links `fan_curve.cpp`; the fixture list was
the consumer nobody had enumerated. Because non-Linux hosts then only compiled
the fixtures to an object, the missing TU was invisible locally and surfaced as
`ld.lld: error: undefined symbol: fan_curve_normalize_for_ipc` in the Linux CI
job alone (run 34984381401). Cross-linking moves that failure onto the machine
that introduced it.

**Gates** (`tools/linux_gates.py::check_crash_report`): the table exists, the
transport fixture's entry names `fan_curve.cpp` + `config_text_utils.cpp`, the
cross path prints `Cross-linking (…)`, and `"-c", fixture_source` is forbidden
so nobody restores compile-only cross coverage.

**Verified failing:** emptying the transport fixture's entry reproduces the
exact CI `undefined symbol` on a Windows host, with a message naming the table
key to fix.

### Linux fuzz link lines (added 2026-09-15, same incident)

The identical asymmetry existed one level up and cost a second red CI run:
`--fuzz` builds the Linux targets **only on a Linux host** (libFuzzer and the
ASan runtime come from a host clang; the bundled Zig ships neither), so
`FUZZ_LINUX_EXTRA_SOURCES` -- a hand-maintained per-target link line -- was
verified nowhere but the Linux CI job. `tests/fuzz_main.cpp` `#include`s
`gpu_core.h` unconditionally for every target, so `service_request` reaches
`validate_service_request_for_ipc()` and needed `fan_curve.cpp` +
`config_text_utils.cpp`. The Win32 side never noticed because it links one
shared `FUZZ_WIN32_SOURCES` list that already contains both.

`security_gates.check_fuzz_linux_link_lines()` now cross-links every
`FUZZ_LINUX_TARGETS` entry for `x86_64-linux-gnu` during `--test` on non-Linux
hosts (skipped on Linux, where the real `--fuzz` links the same lines with real
instrumentation). It drops the sanitizer/coverage flags on purpose: it is a
**link-line check, not a substitute for `--fuzz`**, and proves only that the
declared translation units resolve every symbol the target emits.

`tests/fuzz_link_check_main.cpp` supplies the `main()` libFuzzer would
otherwise provide. It must genuinely **call** `LLVMFuzzerTestOneInput` -- the
link runs with section garbage collection, so a merely-declared entry point is
collected away together with the undefined references the check exists to find.
That was observed during development: the first version only took the
function's address and every target linked clean, hiding the real defect. A
gate in `check_fuzz_harness_in_sync()` pins the call.

Measured link lines (2026-09-15): `service_request` needs `fan_curve.cpp`,
`config_text_utils.cpp`; `config_strings` needs `config_text_utils.cpp`,
`app_shared.cpp`, `fan_curve.cpp`, `platform_posix.cpp`; `vf_snapshot`,
`wire_prefix` and `update_manifest` link bare. Keeping the lists minimal is
deliberate -- it is what makes an accidental Win32 dependency fail loudly.

**Verified failing:** deleting the `service_request` entry reproduces the exact
CI `undefined symbol: fan_curve_normalize_for_ipc` on the Windows host.

### How to run

```bash
python build.py --test
```

This:
1. Creates a temporary build directory
2. Uses the tracked `tests/regression_main.cpp` harness
3. Compiles it with LLVM-MinGW on Windows or Zig elsewhere (always with UBSan
   by default since build 79)
4. Runs the compiled test binary
5. Runs source regression checks
6. Cleans up

On native Linux the command also compiles and runs the socket-pair/pathname
fixture. Windows-hosted Linux check builds compile the production transport;
the fixture is Zig cross-LINKED for an additional ABI/link check but is
executed only where Linux filesystem sockets are available. WSL2 can cover the
native fixture but cannot reproduce the on-metal NVIDIA private VF ABI; that
requires GPU passthrough or bare-metal hardware.

Compiled tests and UBSan tests are always run together. The `--sanitizer` flag is accepted for backward compatibility but is no longer required.

The build-script self-tests also execute the update signer's owner-only key
round trip on Windows. Its current-user SID query must allocate the complete
variable-sized `TOKEN_USER` byte count returned by `GetTokenInformation`; a
fixed ctypes `TokenUser` object is too small for the trailing SID and caused a
native buffer overflow on Actions Python 3.12. A source guard requires
`ctypes.create_string_buffer(needed.value)` and forbids the unsafe fixed-buffer
spelling, while the live Windows round trip exercises the actual API and ACL.
CI keeps Python output unbuffered and runs tests in their own workflow step so
a native crash is both visible and cannot be masked by a later successful
command.

To also run with AddressSanitizer:
```bash
python build.py --test --asan
```

On Windows, the ASan test runner prepends `llvm-mingw/bin` to the test process `PATH` so `libclang_rt.asan_dynamic-x86_64.dll` is found from the bundled toolchain.
Since 2026-09-22 the harness sequences its suites through three stack frames;
the former single `-O0` ASan frame exceeded the Windows stack reserve and
crashed before any assertion. Normal and ASan runs use the same order and
failure codes. The debug-tool discovery self-test also uses one GUID-named
temp directory per invocation and verifies that exact directory before its
recursive cleanup; fixed names under the shared temp root had put unrelated
pre-existing data at risk.

**On Linux this needs a host clang.** The bundled Zig is a complete native
compiler but ships **no ASan runtime** (only `tsan`), so a Zig-linked ASan build
dies at link on undefined `__asan_register_elf_globals`. `--test --asan`
therefore never actually ran on Linux until 2026-07-28 — the plain suite passed
and the ASan variant failed to build, so the Linux daemon, transport and TUI had
never seen ASan. `security_gates.posix_test_compiler()` now resolves
`clang++` from `PATH` for ASan builds only (plain `--test` still uses the pinned
Zig, so the default path stays hermetic), and a missing clang is a hard error
rather than a skip. Both the pure harness and the socket-pair fixture pass under
ASan+UBSan on Arch with clang 22.1.8.

## Source regression checks

Defined in `build.py` as `run_source_regression_checks()`.

These include required/forbidden text, ordering, count, and aggregator-wiring
assertions. `build.py` constructs logical combined surfaces for split service
areas.

`require_text_in_surface()` / `forbid_text_in_surface()` assert against a
*surface* — an aggregator plus the shards it `#include`s — rather than one
physical path. Splitting an oversized module used to break every guard naming
the original file, which quietly discouraged the very splits the size ratchet
asks for. The Linux daemon surface (`linux_daemon.cpp` +
`linux_daemon_lifecycle.h` + `linux_daemon_serve.h` + `linux_fan_runtime.h`) is
the first user. Guards that pin a rule to one specific shard still name that
shard directly.

The table below names the physical source shard that owns each rule:

| File | Required text | Purpose |
|------|--------------|---------|
| `app_shared.h` | `APP_DEBUG_DEFAULT_ENABLED 1` | Debug logging remains default-on |
| `service_protocol.h` | `SERVICE_PROTOCOL_VERSION = 27` | Atomic generation-stamped health/state envelopes, explicit scalar/advanced-clock/fan readback validity, updater state, native zero-RPM fan intent with an independent fan-off gap, active XBAR voltage measurement, compact capability/topology carriage, domain-scoped mutation preconditions/result query, typed apply-origin, startup policy plus its content-only refresh and its own `startupProfile` response member, profile identity, lifecycle-state IPC ABI, and exact wire-size gates |
| `app_shared.h` | `APP_BUILD_NUMBER` | Build number define exists |
| `service_protocol.h` | `serviceBuildNumber` | Service response carries build number |
| `service_protocol.h` | `serviceVersion[32]` | Service response carries app version |
| `main_diagnostics.cpp` | `protocol=%lu` | Session markers include IPC protocol |
| `main_diagnostics.cpp` | `build=%lu` | Session markers include build number |
| `main_diagnostics.cpp` | `close_debug_log_file` | Debug log cleanup exists |
| `main_diagnostics.cpp` | `open_debug_log_file_locked` | Debug log open helper exists |
| `main_crash_artifacts.cpp` | `green_curve_unhandled_exception_filter` | Crash filter exists |
| `main_diagnostics.cpp` / `gpu_backend_apply.cpp` | absence of `+= StringCchPrintf` | Bounded multi-field diagnostics never interpret HRESULT success as a zero-length append |
| `platform_win32.cpp` / `platform_posix.cpp` | `GetExitCodeProcess` / `WIFEXITED` + `WEXITSTATUS` | Captured subprocesses are drained and succeed only after a clean zero exit |
| `main_service_controlled_restart.cpp` | `wcsspn(..., L"0123456789")` / `errno == ERANGE` | Internal PID and inherited-handle arguments reject signs, whitespace, and saturation |
| `main.cpp` | `SERVICE_PIPE_SERVER_IO_TIMEOUT_MS` | Pipe timeout constant exists |
| `main_service_pipe.cpp` | `CancelIoEx(pipe, &ov)` | Stalled server pipe operations are cancellable |
| `main_service_pipe.cpp` | `response.serviceBuildNumber` | Service responses include build number |
| `main_service_client_commands.cpp` | `service_client_ping: identity mismatch` | GUI rejects mismatched service identity |
| `main_service_admin_client.cpp` | `ensure_secure_service_binary_path` | Hardened adjacent service install path check |
| `main_service_admin_client.cpp` | `CopyFileW(sourcePath, tempPath` | Service binary staging |
| `main_service_connection.cpp` / `main_service_admin_client.cpp` | `get_current_executable_directory_w` | Service install resolves the current GUI/service directory |
| `main_service_connection.cpp` | `get_service_binary_path_from_scm(expectedPath` | Pipe identity compares against the SCM-registered service binary |
| `main_service_admin_client.cpp` | `apply_protected_service_dir_dacl` | Service install directory DACL is hardened before SCM registration |
| `main_service_admin_client.cpp` | `restore_inherited_dacl(installDir` | Service uninstall restores the adjacent directory DACL |
| `main_service_admin_client.cpp` | `directory_path_is_root_or_share_root_w` | Service uninstall will not restore DACLs on a drive root or UNC share root |
| `main_service_request_policy.cpp` | `Requested GPU identity no longer matches` | Requested GPU target is validated before mutating service-global selection |
| `service_lifecycle_policy.h` | `authenticationId` / `service_lifecycle_identity_equal` | Logon coalescing identity uses session ID + SID + authentication LUID |
| `main_service_persist.cpp` | `ServiceRestartReapplySnapshot` | Restart snapshots carry target GPU identity |
| `main_crash_artifacts.cpp` | `resolve_service_machine_data_dir` | Service crash artifacts use the machine service data directory |
| `main_crash_artifacts.cpp` | `gc_crash_dir_source(` | The crash artifact directory comes from the shared fail-closed policy |
| `main_crash_artifacts.cpp` | absence of `StringCchCopyA(out, outSize, ".")` | Crash artifacts never fall back to the current working directory |
| `main_crash_artifacts.cpp` | `MiniDumpWriteDump(` x1 | Every crash path shares one minidump writer |
| `cfg_glue.cpp` | `gc_invoke_fatal_dump_hook(GC_FATAL_CFG_VIOLATION` before the fast-fail | A CFG violation is reported before the uncatchable instruction |
| `ssp_glue.cpp` | `gc_invoke_fatal_dump_hook(GC_FATAL_STACK_SMASH` before `__debugbreak();` | A smashed stack is reported without depending on breakpoint dispatch |
| `linux_crash_breadcrumb.h` | `gc_crash_report_fd_slot()`, `gc_crash_report_path_slot()`, absence of `setrlimit(` | One program-wide report descriptor and path; the user's `ulimit -c` is never overridden |
| `linux_crash_breadcrumb.h` / `linux_crash_report.cpp` | `gc_crash_report_open_on_demand`, `if (!data \|\| length == 0) return;`, `gc_crash_report_reserve_slot`, absence of `O_CREAT` in the .cpp | F-LNX-CRASH-LAZY: the report file is created by the handler, never at arm time, so a report exists ⇔ a crash wrote to it |
| `build.py` / `crash_artifacts.py` | `-Wl,--build-id=sha1`, build-id equality | Linux symbols exist and match the shipped binary |
| `main_service_client_commands.cpp` | `(DWORD)service_operation_recovery_response_timeout_ms(),` (matched as the CALL, because the rationale comment beside it names the helper too) | The Windows operation-result query outlasts the mutation it recovers instead of carrying a 5000 ms literal |
| `linux_daemon_transport.cpp` | header-first read helpers, `linux_daemon_deadline_policy.h`, `linux_daemon_command_response_timeout_ms`, `exchangeDeadline`/`frameDeadline`, `SOCK_NONBLOCK` before `connect(fd,`, `note_exchange_duration(`; absence of `GC_DAEMON_IO_TIMEOUT_MS`, `daemon_read_exact(`, `daemon_write_exact(` | F-DAEMON-DEADLINE: deadlines are derived per role, one deadline spans a whole exchange, the connect cannot hang on a full backlog, and slow turnarounds are measured |
| `linux_daemon_client.h` / `linux_daemon_deadline_policy.h` | `linux_daemon_recovery_remaining_ms`, `linux_daemon_recovery_may_retry`, `SERVICE_OPERATION_OUTCOME_UNKNOWN`, `linux_daemon_connect_reachability` | The outcome recovery outlasts the mutation it recovers, cannot spin on a prompt failure, and an unrecovered write is reported as unknown rather than failed |
| `linux_tui_refresh.cpp` / `linux_tui_mutation_actions.cpp` | `linux_daemon_failure_means_offline`, `Apply outcome unknown`, `Reset outcome unknown` | A busy daemon does not tear down live authority, and an unrecovered mutation is never presented as a failed one |
| `linux_service_install.cpp` / `linux_systemd_notify.cpp` | `GC_INSTALL_BIN`, deterministic activation, `READY=1` | Protected staged binary, unconditional restart/version verification, and readiness only after replay/listen |
| `build.py` | `_verify_cached_tool_binary` | Cached toolchains are verified against pinned executable digests. The self-test in `run_build_script_regression_tests()` drives the tamper path deliberately and passes `mismatch_expected=True`, so a green run never prints an `ERROR:` digest-mismatch line — that line must stay rare enough to mean something |
| `main_service_admin_client.cpp` | `wait_for_helper_process_bounded` | Elevated helper waits are bounded |
| `profile_startup_policy.h` / `main_startup_profiles.cpp` | `STARTUP_EDITOR_SOURCE_LIVE_SNAPSHOT` / `show_live_gpu_state_for_disabled_app_launch` | Disabled app-start shows the current service snapshot; selected saved intent requires explicit Load |
| `desired_settings_helpers.cpp` | `desired_settings_match_active_service_intent` | App-start profile can be compared to service active desired intent |
| `main_startup_profiles.cpp` | `already active in background service; skipping reset-before-apply` | App-start auto-load skips disruptive reset/apply when the service already owns the same intent |
| `main_startup_profiles.cpp` | order: `desired_settings_match_active_service_intent(&desired` before `desired.resetOcBeforeApply = true;` | App-start active-service match is checked before reset-before-apply |
| `gpu_backend_apply.cpp` | `attempting driver write anyway` | Explicit memory offsets outside reported range are attempted |
| `main_gpu_state.cpp` | `live_selective_gpu_offset_matches_requested_shape` | Persisted selective GPU offset must match live VF shape |
| `main_gpu_state.cpp` | `runtime selective: ignoring persisted request` | Stale persisted selective GPU offsets are ignored |
| `main_gpu_state.cpp` | `non-selective request clears runtime state` | Uniform GPU offsets clear runtime selective state |
| `main_gpu_state.cpp` | `debug_log_on_change("current_applied_gpu_offset_mhz: not Blackwell` | Stable GPU-offset display diagnostics are change-gated |
| `main_runtime_gpu.cpp` | `debug_log_on_change("populate_global_controls: dirty=` | Global control refresh diagnostics are change-gated |
| `gpu_backend_apply.cpp` | `interactive && !app_is_service_process()` | Service applies do not inherit stale GUI lock state |
| `gpu_backend_apply.cpp` | `post-apply lock clear: no lock requested` | No-lock service applies clear stale lock markers |
| `gpu_backend_apply.cpp` | `non-tail %s point %d actual %u MHz != target` | Non-tail readback/explicit artifacts are verification-only |
| `gpu_backend_apply.cpp` | `keeping strict lock target` | Tail mismatches do not mutate requested lock intent |
| `gpu_backend_apply.cpp` | `not rewriting tail above lock` | Monotonicity handling cannot raise a locked tail above the user's lock |
| `main_runtime_gpu.cpp` | `capture_gui_desired_settings(&resetFull, true, true, false` | GUI apply captures sparse VF intent |
| `main_runtime_gpu.cpp` | `capture_gui_desired_settings(&guiDesired, true, true, false` | Profile save captures sparse VF intent |
| `main_gpu_state.cpp` | `skippedLockedTail ? 4 : 3` | Selective GPU offset detection rejects two-point high-edit false positives |
| `config_profile_repair.cpp` | `profile repair: removed non-tail readback artifact` | Legacy locked-curve profile repair is logged |
| `main_tail_diagnostics.cpp` | `diagnostic only, NO reapply` | Runtime tail drift is diagnostic-only and cannot queue a reapply |
| `main_tail_diagnostics.cpp` | `s_tailDriftLastLoggedValid` | Tail drift diagnostics log first/change/reappeared drift instead of every telemetry poll |
| `main_tail_diagnostics.cpp` | `is_curve_point_visible_in_gui(ci)` | Runtime tail drift diagnostics skip hidden/unpopulated VF endpoints |
| `service_lifecycle_policy.h` / `main_service_logon_coordinator.cpp` | `service_lifecycle_reduce` / `service_lifecycle_thread_proc` | One pure reducer and long-lived worker coalesce logon, suspend, recovery, logoff, supersession, lockout, and stop |
| `main_service_dxgi_readiness.cpp` / `main_service_logon_coordinator.cpp` | `RegisterAdaptersChangedEvent` / `WaitForMultipleObjects(..., INFINITE)` | A user-mode adapter-set change wakes still-pending recovery without a deadline, polling, sleeps, timers, hardware probes, or a per-event thread |
| `service_lifecycle_policy.h` | `service_build_profile_transition_request` / `service_build_full_restore_request` | Named profiles replace ownership safely while sparse restores touch only owned domains |
| `selected_gpu_pnp_policy.h` / `main_service_selected_gpu_pnp.cpp` | exact PCI identity parser/matcher / selected-adapter CM registration | Global devnode cues cannot impersonate selected-GPU recovery evidence |
| `entry.cpp` / `main_service_client_commands.cpp` / `main_service_pipe.cpp` | `SERVICE_CMD_LOGON_HANDOFF` | Every task invocation makes a settings-free authenticated handoff; service derives the profile |
| `main_service_controlled_restart.cpp` / `main_service_host.cpp` | `ordinary non-mutating startup` / controlled validation before `SERVICE_RUNNING` | Install, repair, ordinary SCM start/restart, and snapshot-only startup cannot replay settings |
| `main_service_persist.cpp` | `service_controlled_recovery_authorization.bin` | Controlled recovery is nonce/process/boot/snapshot-bound; stale, wrong, missing, and corrupt authorization fails closed |
| `main_service_recovery_clock.cpp` / `service_recovery_policy.h` | `QueryUnbiasedInterruptTime` / `service_compute_proof_age_ms` | Driver proof counts awake time only and rejects old/cross-boot stamp formats |
| `main_service_recovery_ledger.cpp` / `service_recovery_policy.h` | `service_record_recovery_evidence` / `service_recovery_evidence_already_recorded` | Corroborating recovery observations are deduplicated and spam history persists across automatic success |
| `service_recovery_policy.h` / `main_service_controlled_restart.cpp` | `service_classify_controlled_recovery_scm_stop_state` / `NotifyServiceStatusChangeW` / single `StartServiceW` | Old process publishes `STOP_PENDING`; helper accepts its PID-ambiguous form after the pinned dedicated parent exit, waits for SCM-owned `STOPPED` without polling/sleeps, revalidates authorization, and has one start attempt |
| `main_service_recovery_ledger.cpp` | `ledgerPathReady` / `legacyPathReady` | Explicit history acknowledgement fails closed if either protected path cannot be resolved or deleted |
| `main_service_persist.cpp` | `service_auto_restore_lockout.bin` | Sticky lockout survives restarts and automatic origins cannot clear it |
| `main_service_host.cpp` / `main_service_lifecycle_events.cpp` | `PBT_APMSUSPEND` / `PBT_APMRESUMECRITICAL` / `DBT_DEVNODES_CHANGED` | Suspend generations restore once, exact power events are diagnosed, and null/global devnode notifications are read-only cues |
| `config_utils.cpp` | `update_logon_profile_selection_transaction` | Both logon-profile keys commit/read back under one lock; failure cannot leave a half-selection |
| `main_startup_task_definition.cpp` | `startup_task_definition_classify_xml` | Executable fixtures classify canonical, compatible legacy, and broken task XML |
| `config_profiles_ui.cpp` | `sync_applied_profile_from_service_metadata` | Applied indicator follows service ownership metadata, never live VF drift |
| `main_service_request_policy.cpp` | `PROFILE_READ_FOR_OWNERSHIP` inside `service_profile_record_describes_intent` / `g_serviceActiveDesired` inside `service_confirm_profile_metadata_from_active_intent` | The service proves a claimed slot drift-free, and re-checks a delta Apply against the intent actually in force |
| `main_service_pipe.cpp` | `service_record_apply_profile_identity(&request,` / absence of `g_serviceActiveProfileSource = profileSource;` | A successful APPLY publishes its identity through the two-stage recorder, not the pre-write verdict |
| `main_fan_runtime.cpp` / `tray_presentation.cpp` / `config_profiles_gui_state.cpp` / `gui_mutation_worker.cpp` | `gui_apply_in_flight_*` | Tray theme, tray tooltip, and main-window status all report an in-flight write, driven from the mutation queue |
| `ui_main.cpp` / `gui_apply_in_flight.cpp` / `ui_main_window.cpp` | `gui_draw_apply_in_flight_banner(` in `draw_gui_scene`, the `g_app.applyInFlight` paint guard, `APPLY_IN_FLIGHT_TIMER_ID`, absence of `Sleep(` | The banner is painted only while a write runs, animates from a timer rather than by blocking the pump, and a timer outliving its state stops itself |
| `service_protocol.h` / `service_apply_severity_policy.h` / `service_protocol_validation.h` / `main_service_pipe.cpp` / `linux_daemon.cpp` / `gpu_backend_apply.cpp` | `service_response_resolve_outcome_severity(` ordered before the response write on both producers, `service_outcome_severity_matches_status(` in the validator, `service_apply_outcome_severity_for_lock_mode(` in the backend | Severity is derived once per producer at its single write-out point, validated against `status`, and a partial verify is reported as a warning instead of a silent success; hard NVML pins ignore VF tail readback |
| `ui_mutation_completion.cpp` / `gui_mutation_result_policy.h` / `gui_mutation_worker.cpp` | `completion->response.outcomeSeverity` + `set_profile_status_text(` in `set_mutation_result_status_line`, the `needs_prompt` early return in `present_manual_mutation_result`, `gui_mutation_queued_status_text(`, absence of the old background-operation wording | A clean manual result reports on the status line and raises no dialog; the window reads the published severity rather than reconstructing it from `message` |
| `auto_profile_win32.cpp` / `ui_mutation_completion.cpp` | `service_apply_origin_is_explicit(` guarding the queued line, the already-applied line, and the completion line | An explicit tray/hotkey pick reports its outcome in the window; a rule-driven foreground switch stays silent |
| `gui_tray_menu.cpp` / `main_fan_runtime.cpp` / `ui_main_window.cpp` | `SetForegroundWindow(owner)` and `TPM_RETURNCMD` inside `show_tray_menu`, absence of `SetForegroundWindow(hwnd)` there, the single `WS_EX_TOOLWINDOW, TRAY_MENU_OWNER_CLASS_NAME` creation, absence of `ShowWindow`, absence of `TRAY_MENU_EXIT_ID` in the fan runtime, `destroy_tray_menu_owner_window();` | Opening the tray menu takes the foreground through a never-shown owner instead of raising the main window, forwards the pick by hand, and releases the owner with the tray icon |
| `config_profiles_gui_state.cpp` | `g_app.guiGpuOffsetFromProfileLoad = true;` | Profile population marks the editor value as profile-sourced (exempt from the high-overclock confirm) |
| `ui_main_apply.cpp` | `oc_high_warn_decide(` | Manual Apply consults the high-overclock policy |
| `auto_profile_win32.cpp` / `main_startup_profiles.cpp` | absence of `oc_high_warn_decide` | Automation can never raise the high-overclock dialog |
| `ui_oc_hints.cpp` | absence of `EM_SETCUEBANNER` | Cue banners never paint on these always-populated edits |
| `ui_oc_hints.cpp` | `TTM_SETTIPBKCOLOR` / `TTM_SETTIPTEXTCOLOR` | Tooltips use the application palette, not the system info-tip yellow |
| `message_box_policy.h` | `set.escapeId = GC_ID_NO;` | Dismissing a Yes/No prompt answers No, never Yes |
| `ui_message_box.cpp` | `is_system_dark_theme_active()` | The prompt palette follows the OS theme |
| every prompting GUI shard | absence of `MessageBoxA(` | Prompts go through the themed box (entry.cpp excluded: window-creation failure) |
| `main_gpu_state.cpp` | `current_green_curve_fan_intent_mode` | Fan state uses a Green Curve-owned intent helper |
| `main_state_sync.cpp` | `external live fan policy observed fanIsAuto=0 gcIntent=Auto` | Service snapshots preserve Auto intent when an external fan controller is manual |
| `ui_main_window.cpp` | `preserved visible GUI fan intent` | Profile Save preserves visible fan mode over live external fan policy |
| `main_gpu_front.cpp` | `retaining last known backend for same PCI adapter` | Transient NVAPI metadata failures keep the last known backend for the same GPU |
| `main_gpu_front.cpp` | `nvapi_read_gpu_metadata: archStatus=` | GPU metadata logs architecture query status for Blackwell/best-effort diagnosis |
| `main_state_sync.cpp` | `reporting active desired lock` | Service snapshots prefer active lock intent over live tail detection |
| `ui_lock_checkbox.cpp` | `WM_LBUTTONDBLCLK` / `paired double-click release suppressed` | Lock checkbox double-click and orphan trailing release cannot advance tri-state twice |
| `gpu_backend.cpp` | `live lock detection suppressed; preserving intent` | Live lock auto-detection cannot clear configured lock intent |
| `gpu_backend.cpp` | `requestedMHz=` | GUI-side service apply sync preserves requested lock MHz |
| `ui_main_graph.cpp` | `displayed_curve_mhz_for_gui_point` | GUI graph still has a live-readback path for points the editor does not determine (the editor half is the resolved `GuiGraphPreviewPoint`) |
| `ui_main_graph.cpp` | `gui_pending_graph_preview(ci)` | The graph plots the resolved preview instead of re-deriving it behind the repaint gate's back |
| `ui_main_graph.cpp` | *(forbidden)* `g_app.guiCurvePointExplicit` | Point ownership is resolved once into the preview; a second copy in the renderer is what the repaint gate misses |
| `ui_pending_changes.cpp` | `if (curveOrLockFlipped \|\| previewMoved > 0)` | The graph repaints when the pending VALUES move, not only when the pending presentation flips |
| `ui_pending_changes.cpp` | `gui_pending_resolve_graph_preview(out);` | The preview is resolved on every evaluation path, including the clean-editor early returns |
| `gui_pending_changes_policy.h` | `gui_graph_preview_point_equal` | What the graph draws for a point is one comparable record, so the repaint gate cannot drift from the renderer |
| `ui_lock_checkbox.cpp` | `gui_pending_changes_refresh();` | The lock checkbox re-evaluates the pending state after its dirty transition re-snapshots `GuiDraft` |
| `ui_main_graph.cpp` | `gui locked tail live readback drift:` | GUI logs live locked-tail readback drift instead of hiding it (the gate's actual string; the wiki previously named a `live readback drift hidden in graph` message that no longer exists anywhere) |
| `desired_settings_helpers.cpp` | `desired_is_fan_only_apply_request` | Fan-only apply requests are detected without curve/OC fields |
| `desired_settings_helpers.cpp` | `desired_updates_curve_or_gpu_offset_state` | Memory/power-only partial applies do not replace sparse curve intent |
| `main_service_apply_runtime.cpp` | `merged fan-only request into active desired` | Service fan-only applies preserve active curve intent |
| `gpu_backend.cpp` | `skipped VF edit repaint for fan-only apply` | GUI client fan-only applies do not clear sparse curve masks |
| `ui_main_window.cpp` | `preserving VF editor intent after fan-only apply` | Main apply handler preserves VF editor state after fan-only apply |
| `main_runtime_control.cpp` | `curvePoints=%d (%s)` | GUI capture logs sparse curve point lists |
| `config_profiles.cpp` | `point74=%d/%u point75=%d/%u point76=%d/%u` | Profile save logs edited pre-tail VF points |
| `main_shell.cpp` | `preserving requested value` | Config memory offsets are not clamped to reported range |
| `main_runtime_nvml.cpp` | `parse_cli_point_arg_w(arg, &idx)` | CLI point parsing |
| `build.py` | `--check` | Check build flag |
| `build.py` | `--test` | Test flag |
| `build.py` | `compile_commands.json` | LSP support |
| `.gitignore` | `*.7z` | Windows release archives ignored |
| `.gitignore` | `*.tar.xz` | Linux release archives ignored |
| `.gitattributes` | `*.sh text eol=lf` | Shell scripts are LF regardless of host autocrlf |
| `build.py` | `normalize=(os_name == "linux")` | Linux release text is rewritten to LF at packaging time |
| `build.py` | *(forbidden)* `os.chmod(staged, 0o755)` | The executable bit comes from the archive header, not a Windows no-op chmod |
| `release_manifest.py` | `def write_linux_tarball` | The Linux archive records Unix modes |
| `release_manifest.py` | `Linux cannot run a CRLF shell script` | The finished tarball is rejected if shipped text carries CRLF |
| `security_gates.py` | `def check_linux_release_packaging` | A real packaging round-trip covers the Linux archive |
| `build.py` | `-fstack-protector-strong` | Stack protector flag retained |
| `build.py` | `llvm_bin = os.path.dirname(LLVM_MINGW_CLANG)` | ASan tests can find bundled sanitizer runtime |
| `config_utils.cpp` | `errno == ERANGE` | Windows integer parser rejects overflow/underflow |
| `linux_port.cpp` | `errno == ERANGE` | Linux integer parser rejects overflow/underflow |
| `fan_curve.cpp` | `fan_curve_set_default(config)` | Degenerate fan curve normalization falls back safely |
| `app_shared.h` | `len > bufSize - offset` | HeapBuffer bounds checks avoid size_t addition overflow |
| `main_service_connection.cpp` | `Service response protocol mismatch` | Service responses are validated before use |
| `main_diagnostics.cpp` | `write_all_to_handle` | File writes use size_t-safe chunked helper |
| `build.py` | `generate_lsp=not args.sanitizer` | Sanitizer check does not dirty `compile_commands.json` |
| `main_service_connection.cpp` | `GetNamedPipeServerProcessId` | GUI validates pipe server PID against SCM service process |
| `main_service_client_commands.cpp` | `compatible build mismatch accepted` | GUI accepts same-version/protocol service build-number drift |
| `main_diagnostics.cpp` | `write_text_file_atomic_service` | Service writes use hardened temp/final path policy |
| `gpu_core.h` / `service_protocol.h` | `GpuAdapterInfo` / `targetGpu` | Protocol carries GPU identity |
| `gpu_backend_apply.cpp` | `reset_oc_before_gui_apply` | GUI OC applies reset prior OC baseline before applying |
| `ui_main_window.cpp` / `profile_save_policy.h` | `profile_save_uses_gui_capture` | Profile Save uses live hardware defaults only when neither the editor nor the applied state owns curve/lock intent; an applied pin/curve uses the GUI capture so the saved slot keeps matching the active profile and the tray tick survives |
| `desired_settings_helpers.cpp` / `profile_ownership_policy.h` | `profile_ownership_fan_mismatch_allowed` | The post-apply/ownership checks ignore a fan domain the delta Apply deliberately did not claim; the strict pre-write profile claim check still requires full fan equality |
| `app_shared.h` | `guiHasUserModifiedValues` | Flag reset in populate_desired_into_gui, set in EN_CHANGE handlers; prevents stale loaded-profile values from being saved |
| `main_runtime_gpu.cpp` | `resetOcBeforeApply = true` | GUI OC capture sends explicit reset-before-apply target |
| `main_runtime_nvml.cpp` | `nvml_select_device_for_selected_gpu` | NVML/NVAPI matching prefers selected GPU PCI identity |
| `ui_main_controls.cpp` | `gpuSelectY = dp(10)` | GPU selector stays in the graph header gap |
| `main_layout_policy.h` | `main_layout_build_plan` | Pure responsive layout chooses graph height, VF columns/rows, section boundaries, and overflow |
| `ui_main_layout.cpp` / `ui_main_window.cpp` | stable scrollbar convergence / `WM_DPICHANGED` | Full content remains reachable across custom DPI, small work areas, and monitor changes |
| `main_service_connection.cpp` | `does not match expected` | Pipe server executable path verified against expected service binary |
| `main_service_connection.cpp` | `get_process_image_path` | Pipe server process image path queried for verification |
| `main_diagnostics.cpp` | `parent dir verified` | Service file-write verifies parent directory before temp creation |
| `main_diagnostics.cpp` | `FILE_FLAG_OPEN_REPARSE_POINT` | Service file-write opens parent without following reparse points |
| `gpu_backend_apply.cpp` | `fan failure triggered rollback` | Fan apply failure triggers rollback of earlier hardware writes |
| `main_runtime_nvml.cpp` | `refusing ordinal fallback` | Multi-GPU ordinal fallback blocked when PCI identity is available |
| `main_fan_runtime.cpp` | `fan auto write` | Fan auto mode applied before stopping runtime |
| `main_fan_runtime.cpp` | `restored driver auto, runtime stopped` | Fan auto apply logs successful restore before stopping runtime |
| `main_runtime_nvml.cpp` | `rollbackFailures` | Multi-fan rollback tracks individual rollback failures |
| `main_gpu_state.cpp` | `rejecting out-of-range gpuOffsetMHz` | Persisted selective GPU offset range-checked before use |
| `main_service_fan_worker.cpp` | `thread handle preserved` | Fan thread handle preserved on timeout to prevent replacement |
| `main_shell.cpp` | `draw_checkbox_tick_smooth(hdc, &box, RGB(0xE8, 0xF2, 0xFF))` | Lock "Lk" FLATTEN tick uses the shared anti-aliased checkmark renderer (not a jagged raw-GDI Polyline) |
| `main_shell.cpp` | *(forbid)* `Polyline(hdc, pts, 3)` | Lock checkbox no longer hand-rolls the aliased GDI checkmark |
| `ui_main_window.cpp` | `decide_lock_activation(` | Every lock notification routes through compiled activation policy |
| `ui_main_window.cpp` | `lock checkbox command: vi=%d notify=%u decision=` | Notification, gesture, press-state, current-state, and decision are logged |
| `ui_lock_checkbox.cpp` | `activate_lock_checkbox_once` | The tri-state mutation has one centralized entry point |
| `ui_main.cpp` | `lock checkbox subclass install FAILED` | Failure to install gesture-level hardening is diagnosed |

## Conventions

- Tests do **not** touch GPU hardware
- **Fixtures must be re-runnable.** The DACL fixtures used to name their scratch
  directory by PID alone and remove it with a bare `RemoveDirectoryW`. Because
  the fixture had just applied a *protected* DACL, that removal silently failed
  and leaked the directory; when Windows later reused the PID the next run
  failed at return 167 with no real defect. They now create a
  performance-counter-unique directory via `gc_make_unique_temp_dir()` and tear
  it down with `gc_remove_protected_temp_dir()`, which restores inheritance
  using the same `restore_inherited_dacl()` production uninstall uses, and the
  teardown result is asserted (returns 171/139) so a future leak fails loudly
  instead of accumulating.
- Main-window layout coverage includes the reported 3440x1440/custom-140% case,
  the captured zero-to-78-point 4K/150%-DPI content-growth regression,
  work-area clamping/no-shrink behavior, impossible high-DPI overflow, and 800
  DPI/viewport combinations. It asserts
  exact overflow, all 87 populated point-cell bounds, and monotonic
  non-overlapping section boundaries without creating a visible window.
  It also pins first-launch work-area centering, oversized clamping,
  center-preserving late growth, and identical labeled/unlabeled checkbox box
  metrics at 150% DPI. Source guards cover placement persistence and dark
  auto-profile dropdown/checkbox rendering.
- Auto-profile owner-draw checkbox state has pure set/get/toggle cases (returns
  735–737). Source guards require explicit dialog-model state and synchronous
  repaint, and forbid the `BM_GETCHECK,`/`BM_SETCHECK,` call form because
  `BS_OWNERDRAW` does not provide native checkbox-state storage.
- **Owner-draw checkbox repaint gate (pure, F-CHECKBOX-PAINT, returns
  1650-1655):** the first projection against an unpainted mirror repaints, a
  steady state is inert so background probes cannot flicker, **a checked ->
  unchecked transition repaints (1652)**, and an unknown mirror always repaints.
  1652 is the reported bug reproduced: with the old `BM_GETCHECK` gate emulated
  (always "unchecked"), the harness exits 1652 exactly. The old gate could not be
  unit-tested at all — it needed an `HWND` — so extracting the predicate is what
  made the regression reachable. The guards in `tools/ui_gates.py` forbid the
  call form across every shard that paints or projects one of these controls,
  matching `BM_GETCHECK,` with the trailing comma so the comments explaining
  *why* the rule exists can stay next to the code.
- **Labeled-checkbox hit area (pure, F-CHECKBOX-HIT, returns 1640-1646):** across
  96/120/144/192 DPI, the caption starts exactly at the offset the renderer draws
  it (`inset + box + gap`), the control covers every caption pixel, only the
  balancing inset follows the caption, the box-to-caption gap is inside the
  control so there is no dead stripe mid-control, a caption 40px wider widens the
  control by exactly 40, and the fit stays strictly below the fixed widths it
  replaced — that last one is the point: the old widths handed out empty
  background as a silent toggle. A captionless control still reserves the box,
  and a negative measurement cannot yield a sub-1-pixel width. Source guards
  (`tools/ui_gates.py`) forbid `SS_NOTIFY` in `entry.cpp` and `STN_CLICKED` in
  `ui_main_window.cpp` — a caption in a separate `STATIC` is what left the gap
  dead and forced the untheme-able system grey-text on the disabled label —
  require both runtime re-fit calls, and require the renderer and the fit to read
  the same inset/gap metrics.
- New tests must be deterministic and avoid sleeps, polling, or wall-clock
  thresholds. The obsolete RC6d wall-clock serialization test was removed;
  runtime-lock behavior is now pinned by policy tests and operation-scoped
  source/order guards.
- Exit code 0 = pass, non-zero = fail with specific code indicating which check
- The harness links `fan_curve.cpp`, `config_utils.cpp`, `app_shared.cpp`,
  `service_acl.cpp`, `service_path_chain.cpp`, `vf_backends.cpp`, and
  `platform_win32.cpp` on Windows. It
  directly includes selected pure implementation/policy files such as task XML,
  lifecycle, recovery, PnP identity, auto-profile, and layout helpers.

## Static analysis and CI release gates

`python build.py --tidy` is a hard ratchet implemented by
`tools/static_analysis.py`. Windows uses bundled llvm-mingw clang-tidy; Linux
uses a host-native analyzer and a filtered host compile database. Findings
already in `tidy-baseline.txt` are reported, while any new diagnostic,
`clang-diagnostic-error`, missing analyzer, or non-zero tool exit fails.
The current baseline has 39 entries. The 2026-07-29 split-file drift was fixed
in code (explicit CLI argv ownership/advancement, complete switches, and the
profile comparison), then `--tidy-baseline` removed exactly three obsolete
entries; no new finding was accepted into the baseline. That pruning predates
the 2026-07-30 add-only rule: `--tidy-baseline` no longer removes anything
except entries whose source file is gone, so an equivalent cleanup today is a
hand deletion of the lines the run lists.

The ratchet's own matching rules are covered by `static_analysis.run_self_tests()`,
called from `run_build_script_regression_tests()` so `--test` exercises them on
every host with no toolchain required. They assert what the 2026-07-30 CI
failure violated: an LLVM 18 diagnostic and an LLVM 20+ diagnostic for the same
check cover each other (alias-set intersection both ways), an unrelated check or
the same check in another file is still a new finding, line numbers stay out of
the key, `SHARD_HEADER_FILTER` keeps the amalgamated `.cpp` shards in scope and
`.h`/toolchain headers out, and `_merge_baseline()` keeps an entry this host did
not report while dropping one whose source file is gone. `check_ratchet_wiring()`
holds the matching source guards, in `tools/static_analysis.py` because build.py
stays under its size ratchet. End-to-end verification is in
[build.md](build.md#cross-version-portability-2026-07-30).

For `main` pushes and pull requests, CI runs the ratchet and bounded fuzzing on
both native hosts. It also runs the actual full packaging commands (`--target windows` and
`--target linux`), so archive/setup assembly and exact manifest verification
are merge gates rather than a manual-only path. The Linux job deliberately
does **not** install p7zip: the Linux `.tar.xz` is written by the standard
library, and leaving 7-Zip off that runner turns a reintroduced dependency
into a failing packaging step instead of a silent one. (The separate manual
release workflow is the exception: it installs `p7zip-full` because it must
produce the Windows `.7z` archives and setup executables too.)
The CI workflow requests only `contents: read`, and `check_workflow_structure()`
rejects tab-indented or file-scope workflow steps so a malformed YAML edit
cannot turn every merge gate into a silent no-op.

### Linux release packaging round-trip (`check_linux_release_packaging`)

`tools/security_gates.py` runs the real packaging helpers over a fixture that
reproduces exactly what a Windows working tree hands the packager: CRLF text and
no filesystem executable bit. It stages through `stage_release_file()`, writes a
`.tar.xz` with `write_linux_tarball()`, then re-opens the finished archive and
asserts LF-only text, a `#!` shebang, `0755` on `greencurve` and
`greencurve-setup.sh`, `0644` on `README.md`/`LICENSE`, and `uid/gid 0` — plus
that the ELF fixture's own CR bytes survived untouched, since normalization is
for text only. Three hand-built broken archives (CRLF script, `0644` script, no
shebang) must each be rejected by `verify_linux_tarball()`; verification reads
the archive rather than the staging tree, so a writer bug cannot pass by
agreeing with the code that fed it. `check_packaging_skip_warning()` additionally
asserts that `report_packaging_skipped()` *raises* on a Linux entry.

This suite fails against the pre-2026-07-29 behaviour on both counts.
`tools/linux_gates.py: check_setup_script_line_endings()` is the source-side
half: it reads `tools/greencurve-setup.sh` as bytes and rejects a CRLF working
copy, which `.gitattributes` alone cannot fix in a clone made before it existed.

### Full Windows cross-builds from a Linux host

`python build.py` now preserves the full Windows + Linux default matrix on a
Linux host. It downloads the SHA-256-pinned upstream Ubuntu llvm-mingw 20260519
bundle into the host-separated `llvm-mingw-linux/` cache and invokes its
target-prefixed `x86_64-w64-mingw32-clang++`; using the bundle's generic
`clang++` would silently select `x86_64-unknown-linux-gnu` and reject `-mguard=cf`.
Its native `llvm-rc`, `llvm-objcopy`, `llvm-strip`, `llvm-readobj`,
`llvm-pdbutil`, and `llvm-nm` replace the former hard-coded `.exe`
tool paths. Verified archive symlinks are recreated only when their resolved
target stays inside the toolchain root.

The Linux host now exercises the real Windows release paths, not just syntax:

- x64 GUI/service compile as parallel LTO bitcode objects, link with CFG/ICF,
  and pass PE hardening plus PDB verification;
- ARM64 GUI/service retain the Zig object-first no-LTO path and pass PE
  hardening, BTI/PAC/AUT, and private-debug verification;
- `llvm-rc` builds the application and installer resources;
- 7-Zip produces and reads back both Windows archives; and
- both setup executables are built and verified. Without `cabinet.dll`, their
  payload uses the installer's existing checked `METHOD_STORE` fallback.

The build-script regression suite pins `resolve_targets("all") ==
["windows", "linux"]`, rejects any `.exe` tool selected on Linux, and requires
the native tar asset. The release gates remain the end-to-end proof.

Linux x64 gained full LTO in the same work. Both its object compilation and
link flags must contain `-flto`; the completed check retained split symbols,
ELF hardening, and 559 `endbr64` instructions. ARM64 was tested rather than
assumed: Linux full LTO, Linux ThinLTO, and Windows full LTO all linked but
emitted zero BTI/PAC/AUT instructions, so every artifact was rejected and both
ARM64 paths remain `-O2 -fno-lto`.

## Fuzzing (`python build.py --fuzz`)

Coverage-guided fuzzing over the untrusted-input boundaries. Harnesses live in
**`tests/fuzz_main.cpp`**; the driver is `run_fuzz_targets()` in
**`tools/security_gates.py`**.

### libFuzzer works on the pinned toolchain — no MSYS2 needed

llvm-mingw's clang driver **rejects `-fsanitize=fuzzer`** for
`x86_64-w64-windows-gnu`, which long looked like "no fuzzing on this project".
The gate is only on that *convenience flag*. Both halves it normally implies
ship and work on this target:

- `-fsanitize-coverage=inline-8bit-counters,trace-cmp,trace-div,trace-gep,pc-table`
- `llvm-mingw/lib/clang/22/lib/windows/libclang_rt.fuzzer-x86_64.a`

Passing them directly produces a working libFuzzer binary, including combined
with ASan and UBSan. Verified against a planted crash: libFuzzer drove coverage
2→6 and hit it.

The same clang binary accepts `-fsanitize=fuzzer` for `x86_64-pc-windows-msvc`
and `x86_64-linux-gnu`, so **the gate is per-target, not per-distribution**.
MSYS2's clang64 targets the identical `x86_64-w64-windows-gnu` triple and hits
the identical gate — switching to it would change nothing. Rejected approach;
do not revisit.

### Linux host (added 2026-07-28)

Fuzzing had **never run on Linux**: `run_fuzz_targets()` began with an
unconditional `return 0` on non-Windows, so `--fuzz` printed a note and reported
success without building anything. Every result recorded below came from
Windows-compiled builds.

That is not a redundant re-run, because the host is a real variable here.
Windows is LLP64 and Linux LP64, the libc and allocator differ (so ASan sees
different things), and two of the five targets are Linux daemon code —
`vf_snapshot` is `linux_vf_snapshot_structurally_valid()`, `wire_prefix` is
`daemon_accept_disposition()` / `linux_daemon_format_permission_facts()`.
`service_request` validates what the Linux daemon accepts over its Unix socket
from any member of the `greencurve` group.

A Linux host uses a `clang++` from `PATH` with the plain `-fsanitize=fuzzer`
convenience flag (accepted for `x86_64-linux-gnu`), and a missing clang is a
hard error. `FUZZ_LINUX_TARGETS` in `tools/security_gates.py` is the table, and
`fuzz_targets_for_host()` is the pure selector the build-script regressions
exercise. Asking for a Windows-only target on Linux is refused by name rather
than silently running nothing.

**Four of the five targets run on Linux.** `service_request`, `vf_snapshot` and
`wire_prefix` need no extra translation unit at all. `config_strings` joined
them on 2026-07-28 once `parse_fan_value`, `parse_cli_point_arg_w` and
`config_section_header_matches_ascii` moved out of the Win32-only
`config_utils.cpp` into `config_text_utils.cpp`; it links the extra sources
listed in `FUZZ_LINUX_EXTRA_SOURCES` and gets `win32_compat.h` force-included
the same way `tests/regression_main.cpp` does, because the harness names `WCHAR`
and must stay unmodified for the Windows build. Since that same move deleted
`linux_port.cpp`'s private `parse_fan_value`, this target now fuzzes the code
the Linux daemon actually runs.

The one remaining omission is structural, not an oversight: `task_xml`
`#include`s `main_startup_task_definition.cpp`, a Win32 shard.

### Targets

One binary per target (`-DGC_FUZZ_TARGET=N`), so each keeps a focused coverage
signal and its own corpus. Seeds are committed under `tests/fuzz-corpus/<target>/`
and are read-only: libFuzzer writes new inputs to a scratch corpus in
`build-tmp/`, so a run never mutates the repository.

| Target | Function under test | Why it matters |
|---|---|---|
| `service_request` | `validate_service_request_for_ipc()` | The single gate between an unprivileged caller and the privileged service |
| `vf_snapshot` | `linux_vf_snapshot_structurally_valid()` | Driver-ioctl data that is indexed downstream |
| `task_xml` | `startup_task_definition_classify_xml()` | Hand-rolled UTF-16 tag scanner over user/Task-Scheduler-controlled text |
| `config_strings` | `trim_ascii`, `parse_int_strict`, `parse_fan_value`, `config_section_header_matches_ascii`, `parse_cli_point_arg_w` | INI text and CLI arguments |
| `wire_prefix` | `service_wire_prefix_disposition`, `daemon_accept_disposition`, `linux_daemon_format_permission_facts` | Daemon transport classification and a bounded diagnostic formatter |
| `update_manifest` | `gc_update_manifest_parse()`, the URL/redirect allowlist, the backoff schedule | The signed release manifest and where the updater may connect. In production this parser only ever sees signature-verified bytes, so it is not nominally untrusted — it is fuzzed anyway, because "the verifier ran first" is exactly the assumption that must not be load-bearing twice, and the result of a bad manifest should be a refusal rather than memory corruption in a LocalSystem process about to launch an installer |

### Assertions, not just "did it crash"

Every target asserts the post-conditions its function advertises, via
`GC_FUZZ_CHECK` (`abort()`, because `NDEBUG` compiles `assert` out). A validator
that returns *accepted* while leaving a field out of range is a finding even
with no memory error. Examples pinned today:

- Accepted IPC requests satisfy every documented clamp (`curvePointMHz<=5000`,
  `lockCi` in `[-1, VF_NUM_POINTS)`, `fanPercent` in `[0,100]`, canonical
  `gc_bool8`, NUL-terminated wire strings), and validation is **idempotent** —
  re-validating an accepted request must not change a byte.
- Reason/diagnostic buffers come back NUL-terminated *within* the size passed
  and are never written past it (canary tails catch the overrun).
- The XML classifier returns an in-enum value and is deterministic.

Steering bytes let the fuzzer reach deep validity (e.g. a leading control byte
sets the correct protocol magic/version) instead of burning its budget failing
the first word, while leaving the fully-random shape reachable.

### Running it

```bash
python build.py --fuzz
```

Default is a bounded **20000 runs per target**, measured in iterations rather
than seconds so the gate is deterministic and carries no timing assumption
(`-seed=1` fixes entropy so a CI failure reproduces locally). For a real
session:

```bash
python build.py --fuzz --fuzz-target task_xml --fuzz-runs 1500000
```

`check_fuzz_harness_in_sync()` pins the target table in
`tools/security_gates.py` against the `#define`s in `tests/fuzz_main.cpp` and
requires a non-empty seed corpus plus post-condition assertions for every
target, so a rename cannot quietly disable coverage.

**Negative control performed** (2026-07-25): tightening one post-condition to
something the validator does not guarantee (`lockCi >= 0` instead of `>= -1`)
made `service_request` fail with exit 77 and write a reproducer. The harness
demonstrably detects violations rather than passing vacuously.

**Results so far: no defects found.** Deep sessions on 2026-07-25, all clean:

| Target | Runs | exec/s | Final cov / ft |
|---|---|---|---|
| `task_xml` | 1,500,000 | 22,058 | 242 / 548 |
| `service_request` | 1,000,000 | 55,555 | 150 / 273 |
| `vf_snapshot` | 1,000,000 | 47,619 | 67 / 183 |
| `config_strings` | 1,000,000 | 166,666 | 83 / 196 |
| `wire_prefix` | 1,000,000 | 250,000 | 95 / 214 |

Note the depth gap: `service_request` reaches cov 82 at the bounded 20000-run
gate but cov 150 at 1M runs. The bounded gate is a regression tripwire, not a
substitute for occasional long sessions.

**First Linux-host run (2026-07-28), clean at the bounded gate.** All three
Linux-capable targets passed 20000 runs under ASan+UBSan with clang 22.1.8,
adding 280 / 182 / 272 new corpus units respectively — coverage is genuinely
growing on this host, so these are not re-treading the Windows corpus. No deep
(1M+) Linux session has been run yet; that is the obvious next step and the
place a host-specific defect would most likely surface.

## CET instrumentation gate (`python build.py --check-cet`)

Verifies that `-fcf-protection=full` is actually *effective*, not merely
present on the command line — the "silently ineffective hardening flag" failure
class. Implemented in `check_cet_instrumentation()` in
`tools/security_gates.py`; see [build.md](build.md) for the measured baseline
and the forward-edge/backward-edge distinction.

It walks the PE load-config Guard CF function table and checks each target's
first four bytes for `f3 0f 1e fa`, attributing every target to our source or
to the prebuilt vendor runtime (libc++/libc++abi, libunwind, MinGW CRT — all
shipped without `-fcf-protection`, so they are reported separately rather than
counted as our regressions).

Release binaries are linked with `-s` and carry no symbol table, so attribution
is impossible on them. With no argument the gate therefore **compiles its own
unstripped probe** using the real shipped flag set (minus `-s`), which also
makes it independent of whatever happens to sit in `dist/`. Current result:

```
Guard CF targets           : 312
our targets with endbr64   : 18
our targets WITHOUT endbr64: 0
vendor runtime without endbr64 (expected, prebuilt): 294
```

**Negative control performed** (2026-07-25): removing `-fcf-protection=full`
from `WINDOWS_FLAGS` made the gate exit 1 and name the 18 functions that lost
instrumentation (`WndProc`, the dialog/subclass procs, the thread procs, the
PnP/service callbacks, `__stack_chk_fail`, `__guard_check_icall_fptr`).

## Check builds

`python build.py --check --target <...> --arch <x64|arm64|all>` compiles every
requested OS/architecture combination in isolated temp directories without
replacing release outputs. Artifact gates verify architecture, version,
PE/ELF hardening, private paths, and final ARM64 BTI plus PAC/AUT instructions.

## VF curve comparison tool

`tools/compare_vf_curves.py` compares two `--json` output files and reports:
- Global settings diff (GPU/mem offset, power limit, fan)
- Per-point VF curve diff (freq, volt, offset)
- Tail drift analysis (points 76-126), lock target, spread, drifted count
- Monotonicity violations
- Summary statistics

Usage:
```bash
py -3 tools/compare_vf_curves.py <baseline.json> <comparison.json>
```

See the tail-drift measurement in `llm-wiki/gpu-backend.md` for a real-world usage example.

## Source of truth

- `tests/regression_main.cpp`: the compiled pure harness
- `tests/linux_transport_regression.cpp`: native Linux socket fixture
- `tests/linux_crash_report_regression.cpp`: native Linux crash-artifact fixture
  (fork + real fatal signal). `security_gates.run_linux_fixtures()` loops over
  both native fixtures (`LINUX_FIXTURES`), running them on a Linux host and
  cross-linking them everywhere else.
- `tests/fuzz_main.cpp`: libFuzzer harnesses (one binary per `GC_FUZZ_TARGET`)
- `tests/fuzz_link_check_main.cpp`: `main()` for the non-Linux fuzz
  link-line cross-link; never executed, and not a fuzzing entry point
- `tests/fuzz-corpus/<target>/`: committed read-only seed inputs
- `tools/security_gates.py`: fuzz driver, fuzz/corpus sync check, the CET
  instrumentation gate, and `check_no_developer_profile_paths()` (privacy gate
  over tracked text files); imported by `build.py`, never the reverse
- `tools/wiki_public_gates.py`: the public/private wiki split. The real-tree
  check (`check_all()`, reached through `security_gates.check_public_wiki()`
  from `run_source_regression_checks()`, i.e. `--test` and `--gates`) fails if
  anything under `llm-wiki/log/` or `llm-wiki/private/` is tracked, if
  `.gitignore` loses those rules or re-ignores a public page, if `CLAUDE.md`
  diverges from `AGENTS.md`, or if a tracked agent/wiki page contains an email,
  a Windows auto host name, a non-documentation IPv4 address, a secret-shaped
  token, or the developer's own OS account name; separately, ANY tracked path or
  text file naming a third-party GPU tool (denylist, word-bounded, fragment-built). `run_self_tests()` (called
  from `security_gates.run_self_tests()`) feeds every rule bad input and good
  look-alikes, so each gate is proven to fail. Note that plain `python build.py`
  does not run the source gates; `--test` and `--gates` do.
- `tools/release_manifest.py`: `check_all()` (packaging/archive guards) and
  `check_packaging_skip_warning()` — a behavioural test that captures the
  no-archiver warning and asserts it names every built binary, rather than a
  text match on the code that prints it
- `tools/ui_gates.py`: Windows GDI overclock-row source gates
- `tools/fan_gates.py`: fan manual-write verification, fan runtime failsafe /
  daemon lifecycle, native zero-RPM, and the power-limit range gates. Takes build.py's
  pre-built Linux backend surface so a guard survives a backend split
- `tools/readback_gates.py`: the protocol-v14 readback-provenance producers on
  both platforms, plus the power-target control-surface rules (an unreadable
  power target publishes the board default rather than 0; reset-before-apply
  never writes power on a surfaceless board; Linux's three inert-power-request
  gates). Split out of `build.py` when it hit its size ratchet. Every rule here
  guards a negative that regresses silently
- `build.py`: `run_regression_tests()` (compiles/runs the harness) and
  `run_source_regression_checks()` (text/order guards)

## Power-target control-surface suites (added 2026-09-09)

`tests/regression_main.cpp`, exit codes **5040-5071** plus **4756**. Every
assertion fails against the pre-fix behaviour, so this is a genuine regression
suite rather than a description of what the code now does.

- **5040-5044 — F-POWER-SURFACE, the predicate.** `power_limit_surface_available()`
  refuses a readback flag without a usable mW pair, and stays true for a fully
  answered board so the rule cannot subtract a domain from working discrete
  hardware. 5040 is the exact reported state: constraints known, limit unknown.
- **5045-5051 — the unknown value.** `power_limit_pct_from_mw()` publishes
  `POWER_LIMIT_DEFAULT_PCT` (100) for every unusable mW pair, keeps half-up
  rounding identical to the pre-split arithmetic, and preserves above-100%
  percentages.
- **5052-5057 — the reported bug, pinned.** `power_reset_before_apply_required()`
  schedules no reset write on a surfaceless board (5052/5053), still writes a
  real off-target value (5054/5055), skips a board already at the target (5056),
  and never writes power for a request that does not own it (5057). Since
  2026-09-13 the predicate takes the target as a fourth argument instead of
  comparing against `POWER_LIMIT_DEFAULT_PCT` — see the F-APPLY-CEILING suite.
- **5058-5059 — the publisher agrees.** `apply_control_readback_validity()`
  cannot turn a refused power read into a published readback.
- **5060-5065 + 4756 — F-POWER-SENTINEL.** `validate_desired_settings_for_ipc()`
  normalizes the unknown `0` sentinel to the board default *before* clamping.
  Assertion **72** was updated at the same time: it previously pinned the old
  `0 -> 50` behaviour, which is how a board that never reported its power target
  had every Apply silently ask to halve it. 4756 keeps the genuinely-too-low
  clamp covered.
- **5066-5071 — F-CAP-POWER.** The capability probe classifies a refused limit
  read as REFUSED and an absent entry point as UNPROBED (so older drivers on
  working GPUs are untouched), and a refused power domain must classify the
  board as PARTIAL — VF curve, offsets and fan still available — never
  monitor-only. That last one is the invariant the reported bug violated.

The Linux half is not pure (it reads a `LinuxGpuState`), so it is covered by
source gates in `tools/readback_gates.py` rather than by the harness.

## Apply transition-ceiling suite (F-APPLY-CEILING, added 2026-09-13;
## extended 2026-09-16 for CT-01..CT-08)

`tests/regression_main.cpp`, exit codes **5200-5243** and **5400-5472**. Covers the ordering
defect that let a profile switch crash the display driver: the lock was the last
clock write of an apply, so a switch from an unpinned profile to a pinned one
raised the whole VF curve and only capped it 1.21 s later. See
`llm-wiki/gpu-backend.md` (F-APPLY-CEILING) for the measured timeline.

- **5200-5204 — the power sibling.** `power_reset_before_apply_target_pct()`
  returns the apply's own target for an owning request and the board default
  otherwise, and `power_reset_before_apply_required()` schedules exactly one
  write to that target — never a bounce up to 100% and back down, which is what
  made a power-lowering profile run the entire apply at full TGP.
- **5205-5215 — the ceiling plan table.** A clamp is armed for HARD (with the
  final pin re-asserting it) and for FLATTEN (with the final locked-clock
  release handing it over), and refused when no lock is declared, when the lock
  MHz is zero, when the request does not own the clock domain, and when either
  NVML entry point is missing — arming a clamp that cannot be released would
  strand the cap. Plus the abandon rule
  (`apply_clock_ceiling_release_on_abandon()`).

  Note these all call the 5-argument form, which defaults the outgoing-state
  arguments to "nothing known" and therefore still expresses the ORIGINAL
  lock-only rule. That is deliberate: they remain valid, and 5400+ below covers
  what the rule became.
- **5216-5225 — the Linux phase order.** `LINUX_MUTATION_LOCK_CEILING` executes
  first and strictly before `RESET_BASELINE`, `GPU_OFFSET` and `CURVE`, while
  `LOCK` stays after `CURVE`; a request with no ceiling omits the phase without
  disturbing the rest of the order; and a failure in the ceiling phase rolls
  back nothing because nothing else has run.

- **5226-5239 — the witness verdict.** `apply_clock_witness_verdict()` reports
  HELD at or below the ceiling, `HELD (driver rounded up one bin)` for the
  15 MHz clock bin NVIDIA may round a locked-clock request up to, and EXCEEDED
  one MHz past that (5228/5229 pin the boundary; 5230 is the 2957-vs-3652
  pre-fix reading). A refused clamp (5231) and a missing clock reading (5232)
  can never read as HELD. `apply_clock_witness_load_is_meaningful()` gates on
  20 % utilisation and never upgrades an *unknown* reading to load (5239) —
  which is precisely how the 2026-09-13 post-fix run looked clean while being
  idle.
- **5240-5241 — pre-arm samples are not judged.**
  `apply_clock_witness_counts_toward_verdict()`. The witness samples once before
  the clamp is armed, to record the outgoing profile's clock; folding that into
  the judged peak would report EXCEEDED for a clock the clamp was never in a
  position to cap. Found as a live near-miss: two of four under-load runs peaked
  at `apply entry` (2932 MHz against a 2957 MHz ceiling), so a slightly higher
  outgoing profile would have cried wolf in the one line meant to be
  authoritative. **5242-5243** extend the same exclusion to the sample taken in
  the instant the arming call returns, which is still the pre-clamp clock: three
  of seven full-load runs were judged on it, each reading exactly its pre-arm
  value. Five of eleven real runs were being judged on an unjudgeable sample and
  passed only because the outgoing clock happened to sit under the incoming
  ceiling.

- **5400-5421 — when protection is REQUIRED (CT-01/CT-05).** The predicate is
  no longer "the request named a lock": an unpinned undervolt switching to
  another unpinned undervolt arms at the outgoing curve peak (5400-5404,
  open-ended form only, since a floor would raise an unpinned profile's idle
  clock), while a purely positive outgoing offset arms nothing because the reset
  only lowers (5405). The ceiling is `min(outgoing, incoming)`: a low-pin →
  high-pin switch clamps at the OLD pin until the new curve verifies (5407-5409).
  `required` and `arm` are separate, so a missing NVML entry point produces a
  transition that must be REFUSED rather than one that proceeds unprotected
  (5412-5416) — and protection that was never required refuses nothing, which is
  what keeps unsupported GPUs writable (5417-5418).
- **5430-5438 — releasing a restriction needs proof (CT-04/CT-07).**
  `apply_recovery_permits_release()`: an attempted-and-unverified curve or GPU
  offset reset blocks the release, an un-attempted domain does not (nothing was
  raised there), and a cap that survives recovery must be reported rather than
  left silent.
- **5440-5445 — recovery keys off the ATTEMPT (CT-04).**
  `service_apply_core_requires_recovery()` fires at (attempted, 1 failure),
  where the old `service_apply_core_requires_mixed_failure_rollback(0, 1)`
  returned false — the first core write that mutated and then failed used to
  leave the hardware partial with nothing running to undo it. 5441 asserts the
  two rules genuinely disagree there.
- **5450-5472 — ONE VF offset range (CT-03).** `vf_offset_range_policy.h`. The
  invariant is structural: `permits(flatten_floor(r))` holds for every range the
  header can produce, so the pre-fix state — a planner generating a -1,000,000
  kHz floor that its own 500,000 kHz pre-check then refused, with the refusal
  reported to the user as a successful apply — is unrepresentable rather than
  merely fixed. Plus asymmetric ranges, inverted/unprobed fallback, positive-only
  ranges, and the clamp's alteration reporting.

**Verified empirically in the failing direction.** Moving
`LINUX_MUTATION_LOCK_CEILING` to the end of `linux_execute_transaction()`'s
order array fails the suite with code 5218. For the 2026-09-16 work, three
pre-fix rules were re-simulated in place and each produced the expected failure
before being restored: the lock-only predicate → **5400**, the unconditional
post-recovery release → **5431**, the 500,000-vs-1,000,000 kHz range
contradiction → **5451**. No simulation code remains in the tree.

**What these tests do NOT cover.** They exercise the pure decisions the apply
consumes, not the call sequence it emits. Nothing here executes the real Windows
or Linux apply orchestration against a fake backend, so partial-write failure,
per-write failure injection and intermediate-state assertions remain open — see
the SH-01..SH-42 matrix in `temp/stability-harden.md` (local, untracked) and the open questions in
`llm-wiki/clock-transition-audit.md`. The Windows half is not pure (it
drives NVML through `g_nvml_api`), so its ordering is held by source gates in
`tools/apply_ceiling_gates.py` instead — including a `forbid_text` on the old
`rollback_to_safe_defaults()` gate that released locked clocks only for
`LOCK_MODE_HARD`.

## Path-protection suites (F-SEC-1, added 2026-09-22)

`tests/regression_main.cpp`, exit codes **5500-5558** (Windows walker fixtures,
`#ifdef _WIN32`) and **5560-5623** (host-neutral policy matrix). They were
first written at 5400-5467, which the apply-ceiling suite above already
occupied, so a failure code named two assertions; renumbered 2026-09-22.

- **5500-5540 — the walker against real DACLs.** A temp tree with DACLs the
  test synthesizes: a non-inheritable Users DELETE grant must surface as
  `non_admin_danger`, an inherit-only one must surface only as
  `non_admin_inherit_danger`, an add-file/add-subdirectory grant must surface
  as `non_admin_create_danger` and NOT as substitution danger, a junction must
  read as reparse (including as an ancestor), a file where a directory belongs
  must read as `is_directory = false`, and a UNC path must classify remote
  with zero components probed. Also asserts one component per path level
  (the drive-root walk) and that the WHOLE missing tail is enumerated, not
  just its first component. Verdicts on the temp tree are deliberately not
  asserted: their owner trust depends on the elevation of whoever runs it.
  Every failure path cleans up — a leaked junction keeps resolving to its
  target and a protected temp dir may be undeletable by hand.
- **5541-5558 — read-only probes of the real machine.** `%ProgramFiles%` must
  classify green, or the scheme would warn on the default install path. And
  against the real `%SystemDrive%` root, which on stock Windows hands
  Authenticated Users inheritable Modify: a one-level missing tail must be
  GREEN in preflight (setup creates AND hardens that folder), unproven outside
  preflight, and a two-level tail must warn — the deeper one's intermediate
  keeps what it inherited. The two-level negative is guarded on the root
  actually handing something down, since an administrator may have hardened it.
- **5560-5623 — the policy matrix.** Fail-safe direction first (no facts,
  zeroed facts, unreadable volume, incomplete chain), then each disproof in
  turn, then the preflight leaf exemption in both directions, then the
  remediation string: balanced quotes, every path occurrence quote-wrapped,
  `/setowner` in its own `icacls` invocation with no `/grant` after it, and
  quoted `SID:permission` arguments. That last group exists because all three
  were wrong on first ship and none of them fails visibly — the pasted command
  simply does nothing.

Verified fail-before-fix by reverting each change in turn: the one-level-tail
rule fires **5551**, the remedy spelling fires **5611**. The drive-root
derivation has no behavioural test (creating a directory mount point needs
elevation) and is held by a source gate in `tools/security_gates.py` instead.

The install-location gate has additional read-only Windows tests **5671-5673**
for known folders reached by long and 8.3 names, plus **5680-5683** for an
ancestor junction pointing to the same Program Files directory. Host-neutral
tests **5674-5679, 5684-5687** cover another account's profile root,
conventional/OneDrive shell folders, and allowed dedicated children.
`tools/security_gates.py` also pins
the file-identity and cross-profile checks, because 8.3 names may be disabled
on a CI volume. No test rewrites a real known folder's DACL.

## Read-miss and log-queue suites (F-READ-MISS, F-LOG-ASYNC, added 2026-09-12)

`tests/regression_main.cpp`, exit codes **5154-5166** and **5170-5192**. Both
cover the same incident from opposite ends: a C: volume stall (Windows Volsnap
event 25) blocked a durable debug-log flush the service was making from inside
its serialized dispatch lock for 19.078 s, and the GUI reported a healthy
service as a lost connection. Verified empirically in both directions --
dropping `ERROR_PIPE_BUSY` from the reachability switch fires 5154; making
`fits()` forget the framing bytes fires 5181.

- **5154-5158 — connect errors classify into reachability.** `ERROR_PIPE_BUSY`
  and `ERROR_SEM_TIMEOUT` mean the pipe object EXISTS and every instance is in
  use, so they are positive evidence of a running service (the Windows analogue
  of Linux's `EAGAIN` on a saturated backlog). Only `ERROR_FILE_NOT_FOUND`,
  `ERROR_PATH_NOT_FOUND` and `ERROR_ACCESS_DENIED` mean offline.
- **5159-5161 — only UNREACHABLE means offline.** UNKNOWN (nothing attempted)
  must not, or a queue/allocation failure would look like a stopped service.
- **5162-5164 — the incident shape itself.** Request submitted, service
  connected, response deadline expired: the presentation is KEPT. An
  unreachable pipe is the only case that tears it down, and the decision must
  not depend on how long the read took — a saturated pipe fails immediately and
  is still a present service.
- **5165-5166 — the initial outcome asserts nothing.** A send that never ran
  reports UNKNOWN and an unsubmitted request.
- **5170-5174 — ring occupancy is exact.** Monotonic byte counters, so "full"
  is never confused with "empty", and framing is charged to the producer (a ring
  that ignored `kHeaderBytes` would overrun by exactly that much).
- **5175-5181 — the admission rule.** A ring too small for one maximum line
  refuses everything; zero-length and over-length payloads are refused rather
  than truncated (a clipped line reads as complete and lies); and the overflow
  boundary is exact, because a full ring must DROP rather than make the producer
  wait for the writer — waiting is the coupling the whole change removes.
- **5182-5187 — wrap arithmetic.** Records may straddle the seam, so the second
  half continues at offset 0 instead of the record being refused or padded.
- **5188-5192 — what the lock-free crash drain depends on.** A length it cannot
  trust must stop the walk instead of letting it read past the committed region,
  and the shutdown join bound is non-zero so a stalled volume cannot hold the
  process open.

Source gates rather than tests, because they guard negatives that leave the
build and the log working when they regress: `tools/log_gates.py` (the producer
may not call `WriteFile`, `FlushFileBuffers`, `OutputDebugStringA` or
`GetFileSizeEx`, nor take the log FILE lock) and
`check_read_miss_is_not_a_disconnect()` in `tools/ui_gates.py` (the read
completion must delegate to `gui_service_handle_read_miss()`, and a stale read
must not reach `gui_mutation_advance_gpu_epoch`,
`gui_invalidate_live_authority` or `gui_service_model_disconnect`).

**Not covered by a test:** the ring's runtime concurrency (producer/writer
interleaving, the crash drain against a live producer). The pure suite asserts
the arithmetic those depend on; the threading itself is argued from the
one-way lock order, not measured.

## Pipe deadline-contract suite (F-PIPE-DEADLINE, added 2026-09-11)

`tests/regression_main.cpp`, exit codes **5072-5096**. Every assertion fails
against the pre-fix behaviour; verified empirically by reverting each half of
the fix in turn (5072 fires for the deadline derivation, 5082 for the
identity-evidence rule).

- **5072-5073 — the literals cannot come back.** The derived telemetry and
  snapshot deadlines must exceed the 500 ms / 2000 ms constants they replaced.
  These are the exact numbers that made a healthy service present as a lost
  connection 29 times in ~36.7k telemetry reads.
- **5074-5075 — a deadline must exceed the server's own budget.** Otherwise an
  expiry says nothing about service health and only proves the client was
  impatient.
- **5076-5077 — the handler budget contains the runtime-lock wait**, not just
  the refresh after it, and a full snapshot is budgeted above a telemetry read.
- **5078-5079 — serialization is accounted for.** Telemetry shares one dispatch
  lock with GET_SNAPSHOT, so its deadline must survive being queued behind one.
- **5080-5081 — availability and turnaround are separate budgets.** The connect
  timeout is non-zero and strictly below the response deadline; conflating them
  is why the response deadline could not previously be raised.
- **5082-5087 — connection identity is evidence-driven.** A failed request
  leaves the tracked service instance untouched (5082/5083, including a
  response struct that happens to carry a stale id); a successful envelope
  adopts what it names, restart included (5084-5086); a failure before first
  contact manufactures nothing (5087).
- **5088-5096 — phase arithmetic.** `service_phase_remaining_ms()` with
  `totalTimeoutMs == 0` never charges connect time against the response phase
  (the async lane), and with a non-zero total always returns the tighter of the
  two — which is what keeps the split from doubling any synchronous caller's
  worst-case stall.

The transport itself (`service_send_request_deadlines()`) is a Win32 shard and
is not directly asserted; it delegates its arithmetic to the pure helper above
precisely so the decision is covered.

## Audit follow-up suite (added 2026-09-23)

`run_audit_followup_tests()` in `tests/regression_main.cpp`, called from
`main()` right after the clock-transition fixture (its own frame, per the ASan
stack note). Exit codes **5810-5857**:

- **5810-5821** `gc_crash_module_is_nvidia_control_library()`: x64 shim,
  `_impl`, ARM64, NVML variants, case, both separators; not our binaries,
  `nvcuda.dll`, a directory named `nvapi`, a `.bak` suffix, empty/null.
- **5825-5834** correction budget: derived from the handler budget, pass 1
  never refused, the boundary is inclusive, fallback deadline exclusive and 0
  unbounded.
- **5836-5841** `gui_apply_shape()`: only power (+fan) may go sparse; every
  clock domain, including memory, forces FULL.
- **5845-5850** advanced-domain ownership relaxation: stock only, relaxed read
  only, one direction only.
- **5852-5857** `desired_claims_advanced_clock_domain()` covers all four fields.

Source gates for the call sites (budget wiring, Linux placeholder rule, Reset
NVML failure, VEH predicate, GUI shape) are at the end of
`tools/apply_ceiling_gates.py`; the XBAR shortcut gate in `tools/xbar_gates.py`
and the F-DRIFT-1 baseline gate in `build.py` were re-pointed at the new forms.
The apply orchestrator itself still has no executing test.

`run_ownership_handback_tests()` (own frame, called from `main()`), exit codes
**5860-5899 and 5990-5994**: the startup plan matrix (no marker, corrupt,
unknown boot, other boot, same boot, one retry after a died attempt, give-up at
the attempt budget, NONE scope), the v28 lockout reason's range, the wedge
watchdog decision and progress-age arithmetic, the Windows scope choice, the
marker-retirement rule, and both on-disk marker records (field validation,
Linux boot-id termination/printability, size). Mutation-verified: disabling the
in-flight give-up fails 5867. Ordering gates for the runtime halves (in-flight
before the first write, fan before reset, handback before any other startup or
lifecycle write, ownership recorded at every pre-write boundary) are at the end
of `tools/apply_ceiling_gates.py`. The Windows/Linux handback runtimes
themselves have no executing test.

`run_fan_worker_lifecycle_tests()` (own frame, called from `main()`), exit
codes **6300-6332**: `fan_worker_stop_plan` (a self-requested stop is
signal-only, every other stop joins without releasing the runtime mutex),
`fan_worker_ensure_plan` (reap an exited worker, join a retiring one, never let
the worker replace itself), `fan_worker_lock_wait_outcome` (stop wins over the
mutex; abandoned mutex surfaced; event-abandoned/timeout/failure all fail), and
`update_install_fan_policy.h` (capture only a manual runtime, clamp the fixed
duty, restore only after release and never twice). Mutation-verified: making
the self-stop a join fails 6300. Call-site gates are
`fan_gates.check_service_fan_worker_serialization()` and the install-fan
additions in `tools/update_gates.py`. The threaded runtime itself (worker,
lifecycle worker, updater contending for the mutex) has no executing test.

## Installer policy suites

- **6100-6111 -- install transaction order.** Failure injected at each
  replacement position, record and registration -> exactly one rollback with
  the right record scope; a failed service STOP (6109) runs only the separate
  stop-failure recovery (no rollback, no replacement), and 6110-6111 pin its
  decision table (restart a previously running service that went down, leave
  an active one, never stop it again). See [installer](installer.md#upgrade-rollback).

`tests/regression_main.cpp` covers the setup program's pure policy on both
hosts (the headers are Windows-free):

- **1750-1775 — payload container.** CRC-32 check value, payload-name safety
  (separators, drive letters, wildcards, `.`/`..`, trailing dot or space),
  container validation against flipped payload bytes, entries overlapping the
  directory or running past the end, duplicate names, truncation, bad counts,
  bad magic, and every `GcPayloadFooter` rejection including the
  allocation-ceiling check that runs *before* the size is used.
- **1776-1784 — command line.** `/S`, `--silent`, `/D=`, `--dir`, a dangling
  `--dir`, an unknown switch (must be rejected, not ignored — a typo in an
  unattended update has to fail loudly), the shortcut toggles, `--uninstall /S`,
  the `--uninstall --dir` contradiction, and `/?`.
- **1785-1800 — install plan.** Path equality across case and trailing
  separators, the default-directory join, directory acceptance (drive roots,
  relative paths, `..`, wildcards, UNC, a leading-dot component), fresh-install
  defaults, silent flipping the launch default, in-place upgrade, remembered
  shortcut choices and command-line overrides, a move re-pointing the service, a
  hand-registered portable copy, and a rejected directory producing an invalid
  plan with a reason instead of a silent fallback. The move-cleanup flag is
  set only for a changed path recorded by setup, not an SCM-only portable copy.
- **5690-5707 — moved-folder cleanup on Windows.** Resolved and original access
  path ancestry is checked in both equal/parent and sibling directions, including real volume
  GUID paths from directory handles. A unique temp fixture
  proves the cleanup deletes a setup-owned filename, preserves an extra file
  and its directory, then removes the directory once only setup-owned files
  remain. A directory occupying an owned filename is retained. The full retire
  sequence is source-order gated and has not been
  exercised by a live installation move.
- **1801-1805 — upgrade apply preconditions.** The wire contract a restore must
  satisfy. **These passed for weeks while the restore was broken**: they build
  requests by hand and assert the *validator*, so they said nothing about the
  client that actually builds the request. 2006-2016 below close that gap; treat
  a "contract" suite without a producer-side counterpart as untested.
- **2021-2025 — which binary is asked for the live settings.**
  `gc_version_at_least()` on `0.20`/`0.21`/`0.21.4`/`1.0` and on unreadable
  input (`""`, `nullptr`, `dev`, `0`, `0.` — all "too old", never probed by
  running them), and `captureFromInstalledBinary` for an in-place 0.21 upgrade,
  a hand-registered portable copy with no recorded version, an ARP version
  describing a different directory than the SCM service directory, and a 0.20
  install.
- **2036-2040 — the recorded capability outranks the version.** A version
  answers "does this build know `--export-active-settings`?" only at release
  granularity, and it was already wrong inside 0.21: the verb arrived with the
  installer, twelve commits after `VERSION` moved to 0.21, so builds exist that
  satisfy `≥ 0.21` and open their window when handed the argument. The suite
  pins both directions of the marker — `GC_TOGGLE_ON` promotes a build the
  version would have rejected, `GC_TOGGLE_OFF` refuses one the version would
  have accepted (the case that was broken) — plus the `UNSET` fallback for
  installs predating the marker, and that the marker never overrides the
  same-directory or service-registered rules.

- **2041-2050 — what an uninstall is allowed to remove.** Removal is the one
  operation where matching too widely cannot be undone, so each predicate in
  `installer_uninstall_policy.h` is pinned in both directions. The logon task
  name: an anchored case-insensitive prefix with a non-empty user part, and
  never a substring (`Backup before Green Curve Startup - nightly`), never the
  bare prefix, never an unrelated task. The `HKCU\...\Run` value: the value name
  alone is a plain product string in a shared key, so the command's first
  executable token must have the exact `greencurve.exe` leaf. Valid quoted and
  unquoted paths match case-insensitively; `notgreencurve.exe`,
  `greencurve.exe.backup`, malformed quoting, and argument-only mentions
  survive. Self-deletion: the installed copy
  matches across case, forward slashes, and one trailing separator, while a
  setup stub in a downloads folder, a copy in a subdirectory, and a sibling
  `Green Curve 2` do not — the unconditional version used to schedule the user's
  own setup file for deletion.
- **3236-3239 — the wizard's click filter (added 2026-08-01).** A fast
  double-click on a `BS_OWNERDRAW` action button arrives as `BN_CLICKED` then
  `BN_DBLCLK` (native fixture above), so the old `BN_CLICKED`-only filter
  dropped the second click. `installer_ui_click_policy.h` accepts `BN_DBLCLK`
  on action buttons and keeps checkboxes one-toggle-per-gesture; the cases pin
  both directions plus inert foreign notification codes, and
  `installer_ui.cpp` static-asserts the host-neutral codes against winuser.h.
  The stop-step fail-closed arms have no pure counterpart (they are Win32
  handle sequences), so they are pinned by the F-STOP-GUI-OPENFAIL /
  F-STOP-SVC-OPENFAIL / F-STOP-GUI-ENUMFAIL / F-STOP-GUI-WAITFAIL /
  F-STOP-GUI-PROBEFAIL / F-STOP-TERM-WAIT source gates in
  `tools/installer_build.py` on the new `installer_stop.cpp` shard.

Source gates live in `tools/installer_build.py:check_all()` and cover what has
no unit test: the shared palette, the setup program's independence from
`app_shared.h`, the Compression-API choice, secure-root validation before any
live-install disruption and in the folder UI, the remaining upgrade step
ordering (anchored to `gc_install_execute`), the service-registration read-back, unelevated GUI
launch, the settings transfer using an explicit CLI origin, and the uninstall
ordering (anchored to `gc_uninstall_execute`): autostart removal and the
service-process wait before the file deletions, restart scheduling of the
running uninstaller before `RemoveDirectoryW`. Two of those gates exist because a
plausible-looking edit would silently restore the old behavior — no source file
may use the running-image unlink APIs (`FileRenameInfo`,
`FILE_DISPOSITION_FLAG_POSIX_SEMANTICS`, `FileDispositionInfoEx`; an antivirus
behavior-rule trigger, see [antivirus-heuristics.md](antivirus-heuristics.md)),
and `-ltaskschd` must stay out of `INSTALLER_LINK_LIBS` (Zig's arm64 link step
cannot resolve it).

## Updater policy suites

`tests/regression_main.cpp` assertions **4100-4366**, plus source gates in
`tools/update_gates.py`. Split by what they defend
([updates.md](updates.md) has the reasoning):

| Range | Covers |
|---|---|
| 4100-4119 | Version grammar and ordering, including the **downgrade refusal**. Neither the signature, the GitHub attestation nor the digest rejects a replayed older release — it is genuinely signed, attested and correctly hashed — so this comparison is the only control that does. Also: an unparseable version can never look newer than a real one, in either position. |
| 4120-4149 | The manifest grammar. Unknown and duplicate keys refused rather than ignored; partial per-arch triples refused; and the **mix-and-match** cases — a correctly-formed arm64 name in the x64 slot, and an older asset renamed into a newer release — which only the name binding catches. |
| 4150-4174 | The URL allowlist and redirect budget. Includes `https://github.com@evil.example/a`, the authority that defeats a naive "starts with https://github.com" check. |
| 4175-4199 | The schedule (backoff monotonic and capped at the steady-state rate, clock-moved-backwards is due) and every arm of the install gate. |
| 4200-4229 | The v19 wire rules, enumerating each refused field **one at a time** so a field that stops being checked names itself. |
| 4230-4249 | **The signer and the verifier agree.** A known-answer test across the boundary: `tools/update_signing.py` signs in pure Python, the service verifies with CNG, and the two must agree on curve, hash, signature encoding and byte order. A disagreement is invisible until it is total — every published update refused in the field, with nothing failing locally. The key is the published RFC 6979 A.2.5 vector, so no secret is involved. Confirmed live on 2026-08-14 by corrupting the fixture signature and observing 4233 fail. |
| 4250-4259 | Strict base64. A permissive decoder accepts several spellings of one signature, and "which bytes were signed" must have exactly one answer. |
| 4260-4269 | The installer command line, built and then split with the **real** `CommandLineToArgvW` and fed to the **real** `gc_installer_parse_options()`. Reintroducing `/D=` fails it. |
| 4270-4289 | The version/freshness-bound settings restore and the `--launch-session` grammar. |
| 4290-4299 | `ServiceUpdateState` as a wire block: enum ranges, interval bounds, reserved bytes, terminated strings. |
| 4320-4359 | **Whether the user is told at all.** Which decisions raise an alert (`MANUAL_REQUIRED` does, `NO_ASSET` deliberately does not), both tray captions, the tooltip suffix and its suffix-wins truncation, and every gate on the once-per-machine auto-check question. |
| 4360-4366 | The persisted last-check timestamp's two-halves round trip, including 2038, where the low half no longer fits a signed int. |
| 4370-4394 | **The response read loops**, driven by a fake transport. Chunked delivery, the caller-buffer ceiling (refused, never truncated), empty and stalled bodies, short files, and the mid-transfer size abort. 4384 measures **what the sink received**, not the return value: a loop that writes first and checks after still returns `TOO_LARGE`, so only the measurement catches it. |

The source gates carry the properties a test cannot see, because they all leave
the happy path working: the signature is verified **before** the manifest is
parsed (for a cached manifest as well as a fetched one), the staged installer is
re-verified **before** `CreateProcessW` through a handle that denies writes, and
no client-supplied target can enter an update request. Also enforced there: no
TLS-bypass flag, no temp-folder staging, no plaintext URL literal, no persisted
*conclusion* in the manifest cache, and — since 2026-08-15 —
`check_update_is_actually_surfaced()`, which pins each of the three passive
alert surfaces, and — since 2026-08-29 — `check_update_envelope_is_authorized_only()`,
which pins the update-state stamp inside the pipe dispatch's
`stateEnvelopeAuthorized` gate, so a caller that fails the session/PID/integrity
gates never receives the machine's update posture.

### What is NOT covered, which is most of the runtime

The **pure policy layer is thoroughly covered**: unit tests on both hosts, a
dedicated fuzz target, and three cross-boundary tests that run the real other
side (the real installer parser, the real Python signer, the real CNG verifier).
Every `gc_update_*` policy function is exercised directly or through a caller.

The **orchestration layer has no automated tests at all**. These shards are
covered only by structural source gates, which prove text order, not behaviour:

| Shard | What is unasserted |
|---|---|
| `main_service_update_fetch.cpp` | The WinHTTP session and the hand-rolled redirect loop. The two **read** loops moved behind a transport seam on 2026-08-15 and are now asserted (4370-4394); the connection half is deliberately still untested — see [updates.md](updates.md) for why that trade was taken. |
| `main_service_update_worker.cpp` | check → download → verify → stage → install, and every failure arm. |
| `main_service_update_worker_thread.cpp` | Worker start gating, failure counting, the staged-package revalidation branch. |
| `main_service_update_gui_stop.cpp` | Cross-session process enumeration and its fail-closed arms. |
| `main_service_update_cache.cpp` | The restart round trip, including re-adoption of a staged package. |
| `main_service_update_state.cpp` | Settings round trip (except the timestamp split), staging-directory creation and its DACL. |
| `gui_update_client.cpp` / `_dialog.cpp` | The SRWLOCK response cache, the one-shot shutdown post, control projection. |

This is the same gap [updates.md](updates.md) records as "the install path has
never run on a real machine", stated from the testing side. Closing it needs
either a fake transport seam in the fetch layer or an end-to-end harness against
a local release fixture; neither exists, and the structural gates were chosen
deliberately as the cheaper control for the properties that matter most.

## Open questions / stale-risk

- `run_source_regression_checks()` is still ~2700 lines of ~1000 text
  assertions inside `build.py`. Moving it into an importable `build_checks/`
  package is the remaining half of the extraction; `BUILD_SCRIPT_SIZE_RATCHET`
  should drop again when that lands.
- The harness was moved out **verbatim** as one file. Splitting it into
  per-area shards (`regression_fan_config.cpp`, `regression_protocol_ipc.cpp`,
  …) each exposing `run_<area>_tests()` is now a safe follow-up, because it is
  ordinary C++ refactoring with compiler help rather than editing a Python
  string. The acceptance criterion stays the same: identical exit codes.
- Text-based source guards remain inherently brittle. `require_text_in_surface`
  softens the worst case (module splits), but a guard still breaks when a
  string is legitimately reworded. Prefer compiled/pure coverage where the
  behaviour can be expressed as a testable function.
- `task_xml` remains Windows-only (see "Linux host" above); unlocking it means
  making `main_startup_task_definition.cpp` build without `windows.h`, which is
  a much larger job than the parser hoist was and has no second payoff.
- **The fuzz harnesses stop at the IPC validator.** The fan-curve overflow
  (fan-control.md) sat *downstream* of `validate_service_request_for_ipc()`,
  which accepts a curve with fewer than two enabled points, so no amount of
  `service_request` fuzzing could have reached it. A target that drives the
  daemon's post-validation request handling — normalize, curve target
  construction, the mutation path — would cover a class the current five cannot.
  This is the most valuable remaining fuzz work.
- No duplicate-symbol audit exists. Both duplications were found by reading, not
  by tooling. A check comparing definitions across the Windows and Linux source
  lists would catch the next one mechanically.
- The Linux fuzz/ASan path depends on a host `clang++` rather than a pinned,
  digest-verified toolchain, so it is deliberately not part of the hermetic
  release build — only of the gates. Plain `--test` still uses the pinned Zig.
- The bounded 20000-run gate is not folded into local `--test`; it remains a
  separate `--fuzz` invocation because adding it would lengthen every quick
  test. CI now invokes fuzz explicitly on both native hosts, so the separation
  no longer leaves the merge/release workflow ungated.
- No corpus minimisation step exists. Seeds are hand-written; libFuzzer's
  coverage-increasing finds are discarded with the scratch directory rather
  than being merged back. If a target's coverage plateaus low, merging a
  reduced corpus back into `tests/fuzz-corpus/` is the next lever.

## Last Verified

- 2026-09-15 (second pass, CI run 34997735796): the same root cause one level
  up. `--fuzz` builds the Linux targets only on a Linux host, so
  `FUZZ_LINUX_EXTRA_SOURCES` was unproven off CI and `service_request` was
  missing `fan_curve.cpp` + `config_text_utils.cpp`. Added the entry plus
  `check_fuzz_linux_link_lines()`, which cross-links every Linux fuzz target
  during `--test` on non-Linux hosts. Verified failing by deleting the entry.
  Green locally: `--test`, `--test --asan`, `--tidy` (no new findings, 34
  baselined), `--fuzz --fuzz-runs 5000`, `--check --target all`, full build.
- 2026-09-15: the Linux fixtures are now cross-LINKED on non-Linux hosts, not
  compiled to an object, and their extra link units are declared in
  `security_gates.LINUX_FIXTURE_EXTRA_SOURCES` (transport: `fan_curve.cpp`,
  `config_text_utils.cpp`). Fixes CI run 34984381401
  (`undefined symbol: fan_curve_normalize_for_ipc`), whose real root cause was
  the compile-only cross path hiding the whole undefined-symbol class from the
  development host. `python build.py --test` green, full `python build.py`
  green (6 targets). Verified failing by emptying the transport fixture's table
  entry: the exact CI diagnostic reproduces on the Windows host.
- 2026-09-12: added the F-READ-MISS reachability suite (5154-5166) and the
  F-LOG-ASYNC log-queue suite (5170-5192), plus `tools/log_gates.py` and
  `check_read_miss_is_not_a_disconnect()`. `python build.py` green on all four
  targets, `python build.py --test` green, `python build.py --tidy` no new
  findings (38 baselined). Verified both directions by reverting each half
  (5154, 5181). **Two pre-existing breakages fixed on the way:** `--test` did
  not build on `main` -- regression 5116 still called the pre-split
  `linux_daemon_serialization_budget_ms()` with no argument -- and the Linux
  recovery-deadline gate matched a comment phrase that line-wrapped, so it could
  never pass; it now matches the `static_assert`'s own failure message. Not
  re-observed on live hardware: the trigger is an external volume stall, so
  absence over a session proves nothing.
- 2026-09-11: added the F-PIPE-DEADLINE suite (5072-5096) for the client/server
  deadline contract, the split connect/response/total budgets, and the
  identity-evidence rule. `python build.py`, `python build.py --test`, and
  `python build.py --tidy` (no new findings, 38 baselined) passed. Verified by
  reverting each half of the fix and confirming 5072 and 5082 fire. Not
  re-observed on live hardware yet: the incident is intermittent (29 in ~36.7k
  polls), so absence over a short session proves nothing.
- 2026-09-09: added the power-target control-surface suites (5040-5071,
  4756) and `tools/readback_gates.py`; updated assertion 72, which pinned
  the old unknown-power `0 -> 50` clamp. `python build.py --test` and the
  full `python build.py` matrix passed. No laptop GPU available locally, so
  the fix is covered by pure regressions and source gates only.
- 2026-08-22 correction: removed pipe SDDL cases together with the failed
  pre-read identity design; retained malformed GitHub-workflow structure, Linux
  INI limits, owner-only logs, and auto-profile redaction coverage.
  `python build.py --test` and Windows x64 check passed after the correction.
- 2026-08-22: added audit-hardening coverage for malformed GitHub workflow
  structure, Linux INI limits, owner-only logs, and auto-profile log redaction.
  `python build.py --test`, `--test --asan`, `--tidy`, bounded fuzzing, CET
  instrumentation, toolchain verification, and Windows/Linux x64 checks passed;
  live multi-user pipe switching and NVIDIA hardware journeys remain runtime
  validation.
- 2026-08-23: reworked XBAR ClkDomains coverage for version-keyed schemas:
  unknown-version-word refusal (no SET may fire), `-9` struct-version
  rejection with stale-proof invalidation, schema-table lookups, tri-state
  snapshot status, absolute field offsets, plus the prior pinned-schema/
  decoy-layout cases (4500–4524). `python build.py --test`, `--test --asan`,
  and a live RTX 5070 read-only `--self-test` pass; pre-Blackwell generations
  remain fail-closed pending a real older-generation response report.
- 2026-08-22: added protocol-v20 exact-size/previous-prefix cases, diagnostics
  privacy policy tests, updater worker recovery policy tests, and XBAR pinned
  schema/decoy-layout coverage. `python build.py --test`, `--tidy` (no new),
  ASan+UBSan, bounded fuzzing, and the six-target check matrix pass; live GPU,
  install, uninstall, and updater journeys remain not rerun.
- 2026-08-02 (tray OC/fan class, build 549): added pure cases 4031-4036 for
  `gui_tray_live_state_has_custom_oc()`, which now includes an active applied
  lock/pin in the OC domain. The live-state helpers moved from
  `main_fan_runtime.cpp` to `tray_presentation.cpp`, and
  `check_apply_in_flight_presentation` gained source gates for the pure
  classification and the lock-aware check. `python build.py --test`, `--tidy`
  (38 baselined, no new), and a full six-target `python build.py`
  release/package matrix pass at build 549.
- 2026-08-02 (lock-anchor field re-state, build 548): added the
  `check_pending_changes` order gate that requires
  `populate_desired_into_gui()` to re-state the profile's lock anchor MHz in
  the field and GuiDraft after `apply_lock()`'s refresh, so the stale
  previous-profile draft cannot stay in the VF MHz box. `python build.py
  --test`, `--tidy` (38 baselined, no new), and a full six-target
  `python build.py` release/package matrix pass at build 548.
- 2026-08-02 (ownership-release preview, build 547): added F-PENDING curve
  release cases 4024-4026 and graph-preview release cases 4027-4030, plus the
  `check_pending_changes` source gates for applied HARD-pin ownership folding
  and `releasedToStock` outranking the stale draft. `python build.py --test`,
  `--tidy` (38 baselined, no new), and a full six-target `python build.py`
  release/package matrix pass at build 547.
- 2026-08-01 (pre-ship targeted fixes): added auto-restore gate cases
  3228-3235, installer click-policy cases 3236-3239, the full-sequence native
  owner-draw double-click fixture, and the installer stop-shard gates.
  `python build.py --test`, `--check --target all --arch x64`, `--tidy` (no new
  findings, 38 baselined) and a full `python build.py` at build 535 pass.
- 2026-07-31 (newest): Added the protocol-v18 outcome-severity suite
  (2290-2304) and the manual-result presentation suite (2305-2326), plus
  `ui_gates.check_manual_mutation_result_presentation`. The severity suite is
  worth copying the shape of: it asserts the resolver over **every** status ×
  recorded-severity pair rather than the interesting ones, because the value is
  resolved twice on a mutation path (once by the operation guard before
  persisting, once by the write-out stamp) and a non-idempotent rule would
  corrupt exactly one of them.

  The coherence check in `validate_service_response_for_ipc()` also broke
  several existing fixtures that built non-OK responses without stamping the
  new field. That was the check working: those fixtures were producing bytes no
  real producer can emit. Prefer that over a range-only check.

  Also moved `reset_oc_before_gui_apply()` and the three client file-write
  commands into their own shards to make room under the size ratchet. **The
  second one needed build.py's `_service_server_surface` list updated too** —
  the gate helpers read a concatenated surface, so code moved out of a listed
  file silently stops being guarded. Check that list whenever a service shard
  is split.

  `--test`, `--tidy` (38 baselined, no new) and a full four-target build pass at
  build 526.
- 2026-07-31: Added the crash-artifact policy suite (1900-1963,
  `crash_artifact_policy.h`) plus two new source-gate groups —
  `crash_artifacts.check_windows_crash_artifacts` / `check_linux_symbols`
  (`tools/crash_artifacts.py`) and `linux_gates.check_crash_report`.

  The suite is pure and runs on either host, which is the point: both rules it
  covers are security-relevant and neither is observable after the fact. The two
  assertions that would have failed before the fix are **1904** (a machine-scope
  process that cannot resolve its machine directory must lose the dump rather
  than fall back to the user's, which would put a LocalSystem dump somewhere
  user-readable) and **1942** (rotation must order by the embedded timestamp,
  because `greencurve_crash_` sorts before `greencurve_veh_` for every possible
  date — a whole-name sweep deleted the newest *terminal* crash dump and kept
  stale recovered ones forever; 1941 pins that the trap is real before 1942 pins
  that it is avoided).

  Also asserted: the forbidden `"."`/CWD destinations (1906-1913), the Linux
  directory rule including the refusal of a directory-less config path
  (1914-1926), attribution so rotation never deletes a foreign file (1927-1940,
  1959-1963), and the per-kind budgets (1951-1958). The truncated names in
  1959-1963 are copied into exactly-sized heap buffers rather than used as
  string literals — literals sit adjacent in `.rodata`, so a scan running past
  the terminator would read a neighbour and the assertion would pass either way,
  proving nothing. A sized allocation also matches what the real callers pass
  (`WIN32_FIND_DATAA::cFileName`, `dirent::d_name`).

  The tidy baseline dropped 39 → 38: the `clang-analyzer-deadcode.DeadStores`
  entry for `main_diagnostics.cpp` was *fixed* rather than moved when the crash
  code split out (the terminal `gc_appendf` cursor is now explicitly discarded),
  so the stale baseline line was deleted by hand as the runner instructs.
  `--test`, `--tidy` (38 baselined, no new), and a **full `python build.py`** —
  all six targets plus packaging — pass at build 516.

- 2026-07-31: Added the visibility-neutral projection suite
  (2276-2287), the manual-resync suite (2288-2292), and a second native Win32
  fixture (2275) after Refresh was reported to drop the window out of the
  taskbar list and the tray icon out of its OC/fan theme.

  The native fixture drives a real, off-screen, genuinely *visible* top-level
  window and asserts the mechanism directly: `WM_SETREDRAW(FALSE)` clears
  `WS_VISIBLE` — the bit the shell tracks windows by — while the replacement
  projection leaves it set from beginning to end and still lands its control
  updates. A headless test cannot query the taskbar, so asserting the shell's
  documented *input* is the honest substitute; the mechanism was additionally
  confirmed out-of-tree with a throwaway probe before any code changed.

  The "would have failed before the fix" property is carried by the source
  gates rather than by the pure cases (the transaction itself is amalgamated
  GUI code the fixture cannot instantiate): `tools/ui_gates.py` now forbids
  `WM_SETREDRAW` anywhere in `gui_window_redraw.cpp` and forbids
  `gui_service_begin_full_sync("manual refresh")`. That guard tripped on the
  explanatory comment before it was reworded — the same negative control the
  cue-banner gate got.
- 2026-07-30: Added the auto-profile enable-transition cases (253-255,
  268-272) after auto-switching never fired for a user who enabled it from the
  configuration dialog. **Both new assertions were proven to fail against the
  pre-fix controller** — compiled by shadowing `auto_profile_controller.cpp`
  from a scratch `-I` directory, so the real harness ran unmodified against the
  old logic (253 for the pin taken while disabled, 254 for the missing
  transition). Worth reusing: it verifies "this test would have caught it"
  without touching the working tree. `--test`, `--tidy` (39 baselined, no new),
  and a full four-target build/package pass.
- 2026-07-30 (last): Added the banner-band suite (2259-2274). It asserts the
  invariant that actually broke — the banner never reaches above the header
  strip the GPU selector row owns — across every graph height the layout can
  produce, plus the degenerate cases. Source gates pin the shared top-margin
  constant and the scrolled-content transform.

  **A geometry test would not have caught the original defect on its own**: the
  banner was in the right place *for a window with no children up there*. What
  it missed is that controls are child windows that paint over the parent. So
  the probe was extended to mock the GPU selector row at its real coordinates,
  which is what makes the render meaningful. Reach for that whenever a surface
  overlaps the control tree rather than sitting inside it.

  Also confirmed on real hardware: the settings-transfer identity carry works,
  first reinstall without it and second with, exactly as the asymmetry predicts.
  `--test`, `--tidy` (39 baselined, no new), and a full four-target build pass.
- 2026-07-30 (latest): Added the sweep-geometry suite (2245-2258) covering
  monotonic travel, wrap-around, a frame counter that has run for days, and
  degenerate track widths that must yield nothing to draw rather than an
  inverted rectangle. `--test`, `--tidy` (still 39 baselined, no new), and a
  full `python build.py` across all four targets pass.

  **The banner's appearance was verified, not assumed.** The previous entry's
  in-flight presentation was pinned by tests and gates and was still reported as
  invisible, because "the text is set" and "the user sees it" are different
  claims. So the real `gui_draw_apply_in_flight_banner()` was compiled against
  stubs in a scratchpad probe — the same technique the uninstall self-delete
  used — and rendered to a bitmap. That is repeatable for any GDI surface here
  and costs a few minutes; prefer it over shipping a UI nobody has looked at.

  Still not executable: the service-side identity path (needs a real service and
  GPU) and the settings-transfer round trip (needs an actual upgrade install).
  The first is now confirmed from the installed build's log; the second is
  reasoned plus source-gated.
- 2026-07-30 (later): Added the service profile-identity suite (2210-2227) and
  the in-flight presentation suite (2228-2243), each with a matching source gate
  (`ui_gates.check_service_profile_identity_survives_a_delta_apply`,
  `ui_gates.check_apply_in_flight_presentation`). `--test`, `--tidy` (still no
  new findings over the 39 baselined), and a **full `python build.py`** — all
  four targets plus packaging — pass at build 507, which also clears the "a
  Windows build has not been run" caveat on the entry below.

  **The harness itself was red on master and nobody knew.** `--test` failed to
  *compile*: `repair_profile_locked_curve_readback_artifacts()` had gained a
  `ProfileReadMode` parameter without its harness call being updated, and
  `config_profile_repair.cpp` had started calling
  `gpu_offset_component_mhz_for_point()`, which the pure fixture never provided.
  Both are fixed here (a documented no-exclusions stand-in for the helper, and
  the repair is now called in both read modes, assertion 2244). Treat any suite
  added in that window as never having been executed.

  Coverage honesty: the service-side fix is pinned by a pure decision table plus
  source gates, not by an executable end-to-end APPLY. The two comparisons it
  drives (`load_profile_from_config` + `desired_settings_match_active_service_intent`)
  need a real service process and a real GPU, which the pure harness has
  neither of. What *was* executed against reality is the diagnosis: the root
  cause was read out of a live `greencurve_debug.txt`, not inferred.
- 2026-07-30: Added the applied-profile indicator suite (2054-2068), the VF
  graph axis suite (2069-2081) and the TUI field-selection suite (2082-2099,
  2206-2209), each with a matching source gate. Negative controls were run for
  all three groups (see the F-LNX-TUI entries above; the indicator's drift-free
  read is pinned by
  `ui_gates.check_applied_profile_indicator_is_drift_free` rather than by a
  mutation, because the projection it forbids only misbehaves against live
  hardware). `--test`, `--test --asan`, `--tidy` (no new findings over the 39
  baselined), and `--target linux --check` for x64 and arm64 pass; the
  Linux-host mingw syntax gate is clean over `main.cpp` in both modes,
  `app_shared.cpp`, `service_acl.cpp`, `platform_win32.cpp` and all nine
  installer shards. **A Windows build has not been run**: the installer icon
  fix and the applied-profile fix are both syntax-gated only.
- 2026-07-29: Added Linux VF transition assertions 2051-2053, strengthened the
  uninstall Run-command cases under 2045-2046, and added installer source-order
  gates proving secure-root validation precedes capture/process/service
  disruption and folder acceptance. Fixed all 37 unbaselined tidy findings and
  removed exactly 3 stale entries, leaving 39 unchanged known findings.
  `--test`, both sanitizer spellings, `--tidy`, all-target checks, five fuzz
  targets × 5,000 runs, and full release packaging pass at build 480.
- 2026-07-29: Added the uninstall-removal suite (2041-2050) and its source
  gates. **A unit test could not have caught the actual defect here**: the
  running-image self-delete is a Win32 sequence with no pure counterpart, and
  the published form of it (rename + `FileDispositionInfo`) is silently refused
  on Windows 11 26200. It was settled by building three standalone probe
  binaries, one per combination, and running them — the negative controls
  (`ERROR_ACCESS_DENIED` for POSIX-alone and for the classic disposition) are
  what make the positive result mean anything. The suite covers the pure
  predicates; a source gate pins the API choice the probe established.
  (Superseded 2026-09-23: the in-place self-delete was removed for antivirus
  reasons and the gate now forbids those APIs.)
- 2026-07-29: **`--fuzz` was failing to link on Windows** and had been since the
  `config_text_utils.cpp` split: the Win32 fuzz link is a third source list
  beside `LINUX_SOURCE_FILES` and `WINDOWS_SOURCE_FILES`, and only the first two
  were updated. It stayed hidden because every `--fuzz` run afterwards was on a
  Linux host, which takes `FUZZ_LINUX_EXTRA_SOURCES` instead. The list is now
  `FUZZ_WIN32_SOURCES` and `check_fuzz_target_wiring()` asserts the shared TUs
  are in it. **Run `--fuzz` on the host you are shipping from**; a green run on
  the other host says nothing about this link.
- 2026-07-29: Added the boot-apply carriage suite (2026-2035) and the installer
  capability-marker suite (2036-2040), with matching source gates in
  `tools/linux_gates.py` and `tools/installer_build.py`. Negative controls
  performed on both: disabling
  `service_response_startup_profile_is_coherent()` fails 2030, ignoring the
  recorded marker fails 2036, and re-adding either forbidden line fails its
  gate. `python build.py --test`, `--check` (all four targets) and full
  `python build.py` pass.
- 2026-07-29: Added F-SYNC-STAMP (2006-2020) and the capture-binary suite
  (2021-2025), plus source gates in `build.py` and
  `tools/installer_build.py`. The lesson is recorded above: 1801-1805 asserted
  a wire contract with no producer-side counterpart and passed through three
  broken upgrades. `python build.py --test` and full `python build.py` pass.
- 2026-07-28 (build 25): Moved `parse_fan_value`, `parse_cli_point_arg_w` and
  `config_section_header_matches_ascii` into `config_text_utils.cpp` and put
  that TU in `LINUX_SOURCE_FILES`, which forced out all five of
  `linux_port.cpp`'s duplicate text helpers (they collide at link once the
  shared TU is present — deleting only `parse_fan_value` was not possible).
  Behaviour unchanged; coverage is what moved. Added F-LNX-DEDUP assertions
  1900-1920, un-guarded 21-25 and 523-527 from `#if defined(_WIN32)`, and added
  a source guard against reintroducing any of the five (negative control
  performed). `config_strings` now fuzzes on Linux, so four of five targets do.
  Verified `--test`, `--test --asan`, `--fuzz`, and
  `--check --target linux --arch all`; both touched files also cross-compiled
  clean for `x86_64-windows-gnu`, since the Windows toolchain cannot run here.
- 2026-07-27 (build 11, Linux versioning): Added pure coverage for the shared
  manual-fan write verifier and the power-limit range (returns 1313-1319 and
  1340-1359). The fan cases pin the exact hardware situation that broke Linux
  fan control: a 35% write the driver accepted, echoing intent 35 while the
  measured duty still reads 0 under a zero-RPM fan stop. The power cases pin
  `clamp_power_limit_pct`, `validate_desired_settings_for_ipc`, and
  `normalize_desired_settings_for_ui` — the last of which only became testable
  by moving it out of `linux_port.cpp` (not part of this harness) into the pure
  `source/desired_settings_ui_policy.h`, which is exactly why its 0..100 clamp
  survived unnoticed. New guards live in `tools/fan_gates.py`, moved there
  along with the pre-existing fan failsafe/lifecycle guards to stay under
  `BUILD_SCRIPT_SIZE_RATCHET` (`build.py` was one line over it at HEAD, from
  2884c78). `linux_backend.cpp` was split by extracting
  `linux_backend_nvml_write.cpp` to stay under the source-size ratchet; it is
  registered on the logical backend surface so existing guards still resolve.
  Verified `python build.py --test` and Linux x64 + arm64 builds; the Windows
  target cannot link on a Linux host, so every Windows TU was cross-compiled to
  objects with `zig c++ -c -target x86_64-windows-gnu` instead.
- 2026-07-26 (build 442): Added pure coverage for the themed message box (returns
  1440-1471) and reworked the range-hint cases onto the tooltips after the
  always-visible caption was removed. `gc_message_box` was added to the
  F-PRESENTATION-SILENT token list so renaming the call could not quietly open
  a hole in that gate.
- 2026-07-25 (build 441): Added pure coverage for the advertised overclock
  ranges and the high-overclock confirmation (returns 1400-1435) and moved the
  new source gates into `tools/ui_gates.py`, again to stay under
  `BUILD_SCRIPT_SIZE_RATCHET` rather than raising it. The
  forbid-cue-banner gate was validated by negative control: it tripped on the
  explanatory comment before that comment was reworded.
- 2026-07-25 (build 439): Added coverage-guided fuzzing (`--fuzz`, five targets
  in `tests/fuzz_main.cpp` with committed seed corpora) and the CET
  instrumentation gate (`--check-cet`), both extracted into
  `tools/security_gates.py` to stay under `BUILD_SCRIPT_SIZE_RATCHET` rather
  than raising it. Both gates were validated by negative control (planted
  post-condition violation → exit 77; removed `-fcf-protection=full` → exit 1),
  so neither passes vacuously. Corrected the false CET claim in
  [build.md](build.md): `-fcf-protection=full` *is* effective on our code
  (18/18 address-taken functions instrumented); only the shadow-stack
  CET_COMPAT opt-in is missing, and the earlier "LTO strips it" suspicion was
  disproved. Verified `python build.py --test`, `--test --asan`, `--fuzz`,
  `--check-cet`, and 5.5M total fuzz iterations with no defects found.
- 2026-07-16: Added the Unix-socket sockfs-vs-path regression, exact installer
  pathname-verification stage, NVIDIA vendor/device word-order and NvAPI
  internal/external-ID cases, compatible subsystem forms, and precise conflict
  classification. Normal and ASan/UBSan suites, cross-compiled native fixture,
  Linux x64/ARM64 checks, and full release build 433 passed; on-metal RTX 5080
  validation remains required.
- 2026-07-16: Added protocol-v12 health/domain, deterministic Linux service
  activation/readiness, header-first socket transport, PCI binding/recovery,
  architecture fallback/retention, atomic VF validation/freshness, degraded
  mutation authority, and TUI draft-state regressions. Normal and ASan+UBSan
  suites plus Linux x64/ARM64 warning-as-error checks passed. Full release build
  432 verified every Windows/Linux target and exact-manifest archive. Native
  RTX 5080 acceptance is still required.
- 2026-07-15 (superseded 2026-07-31): Added pure/native coverage for
  presentation-silent hidden redraw transactions. The native HWND fixture
  reproduces the exact top-level
  `WM_SETREDRAW(TRUE)` ghost-window mechanism and verifies hidden projection
  updates without visibility or synchronous paint. Source guards forbid raw
  top-level redraw toggles in accepted service projection and VF-control
  rebuild paths and presentation/focus/process-launch APIs in background
  profile apply/completion. Focused tests, ASan+UBSan, and Windows x64/ARM64
  checks pass; full release build 430 verified every Windows/Linux target and
  archive.
- 2026-07-15: Extended tray-hidden coverage from the `SWP_SHOWWINDOW`
  precondition to the visible-state postcondition. Pure cases cover correction
  and recursion suppression; the native HWND fixture force-adds `WS_VISIBLE`
  without the show flag and proves the next message restores hidden state.
  Source guards pin the WndProc invariant hook, dedicated visibility shard,
  and reentrancy guard. Normal and ASan+UBSan tests, Windows x64/ARM64 checks,
  and the full build/package matrix passed at build 429.
- 2026-07-15: Added pure hidden-intent tray policy coverage and source/order
  guards for pre-display `WM_WINDOWPOSCHANGING` suppression, display/PnP hidden
  reassertion, programmatic authoritative projection, and explicit no-active-
  intent rebasing. `python build.py --test` and the Windows x64 check build
  passed. The full Windows/Linux x64/ARM64 build/package matrix passed afterward
  at build 424.
- 2026-07-15: Release 0.20 added the three-tab responsive Linux cell-grid
  matrix and VF rule/graph checks, plus the exact Windows service-restart then
  late-PnP-start reducer case and change-gated fan/tray-reopen source guards.
  `python build.py --test` and Linux x64/ARM64 check builds passed.
  The full build 422 four-target release/package matrix passed afterward.
- 2026-07-15: Added protocol-v11 envelope/layout/validation/topology cases;
  deterministic service/GUI generation, reducer, draft, coordinator queue, and
  stale-result cases; hidden Win32 state/redraw projection; system-memory DIB
  select/blit coverage; and guards for immutable publication, GUI-thread-only
  adoption, async runtime paths, disposable GDI, and shutdown cancellation.
- 2026-07-13: Added compiled operation tracker/GUI queue, Unicode-path,
  Linux curve-composition, fan-reducer, and state-schema cases. Expanded source
  and order guards cover exact pipe-token identity, PID/integrity rejection,
  impersonation reversal, caller-context output writes, operation query/
  persistence, socket proof, and supervised TUI restoration. Runtime Windows
  token/junction and Linux pseudo-terminal/GPU behavior still require native
  integration environments; the deterministic boundaries are compiled/source
  guarded here.
- 2026-07-13: Added the service-health deferral policy after user logs proved a
  fan-timer/tray ping could mark the serialized pipe unavailable during a
  successful Apply. Sanitizer regressions and all six check targets pass at
  build 414; the complete four-archive release rebuild passes at build 415.
- 2026-07-12: Added explicit owner-draw checkbox model tests and source guards
  after live clicks failed to produce dependable visual/saved state. Normal
  and ASan tests, Windows x64/ARM64 warnings-as-errors checks, and the full
  build-398 release rebuild pass.
- 2026-07-12: Added compiled center/resize/checkbox metrics and shared-GPU INI
  round-trip cases (returns 727–734), plus source guards for placement commits,
  auto-profile theme parity, fail-closed slot/GPU publication, fresh-account
  machine-default resolution, and authoritative restricted-user targets.
  Normal/ASan tests, Windows x64/ARM64 warnings-as-errors checks, and the full
  six-binary/four-archive release rebuild pass at build 397.
- 2026-07-12: Added compiled startup-editor source decisions for disabled,
  invalid, enabled, and logon cases. Source/order guards forbid automatic
  selected-slot restore and require live snapshot receipt before repaint.
  Normal/ASan tests and the full build passed at build 395.
- 2026-07-12: Added compiled VF lock activation/transition/state-stamp tests,
  including a real hidden Win32 owner-draw button notification fixture. Replaced
  the old source-only `HIWORD == BN_CLICKED` assertion with policy-routing,
  one-shot gesture, double-click-pair, centralized-transition, and diagnostic
  guards. Corrected the prior `BN_SETFOCUS` explanation (`BS_NOTIFY` is absent).
- 2026-07-11: Added executable Linux state-record checksum/schema corruption
  cases; exact PCI identity cases for reordered, missing, duplicate, and
  cross-API-mismatched adapters; build-script cases proving `--arch all`
  expands to x64+ARM64 and unexpected `main.lib` payloads are rejected. Source
  guards pin prepared/active/uncertain journaling and atomic rename/fsync. The
  release verifier now checks architecture, PE/ELF hardening, version, private
  build paths, and final ARM64 BTI/PAC/AUT instructions.

- 2026-07-11: Added executable cases for stable GPU selection across adapter
  reordering/missing/ambiguous identities, explicit unavailable logon choices,
  explicit-format unlocked VF curves, atomic auto-rule/hotkey replacement, and
  stable GPU config parsing. Added source guards for coherent profile reads,
  versioned atomic GPU persistence and final-boundary validation, legacy
  multi-GPU fail-closed behavior, redundant lockout persistence, ancestor watch
  recovery, WTS enumeration failure, and checked atomic settings writes.
  Verified with `python build.py --test`, `python build.py --check --target
  windows`, and full `python build.py` (build 384; all four release targets).
- 2026-07-11: Added executable mixed-case INI-section and global-mutex access
  fixtures plus source guards for the least-privilege cross-session lock,
  fail-closed acquisition, locked INI-cache readback, and full-RMW profile
  save/clear locking. Added pure full-restore/named-profile transition and exact
  selected-GPU PnP identity cases. Updated the source-check map for the focused
  service/server/runtime/IPC shards.
- 2026-07-10: Added deterministic lifecycle reducer tests with a fake hardware-
  write counter for task-handoff authorization, WTS/task coalescing, authentication-LUID
  reuse, logoff cancellation, prerequisite failpoints, one-shot standby,
  driver dominance/proof gating, and read-only devnodes. Added a fake unbiased
  clock for 9:59/10:00, sleep exclusion, cross-boot/legacy rejection, fresh
  proof, and recovery-history counting/deduplication policy; executable task-XML
  classification fixtures; failing/successful atomic logon-config transactions;
  shared combo item-data and drift-safe applied-profile metadata cases. SCM,
  helper-process, token/WTS, and persisted-snapshot behavior remains covered by
  source/order guards rather than an executable Windows integration harness.
- 2026-07-04 (build 357+): Added the auto-profile test suite — `F-AUTO-PROFILE`
  compiled tests (returns 220–267: resolver matching/order/require_focus/default,
  controller coalescing/cooldown/manual-pin, config round-trip, hotkey
  parse/format) and the `F-NO-INJECT` + `F-AUTO-PROFILE` source guards. Harness
  now `#include`s `auto_profile_rules.cpp`, `auto_profile_controller.cpp`,
  `hotkeys.cpp`. Verified `python build.py --test` and full `python build.py`
  (all 4 targets). See [auto-profiles](auto-profiles.md).
- 2026-07-03 (build 349): Added the F-LNX-TUI compiled layout test (hitbox lands
  on the drawn bracket + UTF-8 display-column X, return codes 200–214), the
  `require_app_version_fallback_in_sync()` source check (VERSION vs header
  `APP_VERSION` fallbacks), and F-LNX source guards for the TUI (`build_tui_layout`
  usage, `button & 64` / `(button & 3) != 0` mouse filter, `focus_step_vertical`,
  `linux_daemon_apply`, `forbid_text "int y = 1;"`). The harness now
  `#include`s `linux_tui_layout.cpp`. Verified `python build.py --test`,
  `python build.py --target linux --check`, and full `python build.py` (6 targets).
- 2026-07-03 (build 348, superseded 2026-07-10): Added the former
  boot-reconcile tests/guards. They were removed when service-start inference
  was replaced by the real authenticated task handoff; startup is now always
  non-mutating outside validated controlled recovery.
- 2026-06-29: Replaced the no-snapshot startup reset/reconcile source guards with non-mutating startup guards: `main_service_recovery.cpp` must log `no restart snapshot; leaving hardware unchanged`, must not contain the old reset/reconcile hooks, and `main_service_sessions.cpp` must not contain `service start reconcile`. Added the first `WM_LBUTTONDBLCLK` guard for the reported lock-checkbox double-advance symptom, plus ARM64 GUI section-filter source guards. The checkbox input coverage was superseded by compiled policy/native-button tests on 2026-07-12. Verified with `python build.py --test`; full `python build.py` passed build 330. *(Superseded 2026-07-03: the `service start reconcile` / `service_reconcile_active_session_apply` forbids were removed when the gated boot reconcile landed.)*
- 2026-06-29: Added source guards for the app-start active-service skip, change-gated high-frequency GUI diagnostics, tail-drift first/change logging, and the Windows ARM64 service O1/fno-lto workaround. Verified with `python build.py --test`; full `python build.py` passed build 323.
- 2026-06-28: Added regression/source guards for serialized startup reset-before-reconcile, restart snapshot priority, Green Curve-owned fan intent, gated service VF drift reapply, shared architecture-to-backend mapping, same-PCI known-backend retention, and the Windows arm64 GUI O1/fno-lto linker workaround. Verified with `python build.py --test` and full `python build.py` (build 321).
- 2026-06-28: Added GUI lock-checkbox source guards (build 318): anti-aliased FLATTEN tick plus a `BN_CLICKED` gate and cycle diagnostic. The contemporaneous focus-notification explanation was corrected on 2026-07-12 (`BS_NOTIFY` is absent), when the source-only input guards were superseded by compiled policy/native-button coverage. Verified with `python build.py --test` and full `python build.py`.
- 2026-06-01: Added FP-08-003 (build 197 RC6a/RC6b/RC6c) source regression checks: assert the function-level comment block above `service_recover_gpu_connection()` references the RC6 fix, assert exactly one `lock_service_runtime();` before the Phase B `service_safe_close_nvml();` (using a negative lookbehind to avoid matching the substring inside `unlock_service_runtime();`), assert that pre-close `lock_service_runtime();` is positioned before the close in source order, and assert the main loop wedge watchdog in `main_service_server.cpp` no longer contains `service_safe_close_nvml()` between the lock self-test and `launch_recovery_thread()`. Also added the RC6d hermetic compiled lock-serialization test (two `CreateThread` instances against a fresh local `CRITICAL_SECTION`, 200 ms sleeps each, asserts `maxConcurrent == 1` and total wall-clock time >= 350 ms via `QueryPerformanceCounter`/`QueryPerformanceFrequency`). The test adds ~400 ms to the regression test runtime. Verified with `python build.py --test`, `python build.py --test --asan`, and `python build.py --check`.
- 2026-05-20: Added regression coverage for degenerate fan-curve normalization, integer parser overflow/underflow rejection, ASan runtime PATH setup, IPC response validation, size_t-safe file writes, and HeapBuffer overflow-safe bounds checks. Verified with `python build.py --test`, `python build.py --test --asan`, `python build.py --check`, and `python build.py --target linux --check`.
