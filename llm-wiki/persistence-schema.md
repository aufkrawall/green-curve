# Persistence schema (F-PERSIST-SCHEMA)

## Summary

Three files embed `DesiredSettings` as raw bytes. Each carries a version
number, and **none of those version numbers were ever bumped when the struct
changed underneath them**:

| File | Record | Current version | Frozen predecessor |
|------|--------|-----------------|--------------------|
| Linux `active.bin` | `LinuxDaemonStateRecord` (1280 B) | `LINUX_DAEMON_RECORD_VERSION = 7` | `LinuxDaemonStateRecordSchema3` (1184 B, versions 5-6), `LinuxDaemonStateRecordSchema2` (1176 B, version 4), `LinuxDaemonStateRecordSchema1` (1048 B, versions 2-3), `LinuxDaemonStateRecordSchema1V1` (1036 B, version 1) |
| Linux `startup.bin` | `LinuxDaemonStartupRecord` (1336 B) | `LINUX_DAEMON_STARTUP_VERSION = 6` | `LinuxDaemonStartupRecordSchema3` (1240 B, versions 4-5), `LinuxDaemonStartupRecordSchema2` (1236 B, version 3), `LinuxDaemonStartupRecordSchema1` (1108 B, versions 1-2) |
| Windows restart snapshot | `ServiceRestartReapplySnapshot` (1160 B) | `SERVICE_ACTIVE_DESIRED_VERSION = 6u` | `ServiceRestartReapplySnapshotSchema1` (1032 B, version 5) |

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
