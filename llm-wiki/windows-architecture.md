# Windows Architecture

## Transport and shared-bank follow-up (verified 2026-10-01)

Source anchors: `source/service_pipe_transport_lease.h`,
`source/main_service_pipe.cpp`, `source/main_service_machine_config.cpp`,
`source/service_acl_handle.cpp`, `source/service_ipc_throttle_policy.h`.
The six pipe workers take a nonblocking account transport lease BEFORE their
first read. At most two connections from one primary-process SID may occupy
workers, independent of PID, session, or logon LUID. A query failure refuses
immediately; a scope-owned lease releases on every exit. Primary-process SID
lookup is only an availability hint and NEVER authorizes a command. The
connection impersonation token captured after the mandatory first read still
owns all session/PID/integrity/admin authorization. Header failures now charge
the anonymous bucket; admission-refused connections do not drain a body.

A global anonymous accept gate was rejected: one attacker draining it would
also deny honest accounts. Account leases bound silent-header and body/write
stalls without cooldowns, sleeps, or timing assumptions. Same-account traffic
shares its two slots. Native silent-pipe fixtures exercise pre-read SID
capture and six hostile lease attempts; pure fixtures prove another account
still acquires capacity. This is not a claim of absolute fairness; residual
availability limits are tracked in the maintainer's private notes.

Open question (2026-10-01 review): a lease refusal disconnects before the
header, so the client cannot be told "busy". If the client's write lands first
it records `requestSubmitted` and a mutation reports "outcome unknown" until
operation recovery resolves it. One GUI holds at most two concurrent
connections (coordinator + Updates worker; the logon-sync thread only at
startup), so this needs a third same-account client, e.g. a polling
`greencurve --json` script during an Apply. Not changed without evidence.

Shared-bank startup pins the directory and singly linked regular non-reparse file and
checks the PREVIOUS exact config DACL plus Administrators ownership BEFORE
retaining content. Unproven bytes are truncated/flushed through that handle
before reclaiming ownership/DACL. A failed proof makes CONTENT reads fail
closed until the next content read re-proves it (2026-10-01 review; it used to
disable path resolution, including the updater's own folder, for the whole
service lifetime) -- see config-profiles.md. Merely applying an ACL is
not content authentication. The config ACL differs from the binary ACL by
Users read-only vs read/execute; both are explicitly represented.
Diagnostic DACL detectors now flag named principals, delete-child, null DACLs,
and unsupported grants; authorization still uses exact handle-bound proofs.
Windows temp-bank and DACL regression fixtures execute this behavior. No
live installed SYSTEM service was modified for this verification.

## Two-process model

Green Curve on Windows uses a split architecture:

1. **`greencurve.exe`** (GUI process) — runs unelevated, owns the Win32 GDI UI, tray icon, and config editing
2. **`greencurve-service.exe`** (service process) — runs elevated as `LocalSystem`, owns all live GPU control via NVAPI and NVML

Both are unity-built from the **same source tree**. The service binary is
compiled with `-DGREEN_CURVE_SERVICE_BINARY=1`, which activates service paths in
the amalgamated build instead of GUI paths. The former service monoliths are now
thin aggregators: `main_service_server.cpp`, `main_service_runtime.cpp`, and
`main_service_ipc.cpp` include focused host/pipe/policy, identity/fan/apply, and
connection/client/admin/machine-config shards. Lifecycle event capture,
serialized application, and the long-lived worker are separate shards included
from `main_shell.cpp`.

## IPC: Named pipe

Communication uses a local named pipe with a binary struct protocol.

### Runtime reads and state publication

The visible GUI enqueues temperature/fan telemetry at its existing cadence, but
the window thread never opens a pipe or waits for SCM. The sole GUI service-I/O
coordinator owns runtime reads, mutations, service verification, and admin
operations. Telemetry is a cached service read rather than an adapter-readiness
probe; the service fan worker owns hardware polling. A full sync performs the
serialized authoritative refresh needed to enter `READY`.

The listener intentionally keeps the proven broad local ACL: SYSTEM and
Administrators have full access, while Authenticated Users may connect. The
transport is a fixed pool of six worker threads (one pipe instance each;
`main_service_pipe_listener.cpp`) with a two-per-account pre-read lease so one account cannot monopolize all
workers. Each connection is served end to end as: account lease -> 12-byte header probe
(`magic+version+command`, pinned offsets; completes with `ERROR_MORE_DATA`,
which IS the mandatory first read) -> brief impersonation -> stable throttle
key (SID string + authentication LUID + session) -> pure admission decision ->
exact body read into the wire struct -> SERIALIZED dispatch
under `g_serviceDispatchLock` (the one-hardware-mutation-at-a-time invariant)
-> response write OUTSIDE the lock, so a non-reading client cannot stall the
next hardware command.

Admission accounting (`service_ipc_throttle_policy.h`) carries two invariants
that once silently broke and that `check_ipc_transport_and_probe_gates()`
now pins:

- **ONE shared table.** Decisions and charges go through the single
  `gc_pipe_dispatch::service_admission_table()` accessor. The first
  transition-safe build declared two function-local statics — decide read a
  table no charge ever touched — so rate limiting never refused anything,
  invisibly: the pure tests shared one table, and live traffic never got
  fast enough to hit a limit.
- **Exactly one charge per connection, by outcome.** The pure
  `service_ipc_connection_cost_tokens()` maps how an exchange ended to its
  cost (exchanged=1; mismatch/unknown=5; identity-unknown=10 into the
  metering-only anonymous bucket; admission-refused=0, because refusing must
  not refund tokens to a flooder; any transport fault=10). A failed response
  write supersedes the branch outcome. Pure tests cover the mapping and the
  zero-cost-refusal-is-a-no-op property.

The duplicated client impersonation token captured after the probe is owned
by `ServiceClientIdentity`'s destructor. The worker-pool refactor lost the
historical post-dispatch close and leaked one SYSTEM-held token handle per
served connection — including refused/mismatched ones, since capture runs
before every magic/admission check.

Bucket shape: per-identity token buckets, an independent small logon-handoff
reserve, bounded identity table with idle eviction, no permanent bans.
Wrong magic/version and refused connections drain the
inbound message via a PeekNamedPipe-gated bounded drainer and still receive
their payload-free refusal (old-version clients no longer hit a silent 2s
timeout). Attempting identity capture before the header probe fails with
`ERROR_CANNOT_IMPERSONATE` (1368) on every request and must not be
reintroduced; the prefix-read source gate pins the ordering. Watchdog/reaper
semantics generalize the old single-handle CAS slot to per-worker slots.
Retired entirely: the wake/recycle listener events and any per-session SDDL
rebuilding.
Every new request is authenticated. After the first connection verifies the
server against SCM and its registered executable, later connections may reuse
that result only for the same PID *and process creation time*. A service restart
therefore forces full verification. Coordinator results are immutable and are
posted to the main window; only the window thread may reduce them into GUI
state, configuration/profile presentation, tray state, or HWND rendering.

Every fully authorized read or mutation response carries the same complete
state payload. Protocol/malformed/startup-gate and active-session/PID/integrity
failures remain metadata-only, so another local authenticated session cannot
read the active user's GPU identity, intent, profile, or telemetry. Accepting
an authorized envelope also synchronizes the persisted `applied_slot` indicator
against authoritative profile source/slot and active intent. That comparison is
cached against the exact config file size/last-write stamp, so unchanged
telemetry performs no INI reads while a real save/external edit forces a fresh
comparison.

### Protocol constants

- Magic: `0x47535643u` (`"GSVC"`)
- Version: `28` (v20 added Blackwell XBAR clock/MSVDD, v21 SYS clock,
  v22 VIDEO clock, v23 native zero-RPM curve intent, v24 assigned a reserved
  fan-curve byte to its independent fan-off gap, and v25 carries real active
  measured XBAR voltage `xbarMeasuredVoltageUv` in `ServiceSnapshot`. Wire
  sizes did not move because the 4-byte scalar occupied existing alignment tail
  padding before `health`, but protocol version was bumped to reject mixed peers.
  v18 had added `outcomeSeverity`, so a client can tell a clean success
  from one completed with reservations without parsing `message`;
  `ServiceResponse` changed size, hence the bump. v14's explicit validity for
  scalar GPU/memory/power/fan readback, the complete atomic
  `ServiceStateEnvelope`, nonzero service instance/revision/GPU-generation
  identity, phase/topology/active-intent validity, mutation preconditions,
  operation deduplication/result query, and fixed-width `gc_bool8` validation
  all remain. Current exact sizes (protocol 28; unchanged since 27) are `ServiceRequest=1552`,
  `ControlState=188`, `DesiredSettings=964`, `ServiceSnapshot=4248`, and
  `ServiceResponse=7368`; v26/v27 added `DesiredSettings.curvePointFromGpuOffset`,
  the PER-POINT provenance the apply needs to tell a typed absolute MHz from one
  projected over a stock base (see clock-transition-audit.md).
  v15-v17 are Linux-side additions; see
  linux-scaffold.md)
- Pipe name: `\\.\pipe\GreenCurveService` (fixed, no session ID suffix — the session ID was removed because the service starts at boot before user login, creating a stale session-0 pipe that didn't match the post-login GUI's session-ID pipe)

### Request struct (`ServiceRequest`)

Defined in `source/service_protocol.h`:

