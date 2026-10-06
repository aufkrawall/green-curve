# Linux Backend

## Arch recipe source verification (verified 2026-10-01)

Source anchors: `packaging/arch/PKGBUILD`, `packaging/arch/PKGBUILD.bin`,
`tools/arch_recipe_checks.py`, `tools/arch_recipe_checks_tests.py`,
`tools/linux_gates.py:check_release_packaging`.
Both recipes must pin every source SHA-256, including source and binary remote
archives, and each local asset's digest must match its current bytes. The
source archive pin for 0.27.0 was established by fetching the published archive
and comparing all 505 regular-file payloads with `git archive` of the reviewed
local 0.27.0 tag (`2acdcad`). It is not inferred from the release filename.
The post-release resume-unit sandbox change left stale local checksums in both
recipes; these are corrected. The source recipe's fail-on-SKIP guard is retained
but its placeholder is replaced with a verified pin, so it is usable again.

The portable gate parses the restricted source/checksum arrays without running
recipe code, enforces counts and real pins, and hashes local inputs. Remote
delivery integrity remains makepkg's job. Focused fixtures exercise stale unit
digests, unpinned remote source/architecture payloads, and missing checksums.
Native `makepkg --verifysource --nodeps` was run in WSL Arch against isolated
copies of both recipes. Source and binary recipes now pass; the latter was
checked with both CARCH=x86_64 and CARCH=aarch64. The published Linux binary
pins previously disagreed with the released archives; each replacement was
established from the actual published bytes, matching its sidecar and verified
by `gh attestation verify` against reviewed commit `2acdcad` and the release
workflow. Never substitute hashes of current local builds for a published tag.
Remote-pin accuracy is verified by this native reproducible command rather
than a network-dependent unit test; deterministic gates verify shape and local
inputs. Packages were inspected for the exact resume-unit bytes/mode 0644.

## Admission, persistence, and trust follow-up (verified 2026-10-01)

Source anchors: `source/linux_daemon_serve.h`, `source/linux_daemon_transport.cpp`,
`source/record_read.h`, `source/linux_daemon_state.cpp`,
`source/linux_service_sandbox.h`, `packaging/arch/greencurve.service`, `packaging/arch/greencurve-resume.service`.
SO_PEERCRED UID is the account quota key; the previous group-key code actually
reset/duplicated the UID string, so the old UID+primary-group description was
incorrect. Changing primary groups intentionally does not buy a new quota.
Missing peer credentials fail closed. Refused peers get one best-effort
nonblocking busy response before any frame read; neither read nor write gets
a refusal deadline in the serialized accept loop. Completed expensive requests
are still serialized and are not claimed to have round-robin fairness.

The shared token policy now refills on admission and saturates overspending to
zero. Refusals cost zero and quiet peers recover from elapsed monotonic time;
refusal traffic is not required to refill. All record reads retry EINTR and
assemble short reads; genuine I/O errors do not prove an active record corrupt
and do not unlink it. EOF/truncation and validated corruption still fail closed.
The portable injected-read fixture exercises EINTR between partial reads,
hard error, and EOF on this Windows host. Linux socket fixtures are cross-linked
here and execute on native Linux CI, not on the Windows host.

Generated daemon and resume units use ONE sandbox header; packaging gates
require matching directives in both packaged units. Resume has no daemon-owned
RuntimeDirectory declaration that could remove a live daemon socket. NVIDIA
device/driver-state access remains intact. The greencurve group means trusted
GPU administrator, including persistent boot settings; revoking membership
does not revoke that saved policy. Clear it separately before revocation.
README, the native installer, and setup output now state this contract.
Desktop Exec output from BOTH the native C++ escaper and Bash setup now
accounts for string decoding before argument decoding: backslash becomes four
backslashes; quote/dollar/backtick receive two. Literal percent remains doubled.
The audit had treated the old native escaper as correct, but the primary Desktop
Entry specification requires this additional layer. Existing C++ expectations
were corrected; source gates execute the real Bash assignments with controlled
metacharacter paths via stdin (avoiding Windows Bash argv quote loss).
Source anchor: `source/linux_asset_escaping_policy.h`.

Root-only startup-policy commands were rejected as an incompatible feature
change. No live GPU or systemd installation was exercised; WSL package-input
verification is described above.

## Current state

Linux is a **native GPU control port**, not a scaffold. It drives the NVIDIA
driver through the same private NvAPI `nvapi_QueryInterface` path the Windows
backend uses — on Linux via `libnvidia-api.so.1` (proprietary driver ≥ 555) —
plus NVML via `libnvidia-ml.so.1`. It reads and applies the full VF curve,
clock/memory offsets, power limit, locked clocks and fan control. A root daemon
owns the GPU; unprivileged TUI/CLI clients talk to it over a Unix socket.

> Write-path correctness is validated on real Linux NVIDIA hardware by the user;
> the code is cross-compiled and link-verified on Windows. The apply pipeline's
> post-write readback verification is the in-built safety check.

## Build

```bash
python build.py --target linux            # build + package linux x64/arm64 payloads under dist/
python build.py --target linux --check    # compile-only verification
```

**glibc-dynamic, not static musl.** A fully static musl binary cannot `dlopen`
(musl's static `dlopen` is a failing stub) and the NVIDIA libs are glibc shared
objects, so the target is `x86_64-linux-gnu`, dynamically linked, `-ldl
-lpthread`. Hardening retained (PIE, RELRO/BIND_NOW, noexecstack, stack
protector, stripped).

## Architecture

Single multi-role binary (mirrors the Windows one-source-set, two-roles model):

- **client** (default / `--tui` / CLI): no driver libs; talks to the daemon.
- **daemon** (`--daemon`, run by systemd as root): owns NvAPI/NVML, applies
  settings, runs the fan-curve reassertion thread, does startup restart-reapply.
- **installer** (`--service-install` / `--service-remove`): stages the daemon
  binary to root-owned `/usr/local/libexec/greencurve/greencurve`, validates
  root-owned non-writable parents, creates/verifies the `greencurve` admin
  group, symlinks `/usr/local/bin/greencurve`, writes the systemd unit, and
  deterministically reloads, enables, restarts, verifies the filesystem socket
  authorization, and verifies the daemon build/protocol (root).

## Command Symlink and User Configuration Policy

- **Command Symlink**: When installed via `tools/greencurve-setup.sh install` or `--service-install`, a symlink `/usr/local/bin/greencurve -> /usr/local/libexec/greencurve/greencurve` is created. This ensures `greencurve` is in `$PATH` system-wide and directly callable from any working directory.
- **Config Path Policy (`source/linux_config_path_policy.h`)**:
  Previously, default Linux configuration was resolved relative to the executable path (`/proc/self/exe`). For system installs (`/usr/local/libexec/...` or `/usr/bin/...`), this directory is root-owned, preventing unprivileged users from saving profiles, settings, or debug logs.
  The pure policy `linux_resolve_default_config_path` checks:
  1. If `config.ini` exists beside the binary in a non-system directory (i.e. not in `/usr`, `/bin`, `/sbin`, `/opt`), it stays beside the binary (preserving portable mode).
  2. Otherwise, for system installs or non-system directories without a local `config.ini`, configuration defaults to `$XDG_CONFIG_HOME/greencurve/config.ini` or `$HOME/.config/greencurve/config.ini`. The directory is created with `0700` permissions.
  This allows non-root users in the `greencurve` group to run `greencurve` from any working directory, save and manage per-user profiles, and generate logs without permission issues.

## Arch Linux Packaging (`tools/arch_package.py` & `packaging/arch/`)

- **Automatic Package Creation**: Running `python build.py` by default on either Windows or Linux builds and packages pacman-installable Arch Linux packages (`greencurve-<version>-1-<arch>.pkg.tar.zst`, along with its `.sha256`) in the release output.
  - Implemented in `tools/arch_package.py`, called from `build.py:package_release_archive`.
  - Assembles all standard directories (`usr/bin`, `usr/lib/systemd/system`, `usr/lib/sysusers.d`, `usr/share/...`), file permissions (mode `0755` for binary and dirs, `0644` for unit/desktop/config files), metadata (`.PKGINFO`, `.BUILDINFO`, `.INSTALL`, and gzip-compressed `.MTREE`).
  - Supports zstandard compression directly on Windows and Linux via Python `zstandard` or system `zstd`, with fallback to `lzma`/`.pkg.tar.xz`.
  - Can be installed directly via `sudo pacman -U greencurve-<version>-1-x86_64.pkg.tar.zst`.
  - **`.PKGINFO` key allowlist (2026-09-06)**: the generated metadata may only use keys pacman's install-time parser (`be_package.c parse_descfile`) accepts — `makepkgopt` was removed from .PKGINFO in pacman 5.0 and there was never an `install` key (the scriptlet is the `.INSTALL` tar member); both warned "unknown key ... in package description" on every install. `validate_pkginfo_keys()` now fails the build on any other key, self-tested from `security_gates.run_build_script_regression_tests()` with a source guard. No `xdata = pkgtype=pkg` is emitted either: valid per the PKGINFO v2 spec, but pacman < 6.1 would warn about it.

Arch Linux packaging template files are provided under `packaging/arch/`:
- `PKGBUILD`: Builds `greencurve` from source using `python build.py --target linux --arch <arch> --no-package`, installs binaries to `/usr/bin/greencurve`, and exports hicolor icons.
- `PKGBUILD.bin`: Packages official release tarballs (`greencurve-<ver>-linux-<arch>.tar.xz`) verified against exact sha256 checksums.
- `greencurve.service`: Hardened systemd daemon unit running `/usr/bin/greencurve --daemon` with sandboxing matching the internal installer (`ProtectSystem=full`, `ProtectHome=yes`, `NoNewPrivileges=yes`, `UMask=0077`, etc.).
- `greencurve-resume.service`: Standby-resume restore unit running `/usr/bin/greencurve --resume-restore`.
- `greencurve.sysusers`: Declarative group registration for `systemd-sysusers`: `g greencurve -`.
- `greencurve.desktop`: Desktop launcher executing `greencurve --tui --from-desktop` with `Terminal=true` and `Icon=greencurve`.
- `greencurve.png`: 256x256 high-resolution application icon rendered deterministically by `tools/icon_render.py`.
- `greencurve.install`: Scriptlet with automatic user detection (`$SUDO_USER`, `logname`) and group enrollment into `greencurve`, instructions to enable both `greencurve.service` and `greencurve-resume.service`, desktop entry notification, `post_upgrade` automatic service restart, and `pre_remove` clean daemon shutdown.
- `README.md`: Step-by-step instructions for `makepkg -si`.

IPC: Unix socket `/run/greencurve/greencurve.sock`. The daemon creates it under
`umask(0077)`, then changes and proves the **filesystem pathname created by
`bind()`** is a root-owned socket with `root:greencurve` mode `0660`; if the
group is absent it deliberately proves root-only `0600`. The socket descriptor
names a separate sockfs inode and is never used as proof of pathname access.
Path operations are relative to an already-proven root-owned mode-`0755`
runtime-directory descriptor. Ownership/mode/type failures abort startup, and
systemd uses matching `UMask=0077` plus `RuntimeDirectoryMode=0755`. The binary
`ServiceRequest`/`ServiceResponse` protocol
from `service_protocol.h` is version 18 on both ends. The fixed eight-byte
magic/version prefix is read before the version-specific body, so an upgraded
client diagnoses an old daemon precisely instead of reporting its short reply
as generic I/O failure. The daemon supplies its version, build, protocol, PID,
a nonzero per-process service instance, and monotonic revisions/generation in
the complete response envelope. The snapshot publisher stamps adapter
identity, active intent, curve topology, applied controls, fan telemetry, and
typed GPU health atomically. `READY` requires a complete fresh VF/control
snapshot; `DEGRADED` can advertise independently safe mutation domains.
Protocol v14 marks every scalar GPU/memory/power/fan readback valid or
unavailable explicitly; a failed read is never filled from configured intent.
Successful partial applies replace their requested domain atomically while
preserving durable intent in every unrequested domain.
The transaction's first phase is `LINUX_MUTATION_LOCK_CEILING` (F-APPLY-CEILING,
added 2026-09-13): when the request declares a lock target, that target is armed
as an NVML locked-clock ceiling *before* anything that can raise a clock, and
`LINUX_MUTATION_RESET_BASELINE` no longer releases the locked clocks it is
supposed to be running under. The previous order — reset (which released the
pin outright), offsets, curve, then lock — left the newly raised curve uncapped
between the curve and lock phases, which is the Windows-measured TDR shape
documented in `llm-wiki/gpu-backend.md`. Arming is best-effort and never fails
the apply; rollback releases a clamp the transaction armed. See
`source/linux_apply_ceiling.h` and `source/apply_clock_ceiling_policy.h`.
For a VF-domain replacement, the hardware transaction is built from the
post-merge committed intent. Any point owned by the previous curve intent but
not by the replacement is included as an explicit zero-offset write in the
same rollback-capable curve phase; durable intent can therefore never forget a
sparse point while its old offset remains active and untracked.
On a multi-GPU system the backend may use adapter 0 for read-only telemetry,
but it does not publish `ADAPTER_IDENTITY` or `READY` until an exact write
target has been selected. The daemon currently owns one selected backend and
one active desired intent. A request cannot switch that backend to another GPU
while intent is active; the user must Reset the owning GPU first. The fan
worker independently checks that the active-intent identity still matches the
selected backend before any reassertion write.
The installer deliberately creates the group but does **not** add the invoking
account: an administrator must grant non-root control explicitly with
`sudo usermod -aG greencurve "$USER"`, then the user must sign out/in (or run
`newgrp greencurve`) before the new membership is effective. Client connection
errors distinguish this permission-denied state from an unavailable daemon.
Peer creds logged via `SO_PEERCRED`; every request clamped by
`validate_desired_settings_for_ipc()`. Socket I/O is nonblocking and bounded by
an **absolute deadline derived per role** in `linux_daemon_deadline_policy.h`
(see "Request deadlines" below): one deadline spans a whole exchange rather than
each transfer, the daemon keeps a small per-peer stall bound, and a client waits
for an answer as long as the daemon is allowed to take producing one.
APPLY/RESET carry a nonzero 64-bit operation ID; the latest 16 outcomes are
deduplicated and queryable, so a client timeout queries the original operation
instead of issuing another hardware write. The
latest correlation is checksummed and atomically persisted to
`/var/lib/greencurve/operation.bin` (record v2); a restart changes an
in-progress result to `OUTCOME_UNKNOWN`. That record carries the answer's
`ServiceOutcomeSeverity` alongside its status, because a
deduplicated retry is answered *from* it and must not report the original
outcome as cleaner than it was; a v1 file fails its version check and is
discarded, which is the pre-existing "no valid persisted result" path. The
severity itself is derived once, at the end of `handle_request()`, by
`service_response_resolve_outcome_severity()` — the daemon's counterpart of the
Windows write-out stamp — so no command handler can forget it. **The Linux
hardware mutation backend has no partial-verify WARNING class**: it is
transactional, so a phase either commits or the whole apply rolls back. Linux
does produce one non-hardware warning when an explicit Apply/Reset commits but
its separate automatic-restore guard cannot be durably re-armed. The Windows
apply backend remains the only producer of partial-verify warnings (see
windows-architecture.md). Committed intent is persisted to
`/var/lib/greencurve/active.bin` as a root-owned, checksummed schema record with
state, size, exact GPU identity, operation correlation, and desired settings.
The current reader accepts the previous schema. Writes use same-directory
temp + file fsync + atomic rename + directory fsync. `prepared`, corrupt, legacy,
uncertain, or GPU-mismatched records never trigger startup writes; only an exact
`active` record can replay. Reset removes the record only after every reset
phase succeeds, and its in-memory merge clears prior curve/VF ownership before
preserving unrelated power/fan domains or adding points from a downstream
request.

