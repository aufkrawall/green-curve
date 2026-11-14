# Persistence schema (F-PERSIST-SCHEMA)

## Summary

Three files embed `DesiredSettings` as raw bytes. Each carries a version
number, and **none of those version numbers were ever bumped when the struct
changed underneath them**:

| File | Record | Current version | Frozen predecessor |
|------|--------|-----------------|--------------------|
| Linux `active.bin` | `LinuxDaemonStateRecord` (4816 B) | `LINUX_DAEMON_RECORD_VERSION = 11` | schema 7 (4808 B, v10), schema 6 (2472 B, v9), schema 5 (2440 B, v8), schema 4 (1280 B, v7), `LinuxDaemonStateRecordSchema3` (1184 B, versions 5-6), `LinuxDaemonStateRecordSchema2` (1176 B, version 4), `LinuxDaemonStateRecordSchema1` (1048 B, versions 2-3), `LinuxDaemonStateRecordSchema1V1` (1036 B, version 1) |
| Linux `startup.bin` | `LinuxDaemonStartupRecord` (4864 B) | `LINUX_DAEMON_STARTUP_VERSION = 9` | schema 6 (2528 B, v8), schema 5 (2496 B, v7), schema 4 (1336 B, v6), `LinuxDaemonStartupRecordSchema3` (1240 B, versions 4-5), `LinuxDaemonStartupRecordSchema2` (1236 B, version 3), `LinuxDaemonStartupRecordSchema1` (1108 B, versions 1-2) |
| Windows restart snapshot | `ServiceRestartReapplySnapshot` (4688 B) | `SERVICE_ACTIVE_DESIRED_VERSION = 9u` | schema 4 (2352 B, v8), schema 3 (2320 B, v7), schema 2 (1160 B, v6), `ServiceRestartReapplySnapshotSchema1` (1032 B, version 5) |

Every loader admitted a file only when its byte size equalled the **current**
struct. That had two consequences, and the second is the nastier one:

1. Any record written by an older build was classified corrupt and deleted.
2. Every backward-compatibility branch ever written to migrate an older
   generation was declared over a struct that had since changed — so it
   compiled, it looked like a migration, and it could never once have fired.

`SERVICE_ACTIVE_DESIRED_LEGACY_VERSION 4u` was such a branch (it demanded the
current payload size). So was `LinuxDaemonStateRecordV1`, whose "v1" layout
embedded the live `DesiredSettings`. Both are gone; a v1 file whose size still
identifies it (schema-1 `DesiredSettings`, no operation identity) is migrated
properly instead.

## Precise scalar runtime (2026-11-13)

Source anchors: `source/desired_scalar_intent.h`,
`source/linux_daemon_precise_scalar_schema.h`,
`source/gpu_backend_apply_precise_scalar.h`, `source/gpu_backend_apply_power.h`,
`tests/precise_scalar_schema_tests.cpp` (8220-8263).

DesiredSettings schema 4 is 2152 bytes. It appends memory Hz and surface
provenance, a reduced positive power ratio, and independent presence tags.
Legacy scalar fields remain checked display projections when a precise tag is
present. Old desired schema 3 stays frozen at 2120 bytes; wideners leave the new
tags/payloads zero. Protocol 32 used 2744-byte requests and 9936-byte responses.
All enclosing sizes above are compiler-checked on the supported architectures.

Explicit scalar scopes resolve exact units before mutation. Memory keeps the
existing application bound of +/-3000 physical MHz and refuses sub-grid values;
power resolves against fresh destination controls, never its percent preview.
Memory cleanup binds the outgoing surface. Prepared ownership retains the
resolved API, and independently refreshed backend reads must agree. Legacy
overrides following precise ownership resolve exact units; a named-profile transition
following precise ownership goes through scoped cleanup. Profile matching uses
physical memory intent and rational power, retaining named-surface authority.
Flat local document storage and offline transfer are connected; precise runtime
document compilation and GUI/TUI draft integration remain pending.

