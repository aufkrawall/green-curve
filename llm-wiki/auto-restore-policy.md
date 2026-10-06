# Automatic settings apply and restore policy

> **Scope.** The decision table below is the *Windows* service. Linux reaches
> the same place by a different route and is documented in
> [linux-scaffold.md](linux-scaffold.md) — it keeps a mutating daemon start
> (`restore-last`) because it has no logon coordinator, and pays for that with
> a persisted per-boot crash-loop guard. The reset-to-stock rule below is
> genuinely shared code.

Last verified: 2026-09-23 (once-per-crash ownership handback, software only);
2026-07-31 (the shared restore-request builder; the rest 2026-07-12). Source anchors: `source/service_protocol.h`,
`source/service_lifecycle_policy.h`, `source/main_service_sessions.cpp`,
`source/main_service_lifecycle_events.cpp`,
`source/main_service_dxgi_readiness.cpp`,
`source/main_service_lifecycle_apply.cpp`,
`source/main_service_logon_coordinator.cpp`,
`source/main_service_apply_runtime.cpp`, `source/gpu_backend_apply.cpp`,
`source/main_service_persist.cpp`, `source/main_service_recovery.cpp`,
`source/main_service_controlled_restart.cpp`,
`source/main_service_recovery_clock.cpp`,
`source/main_service_recovery_ledger.cpp`,
`source/main_service_selected_gpu_pnp.cpp`,
`source/main_startup_task_runtime.cpp`,
`source/main_startup_task_definition.cpp`, and
`source/main_tray_autostart.cpp`.

## Summary

Green Curve writes settings automatically only in response to an authenticated,
coalesced lifecycle event. Starting, installing, repairing, or unexpectedly
restarting the service is never an automatic-apply event. A persisted settings
snapshot is intent, not permission to write.

The service is the only automatic hardware writer. The per-user Task Scheduler
job sends a settings-free logon handoff; it never supplies a profile or GPU
settings. The service derives the caller's Windows login identity and resolves
an immutable profile context itself.

## Decision table

| Trigger | Automatic behavior |
|---|---|
| Explicit GUI/CLI/hotkey/tray Apply | Apply. On success, clear sticky automatic-restore lockout and recovery history, record the active profile source/slot when known, and start a new 10-minute proof period. |
| WTS logon or authenticated scheduled-task handoff | Apply the configured account/shared profile once unless locked out. WTS and task events for the same Windows login are coalesced. |
| Standby resume | Reapply the complete current in-memory intent once without the proof-period gate. A successful restore preserves an already mature same-boot proof; an immature or unavailable proof starts fresh. |
| Confirmed selected-GPU driver recovery | Reapply once only after 10 minutes of awake stability, in the same boot, and below the recovery-spam threshold. |
| Service install/start/repair or ordinary SCM restart | Never apply or reset merely because the process started, **except** the once-per-crash ownership handback below. |
| Task Manager kill, crash, or failure-action restart | Never replay persisted settings. Hand back, once, what the dead instance still owned in this boot: fan to driver auto first, then the whole GPU to stock (the graceful-stop reset). |
| Expected VF/temperature/boost drift | Log diagnostically only. Never apply, clear ownership, or invalidate the applied-profile indicator. |

## Once-per-crash ownership handback (2026-09-23)

The old rule "startup never writes" was written against REPLAY (the settings
may have caused the crash; Task Manager must stop Green Curve; another tool may
own the GPU) and applied to every write. It therefore also forbade handing
state BACK, so a crash left a custom fan curve frozen at its last duty with
nothing tracking temperature, and left overclock settings nobody owned. A
graceful stop already reset to stock; a crash was the only exit that did not.

Current rule: **startup never replays; it hands back, once, what a previous
instance in the same boot provably owned.** Pure decision in
`source/ownership_handback_policy.h`; Windows runtime in
`source/main_service_ownership_handback.cpp`; Linux in
`source/linux_fan_ownership.h`.

- **Proof of ownership:** `service_ownership_marker.bin` (machine data dir),
  written durably at the pre-write boundary beside the proof invalidation
  (apply both paths, Reset) and removed only by a successful reset to stock.
  Fail closed: a write whose ownership cannot be recorded is refused.