The unit is `Type=notify`. Without linking libsystemd, the daemon sends
`READY=1` to `NOTIFY_SOCKET` only after persisted-state replay and successful
socket listening. Installation treats group and `daemon-reload` failures as
fatal, runs `systemctl enable` followed by unconditional `systemctl restart`,
then requires `is-active`, exact `root:greencurve 0660` metadata on the real
socket pathname, and an exact version/build/protocol ping. GPU degradation is
reported as a warning, not an installation failure. Permission
diagnostics show the actual socket owner/group/mode and whether the process has
the supplementary `greencurve` group; peer-credential logs correctly label the
reported GID as the primary GID.

## Stored-unit migration (Linux VRAM offset, 2026-09-07)

Pre-parity builds stored `mem_offset_mhz` in NVML effective MHz (2x display);
the parity fix reinterpreted the field as display MHz, so a stored +2500 would
apply as +5000 effective without conversion. `source/linux_profile_mem_migration.h`
halves every stored value under `[profile1..5]` + legacy `[controls]` exactly
once, stamped by `[meta] linux_mem_migrated`:

- Marker absent = pre-fix effective units (safe: the parity fix shipped
  unreleased after 0.25.0). Marker present = strict no-op, an already-display
  bank can never be halved twice.
- Runs in all three INI load paths AND in `save_profile_to_config_path()`
  before that save stamps the marker — a save must not declare other slots
  display-unit while they still hold effective values. The pure function of
  the file contents makes a failed rewrite fail open (same conversion retried
  next load, same result). A doc without any profile section is untouched
  (loads never fabricate a config).
- Hand-edited unparseable values are left for the load path's strict parse to
  report; the marker still stamps.
- Daemon records migrate in `linux_daemon_state.{h,cpp}`. The unit conversion
  is now a step INSIDE the schema widening rather than an in-place version
  bump, because 0.26.0 changed the record layouts as well — see
  [persistence-schema](persistence-schema.md) for the size-selects-layout rule
  and the current version numbers (`active.bin` v4, `startup.bin` v3). What has
  not changed: a stored checksum is validated over the stored bytes before
  anything mutates, versions at or below `*_PRE_DISPLAY_MEM_UNITS_VERSION`
  are halved exactly once, startup records rewrite eagerly after migration
  (fail-open dlog), and committed state records stay read-only on load (the
  next state transition rewrites them).
- Windows profile code must never read or stamp the marker (gate-enforced);
  the marker lives in `[meta]` rather than a shared-format bump.
- Coverage: F-MEM-MIGRATION returns 5000-5017 in `tests/regression_main.cpp`,
  continued by F-PERSIST-SCHEMA 5018-5057 in
  `run_persistence_schema_tests()`; source gates in
  `tools/linux_gates.py::check_mem_offset_migration` and
  `tools/persistence_gates.py`.

## Source files

| File | Responsibility |
|------|---------------|
| `source/gpu_core.h` | Platform-neutral data model: NVML `nvmlPciInfo_t` matches the real driver ABI (`busIdLegacy[16]` first, `busId[32]` at offset 36) with `offsetof` asserts, plus `nvml_pci_bus_id_text()` and `nvml_resolve_pci_info()`. Also: NVAPI IDs/struct layouts, NVML typedefs, `VfBackendSpec`, `DesiredSettings` + IPC validator, `ServiceRequest/Response`, `NvmlApi`. Shared with Windows. |
| `source/vf_backends.{h,cpp}` | The six per-family `VfBackendSpec` tables + `vf_backend_for_architecture()`. Shared with Windows (one copy). |
| `source/platform.{h}` / `platform_posix.cpp` | OS shim: `pl_lib_open/sym/close`, `pl_open_driver_library` (NvAPI/NVML sonames), `pl_sleep_ms`, atomics, mutex, threads, `gc_*` bounded strings, `pl_run_capture` (POSIX-only since 2026-09-23; Windows has no subprocess capture). |
| `source/win32_compat.h` | Linux opaque stand-ins for the Win32 types in shared headers (so `app_shared.h`/`AppData` compile on Linux). |
| `source/linux_backend.{h,cpp}`, `source/linux_backend_nvml_write.cpp`, `source/linux_backend_discovery.cpp`, `source/linux_backend_mutation.cpp`, `source/linux_gpu_binding_policy.h`, `source/linux_architecture_policy.h`, `source/linux_vf_validation.h`, `source/linux_curve_targets.h` | Stable NVML/NvAPI PCI discovery and same-PCI recovery, architecture retention/fallback, atomic VF validation/publication, pure curve-target composition, and transactional mutation. |
| `source/linux_daemon.{h,cpp}`, `source/linux_daemon_transport.cpp`, `source/linux_daemon_identity.cpp`, `source/linux_daemon_snapshot_runtime.cpp`, `source/linux_mutation_authority.h`, `source/linux_operation_runtime.h`, `source/linux_fan_runtime.h`, `source/linux_socket_permissions.h`, `source/linux_socket_path_permissions.h` | Root daemon socket/dispatch/header-first transport, coherent health/state publication, domain-scoped mutation authority, bounded operation deduplication/query, interruptible fan runtime, and fail-closed filesystem-socket permission configuration/verification. |
| `source/linux_auto_restore_policy.h`, `source/linux_auto_restore_runtime.h` | The pure crash-loop/lockout rule and the single unattended-write path (guard, backend re-preparation, shared reset-to-stock request builder, resume handler). Both boot-apply modes and the resume restore go through the runtime; nothing else may call `linux_backend_apply()` for an automatic write. |
| `source/linux_daemon_client.h` | The client half's shared machinery — which adapter a response is authoritative about, and how a request is built, sent, and its outcome recovered after a transport failure. Split out of `linux_daemon.cpp` for the source-size ratchet. |
| `source/linux_daemon_client_verbs.cpp` | The individual client request verbs (`linux_daemon_apply/reset/resume_restore/..._checked`, `get_state`, `get_state_ex`, `resolve_write_target`), each deciding what its request must be attached to before it is sent. Split out of `linux_daemon.cpp` along the seam the file already had: unprivileged client code before the include site, root GPU ownership after it. An included `.cpp` rather than a header because the definitions are externally linked, the same rule `linux_startup_policy.h` follows. |
| `source/linux_daemon_deadline_policy.h` | **The ONE place Linux client deadlines and daemon bounds are written down** (F-DAEMON-DEADLINE). Per-role budgets, the four command classes, the derived response deadlines with `static_assert`s, the recovery-retry and connect-reachability rules. The Linux counterpart of `service_request_deadline_policy.h`, and it reuses that header's `service_phase_remaining_ms()` rather than reimplementing the phase/total arithmetic. |
| `source/linux_tui_mutation_actions.cpp` | The TUI actions that change something outside the editor: Apply/Reset, the daemon's boot-apply policy, and the live exports. Split out of `linux_tui_actions.cpp` for the source-size ratchet along the seam "edits a draft in memory" versus "leaves the process"; included inside that file's anonymous namespace. Owns the rule that an Apply/Reset whose outcome could not be read back is reported as UNKNOWN, never as failed. |
| `source/linux_daemon_lifecycle.h`, `source/linux_daemon_serve.h` | Daemon run state, monotonic fan wake condition, async-signal-safe SIGTERM/SIGINT stop handling, listener construction, the poll-driven shutdown-aware accept loop with classified `accept()` failures, and the shutdown fan handback. Included by `linux_daemon.cpp`; guards address them as one logical surface via `require_text_in_surface`. |
| `source/linux_service_install.cpp`, `source/linux_service_install_policy.h`, `source/linux_systemd_notify.cpp` | Protected binary staging, injected deterministic systemd sequencing/verification, and dependency-free readiness notification. |
| `source/linux_daemon_state.{h,cpp}` | Backward-readable checksummed active-state and operation-result records with root-owned atomic persistence. |
| `source/linux_gpu_selection.h` | Pure exact-BDF plus PCI device/subsystem matching, ambiguity rejection, active-intent ownership, and explicit first-selection policy. |
| `source/linux_gpu.{h,cpp}` | `--probe` driver report (NvAPI+NVML enumeration, family, VF read, OC offset range). |
| `source/linux_main.cpp`, `source/linux_cli_options.cpp`, `source/linux_live_output.cpp`, `source/intent_readback_status.h` | CLI dispatch/role selection, argument/help parsing, complete live VF text/JSON export, and the pure configured-intent versus hardware-readback comparison. |
| `source/linux_debug_log.{h,cpp}` | File-backed debug log for every role. Path resolution and the enable rule are pure inline functions; the sink owns the descriptor, rotation, and the session banner. |
| `source/linux_terminal_policy.h`, `source/linux_terminal_launch.{h,cpp}` | Pure terminal-emulator selection table plus the POSIX relaunch used when the binary is started from a graphical file manager. |
| `source/linux_startup_policy.h` | Daemon-side boot-policy handlers, startup write, and once-per-start record load. Client request implementations live in `linux_daemon_transport.cpp` so externally linked functions are not defined in a header. |
| `source/service_protocol_validation.h` | Envelope/response trust-boundary validation, split out of `service_protocol.h` purely for the source-size ratchet and included from it. |
| `source/linux_port*.cpp` | Config/profile INI. Now use the shared `gpu_core.h` `DesiredSettings`. |
| `source/linux_port_internal.h` | Shared INI model plus byte/section/entry limits so malformed input is rejected rather than expanded without bound. |
| `source/linux_tui.cpp`, `source/linux_tui_render.cpp`, `source/linux_tui_actions.cpp` | Parent terminal supervisor plus raw input/diff renderer, spatial keyboard focus, numeric editing, live refresh, and profile/tools actions. Reconnect-safe Apply/Reset, the boot-policy cycle and the live exports moved to `linux_tui_mutation_actions.cpp` (2026-09-12, source-size ratchet). |
| `source/linux_tui_authority_runtime.cpp`, `source/linux_tui_refresh.cpp`, `source/linux_tui_authority.h`, `source/linux_tui_diagnostic_policy.h` | Domain-aware draft attachment, offline/degraded/recovering/ready authority, health remediation, and first-error preservation. |
| `source/linux_tui_layout*.{h,cpp}` | Pure POSIX-free cell-grid layout split across shared, VF, and fan/profile shards. The same grid owns paint cells and non-overlapping hitboxes at compact/medium/wide breakpoints. Included directly by the `build.py` regression harness. |

## NvAPI Linux specifics (handled)

- **Driver-pinned placeholder entries (2026-09-23).** The Linux VF writer
  (`apply_curve_offsets_verified()` in `linux_backend.cpp`) now shares the
  Windows rule `vf_offset_zero_readback_is_benign()`: a point below
  `MIN_VISIBLE_FREQ_MHz` that was asked for a positive offset and reads back
  exactly 0 is accepted as non-offsettable instead of being rewritten for every
  remaining pass (~1 s each) and failing the phase. On the RTX 5070 the Windows
  placeholder (ci=127) lies outside the Linux graphics domain (see below), so
  whether any Linux entry actually triggers this is **unverified**; the rule is
  a no-op for any point the driver does move.

- **The status table concatenates EVERY clock domain**, not just the graphics
  curve, and domains are not padded to a fixed stride. Reading a fixed
  `VF_NUM_POINTS` window therefore runs past the graphics curve into foreign
  domains, whose points break the ordered-voltage invariant and used to fail
  the entire snapshot closed (no VF read, no writes at all).
  `linux_vf_graphics_domain_length()` in `linux_vf_validation.h` takes the
  leading non-decreasing-voltage run as the graphics curve; everything past it
  is zeroed, so validation and `apply_curve_offsets_verified()` both skip it.
  Verified on an RTX 5070, driver 610.43.03:

  | Entries | Domain | Values |
  |---|---|---|
  | 0..126 | graphics (127 points) | 180 MHz @ 450 mV .. 3187 MHz @ 1240 mV |
  | 127 | second domain | 405 MHz @ 540 mV |
  | 128 | third domain | 810 MHz @ 560 mV |
  | 129..131 | memory | 7001 / 13801 / 14001 MHz (GDDR7 28 Gbps) |

  `numClocks` (15 here) is the clock-domain count, not a point count. The
  128-bit editable mask was all-ones and did **not** discriminate the boundary,
  and the trailing per-entry dword is not a domain terminator either (it is set
  on entries 126/127/128 but clear on 131, the last memory point).

- **Error codes are negative** on Linux (`-1` generic, `-9`
  INCOMPATIBLE_STRUCT_VERSION) vs Windows `0x8000xxxx`. Success is `0` both;
  `linux_backend.cpp` uses `nvapi_ok(s) = (s == 0)`.