| Field | Type | Purpose |
|-------|------|---------|
| `magic` | DWORD | Protocol magic |
| `version` | DWORD | Protocol version |
| `command` | DWORD | `ServiceCommand` enum |
| `flags` | DWORD | Reserved |
| `callerPid` | DWORD | Calling process PID |
| `callerSessionId` | DWORD | Calling session ID |
| `operationId` | uint64_t | Nonzero client-generated correlation and deduplication key for APPLY/RESET; reused when querying an uncertain result |
| `applyOrigin` | DWORD | Validated `ServiceApplyOrigin`; only approved explicit origins may clear lockout after success |
| `profileSource`, `profileSlot` | DWORD | Optional validated ownership metadata; never logon authorization |
| `expectedServiceInstanceId` | uint64_t | Interactive mutation precondition copied from the accepted `READY` envelope |
| `expectedGpuGeneration` | uint64_t | Rejects a queued mutation that crosses selected-device authority loss/reconnect |
| `expectedTopologySignature` | uint64_t | Rejects a mutation after a curve/topology identity change |
| `desired` | `DesiredSettings` | Settings to apply (for APPLY command) |
| `resetOcBeforeApply` | DWORD | Apply flag requesting an OC/VF/power baseline reset before applying a new target. **IMPORTANT**: `reset_oc_before_gui_apply()` in `gpu_backend_reset_baseline.cpp` resets in this order: GPU offset → power limit → VF curve offsets. GPU offset FIRST prevents a dangerous transient where VF tail points snap to high factory frequencies while the old GPU offset is still active. The **memory** offset is deliberately NOT reset here: dropping VRAM from a large offset to 0 under load causes TDRs, so the new profile's memory offset is written directly in the apply phase instead. Power is reset only when the incoming request itself owns power. |
| `targetGpu` | `GpuAdapterInfo` | Selected GPU identity for apply/reset target validation |
| `source` | char[64] | Origin label for logging |
| `path` | char[MAX_PATH] | File path for write commands |

### Response struct (`ServiceResponse`)

Defined in `source/service_protocol.h`:

| Field | Type | Purpose |
|-------|------|---------|
| `magic` | DWORD | Protocol magic echo |
| `version` | DWORD | Protocol version echo |
| `status` | DWORD | `ServiceResponseStatus` enum |
| `serviceBuildNumber` | DWORD | Service build number for GUI/service identity check |
| `serviceVersion` | char[32] | Service app version for GUI/service identity check |
| `state` | `ServiceStateEnvelope` | Nonzero instance/revision/generation, full topology signature, explicit GPU phase, authoritative validity mask, and active-desired presence |
| `snapshot` | `ServiceSnapshot` | Full GPU state snapshot |
| `desired` | `DesiredSettings` | Active desired settings, authoritative only when the envelope says they are present/valid |
| `controlState` | `ControlState` | Current control state plus explicit per-domain readback-valid bits; configured intent never substitutes for a failed hardware read |
| `operationId` | uint64_t | Operation whose result is reported |
| `operationState` | DWORD | `IN_PROGRESS`, `SUCCEEDED`, `FAILED`, or `OUTCOME_UNKNOWN` |
| `outcomeSeverity` | DWORD | v18 `ServiceOutcomeSeverity`: `SUCCESS`, `WARNING`, or `ERROR`. Derived from `status` at the single write-out point; see below |
| `message` | char[512] | Human-readable status/error message |

The snapshot also exposes authoritative active profile source/slot, last
lifecycle trigger/result, automatic-restore lockout reason, and the service's
compact per-domain GPU capability plus memory topology. The latter reuses five
formerly reserved `ServiceGpuHealth` bytes without changing protocol v16's wire
layout; the GUI decodes it in `apply_service_snapshot_to_app()` before warning
or unified-memory confirmation policy runs. These are service intent/capability
metadata; live temperature-sensitive VF MHz is not ownership. Numeric defaults
never imply validity.

### Synchronous clients stamp the state they name

Those three `expected*` fields are **mandatory** on APPLY and RESET
(`service_request_reject_reason()`); a request that carries none is refused as
malformed before authorization. Two client paths build such requests and each
needs its own source for them:

| Path | Identity source | Stamped by |
|------|-----------------|------------|
| Window/GUI | `g_app.guiServiceModel` (reconnect-safe, epoch-guarded) | `gui_mutation_stamp_request()` |
| Synchronous (every CLI verb, the installer's settings restore, `--service-remove`'s reset) | `g_syncClientStateIdentity`, adopted in `apply_ready_service_envelope_to_app()` | `service_client_apply_desired()` / `service_client_reset()` |

The synchronous path had no source at all until 2026-07-29 and sent three zeros,
so **every** CLI mutation was refused (`rejected v15 request command=4 …: apply
without service instance / GPU generation preconditions`) — two upgrades
restored nothing and an uninstall left the GPU overclocked. The rule is now pure
and shared in `source/service_client_precondition_policy.h`:

- only a coherent READY envelope may be adopted, and an unusable envelope
  **clears** the identity rather than leaving a stale one to be stamped;
- `service_client_execute_mutation_request()` refuses an unstamped mutation
  *before the send*, so the outcome stays knowable — nothing was attempted —
  instead of arriving as an unreadable protocol refusal;
- a Reset may fetch the identity on demand (it derives nothing from published
  state); an Apply may not, because it names the state its settings were
  computed against.

### A refusal reaches the client that caused it

With ordinary local users narrowed at connect time, `populate_service_state_response()`
runs only after the session/PID/integrity gates pass. A request refused before
that answers with a message and an all-zero payload. That is a complete answer,
and `validate_service_response_for_ipc()` accepts it when the payload is
byte-exactly absent across all four members; a half-populated envelope is still
damaged, and a `SERVICE_STATUS_OK` response must always carry one. Rejecting it
(the behavior until 2026-07-29) discarded the service's reason, surfaced as
"Service response contains an invalid state envelope", and made the mutation
path report "operation … pending or unknown" for what was a definitive refusal.

`service_request_reject_reason()` is the request rule set and
`validate_service_request_for_ipc()` is exactly "no rule is broken", so the
service log names the rule that was broken and cannot drift from the decision.

### Readback validity producer (v14)

`populate_control_state_locked()` in `main_state_sync.cpp` publishes the bits;
the derivation is the pure `source/control_readback_policy.h` so it is
unit-tested on either host rather than only on Windows hardware. The reason the
bits are needed at all is that the published scalars deliberately keep a
last-known or configured-intent fallback so a degraded read still leaves the
editor populated — and 0 MHz / 0% are legal values, so the number alone cannot
distinguish "the driver answered" from "nothing could be read".

Provenance per domain:

- **GPU offset** — `current_applied_gpu_offset_mhz()` now reports it through a
  `fromHardware` out-parameter. Two of its branches (a persisted selective
  request, and the active-desired fallback) deliberately answer with remembered
  intent; those report `false`. `detect_clock_offsets()` replaces that scalar
  unconditionally, so it owns the final answer: a populated VF control table or
  a successful Pstates20 read.
- **Memory offset** — the NVML read, upgraded when the Pstates20 max minus
  the NVML max memory clock (`nvmlDeviceGetMaxClockInfo`) produces a value.
- **Power** — the NVML read, and additionally requires a nonzero default and
  current, because a percentage computed from a missing default is arithmetic
  rather than a reading.
- **Fan policy / target** — per-fan flags recorded in `nvml_read_fans()`. Every
  fan present must have answered; a partial answer is unknown, not a selective
  match. Windows only reads the target duty while a fan is manual, which is why
  the shared comparator reports a policy takeover as an override rather than
  demanding a duty readback that the takeover itself suppressed.

A rollback zeroes the cached scalars before re-reading them; those zeros are
bookkeeping, so `invalidate_scalar_readbacks()` drops the validity with them and
a refresh that then fails leaves the domains unavailable instead of publishing
an invented "reset to stock" match. `apply_control_state_to_gui()` carries each
bit with the value it describes, so a stale `true` cannot survive into a domain
the update did not refresh.

The service owns one immutable published state protected by an SRW lock.
Hardware/recovery paths prepare updates locally and publish phase, payload,
validity, generation, and revision together. `stateRevision` increases for
every publication. `gpuGeneration` increases whenever selected-GPU authority is
lost or identity changes: exact removal, an arrival when the removal was
missed, failure of a formerly `READY` full refresh, or response-time discovery
that adapter identity is gone. `READY` requires identity, complete topology,
applied controls, and active intent after reacquisition/reapply/full refresh;
telemetry may be independently invalid without making those numeric slots
authoritative.

### Outcome severity producer (v18)

`status` is binary; a hardware write is not. An apply can verify every value it
was asked for, or it can commit while the driver quietly declined some of it —
a VF point that refuses its selective offset, a flatten target the tail will
not hold. The backend deliberately **accepts the live value** for such points
rather than failing the whole profile over one stubborn point, so both cases
answer `SERVICE_STATUS_OK` and differ only inside `message`
(`"3 of 8 boost points matched"`). That is prose, and no client may depend on
it, which is why the distinction is now a wire field.

- **Producer.** `apply_desired_settings_service()` computes it with the pure
  `service_apply_outcome_severity_for_lock_mode(hardPin, failCount,
  boostUnmatched, flattenUnmatched)` (`service_apply_severity_policy.h`),
  which delegates to `service_apply_outcome_severity()` and zeroes the tail
  count for hard pins: NVML min=max locked clocks are their own verification,
  so VF tail readback differences stay diagnostics instead of downgrading a
  successful pin to `WARNING`. The severity returns through an out-parameter;
  `service_apply_desired_settings()` passes it to the APPLY handler. Every
  early return in either function starts at `ERROR`, so a caller cannot read a
  refusal as a clean apply. RESET has no warning class — `service_reset_all()`
  either restores stock or fails.