- **Graceful stop returns unreturned writes (2026-09-23 release review):** the
  stop used to reset only when intent was active, so a process whose failed
  Apply disabled the intent (`service_disable_automatic_restore`) or whose
  Reset did not complete stopped cleanly with its marker still committed. The
  next same-boot start then ran a FULL crash handback for a stop that was not a
  crash, logged "previous instance left GPU state it owned", and could
  overwrite a tool that took the GPU in between. `ownership_graceful_stop_reset()`
  now resets on owned intent OR a marker this process committed; a merely
  started instance never commits one and stays non-mutating, and an inherited
  pending handback stays on disk for the next start. Log line:
  `graceful shutdown reset decision=`. Tests 5887-5889/5995 (mutation-verified:
  dropping the marker clause fails 5889); gate at the end of
  `tools/apply_ceiling_gates.py`. Not exercised on hardware.
- **Same boot only:** the marker carries the 128-bit BootIdentifier. Another
  boot, a corrupt marker or an unknown current boot: discard, no write.
- **Bounded:** the marker counts attempts (`handbackAttempts`), stored +1
  before each attempt writes and reset to 0 when an attempt returns. A nonzero
  count at start means an attempt died (crash, or a hang the wedge watchdog
  restarted); it is retried once in the fresh process (fresh driver DLLs cure a
  stale-image wedge). At `OWNERSHIP_HANDBACK_MAX_ATTEMPTS` (2) the start gives
  up, latches lockout reason `HANDBACK_INCOMPLETE` and writes the error log
  instead of looping (SCM restarts us indefinitely: its last failure action
  repeats).
- **Hangs are detected (2026-09-23 follow-up):** Windows cannot see a thread
  hung inside `nvml.dll` in a running service (the SCM only reacts to exit or
  start/stop deadlines; kernel TDR only resets a hung GPU). The service's own
  wedge watchdog used to fire only for a queued fan pulse, so a hung handback,
  Apply or Reset with no fan curve running hung forever. Those now run inside
  `ServiceHardwareWorkScope` (`service_gate_progress.h`) and stamp progress;
  30 s without progress (`service_wedge_watchdog_policy.h`) takes the existing
  `service_emergency_restart_from_poisoned_runtime()` path. A wedged Apply
  therefore now ends, after the restart, in the crash handback to stock.
