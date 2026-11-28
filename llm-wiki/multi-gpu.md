# Multi-GPU: request routing, adapter summary and per-GPU persistence

The service (Windows) and the daemon (Linux) still serve ONE GPU at a time,
and only one GPU may hold active settings (the single-owner guard on both
platforms). This page records the shared contract that lets a later change
give every GPU its own runtime without another protocol change.

## Source anchors

- `source/service_gpu_route.h`: the command route table, the serving rule and
  its refusal text.
- `source/gpu_selection_policy.h`: `gpu_adapter_route_index()`, which resolves
  a route to exactly one live adapter (or none).
- `source/service_protocol_validation.h`: the wire rule.
- Linux: `daemon_route_admits()` / `daemon_served_adapter_index()` in
  `source/linux_daemon_snapshot_runtime.cpp`, called from `handle_request()`.
- Windows: `service_route_admits()` in `source/main_service_route.cpp`, ahead
  of the command switch in `source/main_service_pipe_switch.cpp`.
- Tests: `tests/service_gpu_route_tests.cpp` (14600-14629, 14637-14638 and 14641-14643, both hosts) and the
  production daemon handler in `tests/linux_daemon_commit_rollback_tests.cpp`
  (14630-14636, refresh binding 14639-14640).

## One routing field (protocol v40, 2026-11-27)

`ServiceRequest.targetGpu` is the request's only GPU field. It names the GPU
the command is about; absent, it means the GPU the service serves. There is
no second GPU or UUID field that could disagree with it, and no `#ifdef` in
how it is read.

| Route | Commands | Target on the wire |
|---|---|---|
| machine | PING, LOGON_HANDOFF, GET_OPERATION_RESULT, RESUME_RESTORE, the four updater commands | refused ("machine-wide command carries a target GPU") |
| required | APPLY, RESET | mandatory (unchanged) |
| optional | GET_SNAPSHOT, GET_TELEMETRY, GET_ACTIVE_DESIRED, the three file-write commands, CAPTURE_CURRENT | optional |
| policy | GET_STARTUP_POLICY, SET_STARTUP_POLICY, REFRESH_STARTUP_PROFILE | PROFILE names its binding, other modes and GET carry none, a refresh may name the card its binding must be (an attachment must be complete) |

Changes against v39: PING, LOGON_HANDOFF and GET_OPERATION_RESULT refuse a
target (no client sent one). An operation result is recovered by its id alone,
so a GPU switch between a lost response and its recovery query cannot hide the
outcome; the snapshot attached to that answer is still the currently served
GPU's, not the one the operation wrote. The Linux boot policy stays one
daemon-wide choice until every GPU has its own runtime: its PROFILE target is
the stored binding and may name a card the daemon is not serving, as before. A
refresh may name the card it expects; the daemon refuses it (STALE_STATE)
unless the stored binding resolves to that card, and reuses the binding
verbatim, so a refresh never re-binds.

Serving rule while one GPU is served (`service_gpu_route_disposition()`):

- no target, or a target addressing the served GPU: answered;
- GET_SNAPSHOT naming another GPU: the existing guarded switch
  (`select_request_gpu()` on Linux, `service_prepare_requested_gpu()` on
  Windows), still refused while the served GPU holds active settings;
- APPLY/RESET: unchanged; their reconnect preconditions already require the
  target to be the published selected GPU;
- any other routed command naming another GPU: `SERVICE_STATUS_STALE_STATE`,
  "The requested GPU is not the GPU the service is serving; refresh it first",
  before the handler runs. Diagnostics: `daemon route: refused ...` (Linux),
  `service route: refused ...` (Windows).

A route addresses the served GPU only if it resolves to exactly one live
adapter and that adapter is the served one (`service_gpu_route_addresses()`):
base PCI identity plus exact location when both sides carry PCI data,
otherwise the complete adapter record. Identical cards at an unknown location
never resolve. "Served" is the published selected adapter: on Linux
`selectedAdapterIndex` only while `writeIdentityResolved` (the read-only
telemetry fallback of a multi-GPU host is not served); on Windows
`selectedGpuIndex` while `selectedGpuIdentityValid`, read under `g_appLock`.
Passive capture keeps its own binding check (`hardware_capture_target_matches()`)
behind the route gate.