Protocol 33 adds a getter-only capture command and a separate 4832-byte
observation. The requested-domain word occupies former trailing request padding,
so requests stay 2744 bytes; responses become 14768 bytes. DesiredSettings and
the durable records above are unchanged. Capture accepts no file path, settings,
operation or restore fields, and never rebinds or recovers a GPU. Both platforms
use only an existing usable session. Linux passive envelope publication skips
ordinary snapshot getters; Windows queries only its resolved interface cache.
Cases 8502-8513 and 8520-8526 cover wire boundaries and the daemon handler.

Frozen daemon schema 5 and Windows restart schema 3 preserve the outgoing
captured-offset layout. Loaders check original versions/checksums before
widening and keep identity, scope and operation metadata. Current record,
historical byte and production power-phase fixtures cover precision, owned zero,
cleanup, stale denominators and previews that hide a different target.

## Frozen precise-scalar predecessor (2026-11-13)

`source/desired_settings_precise_schema.h` pins schema 4 at 2152 bytes with
fieldwise widening/narrowing, including exact memory and rational power fields.
`source/linux_daemon_authoring_schema.h` freezes its 2472-byte active and
2528-byte startup records; the restart schema header freezes its 2352-byte
payload. Cases 8640-8644 check offsets, scalar/captured preservation and original
checksum validation. Each file loader dispatches these original sizes before
widening into the current authoring layout.

## Bound authoring runtime capacity (2026-11-13)

Source anchors: `source/desired_authoring_types.h`,
`source/desired_authoring_intent.h`, `source/service_desired_mutation_policy.h`,
`source/service_scoped_apply_policy.h`, `tests/desired_authoring_schema_cases.h`
(8650-8676), and the predecessor loaders above.

DesiredSettings schema 5 is 4488 bytes. Its explicit 2336-byte extension retains
exact absolute point frequencies, graphics offsets and surface, lock frequency,
identity exclusions and full resolved destination voltages. Tags are independent
from display previews. Full topology belongs to runtime authority; portable
source observations and local acknowledgments remain outside this layout.
Structural checks reject inconsistent tags/previews, missing bound identities,
inactive payloads and reserved bytes. Copy, clear, projection and sparse merge
preserve the new authority fieldwise. Historical wideners initialize it to zero.

Protocol 34 introduced 5096-byte requests and 19440-byte responses. It reserves a fresh
control fingerprint and domain mask for reviewed Apply. At that migration,
nonzero authoring extensions and control preconditions were refused with an
explicit reason until destination review and exact adapters were activated.
Storage capacity does not enable rounded replay.
The compiler checks current and frozen sizes; the native suite checks exact
round trips, old-byte migration, checksum corruption, malformed tags, fan-only
retention and omission cleanup. No physical replay is claimed.

Protocol 35 keeps 5096-byte requests and uses 19472-byte responses. Capture
schema 2 grows to 4864 bytes with explicit 32-byte VF constraints: exact offset
grid, range provenance and backend write support. Reported graphics bounds and
the application fallback remain distinct. These facts join the normalized
control fingerprint; mixed protocol/schema peers refuse rather than guessing.
DesiredSettings and the durable disk layouts remain unchanged.