- **Single-bit-mask** per `SetControl` call — `apply_curve_offsets_verified`
  builds a per-point write mask and the verify loop converges via repeated
  single-changed-point passes.
- Version field `(ver << 16) | size`; NvAPI/NVML write the same VF state (single
  writer discipline kept).
- **Modern NVML clock offsets always target P0.** `nvmlDeviceGet/SetClockOffsets`
  is P-state-scoped, while Green Curve exposes one configured value per domain.
  Reading/writing whichever transient state was current could succeed at idle
  P8 while leaving performance P0 unchanged. Both platforms now read, write,
  verify, and roll back P0; Linux snapshot/range capture prefers the modern API
  and falls back to the deprecated global getters only for older drivers.
  Offset apply logs domain, P-state, request, API, and readback.
- **Advanced XBAR/SYS/VIDEO clocks use the shared version-keyed NvAPI
  ClkDomains transaction.** Linux resolves the same GET/SET/measure query IDs,
  reads a fresh complete control block, changes only owned fields, requires
  exact offset readback, and carries the preimage values in the rollback
  snapshot. Unknown response schemas remain fail-closed and their diagnostic
  is latched until a driver/GPU rebind instead of flooding the 1 Hz telemetry
  path. The daemon publishes all three support/readback/live-clock fields, and
  its Reset includes only advanced domains whose exact readback and writable
  surface are both proven. A readable-but-unwritable or unknown optional
  schema cannot make core Reset fail.
- `linux_curve_targets.h` composes the requested curve before any write.
  Explicit point targets win at those points; selective GPU offset covers the
  remaining eligible populated pre-lock points; exclusion counts populated
  points rather than sparse array indices; flatten/hard-lock owns the tail.
  A replacement also compares the previous and committed ownership masks and
  adds zero-offset cleanup targets for points owned only by the previous mask.
  The backend logs the cleanup-point count, and rollback restores the complete
  pre-transaction curve snapshot if any phase fails.
  NVML pinning happens only after the curve commit. A global NVML offset is used
  only when the requested result is truly global; selective/explicit/lock
  compositions use the curve path and region-aware readback verification.
- Unrecognized GPU families keep the best-effort VF backend and write support by
  default. A failed transaction rolls back instead of globally disabling this
  fallback.
- Multi-GPU matching requires a unique compatible PCI match. Exactly one NVML
  GPU and one NvAPI handle can use an explicitly logged sole-handle fallback
  when identifiers do not conflict, even if reported bus/slot values differ.
  Device matching recognizes the NVIDIA vendor/device pair in either 16-bit
  word order or a device-only form, and accepts either NvAPI's internal or
  external PCI device ID as corroboration; subsystem matching recognizes
  compatible full, swapped-word, or 16-bit forms. Genuine conflicts remain
  fail-closed and publish both APIs' raw IDs plus the conflicting component.
- NvAPI architecture is queried once and retained. A failed query falls back to
  NVML's documented Pascal/Turing/Ampere/Ada/Blackwell architecture values or a
  same-PCI cached backend; architecture alone never authorizes a write.
- VF refresh reads info, status, and control tables into temporary storage,
  records all driver statuses, validates versions/masks/counts/ranges/ordered
  voltage topology, and commits only a complete result. Live frequencies need
  not be monotonic. A failed refresh invalidates freshness and performs one
  immediate read-only same-PCI re-enumeration/rebind/re-read, with no sleeps.

## Fan runtime

- `fan_runtime_policy.h` is the platform-neutral reducer for current
  temperature, prior percentage, hysteresis, configured curve, and next action.
- Apply interpolates and writes the initial percentage from the current
  temperature; it does not use a fixed fallback percentage.
- The daemon worker waits interruptibly for `pollIntervalMs` and wakes
  immediately on new desired state, reset, shutdown, or repeated NVML failure.
  Rising demand applies immediately; hysteresis holds only a downward
  transition until its threshold. Repeated failures return the fan to automatic
  control and mark the runtime outcome uncertain.
- The wait deadline is `CLOCK_MONOTONIC`, and `g_fanWakeCondition` is created
  with `pthread_condattr_setclock(CLOCK_MONOTONIC)`. `PTHREAD_COND_INITIALIZER`
  implies the realtime clock, under which a backwards wall-clock step stalled
  re-assertion for the size of the step while a manual duty stayed pinned.
- Failed **temperature reads** escalate on the same ladder as failed fan
  writes, through the shared `fan_runtime_observe_result()` reducer. The old
  worker skipped its entire body when `nvmlDeviceGetTemperature` failed, so a
  persistent telemetry fault pinned the fan silently and forever.
- If the driver refuses the automatic-control handback, the worker forces
  `FAN_RUNTIME_EMERGENCY_PERCENT` (100%), matching Windows.
- **Crash handback (2026-09-23, `source/linux_fan_ownership.h`).** The shutdown
  handback only runs on an orderly exit; a SIGSEGV/SIGKILL/OOM kill left the
  fan at its last manual duty, and with startup policy `none` (or a guard
  lockout / failed replay) nothing ever took it back. `fan-owned.bin` is
  written before any write whose committed intent takes manual fan control
  (explicit apply and the unattended-write path; fail closed) and retired after
  a confirmed shutdown handback, a committed Reset, or a committed intent that
  leaves the fan to the driver. At start, before the startup policy and the fan
  worker, a same-boot marker is handed back once (auto, 100% if refused). The
  in-flight flag stops a handback that kills the daemon from looping under
  `Restart=always`. Clocks are left alone, exactly as the graceful stop leaves
  them. Log prefix `daemon fan ownership:`. Unverified on hardware.
- **The write is verified against `nvmlDeviceGetTargetFanSpeed` (intent), never
  against `nvmlDeviceGetFanSpeed_v2` (measured).** The measured value reads 0
  while a zero-RPM fan stop holds, so the old equality gate failed every
  accepted write and rolled the whole apply back — manual fan control simply did
  not work on Linux until 2026-07-27. See `fan-control.md` for the driver dump
  and the shared `fan_manual_write_confirmed()` contract.
- The requested duty is clamped into `nvmlDeviceGetMinMaxFanSpeed`'s range
  before the write (30..100 on an RTX 5070), because the driver clamps it
  anyway and then reports the clamped intent.
- Write-outcome and duty-range logging is transition-gated on the *effective*
  duty; curve mode re-asserts every poll interval and would otherwise emit a
  journal line per poll.

## Daemon lifecycle and serve loop

- `linux_daemon_lifecycle.h` installs `SIGTERM`/`SIGINT` handlers that set
  `g_running` and poke a self-pipe; `SIGHUP` is ignored. Before this the daemon
  had no handlers at all, so `systemctl stop` killed it at default disposition
  and the teardown block — worker join, socket unlink, NVML shutdown, and the
  fan handback — was unreachable code.
- `linux_daemon_serve.h` owns listener construction, the accept loop, and
  `daemon_release_fan_to_driver()`. The loop `poll()`s the listener together
  with the shutdown self-pipe, so shutdown is event-driven with no timeout and
  a peer that connects without sending cannot hold the loop before `accept()`.
- `accept()` failures are classified by `daemon_accept_disposition()` in
  `linux_daemon_transport_policy.h`:
  - retry — `EINTR`, `ECONNABORTED`, `EAGAIN`, `EPROTO`, `ENOBUFS`, `ENOMEM`, `EPERM`
  - reclaim — `EMFILE`/`ENFILE`, handled with a reserved `/dev/null` descriptor
    that is closed, used to drain one pending connection, then reopened (no
    sleeps, no spinning)
  - fatal — everything else, which exits **non-zero**
- The non-zero exit matters: the unit restarts on a failed exit. The old loop
  treated every non-`EINTR` errno as fatal *and* returned 0, so a transient
  condition permanently removed GPU control with systemd declining to restart.
- **Failing to start the fan worker is fatal** (2026-07-31). `pl_thread_start`
  failure used to be recorded in a `fanThreadOk` flag and otherwise ignored, so
  the daemon served happily while a curve or fixed duty was written once and
  never re-asserted — the driver takes the fan back at the next opportunity and
  nothing says so. Windows stops startup when its lifecycle worker cannot be
  created; this now matches, and the non-zero exit lets `Restart=` try again.
- The accept loop grows a `poll()` timeout **only** when systemd asked for a
  watchdog (`linux_systemd_watchdog_interval_ms()` returns 0 otherwise, and the
  timeout stays `-1`). Every wakeup pings, not just the timeout branch: a busy
  daemon would otherwise never reach that branch and would be killed for doing
  its job.

## Request deadlines (F-DAEMON-DEADLINE, 2026-09-12)

`source/linux_daemon_deadline_policy.h` is canonical; this is the summary.

**What was wrong.** `linux_daemon_transport.cpp` used ONE literal,
`GC_DAEMON_IO_TIMEOUT_MS 2000`, for four roles that ask different questions: the
daemon reading a request, the daemon writing a response, the client writing a
request, and **the client waiting for the daemon to compute an answer**. Only
the first two are stall questions a small fixed number answers correctly. For a
mutation the fourth was an order of magnitude short — Windows gives the same
operation 20000 ms (`SERVICE_APPLY_CLIENT_TIMEOUT_MS`) for strictly *less* work,
while the Linux APPLY handler additionally performs up to four `fsync`-ed record
writes inline and a second full hardware pass on the rollback path.

**The worse half.** On a transport timeout `send_simple()` queries the original
operation ID instead of writing the GPU again — but that query carried the same
2000 ms, and the daemon is single-threaded: `handle_request()` runs to
completion under `g_lock` before the accept loop accepts anything else, so while
the mutation is still running the query cannot be accepted either. It expired
too, in exactly the case the recovery exists to cover. The TUI then showed a
**committed** Apply as "Apply failed" with the draft still dirty, and nothing
stopped the user from pressing Apply again with a fresh operation ID.

**The contract now.**

- Deadlines are *derived*, never picked. Daemon-side budgets (fan-thread
  `g_lock` hold, live refresh, one durable record, one hardware pass, the
  rollback pass, the record count) compose into four command classes:
  `STATE_READ`, `RECORD_WRITE`, `MUTATION`, `OPERATION_RECOVERY`.
- `linux_daemon_send()` derives its deadline from `request->command`, so no call
  site names a number. `linux_daemon_send_deadline()` is the explicit form.
- **The serialization term is worse than Windows on purpose.** Windows
  serializes dispatch but still accepts and reads; the Linux accept loop is
  blocked outright, so a queued request sees the whole handler ahead of it with
  nothing overlapped. A concurrent *mutation* is still excluded from the read
  and record-write lanes — one stale frame is the right degradation — which only
  works because of the reachability rule below.
- **The recovery query is a MUTATION-class wait**, because it is queued behind
  the mutation it asks about. A read-class deadline there could only ever
  recover the rare lost-response case, never the slow mutation. Asserted at
  compile time.
- The recovery **loops**, bounded by `linux_daemon_recovery_remaining_ms()`
  (which reuses `service_phase_remaining_ms()`), so retrying cannot double the
  client's worst case. `linux_daemon_recovery_may_retry()` permits another
  attempt **only** after a deadline expiry against a daemon that owns the
  socket: every failure that returns promptly (unreachable, saturated backlog,
  protocol mismatch) stops the loop, because retrying those would burn the
  budget in a spin rather than waiting on a descriptor. No sleeps, no intervals.
- An unrecovered mutation is reported as **outcome UNKNOWN**, not as a failure:
  the response is stamped with `SERVICE_OPERATION_OUTCOME_UNKNOWN` and the
  operation ID, and the TUI says "Apply/Reset outcome unknown" and refreshes.
- **One deadline spans a whole exchange.** Previously prefix and body each took
  a fresh relative timeout, so a "2000 ms" budget cost up to 4000 ms in practice
  in both directions; a peer that stalled after the header bought a second full
  window against a single-threaded server.
- **`client_connect()` is non-blocking before `connect()`, not after.** A
  blocking AF_UNIX `connect()` against a full listen backlog waits for the
  daemon to drain it with no bound at all — the one client wait no deadline
  covered. Non-blocking turns it into `EAGAIN`, which the kernel returns only
  when the pathname *is* a listening socket whose backlog is full, i.e. positive
  evidence that the daemon exists and is merely saturated.
- **Reachability**: only a failed `connect()` classified as `UNREACHABLE`
  (`ENOENT`/`ECONNREFUSED`/`EACCES`/`EPERM`) means "offline". A request the
  daemon *accepted* and then failed to answer proves it is busy and nothing
  else. `tui_refresh_service()` therefore keeps `serviceOnline`, the draft
  attachment and the last published state on such a miss, and says "Daemon is
  busy; showing the last state it published" — instead of the Linux shape of the
  2026-09-11 Windows incident, where a long mutation on another client tore live
  authority down and restored it a second later, once per slow tick.
- **Instrumentation**: `note_exchange_duration()` logs a per-class high-water
  mark once an exchange passes half its budget. Its absence is why these
  deadlines could only be argued about rather than measured; the Windows
  incident was diagnosable because its log carried real turnaround percentiles.

**Not verified on hardware.** The budgets are derived from what the handlers do,
not measured on a live daemon; the high-water logging exists to supply real
numbers. Open question: whether `GC_DAEMON_HARDWARE_WRITE_BUDGET_MS` (6000) and
`GC_DAEMON_STATE_REFRESH_BUDGET_MS` (2500) match a real RTX 5070 apply.

## Per-peer admission control (F-LNX-ADMISSION, 2026-10-01)

The serve loop is single-threaded — one connection is accepted, read, handled to
completion and answered before the next `accept()` — with a backlog of 8 and, as
of this change, a per-peer budget. `linux_daemon_serve.h` reuses
`service_ipc_throttle_policy.h`, the same pure policy the Windows pipe transport
runs: monotonic milliseconds in, a decision out, no OS types, no allocation. A
port of an existing tested control, not a second design.

- **Metered before the request is read**, so a flood spends its own budget
  rather than the shared backlog.
- **Charged after the outcome**: success 1, connect-without-frame 10, refusal 0.
  Refused peers get one best-effort nonblocking busy response before any read;
  no send/read deadline is spent on them.
- **Identity is the kernel-authenticated UID** from SO_PEERCRED. Missing identity
  fails closed. A fork or primary-group change cannot buy a new quota.