- **One stamp, not per-handler assignment.** The value is resolved by
  `service_response_resolve_outcome_severity()` immediately before
  `service_pipe_write_exact()` — the single point this process writes a
  response out. Every branch that only sets `status` and breaks is therefore
  covered, and a handler that recorded a warning cannot have it survive a
  status that says the operation failed. The rule is total: non-OK ⇒ `ERROR`;
  OK ⇒ `SUCCESS` or `WARNING`. It is also idempotent, which is what lets
  `ServiceOperationRequestGuard` resolve it earlier without conflict.
- **Validated as a pair.** `validate_service_response_for_ipc()` rejects a
  response whose severity disagrees with its status, *before* the payload-free
  refusal shortcut, so the rule covers stateless refusals too. A disagreement is
  not a lesser answer to trust selectively — it means the two fields describe
  different operations.
- **Retries replay it.** `ServiceOperationRecord` and the persisted
  `service_operation.bin` (record **v2**) carry the severity, because a
  deduplicated retry after a lost response is answered from that record.
  Without it, the one client that already lost the original answer would be the
  one told the apply was clean. A v1 file fails its version check and is
  discarded — the already-handled "no valid persisted result" path.
- **Consumer.** Only the GUI's manual Apply/Reset presentation reads it, to
  decide between a status line and a modal box; see windows-ui-layout.md
  (F-RESULT).

### Commands

| Command | ID | Purpose |
|---------|-----|---------|
| `SERVICE_CMD_PING` | 1 | Health check |
| `SERVICE_CMD_GET_SNAPSHOT` | 2 | Get full GPU state |
| `SERVICE_CMD_GET_TELEMETRY` | 3 | Get telemetry (temps, fans, clocks) |
| `SERVICE_CMD_APPLY` | 4 | Apply desired settings to GPU |
| `SERVICE_CMD_RESET` | 5 | Reset GPU to defaults |
| `SERVICE_CMD_GET_ACTIVE_DESIRED` | 6 | Compatibility read; its response is still a complete state envelope (the GUI no longer chains it after a snapshot) |
| `SERVICE_CMD_WRITE_LOG_SNAPSHOT` | 7 | Write log snapshot to file |
| `SERVICE_CMD_WRITE_JSON_SNAPSHOT` | 8 | Write JSON snapshot to file |
| `SERVICE_CMD_WRITE_PROBE_REPORT` | 9 | Write probe report to file |
| `SERVICE_CMD_LOGON_HANDOFF` | 10 | Settings-free authenticated scheduled-task notification; service derives caller session/config/profile |
| `SERVICE_CMD_GET_OPERATION_RESULT` | 11 | Read-only lookup for a mutation result after timeout/disconnect |

The service retains the latest 16 mutation outcomes. Duplicate APPLY/RESET
requests with the same operation ID return the retained state and never execute
the backend write again. The latest correlation is also persisted in a
protected sidecar; an operation that was in progress at service termination is
restored as `OUTCOME_UNKNOWN`. Active desired state schema v5 remains backward
compatible with v4 records.

## Service lifecycle

- Installed service binaries are registered adjacent to the running GUI/service directory. `ensure_secure_service_binary_path()` resolves the current executable directory, stages/copies `greencurve-service.exe` there when needed, hardens and verifies the containing directory and service binary DACLs, then registers the SCM absolute path to that adjacent binary.
- **Recommended deployment:** an all-users admin-controlled directory such as `%ProgramFiles%\greencurve`. This keeps the GUI and service side by side while letting any user launch the GUI and preventing non-admins from replacing the SYSTEM service binary after install hardening.
- **User-profile launch warning:** if the GUI is launched from inside a user profile directory (e.g. `C:\Users\<admin>\...`), the GUI status label warns that other users, including restricted/standard accounts, may be unable to read/execute that GUI binary. Service installation is not blocked (portable mode is preserved), but the adjacent service directory/binary are still hardened; copying the pair into a user-private location remains the user's deployment responsibility.
- Registered with Windows SCM as a machine-wide service
- Service binary is staged via temp file + rename and both the containing directory and binary DACLs are hardened and verified before SCM registration.
- When service is not installed/stopped/unresponsive, live controls are disabled in the GUI
- GUI checks service status on startup and periodically
- Protocol version changes intentionally mark older running services as unavailable. This prevents a stale installed service binary from silently accepting newer GUI requests with older apply behavior.
- GUI ping verifies the service-reported app version and IPC protocol match the GUI binary. Build-number drift with the same version/protocol is accepted and logged as compatible, so replacing binaries and restarting the already-installed service does not force a reinstall checkbox roundtrip.
- The pipe server thread is created with **1 MB reserved stack** (`CreateThread(..., 1024 * 1024, ..., STACK_SIZE_PARAM_IS_A_RESERVATION, ...)`) — 128 KB was insufficient and caused a stack overflow inside `nvmlInit_v2()` when initializing NVML during SNAPSHOT processing.
- Transport I/O runs on a bounded worker pool, but **command execution is strictly serialized** under `g_serviceDispatchLock` (`main_service_pipe.cpp`), so every request's turnaround includes whatever command was already dispatching. Read handlers additionally take `g_serviceRuntimeLock` via `try_lock_service_runtime(SERVICE_STATE_READ_RUNTIME_LOCK_WAIT_MS)` and serve cached `g_serviceControlState` when that wait expires, so a serialized lifecycle write cannot starve them into "service needs repair". The lock helpers live in `main_service_runtime_identity.cpp`.
- **F-PIPE-DEADLINE — the client deadline is derived from the service's own bound, never picked.** `source/service_request_deadline_policy.h` is the single place both halves read: the runtime-lock wait (250 ms), the telemetry refresh budget (750 ms), the snapshot refresh budget (2500 ms), the dispatch-serialization budget (the slowest read handler, 2750 ms), and the response framing budget (250 ms). Derived: telemetry response deadline 4000 ms, snapshot 5750 ms; `static_assert`s keep each deadline strictly above the corresponding server budget and above the 500/2000 ms literals it replaced. A deadline expiry therefore means the service really did miss its own contract.
- **The mutation lane joined the contract on 2026-09-12** (found while auditing the Linux transport). `SERVICE_APPLY_HANDLER_BUDGET_MS` (20000 ms) moved into the same header from `main.cpp`, because a second deadline depends on it: `service_operation_recovery_response_timeout_ms()`. `SERVICE_CMD_GET_OPERATION_RESULT` — the query that recovers a mutation whose response was lost — carried a hard-coded 5000 ms while the apply it recovers is allowed 20000. It is trivial to answer but shares the serialized dispatch lock with the mutation it asks about, so a still-running apply made it expire and the client reported "Operation … outcome is unknown … Do not retry it with a new operation ID" for a write that had **succeeded** — an answer that is both wrong and un-actionable. It is now derived as mutation budget + dispatch serialization + framing, under `static_assert`. `service_phase_remaining_ms()` is `constexpr` and shared with `linux_daemon_deadline_policy.h`.
- **Availability and turnaround are separate budgets.** `service_send_request_deadlines()` takes `{connectMs, responseMs, totalMs}`. The asynchronous coordinator uses `service_send_state_read_request()` (connect 2000 ms, response per the policy, no total cap), so time spent finding the service is not charged against the service's answer. Synchronous callers keep `service_send_request(..., timeoutMs, ...)`, which sets all three to one number — a total stall cap for a thread the user is waiting on. The pure rule is `service_phase_remaining_ms()`, asserted in the regression suite.
- **F-READ-MISS — a deadline expiry is not evidence the service is gone (2026-09-12).** `ServiceClientReachability` in the same header, recorded by `service_send_request_deadlines()` into a `ServiceClientSendOutcome` and consumed by `source/gui_service_stale_read.cpp`. An opened pipe instance means a service exists and accepted the connection; so does `ERROR_PIPE_BUSY` / `ERROR_SEM_TIMEOUT`, which mean the pipe object exists and every instance is in use — the exact Windows analogue of Linux's `EAGAIN` on a saturated backlog. Only `ERROR_FILE_NOT_FOUND` / `ERROR_PATH_NOT_FOUND` / `ERROR_ACCESS_DENIED` are definite answers meaning offline. `service_client_read_miss_preserves_presentation()` is the one rule; `gui_service_handle_read_miss()` is the one entry point the read completion may call. A stale read advances neither epoch, invalidates no live authority, does not disconnect the model, keeps the applied-profile identity, rebuilds nothing, and shows "Background service is busy; showing the last live GPU state." on the status line; `gui_service_note_read_answered()` clears it where the fresh values land. Only an unreachable pipe reaches `gui_service_handle_transport_failure()`. The Win32 error numbers are spelled as values in the policy header (so the contract compiles and is unit-tested on the Linux host) and pinned to `<winerror.h>` by a `static_assert` in `main_service_connection.cpp`. This exists because on 2026-09-12 a **19.078 second** telemetry handler (a C: volume stall, Windows Volsnap event 25) made a healthy service present as a lost connection — and because no deadline can be sized against an arbitrary OS stall, so the reaction has to be right independently of the budgets.
- **F-LOG-ASYNC — producing a diagnostic line must never touch the disk.** `debug_log()` used to perform the whole file write inline, on the caller's thread, and in the service that write is durable by construction (`FILE_FLAG_WRITE_THROUGH` plus `FlushFileBuffers()` after every single line). It is called from inside `service_handle_telemetry_request()`, which holds both the runtime lock and the serialized dispatch lock, so a stalled volume could stop the service answering anything at all — which is what the 19 s incident above actually was (`lockWaitMs=0` proves the runtime lock was uncontended; log lines from two different service threads, one of them the GPU-free main loop, were appended ~19 s after their timestamps). `source/debug_log_queue_policy.h` (pure) and `source/main_debug_log_writer.cpp` (the ring plus one writer thread) make producing a line a bounded memcpy into a 512 KiB byte ring and a `SetEvent`. Two locks, one-way order: the ring lock is held for a memcpy and never across I/O; the file lock is held across I/O and a producer never takes it. Overflow **drops** and counts (a waiting producer would reintroduce the coupling), and the writer emits a marker naming the count. `head` is published with an interlocked store only after the payload lands, which is what lets `debug_log_drain_pending_for_crash()` walk the ring with no lock from the VEH/unhandled/fast-fail paths, so queued lines still reach the crash breadcrumb. The flush is now per drain batch, not per line.
- **A failed request is not identity evidence.** `service_client_tracked_instance_after_request()` leaves the tracked service instance untouched when a request goes unanswered; only a successful envelope changes it. A genuine service restart is still caught by the next successful envelope naming a different instance.
- Service uninstall (`cleanup_secure_service_binary_after_remove()`) does **not** delete the protected service binary from disk. It unregisters SCM (`DeleteService`) and reverts the protected binary and adjacent directory DACLs to inheritance so the payload can be managed manually afterward.
- **Uninstall GPU cleanup:** Before stopping the service during uninstall (`service_install_or_remove(false, ...)`, `source/main_service_install.cpp`), the GUI sends `SERVICE_CMD_RESET` via IPC to revert GPU offsets, power limit, fan, and locked clocks to driver defaults. This prevents an overclock from persisting after the service is removed. The reset is best-effort — skipped if the service is not running/responsive.
- After reboot, the service auto-starts from the SCM binary path before the user logs in. The pipe keeps the transition-safe SYSTEM/Administrators/Authenticated-Users ACL; active-session authorization happens at dispatch using the captured connection token.
- **Startup is not an apply event:** installing, repairing, starting, or ordinarily restarting the service is strictly non-mutating. A snapshot is intent, not authorization; an unexpected termination, Task Manager kill, power loss, stale snapshot, or SCM failure-action restart cannot replay it. Fast Startup and autologon use the authenticated scheduled-task handoff, not a boot/start inference. The obsolete boot-reconcile markers are removed.
- **Controlled driver-recovery restart:** only the nonce-bound `--recovery-restart-helper` protocol may carry recovery intent across a service-process boundary. The old process publishes `SERVICE_STOP_PENDING`, exits with the dedicated code, and leaves final `SERVICE_STOPPED` publication to SCM after dispatcher teardown. The helper then starts the service with `--controlled-recovery <nonce>`. Its SCM stop wait accepts `SERVICE_STOP_PENDING` without trusting that state's API-documented unreliable process ID, while all other non-stopped states must still identify the pinned old generation. Before `RUNNING`, the new process validates nonce, protected snapshot, previous process identity, freshness, and SCM start reason. Missing, stale, corrupt, or mismatched state fails closed.
- **Readiness ordering:** the service registers a DXGI adapter-set event before its long-lived lifecycle worker reports ready, and reports `RUNNING` only after that worker, selected-adapter PnP registration attempt, and pipe listener are ready. Configuration Manager `STARTED` can precede NVAPI availability; the same worker therefore waits indefinitely for a later DXGI user-mode adapter change and retries still-pending intent without polling, sleeps, timers, or a per-event thread. Failure to create the worker or required listener/notification infrastructure fails startup closed; unavailable DXGI registration is logged while PnP/client readiness remains active. Control handlers coalesce state and signal the worker. Unexpected later worker death latches automatic restore off and stops the process; an SCM availability restart has no nonce and remains non-mutating.

