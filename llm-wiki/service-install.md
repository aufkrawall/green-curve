# Installing the background service

Source anchors: `source/service_admin_reason_policy.h`,
`source/service_scm_wait_policy.h`, `source/service_install_location_policy.h`,
`source/service_install_location.cpp` (`gc_service_install_location_verdict*`),
`source/service_acl_handle.cpp` (`apply_protected_service_dacl_to_handle`,
`release_service_hardening`), `source/main_service_install.cpp`
(`service_install`, `service_remove`, `wait_for_service_transition`),
`source/main_service_install_target.cpp` (`service_install_prepare_target`,
`service_install_harden_target`, release helpers),
`source/main_service_admin_client.cpp` (`launch_service_admin_helper`,
`wait_for_service_admin_helper`), `source/main_cli_admin.cpp`
(`cli_handle_service_admin_command`), `source/main_service_host.cpp`
(`service_report_start_progress`), `source/ui_main_window.cpp` (the checkbox),
`source/gui_service_state.cpp` (`gui_service_handle_admin_completion`),
`source/installer_apply.cpp` (`gc_register_service`), `source/installer_ui.cpp`
(`gc_commit_folder_page`).

## Summary

There are three doors into the same function, `service_install_or_remove()`:

| Door | Path | Elevation |
|---|---|---|
| GUI service checkbox | `ui_main_window.cpp` → worker → `service_install_or_remove` when already elevated, else `launch_service_admin_helper` | `ShellExecuteEx("runas")` re-runs `greencurve.exe --service-install` |
| `greencurve.exe --service-install` | `cli_handle_service_admin_command` | caller's; refused with a remedy when absent |
| Setup, step 5 | `gc_register_service` runs the newly installed `greencurve.exe --service-install` | setup is already elevated |

All three converge on `service_install()`, which pins the directory and
`greencurve-service.exe` beside `greencurve.exe` (the old "staging" copy was
dead code: source and target were always the same path) and **replaces that
directory's DACL** with a protected SYSTEM/Administrators-Full +
Users-Read&Execute one through the pinned handle, before it stops anything or
touches the SCM registration.

## The 2026-09-22 audit

A field report — "cannot get the service installed", no further detail —
prompted a review of all three doors. Six defects, in the order they matter:

### 1. The GUI checkbox threw away every failure reason (fixed)

`wait_for_service_admin_helper` reported `GetExitCodeProcess` and nothing else,
and every failure exited 1. The user saw

> Elevated service helper failed (exit code 1)

The real message went to the helper's own `greencurve_cli_log.txt`, which
`resolve_data_paths()` puts under the **LocalAppData of whichever account
approved the UAC prompt**. On a standard-user machine that is an administrator's
profile, and the GUI never named the file anyway. A user in this state has
literally nothing to report, which is exactly the shape of the field report.

`service_admin_reason_policy.h` now owns a `GcServiceAdminReason` taxonomy. The
helper's **exit code is the reason** (base 40, `UNKNOWN` deliberately stays 1 so
an old GUI still reads a failure); the parent decodes it and renders the full
sentence plus `GC_SVC_ADMIN_LOG_POINTER`, which names *whose* profile the log is
in.

**Rejected approach:** a shared handoff file. It would carry the verbatim
message, but it makes an elevated process open a path its unelevated caller
controls — the classic EoP shape. Closing it properly needs `CREATE_NEW`,
`FILE_FLAG_OPEN_REPARSE_POINT`, a no-`FILE_SHARE_DELETE` handle held open across
the elevation, *and* a nonce inside the file to defeat a hardlink swap. An exit
code needs none of that, and a taxonomy fine-grained enough replaces the Win32
number it cannot carry: "marked for deletion" is actionable, "error 1072" is not.

### 2. No Win32 error was ever translated (fixed)

Every failure read `(error N)`. `gc_service_admin_classify_win32(stage, err)`
now maps the codes that actually strand users. Stage matters:
`ERROR_ACCESS_DENIED` at the SCM is "not elevated"; on the binary it is "in
use". **1072 (`ERROR_SERVICE_MARKED_FOR_DELETE`) arrives on
`ChangeServiceConfigW`, not on `CreateServiceW`** — `OpenServiceW` still
succeeds for a service that is only *marked*, so the reconfigure path is what
refuses. It is also the one failure here that retrying cannot fix.

### 3. `--service-install` unelevated was a dead end (fixed)