- Resume restores retain their separate limit of 8 per 60 seconds.

Admission materializes refill itself, so passage of monotonic time suffices to
recover; no refusal charge is needed. Overspending saturates tokens to zero.
Regression cases 5980-5987, the security audit fixture and native transport
fixture cover independent peers, quiet recovery and silent refusal.

**Client side of a refusal (review fix 2026-10-01).** Because the daemon answers
and closes before reading, the client's request write races that close. If the
close wins, `send()` fails with EPIPE (or poll reports only error bits, EIO)
while the busy answer is ALREADY queued in the client's receive buffer -- Linux
delivers queued AF_UNIX data before a reset. The client used to give up at the
write and report a transport fault, so the same refusal surfaced two different
ways depending on scheduling. `daemon_read_refusal_after_failed_write()` now
reads it (only a complete, valid, NON-OK response counts; `requestSubmitted`
stays false). `tests/linux_transport_regression.cpp` 67-75 constructs both
orders explicitly plus a peer that vanished without answering; executed under
WSL Arch on 2026-10-01 in addition to the Windows cross-link.

## CLI/TUI atomic text writer (review fix 2026-10-01)

`write_text_file_atomic()` (`linux_port.cpp`; config saves, `--probe-output`,
desktop/autostart assets) uses a CSPRNG `O_CREAT|O_EXCL|O_NOFOLLOW` temp and
`renameat()` on a dirfd, so a planted LEAF symlink is replaced, never followed.
The first hardened version also refused bare file names (`--probe-output
report.md`, `--config my.ini` -- "Output path has no directory") and opened the
directory with `O_NOFOLLOW`, which broke `--save-config` for anyone whose
`~/.config/greencurve` is a symlink into a dotfiles checkout while protecting
only one path component. Now: path rules live in `linux_atomic_write_policy.h`
(bare name = cwd, `/x` = root, empty/`.`/`..` leaf refused; temp name bounded
inside NAME_MAX), `O_NOFOLLOW` on the directory applies to euid 0 only, retries
cover temp-name collisions only (a write/sync/rename error stops at once and
reports its own errno), and the directory is fsynced after the rename
(best-effort). Harness 6260-6267; an ad-hoc WSL probe on 2026-10-01 confirmed
relative names, a symlinked dir as user, refusal as root, and an unfollowed leaf
symlink.

## Automatic restore: crash-loop guard, resume, and reset-before-apply

Everything that writes the GPU **without a user asking** goes through
`daemon_automatic_restore_write()` in `linux_auto_restore_runtime.h`. There are
exactly three callers, all typed as a `LinuxAutoRestoreTrigger`: the
`restore-last` boot replay, the `profile N` boot apply, and the standby-resume
restore. Two defects made that consolidation necessary.

### 1. Automatic writes did not reset the OC baseline first

`service_merge_desired_after_mutation()` clears `resetOcBeforeApply` before the
intent is persisted — correctly, because it is a one-shot transaction
instruction and not durable state. Both boot paths then applied the persisted
`DesiredSettings` **directly**, so `resetOcBeforeApply` was always false and
every automatic Linux write laid a curve on top of whatever the driver was
already holding. That is exactly the "apply on top of apply" case Windows
resets the baseline to avoid (drifted boost behaviour, stacked offsets).

The fix is to build the request through `service_build_full_restore_request()`
— the same pure builder the Windows standby and driver-recovery paths use — so
the rule lives in one place on both platforms. `linux_backend_apply()` is now
forbidden by a source guard in `linux_startup_policy.h`.

**The shared builder was itself wrong about locks.** It derived "owns VF
policy" from `hasGpuOffset || any hasCurvePoint`, while its sibling
`service_intent_owns_vf_cleanup()` in the same header has always counted
`hasLock` as well. Every lock mode composes a VF anchor/tail write, so a
lock-only profile reset its baseline when it *replaced* another profile but not
when standby restored it. Both functions now use the one predicate; this
changed Windows behaviour too, and regression case 2 of the restore matrix flipped
from `false` to `true`. A memory-, power- or fan-only intent still does **not**
reset: those fields may belong to another tool.

### 2. `restore-last` plus a restart net is a replay loop

Windows can afford a strict "an ordinary service start never writes" rule
because a logon coordinator applies settings instead. Linux has no such thing,
so `restore-last` is how settings survive a reboot at all — which means a
setting that hangs the driver was replayed by every `Restart=` restart, forever.

`linux_auto_restore_policy.h` is the pure guard. It holds no clock: a crash loop
is defined by repetition, and a deadline would be a timing assumption.

| Event | Effect |
|---|---|
| Daemon start, new `boot_id` | attempt counter reset; sticky lockout **and its reason kept** |
| Start-time write authorized | counter incremented **and persisted before the write** |
| 4th start-time write in one boot | refused; lockout latched as `LOCKOUT_UNSTABLE_APPLY` |
| Failed unattended hardware write or ACTIVE-record commit | hardware rollback attempted, uncertain record written, and lockout latched as `LOCKOUT_AUTOMATIC_APPLY_FAILED` |
| Resume restore | never rationed — a machine event cannot repeat by itself |
| Orderly stop | counter cleared; lockout untouched |
| Successful **explicit** Apply/Reset | counter, lockout **and reason** cleared only when the guard record commits; otherwise the prior guard stays authoritative and the successful mutation returns a warning |

### Durable state is part of automatic success

An unattended hardware write is not successful merely because the backend
accepted it. `daemon_automatic_restore_write()` captures a rollback snapshot,
applies the request, and then commits the corresponding ACTIVE record. Only
after that store succeeds does it set `outcome.success`, publish the intent in
memory, or wake the fan runtime as active. If the ACTIVE store fails, the
hardware snapshot is restored, the daemon writes an UNCERTAIN record, latches
automatic restoration off, and returns failure. The resume CLI therefore
cannot exit zero while the daemon has already locked itself out over an
uncommitted record.

Explicit Apply/Reset has a separate secondary transaction: clearing the
automatic-restore guard. The helper keeps the previous in-memory guard until
the cleared record commits. A persistence failure restores that previous guard
and leaves the hardware/active-state operation committed, so the response is
`SERVICE_STATUS_OK` with `SERVICE_OUTCOME_SEVERITY_WARNING` and a precise
message. This avoids both false claims: the mutation did succeed, but automatic
restoration was not durably re-armed.

### The published lockout (fixed 2026-07-31, v0.22.1)

`ServiceSnapshot::autoRestoreLockoutReason` is what the TUI, `--status` and the
JSON export read. The daemon derived it from `g_stateUncertain` alone, which is
a proxy for only **one** of the two latching arms:

- a failed hardware write sets both the flag and the guard, so that case looked
  right;
- the crash-loop arm (`DENY_ATTEMPTS_EXHAUSTED`) latches the guard **without
  ever issuing a write**, so nothing set `g_stateUncertain`.

A machine that tripped the crash-loop guard therefore published `LOCKOUT_NONE`,
reported a healthy GPU everywhere, and silently never restored settings at boot
again — the lockout is sticky across boots — with a `dlog` line as the only
evidence. Windows has always fed the same field from its real lockout state
(`service_auto_restore_is_locked_out()`).

- `linux_auto_restore_published_lockout_reason()` is the pure decision; the
  snapshot calls it and nothing else. `tools/linux_gates.py` forbids the old
  `s->autoRestoreLockoutReason = g_stateUncertain` spelling outright.
- An **uncertain state with a clear guard still reports a lockout**: the
  rollback or the record did not settle, so no unattended write may run until a
  user resolves it, which is exactly what the field means. Since 2026-08-01 the
  runtime enforces it: `linux_auto_restore_decide()` takes `g_stateUncertain`
  and returns the new `LINUX_AUTO_RESTORE_DENY_STATE_UNCERTAIN` verdict, so
  every automatic trigger — including the otherwise unrationed resume — is
  refused while the daemon state is unsettled (a latched guard outranks it,
  and an exhausted boot is reported only after it).
- **A pre-write "GPU not available" failure sets neither the lockout nor the
  uncertain flag** (`F-PREP-NO-UNCERTAIN`): nothing was written and no state
  was disturbed, so the attempt is spent but the daemon stays healthy, the fan
  reassertion thread keeps its authority, and the next resume is allowed to
  try again. `apply_startup_profile_policy()` follows the same rule — it marks
  the daemon uncertain only when the failure reached a hardware write.
- The reason is **persisted**, not just logged: it is published in every
  snapshot, so losing it across a restart would report the generic failed-write
  reason for a lockout no write ever caused. `LINUX_DAEMON_GUARD_VERSION` is
  therefore 2; a version-1 file is read as corrupt and fails closed to locked
  out, which is the safe direction on upgrade.
- **First cause wins.** A later automatic refusal is a consequence of the
  original latch and must not overwrite the reason that explains it.
- The record is coherent or rejected: locked out always names a reason, clear
  never carries one. `linux_daemon_guard_initialize()` enforces the same rule on
  the way out, so a guard mutated field-by-field cannot produce a file the
  validator would refuse to read back.
- Surfaced in `linux_live_output.cpp` as an `Automatic restore:` line and an
  `auto_restore` JSON object, because a latched lockout is the one daemon state
  that makes settings silently stop applying.

- Persisted to `/var/lib/greencurve/restore-guard.bin` through the same
  `store_record_atomic()` path as `active.bin` (root-owned 0600, temp + fsync +
  atomic rename + directory fsync). It authorizes an unattended hardware write,
  so it must not be forgeable or half-written.
- **A corrupt or unreadable guard fails closed to locked out**, matching the
  Windows rule that an unreadable lockout fallback is treated as locked out.
  Treating a damaged counter as "no attempts yet" would hand a crash loop
  unlimited retries exactly when the filesystem is already misbehaving. An
  *absent* record is the ordinary first-run state.
- The attempt is persisted **before** the write; a guard that cannot be
  committed refuses the write. Windows aborts before touching the GPU when its
  protected proof invalidation cannot be committed — same reasoning.
- Boot identity is `/proc/sys/kernel/random/boot_id`, the counterpart of the
  Windows 128-bit `BootIdentifier`: stable for one real boot and unaffected by
  a wall-clock correction. If it cannot be read the counter deliberately
  carries over rather than resetting — over-counting stops an unattended write,
  under-counting authorizes one.
- `LINUX_AUTO_RESTORE_MAX_START_ATTEMPTS` is 3, matching the Windows SCM's
  `SC_ACTION_RESTART` count rather than being invented.

### 3. Standby resume

`SERVICE_CMD_RESUME_RESTORE` (protocol v17) is the Linux counterpart of the
Windows `PBT_APMRESUME*` path: restore the complete in-memory intent exactly
once, with no proof gate.

- **The request carries nothing** — no settings, no target GPU, no operation id,
  no flags, no preconditions; `service_request_reject_reason()` refuses all of
  them. The daemon replays the intent it is already holding for the adapter it
  is holding it for, so a group-reachable socket message can never become an
  apply nobody typed.
- The edge comes from `greencurve-resume.service`, written and enabled by
  `--service-install`. A unit that is both `WantedBy=` and `After=` the sleep
  targets runs on the way back **up** — systemd walks the ordering in reverse on
  the way down.
- **Readiness is answered by ordering, not by waiting.** The unit is also
  `After=nvidia-resume.service`, the driver's own "the GPU is usable again"
  unit, so there is no sleep and no retry loop. `auto_restore_prepare_backend()`
  does one `linux_backend_init` retry (the character devices are torn down and
  rebuilt across a suspend) plus the existing same-PCI rebind, and refuses to
  write if the GPU still is not there.
- `ConditionPathExists=/run/greencurve/greencurve.sock` means a machine whose
  daemon is not running **skips** the unit rather than failing it once per wake.
- No active intent is reported as success, not failure: a machine with no Green
  Curve settings has nothing to lose across a suspend.
- A resume restore while `g_stateUncertain` is set is refused with the new
  `DENY_STATE_UNCERTAIN` verdict until an explicit Apply/Reset resolves the
  state — the flag is a gate, not just a published label (2026-08-01).

## Startup-apply policy (introduced in protocol v13; current protocol v28)

What the daemon writes to the GPU when it starts is administrator-configurable,
not hard-coded. `RESTORE_LAST` is **zero**, so an absent or zero-filled record
behaves exactly as every pre-v13 build did.

| Mode | Behaviour at daemon start |
|---|---|
| `restore-last` (default) | Replay the committed `active.bin` intent, as before |
| `none` | Leave the GPU untouched; `active.bin` stays on disk, unreplayed |
| `profile` | Apply a stored snapshot bound to one exact GPU identity |

- **The daemon cannot resolve "slot N" itself.** `ProtectHome=yes` hides the
  user's `config.ini`, so the *client* loads the profile and sends a snapshot
  plus the exact `targetGpu`; `SERVICE_CMD_SET_STARTUP_POLICY` carries it in the
  existing `desired`/`targetGpu`/`profileSlot`/`source` fields plus a new
  `startupMode`.