## Logon behavior

- Per-user handoff tasks registered with `Green Curve Startup - ` prefix.
  Every `--logon-start` process sends the settings-free authenticated handoff
  and exits. The service resolves the authenticated caller session and is the
  sole automatic writer; see [automatic restore policy](auto-restore-policy.md).
- **Tray residency is independent:** `[startup] start_program_on_logon` controls
  a per-user `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` value whose
  action is `--tray-start --config ...`. The scheduled handoff process never
  enters GUI/single-instance handling, so Task Scheduler's `PT3M` limit cannot
  kill the resident tray process. A stale Run value also checks the config and
  exits when the preference has been disabled.
- Configs remain per-user even though GPU state is machine-global
- GUI/service INI access is serialized by the protected
  `Global\\GreenCurveConfigMutex-v2`, not a session-local mutex. Authenticated
  callers receive only mutex wait/release rights; setup/wait failure is
  fail-closed. Whole-file profile rewrites retain it through atomic rename and
  profile-API cache flush, so they cannot write back a stale logon selection.
- **Shared "all users" default**: an admin shares a profile (settings, stable
  GPU binding, and default) via the **"Share with all users"** checkbox; it is
  stored in the `%ProgramData%\Green Curve\shared-profiles.ini` bank. If a
  logged-on user has no per-user `logon_slot`, the service applies the shared
  default for that session even when the account has no config file yet. See
  [config-profiles](config-profiles.md) "Sharing profiles with all users".
- **Per-user shared-logon choice (`[profiles] logon_shared_slot`)**: any user (incl. restricted) can pick *which* admin-published shared profile auto-applies at their own logon via the unified **"Apply profile after user log in"** combo. Shared entries are tagged with `LOGON_COMBO_SHARED_FLAG`; choosing one sets `logon_shared_slot` and clears `logon_slot`. It records a *bank* slot, so every logon path resolves it to the admin's authoritative copy - the only logon apply that passes the shared-only policy for a restricted user.
- **Policy-aware logon resolution**: `service_resolve_session_config_context()`
  captures immutable identity/config paths and
  `service_load_logon_profile_from_context()` feeds the pure
  `resolve_logon_profile_source()` decision the session user's admin status and
  machine restriction policy. A restricted (policy && !admin) user gets only
  their published `logon_shared_slot` or the machine-wide shared default—never
  their per-user custom OC. GPU config is resolved after this choice: personal
  content uses the account's `[gpu]`, while bank content uses `[profileN_gpu]`.
  `service_lifecycle_revalidate_logon_context()` then
  checks the identity, selection, and target GPU again immediately before the
  sole write.
- **Active-user session router** (`main_service_sessions.cpp`): only the currently active interactive session drives the GPU. Login identity is `{WTS session id, user SID, TokenStatistics.AuthenticationId}`. WTS logon and task handoff for that tuple coalesce to one pending apply; logoff cancels it and clears debounce state, so a reused session number and the same SID with a new authentication LUID still count as a new login. A service start while a user is already logged in is not a synthetic logon. Each session change also signals pipe/lifecycle readiness; authorization remains server-side.
- **Long-lived lifecycle worker:** control handlers only coalesce generations and signal an event. Prerequisite intent remains pending until success, logoff/identity supersession, explicit Apply/Reset, lockout, or service stop, even when GPU re-enable is delayed by minutes or hours. It retries identity/config/PnP/DXGI readiness, never a hardware write. Runtime probes and final applies serialize under `g_serviceRuntimeLock`, with identity and selected GPU revalidated immediately before mutation.
- **Per-user logon task scoped to the requesting user:** `--for-user` / `set_forced_startup_user_sam()` stamp the requesting user into task name/UserId/Principal rather than the approving admin. The canonical task is immediate, `LeastPrivilege`, has a three-minute execution limit, and no scheduler repeat-on-failure. It waits for service `RUNNING` with SCM status notifications (at most 120 seconds), sends the handoff, and exits. A valid delay, `HighestAvailable`, old `PT0S`, or omitted safe schema default is compatible legacy state and remains functional while normalization is best-effort. Disabled/wrong-user/stale-action, extra-trigger/action, battery/idle/network-gated, repetition/restart, too-short/unknown execution-limit, and unsafe multiple-instance definitions are broken and require repair. Saving a logon-profile choice is independent of task synchronization, so repair failure warns about degraded redundancy without rolling back the choice.

## Security