- **User-visible:** `SERVICE_AUTO_RESTORE_LOCKOUT_HANDBACK_INCOMPLETE` (protocol
  v28) is the one lockout reason the Windows GUI announces ("Service stopped
  unexpectedly; GPU may not be at stock. Press Reset."). A successful Reset
  downgrades it to `AUTOMATIC_APPLY_FAILED` (automation still needs an explicit
  Apply, but the hardware is no longer unknown). Linux publishes it through the
  guard; its explicit Reset clears the guard as before.
- **Scope:** ordinary start = FULL (fan to auto, 100% if refused, then
  `service_reset_all`). Validated controlled driver recovery = FAN_ONLY; its
  own recovery write replays the full intent after the proof.
- **When:** decided synchronously before RUNNING (file I/O only); performed by
  the lifecycle worker after RUNNING, first in its loop, when the GPU is ready.
  Any other write that arrives first (explicit/logon/standby/recovery apply)
  runs it before itself; an explicit Reset takes it over.
- **Emergency stop still works:** a Task Manager kill now ends at stock after
  the SCM restart, which is what "stop" should mean.
- **Hardware-confirmed 2026-09-24 (Windows, RTX 5070):** the maintainer killed
  the service twice, once with a custom fan curve and once with a fixed fan
  duty, each over an OC profile. Both restarts logged `verdict=run scope=full
  ... previousAttempts=0/2`, `fan -> driver auto ok=1` about 3 ms into the
  handback, `reset to stock ok=1` about 1.1 s later, then `ownership marker:
  retired` and `complete scope=full`; fan and OC observed at driver defaults.
  Still unexercised on hardware: the retry/give-up branch (an attempt that
  itself dies), the controlled-recovery FAN_ONLY scope, and the Linux daemon
  handback.
- Diagnostics: `ownership handback:` and `ownership marker:` lines;
  failures also go to the Green Curve error log.
- Linux: the handback runs before `READY=1`, so systemd's start timeout kills a
  hung one; the same attempt budget applies.
- Open: never observed on hardware. A thread stuck in an uninterruptible kernel
  wait can delay process exit even after the watchdog's `ExitProcess`.

## Reset-to-stock is part of a restore

`service_build_full_restore_request()` (`service_lifecycle_policy.h`) is the one
place that decides whether an automatic replay resets the OC baseline before
writing. Standby restore and driver recovery both build their request through
it, and as of 2026-07-31 so does every unattended Linux write — the Linux boot
replay used to apply its persisted `DesiredSettings` directly, which meant no
reset at all, because `service_merge_desired_after_mutation()` strips
`resetOcBeforeApply` before the intent is persisted.

The predicate is `service_intent_owns_vf_cleanup()`: GPU offset, any curve
point, **or a lock**. A lock was missing from this function until 2026-07-31
even though its sibling in the same header had always counted it, so a lock-only
profile reset its baseline when it *replaced* another profile but not when
standby restored it — a pin laid onto whatever curve the driver already held.
A memory-, power- or fan-only intent still does not reset: those fields may
belong to another tool.

Automatic app-launch and foreground-profile actions use the normal apply IPC,
but are typed as automatic origins: they honor sticky lockout and cannot clear
it. GUI Apply, explicit CLI Apply, a profile hotkey, and a tray profile choice
are typed explicit origins and may re-arm restoration only after a successful
write.

Once a real hardware write begins, failure is terminal for that event and
latches automatic restoration off regardless of origin. A pre-write rejection
or missing prerequisite is not a failed hardware write: it is either rejected
without mutation or remains pending on a real readiness signal.

The service invalidates the previous proof immediately before the first real
hardware write, after all zero-write validation. If that protected invalidation
cannot be committed, the operation aborts before touching the GPU. This prevents
an interrupted or failed mutation from leaving an older stability proof usable.

## Logon authorization and coalescing

Every `greencurve.exe --logon-start` invocation sends
`SERVICE_CMD_LOGON_HANDOFF` and then exits. The request contains no profile or
GPU settings. Resident tray startup is a separate per-user `HKCU` Run entry
using `--tray-start`; separating the processes prevents the task's three-minute
execution limit from terminating the tray GUI. The
pipe server uses the authenticated client process/token and queues the active
session identity.

A Windows login identity is the tuple:

- WTS session ID;
- user SID;
- `TokenStatistics.AuthenticationId` (the login authentication LUID).

The authentication LUID matters because Windows may reuse a session number for
the same account after logout/login. Logoff cancels matching pending work and
clears its debounce state. A later login with a new LUID is therefore a new
event.

The long-lived lifecycle worker owns prerequisite handling. It coalesces WTS
and task events for one identity, waits for real identity/config/PnP/DXGI readiness
signals, and keeps the intent pending until success, logoff/identity
supersession, explicit Apply/Reset, lockout, service stop, or a terminal write
failure. There is no arbitrary 30-second apply deadline and no Task Scheduler
repeat-on-failure loop. Missing prerequisites may be retried; an actual hardware
write never is.

Configuration Manager can report the selected device `STARTED` before NVIDIA's
user-mode API is usable. The service therefore also registers DXGI's adapter-set
change event before the worker reports ready and waits on it for the service's
entire lifetime. A recovery that is still not ready after PnP arrival remains
pending without a deadline; re-enabling the GPU minutes or hours later can wake
the same intent when the adapter reaches the user-mode display layer. The DXGI
registration itself performs no GPU access and uses no polling, sleep, timer,
or additional worker thread.

The service does not report `RUNNING` until the lifecycle worker and pipe
listener are ready. Worker creation failure stops startup. If the worker later
dies unexpectedly, the service latches automatic restoration off and exits;
the resulting ordinary SCM availability restart has no controlled-recovery
nonce and cannot replay settings.

The worker resolves config/profile data locally rather than using mutable global
config-path state. It arms directory watches before reading, waits
interruptibly for the cross-process config transaction, and reads the GPU
selector, profile selector, protected policy, and profile contents coherently.
An explicit eligible personal/shared selection that is unavailable remains
pending and never degrades to a machine default. The selected GPU is resolved
from its stable PCI identity; missing/ambiguous matches, and legacy ordinals on
multi-GPU systems, block all automatic writes until explicit reselection.
Temporary profile mounts/atomic replacements remain pending and background UI
repair never erases a saved selector or disables a task/Run entry from an
indeterminate read. Immediately before the sole write, while serialized by the
runtime lock, it revalidates the session identity, logoff epoch, exact selected
GPU event generation, and removal/recovery inbox. Explicit Apply or Reset
supersedes older lifecycle intent only when the selected device is not already
in a confirmed recovery transition.

The exact selected-device CM callback is allocation/file/hardware-free, but it
publishes a monotonic generation and removed flag immediately. Logon, standby,
explicit Apply, Reset, and controlled recovery all sample/recheck that state at
their final write boundary. A coincident removal therefore gives driver
recovery precedence even before the lifecycle worker drains its inbox.

Fast Startup and autologon are not inferred from service startup. They are
handled by the real scheduled-task handoff, with WTS notification retained as a
coalesced fast path. Old boot-reconcile markers are obsolete.

## Standby and driver recovery are different events

`PBT_APMSUSPEND` arms a suspend generation. Automatic, interactive, and legacy
critical resume notifications are accepted; their exact power-event value is
logged by the lifecycle worker. Duplicate resume notifications are coalesced,
and the first matching resume restores the complete in-memory
desired state exactly once: curve, GPU/memory offsets, power limit, clock lock,
and fan mode/curve. Standby does not require the 10-minute proof because it is
not evidence that the active settings caused a driver failure. Immediately
before the write, the worker snapshots a valid proof only if it is already at
least 10 awake minutes old. The normal pre-write boundary still durably deletes
the proof. Complete restore success republishes the exact mature stamp; failure
cannot resurrect it. An immature, missing, corrupt, or cross-boot proof receives
a fresh stamp only after successful standby restore.

A confirmed driver recovery is stricter. Its proof age is measured with
`QueryUnbiasedInterruptTime`, so suspend/hibernate time does not advance the
clock. The proof stamp, controlled-restart authorization, and recovery ledger
are bound to the full 128-bit `SystemBootEnvironmentInformation.BootIdentifier`,
which remains stable across wall-clock correction and a fresh service process.
The former `SystemTimeOfDayInformation.BootTime` value was wall-clock-derived
and could move within one real boot, so it is forbidden as restore authority.
Old, cross-boot, corrupt, or ambiguous formats fail closed. Explicit/client,
logon, and driver-recovery applies start a fresh 10-minute period. Standby is
the sole exception and preserves only a proof that was mature before its write.
Empty obsolete restart history is removable; non-empty legacy tick history
cannot be bound to a stable boot identity and requires explicit Apply
acknowledgement.

`DBT_DEVNODES_CHANGED` commonly arrives with null event data. It is only a
read-only re-enumeration cue and never authorizes a write by itself. A driver
recovery needs independently corroborated evidence for the selected GPU, such
as disappearance/rearrival, a validated device-generation change, or the VEH
driver-crash path. A coincident confirmed driver recovery dominates standby.

Recovery evidence is deduplicated in one protected persisted ledger. Automatic
success does not erase its history. Only a successful explicit Apply clears the
ledger and sticky lockout.

## Controlled process restart

Green Curve may need a fresh service process after a confirmed driver fault.
This uses a nonce-bound controlled-restart protocol, not a reusable marker:

1. Before committing to exit, the service launches the minimal
   `--recovery-restart-helper` with a protected snapshot, random nonce, and the
   previous process identity.
2. The old service reports `SERVICE_STOP_PENDING` and exits with the dedicated
   controlled-recovery exit code. It never self-publishes `SERVICE_STOPPED`:
   SCM owns that final transition after the old dispatcher is fully detached.
3. Only that exact exit lets the helper subscribe to SCM status changes. It
   keeps the old process object pinned against PID reuse and waits for SCM's
   authoritative `SERVICE_STOPPED` within the nonce's remaining freshness
   window. The wait is
   alertable synchronization driven by `NotifyServiceStatusChangeW`, with no
   polling or timing sleep. Because `QueryServiceStatusEx` does not guarantee a
   valid process ID in `SERVICE_STOP_PENDING`, that one transitional state is
   waitable after the pinned parent's dedicated exit even with a zero/stale
   reported PID. Every other non-stopped state must still belong to the exact
   old PID. The helper then revalidates the protected authorization and makes
   exactly one `StartServiceW` call with `--controlled-recovery <nonce>`.
   Notification failure, deletion, timeout, or a different SCM process
   generation clears the recovery files and performs no start or hardware
   write.
4. Before reporting `RUNNING`, the new service synchronously validates the
   nonce, snapshot, previous process identity, freshness, and SCM start reason.

Helper launch failure, the wrong exit code, a missing/wrong nonce, stale or
corrupt data, Task Manager termination, a crash, and an ordinary SCM
failure-action restart all fail closed without a hardware write. Obsolete
boot/ticket files and unsafe old snapshots are deleted during migration, while
the existing sticky lockout is preserved. The pre-handshake helper remains
independent of normal `service_main` user-path startup (WTS/profile/Known
Folder/config and global path caches). The parent records the helper process
exit code if it exits before signaling readiness; this preserves actionable
failure-stage diagnostics without expanding the minimal authorization helper's
startup surface.

The sticky automatic-restore lockout is written to both the protected state file
and a protected HKLM fallback. Latching succeeds durably if either store
succeeds; startup treats an unreadable fallback as locked out. An explicit
successful Apply clears both stores and acknowledges recovery history only when
the current protected ledger path and the legacy migration path resolve and are
confirmed absent/deleted. Any required clear/path failure leaves automatic
restoration locked out.

## Scheduled-task compatibility

The canonical task is immediate, least privilege, scoped to the correct user,
uses the current executable/config and `--logon-start`, has no repeat-on-failure,
and has a three-minute execution limit. The task waits for the service to reach
`RUNNING` with SCM status notifications for at most 120 seconds, then sends the
handoff. A failed handoff exits nonzero and logs an actionable reason.

Definitions are classified rather than treated as an exact-text match:

- **Canonical:** current immediate, least-privilege definition.
- **Compatible legacy:** correct user/action/config with a valid delay,
  `HighestAvailable`, the old `PT0S` limit, or an omitted safe schema default;
  it remains functional and is normalized best-effort.
- **Broken:** disabled, wrong user, wrong executable/config/action, additional
  triggers/actions, repetition/restart policy, battery/idle/network gating,
  `PT1S` or another unsupported execution limit, or a multiple-instance policy
  that can suppress/delay the handoff; repair is required.

Failure to normalize a compatible task does not disable it. Failure to repair a
broken task does not roll back the saved logon-profile choice; the GUI warns
that logon-event redundancy is degraded.

## Invariants

1. Service startup never REPLAYS settings unless a new controlled-recovery
   invocation is synchronously validated. Its only other write is the
   once-per-crash ownership handback to stock (see above).
2. A snapshot alone never authorizes a write. This makes Task Manager an
   effective emergency stop, which now ends at stock rather than at a
   half-owned GPU.
3. Sticky lockout dominates every automatic origin until a successful explicit
   Apply.
4. Standby restore is immediate and complete but uses in-memory intent only; an
   ordinary process restart cannot turn a standby snapshot into authorization.
   A successful standby write preserves prior proof age only when that proof was
   already mature and valid in the current boot; otherwise it starts fresh.
5. Driver recovery needs 10 minutes of awake stability and valid current-boot
   proof.
6. Prerequisite waiting may repeat; a real hardware write may not.
7. Expected live VF drift never triggers correction or changes profile
   ownership.
8. Reset may supersede pending lifecycle work, but it does not acknowledge or
   clear sticky recovery history; only a successful explicit Apply can re-arm
   automatic restoration.

## Diagnostics and state

Protected service state includes the current-format proof stamp, sticky lockout,
deduplicated recovery ledger, active desired snapshot, and short-lived
nonce-bound controlled-restart material. Exact filenames and formats are
internal and versioned; reject unknown formats fail-closed.

Useful log terms include `logon handoff`, `lifecycle worker`, `authentication
LUID`, `standby generation`, `unbiased proof`, `controlled recovery`, and
`auto-restore lockout`. Never commit these files or customer logs because they
may contain machine, account, GPU, and settings context.

## Open questions / stale-risk

Windows power, WTS, and PnP notification ordering varies by driver and Windows
version. Keep the reducer deterministic and notifications coalesced, and verify
the event trace on real Fast Startup, autologon, standby, multi-user, and TDR
systems whenever lifecycle code changes.

Failure to enumerate active WTS sessions is transient and fail-closed; the
service does not substitute the active console session because that could bind
an automatic write to the wrong authenticated login.
# Config-watch readiness invariant

Logon profile materialization watches the parent directory so atomic config
replacement is observable, but directory notifications are only wakeup hints.
When only an ancestor exists, a relevant creation moves the nonrecursive watch
inward toward the target before retrying. A failed rearm closes and immediately
re-establishes the watch instead of leaving readiness unobservable.
The lifecycle worker compares the exact config file's presence, volume/file
identity, size, and last-write time before treating a notification as readiness.
Sibling activity (including Green Curve's own debug log in the same directory)
must only rearm the watch: it cannot retry profile resolution or authorize a
hardware write. This prevents a self-sustaining CPU/I/O loop while retaining
event-driven retries for actual config creation or replacement.