- **The snapshot follows the profile it names** (v15). Because it is a
  snapshot, editing slot N used to leave the daemon writing the values captured
  when the policy was bound, forever, while the control still said "PROFILE N".
  The invariant now enforced by `source/startup_snapshot_policy.h` is: *while
  the policy is `profile N`, the stored snapshot equals the on-disk content of
  slot N, or the client says out loud that it does not.*
  - `SERVICE_CMD_REFRESH_STARTUP_PROFILE` replaces only the settings of an
    already-bound policy. It carries a slot and settings and is rejected if it
    carries a `targetGpu` or a `startupMode`: the mode, slot, display name and
    GPU binding are the daemon's. A refresh can therefore neither create a
    boot-apply nor move one to another slot or GPU — which is why saving a
    profile while a *different* GPU is selected cannot silently re-bind it.
  - The client pushes it after every write to the bound slot: the TUI's Save,
    and `--save-config`. Both reload the slot from disk rather than reusing the
    in-memory draft, so what boots is what the file says.
  - **Clearing the bound slot unbinds the policy** (client sets `none`) instead
    of leaving a deleted profile applying at every boot.
  - An offline daemon cannot be told, so the save reports that the boot-apply
    snapshot is now stale rather than reporting plain success.
  - Divergence is still *detected*, not assumed away: hand-editing `config.ini`
    is outside every push path. `linux_startup_snapshot_report()` reads the
    stored snapshot back with `GET_STARTUP_POLICY` and compares it with the
    profile using the shared `desired_settings_equal()`. The TUI evaluates it at
    start and after any write/policy change (never on the 1 Hz tick), renders
    `PROFILE N STALE` in red, and `--show-startup` prints both sides field by
    field.
  - **The snapshot travels in `ServiceResponse.startupProfile`, gated by
    `startupProfileValid` (v16)** — not in `desired`. v15 returned it in
    `desired`, which is the member the envelope defines as the *active* intent
    and which `daemon_stamp_state_envelope()` rewrites at the end of **every**
    request. The snapshot was therefore overwritten before it left the daemon,
    and the staleness check compared whatever was applied right now against the
    profile: applying anything other than profile N painted `PROFILE N STALE`
    for a snapshot that was perfectly in sync, and a genuinely stale snapshot
    read as fine whenever the applied intent happened to match. The whole
    detector was reporting on the wrong data.
    - The stamp owns `snapshot`, `controlState` and `desired`. A handler with
      something else to publish needs its own response member; there is no
      ordering that makes borrowing one safe.
    - Coherence is enforced at the trust boundary
      (`service_response_startup_profile_is_coherent`): the flag is a boolean,
      an unset flag means byte-zero settings, and a set flag is only accepted
      while the published policy really is `profile N` with a nonzero slot.
    - `daemon_publish_startup_snapshot()` fills it from `g_startupPolicy` — the
      *committed* record — for all three startup commands, so a set or refresh
      that failed to reach disk still answers with what will actually boot.
  - Client half: `source/linux_startup_sync.{h,cpp}`, shared by the TUI and the
    CLI. Daemon half: `daemon_handle_refresh_startup_profile()`.
- Persisted to `/var/lib/greencurve/startup.bin` through the same
  `store_record_atomic()` path as `active.bin`/`operation.bin` (same-directory
  temp, file fsync, atomic rename, directory fsync, root-owned 0600).
- The validator refuses a `profile` record without a real slot and an exact
  `pciInfoValid` GPU, and refuses a `none`/`restore-last` record that still
  carries a slot, a target or settings.
- **A corrupt/unreadable record fails closed to `none`**, never back to
  `restore-last`: a policy the administrator cannot read back is a reason to
  leave the GPU alone rather than write under a guess.
- The set path **and** the refresh path assign the in-memory copy **only after**
  the store succeeds, so a policy that never reached disk is never advertised as
  committed.
- `startupPolicyMode`/`startupPolicySlot` ride on every `ServiceStateEnvelope`,
  so a client renders the control with no extra round trip. Both are zero on
  Windows, which has its own logon coordinator. The stored *settings* are not in
  the envelope; reading them back is the deliberate extra round trip that the
  staleness check makes only when something can have moved.
- CLI: `--show-startup` (also reports snapshot-vs-profile divergence),
  `--startup-profile N|last|none`. TUI: the **Startup apply** control on
  Profiles & Tools cycles the three modes and shows `PROFILE N STALE` when the
  snapshot no longer matches the slot.
- Diagnostics: the daemon logs the boot-applied snapshot's own values
  (`fanMode`/`fanPct`/`pollMs`/`hysteresisC`/offsets/power) on every set,
  refresh and boot apply, and the client logs each refresh and every detected
  divergence. Before that, a stale snapshot was only visible by hex-dumping
  `/var/lib/greencurve/startup.bin`.

## Debug log and crash breadcrumbs

`linux_debug_log.{h,cpp}` gives Linux the durable log Windows always had.