- Service pipe SDDL remains `D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)`: SYSTEM + Administrators full, Authenticated Users read+write, plus `PIPE_REJECT_REMOTE_CLIENTS` (no network).
- **F-SEC-3 ("only the active interactive session may drive GPU OC/RESET") is enforced SERVER-SIDE**, not by the pipe ACL: `get_pipe_client_identity()` impersonates the exact connected client, opens and duplicates its thread token, immediately reverts, and derives SID, session, authentication LUID, groups/admin membership, and integrity level from that stable token. `GetNamedPipeClientProcessId` provides the pre-read availability hint and diagnostic correlation, never command authorization; a payload PID mismatch is rejected. Control and file-output requests require medium-or-higher integrity in addition to the active-session/admin policy. Impersonation is RAII-guarded and every path reverts.
- **Authorization tier follows a command's SCOPE (F-03-002).** APPLY and RESET are per-SESSION hardware intent owned by the person at the console, so they require medium integrity plus the active session. `SERVICE_CMD_SET_UPDATE_POLICY` is not session-scoped: it persists a MACHINE-WIDE setting (whether the LocalSystem service checks for updates, and how often), so it requires local Administrators membership on top of that.
  The mapping lives in `source/service_command_authority_policy.h` as a pure, exhaustive table with three tiers — `READ` (answers from published state), `CONTROL` (medium integrity + active session), `MACHINE_ADMIN` (`CONTROL` + local Administrators membership) — consulted by `service_command_authority_reject_reason()`. A table rather than an inline condition because the tier is a property of the command's scope: an inline `if` has nowhere to record which scope a command belongs to. An unknown command number returns `MACHINE_ADMIN`, so a protocol addition that forgets the table fails closed. Every command's tier is asserted on both hosts (assertions 5259-5269); the wiring is pinned by `security_gates.check_service_command_authority_gates()`.
  `MACHINE_ADMIN` is **group membership, not elevation** (`token_is_local_admin()`, the same predicate `service_apply_shared_only_policy()` already trusts): the update-policy checkbox lives in the ordinary non-elevated GUI, so requiring elevation would break a working feature for the administrators who should own the setting, while requiring membership still excludes standard users. `CHECK_FOR_UPDATE` and `INSTALL_UPDATE` deliberately stay at `CONTROL` — neither persists a setting, and `INSTALL_UPDATE`'s `userConsented` property depends on it being reachable from a client request.
  Rejected callers receive status/message metadata only; `populate_service_state_response()` is gated on completion of those checks — and since 2026-08-29 the update-state envelope is stamped inside the same gate (`check_update_envelope_is_authorized_only()` in `tools/update_gates.py`), so an unauthorized caller cannot read the machine's update posture either.
  - **Rejected active-session ACL design (2026-08-22):** deriving/recycling an exact user SID still creates transition races for scheduled logon handoffs, and moving identity resolution before the first pipe read causes `ERROR_CANNOT_IMPERSONATE`. Keep authorization after the complete read; if pre-read availability hardening returns, it must use a mechanism that does not depend on resolving the caller before reading.
- **Shared-only policy (F-15-014):** an admin can restrict non-admin users to admin-published shared profiles only. Enforced **server-side** in both the interactive APPLY policy (`main_service_request_policy.cpp`) and service-side logon resolution (`service_load_logon_profile_from_context()` via `resolve_logon_profile_source()`). File permissions on the GUI binary are not a boundary—any active-session client can reach the service—so the service loads its own protected shared-bank settings **and GPU target** and rejects arbitrary non-admin settings/targets. Legacy bank slots without a GPU section are safe only on a proven single-adapter system. See [config-profiles](config-profiles.md) "Shared-only policy".
- GUI clients validate the connected named-pipe server PID against the SCM `GreenCurveService` process before trusting responses. The client opens the server process and verifies its executable path matches the SCM-registered service binary path (`get_process_image_path()` + `_wcsicmp()` against `get_service_binary_path_from_scm()`). When the unelevated GUI cannot query the service PID's image path (access denied), it falls back to the SCM binary path via `QueryServiceConfigW` with `SERVICE_QUERY_CONFIG` access.
- The service creates its pipe with first-instance and local-client protection where supported.
- The service now fails closed if the restricted pipe ACL cannot be built or returns no descriptor. It logs the failure and defers pipe creation instead of creating a default-DACL service pipe.
- The service also fails closed during startup if the pipe server thread cannot be created; it stops any fan runtime, releases service events, marks the service stopped, and returns.
- `CancelIoEx` used for stalled pipe operations; all cancelled overlapped operations are joined via `GetOverlappedResult(pipe, &ov, &cancelled, TRUE)` before stack-allocated `OVERLAPPED` and event storage leave scope to prevent kernel use-after-free stack corruption.
- Client IPC checks `SetNamedPipeHandleState`, validates response magic/version before using response payloads, NUL-terminates response text fields defensively, and summarizes connect retries to avoid log floods while the service pipe is not yet available.
- Elevated helper process waits are bounded (not infinite)
- Snapshot/probe capture happens as LocalSystem, but destination validation and every filesystem mutation run while impersonating the authenticated caller. The safe writer opens the parent with `FILE_FLAG_OPEN_REPARSE_POINT` and `FILE_SHARE_READ | FILE_SHARE_WRITE` (omitting `FILE_SHARE_DELETE` to pin the directory against rename/delete TOCTOU races during the transaction), compares canonical UTF-16 final paths with ordinal case-insensitive semantics, creates a BCrypt-CSPRNG same-directory `CREATE_NEW` temp file without delete sharing, validates the handle/final path, atomically renames, and verifies the result. A junction swap therefore cannot turn the operation into a SYSTEM write outside the caller-authorized tree.
- **Unicode filesystem boundary:** internal paths remain UTF-8. `win32_utf8_paths.h` performs strict `MB_ERR_INVALID_CHARS` UTF-8→UTF-16 conversion and wraps filesystem, directory, canonicalization/final-path, module/system path, INI/profile, enumeration, and file-open APIs. Conversion failures log the operation and Win32 error without logging the sensitive path. Existing path-size limits remain intentional; this does not claim partial long-path support.
- **Service-binary EoP hardening (F-SEC-1):** the SCM registers the service by absolute path. On install, `ensure_secure_service_binary_path()` uses the current executable directory as the adjacent service directory, applies and verifies protected DACLs on the directory and binary (SYSTEM + Administrators Full, BUILTIN\Users Read+Execute, inheritance disabled), and **fails closed** if hardening or verification fails. This preserves side-by-side GUI/service layout without registering an unprotected user-writable SYSTEM service binary. On uninstall, `cleanup_secure_service_binary_after_remove()` uses the SCM-registered path captured before `DeleteService` and reverts the protected binary and directory DACLs to inherited ACLs so the unregistered payload can be managed normally. Helpers live in `source/service_acl.cpp` (unit-tested). **Location policy (2026-09-22):** "under Program Files" is no longer a gate — `service_path_chain_policy.h` / `service_path_chain.cpp` prove the actual property per path component (admin-owned, non-reparse, no non-admin substitution rights anywhere in the parent chain), and the administrator acknowledges anything weaker; see `installer.md` "Where may it be installed?". A filesystem without persistent ACLs (FAT/exFAT) skips hardening as a loud, acknowledged capability gap instead of failing mysteriously.
- **Shared-bank config DACL (F-SEC-6):** the shared bank (`%ProgramData%\Green Curve\shared-profiles.ini`) and its directory get a protected DACL (`SYSTEM` + `Administrators`: Full, `BUILTIN\Users`: Read / Read+Execute) so unelevated GUIs can read and display/load the shared profiles but cannot modify them, and a non-admin cannot plant/delete files in the bank directory (`apply_protected_machine_config_dacl` / `apply_protected_machine_config_dir_dacl`). Admin-only writes are also enforced by explicit `is_elevated()` checks in every writer. Publish, clear, default-slot, policy, and migration writes now fail closed if the DACL cannot be applied and verified.
- **DLL-search hardening (F-SEC-2):** `initialize_process_mitigations()` (`cfg_glue.cpp`) calls `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS)` + `SetDllDirectoryW(L"")` before any runtime `LoadLibrary`, blocking DLL planting of non-KnownDLLs (e.g. `dbghelp.dll`, `version.dll`).
- **IPC trust boundary (F-SEC-4):** `validate_desired_settings_for_ipc()` clamps every numeric field reachable from an Authenticated-User request — including `lockCi`, `gpuOffsetExcludeLowCount`, `fanMode`, and the embedded fan curve — before the service applies it. Wire booleans are fixed-width `gc_bool8`; request/response validators canonicalize them to `0/1` before privileged logic or GUI adoption.
- **GPU target/recovery identity:** APPLY/RESET requests validate the requested GPU index and PCI identity against the live adapter list before mutating service-global selection. Controlled-recovery snapshots persist both desired settings and target `GpuAdapterInfo`; old snapshots without identity are cleared, and a validated controlled restore skips rather than targeting a different adapter. Ordinary startup never consumes the snapshot.

## GUI repaint robustness

- `GuiServiceModel` is the only live presentation authority. Its pure reducer
  exposes `DISCONNECTED`, `SYNCING`, `DEVICE_MISSING`, `RECOVERING`, `DEGRADED`,
  and `READY`; status text, tray claims, action enablement, and paint behavior
  derive from that one state. Results are accepted only for the current
  connection epoch and monotonically valid instance/generation/revision. A
  service-instance or GPU-generation transition discards live authority before
  any new payload is rendered; retired and out-of-order completions are logged.
- The background coordinator owns all runtime pipe/SCM access. It coalesces
  full syncs, drops telemetry behind a full sync or write, preserves one active
  write, gives Reset precedence, and keeps only the latest pending Apply within
  one GPU epoch. Interactive Apply/Reset is stamped with service instance, GPU
  generation, topology, and exact GPU identity, then the service checks those
  preconditions both before and after acquiring the runtime lock. All transport
  functions return typed immutable data and never mutate `g_app` or HWNDs.
  Stable telemetry adoption is data-only: it updates live fan/tray state but
  does not suspend redraw, rewrite edit HWNDs, or repaint the parent/control
  tree. Authority, active-intent, topology, or forced-refresh changes promote
  the response to one redraw-suppressed full render transaction. Fan combo,
  edit/button text, and enablement setters are individually change-gated.
- Periodic reconnect/backstop probes enqueue a full state request without
  entering `SYNCING`. Repeated identical disconnected results are presentation-
  inert; the GUI renders a failure only when phase, discarded live authority,
  visible service state, or displayed error changes. Interactive manual refresh
  still enters `SYNCING`; tray reopen retains a coherent cached frame while an
  asynchronous telemetry response checks for service/GPU/topology drift.