The Windows gate runs before a handler takes the runtime lock, so a logon or
recovery retarget can intervene. Two checks close that: a file-write command
re-runs the route gate under the runtime lock, again after hardware
initialization (which can itself re-enumerate), right before writing anything;
and for every other routed command, after the envelope is stamped,
`service_gpu_route_answer_mismatch()` turns an OK answer whose published
selected adapter is not the routed GPU into STALE_STATE and clears any capture
(`service route: answer for command=... refused`). A file command's answer,
like every response, carries the envelope stamped after its handler, so after
a retarget it can describe the newly served GPU; the file itself is bound to
the routed GPU under the lock. The Linux
daemon handles each request under one lock that also covers every retarget.

GET_SNAPSHOT keeps each platform's refusal convention for a refused switch:
Windows answers OK with the kept GPU and the refusal text (its GUI and CLI
read it that way), Linux answers ERROR (its TUI reads it that way). A cold
Linux daemon still accepts a targeted GET_SNAPSHOT to bring the backend up.

Behavior deltas: Windows telemetry, active-intent and file commands and Linux
active-intent requests used to ignore a target, Linux GET_TELEMETRY used to
switch, and CAPTURE_CURRENT for another GPU now answers STALE_STATE from the
route gate instead of ERROR from its handler. No in-tree client sends a target
on those other than the shown GPU.

## Adapter UUIDs and per-adapter summary (protocol v40, 2026-11-28)

Source anchors: `source/service_adapter_summary.h` (wire struct, UUID
parse/format, `service_adapter_summaries_build()`, the validator),
`daemon_publish_adapter_summaries()` in `source/linux_daemon_identity.cpp`,
`service_publish_adapter_summaries_locked()` in
`source/main_service_state_envelope.cpp`, `nvml_refresh_adapter_uuids()` in
`source/main_runtime_nvml_selection.h` and `source/nvml_adapter_uuid_policy.h`; tests
`tests/service_adapter_summary_tests.cpp` (14700-14729) and the production
daemon stamp in `tests/linux_daemon_commit_rollback_tests.cpp` (14730-14733).

`ServiceSnapshot.adapterSummaries[MAX_GPU_ADAPTERS]` (40 bytes each, +320 B:
snapshot 4888 B, response 20016 B, request unchanged) is index-parallel to
`adapters[]`. The full snapshot stays the selected GPU's; no card gets a
second full snapshot.

| Field | Meaning |
|---|---|
| `uuid` + `UUID` flag | NVML's physical GPU UUID in binary; never published when two adapters report the same one |
| `RUNTIME` | the service runs this card; `gpuPhase`, `healthReason`, `autoRestoreLockoutReason`, `UNCERTAIN`, `FAN_OWNED`, `FAN_FAULT` are its own and zero otherwise |
| `ACTIVE` + profile source/slot | this card holds Green Curve settings (may differ from the selected card); source NONE when the producer does not track it (Linux) |
| `FAN_HANDBACK` | a fan Green Curve left in manual control still has to be returned to the driver; marked on the card the ownership marker names (Linux; the Windows handback runs at service start) |

The envelope validator refuses unknown flags, a UUID flag that disagrees with
its bytes, duplicate UUIDs, runtime fields without `RUNTIME`, out-of-range
enums, profile identity without `ACTIVE`, anything beyond `adapterCount`, and
a selected-card `RUNTIME` entry whose phase or health disagrees with the
envelope and full snapshot. `RUNTIME` is not limited to the selected index,
so a service that runs every card fills every entry under the same rules.

