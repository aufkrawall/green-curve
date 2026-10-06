# Windows installer

## Exclusive temporary-file creation (verified 2026-10-01)

Source anchors: `source/installer_transaction_files.h`,
`source/installer_apply.cpp`, `tests/service_install_tests.cpp`.
Payload extraction and staged installation exclusively CREATE_NEW their
`.gcnew` file with OPEN_REPARSE_POINT; nothing is ever written through a
pre-existing name. The staged copy reads a pinned non-reparse source and
writes a new file inheriting the target directory ACL. Rollback uses a
fail-if-exists copy to `.gcrestore`. Rename remains write-through;
acknowledged user-writable install targets still carry the documented
binary/directory trust risk and this is not a claim of power-loss atomicity.

**Leftover temporaries (review fix 2026-10-01).** The first version refused any
existing `.gcnew`/`.gcrestore` outright. A setup interrupted between creating
one and renaming it (power loss, killed process) leaves exactly that name, so
every later upgrade -- and every later ROLLBACK -- failed on it until someone
deleted it by hand. Now `gc_discard_stale_install_temporary()` removes the
name through a `DELETE` handle opened with `FILE_FLAG_OPEN_REPARSE_POINT |
FILE_FLAG_BACKUP_SEMANTICS` (a planted symlink/junction is deleted itself, its
target never opened; a hard link loses only this entry; a non-empty directory
cannot be deleted and stays a refusal), then the exclusive create is retried
exactly ONCE -- a name that reappears in between is refused, not raced. The
transaction logs `discarded a leftover ... from an interrupted earlier run`.
In the setup-created scratch folder (`gc_write_payload_file`) the folder is
fresh per run, so no recovery is needed there.

Native tests (`run_install_file_rollback_tests`, 6222-6241): a planted regular
`.gcnew` is discarded and the replacement succeeds; a planted symlink to the
backup is removed with its target untouched (runs when unelevated symlinks are
allowed); a non-empty directory obstruction is refused with its content kept;
a planted `.gcrestore` no longer blocks rollback.
Parser name/log validation also refuses CONIN$/CONOUT$ and superscript COM/LPT
DOS devices. Full elevated setup click-through was not performed.

Source anchors: `source/installer_archive_policy.h`, `source/installer_cli_policy.h`,
`source/installer_plan_policy.h`, `source/installer_uninstall_policy.h`,
`source/service_install_location_policy.h`, `source/service_path_chain.cpp`,
`source/installer_common.h`,
`source/installer_ui_internal.h`, `source/installer_main.cpp`,
`source/installer_ui.cpp`, `source/installer_ui_pages.cpp`,
`source/installer_theme.cpp`, `source/installer_apply.cpp`,
`source/installer_launch.cpp`, `source/installer_transaction.cpp`,
`source/installer_transaction_policy.h`, `source/installer_transaction_files.h`,
`source/installer_register.cpp`, `source/installer_register_install.cpp`,
`source/installer_prior.cpp`, `source/installer_stop.cpp`,
`source/installer_autostart.cpp`,
`source/installer_payload.cpp`, `source/installer_util.cpp`, `source/main_settings_transfer.cpp`,
`source/theme_palette.h`, `tools/installer_build.py`, `tools/build_state.py`,
`tools/pe_verify.py`.


## Source split: setup vs uninstaller (2026-09-23)

Two source lists in `tools/installer_build.py`:

- `INSTALLER_SOURCE_NAMES` — everything the setup stub links, including the
  install orchestrator (`installer_apply.cpp`, which `#include`s
  `installer_transaction.cpp`, `installer_launch.cpp`), the payload extractor
  (`installer_payload.cpp`), the ARP/shortcut writer
  (`installer_register_install.cpp`), prior-install discovery
  (`installer_prior.cpp`), and move cleanup (`installer_move_cleanup.cpp`).
- `UNINSTALLER_SOURCE_NAMES` — removal only. Omits every shard above and keeps
  `installer_register.cpp` (shortcut removal + `gc_uninstall_execute`),
  `installer_autostart.cpp`, `installer_stop.cpp`, `installer_util.cpp`, and
  the shared UI/theme.

`installer_stop.cpp` is a **real translation unit in both** binaries (it used
to be `#include`d by `installer_apply.cpp`). `gc_stop_gui_processes()` is
shared; `gc_stop_background_service()` is compiled only when
`GREEN_CURVE_UNINSTALLER` is unset. `gc_report`/`gc_set_error` live in
`installer_util.cpp` for the same reason.

`GREEN_CURVE_UNINSTALLER` also compile-times the install path out of the shared
shards (`installer_main.cpp`, `installer_ui.cpp`, `installer_ui_pages.cpp`).
A runtime mode flip is not enough: a reachable `else` branch keeps the whole
install orchestrator linked. `_verify_uninstaller_surface()` then proves the
built PE has no payload/install markers. Rationale:
[antivirus-heuristics.md](antivirus-heuristics.md).

The uninstaller ships as `greencurve-uninstall.exe` (product-specific leaf; the
generic `uninstall.exe` is itself an ML filename feature). `GC_SETUP_UNINSTALL_EXE_LEGACY`
keeps the pre-rename spelling so upgrade and uninstall still delete it. The ARP
`UninstallString`/`QuietUninstallString` point at the new name.

The standalone uninstaller now has its own manifest identity
(`GreenCurveUninstall`), window class, help text, and
`greencurve-uninstall-error.log` name. Its setup-only service ACL/path
translation units are omitted, its install-only service-reason strings are
compiled out, and the artifact gate rejects setup/payload markers in both ASCII
and UTF-16. The setup stub keeps its own `GreenCurveSetup` identity and log
name. This is metadata/static-surface hygiene; the removal sequence and
restart-pending self-cleanup behavior are unchanged.

### Upgrade reconcile (2026-09-23)


An in-place upgrade replaces every file the payload contains and leaves every
file it does not. After the rename that stranded the pre-rename
`uninstall.exe` — move cleanup only ran when the install *moved*, and uninstall
cleanup only ran on uninstall. `gc_remove_stale_payload_leaves()` now runs
after `transaction.cleanup()` (never before commit: a failed install must
leave the previous uninstaller working) and deletes **known setup-owned
leaves** the current payload did not write. Fail-safe: an empty/unconvertible
shipped list deletes nothing; a directory or reparse point squatting on a known
name is left alone; the install directory is never removed. Logging:
`upgrade reconcile:`.

## 2026-09-23 uninstall failure gate

The uninstaller used to log a failed `--service-remove` and continue deleting
its files, even when the SCM still registered or ran the service. It now checks
SCM state and captures the service process handle first, stops on an ambiguous
SCM result, refuses to delete files after helper failure, and waits for the
old process to exit. If the helper binary is missing while registration exists,
it preserves files and asks for a repair before another uninstall. The SCM
removal path itself now refuses deletion until STOPPED. Source-order gates in
`tools/installer_build.py` cover the uninstall barriers. This was not exercised
by a live elevated uninstall; it has build, source-gate and policy regression
coverage only. The later upgrade rollback work is described below.

## Summary

`python build.py` produces `greencurve-<version>-windows-<arch>-setup.exe` next
to the release archives for each native-Windows compiler variant, for x64 and
arm64. It is a self-contained Win32
program built from the same toolchain and hardening flags as the application,
with no third-party code and no NSIS. The previous `installer.nsi` was deleted
along with `generate_version_nsh()`.

A setup file is:

```
[ installer stub PE ][ stored GCAR container ][ 44-byte GcPayloadFooter ]
```

The footer is read from the end of the file because the PE image size cannot be
known without parsing the PE. `installer_archive_policy.h` owns the format and
every bounds check; `build.py --test` exercises it (assertion codes 1750-1775)
against truncated, overlapping, duplicate-named, and checksum-damaged inputs.