- Client/TUI write `greencurve_debug.txt` **next to the resolved `config.ini`**
  (the binary's folder by default). The daemon writes it into
  `/var/lib/greencurve/` — `ProtectSystem=full` mounts `/usr` read-only for the
  unit, so it cannot log beside its own binary.
- On by default (`LINUX_DEBUG_DEFAULT_ENABLED`, matching Windows). `[debug]
  enabled=0` in `config.ini` disables it; `GREEN_CURVE_DEBUG=0` wins over the
  config, any other value forces it on. `save_profile_to_config_path()`
  materializes the key so it is discoverable.
- Creates and reopens logs mode `0600` (and tightens an older permissive file on
  open), because they carry applied settings and diagnostic fingerprints.
- Rotates at 4 MB keeping one `.1` generation. Rotation **preserves the
  descriptor number** with `dup2()`, because the fatal-signal breadcrumb writes
  to that fd from a handler and a reallocated number could be the control
  socket.
- `dlog()` (daemon + client transport) routes through the same sink. Only the
  daemon mirrors to stderr; clients keep stderr clean, because it is the
  terminal the TUI is about to take over. This removed the
  `daemon client: connected ...` line that used to print over the user's shell.

### Diagnosing "I cannot reach the daemon"

The permission diagnostic itself is old, but it only ever reached the *caller*,
where the TUI shows it truncated in a status row for a single frame. It now
reaches the log as well:

- `linux_daemon_log_client_environment()` runs once per client process, before
  anything can fail: euid/primary gid, whether the `greencurve` group exists,
  whether this process actually has it, and the socket's type/owner/group/mode.
  It answers "was this user in the group" even for runs that succeeded.
- The remedy line (`sudo usermod -aG greencurve "$USER"`, then sign out/in) is
  printed **only when `access()` really fails**, and it is chosen by cause:
  missing membership, missing group (daemon never installed), or correct
  membership with wrong socket permissions. Advertising a remedy on a healthy
  run would send someone chasing a non-problem.
- Every failure path in `linux_daemon_send()` — connect, request write, header
  read, bad magic, version mismatch, body read, response validation, and a
  daemon-side rejection — logs its full message through
  `log_client_failure()`, **deduplicated on the message text** because the TUI
  polls at 1 Hz and would otherwise bury the first occurrence under hundreds of
  identical copies.
- Permission facts own copies of both error strings. `strerror()` storage may
  be reused by the next call; retaining two returned pointers could make the
  connect reason silently turn into the later metadata reason.
- Daemon side: a protocol mismatch logs the caller's pid, command, magic and
  version, and a malformed request logs the exact fields that failed
  validation (flags, operation id, origin, slot, startup mode, target validity,
  preconditions) rather than only setting `invalid protocol fields`.

## Crash diagnostics

`linux_crash_breadcrumb.h` gives the Linux binary the diagnosability Windows
already had from `SetUnhandledExceptionFilter` + the vectored handler. This
matters because the Linux build uses `-fexceptions -frtti` and `std::string`
(the INI parser in `linux_port.cpp` calls `substr`), while the tree contains no
`try`/`catch` at all — so a `bad_alloc` or `length_error` used to abort the
*root daemon* with nothing in the journal.

- `linux_install_crash_breadcrumbs(role)` installs handlers for `SIGSEGV`,
  `SIGBUS`, `SIGILL`, `SIGFPE`, and `SIGABRT`, plus a `std::set_terminate`
  handler. `main()` installs it as `cli` before parsing; the daemon relabels
  itself `daemon`.
- `linux_set_crash_phase()` records a static string (`daemon-init`,
  `daemon-serving`, `daemon-shutdown`) that appears in the breadcrumb.
- Handlers use `SA_RESETHAND` and then `raise()`, so the breadcrumb **never
  suppresses the crash** — core dumps and systemd crash accounting are
  unchanged.
- Handlers take `SA_SIGINFO` (F-LNX-CRASH, 2026-07-31), so the report carries
  `si_code`, `si_addr` and the fault's PC/SP from `ucontext_t` — x86_64 and
  aarch64 are read explicitly and any other architecture reports zeroes rather
  than a wrong struct offset. Without these the report named a signal but not a
  location.
- **There is still no in-process minidump writer on Linux, deliberately.** The
  binary artifact stays the kernel core (`core_pattern` / `systemd-coredump`),
  which the kernel writes correctly for a process whose own memory may already
  be corrupt. What Green Curve adds is everything needed to act on an address
  *without* the core: fault address, `si_code`, PC/SP, and the full
  `/proc/self/maps` (streamed with `read`/`write`), so a PIE address can be
  reduced to module+offset and resolved with
  `llvm-symbolizer --obj=greencurve.debug`. The report emits that command
  itself. `--probe` still reports `core_pattern` and whether `coredumpctl` is
  present.
- **`RLIMIT_CORE` is deliberately never raised.** Overriding a user's own
  `ulimit -c 0` to force a core is a side effect this program has no business
  having; a gate forbids `setrlimit(` in both files so it does not get "fixed"
  back in.
- **A crash report file lands next to `config.ini`** — which on Linux *is* the
  binary's directory by default (`default_linux_config_path`). The daemon uses
  `GC_DAEMON_STATE_DIR` instead, for the same reason its log does: systemd
  mounts `/usr` read-only for the unit (`ProtectSystem=full`). The rule is the
  pure `gc_linux_crash_dir()` in `crash_artifact_policy.h`, shared with Windows
  and asserted by the regression harness on either host; a config path with no
  directory component (i.e. `/proc/self/exe` was unreadable, so the CWD) is
  **refused** rather than used.
  - `source/linux_crash_report.{h,cpp}` is the startup half — it may format,
    read directories and unlink; `linux_crash_breadcrumb.h` is the handler half
    and may not. The split sits exactly on the async-signal-safety line: the
    **path** is formatted up front, and the **file is created by the handler**
    (`gc_crash_report_open_on_demand()`, `O_CLOEXEC`, `0600`) on the first byte
    it emits, because `open()` is async-signal-safe while formatting is not.
  - **Invariant (F-LNX-CRASH-LAZY): a report file exists ⇔ a crash wrote bytes
    into it.** Arming creates nothing. This is structural, not a cleanup step —
    a 0-byte-report spam bug (recorded in the local-only log) made it so, and
    why "close it on exit" cannot work (`execve` from the TUI relaunch, `_exit`,
    `SIGKILL`). Guards: `O_CREAT` is *forbidden* in `linux_crash_report.cpp`,
    `gc_crash_report_open_on_demand` and the zero-length-emit early return are
    *required* in `linux_crash_breadcrumb.h`.
  - `gc_signal_safe_emit()` returns before opening anything when
    `length == 0`, since creating the file is a side effect of emitting.
  - A `/dev/null` descriptor is reserved at arm time
    (`gc_crash_report_reserve_slot()`) and closed immediately before the real
    `open()`, so a crash *caused by* descriptor exhaustion can still write its
    report. System-wide `ENFILE` is still lost; stderr and the debug log carry
    the breadcrumb either way.
  - The fd slot is tri-state: `>= 0` open, `-1` not yet attempted,
    `GC_CRASH_REPORT_FD_FAILED` (`-2`) sticky failure, so a dying process does
    not retry a doomed `open()` once per emitted fragment. Re-arming clears it.
  - Rotation reuses the shared policy: 10 reports kept, ordered by the embedded
    timestamp. It runs at arm time and now counts only real reports —
    placeholders used to consume the budget and push genuine evidence out.
  - `linux_crash_report_close()` is **optional**; it only exists so
    `configure()` can re-arm onto another directory. Correctness never depends
    on a caller running it.
  - Arming logs `directory writable`/`directory NOT writable` from an
    `access(W_OK)` probe. Diagnostics only — `access()` answers for the real uid
    and misreads ACLs, so it must never gate the report.
- The report also appends to the debug-log descriptor
  (`linux_set_crash_log_fd()`), because a desktop-launched TUI has no stderr
  anyone can read. `linux_crash_breadcrumb.h` keeps that one in `static`
  storage, so **each including TU needs its own `linux_set_crash_log_fd()`
  call** — `linux_main.cpp` and `linux_daemon.cpp` both make it. The *report*
  state deliberately does not repeat that mistake: `gc_crash_report_fd_slot()`
  and `gc_crash_report_path_slot()` are non-static `inline` functions whose
  local statics have one instance across the whole program, so the single
  `linux_set_crash_report_path(linux_crash_report_path())` in `main()` covers
  every TU. A gate forbids a second arming call in `linux_daemon.cpp`.
- Output goes to `STDERR_FILENO` through `write(2)` only. Guards forbid
  `snprintf`/`fprintf`/`strsignal` (build.py) and `malloc(`/`backtrace`
  (linux_gates.py) in this header — the latter pair because a `backtrace()`
  helper would drag in allocating, lazily loaded stack-walking machinery. Integers and
  hex are formatted by hand; the integer formatter was verified against `printf`
  across `LONG_MIN`/`LONG_MAX` under UBSan.

### Debug symbols (F-LNX-SYMBOLS, 2026-07-31)

Until this, `LINUX_FLAGS` carried a bare `-s` and **nothing was extracted**: the
shipped binary had neither `.symtab` nor DWARF, and no file anywhere matched it.
A systemd-coredump core — and the PC in the crash report — were correct and
completely unsymbolizable.

- The Linux build now compiles with `-g -gdwarf-4` plus the same
  `-ffile-prefix-map`/`-fdebug-prefix-map`/`-fdebug-compilation-dir` the Windows
  ARM64 path uses, and links with `-Wl,--build-id=sha1`.
- `crash_artifacts.extract_linux_symbols()` runs post-link:
  `zig objcopy --strip-all --extract-to` writes
  `dist/symbols/linux-<arch>/greencurve.debug`, adds a `.gnu_debuglink` to the
  shipped binary, and strips it. Input and output paths must differ — objcopy
  opens the output first and truncates its own input otherwise (`TRUNCATED_ELF`).
  `zig objcopy` rather than `llvm-objcopy` so the Linux build does not depend on
  the Windows toolchain being downloaded.
- Verification is a hard build failure, not a warning: the symbol file must
  exist, be ≥ 4 KiB, and carry the **same GNU build-id** as the shipped binary —
  a mismatched `.debug` is one `coredumpctl`/gdb silently refuse to use, which
  looks exactly like having no symbols.
- Extraction and stripping happen **before** `verify_release_binary()`, so the
  private-workspace-path scan still runs strictly against the bytes that ship.
  Symbols live outside the release payload and are not packaged.

## systemd unit: persistence directives

`--service-install` writes two units. Parity target is the Windows service's
`SERVICE_AUTO_START` plus `SC_ACTION_RESTART` x3 at 2s/5s/10s with
`dwResetPeriod` 600 (`main_service_install.cpp`).

| Directive | Why |
|---|---|
| `Restart=always` | The daemon exits **0** for any stop signal, so under the old `Restart=on-failure` an out-of-band `kill -TERM` left the machine with no GPU control and systemd declining to act — while the same thing on Windows (`TerminateProcess`) is precisely what the SCM restart actions cover. `systemctl stop` still wins: systemd never restarts a unit it stopped. |
| `RestartSec=2` | Matches the SCM's first restart delay. The 2s/5s/10s escalation is deliberately **not** reproduced: `RestartSteps=`/`RestartMaxDelaySec=` need systemd ≥ 254 and would emit "Unknown key name" on older managers for a benefit the start limit already covers. |
| `StartLimitIntervalSec=600` / `StartLimitBurst=5` (in `[Unit]`) | systemd's 5-in-10s default is tripped almost immediately by a service with a 2s `RestartSec=`, and **once tripped the unit stays `failed` until someone runs `systemctl reset-failed`** — the SCM's reset period re-arms by itself. A ten-minute window gives the same shape: spaced-out driver events each get a fresh restart, a tight crash loop still stops. Note these keys belong in `[Unit]`, not `[Service]`. |
| `WatchdogSec=120` | A wedged serve loop used to sit there `active` while nothing re-asserted the fan. Generous on purpose: a VF apply with per-point readback verification is the longest thing that runs between two pings. |
| `After=nvidia-persistenced.service` | Ordering only, no `Wants=`, so a machine without it is unaffected rather than having a service started for it. |
| ~~`After=multi-user.target`~~ | **Removed.** The unit is `WantedBy=` that same target, so ordering after it scheduled Green Curve dead last in boot, behind every other service that might touch the GPU — plausibly a contributor to the boot-time race with another GPU tuning daemon described in the open questions below. |

`--service-remove` disables and deletes both units and runs `systemctl
reset-failed` on them, because a unit that spent its start-limit budget stays
`failed` until someone clears it and an uninstall is exactly when that
bookkeeping stops being useful.

### Watchdog protocol

`linux_systemd_notify.cpp` resolves `NOTIFY_SOCKET` **into a static** and then
unsets the variable, instead of re-reading it per send. The unset is what stops
a child process from notifying the manager on our behalf, but the watchdog has
to keep sending long after it — reading the env var per ping would have made
`WatchdogSec=` silently dead.

- `WATCHDOG_USEC` is the full deadline; the ping goes at half of it, clamped to
  `[20 ms, 1 h]` so a manager-configured sub-millisecond interval cannot turn
  the serve loop into a spin.
- `WATCHDOG_PID`, when present and not this process, disables pings entirely
  rather than keeping a unit alive from the wrong process.

## systemd sandboxing

The generated unit adds `ProtectSystem=full`, `ProtectHome=yes`,
`PrivateTmp=yes`, `ProtectControlGroups=yes`, `ProtectKernelLogs=yes`,
`RestrictSUIDSGID=yes`, `RestrictNamespaces=yes`, `RestrictRealtime=yes`,
`RestrictAddressFamilies=AF_UNIX`, `SystemCallArchitectures=native`, and
`LockPersonality=yes`.

Deliberately **not** applied, each pinned by a `forbid_text` guard so a future
"harden everything" pass cannot silently break the driver:

| Directive | Why not |
|---|---|
| `PrivateDevices=yes` | hides `/dev/nvidia*` |
| `MemoryDenyWriteExecute=yes` | the NVIDIA user-mode stack maps W+X pages |
| `ProtectSystem=strict` | the driver resolves libraries and state under `/usr` and `/sys` |
| `ProtectKernelTunables` / `ProtectKernelModules` | driver state lives under `/proc/driver/nvidia` |

**Verified on hardware 2026-07-27** (RTX 5070, Arch, driver 610.43.03): the
daemon reaches `READY=1` and publishes `healthy` with the full directive set
applied. `systemctl status` reports active/running with a fresh VF snapshot, so
none of the retained directives block the driver.

## Usage

```bash
greencurve --probe                  # confirm NvAPI+NVML, GPU, family, OC range
greencurve --self-test              # read-only validation of the apply path (arm64 pre-flight)
sudo ./greencurve-setup.sh install  # install + verify + group enrollment + desktop entry
sudo greencurve --service-install   # the privileged step on its own
greencurve --tui                    # edit + apply via the daemon
greencurve --apply-config           # apply the selected profile
greencurve --reset --apply-config   # reset OC/UV to driver defaults
greencurve --show-startup           # what the daemon applies at startup
greencurve --startup-profile 3      # ... make that saved profile 3
greencurve --resume-restore         # re-apply after suspend (the resume unit does this)
sudo greencurve --service-remove
```

`tools/greencurve-setup.sh` ships inside the Linux archive (a `.tar.xz`, see
[build.md](build.md)) next to the binary, LF-terminated and mode `0755`. Both
properties are load-bearing and both were once host-dependent: the script is
copied out of a working tree that is CRLF on Windows, and neither `os.chmod` nor
7-Zip can put a Unix executable bit into an archive on that host. A CRLF copy
does not run at all (`env: 'bash\r': No such file or directory`), and a
non-executable `greencurve` fails the wrapper's own `require_binary`
(`[ -x "$BINARY" ]`) before anything is installed. Packaging now normalizes the
text and writes the mode into the tar header; `verify_linux_tarball()` reads
both back.

It defers everything privileged to `--service-install` (which owns staging, the
group, the unit, and the socket/protocol verification) and adds only the two
things the binary deliberately refuses to do itself: `usermod -aG greencurve`
for the invoking `$SUDO_USER`, and a per-user `.desktop` entry. `uninstall`
keeps `/var/lib/greencurve` and the group unless `--purge` is given, because a
reinstall is the common case and a discarded tuned curve is unrecoverable.
The root wrapper performs desktop-directory creation and the atomic
temp-file/rename entirely through `runuser`; root never follows a path or
desktop-file symlink in the user-controlled home tree.
The generated `Exec=` value quotes and escapes the installed binary path, so a
path containing spaces remains one argument under Desktop-entry parsing.

`--service-install` registers systemd with `ExecStart=/usr/local/libexec/greencurve/greencurve --daemon`; it no longer points the root daemon at the caller's current executable path. Upgrades are in-place and do not require `--service-remove` first.

## TUI (`--tui`)

Dependency-free raw-terminal editor for configured intent plus hardware
readback. The design
invariant now lives in the full `linux_tui_layout*` family:

- **Pseudo-GUI workflow:** four fixed tabs expose VF Curve, Fan Curve,
  Profiles & Tools, and Advanced clocks. The Advanced tab edits XBAR clock,
  XBAR MSVDD, SYS clock, and video clock with live readback/measurement and
  domain-aware disabled states. Unsupported online rows never fabricate zero
  readback or expose edit actions; offline profile-only editing remains
  available. The VF tab has a Braille graph, selected-point card,
  absolute target-MHz fields, live/base/delta/rule columns, GPU offset, excluded
  low-point count, memory offset, power limit, and Windows-compatible
  OFF -> tick/FLATTEN -> dot/PIN tail semantics. Graph clicks select the nearest
  voltage point; deliberate graph dragging is not accepted as an editing source.
  The fan tab provides Auto/Fixed/Curve modes, live telemetry, a fan graph,
  poll/**Curve downshift** fields, eight enabled temperature/percent points,
  a Native zero-RPM toggle, and an independent 2-30C **Off gap** field. The
  first point is the fan-ON temperature, ON minus Off gap is fan-OFF, and the
  panel shows both computed temperatures plus the firmware-minimum duty caveat.
  Profiles
  provides Load/Save/Clear/Reset Draft plus Probe, Assets, and live text/JSON
  exports.
- **Scripted fan edits are partial and transactional.** Linux CLI parsing keeps
  an explicit fan-field mask, so `--fan-zero-rpm 1` selects Curve mode but
  preserves the selected profile's custom points, ON/OFF temperatures, both
  hysteresis values, and polling interval. `--fan-zero-rpm-hysteresis` changes
  only the OFF gap. After merging, an invalid curve is rejected without
  changing the profile instead of being normalized back to defaults.
- **The VF graph carries labelled axes** (2026-07-30). Clock (MHz) runs down the
  left margin, right-aligned on the gridline rows plus the bottom row; voltage
  (mV) runs along the row directly below the plot, with the unit riding on the
  highest tick. The panel previously ended with a single centred
  `450-1240 mV  •  180-3187 MHz` summary, so the two endpoints were the only
  numbers on it and nothing in between could be read off the trace.
  - The clock margin is taken out of the **plot's own width**
    (`TUI_GRAPH_Y_LABEL_WIDTH`, 5) and only when the plot still clears
    `TUI_GRAPH_MIN_PLOT_WIDTH` (12) afterwards, so a narrow terminal drops the
    labels rather than the graph. The MHz unit sits on the free row between the
    panel title and the plot, so naming the axis costs no plot row — the graph
    is 4 rows tall at the medium breakpoint and cannot spare one.
  - `graph_column_voltage()` is the inverse of the mapping the trace **and**
    `tui_nearest_graph_point()` use, so a tick cannot name a different voltage
    than a click on that column selects. Assertion 2076 checks exactly that.
  - Ticks are placed **right to left** and each needs a blank column on either
    side or it is dropped. Highest-voltage-first is deliberate: an earlier
    version reserved the right edge for a standalone `mV` and silently
    suppressed the maximum voltage, which is the number a reader looks for.
- **A numeric field's contents are pre-selected on entry** (2026-07-30).
  `tui_begin_edit()` seeds the buffer with the current value, and the character
  handler used to append unconditionally — so clicking a field showing `100` and
  typing `50` produced `10050`, and the value had to be cleared by hand first.
  The rule is `linux_tui_edit_policy.h`:
  - entering a field selects the whole value, so the first accepted keystroke
    replaces it; the selection renders in `TUI_STYLE_FIELD_SELECTED` (inverted
    against `FIELD_ACTIVE`) so the state is visible rather than discovered by
    typing;
  - clicking again into the field that is **already open** drops the selection
    and keeps the buffer, so typing amends the number. That path deliberately
    does **not** commit and reopen: a commit would push the value through the
    field's clamping and discard a partially typed number;
  - a rejected keystroke (a letter, a sign after the first position) leaves both
    buffer and selection untouched — it must not be the thing that silently
    discards a value the user can still see highlighted;
  - backspace over a selection clears the whole value, as in any GUI field.
- **One exact cell grid owns paint and input.** Every cell has one display glyph
  and style, and every action rectangle is registered against those same cells.
  `tui_layout_actions_valid()` rejects any out-of-bounds or overlapping hitbox.
  UTF-8 display-column helpers keep degree/check/dot glyphs one terminal cell.
  There is no independent printed-line or mouse-row counter to drift.
- **Responsive sizing:** 72x24 is the fail-closed minimum. Compact (<100), medium
  (100..139), and wide (>=140 columns) layouts reflow controls, graph/table
  widths, and virtualization while preserving a fixed header, tabs, action bar,
  and status row. The compiled layout matrix covers 72x24 through 220x70 on all
  tabs and checks exact cell count plus disjoint in-bounds controls.
- **Mouse:** SGR mouse (`?1000h`/`?1006h`) accepts left-button press for every
  interactive control. Wheel events are handled separately as scrolling and
  can never activate a control. Mouse hit testing consumes the same layout that
  was rendered.
- **Keyboard:** Tab/Shift+Tab are linear focus traversal; arrows are spatial
  traversal; Enter/Space edits or activates; Page Up/Down virtual-scrolls the
  current table; Ctrl+Page Up/Down switches tabs; Home/End selects the first/last
  VF point; F1 opens help; `g`, `s`, `l`, `r`, `1`..`5`, `q` provide direct
  actions. Numeric fields accept direct typing plus arrow stepping.
- **Intent and hardware are separate:** startup adopts the daemon's active
  Green Curve intent for editable target fields, but active intent is ownership
  metadata, not evidence that another controller has not overwritten hardware.
  `compare_intent_to_readback()` independently compares P0 GPU/memory offsets,
  power, fan policy/target, and owned VF/lock points. The header and panel titles
  show `INTENT MATCHES HW`, `READBACK PARTIAL`, or `HARDWARE OVERRIDDEN`;
  configured fields are explicitly labeled targets and actual scalar values
  remain visible. An override preserves the draft/intent for review and is
  **never automatically reapplied**; only explicit Apply writes again.
- **Live authority:** startup adopts the daemon's control/active-intent
  state before consulting the saved GPU target, not a profile. A saved target
  is selected only when it does not conflict with another GPU's active intent;
  otherwise the active owner remains attached so Reset is reachable. On a
  previously unselected multi-GPU daemon, the first forward/backward action
  explicitly selects the first/last adapter rather than inheriting the
  telemetry fallback. Loading a profile is explicit. Every draft binds to the
  accepted service instance, GPU generation, and exact GPU identity; only
  VF-dependent drafts also bind to topology. A daemon restart detaches every
  draft, while a topology change detaches only VF-dependent drafts. Offline,
  recovering, degraded, and ready states render separately with the typed
  health reason and remediation. VF editing is disabled without fresh VF data,
  while drafts limited to advertised independent NVML domains remain usable.
- **Profile fidelity:** Linux profile save/load round-trips `lock_mode` and
  `lock_tracks_anchor`. Older locks that omitted the mode, plus contradictory
  affected records with an enabled lock and `NONE`, recover as `FLATTEN`;
  explicit `HARD` remains hard-pin intent. Clearing a slot also removes
  selected/applied/app-launch/logon references to it and updates the legacy
  startup mirror.
- **Live curve dump:** `--dump-live` and `--json-live` report configured intent,
  actual GPU/memory/power/fan readback, checked/overridden/unavailable domain
  masks, maximum owned-VF delta, and every populated point with
  voltage/base/live/offset/target MHz, ownership, match state, mode, and rule.
  The TUI exposes equivalent local exports from Profiles & Tools. `--dump` and
  `--json` remain saved-profile exports.
- Graphics-domain boundary diagnostics are state transitions, not telemetry:
  the backend logs the first observed/truncated boundary and any later boundary
  change, never one identical line per one-second refresh.
- **Rendering:** changed-row diffing avoids whole-screen repaint during the
  one-second live refresh. Supported terminals additionally receive the
  synchronized-update escape around each batch, preventing partial frames.
  Truncated UTF-8 is consumed as one byte, so malformed terminal/status input
  cannot advance the renderer beyond the string terminator.
- **Crash-safe restoration:** a small parent process owns the original termios
  and terminal presentation state while the child runs the TUI. Fatal signals
  terminate the child normally; after `waitpid`, the parent restores termios,
  cursor, mouse mode, and alternate screen using normal APIs. SIGINT/SIGTERM
  handlers only set `sig_atomic_t` state; no stdio/terminal API runs in a signal
  handler. The parent restores **only when the child left the terminal
  modified** (termios compared against the pre-fork copy; the child is already
  reaped, so nothing races).
- **Exit hands the terminal back in one write.** `TUI_PRESENTATION_RESTORE` is
  the single definition of the restore sequence and ends with `\r\n`. Three
  behaviours were removed together because each was wasteful on its own and the
  combination produced the reported "needs one extra keypress" symptom on
  Konsole: an empty synchronized-update batch on an unchanged diff (the usual
  case during the 1 s live refresh), a render after `q` had already cleared
  `running`, and the supervisor's unconditional duplicate restore. Measured
  cause: a write ending *exactly* at `ESC[?1049l` left Konsole not painting the
  next write until further output arrived.
- **Scrolling stops at both list ends and stays where the user put it.** Three
  independent defects had to be fixed before the wheel behaved; fixing only the
  first left it looking like the list "wrapped back to the beginning":
  1. Both scroll paths clamped to `VF_NUM_POINTS - 1` instead of the last page
     of *populated* points. `tui_vf_max_first_visible()` is the pure bound (the
     index of the `visibleRows`-th populated point counted from the end, since
     the table skips unpopulated indices); the wheel and Page Up/Down share it
     through `tui_clamp_vf_scroll()`. The fan list uses
     `FAN_CURVE_MAX_POINTS - layout.fanVisibleRows`.
  2. `reveal_selected_point()` ran on **every** render, so any scroll that moved
     the selection off screen was undone on the next frame. It is now gated by
     `tui_selection_needs_reveal(selectedPoint, revealedPoint)` — reveal on
     *select*, never on render. `TuiState::revealedPoint` starts at -1 so the
     first frame still reveals.
  3. `tui_refresh_service()` clamped `vfScroll` down to `selectedPoint` once a
     second, which snapped the view back on the live-refresh tick. It now
     re-clamps against the end of the list instead.

  Measured before the fix (selected point 76, 16 visible rows): the view walked
  61 -> 64 -> ... -> 76 and jumped straight back to 61, forever. After: it runs
  to 111 (last page, point 126 on screen) and holds there, and wheel-up holds
  at 0.
- **The driver's flat low-voltage floor is not listed.** On this RTX 5070
  points 0..44 are every one of them 180 MHz at rising voltage: 45 identical
  rows that cannot be meaningfully edited, because a target written there is
  clamped straight back to the floor. `tui_vf_first_listed_point()` is the pure
  lower bound used by the table, both scroll clamps and the reveal, so the floor
  cannot be reached by scrolling either. Three safeguards keep this from being
  data loss: the count is stated in the panel header (`45 flat pts hidden`); a
  selection inside the floor (a profile lock, a graph click) *lowers* the bound
  so what the user points at is never invisible; and a run shorter than
  `TUI_VF_MIN_FLAT_FLOOR_RUN` is not a floor at all, so an ordinary rising curve
  is never trimmed and a wholly flat curve never produces an empty table.
- **A layout with no VF table carries no scroll information.** `draw_vf_table()`
  is the only writer of `layout.vfVisibleRows` and `build_tui_layout()` zeroes
  the layout every frame, so on the Fan or Profiles tab it reads zero. Treating
  that as "the maximum offset is 0" made the 1 Hz refresh rewind the list to the
  start of the curve *every time the user visited another tab*; coming back then
  showed the floor, because the selection had already been revealed once and so
  was not revealed again. `tui_clamp_vf_scroll()` now returns the candidate
  unchanged when there are no rows to clamp against.
- **The reveal top-aligns the selection with bounded context.**
  `tui_vf_reveal_first_visible()` starts the view `visibleRows / 4` populated
  points above the selection, clamped to the end of the list. The old rule
  walked back a *whole page*, putting the selection on the last row: on this
  RTX 5070 points 0..44 are all 180 MHz (the driver's flat low-voltage floor),
  so a tall terminal filled its entire table with identical rows and pushed the
  part of the curve being edited off the bottom. Measured at 77 rows: the list
  used to open at point 29, now it opens at 64.
- `reveal_selected_point()` returns whether it could act. A reveal attempted
  while offline or before the table has been laid out must **not** be recorded
  as done, or the selection would never be brought on screen once the daemon
  arrives.

## Desktop launch

Double-clicking the binary in a file manager gives it no controlling terminal,
and the TUI used to print "requires an interactive terminal" to a stderr nobody
could see and exit 1.

- `linux_terminal_should_relaunch()` requires **both** stdin and stdout to be
  non-tty plus a live `WAYLAND_DISPLAY`/`DISPLAY`. A single redirected stream
  (`greencurve --tui > log`) is still a terminal launch, and a headless session
  keeps the old error.
- `linux_terminal_select()` walks the colon-separated `XDG_CURRENT_DESKTOP`
  tokens (KDE -> konsole, GNOME -> ptyxis/kgx/gnome-terminal, XFCE, MATE, LXQt,
  LXDE, Deepin, COSMIC, sway/Hyprland -> foot/alacritty/kitty) and then a
  generic fallback list headed by `x-terminal-emulator`.
- Only emulators with a documented argv flag are listed, so the launcher
  `execvp()`s a fixed vector and **never builds a shell string**: `-e`, `-x`
  (xfce4/mate/terminator, whose `-e` takes one string), `--`, bare, and
  `start --` for wezterm.
- The relaunch appends `--from-desktop`, which both prevents recursion and makes
  a non-zero exit hold the window open ("Press Enter to close"); a clean `q`
  exits immediately so the terminal window closes. The generated `.desktop`
  entries quote/escape their executable path and pass the same flag.

## ARM64 support + robustness (no arm64 hardware to test on)

The build is fully cross-compiled + link-verified for `aarch64-linux-gnu`; these
measures maximize the odds it works on real arm64 NVIDIA hardware:

- **Compile-time layout proof.** `gpu_core.h` has `offsetof`/`sizeof`
  `static_assert`s on every private NVAPI/NVML struct (incl. the bitfields) + a
  little-endian assert. Because the arm64 binary is cross-compiled here, any
  AAPCS64 layout divergence **fails `python build.py`** — verified identical on
  x64 and arm64.
- **Read-only self-test** (`--self-test`, `linux_backend_self_test`): exercises
  getInfo + getStatus (curve) + **getControl (the same struct writes use, read
  without writing)** + NVML ranges, and reports whether the write struct version
  is accepted — a safe pre-flight that needs no GPU changes.
- **Write fail-safe:** atomic info/status/control reads validate accepted buffer
  versions, masks/counts, sane MHz/mV, and ordered voltage topology; a valid
  applied curve may have non-monotonic live frequency. A stale or malformed
  snapshot makes the entire relevant request fail in preflight with zero
  writes. NvAPI negative error codes (`-9` =
  INCOMPATIBLE_STRUCT_VERSION) are named in logs.
- **Driver inspection (no hardware):** `python build.py --inspect-aarch64-driver
  <extracted-driver-dir>` confirms a given aarch64 driver ships
  `libnvidia-ml.so` / `libnvidia-api.so` and exports the symbols we resolve
  (reads aarch64 ELF with the bundled `llvm-nm`).

### Support matrix
| Platform | Status |
|----------|--------|
| Linux **aarch64 + discrete NVIDIA GPU** (Grace, Altra+RTX, ARM workstation) | Target; NVML present, NvAPI VF path likely (verify per driver with `--inspect-aarch64-driver`) |
| Linux **Tegra/Jetson** | **Unsupported** — integrated GPU / L4T stack, no NVML; probe detects `/etc/nv_tegra_release` and says so |
| **Windows arm64** | Builds/runs, but no NVIDIA Windows-on-ARM dGPU driver yet → `--probe` reports "not found" (future-proofing) |

## Group enrollment and the messaging that goes with it

`greencurve-setup.sh install` **does** enroll the invoking account
(`usermod -aG greencurve`); that has always been the wrapper's job, since the
binary deliberately refuses to modify accounts on its own.

Two things were wrong around it, fixed 2026-07-28:

- **Account resolution gave up too early.** `target_user()` documented a
  fallback to "the owner of the login session" that was never implemented — it
  checked `GREENCURVE_USER` and `SUDO_USER`, then returned empty. A plain root
  shell (`su -`, `sudo -i`, a root console) therefore enrolled nobody and
  printed the manual command instead. The chain is now
  `GREENCURVE_USER` → `SUDO_USER` → `logname` → sole non-root `loginctl`
  session, with every candidate filtered through `usable_account()`: it must
  exist, not be root, and have UID ≥ 1000. Enrollment grants GPU
  overclock/undervolt control, so ambiguity (two logged-in users) deliberately
  yields nothing rather than a guess.
- **The install summary prescribed `usermod` unconditionally.** It told users
  who were already enrolled to fix a non-problem, and in the wrapper flow it
  printed the command on the line immediately before the script ran it. This is
  the same rule `linux_daemon_transport.cpp` already stated for the client
  diagnostic — *"advertising usermod on a run that works would send someone
  chasing a non-problem"* — now applied to the installer.

The decision is pure and testable (`linux_group_enrollment_advice` /
`linux_format_group_enrollment_advice` in `linux_service_install_policy.h`,
assertions 1950-1965):

| Situation | Message |
|---|---|
| Account already in the group | `Account 'x' is already in the greencurve group…` — no command |
| Known account, not enrolled | names it: `sudo usermod -aG greencurve x` |
| No account resolved | generic `"$USER"` form |

Membership is read from `/etc/group` **plus** the account's primary GID, because
a user whose primary group is `greencurve` never appears in `gr_mem`.

Ordering constraint: the group cannot exist until `--service-install` creates
it, so the wrapper must enroll *after* that step. The wrapper therefore sets
`GREENCURVE_SETUP_OWNS_GROUP=1` when it has resolved an account, which
suppresses the binary's group paragraph entirely — whoever performs the
enrollment owns the message about it. A manual `sudo greencurve
--service-install` is unaffected and still gets the tailored advice.

`greencurve --help` still lists the group requirement under "Daemon (root)".
That is a reference listing, not a post-install instruction, so it stays
unconditional.

## Generated asset files escape per format (F-03-003)

`--write-assets` (and the TUI's asset action) generates three files whose values
land in three DIFFERENT grammars, and exactly ONE escaping routine used to serve
all of them — `shell_quote_single()`, which is correct for the
POSIX shell and knows nothing about the other two:

```
Exec=sh -lc "exec '<path>' --tui --from-desktop --config '<cfg>'"
ExecStart=/bin/sh -lc "exec '<path>' --apply-config --config '<cfg>'"
ConditionPathExists=<cfg>            <- not escaped at all
```

- **Desktop Entry Specification:** inside a quoted argument, `"`, `` ` ``, `$`
  and `\` each need a preceding backslash, and a literal `%` must be `%%` or it
  is a field code (`%f`, `%U`).
- **systemd unit files:** `$` is `$$`, `%` is `%%`, and — the one that cannot be
  escaped — a **newline ENDS the directive**, so a path containing one has no
  representation at all and must be refused. The generated unit is meant to be
  installed with `sudo install -Dm644 ... /etc/systemd/system/`, so its
  contents are trusted input to a privileged step.

`--config` is copied straight from `argv` with `snprintf`, so the escaping and
refusal must happen at generation time rather than upstream.

`source/linux_asset_escaping_policy.h` is now the single gate, in two halves:
**refuse** what no grammar can represent (a newline or control character — the
same refuse-rather-than-sanitize argument `gc_archive_name_is_safe()` makes for
payload names), then **escape per format** (`linux_desktop_exec_escape()`,
`linux_systemd_value_escape()`; the shell quoter keeps its own job for the inner
`sh -lc` layer). `linux_asset_prepare_paths()` is the only entry point, because
the two halves must not be separable — escaping a path that should have been
refused is precisely the bug. Assertions 5286-5314; every escaper fails closed
on overflow rather than emitting a truncated value that would still parse.

The wiki's 2026-08-22 note that "setup-generated desktop paths are quoted and
escaped" was true only of the shell layer; read it that way.

## Open questions / stale-risk

- **Profile boot apply is hardware-exercised.** On the RTX 5070 (2026-07-28),
  a configured profile startup applied and verified its five phases and the
  immediate post-apply readback still showed the custom VF tail. A separately
  enabled external tuning daemon initialized about 100 ms later; its NVIDIA startup
  path resets clocks before applying its own configuration, which had no
  offsets/curve, so it replaced Green Curve's GPU/memory/VF settings. The first
  later TUI observation was not the reset time. Still unobserved on hardware:
  `none` suppressing replay, corrupt-record fail-closed behavior, and the TUI
  cycle control.
- **The snapshot refresh is unexercised against a live daemon** (as of
  2026-07-29). Its decisions, the wire contract and the record identity across a
  refresh are covered by F-LNX-STARTUP-SYNC, but no `REFRESH_STARTUP_PROFILE`
  has yet crossed a real socket, and the unbind-on-clear path in particular has
  only pure coverage. Exercising it needs a matched client+daemon reinstall,
  since an older daemon rejects a newer client outright.
- **The v16 `startupProfile` read-back is likewise unexercised on hardware.**
  The wire coherence rules are pinned (2026-2035) and the client's read is
  pinned by a source gate, but "the TUI shows PROFILE N STALE only when the
  snapshot really is stale" has not been watched on a live daemon. That is the
  first thing to check on the next Linux install: apply something other than the
  bound profile and confirm the row stays green.
- The terminal table is asserted against a fake probe, not against the real
  emulators. Konsole (`-e`) is confirmed on hardware; the argv flags for
  ptyxis, cosmic-term, deepin-terminal and lxterminal are **unverified**.

- New unrecognized GPU families use the future fallback backend and should be
  validated with `--probe` plus `--self-test` before trusting write behaviour.
- **RTX 5070 (GB205) acceptance passed 2026-07-27** on Arch/610.43.03: socket
  pathname verified `root:greencurve 0660`, single-GPU binding reaches a fresh
  127-point VF snapshot, and apply/reset/power/per-point/memory-offset all
  verified against the raw driver tables. The RTX 5080 report is likely the same
  two defects (NVML pciInfo ABI, per-domain VF status) and should be re-tested
  on this build.
- Memory offsets are quantized by the driver to a 2 MHz grid; graphics offsets
  are exact. See `nvml_clock_offset_grid_step()`. Whether the memory grid is
  2 MHz on every board/driver is **unverified** — only the RTX 5070 was measured.
- Fan behaviour **is** now exercised on hardware (2026-07-27): fixed 35%/55%,
  curve mode tracking temperature, the driver-range clamp, reset-to-auto and the
  shutdown handback all verified against `nvidia-smi` and the NVML policy
  readback. The fan *tab*'s live telemetry/graph rendering was still not
  observed in a captured frame — **unverified**.
- The Linux host now builds the complete Windows x64/ARM64 release matrix with
  a pinned native llvm-mingw bundle plus Zig. Resources, links, PE hardening,
  private symbols, archives, and setup executables are all verified. Keep the
  `llvm-mingw-linux/` cache separate from the Windows-hosted `llvm-mingw/`
  cache; the generic Linux `clang++` selects the wrong target, so x64 must use
  the target-prefixed `x86_64-w64-mingw32-clang++` wrapper.
- Multi-GPU telemetry remains available, but writes require a unique exact PCI
  identity selected by `--gpu` or the TUI. Ordinal fallback is single-GPU-only.
  The current daemon has one active-intent owner, so Reset is required before
  selecting a different GPU after Apply; per-GPU simultaneous intent would
  require a future runtime ownership model.

## Last verified

- 2026-09-05: **Linux PATH command symlink, XDG config policy, Arch Linux packaging.**
  Installed binary symlinked to `/usr/local/bin/greencurve` so running `greencurve`
  works from any terminal directory. Non-root user configuration resolved via
  pure `linux_config_path_policy.h` to `$XDG_CONFIG_HOME/greencurve/config.ini`
  (or `~/.config/greencurve/config.ini`) while preserving portable mode beside binary.
  Arch Linux PKGBUILD (source) and PKGBUILD.bin (binary) with systemd services,
  sysusers, and desktop integration added under `packaging/arch/`. Verified via
  `tests/regression_main.cpp` (F-LNX-CONFIG-PATH assertions 4860-4882),
  `tools/linux_gates.py`, `python build.py --test`, `python build.py --target linux --check`,
  and full release build.
- 2026-08-24 (build 118): Independent ordinary curve-downshift and 2-30C
  zero-RPM fan-off gaps, v23 profile migration, and field-masked Linux CLI/TUI
  editing passed pure/ASan regressions, 20k IPC fuzz iterations, packaged x64
  save/load/migration/error checks, and the full x64/ARM64 Windows/Linux release
  build. Live zero-RPM hardware behavior is unverified.
- 2026-08-24: Linux advanced-clock parity, rollback, sparse intent, override
  detection, capability publication, and the four-tab TUI layout passed the
  pure regression suite plus Linux x64 warning-as-error build. Live hardware
  application remains unverified.
- 2026-08-22: Linux logs are forced owner-only, malformed INIs are bounded by
  size/section/entry limits, and setup-generated desktop paths are quoted and
  escaped. Source gates cover all three; Windows-hosted Linux x64 cross-build,
  regression/ASan suites, and toolchain verification passed. Live desktop launch
  with a space-bearing install path and real NVIDIA hardware remain runtime
  checks.
- 2026-08-01 (pre-ship targeted fixes): the uncertain flag now gates every
  unattended write (`LINUX_AUTO_RESTORE_DENY_STATE_UNCERTAIN`, assertions
  3228-3235), and the pre-write GPU-not-available arm no longer sets it — one
  transient resume failure previously disabled fan reassertion and published a
  lockout until an explicit Apply/Reset. `python build.py --test` (including
  the installer/native suites), `--check --target all --arch x64`, `--tidy`
  (no new findings) and a full `python build.py` at build 535 pass. Still not
  verified on hardware: no suspend/resume or boot replay has been watched
  through the new gate, and no transient GPU-not-ready edge has been provoked
  on a real machine.
- 2026-07-29: Sparse Linux VF replacement now releases omitted old points in
  the same transaction that applies the committed replacement. Pure assertions
  2051-2053 cover a point moving from index 3 to 5 (index 3 must receive an
  explicit zero offset) and replacement inside a composed selective/lock
  policy. The backend logs the number of cleanup writes. `python build.py
  --test`, both sanitizer spellings, `--tidy`, `--check --target all`,
  `--fuzz --fuzz-runs 5000`, and the full build/package matrix pass at build
  480. Hardware transition behavior has not been re-run on the RTX 5070.
- 2026-07-29: **The boot-apply snapshot reaches the client (protocol v16).**
  Found in the pre-ship review of 7248a75..HEAD: the v15 entry below shipped a
  correct push path and a detector that read the wrong field. The snapshot rode
  in `ServiceResponse.desired`, which `daemon_stamp_state_envelope()` rewrites
  with the active intent at the end of every request, so `PROFILE N STALE` fired
  whenever the user had applied anything other than the bound profile, and a
  genuinely stale snapshot read as in-sync whenever the applied intent matched.
  Fixed with a dedicated `startupProfile`/`startupProfileValid` pair rather than
  by reordering, so `desired` keeps exactly one meaning and no handler/stamp
  ordering remains to get wrong. `python build.py --test`, `--check` (all four
  targets), `--fuzz` and a full `python build.py` pass; assertions 2026-2035 and
  source gates in `tools/linux_gates.py` cover it, with negative controls run on
  both. **Not yet exercised on hardware:** no `GET_STARTUP_POLICY` has crossed a
  real socket since the field moved, and the protocol bump again means client
  and daemon must be reinstalled together.
- 2026-07-29: **The boot-apply snapshot follows its profile (protocol v15).**
  `python build.py --test` (pure + Linux socket transport), `--tidy` (no new
  findings over the 42 baselined) and `--fuzz` all pass; Linux x64 + arm64 build
  and archive. Root cause established from the live machine before any code
  change: `/var/lib/greencurve/startup.bin` dated 2026-07-28 21:12 held
  `pollIntervalMs=1000`/`hysteresisC=2` while `config.ini` held `2000`/`4`, and
  the daemon's own log showed `startup profile 1 (profile 1) applied` at
  00:35:56 rewriting `active.bin` from that stale record. **Not yet exercised on
  hardware:** the new refresh command has no live daemon round trip yet — the
  protocol bump means the client and daemon must be reinstalled together
  (`sudo ./greencurve-setup.sh install`), and a v14 daemon rejects a v15 client
  with the header-first mismatch message.
- 2026-07-28: **Desktop integration, debug log, boot policy, scroll clamp.**
  `python build.py --test` passes; Linux x64 + arm64 build and archive; every
  Windows TU (GUI, service, arm64, installer) cross-compiles clean with
  `-Werror`. Verified on hardware in a real KDE Konsole: the TUI exit now shows
  the shell prompt with no extra keypress, a no-tty launch with a live Wayland
  display opens Konsole and closes it again on `q`, and the debug log appears
  next to the binary. The v13 daemon (build 17) was then installed and observed
  starting healthy, writing its log to `/var/lib/greencurve/`, replaying the
  committed intent under the default policy, and committing both a `none` and a
  `profile 1` policy. A restart *under* a non-default policy is still
  unobserved — see the open questions.
- 2026-09-09: **A board-default power request on a board with no power control
  surface no longer refuses the whole apply.** `linux_backend_preflight()` and
  the `unavailableDomains` gate both rejected `hasPowerLimit` when
  `snapshot->powerValid` was false, and a saved profile always carries the
  mandatory `power_limit_pct` key, so on such hardware every profile apply was
  blocked by a domain the user never touched — VF curve included.
  `linux_power_request_is_inert()` exempts a `POWER_LIMIT_DEFAULT_PCT` request
  there from both gates and from the write phase. **Ownership is deliberately
  not dropped**, so profile equality and active-intent recording are unchanged,
  and a non-default request still fails loudly. Parity fix for the Windows
  F-POWER-SURFACE bug; see [gpu-backend.md](gpu-backend.md).
  *Unverified on hardware* — reasoned from a Windows user log,
  covered by source gates in `tools/readback_gates.py`.
- 2026-07-27: **Fan control and the power-limit display fixed and accepted on
  hardware.** Manual fan writes were gated on `nvmlDeviceGetFanSpeed_v2`
  (measured, reads 0 under a zero-RPM fan stop) instead of
  `nvmlDeviceGetTargetFanSpeed` (intent), so every fixed/curve apply failed and
  rolled back; and `normalize_desired_settings_for_ui()` clamped the power limit
  to 0..100, rewriting a 105% target to 100% on load, on save, and on every TUI
  refresh. Verified: fixed 35%/55%, curve tracking at 500 ms, 27%->30% driver
  clamp, 105% -> 262.50 W with the TUI reading `[ 105 ]`, reset-to-auto, and the
  `systemctl stop` handback. `python build.py --test` passes; Linux x64 + arm64
  build; all Windows TUs cross-compile clean. See `fan-control.md`.
- 2026-07-27: **RTX 5070 (GB205, driver 610.43.03) hardware acceptance on Arch
  Linux — `--probe` and `--self-test` now PASS.** Two real defects were found
  and fixed, both of which had made the GPU completely uncontrollable:
  1. `nvmlPciInfo_t` declared a leading `busId[32]`, but the real NVML ABI puts
     `busIdLegacy[16]` first and `busId[32]` at offset 36. Every integer field
     was read 16 bytes early, so `pciDeviceId`/`pciSubSystemId` decoded as ASCII
     out of the trailing bus-id string (`0x3a37303a` = `":07:"`,
     `0x302e3030` = `"00.0"`). Identity matching then failed closed with
     `sole GPU identity conflict`. Confirmed byte-exact against the live driver
     before fixing; NvAPI's `2f0410de`/`89e71043` match NVML's once corrected.
     Windows also resolved only the v1 entry point, which never fills
     `pciSubSystemId`; it now prefers `_v3`.
  2. The VF status table concatenates all clock domains (see above); the
     graphics curve is 127 points on this GPU, so index 127 pulled in a foreign
     domain and failed the whole snapshot on `point 127 voltage topology is not
     ordered`.
  Covered by new pure suites F-LNX-PCI (byte-exact driver capture; mutation-
  verified — the old struct exits 1670) and F-LNX-VFDOMAIN.
- 2026-07-27: `python build.py --test` now builds and runs natively on a Linux
  host; it previously could not run at all there. See `testing.md`.
- 2026-07-16: Follow-up build 433 configures/verifies the real filesystem
  socket pathname, makes installer success depend on exact socket metadata,
  normalizes NvAPI/NVML internal/external PCI-ID representations, preserves
  stable NVML identity, and emits raw conflict diagnostics. Pure and ASan/UBSan
  tests, the cross-compiled native path fixture, Linux x64/ARM64 warning-as-
  error checks, and the full four-target/package build passed. RTX 5080 hardware
  acceptance remains pending.
- 2026-07-16: Protocol v12 daemon identity/health, deterministic restart and
  readiness verification, same-PCI discovery recovery, atomic VF freshness,
  domain-scoped degraded mutations, and TUI authority were covered by pure and
  socket-pair regressions plus Windows-hosted ASan/UBSan and Linux x64/ARM64
  warning-as-error check builds. Full release build 432 verified every
  Windows/Linux target and archive; RTX 5080 hardware acceptance remains
  pending.
- 2026-07-15: Release review pinned Linux lock-mode/anchor persistence, slot-
  reference cleanup, malformed UTF-8 bounds, and single-owner multi-GPU
  selection/fan-runtime behavior. `python build.py --test`, ASan+UBSan, warning-
  as-error Linux x64/ARM64 checks, and full build 425 passed; live hardware
  validation remains needed.
- 2026-07-15: Release 0.20 pseudo-GUI TUI, coherent Linux READY envelopes,
  reconnect-safe checked mutations, live absolute VF text/JSON export, and
  compact/medium/wide layout matrix were covered by `python build.py --test`
  and cross-compiled/verified for Linux x64 and ARM64. Live hardware behavior
  still requires release-machine validation.
- 2026-07-13: Protocol v10 mutation deduplication/query and operation journal;
  deterministic selective/explicit/lock curve composition; configured,
  interruptible hysteretic fan runtime; the then-intended socket permission
  proof; and parent-supervised TUI restoration were cross-compiled for x64/ARM64
  and covered by expanded compiled/source regression gates. The 2026-07-16
  follow-up proved that descriptor check targeted sockfs rather than the bound
  pathname and replaced it with pathname-relative verification. Live hardware
  behavior still requires release-machine validation.
- 2026-07-10: Linux daemon access onboarding now prints and documents the
  manual `greencurve`-group enrollment step; client `EACCES`/`EPERM` errors
  show the same remedy without claiming the daemon is stopped. Verified
  `python build.py --test` and `python build.py --target linux --check`.
- 2026-07-03: TUI mouse-offset root-cause fix (`linux_tui_layout.{h,cpp}`),
  keyboard focus navigation, left-button-only mouse filter, and live daemon
  Apply/Reset added. New F-LNX-TUI compiled test (hitbox lands on the drawn
  bracket + display-column X) and source guards. Verified `python build.py
  --test`, `python build.py --target linux --check`, and full `python build.py`
  (all 6 targets, build 349).
- 2026-06-28: daemon hardening verified by `python build.py --target linux
  --check` and full `python build.py`: staged
  `/usr/local/libexec/greencurve/greencurve` systemd path, root-owned
  non-writable parent validation, socket I/O deadlines, stale `active.bin`
  deletion on reset, and response bool canonicalization.
- 2026-06-20: full Linux binary compiles+links (`python build.py --target linux
  --check`); Windows + `python build.py --test` pass with the new Linux
  source-invariant checks (F-LNX).