Producers stamp the summary from the same publication as the envelope phase
(Windows `service_publish_gpu_phase()` also moves the selected card's summary
phase with the envelope's), and Windows matches the card holding settings by
PCI identity (`gpu_adapter_has_same_pci_identity()`, unique), because its
recorded identity may predate NVML's location enrichment:
Linux in `daemon_stamp_state_envelope()` (runtime on the published selected
adapter only while `writeIdentityResolved`; UUIDs from discovery for every
adapter), Windows in `populate_service_state_response()` under `g_appLock`.
Windows builds a UUID inventory for every adapter through an optional
`nvmlDeviceGetUUID` (`nvml_refresh_adapter_uuids()`), only after
`nvml_select_device_for_selected_gpu()` matched NVML by PCI identity (never on
an ordinal fallback). `nvml_adapter_uuid_assign()`
(`source/nvml_adapter_uuid_policy.h`) gives an adapter the UUID of the one
NVML device matching it by device ID, compatible subsystem, bus and device,
and only if that device matches no other adapter; NvAPI and NVML ordinals are
never compared. Every PCI-identified device takes part whether or not its UUID
could be read, and nothing is assigned if any device's handle or PCI identity
could not be read. Each entry keeps the adapter identity it was matched against
and is published only while that slot is still the same card; the inventory
is cleared with the adapter cache (`close_nvapi()`,
`reset_gpu_runtime_selection()`), and written and read under `g_appLock`.
A missing getter or failed query only leaves the flag clear.

## Per-GPU daemon journal on Linux (2026-11-28)

Source anchors: `source/linux_journal_owner_policy.h` (owner and write
decision), `source/linux_daemon_journal.h` (`daemon_journal_admit()`),
`source/linux_daemon_state_set.h` and `keeps_multi_gpu_journal()` in
`source/linux_daemon_state.cpp`, `restore_committed_record()` in
`source/linux_daemon.cpp`; tests `tests/linux_journal_owner_tests.cpp`
(14900-14929, 14940, 14947), `tests/linux_daemon_commit_rollback_tests.cpp` (14930-14939, 14942-14946, 14948, 14950-14954, 14959),
`tests/linux_daemon_state_regression.cpp` (codes 12-16).

`active.bin` is an identity-bound single-record journal: the same file every
earlier build wrote, so an installer rollback to an older daemon reads it
unchanged. It does not yet store several cards' records; that arrives with
per-GPU runtimes. Its record already
carries the card's identity (PCI identity, location role, optional UUID); it
is attributed to a live card only by the existing restore resolver
(`linux_gpu_restore_selection()`: UUID first, PCI/location fallback, roles),
never by file name and never by guessing. No file is moved or migrated.

Before any hardware write (explicit Apply/Reset after target selection;
boot and resume restores after backend preparation) `daemon_journal_admit()`
classifies the journal against the served card:

| Owner | Meaning | Apply / automatic | Reset |
|---|---|---|---|
| none / served | no journal, or this card's | allowed | allowed |
| other | resolves to another present card | refused: select it and Reset it | refused |
| gone | discovery is complete and no other present card could be it: none reports its UUID, and none shares its model unless that card reports its own unique, different UUID | refused | releases a settled journal (ACTIVE, no named controls) as its own durable step, then resets this card normally; never unfinished recovery |
| unproven | ambiguous, contradicted, or discovery listed fewer devices than NVML reported (or one without PCI identity) and the journal resolves by neither a UUID match nor (for a journal without a UUID) its exact recorded location | refused | refused |
| unreadable | several GPUs' journals, a read error, or (for an automatic write) a journal the loader just refused and removed | refused | refused |

Until every GPU has its own runtime only one card holds Green Curve state,
on disk as in memory; the in-memory single-owner guard, the fan ownership gate
and the named-graphics checks are unchanged. "Not found" is never treated as
"absent" unless discovery proves it (`LinuxGpuDiscovery::reportedCount`).

Releasing a missing card's settled journal is a durable step of its own,
completed before the served card's Reset begins; that Reset then journals
normally (PREPARED, removed on commit). One record never holds both claims,
and every crash point leaves a consistent journal: the old one, none, or the
Reset's own PREPARED record. A release that fails (including an unlink whose
directory sync failed) restores the journal's exact bytes and stops before
any write. Unfinished recovery
(PREPARED, UNCERTAIN, named controls) of another card is never taken over,
even on a one-card host. Unattended writes (boot, resume) also prove their
own target with the same rule before writing, since a boot profile can exist
without any journal; a target that is not proven on the bound card is
refused (`daemon auto-restore: ... target proof=`). An automatic restore that finds an unreadable journal (removed
by the loader) latches uncertainty, so the next resume still refuses.