## Compression (none, deliberately — 2026-09-23)

Every setup file stores its container verbatim (`GC_PAYLOAD_METHOD_STORE`),
on every build host. `_append_payload()` in `tools/installer_build.py` never
compresses and `_verify_setup_file()` refuses any other method.

Why: an unsigned PE whose tail is ~1.2 MB of entropy-8.0 bytes in an unknown
format is the textbook shape of a packed dropper, and antivirus heuristics /
PE-feature classifiers score it that way (see
[antivirus-heuristics.md](antivirus-heuristics.md)). Stored, the overlay is the
same plain PE/text bytes the `.7z` ships (entropy ~6.8), which scanners can
inspect. Cost: ~0.8 MB of download (x64 clang-cl build: 1.52 MB -> 2.33 MB).

Before this, Windows hosts compressed with `COMPRESS_ALGORITHM_XPRESS_HUFF`
through cabinet.dll's Compression API (~58% payload) and Linux hosts stored.
**Published releases are built by `release.yml` on `ubuntu-latest`, so every
published setup file was already stored** — the change aligns local Windows
builds with what users actually download. The setup stub now rejects the old
XPRESS_HUFF method at footer validation and contains no decompressor. Its
payload is self-contained: a newly built stub reads only the bytes appended to
itself, while previously published setup files retain their own older stub.
`tools/pe_verify.py` checks the setup stub for the removed API names;
`tests/regression_main.cpp` covers method rejection and stored-size validation.

## Payload manifest

The container carries exactly the archive manifest that
`package_release_archive` already validated — `greencurve.exe`,
`greencurve-service.exe`, `README.md`, `LICENSE` — plus `uninstall.exe`, built
from the same sources with `-DGREEN_CURVE_UNINSTALLER=1`. Building from the
staged folder is deliberate: an archive and an installed copy are the same bits.

## Install ordering (the correctness property)

`gc_install_execute()` in `installer_apply.cpp`, guarded by
`require_order_in_operation` checks anchored to that function:

1. **Preflight the service root** — decode the final target and classify how
   well its whole parent chain can protect the LocalSystem service binary
   (`classify_path_protection` with `preflight_mode = true`, see
   [Where may it be installed?](#where-may-it-be-installed-path-protection-and-consent))
   before capturing settings, closing the GUI, or stopping the service. Preflight
   mode ignores `non_admin_danger` on the leaf target directory itself because
   setup replaces the leaf DACL with a hardened administrator-only DACL before
   payload extraction. Every folder the administrator can name is installable; a
   location that is not protected like Program Files requires the explicit
   acknowledgment the folder page collects, and the worker repeats the
   classification as a trust-boundary check for silent mode (which never blocks
   on the acknowledgment — the updater must keep working unattended). After the
   directory is created and hardened, the classification is recomputed with
   `preflight_mode = false` and a location that came out *less* protected than
   preflight vouched for fails the install closed. A rejected path therefore leaves
   a working installation running and untouched. Runtime GUI checks and service
   startup checks also use `preflight_mode = false` so post-install tampering or
   loosening of the leaf directory DACL is caught and flagged as untrusted.
   **Then, still before anything is disturbed (2026-09-22 second pass):** the
   target is created, opened with `FILE_FLAG_OPEN_REPARSE_POINT` and without
   `FILE_SHARE_DELETE` (the handle is held until setup returns), re-judged on
   that handle (`gc_service_install_location_verdict_for_handle`), hardened
   through it (`apply_protected_service_dacl_to_handle`, owner + DACL in one
   call, read back exactly) and re-classified. Changing a DACL needs nothing
   stopped, so a refusal here leaves the old installation running. Hardening by
   name used to follow a junction swapped in between check and write.
2. **Capture** — run `--export-active-settings <staging>` against the
   still-running old service, writing into an unpredictable, protected staging
   folder created directly beneath Program Files. The helper and exported
   settings stay under the administrator-only DACL until consumption; the
   elevated installer never stages executable content in the invoking user's
   writable `%TEMP%`. Afterwards there is nothing left to ask.

   **Which binary asks is a two-attempt rule** (`gc_capture_active_settings`),
   because neither candidate is right alone:

   - The **installed** binary is asked first. It is the only client guaranteed
     to speak the protocol the running service speaks — they were built
     together. It is used only when `gc_prior_supports_settings_export()` holds,
     which needs the ARP entry to name the same directory the SCM runs the
     service from *and* to say that the installed build knows the verb. A build
     older than the verb does not recognize it, and `handle_cli()` falls through
     to launching the GUI for an unrecognized argument — the first live run sat
     in front of an unwanted second window until the timeout.

     **The capability is recorded, not inferred.** Every install writes
     `GreenCurveSettingsExport = 1` beside the ARP entry, because the binary in
     the install directory is the only thing that actually answers the question.
     A version comparison is a proxy at *release* granularity and was already
     wrong once: `GC_SETTINGS_EXPORT_MIN_MINOR` says 0.21, but the verb arrived
     with this installer, twelve commits after `VERSION` had moved to 0.21 — so
     development builds exist that satisfy `≥ 0.21` and open their window when
     handed the argument. `gc_version_at_least()` is now only the fallback for
     installs predating the marker; a recorded answer wins in **both**
     directions, including `0` meaning "this build does not have it".
   - The **payload's** binary is the fallback. It always understands the verb
     regardless of what is installed, which is why it was once the only attempt;
     that is wrong across a **protocol bump**, where the new client refuses the
     old service's responses, reports "no active settings", and the upgrade
     restores nothing while looking like a clean run. Windows moved from
     protocol 14 through 16 during 0.21, so this is a real upgrade path, not a
     hypothetical one.

   Both attempts are bounded and terminated on timeout, and success is "a
   readable snapshot file exists", not an exit code. The export gets its own
   `GC_APP_EXPORT_TIMEOUT_MS` (20 s) rather than the general 60 s CLI bound:
   this is the one place setup runs a binary whose vocabulary it had to infer,
   so a wrong guess should cost seconds, not a minute of an unattended update
   looking hung.
3. **Stage and save** — prepare the complete payload, previous target files,
   SCM configuration, and uninstall values in protected storage before shutdown.
4. **Stop** — post `WM_CLOSE` to every `GreenCurveClass` window (the app's normal
   shutdown path: tray icon, single-instance mutex, service connection all
   released), wait on the process handles, escalate to `TerminateProcess` only
   after the bounded wait. Then stop the service and wait on its *process*
   handle — process exit is the real event, and it is also what unlocks the
   binary about to be overwritten. The stop step fails closed (2026-08-01,
   `source/installer_stop.cpp`): a GUI or service process whose handle cannot
   be opened fails the step instead of being skipped, window-enumeration and
   process-wait API failures are not treated as "no process", and a terminated
   GUI that does not exit within its post-termination wait still fails it.
   `WAIT_OBJECT_0` is the only successful wait result — "all exited" must be a
   fact, not an assumption, before the files are replaced.
5. **Extract** — into the target that was pinned and hardened before step 2. Each staged file is copied to `<target>\<name>.gcnew` and
   replaced with `MoveFileEx(REPLACE_EXISTING | WRITE_THROUGH)`.
6. **Uninstall record** — update the Add/Remove Programs values while the old
   values remain available for rollback.
7. **Register** — run the *new* `greencurve.exe --service-install`. The installed
   binary owns `CreateService`/`ChangeServiceConfig`, the SCM failure actions,
   and the hardened directory/binary DACLs; duplicating any of that in setup
   would be a second copy of the security model. The resulting SCM ImagePath is
   read back and compared against the target directory, so a move that failed to
   re-point cannot pass silently.
8. **Shortcuts** — updated after registration; failure to create an icon does
   not invalidate a working service.
9. **Re-apply** — run the new `greencurve.exe --apply-settings-file <temp>`.
10. **Retire the old folder** — only after the new registration and shortcuts
   are attempted. A setup-managed old directory loses only the five known setup file
   names (including `uninstall.exe`); `RemoveDirectoryW` removes it only when
   empty. Extra files keep their folder. A portable copy discovered solely
   through the SCM keeps its files. If the folder remains, its hardening is
   released by `release_service_hardening()` — only when the DACL is exactly
   ours, restoring inheritance with an EMPTY ACL. (Until 2026-09-22 this passed
   a null DACL, which Windows stores as Everyone: Full Control; see
   [service-install.md](service-install.md#7-moving-an-install-left-the-old-folder-with-no-dacl-fixed).)
   Old/new directory identity, resolved ancestry, and the original access-path
   ancestry are checked first: if they overlap, neither cleanup nor ACL release
   runs, because releasing an ancestor would weaken the new service binary's
   path (including when a mount point puts its files on another volume). A
   prior known folder or reparse target is never automatically deleted.

## Settings transfer

The export CLI now returns exit code 4 only after a successful service read
confirmed that no active intent exists. Code 0 still requires a readable,
round-tripped snapshot; any other outcome is a capture failure. Setup asks the
installed binary first, falls back to the payload binary only on failure, and
reports an unresolved capture on its completion page rather than silently
treating an old client's exit 1 as "nothing active." The same result taxonomy
lives in `source/settings_transfer_exit_policy.h`; assertions 5790-5795 cover
success, confirmed absence, old-client ambiguity, process failure, and missing
or contradictory snapshot evidence. The new CLI result is consumed only by
setup; the updater GUI calls the export function directly.

The updater adds `--settings-captured-by-gui` to both its relaunch and
no-relaunch setup commands. The authorized GUI has already handled capture,
and the SYSTEM child in session 0 cannot repeat that query.
`GcInstallContext.settingsCaptureHandledByGui` therefore skips setup's second
capture. For older services that omit the marker, silent setup verifies that
its live parent is the running SCM service in Session 0, with a creation-time
check against parent PID reuse (`gc_setup_launched_by_service()` in
`source/installer_main.cpp`). This supports both relaunch and no-relaunch
commands. A manual `--launch-session` alone never transfers capture ownership.
The parser accepts the explicit marker only with `/S`; assertions 5796-5801
check both command forms and the parser handoff, and 6474-6479/6680-6686 cover
legacy provenance and manual launches.

Without a handoff, Session 0 capture returns early as FAILED with `gc_log_fail`,
retaining the failure log and completion warning instead of treating capture
as unnecessary. The native parent probe logs query failures and the selected
handoff source without account paths. A live elevated launch remains an open
verification case; native wiring and failure reporting have source gates.

## Upgrade rollback

The 2026-09-23 rollback fix makes **synchronous setup failures** recoverable.
`GcInstallTransaction` in `source/installer_transaction.cpp` snapshots the SCM
command/start/display/recovery/description fields and each installer-owned
uninstall value, stages the complete payload in a protected private folder,
and copies every target payload file before the GUI/service shutdown. Each
replacement writes `.gcnew` on the target volume with an exclusive CREATE_NEW
and a bounded copy loop (since 2026-10-01; formerly `CopyFile2` -- either way it
inherits the target folder's DACL rather than copying the protected scratch
DACL), then renames atomically. `CopyFileW` is used only for the old-file backup/restore so
its original security properties survive. The uninstall record is written
before the service helper. Any
replacement, record, helper, or SCM read-back failure stops the service,
restores the old files, the prior SCM state and record, re-hardens a previously
hardened service location through pinned handles, and restarts a service that
was running before setup. When the old client is known to support the transfer
command and setup owns a settings snapshot, rollback also reapplies that active
intent; otherwise the failure message tells the user to reapply GPU settings
if needed. A failed recovery retains the protected copies and
logs their path; setup never claims the previous installation was restored in
that case. Shortcut updates are best effort and occur after commit, so they
cannot cause a working service to be rolled back. `installer_transaction_policy.h`
owns the transaction order; regression tests inject failure at each of three
replacement positions, record, and registration, then assert rollback.

**A failed service stop is not a rollback (2026-09-24).** It happens before
the first file, record or registration write, so the previous installation is
intact. It used to enter `rollback()`, whose first step retries the stop: a
stop that timed out waited out the timeout a second time, and a stop that kept
failing reported "Recovery failed; keep the protected backup", disarmed the
created-folder guard and kept scratch copies although nothing had changed. Now
`gc_run_install_transaction` calls a separate `stopFailed` callback;
`GcInstallTransaction::recover_after_stop_failure()` queries the SCM once and
applies the pure `gc_stop_failure_recovery(serviceWasRunning, state)`:
restart a previously running service that did go down (plus captured-settings
reapply under the same gate as rollback), leave an active/pending one alone,
never stop it again, always discard staging. The user sees the stop error plus
"Nothing was changed." and a note on the restart. Log lines:
`stop-failure recovery: service state N, pid P` and `... action=<name>`.
Regression 6109 (stop failure -> exactly one stopFailed, zero rollbacks, no
replacement; mutation-checked: reverting to `rollback(false)` fails 6109),
6110-6111 (decision table); `tools/installer_build.py` requires the call site
and forbids `gc_stop_background_service` inside the recovery. Not provoked on a
live machine.
The native Windows fixture uses disposable files to verify actual replacement,
restoration, failed staging, and failed backup recovery without touching SCM.

Folders created by the run are removed again on failure (2026-09-23 release
review): before this, a refused or rolled-back fresh install left an empty,
administrator-only folder behind, because the target is created and hardened
before `transaction.prepare()`. `gc_install_execute` counts the missing path
components first (`gc_count_missing_directory_components`) and a guard declared
BEFORE the pinning `targetHandle` (which lacks `FILE_SHARE_DELETE`, so the
guard must run after it closes) calls `gc_remove_created_directory_chain`:
leaf first, at most that many, non-recursive `RemoveDirectoryW` only. Disarmed
on commit and when rollback itself failed (retained backups). Log line:
`install failed: removed N of M folder(s)`. Native fixture codes 6130-6139;
gates in `tools/installer_build.py`. An empty rollback message prefix now reads
"Setup failed." instead of starting with a space.

This is an in-process rollback, **not crash/power-loss atomicity**. If setup is
forcibly killed between replacement and recovery, the protected staging folder
remains but no automatic startup recovery journal replays it. A live elevated
upgrade/failure fixture was not run on the development machine because its
Green Curve service is active; policy fault tests, the native file fixture,
and Windows builds cover ordering and file swaps, not SCM/registry mutation
under diverse policies.
Durable crash recovery and a disposable elevated integration fixture remain
open verification work.

The capture warning is a separate verification gap, not a reason to change
code now. During the next planned interactive upgrade or release check, use a
disposable installation to exercise failed capture and confirm the updater's
proceed/stop prompt and setup's final warning (`installer_ui.cpp`). Record the
observed result; do not mark this flow live-verified from the current pure tests
or source gates.

`source/main_settings_transfer.cpp` adds two CLI verbs to `greencurve.exe`:

| Verb | Behavior |
|---|---|
| `--export-active-settings <path>` | `refresh_service_snapshot_and_active_desired()`, then `save_profile_to_config(path, 1, ...)`, plus the active profile identity into `[transfer] active_profile_source` / `active_profile_slot`. **Fails when the service holds no active intent** — an upgrade with nothing applied must not "restore" a synthesized stock profile afterwards. The file is read back before success is reported, because the caller is about to stop every Green Curve process. |
| `--apply-settings-file <path>` | `load_profile_from_config(path, 1, ...)`, `resetOcBeforeApply = true`, then a normal `apply_desired_settings(..., SERVICE_APPLY_ORIGIN_CLI, <recorded identity>, ...)`. |

This does **not** weaken [auto-restore-policy.md](auto-restore-policy.md). The
restore is an ordinary explicit CLI Apply — the decision table's first row —
performed by a client on request. The service gains no new way to write on its
own, and a persisted snapshot still never authorizes anything.

Design notes:

- `resetOcBeforeApply` is required: without it a curve restored on top of
  whatever the freshly started service found would compound offsets.
- **The profile identity travels with the intent** (`[transfer]` section, kept
  separate from `[profile1]` so it cannot collide with a profile key). This was
  hard-coded to `AD_HOC` until 2026-07-30, on the reasoning that the file may no
  longer match the slot it came from. Sound concern, wrong answer: it paid for a
  hypothetical staleness with a guaranteed loss on *every* upgrade — the tray
  menu lost its tick and the tooltip said "Manual settings" until the user
  applied a profile by hand. The service now verifies a claimed slot against its
  own copy of that record
  ([config-profiles.md](config-profiles.md#the-upgrade-path-lost-the-identity-separately)),
  so an edited slot lands on `AD_HOC` by measurement instead of by assumption.
  A file from an older build has no `[transfer]` section, reads back as
  `SERVICE_PROFILE_SOURCE_NONE`, and claims nothing — so the first upgrade that
  preserves the identity is the one *from* a build carrying this change. Writing
  the keys is best-effort: correct settings without a slot name beat a failed
  upgrade over a label.
- `desired_settings_have_explicit_state(..., requireCurve=false)`: an exported
  intent legitimately has no curve when only power or fan changed. A *logon*
  profile still requires one.
- The installed binary performs the export when its capability is recorded;
  the payload binary is the fallback (see the capture step above). The service
  authorizes the caller's token/session rather than its image path, so a helper
  in the protected scratch directory is accepted in the active user session.

## Upgrade detection

`gc_read_prior_install()` reads `HKLM\...\Uninstall\Green Curve`
(`InstallLocation`, `DisplayVersion`, and the remembered
`GreenCurveStartMenuShortcut` / `GreenCurveDesktopShortcut` choices), and also
queries the SCM for the registered service's directory. **The SCM can disagree
with the registry** — a hand-registered portable copy has no ARP entry at all —
and that case is still treated as a prior install, so the service is re-pointed
instead of leaving two registrations behind.

`gc_install_build_plan()` (pure) then decides target directory, `isUpgrade`,
`directoryChanged`, `cleanupPreviousDirectory` (only with a prior ARP
`InstallLocation`), `repointService`, `captureActiveSettings`, and the three
checkbox values. An explicit command-line answer beats the previous install's
recorded choice, which beats the fresh-install default (Start menu on, desktop
off, launch on — but **off** when silent).

## Silent mode

`/S` or `--silent`, plus `/D=<path>`, `--dir`, `--start-menu`/`--no-start-menu`,
`--desktop`/`--no-desktop`, `--launch`/`--no-launch`, `--uninstall`,
`/log=<name>`, `/?`. The log override is a bare filename only; paths,
separators, drive prefixes, and device syntax are rejected. `/S` and `/D=` keep their NSIS spelling so existing notes
and scripts still work.

Unknown switches are **rejected**, not ignored: an auto-updater passing a typo
must fail loudly rather than install with defaults nobody asked for.

**That updater now exists** ([updates.md](updates.md)). The service invokes
`<setup> /S /D=<current install dir> --no-launch`: the directory is passed
explicitly because omitting it would let the installer's own default relocate an
installation the user deliberately put elsewhere, and `--no-launch` because the
service cannot know whether a GUI was running and would start one as the wrong
user. It branches on the exit codes below.

Exit codes (an updater will branch on these): `0` success, `1` failure,
`2` cancelled, `3` bad arguments.

The manifest requests `requireAdministrator`. A silent run started from an
already-elevated process is genuinely silent; started unelevated it still shows
the UAC prompt. That is inherent to registering a service, not a defect.

## UI

- **Fast double-clicks navigate, checkboxes do not toggle twice (2026-08-01).**
  A fast double-click on a `BS_OWNERDRAW` button arrives as `BN_CLICKED` then
  `BN_DBLCLK` (measured against the real button control; the trailing release
  adds nothing), so the old `BN_CLICKED`-only filter swallowed the second
  click of every fast double-click — rapid page navigation ignored the user.
  `installer_ui_click_policy.h` accepts `BN_DBLCLK` on the action buttons
  (Next/Back/Cancel/Browse) and keeps the checkboxes one-toggle-per-gesture;
  the wizard logs both halves so a future "click lost" report is diagnosable
  from the failure log.

- Client area always uses the Green Curve palette; the **title bar** follows the
  Windows light/dark setting via `DwmSetWindowAttribute` attribute 20 with a
  fallback to 19, refreshed on `WM_SETTINGCHANGE`. This is the same split the
  application's main window uses — Windows paints the caption itself, so forcing
  it dark under a light system would match nothing on the desktop.
- `theme_palette.h` is the single palette source, included by both `app_shared.h`
  and `installer_common.h`. A source gate forbids redefining `COL_*` next to it.
- **The window wears the Green Curve icon** (fixed 2026-07-30). `INSTALLER_RC`
  has always embedded it, which is what Explorer shows for the setup *file*, but
  the window class registered `LoadIconW(nullptr, IDI_APPLICATION)` — so the
  title bar, Alt-Tab and the taskbar showed the stock Windows executable icon
  while the file on disk looked right. `gc_load_setup_icon()` loads
  `GC_SETUP_ICON_ID` from the module for **both** class slots (`hIconSm` is the
  caption icon, `hIcon` Alt-Tab/taskbar; leaving the small one null makes
  Windows down-scale the large one, which is muddy at 16px). Sized through
  `GetSystemMetricsForDpi`, resolved dynamically for the same reason
  `gc_dpi_for_window()` resolves `GetDpiForWindow` that way, and reapplied from
  `WM_DPICHANGED` so dragging setup to another monitor does not leave a
  stretched caption icon. `LR_SHARED`, so the handles are module resources and
  are never destroyed. The id lives in two languages — `GC_SETUP_ICON_ID` in
  `installer_common.h` and `GC_SETUP_ICON_RESOURCE_ID` in
  `tools/installer_build.py` — and `check_all()` asserts they agree, because a
  silent disagreement reproduces this exact bug.
- Text sharpness needs all three of: per-monitor-v2 (manifest **and** a
  `SetProcessDpiAwarenessContext` call, since a launcher can strip the manifest),
  fonts built from the *window's* DPI via `SystemParametersInfoForDpi` rather
  than a scaled 96-DPI font, and `CLEARTYPE_QUALITY`. `WM_DPICHANGED` rebuilds
  the fonts **before** taking the suggested rectangle, so the relayout measures
  with the metrics it will paint with.
- Edit-control scrollbars are forced to `DarkMode_Explorer` regardless of the
  system theme, because the panel around them is always dark.
- **Path edit resets risk acknowledgment**: In `installer_ui.cpp`, editing the
  destination path (`EN_CHANGE` on `GC_ID_PATH_EDIT`) resets `wizard->riskAccepted = false`
  and redraws the risk check. This prevents a previously checked acknowledgment
  from carrying over silently if the user subsequently points setup to a different
  untrusted directory.
- **Expanded remedy buffer**: In `installer_ui_pages.cpp`, the remedy formatting
  buffer is 2560 bytes (expanded from 1024) to ensure long `mkdir` and `icacls`
  command chains are not truncated when rendered in the UI.
- **Cached path-protection queries**: In `main_service_admin_client.cpp`, the GUI
  status line timer ticks query `running_exe_dir_protection()`, which computes
  the path-protection classification of the running executable's directory once
  and caches it for the process lifetime (with `running_exe_dir_protection_invalidate()`
  available for dynamic invalidation). This eliminates repeated synchronous SAM/RPC
  and filesystem DACL queries every second on the UI thread.
- The work runs on a worker thread and reports via `PostMessage`, so the window
  keeps repainting and stays responsive to a monitor change while the service is
  being stopped.
- Pages: license (real `LICENSE` text out of the payload) → folder → options →
  progress → done. The header shows the version being installed. The uninstaller
  is the same window with a confirm page and a different worker.
- The installed GUI is started with the **desktop shell's token**
  (`CreateProcessWithTokenW`), not setup's admin token: the application manifest
  asks for `asInvoker`. `ShellExecuteEx` is the fallback.

## Live-run findings (2026-07-27, first real install)

The first end-to-end run on a real machine found two defects that no unit or
build-time check could have caught. Both are fixed and now guarded.

**Shortcuts were never created: `CoCreateInstance(ShellLink)` returned
`CO_E_NOTINITIALIZED` (0x800401f0).** `CoInitializeEx` applies to the calling
*thread*; `WinMain` initialized COM on the UI thread, but the install runs on a
worker thread that had never initialized it. The worker now calls
`CoInitializeEx(COINIT_APARTMENTTHREADED)` itself — matching the shell link
handler's own apartment model, so the object is created in-process with direct
calls rather than through a COM-hosted apartment and a marshalling proxy. Gated
by a `require_text_in_operation` check on `gc_worker_thread`.

**The settings capture blocked for the full 60 s timeout.** See the capture step
above: an unrecognized switch makes `greencurve.exe` open its GUI. Fixed at the
time by running the payload's binary exclusively — which was itself wrong across
a protocol bump, so the installed binary is asked first again, now behind a
version gate instead of being run blind. Additionally `gc_run_and_wait()`
`TerminateProcess`es any child that exceeds its timeout, so setup can never
leave a helper (or a stray window) behind holding files it is about to replace.

Also confirmed working in that run: payload decompression and checksums, the
prior-install adoption of a hand-registered service directory
(`C:\Program Files\greencurve`, no ARP entry), in-place upgrade without a move,
atomic file replacement, service re-registration with read-back verification,
the ARP entry, unelevated GUI launch through the shell token, and the failure
log being written only because something failed.

### The restore failed on every live run until 2026-07-29

The fix below was necessary and not sufficient: it made the transfer *fetch* a
READY envelope, but nothing ever carried that envelope's identity onto the
request built from it. `service_client_build_apply_request()` never set
`expectedServiceInstanceId` / `expectedGpuGeneration` /
`expectedTopologySignature` — only the GUI's `gui_mutation_stamp_request()` did,
from `GuiServiceModel` — so the restore was refused as malformed exactly as
before, and so was every other CLI mutation (see
[windows-architecture.md](windows-architecture.md#synchronous-clients-stamp-the-state-they-name)).
Assertions 1801-1805 tested the validator with hand-built requests and passed
throughout. The service's refusal could not even be read: it publishes no state
envelope to a caller that has not cleared the authorization gates, and the
client rejected that as a damaged response, so the restore surfaced as
"Operation … is still pending or its outcome is unknown".

### The restore itself failed on the second live run

`greencurve_cli_log.txt` said `Service request contains invalid protocol fields`.
`validate_service_request_for_ipc()` rejects a `SERVICE_CMD_APPLY` that does not
carry `expectedServiceInstanceId`, `expectedGpuGeneration`, a valid `targetGpu`,
and — for any VF-domain change — `expectedTopologySignature`. All four are
projected onto app state by `apply_ready_service_envelope_to_app()` after a READY
envelope, which `handle_cli()`'s normal apply path fetches in its preamble.
`settings_transfer_apply()` called `apply_desired_settings()` directly and
skipped that preamble, so every one of them went out as zero.

There was a second problem hiding behind the first: setup applies immediately
after `--service-install` returns, and the SCM reports RUNNING as soon as the
pipe listener and lifecycle worker are ready — *before* NVML/NvAPI have produced
a READY GPU phase. In the captured run only 18 ms separated the two.

`settings_transfer_wait_for_ready_service()` now fetches a READY envelope,
retrying against a 60 s deadline, before
`validate_configured_gpu_selection_for_client()` and the single write. That is a
**prerequisite** wait, which the auto-restore contract explicitly permits
("missing prerequisites may be retried; an actual hardware write may not"); the
write still happens exactly once. The installer's re-apply timeout was raised to
180 s to leave room for it.

Also fixed the reason this was invisible: a failed restore does not fail the
installation, so no log was written and the final page said nothing. The outcome
is now recorded in `GcInstallContext` and stated on the final page either way.

Pinned by assertions 1801-1805 (the exact invalid request, then each
precondition added in turn) plus a `require_order_in_operation` gate.

## Failure log

Buffered in memory for the whole run; written to
`greencurve-setup-error.log` next to the setup executable **only if a step
failed** (falling back to `%TEMP%` when that location is read-only, the common
case for a setup file run from a mounted image). A successful install leaves
nothing behind. Log creation uses `CREATE_NEW` plus
`FILE_FLAG_OPEN_REPARSE_POINT`; an existing name receives a random sibling
filename rather than being truncated or followed. The transcript includes the
steps that succeeded, because "how far did it get" is the first question a
support report has to answer.

## Uninstall

`greencurve-uninstall.exe` in the install directory; `UninstallString` and
`QuietUninstallString` (`... /S`) both point at it. `gc_uninstall_execute()` in
`installer_register.cpp` owns the order:

1. **Leave the install directory** (`SetCurrentDirectoryW` to the system
   directory). A process's own working directory holds a folder open exactly
   like an open file does, and an uninstaller double-clicked in Explorer
   inherits that folder as its CWD.
2. **Stop the GUI**, then run the installed binary's `--service-remove` (which
   also resets the GPU and reverts the hardened DACLs).
3. **Wait for the service *process*.** The handle is opened from
   `QueryServiceStatusEx` *before* the registration disappears, because
   `--service-remove` waits for SERVICE_STOPPED — a status the service reports
   from inside its own process — not for the process to exit. Deleting
   `greencurve-service.exe` in that window fails and pushes the whole folder to
   the next restart.
4. **Shortcuts**, then the two autostart registrations (below).
5. **Files setup installed**, and only those; the ARP key.
6. **The uninstaller's own running image**, handed to the session manager
   for the next restart (below).
7. **The directory**, which is now empty.

### Autostart outlives everything else

Neither autostart registration lives in the install directory or under the
uninstall key, so before `installer_autostart.cpp` existed both survived an
uninstall and pointed at a binary that no longer did:

| Registration | Written by | Removed by |
|---|---|---|
| `Green Curve Startup - <user>` logon task | `main_startup_task_runtime.cpp` | `gc_remove_startup_tasks()` |
| `HKCU\...\Run` value `Green Curve` (resident tray, `--tray-start`) | `main_tray_autostart.cpp` | `gc_remove_tray_autostart_values()` |

Both are registered **per user**, so a machine carries one of each per account
that ever enabled the feature. The uninstaller is elevated and is therefore the
only component that can reach all of them rather than only the ones belonging to
whoever clicked through UAC.

- Tasks are enumerated and deleted through `ITaskService`/`ITaskFolder` in the
  root folder (Green Curve never registers into a subfolder). The fallback, used
  only when the COM path is unavailable, lists `%SystemRoot%\System32\Tasks` for
  names and deletes through `schtasks.exe` — the on-disk layout finds names, but
  removing the file alone would strand the scheduler's registry bookkeeping.
  Success is verified by **re-enumeration**, because both deletion paths report
  failure for "already absent" as readily as for "refused".
- Run values are removed from every **loaded** hive under `HKEY_USERS`.
  Unmounted profiles are left alone: `RegLoadKey` over another account's
  `ntuser.dat` can corrupt it if that user signs in concurrently, and a Run value
  pointing at a deleted executable is inert. The shared value name is not proof
  of ownership: its command line is parsed and the first executable token's
  leaf must equal `greencurve.exe` case-insensitively. Prefixes, suffixes,
  malformed quotes, and an argument that merely mentions `greencurve.exe` do
  not authorize deletion.
- Neither removal can fail the uninstall. Every failure lands in the transcript.
- `installer_autostart.cpp` includes `<initguid.h>` so `taskschd.h` emits
  `CLSID_TaskScheduler`/`IID_ITaskService` in-place. `-ltaskschd` is **not** an
  option: mingw ships those symbols in a static UUID archive rather than an
  import library, and Zig's arm64 link step rejects the name outright.

### The uninstaller's own image goes at the next restart (2026-09-23)

Windows refuses to delete a mapped image, so the running
`greencurve-uninstall.exe` is scheduled with `MOVEFILE_DELAY_UNTIL_REBOOT`, and
with it the (otherwise empty)
folder. `gc_uninstall_execute()` reports this through its
`folderLeftForRestart` out-parameter; the finish page then says the folder is
deleted at the next restart, and silent mode logs it.

**Rejected: deleting the running image in place.** From 2026-07-29 until
2026-09-23 the uninstaller did exactly that: rename the file's default data
stream into an alternate stream (permitted on a mapped image), then a
`FileDispositionInfoEx(DELETE|POSIX)` unlink (measured on Windows 11 26200: the
POSIX form was the only one that worked; classic disposition and POSIX-alone
were `ERROR_ACCESS_DENIED`). It works, but it is a published malware
self-deletion technique (MITRE T1070.004) that EDR/antivirus behavior rules
watch for, and a false detection in the middle of an uninstall is worse than a
folder that lingers until the next restart. Also rejected: the NSIS/Inno
"copy the uninstaller to %TEMP% and run the copy" pattern — copying yourself
elsewhere and executing the copy is its own dropper heuristic, and an elevated
binary in a user-writable temp folder is a planting/replacement risk this
project would then have to harden against. `tools/installer_build.py`
`check_all` forbids `FileRenameInfo`, `FILE_DISPOSITION_FLAG_POSIX_SEMANTICS`,
`FileDispositionInfoEx` and `(FILE_INFO_BY_HANDLE_CLASS)21` in every
`source/*.cpp|h` (mutation-verified: re-adding one fails the gate).

Restart scheduling of the running image happens **only when it is the installed copy**
(`gc_uninstall_self_is_installed_copy`). `gc_uninstall_execute()` also runs
inside the setup stub launched with `--uninstall`, which normally sits in a
downloads folder — the old code scheduled *that* for deletion unconditionally,
quietly taking the user's setup file with it.

## Archive folder rename

`release_archive_root()` in `build.py`: Windows archives now use a `Green Curve`
root so extracting produces the same folder the installer creates; Linux keeps
`greencurve` (its service files, shell paths, and tarball conventions expect the
lowercase, space-free name). `_verify_archive_manifest` takes the root as a
parameter and still requires an exact match.

## Where may it be installed? (path protection and consent)

The old rule — a direct child of Program Files, hard-rejected everywhere else —
was a proxy for one property (F-SEC-1): the SCM registers a LocalSystem service
binary by absolute path and the driver-recovery design auto-restarts it, so
whoever can replace that binary, *or rename away any directory above it*, gets
SYSTEM code execution. `service_path_chain_policy.h` states the property
directly and `service_path_chain.cpp` proves it per path component: a plain
directory, admin- or SYSTEM-owned, not a reparse point, and no delete /
delete-child / WRITE_DAC / WRITE_OWNER grant held by any non-admin. Unproven
facts always classify *dangerous* — a false "protected" is the one verdict that
would let someone install a hijackable SYSTEM service while believing they were
told it was safe. Three rules carry most of the weight:

- **The walk starts at the DRIVE root (`X:\`), not at `GetVolumePathNameW`.**
  That call answers with the path a volume is *reachable* through, and a volume
  mounted into a directory (`C:\mnt\data`) hangs below ordinary directories a
  non-admin with DELETE can rename away. Starting at the mount path skipped
  those ancestors entirely *and* applied the "a root cannot be renamed" DELETE
  relaxation to a directory where it is false, so a substitutable chain read as
  protected. Only a true drive root gets that relaxation now.
- **Create-only grants are harmless on an ancestor and decisive on the leaf.**
  They cannot displace an existing protected child, but the leaf is the
  directory the LocalSystem binary resolves its DLL imports from: a standard
  account that can drop `version.dll` beside it owns SYSTEM without ever
  touching the hardened binary (`non_admin_create_danger`).
- **What setup BUILDS versus what it HARDENS.** Setup creates the whole missing
  tail but replaces the DACL of the final component only. A tail of exactly one
  is therefore fully covered — which is why `C:\Green Curve` under a stock
  drive root is green, even though that root hands Authenticated Users
  inheritable Modify. A longer tail leaves intermediates carrying whatever the
  last existing ancestor hands down, so inheritable non-admin danger there is a
  renameable parent above a hardened child and the chain is not protected.
  Outside preflight nothing is about to be created at all, so a missing
  component simply means the target is not there: unproven.

Beyond that the administrator decides, informed:

**A separate location gate runs before any directory DACL rewrite.** It
refuses drive/share roots and Windows-owned shell folders themselves, because
hardening one would take write access to unrelated files away. For an existing
folder it compares the directory's volume/file identity with known folders,
not just the path spelling: `GetFullPathNameW` does not turn `PROGRA~1` back
into `Program Files`, and an ancestor junction is another name for the same
folder. `SHGetKnownFolderPath` with no token reports the approving account's
folders, so the gate also checks the resolved target under the machine's
UserProfiles root for another account's profile and ordinary shell folders,
including profile-local OneDrive desktop/document redirects.
An existing leaf reparse point or an unreadable target refuses during
preflight. A dedicated child, including one under Downloads, remains allowed.
Since the 2026-09-22 second pass the gate is also an ownership test: anything
inside the Windows directory, `%LOCALAPPDATA%\Programs`, and an existing folder
holding files that are not Green Curve's (unless it already carries exactly
Green Curve's DACL) are refused — so `D:\Apps` as a target is refused while
`D:\Apps\Green Curve` is fine.

- **`chainProtected` (green)** — exactly as safe as Program Files. Any local
  NTFS/ReFS folder with an admin-protected chain qualifies (`D:\Apps\Green
  Curve`, `C:\Program Files\Vendor\Green Curve`), not just direct Program
  Files children. Default Program Files stays green (asserted read-only by the
  regression harness on every run).
- **Everything else installs with an explicit acknowledgment** that carries
  the risk verbatim: *anything that can write this folder — other accounts or
  software running as you — can replace the LocalSystem background service and
  gain SYSTEM rights* (pinned by build gates so it cannot silently vanish). A
  user-profile folder additionally means other accounts cannot run the copy; a
  network location is server-controlled (server-side ACLs are advisory and are
  never probed — which is also why typing a dead share cannot stall the folder
  page); a FAT/exFAT volume cannot express a DACL at all and is handled as a
  loud capability gap (hardening skipped and logged, never a mysterious
  failure).
- **The failing folder is named with a ready-to-run `icacls` recipe** — shown,
  never applied, because rewriting an existing folder's ACLs can break other
  software. The `mkdir …; icacls …` spelling covers a folder setup would
  otherwise create under inheritable danger two or more levels down. This
  recipe is how a custom path becomes Program Files-equivalent. Its exact
  spelling is part of the contract, because it is a line a user pastes: every
  path is quote-closed, `/setowner` is its OWN `icacls` invocation (icacls
  answers `Invalid parameter` / error 87 when it is combined with `/grant` or
  `/inheritance`), every `SID:permission` argument is quoted so PowerShell does
  not read `(OI)(CI)F` as a subexpression, and `;` separates the invocations.
  All three were wrong when the recipe first shipped and none of it fails
  visibly — the command simply does nothing — so the assembled string is now
  asserted by the harness and by `tools/security_gates.py`.
- **Both interactive paths collect consent, not just setup.** The GUI's
  background-service checkbox already raises a confirmation; when the folder
  the running copy lives in is not protected, that confirmation now carries the
  same headline and notes (`ui_main_window.cpp`). Without it, a portable copy
  on an exFAT stick registered a LocalSystem service with every DACL hardening
  step skipped and nothing but a log line to show for it — the install-anywhere
  change had turned a fail-closed refusal into a silent proceed.
- **Silent runs never block.** The updater re-installs the chosen path on
  unattended machines; the verdict is logged instead. The GUI status line
  warns whenever the *running* location is less protected than Program Files,
  and service startup logs its own directory's verdict
  (`service_log_path_protection_at_startup`). A chain loosened after the
  install informs; it never stops the service.

Known classifier limitations, all in the fail-safe direction: Administrators membership is
resolved one level (direct members of the local Administrators group), so a
domain user nested deeper inside a member group classifies untrusted and warns;
the drive-root DELETE relaxation is untested because no test may rewrite a real
root's DACL; and the drive-root *derivation* is pinned by a source gate rather
than behaviourally, because creating a directory mount point needs elevation
(the harness asserts the component shape instead, which does catch it on a host
whose `%TEMP%` is under a mount point).

The location gate recognizes the current account's redirected known folders
by identity and other accounts' standard folders under UserProfiles by their
resolved path. Do not describe this gate as proof that every chosen existing
folder belongs to Green Curve; it is the prevention for known destructive
destinations, alongside the dedicated-folder instruction. Its remaining
coverage limits are tracked in the maintainer's private notes.

The admin trust set (SYSTEM, Administrators, TrustedInstaller, direct members
of the local group) is built **once per process** behind `InitOnceExecuteOnce`.
It costs a `LoadLibrary` plus a `NetLocalGroupGetMembers` round trip — SAM, and
a domain controller for domain members — and setup reclassifies on every
keystroke in the folder page, so rebuilding it per call made typing stutter on
domain-joined machines.

## Invariants

1. The payload is fully validated — footer CRC, container CRC, every entry's
   name, range, and checksum — before a single file is written.
2. Payload names are bare file names. Anything with a separator, a drive letter,
   a wildcard, or a trailing dot/space is refused, never sanitized.
3. The path-protection classification is computed before settings capture or
   any process/service disruption, and the post-hardening re-check must not
   vouch for less than preflight did. Preflight mode (`preflight_mode = true`)
   exempts leaf non-admin danger because setup establishes the hardened DACL,
   whereas post-hardening verification, runtime GUI checks, and service startup
   checks use `preflight_mode = false` to enforce leaf DACL protection. Settings
   are then captured before anything is stopped, and re-applied only after the
   new service is registered.
4. The re-apply is an explicit CLI Apply, never a service-side snapshot replay.
5. A moved installation always re-points the SCM registration, and the move is
   verified by reading the ImagePath back.
6. Files under a previous install path are never deleted by setup.
7. The LocalSystem service directory is hardened (protected DACL and
   Administrators owner, through a pinned handle, never a reparse target)
   before the live installation is disturbed, and its parent chain is proven to be
   admin-controlled — a Program Files-shaped chain installs silently, and a
   substitutable chain installs only as the acknowledged, logged risk the
   administrator chose (see the section above).
8. Elevated capture helpers never execute from a user-writable parent.
9. Failure logs create a new regular endpoint from a bare filename and never
   truncate a caller-selected path.
10. A run without failures writes no log file; a successful install whose
    settings capture failed retains the failure log for diagnosis.
11. Setup never links or includes the application model (`app_shared.h`).
12. The settings capture is asked of a binary that speaks the running service's
    protocol *and* is known to understand the verb; the payload's binary is a
    fallback, never the only attempt.
13. An uninstall removes every autostart registration it can reach, and removes
    a task or Run value only when the pure predicate in
    `installer_uninstall_policy.h` accepts it. A match that is too wide is the
    one mistake here that cannot be undone; a Run command matches only when its
    first executable token has the exact `greencurve.exe` leaf.
14. The uninstaller deletes only its own installed copy; a setup stub run with
    `--uninstall` is never scheduled for deletion.
15. The install directory is scheduled for restart removal only when the files
    left in it are ours. Files the user put there keep their folder.
16. The stop step fails closed: every enumerated GUI process and the service
    process must be opened and proven exited (or already gone) before setup
    reports "stopped"; an unopenable process aborts the install/uninstall with
    a message instead of surfacing later as a file-replace failure.
17. Editing the destination path in setup resets risk acknowledgment (`riskAccepted = false`),
    requiring explicit re-consent for each untrusted path. The wizard hands
    `gc_install_execute` the user's ACTUAL answer
    (`pathRiskAcknowledged = wizard->riskAccepted`), never an unconditional
    true: an unconditional true left invariant 3's apply-side check inert on
    every GUI run while every build gate still passed.
18. GUI timer ticks consume a cached path-protection classification of the running
    executable directory, eliminating synchronous filesystem DACL and SAM/RPC queries
    on the UI thread. The cache is dropped in `end_background_service_toggle()`,
    which is exactly when the install directory's DACL changes.
19. Service install logging under user profiles tokenizes both target path and
    install directory to prevent user account paths from appearing in support logs.
20. No interactive path registers a LocalSystem service from an unprotected
    location without an explicit yes: setup's ticked acknowledgment, or the GUI
    checkbox's confirmation text. A filesystem that cannot express a DACL skips
    hardening rather than failing closed, so that consent is the ONLY thing
    standing between a portable exFAT copy and a world-writable SYSTEM service.
21. A known folder is refused by directory identity even when named through
    an 8.3 path or ancestor junction; another account's standard profile
    shell folders are refused when UAC runs under a different user.

## Open questions / stale-risk

- **The consent flow has not been clicked through live.** The classification,
  its fail-safe direction, the acknowledgment gating, the remedy string's exact
  shape, and the silent no-block path are unit-tested (pure matrix 5560-5623,
  walker fixtures 5500-5558, source-order gates), and the gatherer asserts
  facts against real synthesized DACLs — but no interactive setup window has
  been driven through the folder page on real hardware since the Program Files
  gate was replaced, and the GUI service-install confirmation's new
  path-protection paragraph has not been seen on screen either.
- **The location gate has not been exercised by a real install.** Read-only
  native tests cover current known folders by long/short path and through an
  ancestor junction (5671-5673, 5680-5683); pure tests cover another profile's
  conventional and OneDrive shell-folder layout (5674-5679, 5684-5687).
- **The exit codes for these suites were renumbered to 5500-5623** (from
  5400-5467, which an unrelated suite already occupied — a failure code no
  longer identified one assertion). The rest of the file still has older
  collisions in 5040-5221; they predate this work.
- **Resolved for the version-bound updater path on 2026-08-17.** Two complete
  updates against a throwaway public repository exercised WinHTTP, protected
  staging, verification, GUI stop, silent install, version-bound settings
  restore, and relaunch on real Windows hardware. The earlier three failed
  restore attempts were diagnosed and fixed before those successful cycles;
  see [updates.md](updates.md) for the chronological findings. The legacy
  capture path remains unit-tested rather than live-tested.
- Linux-host release builds compile, link, package, and PE-verify both setup
  stubs and uninstallers, and their payload/footer round-trip passes. A current
  public-release 0.24.0 -> 0.25.0 installed-client click-through has not been
  rerun; readiness is established from the unchanged updater/installer command
  contract, exact old-client policy/key compatibility, focused regressions, and
  the prior live version-bound cycles. Still unexercised beyond that: a
  *moving* upgrade, a fresh install with no prior state, and the uninstaller
  (see the separate entry below).
- The service resets the GPU when it stops with owned intent, so between the
  stop and the restore the GPU really is at stock. That is why a failed restore
  is visible as "my settings are gone" rather than "my settings stayed".
- **The uninstall path has not been run end to end on a real machine.** The
  pure predicates are unit-tested and the restart scheduling is a single
  documented `MoveFileExW` call — but no real `uninstall.exe` has removed a
  real installation since these changes. Specifically unverified: the task
  enumeration against actual registered logon tasks, the multi-hive Run-value
  sweep, the service-process wait, and the restart-pending finish-page text
  (2026-09-23).
- Run values in user hives that are not loaded (accounts not signed in) are
  deliberately left behind. They are inert once the executable is gone.
- `--export-active-settings` only exists from 0.21 on, so an upgrade *from* an
  older build cannot restore settings (that build has no such verb, and the
  version gate deliberately does not run it to find out). Expected and logged;
  not a defect.
- An upgrade from a build older than 0.21 that *also* crosses a protocol bump
  has no working capture path at all: the installed binary lacks the verb and
  the payload binary cannot talk to the old service. Both attempts fail fast and
  the install proceeds without a restore.

## Last verified

- 2026-09-27 (capture ownership review): explicit or verified legacy-service
  handoff skips export; manual relaunch requests preserve capture and unhandled
  Session 0 preserves its failure diagnostic. Regression suite, full build
  matrix, artifact diagnostics, and tidy ratchet passed. Cases 6680-6686 and
  source gates cover the change; live elevated updater/SCM launch unverified.

- 2026-09-23 (service removal audit): full Windows/Linux build and packaging,
  normal and ASan regression suites, source gates, and clang-tidy passed.
  Failed helper/SCM states were not injected into a live elevated uninstall.
- 2026-09-23: Distinct export exit 4 for confirmed no-active intent; failed
  captures now produce diagnostics and a completion warning. The updater setup
  marker is present with or without GUI relaunch. Pure cases 5790-5801,
  sanitizer regression tests, and the full build matrix passed. No elevated
  click-through or post-stop fault injection was performed.
- 2026-09-22 (review follow-up, second pass): folder pinned and hardened
  through one handle before capture/stop; move cleanup releases only an exact
  Green Curve DACL and never writes a null DACL; ownership-based location gate.
  Harness 5720-5786, `--test`, `--test --asan`, `--tidy`, full build matrix
  passed. No live elevated install/move was run.

- 2026-09-22 (moved-folder retirement): native temp fixtures 5690-5707,
  `python build.py --test`, `--test --asan`, `--tidy` (no new findings), and the
  full x64/ARM64 Windows and Linux build/package matrix at build 214 passed.
  The move against a real installed service was not run; retained-folder ACL
  fallback remains covered by source-order gates and the earlier installer behavior.
- 2026-09-22 (install-anywhere with consent): the "direct child of Program
  Files" gate replaced by the chain proof + acknowledgment model described
  above. `python build.py --test`, `--check --target all`, `--tidy` (no new
  findings), `--fuzz --fuzz-runs 5000`, and a full `python build.py` pass at
  build 535+; pure classification matrix, walker fixtures (synthesized DACLs,
  junction reparse, UNC short-circuit) and the read-only Program Files probe
  are in the regression harness. Not live-tested: the interactive folder-page
  click-through and an install into a non-protected folder on real hardware.
- 2026-08-01 (pre-ship targeted fixes): the stop step fails closed when a
  process handle cannot be taken, window enumeration fails, a multi-process or
  per-process wait fails, or termination cannot be proven
  (F-STOP-GUI-OPENFAIL / F-STOP-SVC-OPENFAIL / F-STOP-GUI-ENUMFAIL /
  F-STOP-GUI-WAITFAIL / F-STOP-GUI-PROBEFAIL / F-STOP-TERM-WAIT; the
  `installer_stop.cpp` size ratchet was not raised), and the wizard accepts
  `BN_DBLCLK` on action buttons so fast
  double-clicks advance pages (pure policy tests 3236-3239 plus the extended
  native owner-draw fixture that measures the full DOWN/UP/DBLCLK/UP sequence).
  `python build.py --test`, `--check --target all --arch x64`, `--tidy` (no new
  findings) and a full `python build.py` at build 535 pass; both setup binaries
  rebuilt and packaged. The fast-click behavior is reasoned from the measured
  notification sequence, not from clicking through a live setup window, and
  the stop-step failure arms have not been provoked on a live machine.
- 2026-07-29 (pre-ship targeted fixes): unsafe install roots are rejected by
  both the folder page and `gc_install_execute()` before capture/GUI close/
  service stop, with source-order gates for both boundaries. The Run-value
  predicate now requires an exact first-token executable leaf; assertions
  2045-2046 include `notgreencurve.exe`, `.exe.backup`, argument-only mentions,
  unquoted valid paths, and malformed quoting. Normal/sanitizer/ASan tests,
  tidy, all-target checks, 5,000 runs for each fuzz target, and full setup/
  archive packaging pass at build 480. Neither behavior has been exercised in
  a live install/uninstall yet.
- 2026-07-29 (after the uninstall leftovers fix): `python build.py --test` and
  full `python build.py` pass for x64 and arm64; assertions 2041-2050 added
  with matching source gates. The self-delete sequence was measured directly on
  Windows 11 26200 against standalone probe binaries, including both negative
  controls (superseded 2026-09-23: in-place self-delete removed, see "The
  uninstaller's own image goes at the next restart"). The
  autostart removal has **not** been run against real registrations.
  The 37 then-pre-existing tidy findings and 3 stale entries were resolved by
  the later pre-ship targeted-fixes pass above; the current baseline has 39
  entries and no new findings.
- 2026-07-29 (after the precondition/capture fixes): `python build.py --test`
  and full `python build.py` pass; assertions 2006-2025 added. Not confirmed on
  a live upgrade.
- 2026-07-27 (after the restore fix): `python build.py --test` and full
  `python build.py` pass; assertions 1801-1805 cover the apply preconditions —
  but they test the validator, not the client that builds the request, which is
  why the restore stayed broken.
- 2026-07-27 (after the live-run fixes): `python build.py --test` and full
  `python build.py` pass; both setup files rebuilt with the worker-thread COM
  init and the payload-binary settings capture.
- 2026-07-27: `python build.py --test`, `python build.py --test --asan`, and full
  `python build.py` all pass. The full build produced both
  `greencurve-0.21-windows-{x64,arm64}-setup.exe` (payload 59.3% / 57.8% after
  XPRESS_HUFF, round-trip verified), all four `.7z` archives with the new
  `Green Curve` / `greencurve` roots, and passed PE hardening plus ARM64
  BTI/PAC/AUT verification on the stub and the uninstaller.