README documented the archive route without saying it needs an elevated shell
(every Linux equivalent says `sudo`), and the failure was
`Failed opening service manager (error 5)` — printed *after* the shell prompt
returns, because the binary is `-subsystem:windows`. `service_install_or_remove`
now checks `is_elevated()` first and answers with a remedy. Verified live:
exit code 40, remedy sentence, no error number.

### 4. The service never published a wait hint or checkpoint (fixed)

`dwWaitHint` and `dwCheckPoint` were assigned **nowhere in the tree**;
`g_serviceStatus` is zero-initialized. START_PENDING went out once and nothing
moved until RUNNING, ~200 lines later, through legacy-artifact cleanup, config
migration, a ProgramData DACL rewrite, crash-artifact rotation, lifecycle worker
start and an `INFINITE` wait on pipe readiness. A wait hint of 0 satisfies the
SCM's hung-service heuristic immediately, so nothing — SCM, `sc`, Services.msc,
or our own installer — could tell a slow start from a hung one. That is the
condition behind *"did not respond to the start or control request in a timely
fashion"*, emitted by omission.

`service_report_start_progress(stage)` now bumps the checkpoint at each
expensive step; STOP_PENDING carries a hint too; every terminal state resets
both, so a stopped service is not advertised as still making progress.

### 5. Our own waits were tighter than the SCM's (fixed)

`GC_SVC_SCM_STATE_WAIT_MS` was 10 s against a 30 s default
`ServicesPipeTimeout`, so an install failed for a service Windows itself was
still willing to wait for — antivirus scanning a freshly written binary on first
run is enough. Now 30 s, with the reached state named in the message and the
elapsed time, checkpoint and wait hint logged.
`GC_SVC_ADMIN_HELPER_TIMEOUT_MS` (150 s) must exceed a stop wait plus a start
wait plus staging; the harness asserts that relationship.  *(Superseded by the checkpoint-following wait in the second pass below: 30 s
is now only the stall budget for a service that sends no hint.)*

### 6. A diagnostic sat on the critical path to RUNNING (fixed)

`service_log_path_protection_at_startup()` ran **before** RUNNING purely to emit
one log line, and it is the first caller of `classify_path_protection` in the
service process — so it built the admin trust set, a `LoadLibrary` plus
`NetLocalGroupGetMembers` against SAM. Moved to immediately after RUNNING is
published. Nothing consumed it synchronously.

## The 2026-09-22 review follow-up (second pass)

A code review of the day's commits found five more defects in the same area;
all fixed in one commit.

### 7. Moving an install left the old folder with NO DACL (fixed)

