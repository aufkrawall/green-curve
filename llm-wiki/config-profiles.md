# Config and Profiles

## Config file

- **Format:** Windows INI through strict UTF-8→UTF-16
  `GetPrivateProfileStringW` / `WritePrivateProfileStringW` wrappers
- **Per-user config:** `%LOCALAPPDATA%\Green Curve\config.ini` (each user edits their own)
- **Shared profile bank (machine-wide):** `%ProgramData%\Green Curve\shared-profiles.ini` — admin-write, all-users-read; holds the admin-published profiles + the all-users default `logon_slot`. Publish/clear/default-slot/policy/migration writes fail closed if the protected DACL cannot be applied and verified. See [windows-architecture](windows-architecture.md) for logon routing and shared-bank security.
- **Legacy per-user location:** Beside the executable (one-time READ-ONLY import on first run; `set_default_config_path()` now fails closed and never *writes* a config beside the binary).
- **Linux:** `~/.config/greencurve/config.ini`

### Storage policy (Windows)

| Data | Location | ACL |
|------|----------|-----|
| Per-user config / per-user GUI logs | `%LOCALAPPDATA%\Green Curve\` | per-user |
| SYSTEM service dumps / logs / restart state | SYSTEM `%LOCALAPPDATA%\Green Curve\` | admin-only (sensitive — never `%ProgramData%`) |
| Shared profile bank + all-users default | `%ProgramData%\Green Curve\shared-profiles.ini` | SYSTEM+Admins:Full / Users:Read |

`%ProgramData%` is reserved for deliberately-shared, non-sensitive config only. Sensitive SYSTEM artifacts (crash dumps etc.) must never live there — `service_cleanup_legacy_programdata()` sweeps any such legacy files but preserves `shared-profiles.ini`.

## Config access

- Cross-session mutex-protected via `enter_config_storage_lock()` /
  `leave_config_storage_lock()`. The mutex lives in the `Global\\` namespace so
  the interactive GUI and session-0 service share the same lock. Its protected
  DACL grants Authenticated Users only wait/release rights, and a medium
  integrity label permits the unelevated GUI to open a SYSTEM-created object.
  Creation/open/wait failures fail closed.
- Guarded by `g_configLock` critical section
- `ConfigStorageLockGuard` is the RAII wrapper used by `config_profiles.cpp`.
  Whole-file profile save/clear holds the storage guard
  from its first preference read through atomic replacement and INI-cache flush;
  releasing after only the initial read can overwrite a concurrent logon choice.
- Profile load holds the same cross-session guard while reading the profile,
  curve, and fan-curve sections, so it cannot assemble settings from different
  file generations during an atomic replacement.
- Read: `get_config_int(path, section, key, default)`
- Write: `set_config_int(path, section, key, value)`; successful writes flush the
  Win32 INI cache and require exact locked readback before returning success.
- Check: `config_section_has_keys(path, section)`
- Paths stay UTF-8 in application state and cross `win32_utf8_paths.h` only at
  Win32 boundaries. Invalid UTF-8 fails closed with operation/error diagnostics;
  non-ASCII user/config/profile paths therefore do not depend on the active ANSI
  code page. Current path-size limits remain unchanged.

## Profile slots

- **5 slots** (`CONFIG_NUM_SLOTS = 5`), slot 1 is default (`CONFIG_DEFAULT_SLOT = 1`)
- Each profile is a separate INI section (e.g., `[profile1]`, `[profile2]`, ...)
- Profile data includes:
  - VF curve points (MHz values per point)
  - Curve lock state (anchor voltage, frequency)
  - GPU offset, memory offset
  - Power limit percentage
  - Fan mode, fixed fan percent, fan curve config, native zero-RPM intent
  - `gpu_offset_exclude_low70` flag
  - `curve_semantics` compatibility marker

### Native zero-RPM profile fields

Windows and Linux fan-curve sections persist `zero_rpm_enabled=0|1` and the
independent `zero_rpm_hysteresis_c=2..30`. Missing enablement means disabled,
preserving profiles written before protocol v23. A v23 profile that enables
zero-RPM but lacks the new v24 gap key inherits its old `hysteresis_c` value
(clamped to the 2-30C gap range), preserving its effective ON/OFF thresholds;
the next save writes both keys. This one-time migration is debug-logged.

`hysteresis_c` now means only ordinary curve-downshift hysteresis (0-10C).
The first enabled curve-point temperature is fan ON, and ON minus
`zero_rpm_hysteresis_c` is fan OFF. Both fields travel through profile
load/save, runtime capture, active/startup intent, and IPC; reserved bytes are
cleared at trust boundaries. See [fan-control.md](fan-control.md). Linux CLI
fan switches are partial overrides: `--fan-zero-rpm` and
`--fan-zero-rpm-hysteresis` select Curve mode without resetting saved points,
poll interval, or the other hysteresis. Invalid gap values fail before profile
write, and a duplicate-temperature override likewise leaves the profile
untouched.

## Lock state persistence

Lock state is **four fields treated as one unit**: `lockCi`, `lockMHz`, `lockMode`
(`none`/`flatten`/`hard`), and `lockTracksAnchor`. They are serialized as
`lock_ci` / `lock_mhz` / `lock_mode` / `lock_tracks_anchor`.

- **Invariant:** `merge_desired_settings()` copies all lock fields together when
  `override->hasLock`. Callers must not patch lock fields piecemeal.
- **Resolved root cause (Build 269):** the pin (HARD/dot) mode was previously lost
  on profile save because `merge_desired_settings()` ignored lock fields and
  `capture_gui_config_settings()` forgot `lockMode`, so saves wrote `lock_mode=0`
  and reloaded as flatten. The live *apply* path was unaffected (it carries
  `lockMode` end-to-end), which is why pinning worked until save/reload.
- `LockMode` has a fixed underlying type (`enum LockMode : int`) so the IPC
  sanitizer can clamp out-of-range values without UB.
- **Resolved root cause (Build 272):** even with Build 269's save fix, a HARD pin
  was still reverted before it could be applied or saved: the snapshot lockMode
  sync in `apply_service_snapshot_to_app()` (`main_state_sync.cpp`) had no
  intent guard, so the per-second telemetry snapshot (carrying the previously
  APPLIED mode) overwrote a fresh FLATTEN→HARD click or a loaded HARD profile
  at the same lock point within ~1 s ("No changes to apply" on Apply, flatten
  on save). Fixed with `lock_mode_sync_allowed()` (`app_shared.h`): adopt the
  snapshot's mode only when the GUI is clean and `lockMode == appliedLockMode`
  (no pending intent). `sync_applied_lock_state_from_curve()` now also syncs
  `appliedLockMode` so curve detection never fakes pending intent.
- `desired_has_any_action()` (Windows + Linux) counts `hasLock` — a pin-only
  profile is a real action on the CLI/apply gating paths.
- `load_profile_from_config()` runs `validate_desired_settings_for_ipc()` on the
  parsed profile before derived curve math, so corrupt/hand-edited INI values
  cannot reach the `(int)` casts in
  `restore_curve_points_from_base_plus_gpu_offset()` unclamped. The service's
  restart-reapply snapshot load does the same.
- The Linux `DesiredSettings` has no `lockMode` (no hard/NVML pin concept); its
  `merge_desired_settings()` carries `lockTracksAnchor` only.

## Profile operations

- **Save:** `save_profile_to_config()` — writes current GUI state to the selected slot sections (logs the lock ci/mhz/mode/tracks-anchor actually written)
- **Load:** `load_profile_from_config()` — reads a profile slot into `DesiredSettings`
- **Clear:** Deletes the profile section from INI
- **Editor provenance:** `populate_desired_into_gui()`
  (`config_profiles_gui_state.cpp`) is the single sink for every profile-sourced
  editor population — user slot Load, shared slot, machine/logon slot, the
  pre-READY pending draft, startup, and the post-apply repopulate. It marks
  `guiGpuOffsetFromProfileLoad` / `guiMemOffsetFromProfileLoad`, which the
  high-overclock confirmation (F-OC-WARN, see
  [windows-ui-layout.md](windows-ui-layout.md)) uses to exempt the user's own
  saved intent. The flags are cleared **per field** by the genuine `EN_CHANGE`
  edits in `ui_main_window.cpp`, so loading a `+300` MHz profile and then
  hand-typing a high memory offset warns about memory only. Programmatic writes
  cannot clear them: every one is wrapped in
  `begin/end_programmatic_edit_update()`.
- **Selected profile:** `[profiles] selected_slot` controls the slot shown in the profile combo and used for save/load actions
- **Applied profile:** `[profiles] applied_slot` is a GUI cache of service-owned
  profile identity, not a live-curve equality test. The service tracks active
  `ServiceProfileSource` plus slot in its snapshot/recovery state. When service
  intent still matches, normal temperature/boost VF drift does not clear the
  applied indicator. If the service has no matching active intent, clear the
  indicator without changing the user's saved `selected_slot` or logon choice.
  The combo-selection handler never marks a slot applied merely because it was
  selected.
- **GUI startup display:** when `[profiles] app_launch_slot=0`, startup refreshes
  the editor and graph from the background service's current GPU snapshot. The
  saved `selected_slot` remains selected for Load/Save/Clear, but its stored OC
  intent is not loaded until the user clicks **Load**. Selection is storage/UI
  state, not evidence that a profile is live.
- **App launch apply:** `[profiles] app_launch_slot` controls an optional profile apply on normal GUI start. Before the disruptive `resetOcBeforeApply` path, `maybe_load_app_launch_profile_to_gui()` compares the slot with active intent from the already accepted coherent `READY` protocol-v13 envelope; it does not issue a synchronous `GET_ACTIVE_DESIRED` request or chain a second state read. If the service already owns the same profile intent, the GUI only loads the slot into its draft, updates `selected_slot`, and logs `already active in background service; skipping reset-before-apply`. Otherwise the request enters the shared background coordinator, stamped with the accepted service/GPU/topology preconditions, and its response is adopted only on the GUI thread. This prevents an ordinary GUI reopen from resetting a matching active profile and avoids blocking the message pump.

### App-start active-service invariant

The app-start skip compares **Green Curve-owned desired intent**, not live driver telemetry:

- Exact ownership/value match is required for GPU offset, memory offset, power limit, lock fields, and explicit VF curve points.
- Fan comparison uses Green Curve fan intent. Auto ignores external live manual policy and fixed percent, so an external fan controller making NVML report manual does not turn Auto into Fixed.
- The transient `resetOcBeforeApply` flag is ignored because it is an apply mechanism, not runtime desired state.
- If there is no accepted `READY` active intent, Apply remains disabled while a
  full sync is pending; the configured app-start slot is queued only after a
  coherent state is accepted. If the accepted intent differs, it applies with
  reset-before-apply and generation-stamped preconditions.

The disabled app-start path always shows the current service snapshot and never
loads `selected_slot`. Expected driver drift remains diagnostic-only. When the
user explicitly enables an app-start slot, the active-service idempotency check
still uses service ownership metadata rather than absolute live VF MHz.

## Ownership versus live drift

Absolute live VF MHz cannot prove whether a profile is applied. NVIDIA may move
the reported curve by several MHz with temperature and boost state while the
service's intended offsets/curve remain owned. Comparing saved absolute curve
points to readback caused false `applied_slot=0` transitions.

The authoritative signal is now service intent plus active profile source/slot.
Normal drift may be logged but never triggers a write, invalidates ownership, or
changes the user's selection. A service Reset or an explicit divergent apply
clears/replaces ownership authoritatively. An ordinary service restart has no
active in-memory ownership and therefore clears only the applied indicator; it
does not infer a profile from live hardware or alter `selected_slot`.

### The applied indicator must be drift-free

Source anchors: `source/applied_profile_indicator_policy.h`,
`sync_applied_profile_from_service_metadata()` in `config_profiles_ui.cpp`,
`load_profile_from_config()` in `config_profiles.cpp`,
`config_profile_sync_cache.cpp`.

The rule above ("never compare against live readback") was enforced on the
*direct* comparison and then re-broken indirectly. Reported 2026-07-30: loading
a profile in the GUI **without applying it** made the tray menu's tick
disappear, and only a fresh Apply brought it back.

`sync_applied_profile_from_service_metadata()` confirms service-declared
ownership by re-reading the named slot and comparing it with the active intent.
Both sides are supposed to be records. But `load_profile_from_config()`
*projects* a stored profile onto the current GPU before returning it:

| Projection | Live input |
|---|---|
| lock-anchor re-derivation (`lockTracksAnchor`) | `is_curve_point_visible_in_gui()` -> `curve_base_khz_for_point()` = `g_app.curve[i].freq_kHz - g_app.freqOffsets[i]` |
| curve-ownership visibility strip | `g_app.curve[i].volt_uV`, gated on `g_app.loaded` |
| `profile_point_saved_visible()` fallback | same, for profiles predating `pointN_visible` |

`lockTracksAnchor` is compared field-for-field by
`desired_settings_match_active_service_intent()`, so the verdict flipped with
ordinary boost/temperature drift — and none of those live inputs were in
`AppliedProfileSyncCache`, so **the flip was not noticed when it happened**. The
next event that did invalidate the cache applied the stale verdict and wrote
`applied_slot = 0`. A plain profile **Load** is that event: it writes
`selected_slot`, changing the config stamp. Hence the symptom pinned the blame
on Load, which does nothing wrong. The write is sticky because the recomputed
value is then cached as consistent, so only a new Apply re-establishes it.

Fixes:

- `ProfileReadMode` (`applied_profile_indicator_policy.h`).
  `PROFILE_READ_FOR_EDITOR` is the default and unchanged — projection is right
  for the editor and for anything on its way to hardware.
  `PROFILE_READ_FOR_OWNERSHIP` skips all three projections above.
- The verdict is a pure function, `applied_profile_indicator_slot()`, with a
  typed reason. Assertions 2054-2068 cover the whole table; a source gate
  (`ui_gates.check_applied_profile_indicator_is_drift_free`) pins that the sync
  asks for the ownership read and that both loader gates stay in place.
- The cache key gained the populated-point mask. The ownership read is
  drift-free but still **topology-scoped**: a `curve_semantics=base_plus_gpu_offset`
  profile is decoded through `is_gpu_offset_excluded_low_point()`, which counts
  populated points. Populated-ness does not move with temperature, but it does
  move with the GPU — and a cache that omits an input the decision reads is
  exactly how this hid for so long.
- Every evaluation logs `verdict slot=N reason=... (authoritative/activeIntent/
  source/serviceSlot/candidate/readable/matches/detail)` through
  `debug_log_on_change()`, so "why is my tick gone" is answerable from one run's
  log instead of a rebuild.

Deliberately **preserved**: a slot whose stored record genuinely no longer
matches what is running still clears the indicator
(`APPLIED_PROFILE_REASON_PROFILE_EDITED`). The bug was the false positives, not
the feature.

**Unverified:** the fix is reasoned from source plus the reporter's observation
that only a re-Apply restored the tick; it has not been watched on a live
Windows machine, and the new log line has never been read from a real run.

### The service must not lose a slot identity to a delta Apply

Source anchors: `service_profile_record_describes_intent()`,
`service_validate_requested_profile_metadata()`,
`service_confirm_profile_metadata_from_active_intent()`,
`service_record_apply_profile_identity()` in `main_service_request_policy.cpp`;
`source/service_profile_identity_policy.h`; the `SERVICE_CMD_APPLY` handler in
`main_service_pipe.cpp`.

Reported 2026-07-30, and **confirmed from a real debug log** (unlike the entry
above): with a profile active, the tray menu showed no tick at GUI start and
still showed none after switching profile in the main window; only picking a
slot from the tray context menu produced one.

All three symptoms are one defect, and it is upstream of the GUI indicator. The
service refused to record the slot identity at all, so the indicator correctly
reported `APPLIED_PROFILE_REASON_NOT_A_USER_SLOT` for an
`AD_HOC`/slot-0 owner. The log line that proves it:

```
service APPLY: ignoring unverified profile metadata source=1 slot=1:
    fan ownership differs profile=1 active=0