- Selected-device PnP notifications in the GUI are presentation invalidation
  cues only. Exact removal/arrival immediately enters a neutral state, retires
  pending mutations and GDI surfaces, and enqueues one full sync; the service
  remains the only hardware owner. A newer same-instance GPU generation is
  required only when the cue actually retires an accepted READY authority. A
  late `DEVICEINSTANCESTARTED` cue received after a fresh service instance was
  already accepted in RECOVERING cannot demand generation 2 from that new
  instance's valid generation 1. Shell callback handling follows the
  successfully negotiated notification version, and repeated open
  notifications are idempotent, so one click cannot restart the reconnect/GDI
  transaction multiple times. Duplicate tray-autostart processes leave an
  already-resident hidden window hidden. The resident window also stores an
  explicit tray-hidden intent: display-driver reconstruction cannot satisfy an
  unsolicited top-level show request while it is set. The GUI rejects
  `SWP_SHOWWINDOW` before visibility changes, checks the visible-state
  postcondition on every main-window message for reconstruction paths without
  that flag, and uses a reentrancy-guarded `SW_HIDE` correction. Display/device/
  PnP events also reassert hidden state. An explicit tray click/menu command or
  foreground duplicate launch clears the intent before showing the window.
- `GuiDraft` owns editor text/curve values independently of live state and is
  keyed by stable GPU identity plus the complete topology signature. Dirty
  drafts survive hide/show, transport failure, service restart, and same-GPU
  recovery. They reattach only on an exact identity/topology match; otherwise
  they remain visibly detached and Apply is blocked until the original GPU is
  selected or the draft is discarded. Clean drafts rebase from the next
  accepted `READY` state. A sparse desired/profile overlay loaded before the
  first `READY` state is retained separately, rebased onto that coherent live
  baseline, and only then attached to the accepted GPU/topology; omitted fields
  therefore cannot turn into empty/default editor values. Graph paint consumes
  the model/draft snapshot and never scrapes HWND text. Accepted READY state is
  projected under one programmatic-edit guard, so nested native-control
  notifications cannot convert a clean authoritative projection into a false
  dirty draft. A changed authority with no active desired intent clears the
  clean active projection, rebases from live controls, and reports the no-intent
  state explicitly.
- Reconnect phases render a neutral overlay and tray presentation, disable live
  actions, and label a retained dirty editor as an unsaved preserved draft.
  One accepted state is applied as one render transaction. Full topology
  comparison includes every `visibleMap` entry; a valid `READY` change rebuilds
  dynamic controls under a visibility-neutral transaction. That transaction
  never redraw-toggles the top-level window: `WM_SETREDRAW` is implemented by
  clearing/setting `WS_VISIBLE`, which the shell reads as hide/show, so the old
  pair both resurrected a tray-hidden owner and removed an open window from the
  taskbar list for the length of the projection. Painting is suppressed
  internally (`g_guiTopLevelRedrawDepth`, honoured by `WM_PAINT`,
  `WM_ERASEBKGND`, and `invalidate_main_window()`) and one settled redraw is
  issued while initially visible; a tray-hidden owner receives deferred
  parent/child invalidation for its next explicit show.
- A manual Refresh is a re-read, not a reconnect: `gui_service_request_resync()`
  keeps the whole presentation when the model is READY with live authority and
  the read names the same GPU/service, and lets the completion paths transition
  if the answer really is "no longer READY".
- GDI resources are treated as disposable across display/device composition
  changes. The retained full-client surface is a top-down 32-bit
  `CreateDIBSection(nullptr, ...)` backed by process-owned system memory, never
  `CreateCompatibleBitmap`. Selected-GPU PnP, tray reopen, `WM_DISPLAYCHANGE`,
  `WM_DWMCOMPOSITIONCHANGED`, and `DBT_DEVNODES_CHANGED` advance a GUI GDI
  generation and destroy the old surface. `SelectObject` and final `BitBlt` are
  checked; creation/selection failure draws directly to the paint DC, and a
  failed blit retires the surface without posting a repaint loop. Normal paint
  never clears the physical window before the completed DIB is blitted, and
  coherent redraws invalidate without `RDW_ERASE`/`RDW_FRAME` exposure.
- Service/share control projections compare text, check state, enablement, and
  visibility before invoking Win32 setters. They never force `UpdateWindow`,
  and service-state refresh does not refresh unrelated sharing controls. Fan
  telemetry uses the same idempotent projection rule for combo selection,
  fixed-percent text, curve-button text, and enablement.

## Owner-drawn checkboxes (lock "Lk" column + themed checkboxes)

All checkboxes are `"BUTTON"` controls with `BS_OWNERDRAW` and are painted from `WM_DRAWITEM`. Lock-input policy is split between the pure `lock_checkbox_policy.h`, the gesture/transition shard `ui_lock_checkbox.cpp`, and command routing in `ui_main_window.cpp`. Invariants:

- **Windows stores no check state for them.** `BM_GETCHECK` always answers `BST_UNCHECKED` and `BM_SETCHECK` is a no-op, so every tick is derived on each paint and a repaint gate may only compare against the value last *painted* — see F-CHECKBOX-PAINT in `windows-ui-layout.md`. `tools/ui_gates.py` forbids the call form (`BM_GETCHECK,` / `BM_SETCHECK,`) across every shard that touches these controls.
- **Shared anti-aliased checkmark.** Draw every tick through `draw_checkbox_tick_smooth()` (`ui_theme_button.cpp`), which uses GDI+ `GDP_SMOOTH_AA` with color `RGB(0xE8,0xF2,0xFF)` and a size-scaled pen. The per-row lock "Lk" checkboxes (`draw_lock_checkbox()` in `main_shell.cpp`) share this renderer for their FLATTEN tick — do **not** hand-roll a raw GDI `Polyline`/`LineTo` checkmark, which renders jagged and a different color than the themed checkboxes. (The tri-state lock checkbox also has a HARD `●` dot drawn with GDI `Ellipse`, which has no themed-checkbox counterpart.)
- **Only `BN_CLICKED` advances.** `BS_OWNERDRAW` buttons automatically emit `BN_DBLCLK`; `BN_SETFOCUS`/`BN_KILLFOCUS` require `BS_NOTIFY`, which these controls do not use. The old ID-only branch nevertheless accepted every notification code. `decide_lock_activation()` now rejects every non-`BN_CLICKED` notification, and the handler logs every code and decision before filtering.
- **One transition per armed gesture.** The checkbox subclass records mouse/Space press time, control index, and the complete lock-model stamp. The synchronous release-time `BN_CLICKED` can consume that gesture once. A duplicate command, wrong control, or model change between press and release is rejected and logged. This closes the startup/profile/service-sync case where a newly arrived FLATTEN state could otherwise make the user's apparent first click select HARD.
- **Double-click pair is inert after its first click.** `WM_LBUTTONDBLCLK` and its paired trailing `WM_LBUTTONUP` are suppressed. The ordinary first DOWN/UP remains one click; the OS-classified double-click half cannot become an unarmed click. Subclass installation failure is logged, while command-level notification filtering remains active.
- **No timing workaround.** Input correctness uses message identity, one-shot consumption, and exact state stamps—never sleeps, debounce windows, or polling delays. `BM_CLICK`/accessibility/dialog activation without a raw gesture remains supported as one unarmed `BN_CLICKED`.

## Diagnostics

- Session markers log app version, auto-incremented build number, and IPC protocol version for both GUI and service processes.
- Service ping logs exact identity matches, version mismatches, and compatible build-number drift separately.
- Service-state refresh stores the last ping failure reason so the GUI can show version/protocol mismatch details instead of a generic not-responding message.
- The GUI main window uses the ANSI Win32 path consistently for class registration, creation, single-instance lookup, and caption text. Keep this path consistent unless the whole top-level window procedure is migrated to Unicode.
- **GUI label/button literals must be ASCII-only.** Because window text goes through `CreateWindowExA`/`SetWindowTextA`/`DrawTextA`, a non-ASCII char in a displayed string renders as mojibake (e.g. `…` U+2026 → `â€¦`). Use `...`. A source check forbids the `…` char in `entry.cpp`/`config_profiles_ui.cpp` (F-15-013). `debug_log` strings may keep Unicode — they go to a UTF-8 log file, not a window.
- GPU selector changes, target mismatches, ordinal fallback, and unknown-family best-effort VF write status are logged for later diagnosis.
- Startup logs whether a controlled-recovery invocation was synchronously
  authorized. A no-snapshot, missing/wrong nonce, ordinary SCM, or unexpected-
  termination start is explicitly non-mutating. If GPU state changes near
  startup, look for validated controlled recovery, a later authenticated task/
  WTS lifecycle event, or an explicit client Apply/Reset; startup itself is not
  an explanation.
- The controlled restart helper initializes serialized diagnostics and installs
  the unhandled crash filter before internal dispatch. It deliberately avoids
  normal per-user service path initialization before its authorization
  handshake. A helper crash writes the same protected service breadcrumb and
  actionable minidump as a normal service crash.
- **Logon decisions are logged to the user's log.** After resolving an immutable
  session config context, the service honors that account's `[debug] enabled`
  setting and logs task/WTS coalescing, authentication LUID, prerequisite
  transitions, final identity/GPU validation, and terminal result to the user's
  `%LocalAppData%\Green Curve` log. Pre-login startup logging still belongs to
  the SYSTEM profile and must never synthesize a logon apply.
