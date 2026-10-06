# Persistence schema (F-PERSIST-SCHEMA)

## Summary

Three files embed `DesiredSettings` as raw bytes. Each carries a version
number, and **none of those version numbers were ever bumped when the struct
changed underneath them**:

| File | Record | Current version | Frozen predecessor |
|------|--------|-----------------|--------------------|
| Linux `active.bin` | `LinuxDaemonStateRecord` (1176 B) | `LINUX_DAEMON_RECORD_VERSION = 4` | `LinuxDaemonStateRecordSchema1` (1048 B, versions 2-3), `LinuxDaemonStateRecordSchema1V1` (1036 B, version 1) |
| Linux `startup.bin` | `LinuxDaemonStartupRecord` (1236 B) | `LINUX_DAEMON_STARTUP_VERSION = 3` | `LinuxDaemonStartupRecordSchema1` (1108 B, versions 1-2) |
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

Last verified: 2026-09-17 against the source, with `build.py --test`,
`--test --asan`, `--tidy` and a full `build.py` green.