... applied profile metadata sync: verdict slot=0 reason=active profile is not
    a personal slot (source=4 serviceSlot=0)
```

`service_validate_requested_profile_metadata()` proved a claimed slot by
comparing the stored record with the **request payload**. That works for the
tray, hotkey, and app-start paths, which load a slot and send it whole. It can
never work for the GUI's Apply, because `capture_gui_apply_settings()`
deliberately emits a **delta**: it drops every domain the editor is not
changing, and dropping the fan is the point — re-writing an unchanged fan policy
would disturb a running curve mid-game. A delta cannot equal a complete record
field-for-field, so every main-window Apply was recorded as ad-hoc. The startup
symptom is the same state observed later: the service was still holding the
`AD_HOC` identity its last GUI Apply left it with.

The question the check was asking ("is the payload the profile") was the wrong
one. The right one is "is the profile what is now in force", and after a
successful write the service holds exactly that — the delta merged over the
intent it already owned. So the proof happens twice:

| Proof | When | Effect |
|---|---|---|
| `FROM_REQUEST` | before the write | records the identity **and** authorizes replacing active ownership with the named profile |
| `FROM_ACTIVE_INTENT` | after the write | records the identity only |

`FROM_REQUEST` deliberately outranks `FROM_ACTIVE_INTENT`, and only it may
replace ownership: treating a delta as a complete declaration would return every
domain it omits to defaults, which is precisely how an Apply that never
mentioned the fan would reset a running fan curve. See
`service_build_profile_transition_request()` under *Named-profile transitions
versus partial applies* below.

Two further points:

- The post-write check evaluates the **same predicate** the GUI's applied
  indicator does (stored record versus `g_serviceActiveDesired`, through
  `desired_settings_match_active_service_intent()`), so the recorded identity
  and the displayed tick can no longer disagree.
- The service's read of the stored record was switched to
  `PROFILE_READ_FOR_OWNERSHIP`. It had been using the default editor
  projection — i.e. the same boost-drift exposure the section above documents,
  latent on the service side. Both halves are pinned by
  `ui_gates.check_service_profile_identity_survives_a_delta_apply`.

Deliberately **preserved**: a client cannot assert an identity. The service
loads the slot from its own trusted path (per-user config, or the machine bank
for a shared slot) and compares it against its own state; a genuinely edited
editor still yields `AD_HOC` and the honest "Manual settings" tooltip.

**Confirmed working on real hardware** the same day, from the log of the
installed build:

```
service APPLY: request payload does not itself prove profile metadata source=1
    slot=2: fan ownership differs profile=1 active=0 (re-checked after the write)