`gc_release_previous_dacl` (setup's move cleanup, since 0.21) called
`SetNamedSecurityInfoW(..., DACL | UNPROTECTED_DACL, pDacl = nullptr)`.
Windows stores that as "no DACL" (icacls reports that no permissions are set
and all users have full access) on the retained folder AND the
`greencurve-service.exe` inside it, i.e. a world-writable folder of our
executables. Fix: `release_service_hardening()` (empty explicit ACL +
UNPROTECTED, via a pinned handle), and `check_no_null_dacl_writes` in
`tools/security_gates.py` scans every `SetNamedSecurityInfoW`/`SetSecurityInfo`
call for a null 6th argument with `DACL_SECURITY_INFORMATION`. Verified the
gate flags both old call sites.

### 8. A refused or failed install stopped the working service (fixed)

`service_install_or_remove` stopped the existing service BEFORE the location
gate and hardening ran. `--service-install` from a refused folder, or any
hardening failure, left the previous service stopped (GPU reset to stock). Now
`service_install()` in `main_service_install.cpp` runs
`service_install_prepare_target` + `service_install_harden_target` first
(neither needs a stop), and only then stops. A failure after the stop calls
`restore_previous_service_after_failure`: restarts the old service if it was
running, and restores the old ImagePath if it had already been re-pointed and
the new start *failed* (SETTLED_ELSEWHERE). A start that is merely stalled or
out of time is not rolled back underneath a service that may still come up.
Source-order gate in `check_path_protection_gates`.

**Setup had the same shape**: `gc_install_execute` now creates, pins, re-judges
and hardens the target BEFORE capture/GUI stop/service stop (order gate in
`tools/installer_build.py`). Restoring the old service after a later setup
failure was added afterwards; see Open questions for what it covers.

### 9. Check and DACL write named different objects (fixed)

The check and the DACL write must name the same object. Both used to go by
path (`SetNamedSecurityInfoW`), and a path-based write follows junctions, so
the judged folder and the hardened folder could differ. Now
`main_service_install_target.cpp` opens the directory and the binary with
`FILE_FLAG_OPEN_REPARSE_POINT` and **without `FILE_SHARE_DELETE`** (neither
it nor an ancestor can be renamed while held), judges through the handle
(`gc_service_install_location_verdict_for_handle`), and hardens through the
same handle (`apply_protected_service_dacl_to_handle`, `SetSecurityInfo`).
Measured by harness 5757: handle-bound `SetSecurityInfo` still propagates the
inheritable ACEs to files already in the folder. The binary must also be a
single-link plain file (a hardlink would harden, and register, another file).

### 10. The location gate was a blocklist, not an ownership test (fixed)

Exact known folders only: `%LOCALAPPDATA%\Programs`, `C:\Windows\Temp`,
`C:\Program Files\<OtherVendor>`, a shared `D:\Tools` all passed and would
have been locked admin-only. `service_install_location.cpp` (moved out of
`service_path_chain.cpp`) now also refuses:

- anything inside the Windows directory (`GC_SVC_LOCATION_SYSTEM_SUBTREE`,
  by spelling and by resolved final path);
- `FOLDERID_UserProgramFiles` exactly;
- an existing folder whose entries are not all ours
  (`gc_service_location_entry_is_ours`: payload names, `.gcnew`, the legacy
  `.tmp`, legacy side files, `desktop.ini`/`Thumbs.db`; no subfolders, no
  reparse entries) → `GC_SVC_LOCATION_FOREIGN_CONTENT` — **unless the folder
  already carries exactly our DACL** (`service_security_descriptor_is_ours`),
  so existing installs with a stray file keep repairing.

Spelling checks run before any open, so a folder a standard user cannot open
(`C:\Windows\Temp`) is named SYSTEM_SUBTREE, not UNREADABLE.

### 11. Owner change was best effort and never verified (fixed)

`apply_protected_*` ignored the Administrators owner result, and
`*_is_hardened` never read the owner. A user-owned portable folder keeps
implicit WRITE_DAC. The handle path sets owner + DACL in ONE `SetSecurityInfo`
when elevated and reads both back exactly.

### Releases are proven, not assumed

Uninstall, re-point, and setup's move cleanup all go through
`release_service_hardening()`, which acts only when the DACL is *exactly*
ours. Before, uninstall reverted whatever directory the SCM ImagePath named
(only roots skipped) — a registration pointing at `C:\Program Files` itself
would have had Program Files re-inherited from the drive root. Roots are
still skipped even when ours (no parent to inherit from → empty DACL).

### SCM waits follow the checkpoint protocol

`service_scm_wait_policy.h` (pure) + `wait_for_service_transition()`: a
pending state is alive while `dwCheckPoint` advances within `dwWaitHint`
(floor 2 s, no hint = 30 s); one transition is capped at 60 s; a state that
settles elsewhere (STOPPED while waiting for RUNNING) ends the wait at once and
the exit codes are logged. Sampling, not `NotifyServiceStatusChangeW`: a
checkpoint change raises no notification; the interval is hint/10 clamped to
250–500 ms. `GC_SVC_ADMIN_HELPER_TIMEOUT_MS` (150 s) `static_assert`s
≥ 2 × 60 s + 30 s. The service now also publishes STOP_PENDING checkpoints
through teardown (`service_report_stop_progress`), with controls withdrawn.

### Considered and deliberately NOT done: restricting the service's privileges

`SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO` was recommended. Audit: the service
needs SeTcb (`WTSQueryUserToken`, `main_service_sessions.cpp`,
`main_data_paths.cpp`), SeImpersonate (`ImpersonateNamedPipeClient`), and —
because the updater runs setup as its child, which inherits the token —
SeAssignPrimaryToken + SeIncreaseQuota (`CreateProcessAsUserW`). With SeTcb
kept the gain is marginal, while NVAPI/NVML driver escapes might check a
privilege we would drop (NVIDIA's own `NVDisplay.ContainerLocalSystem`
requests no restriction, so there is no reference), and it cannot be verified
without an elevated hardware run. Service SID type was skipped for the same
reason: nothing would use the SID. A description (`SERVICE_CONFIG_DESCRIPTION`)
was added.

## Where the service may be installed from

Two different questions, and only the first existed before this audit:

1. **How protected is this folder?** `service_path_chain_policy.h`. Informational
   — the administrator decides, with consent (see
   [installer.md](installer.md#where-may-it-be-installed-path-protection-and-consent)).
2. **May we REWRITE this folder's permissions?**
   `service_install_location_policy.h`. A **gate**, because registering the
   service replaces the folder's DACL with an administrators-only-write one and
   propagates it to everything already inside.

Setup had always refused a drive root (`gc_install_directory_is_acceptable`);
the portable path refused nothing. README says "extract the `.7z` anywhere", and
7-Zip's *Extract Here* into Downloads is the obvious way to follow that — so
ticking the GUI's service checkbox turned the user's own Downloads folder into
`Users: Read & Execute`, no create, no write. On a drive root it was permanent:
`cleanup_secure_service_binary_after_remove` deliberately skips reverting a root,
so the volume was never given back.

`gc_service_install_location_verdict()` refuses:

- a drive root (`D:\`, `D:`, `C:/`) and a UNC share root (`\\server\share`);
- any well-known shell folder — profile, `C:\Users`, Desktop, Downloads,
  Documents, Music/Pictures/Videos, LocalAppData(Low), RoamingAppData,
  ProgramData, Windows, System32, SysWOW64, Program Files (both), Common Files,
  Public and its subfolders;
- anything not drive-absolute or UNC.

For an existing directory, it compares filesystem volume/file identity with
the current account's known folders. This catches an 8.3 path or an ancestor
junction that names the same directory; `GetFullPathNameW` alone does not.
Because UAC can run the helper as a *different* administrator, the resolved
target is also checked against standard shell folders and profile-local
OneDrive redirects under the machine's UserProfiles root, regardless of which
profile is current. An unreadable
existing target or a leaf reparse point refuses before any DACL write.

A dedicated **subfolder** is fine when it is not itself a shell folder, which
keeps `C:\Program Files\Green Curve` and a deliberate portable folder working.
Verified against a real machine (probe output kept in the local-only log).

The gate stands in front of all three doors: `service_install_prepare_target`
(portable/CLI, on the pinned handle), `gc_commit_folder_page` (setup's folder
page), and `gc_install_execute` (preflight by name, then again on the pinned
handle) — the last being the **only** check a silent `/S /D=<path>` run gets. The GUI checkbox also refuses before it asks for
consent, since a consent dialog for an operation that will be declined is just a
slower no.

The uninstall revert's root skip now calls the same predicate
(`gc_service_location_shape_is_acceptable`) instead of a second hand-written
copy. That asymmetry is what made hardening a drive root unrecoverable.

## Invariants

1. A reason survives `reason -> exit code -> reason` for every enum value, no
   two reasons share a code, and an unrecognized code degrades to `UNKNOWN` —
   **never** to `OK`. Asserted 5634-5642.
2. Every failure reason has a remedy sentence of its own; `OK` has none.
   Asserted 5643-5644.
3. Nothing hardens a folder the location gate refuses, and nothing the gate
   accepts is skipped by the uninstall revert. One predicate, both directions.
   Exact known-folder identity is checked even when the input uses a short
   name or an ancestor junction.
4. Elevation is checked before the SCM is touched, so the remedy for the most
   common failure costs nothing to produce.
5. A pending SCM state always carries a checkpoint and a wait hint; a terminal
   state carries neither. Both START_PENDING and STOP_PENDING advance it.
6. Nothing that can refuse without side effects runs after the running service
   is stopped; a failure after the stop restores the previous registration.
7. The object judged is the object hardened: one handle, reparse points not
   followed, no FILE_SHARE_DELETE while held.
8. A release (uninstall, re-point, setup move) only ever undoes a DACL that is
   exactly ours, and never writes a null DACL (source gate).
9. A waiter follows the checkpoint protocol; a start that settles in STOPPED
   is reported at once with its exit codes.

## Diagnostics / failure modes

- `service install: finished ok=%d reason=%d exitCode=%d` — the classification,
  at the exit.
- `service admin helper: exited after %llu ms exitCode=%lu reason=%d` — the
  parent's side of the elevation boundary.
- `service install: start wait finished after %llu ms state=%lu checkPoint=%lu
  waitHint=%lu pid=%lu` — separates "slow" from "never moved".
- `service install: REFUSED location token %s verdict=%s exists=%d
  alreadyHardened=%d entriesScanned=%u foreignIsDirectory=%d foreignIsReparse=%d`
  — the location gate. Path tokenized; foreign entry names never logged.
- `service state wait: desired=%lu verdict=%s after %llu ms state=%lu
  checkPoint=%lu waitHint=%lu win32Exit=%lu specificExit=%lu pid=%lu` — every
  SCM transition the install/remove path waits on.
- `service install: existing registration wasRunning=%d repoint=%d`, then
  `previous registration restored` / `previous service restarted after %s`.
- `service uninstall|re-point: binary|directory hardening release ... -> %s`
  (`released` / `not-ours` / `absent` / `failed`).
- `service_main: stop progress checkpoint=%lu stage=%s` — where a slow stop is.
- `service_main: start progress checkpoint=%lu stage=%s` — where a slow start
  actually is.

## Open questions / stale-risk

- **Nothing here has been exercised against a real failure.** The elevation
  refusal and the location verdicts were verified live on build 208; the 1072
  path, the start-timeout path and the helper-timeout path are covered only by
  the harness, because reproducing them needs a machine in that state.
- The known-folder identity check has read-only native tests for long/8.3
  names and an ancestor junction (5671-5673, 5680-5683). Other-account
  standard and OneDrive profile folders have a pure path-shape suite
  (5674-5679, 5684-5687). The coverage limits of the cross-account check are
  tracked in the maintainer's private notes.
- A final-path or machine UserProfiles-root lookup failure refuses an existing
  target rather than skipping the cross-account check.
- `GC_SVC_ADMIN_HELPER_TIMEOUT_MS` at 150 s is a bound, not a measurement. No
  real run has approached it.
- **The review-follow-up paths are unverified live** (no elevated account on
  the dev machine): the handle-bound hardening, the post-stop rollback, the
  re-point release and the checkpoint waits are covered by harness 5720-5786
  and source gates only. The installed 0.26 service still has no description
  until it is re-registered.
- Setup restores the previous service after a synchronous post-stop upgrade
  failure; a setup crash or power loss still has no durable recovery journal.
- The content allowlist is hand-maintained; a new file the app writes beside
  its binary must be added to `gc_service_location_entry_is_ours` or a repair
  of a not-yet-hardened portable folder will refuse.
- The GUI never learns *which* folder was refused — the reason text names the
  fix generically. Naming the path would mean logging it, which
  `log_redaction_policy.h` forbids in the clear.

Earlier verification: 2026-09-22 (second pass): `python build.py --test`,
`--test --asan`, `--tidy` (no new findings) and the full `python build.py`
matrix passed; rebuilt `greencurve.exe --service-install` unelevated still
exits 40. Earlier: 2026-09-22, build 212 (x64/arm64 Windows + Linux build green,
`python build.py --test` and `--test --asan` green; read-only native known-folder
tests passed; no real install click-through). The earlier elevation refusal
was confirmed by running build 208.

## 2026-09-23: Removal failure boundaries

`service_remove` previously treated *every* failed `OpenServiceW` as absence,
then released the registered binary's DACL. Only error 1060 proves absence;
access denial or another SCM fault now preserves permissions and returns a
classified removal failure. A stop that never reaches `SERVICE_STOPPED` now
preserves the registration and DACL instead of calling `DeleteService`, which
only marks a running service for later deletion. The shared absence predicate
has regression cases for 1060, access denial, marked-for-delete and unknown
errors; `tools/security_gates.py` pins the refusal before `DeleteService`.

`release_service_hardening` used to request `WRITE_DAC` before checking whether
an unrelated directory was ours. A protected unrelated path could therefore
produce an ACL failure instead of a harmless `NOT_OURS`. It now opens read-only
first, keeps that handle pinned without delete sharing while opening a write
handle only for a matching DACL, and rechecks on the write handle. The native
service-install fixture exercises release on an unrelated temp root; a source
gate pins read-before-write ordering. The fixture's unelevated handles no
longer request unused `WRITE_OWNER`, which varied with the host temp ACL.

SCM failure-action setup was best-effort after the old service had already
stopped: a restricted service DACL or policy could produce a "successful"
install with no unexpected-crash restart. Existing-service repair now requires
the failure actions before stopping the old service. New registration removes
its unstarted SCM entry if configuring recovery fails. The classified error
survives the elevated helper's exit code and tells the user to check policy or
security software. The source gate requires both branches and checks ordering.

No live elevated service removal was performed. Setup now has in-process
post-stop upgrade rollback for payload, SCM configuration, and the uninstall
record; see [installer](installer.md#upgrade-rollback). A forced setup crash
or power loss still has no durable recovery journal, and a disposable elevated
fault-injection fixture remains open.

Last verified: 2026-09-23. `python build.py --test`,
`python build.py --test --asan`, full x64/ARM64 Windows/Linux `python build.py`
and `python build.py --tidy` passed (no new findings). A rebuilt x64
`greencurve.exe --help` printed its warning and normal help with exit 0 when
the CLI log could not be opened. SCM failure injection and live elevated
install/remove remain unverified.