- Service/helper crash breadcrumbs and bounded actionable minidumps route through the SYSTEM-owned machine service data directory; GUI crash artifacts use the user's data directory, which is the directory holding that user's `config.ini`. Matching x64 PDBs and ARM64 DWARF debug images remain private under `dist/symbols` and are not packaged.

### Crash artifacts (F-CRASH, 2026-07-31)

`source/main_crash_artifacts.cpp` (split out of `main_diagnostics.cpp`, which
kept the debug log) owns every Windows crash path. Location and rotation are
decided by the pure, platform-neutral `source/crash_artifact_policy.h`, which
the regression harness asserts on either host.

- **Fail closed on location.** `crash_artifact_data_dir()` resolves the machine
  directory for the service/helper and the user's `config.ini` directory for the
  GUI, using environment variables only — no `SHGetKnownFolderPath`, which is COM
  and not callable from an exception filter. When the cached user directory was
  never resolved it re-derives it from `%LOCALAPPDATA%`. When nothing resolves it
  writes **nothing**. The old `"."` fallback is forbidden by a source gate: the
  working directory is `%SystemRoot%\System32` for a service and an arbitrary
  folder for a shell-launched GUI, so it scattered dumps where nobody looks and
  put SYSTEM dumps outside the admin-only directory. A machine-scope process that
  cannot resolve its machine directory loses the dump rather than borrowing the
  user's — that asymmetry is deliberate.
- **One minidump writer** (`write_crash_minidump`) for all four paths (unhandled
  exception, GPU-driver-DLL crash, VEH-recovered, fast-fail). There were three
  near-identical copies, which is how the VEH copy ended up on a different
  directory rule; a source gate pins the `MiniDumpWriteDump(` count at 1.
- **Fast-fail and stack-smash crashes now produce dumps.** `__fastfail` (the CFG
  violation in `cfg_glue.cpp`) bypasses the vectored handler *and*
  `SetUnhandledExceptionFilter` by design, and nothing here registers a WER
  LocalDumps entry — so those crashes used to leave nothing on disk at all.
  `source/fatal_dump_hook.h` is the seam: a null-by-default function pointer
  owned by `cfg_glue.cpp` (which is also linked into the setup program, where a
  null hook keeps the old behaviour), invoked *before* the uncatchable
  instruction. `green_curve_report_fatal_dump()` synthesises an
  `EXCEPTION_RECORD` from a live `RtlCaptureContext`, so the dump opens in
  cdb/WinDbg normally and `.ecxr` lands on the detecting frame.
  `gc_invoke_fatal_dump_hook()` is **one-shot** — `__guard_check_icall_fptr` runs
  before every indirect call in the program, including the hook's own, so an
  unguarded call would recurse until the stack ran out.
  `__stack_chk_fail` reports through the same hook instead of relying on its
  `__debugbreak()` being caught, which stops working with a debugger attached.
- **Bounded on disk.** `rotate_crash_artifacts_for_process()` runs once per fresh
  process in the GUI *and* the service, keeping `GC_CRASH_ARTIFACT_MAX_KEEP` (10)
  of **each** prefix plus a 1 MiB cap on `greencurve_crash.txt`. Ordering uses
  the embedded `YYYYMMDD_HHMMSS_mmm` stamp, never the whole filename:
  `greencurve_crash_` sorts before `greencurve_veh_` for every possible date, so
  a whole-name sweep would delete the newest terminal crash dump and keep stale
  recovered ones forever. Only names this project formats are ever deleted.
- **The GUI no longer suppresses driver-DLL dumps.** It used to write only a
  breadcrumb for a crash inside `nvml`/`nvapi64`/`nvcuda`/`nvwgf2umx` to avoid
  filling the disk on every driver update. Rotation now bounds that
  structurally, and the suppressed dump was exactly the evidence needed to tell
  "the driver died under us" from "we passed the driver a handle we had already
  invalidated". The breadcrumb still records `gpuDriverDll=1`.
- `install_crash_handlers(bool installVectoredNvmlRecovery)` is the single entry
  point used by all three process modes (GUI, service, restart helper) — they had
  drifted on which handlers they installed.

## Source of truth

- `source/service_protocol.h` and `source/service_operation_tracker.h`: protocol
  v16 atomic health/state envelope, compact capability/topology carriage, explicit scalar readback validity, mutation preconditions, commands/origins/profile/
  operation metadata, bounded deduplication, and request/response wire structs;
  `source/gpu_core.h` owns the shared
  GPU/settings data model and fixed-width aliases used by that protocol
- `source/main_service_state_envelope.cpp` and
  `source/main_service_snapshot_request.cpp`: immutable published service state,
  service/revision/GPU-generation identity, phase transitions, topology, and
  authoritative refresh
- `source/control_readback_policy.h`: pure protocol-v14 readback provenance —
  the `HardwareReadbackValidity` block carried in `AppData`, rollback
  invalidation, post-detection GPU offset provenance, and the all-fans-answered
  rule. Deliberately outside the ratcheted Windows shards so it is testable on
  either host; the shards only record facts and call in here.
- `source/gui_mutation_worker.cpp`, `source/gui_service_state.cpp`,
  `source/gui_service_model.h`, `source/gui_draft_policy.h`, and
  `source/gui_service_io_queue_policy.h`: sole runtime I/O coordinator, epoch
  reducer, independent draft, read/write queue policy, and main-thread adoption
- `source/gui_tray_visibility.cpp`, `source/gui_selected_gpu_pnp.cpp`,
  `source/gui_window_redraw.cpp`, `source/gui_window_redraw_policy.h`,
  `source/gui_process_cleanup.cpp`, `source/ui_main.cpp`, and
  `source/ui_main_window.cpp`: durable/reentrancy-safe tray residency,
  visibility-preserving coherent redraw transactions, presentation-only PnP,
  teardown safety, system-memory backbuffer generation, and reconnect rendering
- `source/service_health_probe_policy.h` and `source/main_fan_telemetry.cpp`:
  expected-busy health classification and mutation-aware telemetry polling
- `source/win32_utf8_paths.h`: strict UTF-8 path/profile Win32 boundary
- `source/service_lifecycle_policy.h`: pure intent-transition helpers, lifecycle
  reducer, and `{session,SID,authentication LUID}` identity
- `source/service_recovery_policy.h`: pure awake-proof and recovery-evidence decisions
- `source/startup_task_definition_policy.h`: testable task-definition classification contract
- `source/main_service_request_policy.cpp`, `source/main_service_pipe.cpp`, and
  `source/main_service_host.cpp`: request authorization, pipe command handling,
  SCM control/startup/shutdown, readiness ordering, and worker health monitoring
- `source/main_service_runtime_identity.cpp`,
  `source/main_service_fan_worker.cpp`, and
  `source/main_service_apply_runtime.cpp`: runtime lock/authentication, fan
  worker, and serialized apply/reset ownership updates
- `source/main_service_sessions.cpp`: immutable session config/profile context
- `source/main_service_lifecycle_events.cpp`,
  `source/main_service_dxgi_readiness.cpp`,
  `source/main_service_lifecycle_apply.cpp`, and
  `source/main_service_logon_coordinator.cpp`: event coalescing, sole lifecycle
  write boundary, and long-lived prerequisite worker
- `source/main_service_persist.cpp`,
  `source/main_service_operation_persist.cpp`,
  `source/main_service_controlled_restart.cpp`,
  `source/main_service_recovery_clock.cpp`, and
  `source/main_service_recovery_ledger.cpp`: protected intent/nonce state,
  controlled restart, awake-time proof, and sticky recovery history
- `source/main_service_selected_gpu_pnp.cpp` and
  `source/selected_gpu_pnp_policy.h`: selected-adapter notifications and exact
  PCI identity policy
- `source/main_service_connection.cpp`,
  `source/main_service_client_commands.cpp`,
  `source/main_service_admin_client.cpp`, and
  `source/main_service_machine_config.cpp`: GUI/client IPC, SCM readiness,
  helper/install operations, and protected shared-bank configuration
- `source/entry.cpp`: CLI mode, service install/remove, logon handoff, and
  share/unshare commands
- `source/cli_console.cpp`: the CLI's console sink (F-01-001)

### CLI output has two sinks (F-01-001, 2026-09-15)

`greencurve.exe` is linked `-subsystem:windows`, so Windows attaches no console
to it. Until 2026-09-15 that meant **every CLI verb printed nothing at all**:
`greencurve.exe --help` wrote zero bytes to stdout and stderr and returned exit
code 0, and the output existed only in
`%LOCALAPPDATA%\Green Curve\greencurve_cli_log.txt` — a path neither `README.md`
nor the help text mentioned. `README.md` documents
`greencurve.exe --service-install` as the archive-install path, so the failure
mode was an administrator unable to tell an install failure from a success.

`source/cli_console.cpp` resolves one console sink per process, in this order,
because each answers a different question:

1. `AttachConsole(ATTACH_PARENT_PROCESS)` — borrow the parent's console.
2. `STD_OUTPUT_HANDLE` when `GetFileType()` reports `FILE_TYPE_DISK` or
   `FILE_TYPE_PIPE` — the caller redirected us (`> out.txt`, `| more`, or a test
   harness capturing us). Inherited and valid whether or not step 1 worked.
3. `CONOUT$` opened by name when step 1 succeeded — the interactive case.
   Opened by name rather than taken from `STD_OUTPUT_HANDLE` because a
   GUI-subsystem process's inherited std handle is not guaranteed to address
   the console just attached.

The file sink stays authoritative and is never replaced: Task Scheduler, the
logon task and setup have no console at all. `gc_cli_emit()` writes the
timestamped line to the file and the bare line to the console — the timestamp is
what makes the file useful across sessions and what makes console output
unreadable.