service APPLY: profile metadata proven by resulting active intent source=1 slot=2
applied profile metadata sync: verdict slot=2 reason=applied
tray profile: applied slot=2 source=1 sourceSlot=2 authoritative=1 -> "Profile 2"
```

#### The upgrade path lost the identity separately

Source anchor: `settings_transfer_export()` / `settings_transfer_apply()` in
`main_settings_transfer.cpp`.

Immediately after fixing the above, the tick was still missing **right after an
installer upgrade** — a second, independent cause with the same symptom.

The upgrade carries live settings across the new install by exporting the
service's active intent to a file and re-applying it afterwards
([installer.md](installer.md)). That restore was hard-coded to
`SERVICE_PROFILE_SOURCE_AD_HOC`, with this reasoning: the file may no longer
match the slot it came from, so claiming the slot would show a stale profile as
applied. The concern was right and pessimism was the wrong answer to it — every
upgrade silently demoted a running profile to "manual settings".

The concern is now handled by measurement instead. The export records
`[transfer] active_profile_source` / `active_profile_slot` alongside the intent,
the restore claims them, and the service verifies the claim against **its own**
copy of that slot's record. An edited slot lands on `AD_HOC` because the
comparison says so; an untouched one keeps the name it had before the upgrade.

Two properties worth knowing:

- A transfer file written by a build **older** than this one carries no
  `[transfer]` section. It reads back as `SERVICE_PROFILE_SOURCE_NONE`, claims
  nothing, and behaves exactly as before — so the first upgrade that preserves
  the identity is the one *from* a build that has this change, not *to* it.
- Recording the identity is best-effort. A snapshot that restores the correct
  settings without a slot name is far better than failing an upgrade over a
  label, so the export logs and continues if the keys cannot be written.

### Named-profile transitions versus partial applies

A named personal or shared profile is a complete Green Curve ownership
declaration, even when that profile intentionally omits a control. Selecting a
new named profile therefore replaces the previous active profile intent. The
hardware transition returns only controls that Green Curve previously owned but
the new profile omits to their defaults (for example GPU/memory offset, power,
fan, lock, or VF policy), then publishes the unmodified new profile as the exact
active intent. This prevents settings from another account or profile from
silently leaking into the new ownership snapshot.

An ad-hoc sparse Apply is different: it merges only the requested domains into
the current active intent. A fan-only, power-only, or memory-only adjustment
must not reset or discard unrelated curve ownership. Standby and controlled
driver recovery replay the resulting exact active intent. The pure transition
helpers are `service_build_profile_transition_request()` and
`service_build_full_restore_request()` in `service_lifecycle_policy.h`.

The window's manual Apply picks its request shape in
`source/gui_apply_shape_policy.h` (2026-09-23): no change, fan-only,
**power-sparse** (power, optionally with fan: no reset-before-apply, merged by
the service), or FULL (reset-before-apply with every current global). Any GPU
offset, memory, XBAR/SYS/VIDEO, curve or lock change is FULL; memory stays FULL
because a memory write can drop the VF curve outside the transition clamp.
Diagnostic: `capture_gui_apply_settings: shape=...`.

### Advanced-domain ownership relaxation

A FULL apply claims every advanced ClkDomains field the GPU exposes (XBAR clock,
MSVDD, SYS, VIDEO) at its current value. A profile saved before a domain
existed, or before the GUI learned the GPU exposes it, has no key and claims
nothing, so the post-write identity check logged
`xbar clock ownership differs profile=0 active=1` and the tray showed "Manual
settings". The relaxed ownership read (`relaxedOwnershipRead`, used by the
post-write identity check and the tray indicator) now also accepts a
profile-silent domain that the active intent claims at exactly 0
(`profile_ownership_advanced_mismatch_allowed()` in
`source/profile_ownership_policy.h`). A non-zero active claim, or the reverse
direction, still breaks the match; pre-write claims stay strict.

## Curve semantics

A saved VF point carries a number and, separately, what that number IS. The
second part is what `curve_semantics` records. One shared decision function,
`profile_curve_decode_from_marker()` in `source/profile_curve_semantics.h`,
maps the marker to a `ProfileCurveDecode` for BOTH platforms; the two loaders
used to spell the same test twice.

| Marker | Meaning | Written by |
|--------|---------|-----------|
| (absent) | `PROFILE_CURVE_DECODE_UNMARKED` — pre-marker file; each platform applies its own compatibility rule | pre-`curve_semantics` builds |
| `absolute_with_origin` | absolute MHz per point, plus `pointN_from_gpu_offset=1` on the projected ones | 0.26.0 onward, **every** save path |
| `base_plus_gpu_offset` | whole-section: stored MHz are BASE, the GPU-offset component is added back on load, and every restored point becomes offset-derived | up to 0.25.2 — **read only** now |
| anything else | `PROFILE_CURVE_DECODE_ABSOLUTE` — a marker from a newer build; the numbers are read as absolute | future builds |

**Why `absolute_with_origin` exists (F-CURVE-PROVENANCE).** `DesiredSettings`
gained per-point provenance (`curvePointFromGpuOffset[]`) in 0.26.0, and the
model it supports is a profile that MIXES kinds: load an offset profile, hand-
edit one point, and that point is a real absolute target while its neighbours
are still projections of the GPU offset over a stock base. A whole-section
marker cannot express that. Saving flattened it, and the loader then marked
every restored point offset-derived — so load, edit one point, save, reload
handed a number the user typed back to a base that moves with load (point 70:
2322 MHz idle, 2352 MHz at 99% util, a whole VF bin). The MHz round-tripped
exactly; only the authority behind them did not, which is why it was invisible.

Nothing is lost by storing absolutes. A projected point's absolute was only ever
a preview: `curve_point_target_offset_khz()` derives its offset from the
request's own `gpuOffsetMHz` and never reads the stored number
(`curve_point_offset_policy.h`), so per-point re-projection still happens. A
typed point keeps both its absolute and its authority.

The format is also the safer one to hand to an older build: an unrecognized
marker makes every loader fall through to "these are absolute", which is right
for the typed points and is what 0.25.2 did with every point anyway. Writing
base MHz under a marker an old build recognizes would have been the dangerous
direction — it would re-add an offset the new file no longer subtracted.

**Source anchors**
- `source/profile_curve_semantics.h` — the format, the marker constants, the shared decode.
- `source/profile_curve_origin_io.h` — Windows reader (`restore_curve_point_origins_from_section()`, `profile_curve_section_decode()`, `read_profile_point_int()`). Header-only and free of `g_app` so the regression harness can run it against a real INI.
- `source/config_profile_curve_format.cpp` — Windows writer half (`profile_curve_point_record_for_save()`), the one place that decides what a saved point contains; used by both `config_profiles.cpp` save sites and the installer transfer export in `main_runtime_capture.cpp`.
- `source/linux_profile_curve_codec.h` — the Linux read/write pair.
- `source/config_profile_repair.cpp` — `curve_section_uses_base_plus_gpu_offset_semantics()`, now keyed to the shared decode, keeps its extra heuristic for UNMARKED files only.
- Gates: `tools/persistence_gates.py::check_profile_curve_format`. Tests: `tests/regression_main.cpp` codes 5060-5067 (marker decode, every host) and 5070-5089 (real INI round trip, Windows).

**Invariants**
- The loaders try per-point provenance FIRST and fall through to the legacy
  reconstruction only when the section is not `absolute_with_origin`; reversed,
  a new file would be re-flattened by the old code path.
- Absence of `pointN_from_gpu_offset` means "typed absolute". That default is
  the whole reason a hand-edited point survives a save.
- A point the loader did not populate never picks up provenance from a stale
  key: the flag belongs to a value.
- No save path may write `base_plus_gpu_offset` again (gated).

`format=explicit_vf_points_v1` is also a compatibility boundary. An explicitly
saved unlocked curve (`lock_ci=-1`) keeps its custom VF points. The old
"unlocked means stale readback" cleanup is restricted to pre-format profiles,
where that ambiguity actually exists.

Profile saving is intentionally sparse for VF curves: it writes only explicit user curve points and the locked tail. Missing `point*_mhz` entries are valid and mean "no explicit curve target" for that point. This prevents live NVAPI readback artifacts from becoming durable profile intent. Fan-only applies must preserve the GUI explicit-point mask and service active desired curve intent, so saving after a fan-only adjustment still writes pre-tail user points such as 74/75 plus the lock tail.

Saves are drift-free because the editor holds intent, not live readback (build 350). `save_profile_to_config` writes `desired->curvePointMHz` for owned points (`profile_curve_point_record_for_save()`; the former `saved_curve_point_mhz()` / `can_save_curve_as_base_plus_gpu_offset()` pair went away with the base+offset writer), and the GUI editor's owned points are now sourced from the drift-free `g_app.appliedCurveMHz` baseline rather than live `g_app.curve[]` (see [gpu-backend](gpu-backend.md) "Drift-free owned-curve baseline"). Consequence: editing only point 76 and re-saving leaves points 74/75 at their previously-saved values — the expected NVIDIA boost/temperature drift of untouched points is never persisted. (`point*_mv` is written from live state, but voltage is immutable on NVIDIA VF tables so it equals intent; `point*_mhz` is authoritative on reload for `format=explicit_vf_points_v1`.)

Locked profile load runs a narrow readback-artifact repair in `source/config_profile_repair.cpp`. When an old profile has `gpu_offset_mhz=0`, a flat saved tail, stock zero-offset scaffold points, and a single non-tail readback artifact before the explicit high pre-tail edits, the loader drops the scaffold/artifact and logs the repair. Ambiguous custom pre-tail edits are preserved.
Since 2026-09-24 it is skipped entirely for a section marked
`curve_semantics=absolute_with_origin`: that format is written only by builds
that save explicit intent, so its fixed-threshold rules (60/150 MHz gaps, a
60-250 MHz saved offset) could only ever delete a point the user shaped on
purpose. Tests 6470-6473 (legacy file still repaired, marked file untouched).

**INI text encoding (2026-09-24).** Every whole-file writer
(`write_config_sections_atomic`, and `write_config_text_atomic` used by profile
save/clear) encodes its UTF-8 text through `gc_utf8_to_ini_file_bytes()` into
the ANSI code page that `GetPrivateProfileStringW` decodes a BOM-less INI with,
and refuses characters that code page cannot hold. Profile save/clear rebuild
the file by reading every section back as UTF-8 and used to write it raw, which
re-encoded every non-ASCII value (auto-profile patterns) on every save. See
[auto-profiles](auto-profiles.md).

## Startup and logon

- **Logon profile handoff:** Registered as a Windows scheduled task with prefix `Green Curve Startup - `
- **Startup task names** include user-specific identifiers
- Automatic logon profile authorization is controlled by `[profiles]
  logon_slot` / `logon_shared_slot`. `[startup] start_program_on_logon` is
  tray-only. The legacy `[startup] apply_on_launch` value is retained when
  reading older configurations/task state, but it is not profile or hardware-
  write authorization for the service.
- Tray resident mode is registered separately in the current user's Windows
  Run key and launches `--tray-start` hidden. It is not a long-running action of
  the bounded handoff task.
- The scheduled task sends `SERVICE_CMD_LOGON_HANDOFF`, which contains no
  profile or GPU settings, then always exits. Independently, the tray Run entry
  may launch the resident GUI. The service derives the
  authenticated Windows-login identity, resolves the profile locally, and
  applies only after identity/profile/driver readiness is available.
- **`[profiles] logon_shared_slot` (per-user, references a SHARED BANK slot, 0 = unset):** "auto-apply admin shared profile N at my logon". It takes **precedence over `logon_slot`**, and every logon path resolves it to the admin's *authoritative bank copy*, so it passes the shared-only policy by construction. `should_enable_startup_task_from_config()` returns true when it is set (registers the startup task like `logon_slot`). Both whole-file rewriters (`save_profile_to_config`, `clear_profile_from_config`) must re-emit it — it is NOT cleared by clearing a per-user slot (it points at a bank slot).
- **Unified Logon dropdown (the per-account logon control):** the single **"Apply profile after user log in:"** combo (`hLogonCombo`, populated in `refresh_profile_controls_from_config`) is the one always-visible place to choose this account's logon profile. Each item is tagged via `CB_SETITEMDATA` (`LOGON_COMBO_SHARED_FLAG` in `app_shared.h`): index 0 = no personal choice (label shows the effective **"Use admin's default (Shared profile N)"** when a machine default is published, else "Disabled"), `1..N` = per-user `logon_slot`, `LOGON_COMBO_SHARED_FLAG|N` = admin shared slot N → `logon_shared_slot`. The handler (`LOGON_COMBO_ID` in `ui_main_window.cpp`) decodes `CB_GETITEMDATA` and sets `logon_slot`/`logon_shared_slot` mutually exclusively (one to 0). For `restricted_to_shared_profiles()` users the per-user slot entries are omitted (the service ignores a per-user `logon_slot` for them). There is **no** separate "apply at logon" entry in the "Shared profiles…" popup — that popup is read-only load only and points the user to this dropdown. The choice is strictly per-account (per-user config); the machine-wide default ("Share with all users") is the all-accounts fallback the dropdown surfaces and that a per-account choice overrides.
- **Atomic logon choice:** `logon_slot` and `logon_shared_slot` are rewritten
  together under the cross-session config lock, atomically replaced, the Win32
  INI cache is flushed, and both values are read back before task
  synchronization. Direct-file section replacement follows Win32's
  case-insensitive section semantics (`[Profiles]` and `[profiles]` are the same
  section), preventing a stale duplicate from winning readback. The combo is
  restored by tagged item data, never by assuming its numeric index, so shared
  entries survive a refresh. Asynchronous task/Run-entry repair is tagged with a
  generation; completion always re-reads current config on the UI thread and a
  stale generation schedules a current-state reconciliation instead of moving
  the combo or task back to an older choice.
  Task repair is a separate best-effort step: failure warns that event
  redundancy is degraded but never rolls back the saved choice.
- **One authoritative resolver:** `service_resolve_session_config_context()`
  captures immutable account paths/identity and
  `service_load_logon_profile_from_context()` resolves the configured choice to
  the protected personal/shared profile copy. The lifecycle worker revalidates
  that same context immediately before the write. The GUI/CLI scheduled-logon
  path is only an observer/handoff, so it cannot race the service with a second
  reset-before-apply request. A restricted user with a per-user `logon_slot` but
  no `logon_shared_slot` is **not** auto-applied; the service uses the
  machine-wide shared default or nothing.
- **Explicit choices fail closed:** if an eligible personal or shared explicit
  choice is temporarily unavailable, corrupt, or not yet materialized, the
  lifecycle intent remains pending. It never silently substitutes the
  machine-wide default. The default is considered only when there is no
  eligible explicit choice.

The logon-profile dropdown is the permission for automatic GPU settings at user
logon. `[startup] start_program_on_logon` only controls hidden tray residency;
it is not an automatic-apply setting. Selecting a logon profile therefore
creates the silent handoff task even when tray startup is disabled. An effective
machine-wide shared default also enables the account's task when its GUI next
synchronizes startup state. The
complete safety contract for logon, Fast Startup, standby, and driver recovery
is in [automatic restore policy](auto-restore-policy.md).

## Auto-profiles / hotkeys (config sections)

The auto-profile feature adds per-user `[auto_profiles]` + `[auto_ruleN]` +
`[hotkeys]` sections. Saving rewrites the global options, all active rules,
removes stale rule sections, and updates hotkeys in one cross-session-locked
atomic whole-file transaction. Loading the rules and hotkeys also uses one
locked generation. It automatically switches the active profile slot by
foreground process/window and binds per-slot global hotkeys. GUI-process only,
no injection. See [auto-profiles](auto-profiles.md) for the full design; the
switch reuses this page's profile-apply path with the
`desired_settings_match_active_service_intent()` idempotency skip.

## Sharing profiles with all users (Windows)

The shared bank lives at `%ProgramData%\Green Curve\shared-profiles.ini`
(`resolve_machine_config_path` → `FOLDERID_ProgramData`; no SCM command-line
parsing). It holds the published profile sections (`[profileN]`,
`[profileN_curve]`, `[profileN_fan_curve]`), each slot's stable GPU binding
(`[profileN_gpu]`), **and** the all-users default
`[profiles] logon_slot`. DACL: `SYSTEM`+`Administrators` Full, `BUILTIN\Users`
Read; the directory itself is hardened the same way
(`apply_protected_machine_config_dir_dacl`) so a non-admin cannot plant/delete
files. `is_elevated()` is also enforced in every writer. Verified live: `icacls`
shows `Users` `(RX)` on the dir / `(R)` on the file with **no inherited ACEs**
(PROTECTED), so `%ProgramData%`'s permissive default does not apply.

**Anti-squat (boot hardening) and the content proof (verified 2026-10-01):**
the default `%ProgramData%` ACL lets standard users create subfolders, so
`secure_shared_bank_at_startup()` runs at service start (SYSTEM, before any
interactive login) and calls `service_prove_shared_bank("startup")`: harden the
directory, require its exact protected DACL and Administrators owner through a
no-follow handle, then keep the file's bytes only if its PREVIOUS DACL was the
exact `GC_SERVICE_ACL_CONFIG` one with an Administrators owner and one link;
otherwise truncate through the pinned handle (`service_prepare_shared_bank_handle`)
and re-harden. A repaired ACL never authenticates bytes planted before install.

Scope of a failed proof (`g_sharedBankContentTrusted == 0`, service only; GUI/CLI
processes keep 1): `resolve_machine_config_path()` returns false, so every
CONTENT reader fails closed -- machine default, shared slots, the shared-only
policy (a non-admin APPLY is refused "policy temporarily unavailable"; logon
restore returns TRANSIENT), and the machine update settings (defaults). It does
NOT gate the update cache or staging directory, which resolve the folder via
`resolve_machine_config_dir()`. Until the 2026-10-01 review the flag gated path
resolution itself and was latched for the service lifetime, so one transient
failure (e.g. a sharing violation at boot) also broke the updater until a
restart and an administrator's later rewrite of the bank was never picked up.
Now the next content read re-runs the proof; reads are user actions (non-admin
APPLY, logon, update check), never a timer, and one log line per untrusted
period names the cause (`shared bank: <origin> proof FAILED (...)`).

The proof runs under the cross-process config lock, and every admin writer
holds that lock across write AND re-harden (`write_machine_config_int_hardened`,
the publication transaction, `service_update_save_settings`): a first write
creates the file with the folder's INHERITED ACL, and a proof landing in that
window would discard a legitimate write as unproven. Coverage: source gates in
`security_gates.check_audit_finding_gates`; the handle-level truncate/hard-link
rules in `tests/security_audit_tests.cpp`. The re-proof state machine itself
has no executing test (it lives in the service amalgamation). Stale-risk: not
exercised on a live service with a deliberately squatted folder.

### Coherent "Share with all users" (the headline action)

- **One action does both halves.** `share_profile_slot_for_all_users()`
  publishes the slot's **full data and selected stable GPU identity** into the
  bank AND sets it as the all-users
  default `logon_slot`. `unshare_profile_slot_for_all_users()` reverses both
  (clearing the default only when it points at that slot).
- **Why coupled:** the old "All users" button only wrote `logon_slot=N` without
  the `[profileN]` data, so the service resolved the slot but found it empty and
  applied nothing for restricted users. The service logon resolver
  (`service_load_logon_profile_from_context`) still requires
  `is_profile_slot_saved(machinePath, slot)`, which the coupled action
  guarantees.
- **Settings/GPU coupling fails closed:** publication holds the cross-session
  lock across source reads and target writes, invalidates all four slot
  data sections first while writing `[profileN_publish] state=publishing`, then
  writes settings plus `[profileN_gpu]`, verifies the identity by locked
  readback, and exposes the slot by committing `state=committed` last. A crash
  or partial failure therefore leaves the slot unavailable rather than pairing
  settings with a stale target. Clear removes data, binding, marker, and slot-1
  legacy aliases atomically.
- **GUI:** a labeled **"Share slot N with all users"** checkbox (bound to the
  *selected profile slot*) replaces the old "All users" button. Checking it
  shares (publish + default); unchecking unshares. Unelevated GUIs trigger a UAC
  `runas` for just this op (`run_elevated_command`). Right-click the checkbox for
  the **advanced** bank menu (publish/clear an individual slot *without* changing
  the default).
- **CLI:** `--share-slot <slot>` / `--unshare-slot <slot>` (coherent);
  `--set-machine-logon-slot` / `--clear-machine-logon-slot` and
  `--publish-slot-to-machine` / `--clear-machine-slot` remain as advanced
  primitives. All require elevation.

### Reading shared profiles on demand (any user)

- Any user (incl. restricted) can load the admin's published profiles **on
  demand**, not just at logon, via the **"Shared profiles…"** button. It reads
  the bank (Users:Read; no SCM/elevation needed), lists published slots, and
  `show_shared_profiles_menu()` loads the chosen one into the editor **read-only**
  (`populate_desired_into_gui`, marked dirty so Apply is enabled; the user's own
  `selected_slot`/config is untouched). The active session applies it via the
  service like any other Apply.

### Migration

- `migrate_legacy_machine_config()` runs once at service start (LocalSystem): if
  the old `<service-dir>\machine.ini` exists and the new file does not, it copies
  it to `%ProgramData%`, applies the DACL, and deletes the legacy file.

### Shared-only policy (restrict non-admins to shared profiles)

- An admin can set `[policy] restrict_non_admin_to_shared = 1` in the shared bank
  (`set_machine_restrict_policy`, admin-only; GUI: right-click the share checkbox
  → "Restrict standard users to shared profiles"; CLI: `--set-restrict-shared <0|1>`).
- **Enforced server-side** (the real boundary): in the `SERVICE_CMD_APPLY` handler
  the service checks the policy and whether the caller is a machine admin
  (`token_is_local_admin`, resolved from the caller's token incl. deny-only, so an
  unelevated admin still counts). A non-admin caller under the policy is only
  honored when the request carries `SERVICE_REQUEST_FLAG_SHARED_SLOT` + a slot
  (bits 8..15); the service then loads its OWN copy of that shared slot from the
  bank and applies it, **ignoring the client-supplied settings**. Any other apply
  is rejected ("Your administrator restricts this PC to shared profiles…").
  The service also ignores the client-supplied target GPU and resolves the
  authoritative `[profileN_gpu]` binding. Legacy slots without that section are
  accepted only when hardware proves there is exactly one adapter; malformed
  bindings and multi-GPU ambiguity fail closed and ask the admin to republish.
  Policy, profile, and binding reads share one cross-session transaction, so a
  concurrent admin publish/policy toggle cannot create a mixed authorization.
- GUI side: `show_shared_profiles_menu` sets `g_app.loadedSharedSlot`;
  `populate_desired_into_gui` clears it; `service_client_apply_desired` tags the
  request with the flag only when the editor holds the unmodified shared profile
  (`!guiHasUserModifiedValues`). This is purely an enablement signal — security
  is in the service applying its own authoritative copy. RESET (return to stock)
  is always allowed (safe direction). Admins and machines without the policy are
  unaffected. File permissions on the GUI binary are NOT relied on for this.
- **Logon auto-apply under the policy:** the policy is also enforced on
  the service-side logon path via the pure `resolve_logon_profile_source()`
  (`app_shared.cpp`), fed the session user's admin status
  (`service_session_user_is_local_admin` → `WTSQueryUserToken` +
  `token_is_local_admin`). A restricted user gets ONLY their `logon_shared_slot`
  (an authoritative bank copy) or the machine-wide shared default — their own
  per-user `logon_slot` custom OC is never auto-applied. This both lets a
  restricted user auto-apply a chosen shared profile at logon AND ensures the
  service router never applies per-user content without a policy check. Decision is exhaustively unit-tested in `build.py` (returns 140–149).
- **New/restricted account reliability:** machine/shared sources no longer
  require the logging-on account to already have a per-user config or `[gpu]`
  section. The resolver chooses the source first: a per-user source uses that
  account's `[gpu]`, while shared and machine-default sources use the GPU bound
  to the published slot. This fixes the fresh-account case where a valid admin
  default previously stayed pending forever because no user config existed.

## Service config

- Service install/remove via CLI (`--service-install`, `--service-remove`)

## GPU selection config

- `[gpu]` persists a versioned stable PCI identity (device/subsystem/revision,
  extended device ID, and PCI domain/bus/device when available) plus
  `selected_index` only as a compatibility hint.
- Startup resolves the stable identity against current enumeration, so adapter
  reordering does not redirect writes. Missing or ambiguous identity fails
  closed and requires explicit reselection. A legacy ordinal-only selection is
  accepted only when exactly one NVIDIA adapter is present.
- Service apply/reset requests carry the selected `GpuAdapterInfo`, so a stale GUI or mismatched service target cannot silently apply to a different active adapter.
- Published machine slots use the same serialized identity schema under
  `[profileN_gpu]`; one parser/formatter owns both formats.

## Source of truth

- `source/config_utils.cpp`: INI primitives, cross-session mutex, atomic
  logon-selection transaction, combo item-data mapping, and service-profile to
  personal-applied-slot mapping
- `source/win32_utf8_paths.h`: strict Unicode Win32 filesystem and INI/profile
  boundary wrappers
- `source/gui_mutation_worker.cpp` / `source/ui_mutation_completion.cpp`:
  background app-launch mutation transport and GUI-thread completion adoption
- `source/main_secure_write.cpp`: direct atomic file/section replacement,
  including case-insensitive INI section matching
- `source/config_profiles.cpp`: Personal/profile slot I/O and save/load/clear
- `source/config_profiles_machine.cpp`: Machine-bank publication/clear/share,
  per-slot GPU binding, locked verification, and fail-closed coupling
- `source/config_profile_repair.cpp`: conservative cleanup for legacy locked-curve readback artifacts
- `source/config_profiles_gui_state.cpp`: Profile-intent projection onto the editor (incl. per-field provenance), profile status/state labels, background-service controls, and the profile-row enablement gate (F-ACTIONABLE)
- `source/config_profiles_ui.cpp`: Profile UI refresh, atomic logon choice
  caller, item-data selection, and service-owned applied indicator
- `source/main_service_machine_config.cpp`: Shared-bank path resolution under `%ProgramData%` (`resolve_machine_config_path` → `FOLDERID_ProgramData`), `ensure_machine_config_directory` (+ dir DACL), `migrate_legacy_machine_config`, `get/set/clear_machine_logon_slot`
- `source/main_service_sessions.cpp`: immutable per-session config context and
  personal/shared profile resolution
- `source/main_service_lifecycle_events.cpp`,
  `source/main_service_lifecycle_apply.cpp`, and
  `source/main_service_logon_coordinator.cpp`: authenticated event routing,
  context/GPU revalidation, and serialized lifecycle application
- `source/service_lifecycle_policy.h`: pure named-profile transition, sparse
  full-restore construction, session identity, and lifecycle reducer
- `source/main_startup_task_definition.cpp`: canonical/compatible/broken task XML classification
- `source/main_startup_task_runtime.cpp`: task creation/repair and shared combo synchronization
- `source/ui_main_window.cpp`: "Share with all users" checkbox handler + `show_shared_profiles_menu` (read-only shared load)
- `source/service_acl.cpp`: shared-bank file + directory DACL helpers
- `source/app_shared.h`: Config constants, profile-related struct definitions

## Last Verified

- 2026-07-25 (build 441): `populate_desired_into_gui()` moved to
  `config_profiles_gui_state.cpp` and now stamps the per-field profile-load
  provenance the high-overclock confirmation exempts. Verified by compiled pure
  regressions plus source guards; runtime load/apply sequences not yet re-run on
  hardware.
- 2026-07-15: App-launch/profile presentation now consumes one accepted
  protocol-v13 READY envelope, preserves edits in the independent GPU/topology-
  keyed draft, and hands mutations to the generation-stamped coordinator; no
  runtime snapshot/active-desired request chain remains.
- 2026-07-13: Config/profile paths now use strict Unicode Win32 APIs while
  preserving UTF-8 internal storage and existing path limits. App-launch applies
  use the serialized nonblocking mutation queue. Compiled non-ANSI INI/path and
  queue-policy regressions plus source guards cover these boundaries.
- 2026-07-12: Published slots now bind settings to the administrator-selected
  stable GPU. Fresh accounts can receive a valid machine default without first
  creating per-user config; restricted manual applies use authoritative bank
  settings and target. Legacy missing bindings are single-GPU-only. Compiled
  round-trip, normal/ASan regression, Windows x64/ARM64 build checks, and the
  full release rebuild pass at build 397.
- 2026-07-12: Disabled app-start now refreshes the complete editor from the
  service snapshot. Merely selecting a saved slot cannot display its stored OC,
  lock, fan, or curve values as current state; explicit Load and enabled
  app-start behavior remain unchanged. Verified through build 395.
- 2026-07-11: The config mutex now spans GUI/service sessions with an explicit
  least-privilege DACL and fail-closed setup. Whole-file profile save/clear hold
  it for the complete RMW transaction; atomic logon selection flushes the INI
  cache, and mixed-case section headers are replaced rather than duplicated.
  Named profile selections replace ownership while cleaning up only previously
  Green Curve-owned omitted controls; ad-hoc sparse applies still merge.
- 2026-07-10: Logon task now sends an authenticated settings-free handoff;
  `logon_slot`/`logon_shared_slot` commit together and survive task repair
  failure; shared combo selection restores by item data. Applied ownership comes
  from service profile source/slot and is not cleared by normal VF drift.
- 2026-06-29: Added app-start active-service skip for `[profiles] app_launch_slot`: normal GUI launches no longer reset/reapply a profile when the service already has matching active desired intent. Verified with `python build.py --test` and full `python build.py` (build 323).
- 2026-06-19: Reworked into a coherent "Share with all users" action (publish + default in one), moved the shared bank to `%ProgramData%\Green Curve\shared-profiles.ini` with a hardened dir/file DACL + legacy migration, and added a read-only "Shared profiles" on-demand load for any user (Build 286). Verified `python build.py --test`/`--asan`/`--target linux --check`.
- 2026-06-14: Added machine-wide default logon profile and machine-wide profile bank (Build 284). Verified `python build.py --test`/`--asan`/`--target linux --check`.
- 2026-06-07: Verified lock-state unit merging and the pin-on-save fix against `source/config_profiles_ui.cpp` (`merge_desired_settings`), `source/main_runtime_gpu.cpp` (`capture_gui_config_settings`), `source/config_profiles.cpp`, and `source/linux_port_profiles.cpp`. `python build.py --test`/`--asan`/`--target linux --check` pass (Build 269).
- 2026-05-18: Cross-checked sparse VF profile saving, fan-only apply intent preservation, missing-curve-point load behavior, and locked-curve readback artifact repair against `source/config_profiles.cpp`, `source/config_profile_repair.cpp`, `source/main_runtime_gpu.cpp`, `source/main_service_runtime.cpp`, and `source/ui_main_window.cpp`.