Protocol 36 activates the reserved request control-precondition fields without
changing those sizes. The service capture revision binds physical controls and
outgoing owned intent, while its physical fingerprint stays separate. Reviewed
Apply requires all-domain passive evidence and an explicit interactive scope;
authoring/captured replay remains gated. See
[profile-document.md](profile-document.md#fresh-controls-and-ownership-review-2026-11-13).

## Captured-offset capacity (2026-11-12)

Source anchors: `source/desired_settings_schema.h`,
`source/linux_daemon_captured_offset_schema.h`,
`source/service_restart_snapshot_schema.h`,
`source/main_service_restart_load.cpp`, `tests/captured_schema_tests.cpp`.

DesiredSettings schema 3 (2120 bytes) appends independent presence and signed
physical-Hz arrays for captured VF offsets, plus an explicit application scope
in new-layout padding. Captured zero is owned; absence is separate. Schema 2
stays frozen at 964 bytes and widens with empty new arrays and legacy scope.
Protocol 31 has 2712-byte requests and 9872-byte responses. Sizes are checked
with the real compiler, including enclosing-record alignment. No capture
producer is connected yet; IPC explicitly refuses captured replay until the
document/editor pipeline is connected. Protocol 31 accepts explicit scopes and
the orthogonal VF point-set replacement flag, with strict numeric refusal
instead of legacy clamping. Scope does not authorize a global baseline reset.

The outgoing UUID-bearing daemon layouts are frozen as schema 4. Loaders
validate the original checksum before widening and retain UUID, location role
and operation identity. The outgoing Windows payload is frozen as schema 2 and
loaded by its size/version pair. Older migration branches remain. New flags,
inactive-payload canonicalization, empty-response validation and semantic
comparisons cover the appended arrays. Fixed-width aliases moved to
`source/gpu_wire_types.h`; restart loading moved out of its oversized shard.

Fixtures 8010-8030 construct historical structs directly, verify checksums and
widening, distinguish an owned zero, preserve 15000500 Hz and reject mixed
protocol versions. Actual Linux loader coverage of these new record generations
and hardware replay remain separate from shared-fixture execution under WSL.

## Windows graphics-control recovery marker (2026-11-13)

Source anchors: `source/ownership_handback_policy.h`,
`source/graphics_scalar_recovery.h`, `source/main_service_ownership_handback.cpp`,
`source/gpu_backend_graphics_offset.h`, `tests/memory_recovery_tests.cpp`
(9033-9059).

The separate Windows ownership marker is version 2, 224 bytes. Its frozen
version-1 predecessor is 216 bytes and widens fieldwise with zero new control
coverage; old tail padding cannot become new fields. Size/version, boot
identity, attempt count, known coverage bits, reserved zero and target binding
are validated before publishing decoded state. New nonempty coverage requires
a valid target. Oversized/truncated records refuse through the existing loader.

The shared preparation policy publishes added named graphics-control coverage
only after the existing atomic, write-through marker store succeeds. Repeated
coverage needs no further I/O. A failed store blocks the named setter, including
an otherwise inert ownership claim. Coverage includes owned zero and survives
service restart independently of the current request or preferred getter.
The durable union is conservative until a successful full reset; live coverage
may retire a verified handback earlier. It is not a stored value or replay proof.

Handback restores coverage after selected-GPU epoch validation. An
explicit pending-handback takeover retains coverage only for the matching
GPU; a superseding write cannot discard unresolved named controls. A new
pre-write marker preserves adopted coverage before pending authority retires.
Failed preparation leaves the handback pending. An Apply cannot bypass
unowned recovery coverage; a same-control repair remains possible.
Reset and rollback cannot treat the VF route as scalar-free while named coverage remains;
they repair each covered control through its own exact API. Unavailable or
unverified repair retains remaining coverage and prevents ceiling release.
GPU selection clears only the outgoing live evidence through its existing
selection lifecycle. Diagnostics use bounded `graphics recovery:` lines.

Fixtures execute the production named setter/reset with a fake durable store,
verify zero setters on store failure, retain owned zero, restore two controls
from coverage alone, test failed repair and frozen padding, and check target
switch evidence retirement. Native GPU behavior remains unverified. Full
cross-surface Apply cleanup and reviewed transition protection remain separate
replay-activation requirements. Linux durability is described below.

## Linux graphics-control recovery (2026-11-14)

Source anchors: `source/linux_daemon_graphics_recovery_schema.h`,
`source/linux_graphics_recovery_policy.h`, `source/linux_graphics_offset_control.h`,
`source/linux_backend_rollback.h`, `source/linux_daemon_record_recovery.h`,
`tests/linux_clock_offset_tests.cpp`, `tests/desired_authoring_schema_cases.h`.

- Active version 11 adds validated named-control coverage and a zero reserved
  field. Version 10 remains frozen at 4808 bytes, including its old checksum
  boundary; migration preserves authoring intent and grants no recovery
  authority from padding. Its embedded authoring layout is explicitly frozen.
- Explicit Apply, Reset and automatic restore publish operation-scoped write
  permission only after PREPARED is durable. Owned zero retains its exact API.
  Snapshot and rollback track both named preimages and actually attempted APIs;
  failed rollback retains coverage even after a successful forward Reset.
  Missing named readback blocks affected writes and Reset, while independent
  power-only updates preserve that control without requiring its getter.
- PREPARED/UNCERTAIN records require recovery regardless of startup policy.
  Non-replayed ACTIVE named controls also remain pending; a blocked startup
  replay cannot discard them. Explicit Reset adopts coverage only after the
  stored location role and optional UUID resolve to the selected GPU.
  Positive UUID identity still follows PCI movement during backend rebind;
  without comparable UUIDs the existing exact PCI/model identity is required.
- New named writes refuse unresolved recovery and control-surface handoffs.
  Full authoring replay remains closed pending cross-surface transitions and
  reviewed transition/pin evidence. Legacy unknown-GPU fallback remains enabled.
- The native file fixture adapts root identity only for its own scratch tree;
  production loader/store, permissions, links, fsync and atomic replacement
  execute unchanged. It checks old-file migration without rewriting, current
  coverage persistence and rejection without damaging the previous record.
- Diagnostics use the stable graphics-offset prefix with surface, coverage,
  attempt and verification fields. Cases 9060-9088 cover denied preparation,
  owned zero, dual-control Reset, exact rollback, target binding and migration;
  the transition fixture covers early protection refusal and no-write rollback.

Last verified: 2026-11-14 against compiler-checked Windows/Linux layouts and
the hardware-free fixtures. Native Linux files passed both the ordinary-user
fixture and real root ownership path. ASan/UBSan, all-target compile checks and
static analysis passed; disabling preparation fails case 9060. Physical GPU
recovery remains unverified.

## The rule

> **SIZE selects the LAYOUT. VERSION selects the SEMANTICS within it.**

A record's byte count is the one thing a writer cannot get wrong, so it is what
a reader dispatches on. The version field then says what the fields MEAN — the
0.25.2 memory-offset unit reinterpretation is exactly such a change: same
layout, different meaning — and it is never asked to imply a layout.

This is not a style preference. `SERVICE_ACTIVE_DESIRED_VERSION` sat at 5 from
0.19.2 onward across several payload layouts, and `LINUX_DAEMON_RECORD_VERSION`
sat at 1 across at least three (the XBAR, SYS-clock and video-clock fields all
landed inside that window). A version number that spans multiple layouts cannot
select one; only the size can.

## Durable GPU location role (2026-10-24)

Source anchors: `source/linux_daemon_state.h`,
`source/linux_daemon_location_schema.h`, `source/desired_settings_schema.h`,
`source/linux_daemon_state.cpp`, `tests/linux_restore_role_tests.cpp`.
State v5 and startup v4 append the GPU location role under their checksums;
state v6 and startup v5 keep that layout and change only what a stored
`required` means (below).
The outgoing layouts use frozen `DesiredSettingsSchema2` (964 bytes), with
field-by-field widening that preserves per-point provenance and display MHz.
Both loaders dispatch the old sizes and validate their original checksum
before widening. Schema-1 memory conversion remains unchanged.

Every older record (schema 1 and schema 2, state and startup) widens with
`LINUX_DAEMON_LEGACY_LOCATION_ROLE`, which is `unrecorded`: the record never
said whether its location distinguished identical cards, and `unrecorded` is
exactly that rule -- the exact location wins, and a moved model is followed
only while it is the one adapter present. It is the rule Windows applies to a
role-less `[gpu]` record and the rule every earlier Linux build applied to
these targets. The first successful automatic restore (or an explicit
Apply/startup binding) records the real role from current enumeration, and a
boot profile re-binds startup.bin to the live location with it.

Rejected (2026-10-24): widening as `required`. It was stricter than any build
had read the record, so an upgrade silently stopped the boot apply of a lone
GPU whose PCI bus firmware had renumbered -- the very case the location role
exists for. Source gates in `tools/persistence_gates.py` forbid `REQUIRED` in
the frozen-layout header and pin every widening call to the legacy role.

The first role generation (state v5, startup v4, written only by unreleased
builds) is re-read on load (`linux_daemon_*_widen_schema3()`): its
`required` may have been invented by that widening and cannot be told apart
from a twin selection, so only `advisory` survives and everything else becomes
the legacy role; the record widens with an absent UUID after its own
checksum validates. Users therefore need no manual re-bind after updating. Cost:
a twin binding made by those builds follows the remaining twin if the other is
removed (the released builds' rule) until a restore re-records `required`; with
both twins present the exact location still wins. The loaders' wiring is
source-gated; a native Linux probe also exercises their protected-file path.

Automatic restore, resume, startup content refresh and backend replacement keep
the role; without UUID evidence a required role never weakens merely because a twin disappeared.
Migration diagnostics name the old size/version and the role the record widened
with; automatic restore logs its resolved role and live location, and an
aborted one logs the stored location, role and remedy. Fixtures 7420-7429 and
7440-7447 check both record types and all five widening paths, preservation of
curve/lock/provenance/memory settings, checksum coverage, invalid roles, exact
versus moved versus ambiguous restore, the moved single-GPU boot profile and the
remedy text; 7450-7459 cover the first-generation re-read (required to
unrecorded, advisory kept, current-version required untouched, tampered bytes
refused, disabled policy). A negative control dropping the backend role fails
at 7421; widening as `required` again fails at 7423; keeping a re-read
`required` fails at 7451. Real Linux hardware upgrade and
resume remain unverified.

## Optional physical GPU UUID (2026-10-24)

Source anchors: `source/linux_gpu_uuid.h`, `source/linux_daemon_uuid_schema.h`,
`source/linux_backend_discovery.cpp`, `source/linux_auto_restore_runtime.h`,
`source/linux_startup_policy.h`, `tests/linux_gpu_uuid_tests.cpp`.
State v7 and startup v6 append an optional physical-device UUID inside their
checksummed atomic records. The preceding role layouts are frozen as schema 3;
state v5/startup v4 keep the compatibility role re-read, state v6/startup v5
preserve their trusted role, and every earlier generation retains its existing
migration. All predecessors start without a UUID. DesiredSettings, the client
wire and GpuAdapterInfo are unchanged.

The daemon optionally queries NVML's physical GPU UUID. A unique matching UUID
and nonconflicting known model metadata resolve the same physical card across a PCI bus move, even
with identical cards or a required location. A positive UUID mismatch on the
candidate refuses the restore before any write. Duplicate UUID matches refuse
as ambiguous. Missing functions, failed queries and unrecognized UUID formats
fall back to the previous model/location resolver; they never become a new
hardware-support prerequisite. A selection fast path queries its existing
handle and preserves that fast path on an unavailable UUID query.

Explicit Apply/startup binding capture the observed UUID. Automatic restore,
resume and startup content refresh retain a learned UUID through optional-query
failure. A successful legacy boot profile also stores its newly learned UUID
when its location and role did not change. Diagnostics report only verdicts and
presence flags, never the physical identifier.

Design limit: a legacy record cannot identify a removed physical card before
its first successful UUID capture. This change preserves that first restore's
compatibility behavior. UUID-unavailable hardware also retains its old ambiguity.
Downgrading to an older binary cannot read the new larger records; existing
forward upgrades remain supported by the frozen-layout loaders.

Last verified: 2026-10-24 (host-neutral UUID and frozen-record regression tests,
plus native Linux actual-loader and atomic-record probes; live physical GPU replacement and Linux resume remain hardware-unverified).

## Adding a field to DesiredSettings

One commit must do all of this, or the build breaks:

1. Freeze the outgoing layout in `source/desired_settings_schema.h` as
   `DesiredSettingsSchema<N>`, with its exact byte size asserted, plus a
   widening copy into the live struct that gives every NEW field the value that
   preserves the OLD behaviour.
2. Freeze the outgoing record layouts that embed it (`linux_daemon_state.h`,
   `service_restart_snapshot_schema.h`) and teach each loader the new size.
3. Bump `LINUX_DAEMON_RECORD_VERSION`, `LINUX_DAEMON_STARTUP_VERSION`,
   `SERVICE_ACTIVE_DESIRED_VERSION` and `SERVICE_PROTOCOL_VERSION`.

The `static_assert`s are what make step 1 unmissable. `sizeof(DesiredSettings)
== 964` in `service_protocol.h` now names all four version constants in its
failure message instead of saying only "bump the IPC protocol version" — which
is precisely the instruction that was followed, alone, in 0.26.0. Each record
additionally pins its own size, so a change to `GpuAdapterInfo` (also embedded
by value) breaks the build at the record too.

## Widening rules

- A stored record is validated against **its own checksum over its own bytes**
  before anything mutates. The stored hash covers the stored version field.
- `curvePointFromGpuOffset[]` widens to **zero**, which preserves 0.25.2
  behaviour exactly: that build had no provenance, so every point it stored was
  honoured as an absolute target, and an unflagged point still is. A widening
  that forged provenance would turn every restored point into an offset that
  tracks a moving base.
- The memory-unit halving rides on the version
  (`version <= *_PRE_DISPLAY_MEM_UNITS_VERSION`), applied once, inside the
  widening. Layout migration by size, value migration by version, neither
  inferred from the other.
- Widening is field by field, never `memcpy` of a common prefix: a prefix copy
  keeps compiling — and starts corrupting — the moment someone reorders the
  live struct instead of appending to it.
- Nothing ever WRITES an old layout. The frozen structs exist to read files
  already on disk and to be deleted once that generation cannot be in the field.

## Diagnostics / failure modes

The original failure was silent by nature: the Linux daemon simply applied
nothing, and the Arch package's `post_upgrade` `try-restart` made it happen the
moment the package landed. Both Linux loaders now say why a record was refused
and what it was migrated from:

- `daemon state rejected: <n> bytes matches no known record layout (current=… schema1=… schema1v1=…)`
- `daemon state rejected: not a root-owned private regular file (regular=… uid=… links=… mode=…)`
- `daemon state rejected: record failed validation (version=… expected=… state=…)`
- `loaded <n>-byte schema-1 daemon state v<a> -> v<b>; mem offset <x> -> <y> display MHz`
- `daemon startup policy rejected: …` / `startup policy record migrated from 1108-byte schema-1 v<a> to v<b>; …`

`linux_daemon.cpp` now prints the loader's reason alongside its own
"rejected and removed" line; it used to drop it.

Windows: `restart reapply load: header mismatch magic=… ver=… size=… (expected v6/1160 or v5/1032)` and
`restart reapply load: widening v5 schema-1 snapshot (1032 bytes) to v6`.

## Coverage

- `tests/regression_main.cpp::run_persistence_schema_tests()` — codes 5018-5057.
  Fixtures are **byte-exact**, built through `desired_settings_narrow_to_schema1()`.
  The suite this replaced constructed the CURRENT structs and relabelled their
  version numbers, which is why it passed while the layouts diverged.
  Verified failing when a widening forges provenance (5030).
- The block lives in its own function on purpose: the frozen records are over a
  kilobyte each and several are live at once, which under ASan's redzones
  overflows `main()`'s frame. `main()` in this harness is close enough to that
  limit that a future large-struct suite should assume the same.
- Source gates: `tools/persistence_gates.py::check_all` (frozen sizes, the
  absence of the dead live-struct record, the loader's size dispatch, the
  version-keyed halving) and, for the Windows snapshot, the
  `service_restart_snapshot_schema.h` rules in the same module.

## Open questions / stale-risk

- **Unverified:** no real 0.25.2 → 0.26.0 Linux upgrade has been run on
  hardware. The widening is exercised only against synthesised fixtures. The
  acceptance test is: install 0.25.2, configure a startup profile, upgrade,
  and confirm the daemon logs a migration rather than "startup policy
  unreadable".
- A genuine v1 `active.bin` written before the video-clock field landed has a
  size this build does not recognise and is discarded. That is deliberate:
  version 1 never identified a single layout, so those files cannot be decoded,
  only guessed at.
- `LINUX_DAEMON_OPERATION_VERSION` (2) does not embed `DesiredSettings` and is
  unaffected; it still discards a v1 file, which is its existing documented
  "no valid persisted result" path.

Last verified for GPU role: 2026-10-24 (first-generation re-read: `--test`,
negative control at 7451; legacy role corrected to `unrecorded`:
`python build.py --test`, negative control at 7423, the failing boot apply
read from the daemon journal of a real single-GPU upgrade).
Prior payload-schema verification: 2026-09-17, with `build.py --test`,
`--test --asan`, `--tidy` and a full `build.py` green.