The journal's exact bytes are pinned before the write, and the card's learned
UUID carries through PREPARED, ACTIVE and rollback even when a UUID read
fails at that moment. When a failed write would otherwise remove the
card's own journal (no intent in memory: a journal not replayed under startup
policy `none`), `restore_committed_record()` stores those bytes back instead
(`daemon state: failed operation restored the journal it replaced`). A release
whose unlink happened but could be neither synced nor undone keeps the exact
record in the controller (`journalHold`); every write refuses until it is
stored again, after which admission proceeds against it (14959). That hold lives in memory: if storage fails that badly and the daemon
then restarts, whatever the filesystem kept decides, and a removal that
persisted is exactly the release the user asked for (a missing card's settled
settings). Backend target selection decides by UUID first on the evidence
both discoveries hold: a UUID an earlier discovery read for the same exact
location carries forward when a later query fails
(`linux_gpu_uuid_carry_forward()`, never a UUID this discovery read
elsewhere), so a known mismatch is refused instead of becoming a match by
location, while a card proven by UUID at a new location is still found.
Carried UUIDs only veto: they are evaluated in a separate evidence array and
never published or journaled as a card's identity, which stays what the
current discovery read (14964). On a host with several GPUs (NVML reports or
discovery lists more than one), a record naming a UUID is proven only by a
current read of that UUID, and only while every reported card was listed and
read (`linux_journal_every_uuid_read()`): `linux_journal_owner()` never
accepts it by location or model there, because no history of earlier reads
can show which card a location holds once cards can be swapped, moved or left
unopened, and a UUID that looks unique among partial reads may be a
duplicate of an unread card. This one rule covers journal admission, the
unattended-target proof, the fan-marker proof and the startup binding's
refresh check and published topology (`daemon_stored_gpu_index()`); a single
GPU, and a record written without a UUID, keep the location rule as before
(14917, 14940, 14942-14944, 14947). "Current" means read in the present NVML
session: `LinuxDriverContext::nvmlSessions` counts successful
initializations, and `linux_backend_expire_session_uuids()` drops a state's
UUIDs from a replaced session at the start of every request, before journal
admission and the fan-marker proof, on the bound fast path and in same-PCI
recovery. Recovery reads every device it enumerates: the selected card keeps
a UUID only by reading it (or, on the same handle and session, keeping its
own), every other row becomes exactly what recovery read at that location
(absent when unread), and a read showing another card at the bound location
revokes the binding's write identity until a full rebind. The intent in
memory drives the fan worker, the snapshot's fan state and the adapter
summary's ACTIVE flag only while `daemon_active_intent_on_served()` holds:
write identity, the PCI identity, and the served card's own UUID read now
equal to the intent's (`daemon_intent_uuid_names()`; with several GPUs a
missing read is no proof; other cards' reads do not matter here, since the
question is which card the handle addresses), except that a UUID another
listed card also reads proves neither card, wherever the duplicate was found.
An owned manual fan that loses
this proof is not silently left: nothing is written to a card that may be
another one, so the handback is latched incomplete, a fault (handback failed)
and uncertainty are recorded, and the ownership marker keeps the card's
binding until a Reset on the proven card returns the fan (14973-14974). With
several GPUs a new manual fan is taken only under the proof its return will
need: every reported card listed and read, this card's UUID unique (14975),
and likewise a first UUID-bound journal (or one replacing this card's record
without a UUID) is written only where its own Reset could prove the card
again (14984); handing a fan back, the safe direction,
needs only the served card reading the marker's UUID uniquely among the
cards read (14985), and a selected card that had a UUID and lost it through a new
handle or NVML session, or whose UUID another device recovery read reports
too (listed or not, including a UUID read for the first time), loses its
write identity in recovery; so does any card
on a host that only now shows several GPUs (its single-GPU binding never
named it by UUID; a full rebind does), and a recovery that cannot enumerate
or find the selected card again on such a host. A hardware write refreshing
through it is refused by preflight (14976, 14981), and a write after which
the binding lost its identity is never committed: no ACTIVE record is stored
for a binding without write identity, so Apply, Reset and automatic restores
end UNCERTAIN instead (14978, 14982); such a record never takes a UUID read
through the revoked binding (14983), and a binding pinned before a write is
kept as given, so a UUID first read during the write never enters its record
(14986). A record stored
for an explicit UUID keeps it; a different UUID read from the card now at
that location never re-attributes the journal (14977); an Apply pins the
UUID it verified before writing through commit, so its own refresh losing
the read does not leave its records without it (14978); a rollback is never
written through a binding whose write identity was revoked (the caller keeps
recovery state); and after a renumbering the card that reads the intent's
UUID may be selected to Reset it (another card's read does not matter),
while an unrelated card, an intent without a UUID (a model match is no
proof), or a UUID another listed card also reads (a duplicate proves
neither) may not. Once selected, and still reading that UUID on the new
binding, the intent follows it to its new location, so the fan worker and
Reset, which compare PCI identity, treat it as that card's; otherwise an
owned manual fan is reported incomplete like any lost proof (14979). A Reset
whose journal removal and rollback both fail leaves a durable UNCERTAIN
record with its pinned UUID, as Apply does, so a restart never replays the
old settings; after a failed rollback neither Apply nor Reset writes the old
ACTIVE record back first (14980). Recovery also
raises the device count to what NVML reports now, so a host that grew is not
judged as one GPU (only a full discovery, which rebuilds the adapter list,
lowers it) (14946, 14948-14949, 14967-14972). A journal's named graphics
coverage joins the card's recovery only after the journal and the target are
proven (14945);
the location shortcut is skipped when the known inventory places the stored
UUID on another adapter or on several, and when the earlier evidence was a
mismatch at the chosen location or a duplicate claim of the stored UUID,
the rediscovered choice needs fresh proof
(`linux_gpu_uuid_known_evidence_unresolved()`: the stored UUID read at the
chosen card, every other earlier claimant gone or reading something else).
Earlier evidence belongs to a location (device IDs compared only where both
are known), so optional metadata a rediscovery failed to read never detaches
it, and a claimant that is not listed counts as gone only when discovery
listed every device NVML reported, each with a usable location, and no card
of its model at another location could be it moved (one that has not freshly
read a different UUID); a different card now at its old location rules out
only that occupant. A mismatch or ambiguity the held evidence already
showed, including one first learned by the selected handle's own fresh read,
refuses until fresh evidence matches, even when nothing ties it to a location
(14921-14929, 14958, 14960, 14962-14963); a bound fan marker with no recorded location is
held to the same proof as the journal (14961).

The fan ownership marker is held to the same proof: its write gate and the
startup handback accept a bound marker only when `linux_journal_owner()`
proves its card on the selected adapter (so a marker never follows its model
to a twin while a reported device was not opened), and a target-less legacy
marker counts as "one card" only when NVML reported exactly one device
(`tests/linux_fan_ownership_tests.cpp`, 14955-14957). The backend's
selected-GPU fast path keeps the UUID discovery learned when a fresh UUID
query fails, so the evidence that would expose a replacement card is never
erased (`tests/linux_driver_discovery_cases.h`, 14958).

Two journal writes keep their card's binding: an automatic restore whose
write failed records UNCERTAIN against the card the backend resolved and
wrote (not the stored target, which may name the card's pre-renumbering
location), and a fan lockout's UNCERTAIN journal keeps the binding's location
role and learned UUID (`tests/fan_runtime_followup_tests.cpp`, 14936, 14951).

A file of k > 1 current-layout records is the per-GPU journal of a daemon that
runs several GPUs. This daemon reports it as `LINUX_DAEMON_STATE_MULTIPLE`
(boot treats it as requiring recovery), and refuses to replace or remove it,
instead of deleting it as an unknown layout. It never writes one itself.

Still daemon-wide until each GPU has its own runtime: the startup policy
(`startup.bin`, one choice whose PROFILE binding names its card), the
automatic-restore guard (`restore-guard.bin`; a lockout covers every card,
which is strictly more conservative) and operation results (`operation.bin`,
looked up by id). The fan ownership marker keeps its single-record form; its
existing gate already refuses writes while another card's marker is
outstanding. Diagnostics: `daemon journal: <write> owner=... decision=...`.

Behavior change for multi-GPU hosts: a journal left on card A (for example
under policy `none`) now blocks writes to card B until A is Reset, instead of
being overwritten by B's first write. A single unchanged card behaves as
before; a single replaced card (a different model in the slot) needs one
Reset before its first Apply, which releases the old card's journal.