**Known limitation, not a bug and not fixable from inside the process:** neither
cmd.exe nor PowerShell WAITS for a GUI-subsystem child, so the prompt returns
before the output lands and the text appears after it. `start /wait
greencurve.exe --help` orders it. The complete fix is a second,
console-subsystem launcher stub, which is a packaging change and was
deliberately not taken; printing out of order beats printing nothing.

The CLI log is now opened `"a"`, not `"w"` (F-01-003) — truncating meant a later
`--help` erased the preceding `--service-install` record — and is bounded by
`gc_debug_log_rotation::kCliRotateBytes` (2 MiB).

`security_gates.run_cli_console_fixture()` runs the BUILT binary with a captured
stdout and fails if `--help` produces nothing. It is the one check here that
could not be a source guard or a pure assertion. The interactive `CONOUT$` path
cannot be driven from a test and is verified by hand.
- `source/config_profiles_machine.cpp` and `source/service_acl.cpp`: coupled
  profile/GPU bank transactions and protected file/directory DACLs

## Last Verified

- 2026-08-23: transition-safe transport shipped -- six-worker listener,
  12-byte probe-first identity capture, pure admission policy, serialized
  dispatch behind a critical section, out-of-lock response writes, Peek-gated
  drain, wake/recycle events removed. Gates rewritten for the new shape; full
  build/test/tidy/fuzz/CET pass; installed live on the reporting machine with
  clean GUI telemetry and a verified CLI APPLY. Service-side log reviewed
  2026-08-23 elevated: zero real failure-pattern hits since install (only
  false-positive \b1368\b substring matches inside runtimeLastApply values).
  Fast-user-switching and scheduled-handoff live tests still pending.
- 2026-08-22 correction: an attempted active-session pipe ACL/pre-auth check was
  reverted after live use showed `ERROR_CANNOT_IMPERSONATE` because identity was
  resolved before the required first pipe read. Restore the broad local ACL and
  post-read authorization. Process startup dynamic-code/extension-point
  mitigations remain. Windows x64 regression/check passed; live GUI/service IPC
  should be retested after installing this correction.
- 2026-08-22: Protocol **v20** adds XBAR clock/MSVDD fields to the request,
  snapshot, control state, and response while pinning every changed wire size.
  The fixed prefix rejects a previous-version peer before either side reads a
  different body length. XBAR frequency and MSVDD now carry independent
  readback-valid bits sourced from actual reads, and GUI adoption is independent
  of fan-state presence. Regression, ASan/UBSan, six-target check, fuzzing, and
  tidy gates pass. Mixed-v19/v20 processes are deliberately refused; live
  hardware was not exercised in this pass.
- 2026-07-31: Protocol **v18** adds `outcomeSeverity`, derived by
  `service_response_resolve_outcome_severity()` at each producer's single
  write-out point, validated against `status`, and carried by both persisted
  operation records (v2) so a deduplicated retry replays the same answer. The
  Windows apply backend is the only producer of `WARNING`; Linux is
  transactional and has no partial-verify class. Assertions 2290-2304 plus the
  tracker cases; `python build.py --test`, `--tidy` (38 baselined, no new) and a
  full four-target build pass at build 526. **Not exercised against a live
  service**: the bump means an installed v17 service answers `VERSION_MISMATCH`
  until it is reinstalled or restarted. Two stale claims on this page were also
  corrected: the version line still said 14, and `resetOcBeforeApply` claimed a
  memory-offset reset that the code deliberately does not perform.
- 2026-07-29: Synchronous clients now stamp the READY identity their mutation
  names (`source/service_client_precondition_policy.h`, adopted in
  `apply_ready_service_envelope_to_app()`), the transport refuses an unstamped
  APPLY/RESET before sending, and a payload-free refusal is accepted and
  surfaced instead of being read as a damaged response. Diagnosed from
  `greencurve_debug.txt` on a live machine (the same rejection appears for an
  upgrade restore on 2026-07-27 and 2026-07-29 and for an uninstall's reset).
  Assertions 2006-2020; `python build.py --test` and full `python build.py`
  pass. The end-to-end upgrade restore is not yet confirmed on hardware.
- 2026-07-15: Protocol v11 complete atomic envelopes, immutable service
  publication, instance/revision/GPU-generation fences, mutation preconditions,
  main-thread GUI reducer/draft/render transactions, sole runtime I/O
  coordinator, selected-device invalidation, and reconnect-safe disposable GDI
  surfaces were cross-checked against source and deterministic regressions.
- 2026-07-13: Protocol v10 mutation IDs/result query, bounded deduplication and
  persisted unknown outcomes; nonblocking serialized GUI applies; exact
  pipe-client token authentication; caller-impersonated output writes; and
  strict UTF-16 Win32 path boundaries were cross-checked against source and the
  expanded compiled/source regression gates.
- 2026-07-13: Runtime logs confirmed that telemetry and tray pings could
  misclassify the intentionally occupied pipe during Apply. Known-owned
  mutations now defer health probes and a validated completion re-proves
  availability; deterministic regression and all-target checks pass.
- 2026-07-12: Shared slots now carry stable GPU bindings. Machine defaults work
  for fresh accounts without per-user config, and restricted manual/logon paths
  both resolve authoritative bank settings and targets. Missing legacy bindings
  are single-GPU-only; malformed/ambiguous bindings fail closed.
- 2026-07-12: VF lock input now has an executable pure policy, press/release
  state-stamp guard, one-shot gesture consumption, complete notification
  diagnostics, subclass-install diagnostics, and explicit double-click trailing-
  release suppression. A native hidden `BS_OWNERDRAW` fixture proves the Win32
  `BN_CLICKED`/`BN_DBLCLK` contract. The earlier focus-notification explanation
  was corrected: these controls do not set `BS_NOTIFY`.
- 2026-07-11: Config serialization now spans interactive and service sessions
  with a least-privilege global mutex; full profile rewrites hold it for their
  complete RMW transaction and case-insensitive INI section replacement. The
  service/server/runtime/IPC implementation is split into focused shards; the
  source map above names physical implementation files rather than aggregators.
- 2026-07-10: Replaced boot inference with settings-free authenticated task
  handoff plus WTS coalescing; added typed apply origins/profile ownership,
  authentication-LUID session identity, complete one-shot standby restore,
  awake-time driver proof, sticky recovery ledger/lockout, compatible legacy
  task classification, and fail-closed controlled recovery. Ordinary startup
  and Task Manager termination remain non-restoring.
- 2026-07-03 (0.17.1, build 348, superseded 2026-07-10): Added the
  former boot-reconcile fallback. It was removed because service-start inference
  could not distinguish logon from an intentional emergency restart; Fast
  Startup/autologon now use the authenticated task event.
- 2026-06-29: Updated startup invariant after fresh 0.17 logs: no-snapshot service starts are non-mutating and do not synthesize active-session logon applies. Service install now uses the adjacent GUI/service directory with protected DACL verification and SCM-path pipe identity. Verified `python build.py --test` and full `python build.py` (build 330). *(Superseded 2026-07-03: no-snapshot **boot** starts now reconcile an already-active session; **manual** starts remain non-mutating.)*
- 2026-06-28: Startup coordinator race fix documented. Superseded on 2026-06-29 for no-snapshot behavior: the coordinator still serializes recovery snapshots, but no-snapshot service starts now stay idle instead of reset/reconcile. Verified `python build.py --test` and full `python build.py` (build 321).
- 2026-06-28: Owner-drawn checkbox section added — lock "Lk" tick now shares `draw_checkbox_tick_smooth()` (AA, `RGB(0xE8,0xF2,0xFF)`) instead of a raw GDI `Polyline`; the ID-only lock command branch was restricted to `BN_CLICKED`. Build 318. The contemporaneous focus-notification root-cause claim was corrected on 2026-07-12 because focus notifications require `BS_NOTIFY`; the filtering fix itself remained valid for owner-draw `BN_DBLCLK` and other non-click codes. Verified `python build.py --test` and full `python build.py`.
- 2026-06-28: Security hardening pass: protected `%ProgramFiles%\Green Curve` service staging, fail-closed shared-bank DACL writes, protocol 8 `gc_bool8` canonicalization, GPU target validation/restart snapshot identity, SID-based session debounce, service crash artifacts in machine data dir, INT_MIN profile repair, elevated argv quoting. Verified `python build.py --test`, `python build.py --test --asan`, `python build.py --target windows --check`, `python build.py --target linux --check`, and full `python build.py`.
- 2026-06-19: Active-user session router, coherent "Share with all users" (publish + default), `%ProgramData%` shared bank + dir/file DACL + legacy migration, requesting-user logon-task scoping, read-only on-demand shared load (Build 286). Verified `python build.py --test`/`--asan`/`--target linux --check`.
- 2026-06-14: Documented machine-wide default logon profile, machine-wide profile bank, and user-profile install warning (Build 284). Verified `python build.py --test`/`--asan`/`--target linux --check`.
- 2026-06-07: Added GUI repaint-robustness section (message-pumping service/helper waits via `wait_object_pumping_ui`/`UiInputGuard`, redraw-suppressed `rebuild_edit_controls`). Verified against source and `python build.py --test`/`--asan`/`--target linux --check` (Build 271).
- 2026-05-20: Cross-checked pipe ACL fail-closed behavior, pipe-thread startup failure handling, response protocol validation, message-mode checks, and connect-retry summary logging against source and `python build.py --check`.
