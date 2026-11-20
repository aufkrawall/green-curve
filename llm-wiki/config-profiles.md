# Config and Profiles

## Advanced fields omitted from a scoped profile (2026-11-20)

`config_profiles_gui_state.cpp` projects the current advanced values for
omitted fields on explicit-scope Load. The per-field profile projection flags
keep these controls out of Apply and Save overlays even if live values drift.
`xbar_dialog.cpp` clears each flag only when its displayed value is edited.
`tests/gui_editor_base_tests.cpp` covers an untouched overlay and an explicit
edit of one field with its three siblings still omitted.

## Profile tools: capture, import and export end in ordinary slots (2026-11-19)

Source anchors: `source/profile_slot_runtime.h`, `source/profile_legacy_flat_tail_offset_policy.h`,
`source/config_profile_document_load.h`, `source/config_profile_repair.cpp`,
`source/linux_profile_document_load.h`, `source/profile_capture_save.h`,
`source/profile_summary.h`, `source/profile_editor_transfer.h` (probe and explicit
selection), `source/profile_transfer_slot_policy.h`, `source/gui_profile_tools.cpp`,
`source/gui_profile_capture.cpp`, `source/gui_profile_transfer.cpp`,
`source/gui_profile_dialog_kit.h`, `source/gui_profile_transfer_work.h`,
`source/profile_ini_slot.h`, `source/linux_tui_capture_draft.h`,
`source/linux_tui_transfer.h`, `source/linux_tui_layout_transfer.h`,
`tests/profile_slot_runtime_tests.cpp` (9950-9969, 12500-12504, 12600-12622),
`tests/profile_tools_tests.cpp` (9970-9996), `tests/tui_profile_tools_tests.cpp`
(9770-9793), `tests/profile_ini_slot_tests.cpp` (9930-9948).

This supersedes the source-editor workflow described in the 2026-11-14/15 sections
below ("Windows capture into an unsaved source draft", "Windows canonical source
editor", "Windows read-only destination checks", "Reviewed source submission",
"Windows/TUI reviewed profile transfer"). Those sections remain as history of the
engines that still exist (CLI `--check-profile`, the passive capture collector,
the transfer engine); their Windows windows are gone.

Why: import and capture stored portable documents in the bank, and every slot
reader (editor Load, tray, hotkeys, auto-switch, app start, logon, sharing) refused
them, so an imported profile could only be applied through a separate source window
(Check, Use mapped, Apply, every time). The dialogs also exposed the engine's state
machine (domain bits, representation names, consent checkboxes).

Current behavior:

- **Every slot reader converts documents.** `profile_slot_document_runtime()`
  turns a slot document into the runtime record. Position-based records (exports
  of ordinary profiles and old INI files; `legacyIndicesUnresolved`) reverse
  `profile_document_from_legacy_desired()` without a GPU; a record with nothing
  only an explicit scope can carry stays legacy-scoped, otherwise it keeps an
  explicit scope (the service refuses precise/captured intent in legacy scope).
  Voltage-bound records map by exact voltage onto the live curve
  (`profile_destination_from_vf_curve()`, fixed floor points read-only) and refuse
  the whole record with a plain reason otherwise (needs the GPU / different GPU /
  fixed point). Windows uses `g_app.curve`; the TUI Load and startup-policy bind
  use the shown daemon snapshot; Linux CLI (`--dump`, `--json`, `--apply-config`,
  `--startup-profile`) load
  position-based slots offline and fall back to fetching the live daemon VF
  topology once (`linux_cli_profile_load.h`) when a slot reports `needsLive`,
  resolving the configured GPU target lazily only after `needsLive` is confirmed
  (`requireWriteTarget` enforces `linux_gpu_target_for_write()` for mutating
  verbs and `linux_gpu_target_for_read()` for read-only verbs so offline
  ordinary profiles never fail on unrelated GPU config).
  Startup status uses the voltage table in the same policy response only when
  the boot resolver identifies the selected GPU. Otherwise the comparison is
  unknown. A voltage-mapped refresh checks the same GPU, generation, service
  instance and topology again when the daemon accepts the new settings.
  `linux_main.cpp` also defers slot materialization for verbs that do not need a
  configured profile (`--tui`, `--probe`, `--reset --apply-config`), reading
  only `selected_slot` via `load_selected_profile_slot()` so a voltage-bound
  selected slot never blocks offline startup or recovery. Purely offline callers
  with no reachable daemon still refuse voltage-bound slots cleanly.
- **Legacy flat-tail offset-artifact repair (2026-11-20).** Profiles saved
  before live VF offset detection excluded unowned external programmed flat
  tails (`vf_live_offset_policy.h`) could record a large negative graphics
  offset below `-1000 MHz` (e.g. `-1927 MHz` with `exclude_low_count >= lock_ci`)
  alongside an explicit `FLATTEN` lock, an absolute flat tail matching
  `lock_mhz`, and zero projected points. `source/profile_legacy_flat_tail_offset_policy.h`
  recognizes this exact dead artifact (`< -1000 MHz`, `FLATTEN` lock with
  absolute tail points above `lockCi`, zero projected/captured points, and
  either `excludeLowCount >= lockCi` corroborated by any recorded tail observed
  offsets or `excludeLowCount == 0` with zero pre-tail points, at least four
  matching observed tail offsets within `30 MHz`, and an observed anchor offset
  not matching the tail delta) and neutralizes it to stock `0 MHz` (`exclude=0`,
  `lockTracksAnchor=0`) identically across `profile_desired_from_indexed_document()`,
  Windows ordinary INI load (`config_profile_repair.cpp` before IPC clamping),
  Linux ordinary INI load (`linux_profile_document_load.h` /
  `linux_port_profiles.cpp`, skipping `PROFILE_CURVE_DECODE_BASE_PLUS_GPU_OFFSET`
  records and reading `pointN_offset_khz` evidence identically to Windows),
  and `profile_document_summary()`, with one-line
  repair diagnostics. All genuinely out-of-range offsets (no flatten lock,
  `excludeLowCount < lockCi`, projected points, hard lock, positive values, or
  contradictory/missing uniform proof) continue to be refused with exact MHz
  and bounds.
- **The legacy adapter is exact now.** Precise graphics/point/lock Hz export as
  such, and projected points keep their saved MHz as a labeled observation, so an
  export/import round trip reproduces the slot.
- **Import current GPU settings** (Windows dialog, TUI modal) reads all groups except
  fan on open (`PROFILE_CAPTURE_DEFAULT_DOMAINS`), reviews with partial allowed, and
  shows one row per group (`profile_capture_summary()`). Unreadable groups cannot be
  included; advanced clocks appear only when the GPU has them. The curve choice lists
  only selectable representations, suggested first. Tier 3 pin detection is a
  secondary button shown only when observable; a suspected pin is confirmed at the
  sampled frequency and preselected. Save builds the record with
  `profile_capture_record()` (left-out groups dropped from the document, mapped onto
  the snapshot's own topology, legacy scope promoted to an explicit one so a slot
  save cannot refill omitted groups from live values) and writes it with the
  ordinary slot writer. It never touches the editor draft, the GPU or ownership,
  so no discard decision exists. A changed GPU session rereads (Windows) or expires
  the snapshot (TUI).
- **Export** ticks saved slots (summaries via `profile_document_summary()`), then a
  native Save As (Windows) or a typed path with overwrite confirmation (TUI); the
  job prepares and commits in one worker run. The TUI uses an atomic no-replace
  rename for its first attempt. Empty and oversized destinations also prompt.
- **Ordinary INI slots decode as their runtime record** (`profile_ini_slot.h`, used
  by export, the slot summaries and INI import). Base keys go through the historical
  decoder; the keys newer slots add (`application_scope`, precise Hz, captured
  points, `topology_bound` voltages, identity exclusions) are applied afterwards by
  the loader's own extension reader, and the record is described with
  `profile_document_from_legacy_desired()`. A voltage-bound record exports by its
  saved voltages (`legacyIndicesUnresolved` false), so an import maps it exactly or
  refuses, as the record does at Apply. For this machine's own slots
  (`ProfileLegacyOptions::nativeSlot`) a curve without `curve_semantics` uses the
  slot loader's rule: Windows reads base clocks plus offset only when the stored
  lock point plus the offset equals the lock frequency and the stored value alone
  does not, otherwise final clocks; Linux keeps the offset and drops the points.
  A file being imported still asks. The `gpu_offset_exclude_low_count` copy that
  Windows writes into the curve section is accepted and must equal the control
  section's value (the loaders read only that one).
- **Import** keeps the reviewed source bytes through commit. It reads the file first (`profile_editor_transfer_probe()`): portable banks
  list records in file order; anything else is tried as an old INI (Windows
  interpretation by default, switchable), slot by slot, and a missing memory unit or
  curve meaning becomes a question instead of a failure. Default destination slots
  come from the shared `profile_transfer_default_slots()` policy
  (`source/profile_transfer_slot_policy.h`, used by both Windows GUI and Linux TUI):
  when all records fit in empty slots no saved slot is overwritten (keeping distinct
  empty preferred slots or assigning ascending empty slots); when some saved slots
  must be replaced, each record first claims its own empty preferred slot so a
  multi-profile import (e.g. 5 profiles with only slot 5 empty) never swaps Profile 1
  into slot 5 and Profile 5 into slot 1, and any remaining empty slots are matched
  monotonically to the closest unassigned rows. Duplicates block Import; replaced
  slots are named. Both Windows (`gui_transfer_import_note()`) and Linux TUI
  (`tui_transfer_refresh()`) pre-check every checked record (both indexed and
  voltage-bound) via `profile_slot_document_runtime()` before committing, so records
  with out-of-range offsets (e.g. a legacy `-1927 MHz` offset outside
  `-1000..1000 MHz`) or destination mismatches are named before Import with exact
  MHz and bounds. The job prepares the explicit `sources`/`slots` edit and commits
  it in one run; the bank generation check still refuses a bank that changed
  meanwhile.
- **Windows plumbing.** The three dialogs share `gui_profile_dialog_kit.h` (house
  themed buttons/checkboxes/combos, DT_NOPREFIX, centered, main window disabled,
  Esc/Enter/Tab routing). Transfer jobs are `GUI_PROFILE_TRANSFER_PROBE` or `_RUN`;
  only a job that has not started can be cancelled, and the dialog cannot close
  during a write. An unmatched passive-read completion is freed by the main window.
  `g_app.guiSource`, the Profile source window and the mapped-source Apply path are
  removed; Apply always submits the editor draft.
- **The TUI edits ordinary slots only (removed 2026-11-19).** The source-document
  mode (draft editing, Check, mapped draft, conversion, tail restore, curve
  assistance, decimal-MHz field input) was unreachable once Load converted slots
  and capture/import stopped staging drafts, and was deleted with its tests. What
  stays: Load (`profile_slot_document_runtime()`), Save, ordinary field editing,
  the capture modal, the file dialogs and the frozen-candidate replacement
  confirmation (`linux_tui_profile_load.h`, `DesiredSettings` candidate). Shared
  engines kept for the CLI and Windows capture review: `profile_check_commands.h`,
  `profile_editor_check.h`, `profile_mapped_draft.h`, `profile_destination_review.h`.
  `profile_editor_document.h`, `profile_editor_source_edit.h` and
  `profile_editor_source_rows.h` had no other user and are gone. Tests moved with the
  engines they cover: `tests/profile_editor_check_tests.cpp` (9230-9274),
  `tests/profile_mapped_draft_tests.cpp` (9630-9680, 9870-9874, no UI state),
  `tests/profile_document_slot_tests.cpp` (9210-9226), `tests/tui_capture_context_tests.cpp`
  (9757-9760) and the capture half of `tests/tui_review_lifetime_cases.h`. Gate:
  `check_canonical_tui_documents()` in `tools/ui_gates.py` fails if the deleted headers or
  any source-mode token return to the TUI files.

Invariants: no dialog writes hardware; every saved result is an ordinary slot or a
document every reader converts; a refusal names what the user can do; probe,
capture and import diagnostics log one `profile-capture:` / `profile-transfer:` line
per event without paths or document bodies.

Diagnostics / failure modes: the native decoder used to reject the curve-section
exclusion copy and every unmarked curve, so export (dialog and CLI) failed for
ordinary Windows slots with a core offset and the Export dialog showed "Saved
profile" without a summary; a slot whose summary cannot be built now logs
`profile-transfer: slot N summary unavailable: <reason>`. Verified 2026-11-19 by a
CLI export of real slots followed by import into a scratch config and a
byte-identical re-export. The repair pass of the Windows loader
(`repair_profile_locked_curve_readback_artifacts`) is deliberately not mirrored,
so an export describes the stored record, not a repaired one.

Open questions / stale-risk: the Windows Profiles menu, capture dialog and Export
dialog were opened on a desktop (layout and live read confirmed); the Import
dialog pass and physical GPU replay of captured records are not recorded here.
The TUI file dialogs and capture modal are covered by scripted tests only, not by
a terminal session on a GPU host.

## Editor Apply and Save build on a loaded explicit record (2026-11-19)

Source anchors: `source/gui_editor_base_policy.h`, `source/gui_editor_base.cpp`,
`source/main_runtime_capture.cpp` (`gui_editor_current_scalars()`,
`capture_gui_apply_settings()`, `capture_gui_config_settings()`),
`source/gui_profile_commands.cpp`, `source/ui_main_window.cpp` (typed flag),
`source/ui_pending_changes.cpp` (`GUI_PENDING_PROFILE`),
`tests/gui_editor_base_tests.cpp` (12320-12333), `tools/ui_gates.py`
(`check_editor_builds_on_its_base_record()`).

- **Why.** The editor holds whole-MHz fields and owns a point only through an
  explicit MHz. Records with an explicit application scope (Import current GPU
  settings, imported profile files) also carry captured per-point offsets, exact
  Hz for lock/memory/points, power ratios and a topology binding. Load->Apply
  and Save rebuilt the request from the fields, so captured points went back to
  stock, the request fell back to legacy scope and ownership read "Manual
  settings". Tray, hotkey, logon and app start sent the record itself.
- **Base record.** `g_app.guiEditorBaseMode`: Load, shared Load, Save and the
  tray/app-start "already active" populate attach the slot record
  (`gui_editor_base_attach()`); an explicit record becomes the base, a legacy
  one means no base. Otherwise the base is the service's active intent when it
  is explicit (a tray-applied import, or an earlier Apply of a loaded one), so
  later edits keep its captured points too; before, pre-tail retention put
  those captured points into a legacy request the service refuses. A clean or
  discarded draft resets to the active intent.
- **Overlay.** `gui_editor_edits()` decides per domain: an owned domain is
  edited when the field differs from the record's value, an omitted one when it
  differs from what the GPU runs. Advanced fields also require a user edit
  once projected by Load. A point is edited only when the user typed it
  (`g_app.guiCurvePointTyped`, set in the VF field EN_CHANGE handler, cleared by
  every projection), so projected values and an unchanged lock's tail never
  count. An edited lock takes the editor's lock and every point from the lower
  of the two anchors. Edited domains drop the record's exact Hz, captured
  offset or ratio; the rest keep it. An invalid result is refused with a reason,
  never sent as something else.
- **Apply.** A full Apply sends the overlay through
  `desired_prepare_replacement_request()`, as the tray does, so an unedited
  loaded slot equals the tray request byte for byte and is claimed as that
  slot. A loaded record the service does not run forces a full Apply
  (`GuiApplyChangeSet::loadedProfile` in `gui_apply_shape_policy.h`) and sets
  `GUI_PENDING_PROFILE`, so Apply stays enabled when no field differs. Sparse
  power/fan Applies over an active explicit intent are unchanged (the service
  upgrades their scope). Legacy records keep the legacy request path.
- **Save** writes the overlay with the record's own scope.
- **Diagnostics.** `editor base: <why> slot=N mode=... scope=... captured=...`
  on attach, `editor base: reset from ...`, `editor base: apply|save on <mode>
  base ... edits={gpu=.. mem=.. ... lock=.. lockFrom=.. points=..}`, and
  `... overlay refused ...: <reason>`.
- **Open questions.** The fields still show live values for captured points
  until Apply (a field cannot show an offset). Verified by fixtures and a
  Windows compile; not yet driven on hardware.

## Capture memory from the running clock; loaded locks reach the editor draft (2026-11-19)

Source anchors: `source/memory_clock_evidence.h`, `source/hardware_capture_getters.h`
(`hardware_capture_apply_memory_clock()`, `HardwareCaptureContext::memoryClockReader`),
`source/main_service_capture.cpp`, `source/gpu_backend.cpp` (`nvapi_read_pstates()`,
`detect_clock_offsets()`), `source/gui_draft_desired_policy.h`,
`source/gui_service_state.cpp` (`gui_draft_capture_desired()`),
`tests/capture_memory_clock_tests.cpp` (12306-12319),
`tests/gui_draft_desired_tests.cpp` (12300-12305), `tools/ui_gates.py`
(`check_loaded_lock_reaches_the_draft()`).

- **Memory: one rule for live state and capture.** The Windows live state reads
  the memory offset as the highest Pstates20 memory clock minus the board maximum
  (`nvmlDeviceGetMaxClockInfo`). Passive capture read only the NVML offset getter,
  which stays 0 when another tool programs memory through Pstates20 (observed:
  running maximum 17001 MHz vs board 14001 MHz, getter 0), so Import reported
  memory as stock and the slot saved 0. Capture now takes the same running-clock
  reading (`memory_clock_read()`, getters only) and reports it when it disagrees
  with the getter by a whole MHz; a getter within that resolution keeps its finer
  value. The value stays on the getter's surface, the control replay writes; an
  NVML write replaces a Pstates20 offset (a reset to 0 brought the running maximum
  back to the board value). Without a usable getter the domain stays unread.
  `memoryEvidence` (capture schema 5) records `getter`, `agrees` or
  `running-clock`. Linux does not set the reader: its live state reads NVML only.
- **Loaded lock reaches the draft.** Apply takes the lock target from the draft's
  anchor value (`capture_gui_desired_settings()`), and Load rebuilds the draft from
  the applied/live curve (`set_gui_state_dirty(true)` ->
  `gui_draft_capture_clean_projection()`) before copying the record in. That copy
  handled explicit curve points only, so a record whose lock owns its anchor
  without an explicit point (a captured or imported flatten) kept the live anchor
  clock: the reported slot held `lock_mhz=2962` and Apply sent 2482 MHz. The copy
  now writes every value `gui_draft_desired_point_mhz()` declares: explicit points
  and the lock anchor at the lock target, which wins over an explicit anchor point
  as it does in the field.
- **Diagnostics.** Service: `capture memory: getter=... running=... board=... ->
  ...Hz evidence=...` once per changed reading; the capture summary line carries
  `memory=<Hz>(<evidence>)`; the CLI report adds a line when the running clock
  replaced the getter. GUI: `GUI draft: lock anchor ci=N set to the record's lock
  M MHz (was ...)`.
- **Open questions.** Hardware verification with another tool's memory offset
  is pending. The editor Load->Apply gap first noted here is closed by the
  section above.

## Capture: curve-only judging and load-free driver locks (2026-11-19)

Source anchors: `source/profile_capture_proposals.h` (`profile_capture_judge_curve()`,
`profile_capture_suggest()`, `profile_capture_point_at_voltage()`),
`source/profile_document_domains.h`, `source/hardware_capture_aux.h`,
`source/nvapi_client_lock_table.h`, `source/hardware_capture_getters.h`
(`hardware_capture_apply_client_locks()`), `source/profile_summary.h`
(`profile_capture_lock_text()`), `tests/capture_curve_choice_tests.cpp`
(12260-12264, 12268-12272), `tests/capture_aux_writable_tests.cpp` (12265-12267),
`tests/capture_client_lock_tests.cpp` (12273-12289).

- **Curve-only judging.** Exact, flatten and pin differ only in the curve and the
  lock; memory, power, fan and the advanced clocks are carried identically and
  are separate rows. Each representation is therefore judged on a copy with the
  non-curve domains dropped (`PROFILE_DOCUMENT_NON_CURVE_DOMAINS`); the saved
  document keeps every domain. Before, the GUI/TUI default domains included the
  advanced clocks, whose capture scalars carried no setter flag, so every
  flatten/pin proposal was "unavailable" and only the exact copy was offered.
  The CLI's default domains (no advanced clocks) hid it.
- **Advanced-clock writability.** Their capture scalars report `writable` when the
  ClkDomains setter entry point resolves, like the NVML scalars (entry point, not
  capability proof).
- **Recommendation.** A selectable flatten (ready or conditional) without competing
  pin evidence is preselected; a pin from the driver table or Green Curve's applied
  intent is preselected when it agrees with any recognized flatten. Conflicts,
  workload observations and no usable lock leave the exact copy preselected. A
  flatten is only ever recognized on a shaped curve, whose live baseline cannot
  prove the plan, so the old "representable only" rule practically never fired.
- **Exact copy check.** Exact replay is judged too (`exactControls`). Other tools
  flatten by pushing every tail point far below the reported offset range (one
  observed capture: -2045 MHz against a +/-1000 MHz range) and let the driver's
  monotonic rule hold the tail at the anchor; Green Curve would refuse that exact
  copy at Apply, so its choice reads "cannot be applied on this GPU".
- **Load-free lock evidence (Tier 1).** Passive capture reads the driver's client
  lock table (see [gpu-backend.md](gpu-backend.md#reading-locks-held-by-any-client-2026-11-19)).
  A fixed clock lock (floor = ceiling) becomes a pin proposal; a VF voltage lock
  becomes a pin at the frequency of the curve point with exactly that voltage,
  rounded down to whole MHz (never above the point); a voltage lock that names no
  point, a voltage lock next to a clock limit, a ranged limit and a ceiling with
  no floor stay unselectable with a reason. Rows say "locked at 925 mV (2482 MHz)",
  "clock locked/capped at N MHz". The workload "Detect clock lock under load"
  action appears only when the table could not be read.
- **Diagnostics.** GUI/TUI log `profile-capture: proposal=flatten|pin|exact ...`
  after each review; the service/daemon log `client-locks: known|unknown ...`
  once per changed reading; the capture summary line carries
  `lockStatus/clockMax/clockMin/voltageUv`.
- **Open questions.** The table is validated on one Blackwell board and driver
  (Windows); other generations, Linux NvAPI and memory locks (presumably entries
  2/3) are unverified, and any unobserved entry makes the reading unknown. The
  VF per-point offset range is still NVML's graphics range although the driver
  accepted -2045 MHz per-point offsets; widening it is a separate decision.

## Capture lock proposals (2026-11-15)

Source anchors: `source/profile_capture_proposals.h`,
`source/profile_capture_commands.h`, `source/vf_flat_tail_recognition.h`,
`source/profile_commands_windows.h`, `source/profile_commands_linux.h`,
`source/linux_tui_capture_binding.h`, `source/profile_source_window.h`,
`tests/profile_capture_proposals_tests.cpp`, `tests/tui_capture_proposals_tests.cpp`.

Windows, the Linux TUI and CLI offer exact/flatten/pin representations after a passive
snapshot report. Recognition uses the existing ascending-voltage tolerant
helper over complete graphics topology, retaining hidden writable anchors and
the bound read-only floor. It never reconstructs stock from a shaped curve.
Every proposal runs through runtime mapping and the complete shared control/
curve planner. Proven refusal and unavailable constraints keep exact controls;
conditional proposals retain their need for fresh operation checks. The
preselection rule here was superseded on 2026-11-19 (see
[Capture: curve-only judging](#capture-curve-only-judging-and-load-free-driver-locks-2026-11-19)).
No representation is implicitly accepted by the CLI.

Flatten/pin conversion removes authoritative tail values while retaining exact
observed offsets and frequencies in the portable document. Pre-anchor intent,
owned zero, microvolt identities and raw exact replay stay intact. Serialization
therefore retains the data needed for a later return to exact controls.

Pin evidence prefers a validated fixed-range getter, then service-owned applied
intent explicitly labeled as not read from the driver. A getter reporting no
pin or a non-fixed range cannot borrow an older applied value. Current clocks
and a flat table never supply a pin. The retained intent is accepted only from
the same generation and opaque ownership-stamped capture revision; missing
snapshot capability is unknown, while known absent hard-pin control keeps the
pin proposal unresolved. Linux passive replies deliberately omit ordinary
snapshot assembly and may publish DEGRADED; a successful coherent capture can
still retain labeled applied intent. This grants no write authority or readback
proof. Equal pin/flatten targets describe combined hard-pin intent; different
targets require an explicit choice. A clock pin does not guarantee voltage.

Cases 9680-9715 cover hidden anchors, raw/converted round trips, evidence tiers,
unknown pins, source preservation, conflict/availability/grid/range failures,
128 graphics points, fallback family support and CLI confirmation/refusal.
Native controls use frozen proposal documents, with independent omission and
unfinished-draft confirmations. Unavailable choices cannot be accepted, including
forged button events. The TUI expires the capture on service instance,
generation, topology, ownership, target or readiness drift while preserving the
previous source and unfinished input. Telemetry-only updates keep it usable.
Acceptance checks newly dirty input again before replacement. Both frontends
clear cached proposals on cancellation or a new read. Cases 9720-9763 cover
production controllers, transport context, DPI, keyboard and compact layouts.
User-started bounded workload observations are documented under
[User-started workload pin observation](#user-started-workload-pin-observation-tier-3-2026-11-16).
Physical replay and optional pin getters remain unverified.
The Windows ASan/UBSan suite, native Linux UBSan fixtures, all-target builds and
source gates pass; static analysis adds no findings to its 34-entry baseline.
Replacing missing pin evidence with a guessed value fails case 9680.

## Reviewed source submission (2026-11-15)

> Superseded 2026-11-19 for the Windows and TUI frontends: see
> [Profile tools](#profile-tools-capture-import-and-export-end-in-ordinary-slots-2026-11-19).
> Kept as history of the engines that remain.

Source anchors: `source/profile_mapped_draft.h`,
`source/gui_profile_source_review.cpp`, `source/ui_main_apply_dispatch.h`,
`source/gui_mutation_request_binding.h`,
`source/service_control_review.h`,
`tests/profile_mapped_draft_tests.cpp`, `tests/gui_profile_source_review_tests.cpp`.

Check passively reads all domains, including preserved and cleanup controls,
so its opaque service revision can bind a complete scoped operation. Use mapped
draft is a separate local choice. It retains the source document and exact runtime
units, creates no ownership/save acknowledgment and performs no service write.
Representable/conditional reviews whose runtime adapters match now execute
through active exact authoring and captured-offset runtime paths.

Windows and TUI Apply submit the frozen runtime value with that complete
control token through their existing transport/coordinator. The service checks
the token in its runtime lane before planning any mutation. Source revision,
metadata, GPU session, ownership and unfinished input can invalidate staging.
Windows rechecks after modal confirmations and retains manual high-clock and
unified-memory confirmations. A reviewed source is ad hoc: an earlier loaded
shared slot cannot replace its payload or grant shared-bank authority. The
abandoned numeric draft neither authorizes nor blocks a separately bound source;
service readiness and in-flight gates still apply. TUI acceptance is consumed
by one submission, including an unknown outcome, to prevent accidental retries.

Cases 9630-9677 execute shared acceptance, exact scalar payloads, stale controls,
native window action guards, production Windows dispatch and mutation binding,
confirmation cancellation/reentrancy and compact TUI layout. Removing focused
button Enter handling fails native case 9673. Capture/transfer/load modals and
incomplete READY envelopes cannot expose TUI Apply. Imported source
intent stays inspectable/saveable after refusal or expired review. Physical GPU
replay is not established by these scripted checks.
Windows normal/ASan/UBSan suites, all-target builds and native Linux UBSan
fixtures pass. Static analysis retains 34 baseline findings with no additions.
The mapped-source pane was rendered offscreen and inspected; its helper exited
without leaving a window or process behind.

## Windows reviewed profile transfer (2026-11-15)

> Superseded 2026-11-19 for the Windows and TUI frontends: see
> [Profile tools](#profile-tools-capture-import-and-export-end-in-ordinary-slots-2026-11-19).
> Kept as history of the engines that remain.

Source anchors: `source/gui_profile_transfer.cpp`,
`source/profile_transfer_window.h`, `source/profile_transfer_picker.h`,
`source/gui_profile_transfer_work.h`, `source/gui_profile_transfer_queue.cpp`,
`source/gui_mutation_worker.cpp`, `source/ui_main_window.cpp`,
`tests/gui_profile_transfer_tests.cpp`.

Profiles & tools remains available offline. Its transfer pane offers native
file selection or exact UTF-8 path entry, record/bank selection, explicit
destination slots and legacy interpretation. Single-record import initially
requires a destination choice. Preparation uses the same frozen preview engine
as the TUI. The review names the file, replaced slots, domains, scope and
affected assignments before confirmation.

The existing coordinator owns one pending file job and serializes it after
mutations, administration, full sync and passive capture, before telemetry.
Only immutable options, paths and owned preview data cross the thread boundary.
File preparation/commit never runs on the window thread and never performs a
service operation. A job has exactly one owner: frontend, queue, worker or a
locked completed-result slot. Wake messages own no object. The normal GUI
message route also drains that slot, so a failed wake cannot lose the result
or strand a confirmed commit. Shutdown clears retained jobs under the same lock;
stale results are freed on adoption. Matching cancellation
removes queued preparation only; an explicitly confirmed commit cannot be
cancelled or closed halfway through. Existing atomic storage remains decisive.

Main and source windows remain disabled while this pane is open. Their source
model, raw unfinished input and selection survive cancel, failure, export and
import-only confirmation. Refreshing slot labels restores the previous selector
unless the user separately chose to load the first imported document. Loading
requires an independent discard decision when dirty, rechecked before enqueue;
it creates a saved source draft and grants no Apply authority. Ordinary hardware
menu actions stay unavailable offline while file tools remain usable.

The pane supports DPI changes, virtual scrolling, focus reveal, exact Unicode
paths and caret preservation. Each review defaults focus to Cancel. Cases
9510-9541 execute the production controller and native controls with scripted
file jobs: late replies, one/five-profile imports, frozen export, conflicts,
independent decisions, Unicode paths and DPI/focus behavior. Native file-picker
COM lifetime is balanced; production builds verify its existing system links.
Failure to open the source editor after a successful bank commit keeps the old
draft and unfinished input. The options and review panes were rendered offscreen
and inspected; no test application remains running after these bounded checks.
Enter explicitly activates focused custom action buttons; ordinary path input
cannot confirm a transfer. Default Enter cancels a newly opened pane.
Focus notifications cannot cancel an active preparation. A scripted missing
wake retains a finished commit until the GUI drains it, without retry delays.
Full Windows normal/ASan/UBSan, all-target builds and source gates pass. Static
analysis retains 34 baseline findings with no additions. Removing discard,
keyboard or focus guards fails cases 9523, 9538 or 9540 respectively.
Interactive picker use and physical GPU replay are separate verification gaps.

## TUI reviewed profile transfer (2026-11-15)

> Superseded 2026-11-19 for the Windows and TUI frontends: see
> [Profile tools](#profile-tools-capture-import-and-export-end-in-ordinary-slots-2026-11-19).
> Kept as history of the engines that remain.

Source anchors: `source/profile_editor_transfer.h`, `source/profile_commands.h`,
`source/profile_slot_io_linux.h`, `source/profile_slot_io_windows.h`,
`source/linux_tui_transfer.h`, `source/linux_tui_layout_transfer.h`,
`source/linux_tui_actions.cpp`, `source/linux_tui_render.cpp`,
`tests/profile_editor_transfer_tests.cpp`, `tests/tui_transfer_tests.cpp`.

Transfer is available beside Capture with both legacy and canonical drafts,
including the minimum terminal size. File selection accepts exact UTF-8 paths;
incomplete sequences and terminal controls are escaped for presentation only.
The editor's numeric input buffer is separate and survives opening/cancelling
the modal, failed previews, conflicts, exports and import-only confirmation.

The shared preview engine reuses CLI parsing and bank preparation through
virtual output sinks. Preparation performs no storage write or commit and
reads imported file bytes once. Single-profile imports select a file ordinal
and destination slot. Bank imports map records to slots 1..N in one prepared
edit, preserving other slots, preferences and assignment references. Legacy
format, memory units and curve semantics remain explicit choices.
Import initially chooses the first unused section; a full or unreadable bank
requires a destination choice before single-record preparation.

Confirmation consumes exactly the frozen edit or export bytes. Import uses
the existing platform lock, exact configuration-generation comparison and
atomic replacement. Conflict/write failure consumes the preview; retry needs
a new review. No GPU or daemon operation participates. Existing daemon startup
snapshots remain unchanged, and their cached comparison becomes unknown.

Loading the first imported document is a separate choice. Replacing a dirty
or unfinished draft requires an independent discard decision, rechecked at
commit. Imported source precision and metadata stay intact; loading does not
grant Apply authority. Changing preparation choices invalidates the preview.
An unseen or undersized modal accepts only cancellation, and each newly
presented phase initially focuses Cancel.

Cases 9440-9491 execute the shared engine and TUI controller with in-memory
storage, exact units, five-slot imports, frozen bytes, stale generations,
Unicode input, dirty-draft decisions, safe path display and bounded layouts.
The production adapters remain shared with the existing CLI file fixtures.
Focused ASan/UBSan tests and full Windows normal/ASan/UBSan regressions pass;
removing the discard guard fails case 9465. Native Linux UBSan execution and
all-target checks pass. Static analysis reports no additional findings.
Windows uses the reviewed transfer pane above. Destination acceptance remains
separate pending work.

## Windows capture into an unsaved source draft (2026-11-14)

> Superseded 2026-11-19 for the Windows and TUI frontends: see
> [Profile tools](#profile-tools-capture-import-and-export-end-in-ordinary-slots-2026-11-19).
> Kept as history of the engines that remain.

Source anchors: `source/gui_profile_capture.cpp`,
`source/profile_source_window.h`, `source/gui_passive_capture_queue.cpp`,
`source/gui_passive_capture.h`, `source/gui_profile_source.cpp`,
`source/main_fan_runtime.cpp`, `source/ui_main_window.cpp`,
`tests/gui_profile_capture_tests.cpp`, `tests/profile_source_window_tests.cpp`,
`tests/profile_review_session_tests.cpp`.

The main Profiles & tools popup opens a temporary capture pane in the native
source window. Nine domain choices precede the Read action; the default mask
excludes external fan behavior and advanced controls. Native checkboxes expose
separate omission and unsaved/unfinished-draft decisions. The exact source,
saved baseline and hidden edit text stay untouched on begin, Read, failure and
Cancel. Cancelling without an existing canonical source closes only this pane;
it never invokes the source-close live-editor rebase. Ordinary draft editing,
personal Load/Save/Clear/slot selection and shared-profile loading are blocked
while capture is open. Reset retains its existing gate.

Read uses the same fixed getter queue and context proof as Check. A shared token
sequence distinguishes the controllers; each reply has exactly one owner and
stale replies are freed. Matching queued reads can be cancelled under the
coordinator mutex; an active getter finishes within the existing bound and its
obsolete result is ignored. No separate worker, delay or polling loop is added.
Read results require the named binding, domain mask, fingerprint, ownership
revision and current service/GPU/presentation context. Changing domains or
leaving that context expires evidence, without altering the previous draft.

Review and acceptance use the existing strict capture engine through its
in-memory adapter. Accept requires readable controls, explicit omitted-domain
permission where needed and a fresh discard decision for current unsaved or
unfinished input. It consumes the frozen observation without another Capture
request. Conversion must succeed before the old draft is discarded; only then
is an unsaved canonical source installed and the native source view restored.
Save is a separate action. Applied service intent and ownership remain intact.
Flatten/pin proposals and mapped replay remain separate integration work.

Native capture layout keeps report/checkbox text readable, retains the capture
title and hidden input across DPI changes, and scrolls oversized checkboxes to
keep their leading glyph visible. An offscreen native render verified the pane;
a production desktop interaction or physical GPU write is not claimed.
Cases 9394-9433 cover matching cancellation, native decisions/cancel/DPI/focus,
production controller read/accept with precise offsets and owned zero, one-read
acceptance, late replies, ownership/epoch changes, invalid fingerprints,
allocation/queue/offline failures and legacy invalid-input preservation.
Diagnostics use bounded `profile-read:` queue/cancel and `profile-capture:`
open/review/expire/accept events without paths or complete document bodies.

## Windows read-only destination checks (2026-11-14)

> Superseded 2026-11-19 for the Windows and TUI frontends: see
> [Profile tools](#profile-tools-capture-import-and-export-end-in-ordinary-slots-2026-11-19).
> Kept as history of the engines that remain.

Source anchors: `source/gui_profile_source_review.cpp`,
`source/profile_editor_review_session.h`, `source/gui_passive_capture.h`,
`source/gui_passive_capture_queue.cpp`, `source/gui_mutation_worker.cpp`,
`source/gui_service_io_types.h`, `source/profile_source_window.h`,
`tests/profile_review_session_tests.cpp`,
`tests/gui_profile_source_review_tests.cpp`,
`tests/profile_source_window_tests.cpp`.

Check stages valid pending input, freezes the source revision and named shown
GPU, and queues one passive Capture through the existing serialized GUI I/O
coordinator. It has no Apply, recovery, initialization or file callback.
Mutation/admin/full-sync priority is retained; the passive read precedes
telemetry. The worker owns an immutable getter request and token, while all
source, HWND and report state stays on the window thread. Allocation failures
have a token-addressed failure notification, and shutdown drains posted results.
The coordinator mutex also protects delivery-handle reads during shutdown.

Completion checks the job token, source revision, presentation epoch, ready
session, service instance, GPU generation, topology, mutation domains and
outgoing ownership. Capture binding, physical fingerprint and ownership-bound
revision must agree. Only then does the shared CLI check engine evaluate the
frozen observation, without a second read. An ordinary telemetry revision or
temperature change preserves the report. New input invalidates a pending or
completed review immediately; source replacement, closed windows, lost readiness,
service/GPU changes and queued writes also expire it. Source values and applied
ownership remain unchanged, including on refused checks.

Reports use a separate read-only, wrapping/scrolling native pane. Switching
back to Source restores the exact selected value; invalid input cannot start a
check. The report is a control/mapping preview and grants no Apply authority.
Graphics preview offers Auto, P0, Global and VF component; memory offers Auto,
P0 and Global. Absent source domains have disabled selectors. Choices stay out
of the source document and Save, are frozen into each check job and expire any
previous/pending report when changed. Loading another source resets them to Auto.
Destination acceptance and mapped Apply remain separate integration work.

Cases 9387-9393 also cover native API selection, absent domains, job choices,
review invalidation, source preservation, reload defaults, DPI label fit and
keyboard focus scrolling without losing unfinished text.

Diagnostics use `profile-read:` queue/cancel and `profile-check:` completed/expired lines with tokens,
revisions, selected controls, stage/result and bounded reasons. Cases 9350-9386 cover the pure
request/session contract, native report controls and production GUI callbacks
with scripted transport. They include owned zero, stale/late completions,
invalid input, selection mismatch, duplicate reads, queue/transport/service
refusals and allocation failure. Production GPU capture and replay are not
claimed by these fixtures.

## Windows canonical source editor (2026-11-14)

> Superseded 2026-11-19 for the Windows and TUI frontends: see
> [Profile tools](#profile-tools-capture-import-and-export-end-in-ordinary-slots-2026-11-19).
> Kept as history of the engines that remain.

Source anchors: `source/gui_profile_source.cpp`,
`source/profile_source_window.h`,
`source/profile_slot_io_windows.h`, `source/gui_profile_commands.cpp`,
`source/config_profiles_gui_state.cpp`, `source/gui_service_state.cpp`,
`tests/profile_source_window_tests.cpp`.

Canonical personal slots open a separate native source editor. Offline legacy
slots use the strict Windows legacy decoder without current-GPU substitution;
online legacy slots retain their previous editor path. The decoded candidate
is frozen before unsaved-draft confirmation. Refusal or cancellation preserves
the previous model and unfinished text. Source documents live independently of
the GPU-keyed live draft and survive reconnect, selection and Reset.

The Windows window and TUI share exact source rows, decimal parsing, point-kind
conversion and revision updates. Selecting or saving a captured point does not
convert it. Explicit staging converts just the selected writable source point
to absolute frequency. Invalid text remains open across selection, Save, close
refusal and DPI changes; known read-only source points stay disabled. Unicode
rows, native keyboard navigation and scrollable small-window layouts retain
source identities without projecting them onto destination indices.

Source Save works offline through the shared bounded, generation-checked,
atomic Windows slot writer. It preserves source precision, omitted intent and
other slots/configuration; successful Save alone advances the saved baseline.
The source label never claims applied ownership. Both legacy Apply gates stay
closed while a source is open; Reset keeps its existing ready-state gate.
Closing a source requests a coherent live-editor resync. Closing an unsaved
source or exiting with one requires explicit discard, defaulting to No.
Destination acceptance and mapped Apply remain separate integration work.

Diagnostics use bounded `profile-source:` load/stage/save/close events and
`profile-bank:` commit results, without paths or document bodies. Cases
9330-9349 execute a hidden native window with fake storage callbacks and no GPU.
They cover exact/Unicode presentation, captured-kind preservation, explicit
staging, invalid-input refusal, failed Save, saved-baseline advancement, DPI,
scroll reachability, read-only points and discard/teardown. The all-target
build and existing slot/CLI tests cover the integration and shared writer;
a production desktop interaction and physical GPU writes are not claimed.

## TUI capture into an unsaved source draft (2026-11-14)

> Superseded 2026-11-19 for the Windows and TUI frontends: see
> [Profile tools](#profile-tools-capture-import-and-export-end-in-ordinary-slots-2026-11-19).
> Kept as history of the engines that remain.

Source anchors: `source/linux_tui_capture_draft.h`,
`source/linux_tui_capture_transport.h`, `source/profile_editor_capture.h`,
`source/profile_editor_report.h`, `source/linux_tui_layout_fan_profiles.cpp`,
`source/linux_tui_capture_input.h`, `source/linux_tui_render.cpp`,
`tests/tui_capture_draft_tests.cpp`.

Capture opens domain selection before any service request. Read freezes one
target-bound passive observation using the same transport as destination
Check. The shared CLI engine renders statuses, named controls and omissions
through a bounded in-memory adapter; its virtual output never reaches a file.
The modal wraps/scrolls its report, defaults focus to Cancel and cannot read
or accept before its current layout has been presented. Other edits and file
actions stay blocked while the modal is open.

Exact/flatten/pin acceptance is an explicit representation decision. Omitted settings
and replacing unsaved/invalid edits require independent opt-ins. Acceptance
uses the frozen observation without a second hardware request, installs a
canonical source draft, clears prior destination review and detaches Apply.
A separate unsaved flag distinguishes new capture from a loaded saved record;
successful Save updates the saved baseline and clears that flag. Cancel or
failure preserves the previous source, slot and edit buffer.

Exact source points, owned zero, memory API/Hz and rational power survive the
factory. Read-only points retain observations without write authority. This
workflow preserves exact controls, with reviewed alternatives described under
[capture lock proposals](#capture-lock-proposals-2026-11-15). (Graphics replay was later
activated alongside extended INI persistence; see [profile-document.md](profile-document.md).) Cases 9295-9329 cover strict decisions, frozen acceptance,
discard/cancel, empty reads, unsaved state, compact modal actions, keyboard/
wheel isolation, Cancel focus and wrapping.

## Explicit scalar edits over precise intent (2026-11-14)

Source anchors: `source/desired_scalar_editor.h`, `source/linux_tui_actions.cpp`,
`source/desired_scalar_intent.h`, `tests/scalar_editor_tests.cpp`.

The TUI's ordinary memory and power controls use typed-intent helpers rather
than changing only legacy display fields. A memory edit replaces exact Hz
while retaining a previously named API. A power percentage replaces a prior
rational value with that exact percentage. Retyping the same displayed integer
is still an explicit change when the old value had fractional precision.
Legacy values retain their legacy representation, and inactive precise
payloads are cleared. Existing UI bounds remain in force.

Refused helper inputs leave intent untouched. Accepted edits preserve unrelated
curve/fan intent and log the authoritative Hz/surface or power ratio once per
edit. Cases 9286-9294 cover semantic dirty comparison, named controls, owned
zero, reduced ratios, invalid input and legacy compatibility.

## TUI review session lifetime (2026-11-14)

> Removed 2026-11-19: the TUI source-document mode this section describes (draft
> editing, Check, mapped draft, conversion, tail restore, curve assistance) no
> longer exists; TUI Load converts imported slots to ordinary records. See
> [Profile tools](#profile-tools-capture-import-and-export-end-in-ordinary-slots-2026-11-19).
> Kept as history of the shared engines that remain.

Source anchors: `source/linux_tui_review_context.h`,
`source/linux_tui_refresh.cpp`, `source/service_control_review_revision.h`,
`tests/tui_capture_context_tests.cpp`, `tests/tui_review_lifetime_cases.h`.

Check freezes the shown GPU into the passive request. The service refuses a
different target without selecting or initializing it. The client compares
the returned service instance, GPU generation and outgoing intent against
the reviewed context before publishing capture. Passive responses do not
carry an ordinary refreshed snapshot; the TUI does not adopt their empty
telemetry as live state. The outgoing-intent hash is the existing service
review policy extracted unchanged into a pure header.

Cached previews expire on disconnect, source-revision mismatch, selection,
service/GPU/topology lifetime, available mutation domains, recovery phase or
outgoing-intent changes. Expiry clears only the report/result, preserves the
canonical source and emits one diagnostic. Normal response revision and
temperature changes do not discard a readable snapshot report. Physical
controls still need a fresh all-domain review before any future Apply.

Portable source drafts have no destination ownership and can remain open
across GPU selection/adoption. Legacy attached drafts keep their existing
switch guard; active daemon-owned settings still prevent a device switch.
Cases 9275-9285 execute the actual TUI Check and refresh/selector helpers with
scripted transport: exact target, passive payload, changed-session rejection,
cache expiry, telemetry retention and source preservation across selection.

## Structured destination review (2026-11-14)

> Removed 2026-11-19: the TUI source-document mode this section describes (draft
> editing, Check, mapped draft, conversion, tail restore, curve assistance) no
> longer exists; TUI Load converts imported slots to ordinary records. See
> [Profile tools](#profile-tools-capture-import-and-export-end-in-ordinary-slots-2026-11-19).
> Kept as history of the shared engines that remain.

Source anchors: `source/profile_destination_review.h`,
`source/profile_check_commands.h`, `source/profile_editor_check.h`,
`tests/profile_editor_check_tests.cpp`.

The shared check engine optionally publishes a typed result with explicit
source/capture/mapped/planned stages, source revision, original document,
full returned observation, selected routes, mapping, compiled runtime intent,
control/curve plans and failure details. It clears the previous result even
for unhandled or invalid input. Mapping/compilation refusals never publish
runtime intent, and failed/invalid reads never publish a capture. A refused
control plan remains a completed preview, distinct from a mapping failure.
No stage grants permission to Apply or proves operation-level feasibility.

The TUI retains this result on the heap and summarizes its typed stage and
disposition without parsing report text. Successful source edits increment a
local revision, preserve the saved baseline and invalidate the result. Load,
Reset Draft and control-preview changes also invalidate it. A fresh check uses
the current revision; original source API metadata stays separate from an
explicit preview control choice. Operation acceptance still needs fresh
ownership, transition and recovery review over every affected domain.

Memory compilation distinguishes application bounds from the runtime's
half-MHz physical grid. Sub-grid source values remain inspectable and require
explicit conversion; no preview rounds them into applicable runtime intent.
Cases 9265-9274 cover stage progression, exact choices, revision binding,
failure reset, malformed capture, source-preserving conversion refusal,
mapping failure and a completed refused control plan.

## Offline TUI source edits (2026-11-14)

> Removed 2026-11-19: the TUI source-document mode this section describes (draft
> editing, Check, mapped draft, conversion, tail restore, curve assistance) no
> longer exists; TUI Load converts imported slots to ordinary records. See
> [Profile tools](#profile-tools-capture-import-and-export-end-in-ordinary-slots-2026-11-19).
> Kept as history of the shared engines that remain.

Source anchors (all removed 2026-11-19): `profile_editor_source_edit.h`,
`linux_tui_source_edit.h`, `linux_tui_document_view.h`, `tests/tui_source_edit_tests.cpp`.

Source point and included graphics/memory offset fields accept exact decimal
MHz through six fractional digits, independently of daemon availability. The
parser rejects excess precision, trailing text and signed 64-bit overflow;
it never rounds or clamps to a destination range. Point input must be positive.
Typing an observed frequency explicitly changes only that source point to
absolute intent, even when its displayed number is unchanged. Captured payload
authority is cleared; observations and untouched points remain source data.
There is no baseline inference, neighbor adjustment or lock retargeting.

Known read-only source points and release documents have no edit action.
Invalid input stays open; cancel leaves the document unchanged. Successful
edits refresh only scalar display values, preserve the saved document baseline,
clear prior Check output and leave Apply detached. Source identities travel
through wrapped rows and never become destination indices. Check reports
remain read-only. New destination review is required before Apply integration.

Cases 9241-9264 cover exact signed limits, malformed input, decimal keystrokes,
kind conversion, source immutability on refusal, hidden/read-only points,
unrelated power/lock preservation, scalar projection, slot round-trip and
editable layouts at all supported breakpoints.

## Read-only TUI destination checks (2026-11-14)

> Removed 2026-11-19: the TUI source-document mode this section describes (draft
> editing, Check, mapped draft, conversion, tail restore, curve assistance) no
> longer exists; TUI Load converts imported slots to ordinary records. See
> [Profile tools](#profile-tools-capture-import-and-export-end-in-ordinary-slots-2026-11-19).
> Kept as history of the shared engines that remain.

Source anchors: `source/profile_editor_check.h`, `source/profile_check_commands.h`,
`tests/profile_editor_check_tests.cpp`.

The source Profiles panel can switch between stored intent and a destination
check. Graphics/memory preview buttons select the named APIs or VF component;
their choices do not rewrite the canonical source. The editor adapts its
in-memory document to the existing CLI check engine using Capture/report-only
callbacks. No temporary file, Save or Apply callback exists in that adapter.
The TUI checks its selected/shown GPU before reading the already bound daemon
session, and sends only the passive Capture command.

Mapping, precise scalar ranges, grids, curve constraints and conditional
relationships use the same engine as the CLI. Reports are bounded, wrap long
reasons and scroll by visible rows so compact terminals retain trailing bounds.
Load, source edits/reset and control-choice changes invalidate the displayed report.
A completed check grants no destination editing or Apply authority; structured acceptance,
mapped editing and fresh service review remain separate integration work.

Cases 9230-9240 exercise exact API choices, source preservation, invalid choices
before Capture, range refusal, alternate-control uncertainty, invalid/unavailable
capture and report layouts at all supported breakpoints.

## Canonical source slots in the TUI (2026-11-14)

> Removed 2026-11-19: the TUI source-document mode this section describes (draft
> editing, Check, mapped draft, conversion, tail restore, curve assistance) no
> longer exists; TUI Load converts imported slots to ordinary records. See
> [Profile tools](#profile-tools-capture-import-and-export-end-in-ordinary-slots-2026-11-19).
> Kept as history of the shared engines that remain.

Source anchors: `source/profile_document_slot.h`, `source/profile_slot_io_linux.h`,
`source/linux_tui_profile_load.h`, `tests/profile_document_slot_tests.cpp`
(the editor-document and viewer anchors this section once listed are removed).

TUI Load stages a complete canonical document, independently of its scalar
display projection. The source viewer lists exact physical units, point kinds
and hidden voltages without converting source indices into destination indices.
Save serializes that document without hardware reads. Shared CLI/TUI slot I/O
uses the existing bounded read, configuration lock, generation comparison and
atomic write. UTF-16 banks and the slot-one controls mirror remain supported.
Legacy slots retain their previous loader and writable workflow.

The pending-load candidate contains the source document as well as its display
values, so cancellation never swaps hidden intent. Document dirty tracking
compares canonical serialization, including names and metadata, rather than
only runtime control equality. Saving updates its saved document baseline;
accepted/applied intent remains separate. Unmapped source points and locks are
not projected onto the live GPU graph. Source drafts stay detached from Apply
until destination review is integrated. Offline source edits preserve those
identities without granting destination authority.

Cases 9210-9226 execute Save, fresh editor construction, Load and portable
export with no GPU; they preserve captured zero, sub-grid offsets, hidden
points, precise absolute targets, fractional memory and rational power. They
also cover concurrent file changes, UTF-16 storage, missing slots, display-only
overflow handling, name dirtiness and exact source rendering.

Verified 2026-11-14: full Windows ASan regressions, focused ASan/UBSan, native
Linux UBSan, both production CLI variants, all-target builds and source gates.
Static analysis retained 34 baseline findings and added none. Replacing stored
document dirty comparison with control equality fails 9221.

## Confirming draft replacement (2026-11-14)

Source anchors: `source/linux_tui_profile_load.h`, `source/linux_tui_actions.cpp`,
`source/linux_tui_render.cpp`, `source/linux_tui_layout.cpp`,
`source/config_profiles_gui_state.cpp`, `tests/tui_profile_load_tests.cpp`.

Linux Load freezes the selected record before presenting a dirty-draft
confirmation. No later file read can replace the reviewed generation. Keep,
Escape or N preserves draft values and uncommitted edit text; Discard and load
stages the frozen record without daemon or hardware writes. Keyboard and mouse
confirmation share the same gate, with Keep as the default. No discard is
accepted until the dialog has been rendered; too-small terminals have no
hidden actionable confirmation. Ordinary input cannot alter the pending draft
or invoke other actions while this modal is open.

Windows retains its existing comparison and Yes/No flow. If dirty input cannot
be captured or the target cannot be compared, it asks before proceeding and
defaults to keeping the draft. A failed actual load still leaves the draft
unchanged. Cases 9190-9205 execute the Linux load helpers and layout without a
daemon, including exact-value preservation, cancellation, frozen file
generation, failed reads, uncommitted edits and terminal sizes. Windows' dialog
branch uses the existing message-box implementation; native dialog interaction
is a separate verification gap.

Verified 2026-11-14: focused Windows ASan/UBSan, full native regressions,
native Linux UBSan, all-target builds and source gates. Static analysis retained
34 baseline findings and added none. Bypassing the dirty guard fails 9190.

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

### Portable documents and offline transfer (2026-11-13)

Source anchors: `source/profile_slot_codec.h`, `source/profile_bank_edit.h`,
`source/profile_commands.h`, `source/profile_commands_windows.h`,
`source/profile_commands_linux.h`, `source/profile_file_read.h`,
`source/profile_config_windows.h`, `source/profile_legacy_document.h`,
`source/profile_legacy_ini.h`, `tests/profile_legacy_ini_tests.cpp` (8986-9008),
`source/profile_config_encoding.h`,
`tests/profile_slot_codec_tests.cpp` (8370-8383, 9018-9032),
`tests/profile_commands_tests.cpp` (8400-8411, 9010-9017),
`tests/profile_commands_cli_tests.py`, `tools/security_gates.py`
(`run_profile_commands_fixture`), `build.py` (native artifact verification).

New documents use flat `document.*` keys inside the existing `profileN` section.
Exact numeric fields remain decimal. Only quoted text is encoded as UTF-8 hex,
so INI quote removal and the Windows code page cannot change names/metadata.
Reading raw bounded sections bypasses INI enumeration limits; a document marker
never falls back to historical repair. Historical Save/Clear preserves other
new documents through raw sections. The Windows whole-file buffer is 1 MiB,
with full five-slot and slot-1-mirror capacity checked by regression fixtures.

Both clients recognize `--export-profiles FILE [--slots all|1,2]` and
`--import-profiles FILE --slots 1,2 [--confirm-import]`, with optional `--config`.
They run before GPU/service initialization. Export uses stored intent only;
historical records retain unresolved indices instead of borrowing live voltages.
Import prints controls, scope, destinations and assignment references, then
requires the literal `IMPORT` after review when confirmation was requested.
Without confirmation it exits in the decision-required category without writing.

The prepared edit retains the complete original configuration generation. Under
the existing Windows cross-session lock, or the Linux stable side-file lock,
commit re-reads and compares those bytes before one atomic replacement. It
preserves preferences and assignments, never applies hardware, and flushes the
Windows INI cache after success. Native deterministic seams cover stale review,
cancelled review, write failure, one replacement and offline load/export fidelity.
The shared bounded encoding adapter exposes a UTF-8 parsing view of BOM-marked
UTF-16LE snapshots, while the edit keeps original raw bytes for generation
checks. Replacement restores the original encoding and preserves unrelated
Unicode code units, including surrogate pairs. BOM-less opaque bytes stay
unchanged. Malformed Unicode refuses before confirmation or writes. Windows
native document-section inspection uses the same decoder; commit diagnostics
report record count and encoding without preferences or profile contents.
Native Windows cases read the produced bank through both production section
inspection and real wide INI getters, then verify scratch-file cleanup.
The Linux writer lock spans profile Save/Clear and selected-GPU updates too;
the native POSIX fixture checks lock exclusion and release without timing waits.
Production build/check verifies the freshly linked native client through bounded
subprocess workflows before temporary artifacts are removed: preview/cancel,
exact zero/fraction/ratio transfer, five-slot restart/export, UTF-8 paths and
malformed input. Both Windows x64 variants ran this fixture. Linux x64/ARM64
production link checks passed; native Linux unit/store fixtures now pass under
WSL, as recorded in [testing.md](testing.md#windows-hosted-native-linux-execution-2026-11-13).
Native Linux x64 production-client subprocess workflows also passed under WSL
(2026-11-14), using the verified ELF and native temporary storage. They cover
one/five-slot transfer, source choices, exact values, Unicode-bank preservation,
cancel/refusal and native legacy export. Foreign INI export explicitly selects
its source format; ordinary local legacy export exercises each platform's own
layout. This is client-file-operation evidence, separate from driver replay,
daemon deployment and native ARM64 execution.

### Explicit legacy transfer (2026-11-13)

The common raw decoder reads selected Windows or Linux Green Curve INI records.
`--legacy-format windows-ini|linux-ini` selects incoming layout independently of
the importing OS. `--source-profiles` selects source slots (portable record
ordinals for new banks), while `--slots` selects destinations. Export uses
`--slots` for source selection. Normal native export also uses this decoder for
legacy slots rather than the settings loader's repair/migration path.

Windows memory is display MHz; a Linux migration marker proves display MHz.
Unmarked Linux memory requires `--legacy-memory-unit`; effective MHz converts
at exactly 500000 Hz per unit. Curve markers preserve absolute, projected and
per-point-origin meaning. An unmarked curve requires `--legacy-curve-semantics`;
the historical point-layout marker alone cannot prove numeric meaning. Explicit
choices never override conflicting markers. Missing locks are not inferred.
Legacy indices/exclusion counts stay unresolved; rounded mV labels supply no
exact voltage identity. Point frequency/offset observations never become
captured control authority. Fan intent includes dormant configuration and the
established zero-RPM hysteresis migration.

Both UTF-8-compatible text and BOM-marked UTF-16LE sources are accepted. The
decoder bounds bytes, lines, field count and five slots, rejects duplicate or
unknown tuning fields, and leaves output unchanged on failure. Selected new
documents cannot enter legacy decoding. Unselected modern slots may coexist
with native legacy slots. Destination replacement still uses the existing
review, confirmation, generation check and single atomic write. Native export
logs one bounded `profile legacy export:` result per inspected slot.

GUI/TUI/CLI destination reviews, mapped drafts, per-domain conversions, and
assistance workflows were subsequently wired (2026-11-15/16; historical Apply
readers refuse unknown new-format records as intended).

### Passive capture CLI (2026-11-13)

Source anchors: `source/profile_capture_commands.h`,
`source/profile_commands_windows.h`, `source/profile_commands_linux.h`,
`tests/profile_capture_commands_tests.cpp` (8540-8551),
`tests/profile_commands_cli_tests.py` (invalid choices before service access).

Both clients recognize `--capture-current FILE` before ordinary GPU setup.
They send only a domain mask to the existing service session; file output stays
in the client. Default requested domains are VF, graphics, memory, power and pin.
`--domains vf,graphics,memory,power,pin,fan,xbar,system,video` selects a subset;
duplicates and unknown values refuse before connecting. Fan is optional and
copies only explicitly requested owned intent, never an external fan controller.

The report names each requested domain's status, driver status, completeness,
composition and pin knowledge. Strict capture writes nothing when a domain is
omitted; `--allow-partial` explicitly accepts that report. `--representation exact`
is separately required before saving the raw programmed controls. Capturing no
successful controls always refuses. A failed session, invalid result or failed
atomic write preserves the prior output file. `--config` changes only the path
protected against accidental overwrite; capture neither reads nor changes slots.

Saved portable documents retain exact offsets including owned zero, microvolt
identities, named memory control, power ratios and separate observations. Import
can subsequently place the document into a local slot. Capture establishes no
service ownership and performs no Apply. GUI/TUI exact-capture review is
implemented above; CLI lock proposals are described in
[Capture lock proposals](#capture-lock-proposals-2026-11-15). Exact capture does not infer a missing pin or scalar
composition. Native fixtures execute review/refusal/save and local restart
decoding; production builds run invalid-command tests without contacting a GPU.

### Existing access helpers

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
| lock-anchor re-derivation (`lockTracksAnchor`) | `is_curve_point_visible_in_gui()`; until 2026-10-24 it read `curve_base_khz_for_point()` = `g_app.curve[i].freq_kHz - g_app.freqOffsets[i]`, then voltage only (`vf_point_visibility_policy.h`), since 2026-11-12 the listed-point rule over the frozen floor binding (`vf_writable_point_policy.h`), which still has no frequency term that drifts |
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
Windows machine. (The verdict log line has since been read from real runs:
it is what found the projection-order defect below.)

### Identity and intent come from one envelope (2026-10-22)

Source anchors: `apply_service_snapshot_to_app()` in `main_state_sync.cpp`
and its callers `apply_ready_service_envelope_to_app()`,
`refresh_service_snapshot_and_active_desired()` (`main_gpu_state.cpp`), the
GUI adoption in `gui_service_state.cpp`, and the client apply in
`gpu_backend.cpp`.

Symptom: at every GUI or CLI start the log showed `applied_slot 3 -> 0
reason=service published no active intent`, then `0 -> 3 reason=applied`
40 ms to 1.5 s later. The tray read "Manual settings" in between, and
`config.ini` was written twice; a process dying between the writes would have
left `applied_slot=0`. Reproduced on demand with `greencurve.exe
--export-active-settings <file>` against a service holding profile 3.

Cause: `apply_service_snapshot_to_app()` projects the envelope's profile
identity and ends by calling `sync_applied_profile_from_service_metadata()`,
which judges that identity against `g_app.serviceActiveDesired(Valid)`. Three
callers stored the same envelope's active intent only after it returned, so
the sync judged the NEW identity against the PREVIOUS intent (none at
startup; the old profile's intent after a switch, which reads as
`PROFILE_EDITED`). The client apply/reset commands stored the intent first,
which is why an Apply never flickered.

Fix: the projection takes the intent as parameters and stores it under the
same lock as the identity, before the sync; callers no longer assign it.
`ui_gates.check_applied_profile_projection_order` pins the order and forbids
the post-hoc assignments (verified failing with one put back). No executing
test: the GUI projection is not linked into the regression harness.

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

### Fan ownership relaxation (tightened 2026-11-20)

The relaxed ownership read accepts a profile that claims the fan against an
active intent that does not only when the profile's fan is Auto: both then
leave the fan to the driver. A Fixed/Curve profile against an unclaimed fan
means nothing drives the profile's fan setting, so the post-write identity
check logs `fan ownership differs profile=1 (mode N) active=0` and the tray no
longer ticks that profile (`profile_ownership_fan_mismatch_allowed()`; root
cause and the rest of the fix in fan-control.md).

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

### Lock-tail ownership relaxation and legacy provenance (2026-11-19)

Source anchors: `source/desired_curve_intent.h` (`desired_curve_point_kind_name()`,
`desired_curve_point_in_explicit_lock_tail()`,
`desired_base_plus_offset_point_is_projected()`,
`desired_clear_explicit_lock_tail_projection_flags()`),
`source/config_profile_repair.cpp`, `source/config_profiles.cpp`,
`source/linux_port_profiles.cpp`, `source/gpu_backend_apply_targets.h`,
`source/profile_ownership_policy.h` (`profile_ownership_lock_tail_mismatch_allowed()`),
`source/desired_settings_helpers.cpp`,
`tests/profile_lock_tail_ownership_tests.cpp` (12400-12427, 12433-12447),
`tests/regression_main.cpp` (12428-12432), `tools/ui_gates.py`.

A legacy `base_plus_gpu_offset` locked profile stored its flattened tail as
`lockMHz - gpuOffsetMHz`. On load, `restore_curve_points_from_base_plus_gpu_offset()`
(and Linux's matching loop) reconstructed `lockMHz` but marked every point
`curvePointFromGpuOffset[ci] = 1`, including `ci >= lockCi`. GUI Load -> Apply
expands the locked tail with `curvePointFromGpuOffset[ci] = 0` (absolute
`lockMHz`), so both pre-write and post-write identity checks failed at the lock
anchor (`curve intent differs at ci<lockCi>`) and recorded `AD_HOC`, leaving
the tray menu unticked and the tooltip on "Manual settings". Both loaders now
keep points at `ci >= lockCi` matching an explicit `lockMHz > 0` absolute (and
clear stale projection flags if a legacy lock is inferred afterward), while
leaving pre-tail points and interactive `lockMHz == 0` anchors projected. In
addition, `relaxedOwnershipRead` accepts differing tail-point representations
(`absent`, `absolute` at exact `lockHz`, or `projected` at `0`/`lockMHz`) on
`ci >= lockCi` once both records agree on the explicit lock and authoring, so
legacy/imported lock-only slots and pre-fix restart snapshots match too;
pre-write claims, pre-tail points, captured points, and divergent tail
frequencies remain strict. Mismatch diagnostics now report both sides' point
kind and value.

### Unique personal-slot resolution for `AD_HOC` active intent (2026-11-19)

When the service holds an authoritative active intent under
`SERVICE_PROFILE_SOURCE_AD_HOC` (after saving active manual settings into a
slot, applying with the combo pointing at another slot, applying via CLI, or
restoring an older `AD_HOC` upgrade transfer), both the service post-write
identity check (`service_profile_identity_unique_user_slot_match()` in
`source/service_profile_identity_policy.h`, `main_service_request_policy.cpp`)
and the GUI applied-profile indicator (`applied_profile_indicator_slot()` in
`source/applied_profile_indicator_policy.h`, `config_profiles_ui.cpp`) scan
personal slots `1..CONFIG_NUM_SLOTS` using `PROFILE_READ_FOR_OWNERSHIP`. A
unique match (`matchCount == 1`) resolves to that personal slot (ticking the
tray menu and naming it in the tooltip); duplicate identical slots remain
unticked (`APPLIED_PROFILE_REASON_AMBIGUOUS_AD_HOC_SLOT`), and shared/machine
bank sources never consult personal slots. Live VF readback is still never
used.

### Zero-MHz projected curve points across INI transfer and ownership matching (2026-11-20)

Source anchors: `source/profile_curve_semantics.h`
(`profile_curve_point_should_emit()`, `profile_curve_point_zero_mhz_allowed()`),
`source/profile_curve_origin_io.h`
(`profile_curve_point_zero_mhz_in_section_allowed()`),
`source/config_profile_curve_format.cpp`, `source/main_gpu_state.cpp`,
`source/config_profiles.cpp`, `source/main_settings_transfer.cpp`,
`source/linux_profile_curve_codec.h`, `source/linux_port_profiles.cpp`,
`source/profile_legacy_ini.h`, `source/profile_ownership_policy.h`
(`profile_ownership_scope_replaces_curve_points()`,
`profile_ownership_projected_point_mismatch_allowed()`),
`source/desired_settings_helpers.cpp`, `source/config_profiles_ui.cpp`,
`tests/regression_main.cpp` (12560-12575),
`tests/profile_legacy_ini_tests.cpp` (12576-12579),
`tests/profile_lock_tail_ownership_tests.cpp` (12580-12595),
`tools/ui_gates.py`.

Imported portable indexed documents store `kind=projection` points without
`observed_frequency_hz`, which `profile_desired_from_indexed_document()` decodes
as valid zero-MHz projected points (`hasCurvePoint=1`,
`curvePointFromGpuOffset=1`, `curvePointMHz=0`). Both INI writers
(`profile_curve_point_record_for_save()` on Windows,
`linux_profile_write_curve_points()` on Linux) previously skipped any point with
`mhz == 0`, so `--export-active-settings` (and any INI slot save) silently
dropped all projected points. On upgrade restore, the GPU still received the
same selective/uniform GPU offset and lock, but the service held `curvePoints=0`
(`ABSENT`) while the stored indexed slot decoded `PROJECTED/0`, failing both
claimed-slot confirmation and the `AD_HOC` personal-slot scan at the first
pre-tail point (`curve intent differs at ci<N> (profile=projected/0
active=absent/0)`).

Two complementary invariants fix both future upgrades and upgrades from
already-installed builds whose exporter still drops zero-MHz projected entries:

1. **Zero-MHz projected INI round-trip:** `profile_curve_point_should_emit()`
   emits owned projected points even when `mhz == 0`, and
   `profile_curve_point_zero_mhz_allowed()` accepts `mhz == 0` on read ONLY
   under `curve_semantics=absolute_with_origin` with an explicit
   `pointN_from_gpu_offset=1` (Windows explicit loader, Linux profile loader,
   and `profile_legacy::points()`); zero-MHz absolute points and legacy sections
   remain refused. Stored-MHz monotonicity checks skip zero-MHz projected
   points. `settings_transfer_export()` and `settings_transfer_apply()` read
   transfer files with `PROFILE_READ_FOR_OWNERSHIP` and log projected point
   counts (and any export-vs-readback discrepancy).
2. **Projected-vs-absent relaxed ownership equivalence:**
   `profile_ownership_projected_point_mismatch_allowed()` treats
   `DESIRED_CURVE_PROJECTED` and `DESIRED_CURVE_ABSENT` as equivalent on
   `relaxedOwnershipRead` (post-write identity confirmation and the GUI tray
   indicator) once both records already agree on `hasGpuOffset`,
   `gpuOffsetMHz`, `gpuOffsetExcludeLowCount`, and graphics authoring, and both
   scopes replace the VF point set (`DESIRED_SCOPE_LEGACY`,
   `DESIRED_SCOPE_REPLACE_PROFILE`, or `DESIRED_SCOPE_REPLACE_VF_POINTS`).
   Pre-write claims, non-replacing partial-update scopes, `ABSOLUTE` points, and
   `CAPTURED` points remain strictly compared. When an `AD_HOC` personal-slot
   scan matches zero slots, `sync_applied_profile_from_service_metadata()` now
   logs a concise slot mismatch reason (preferring `selected_slot`) instead of
   `detail=-`.

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
| `base_plus_gpu_offset` | whole-section: stored MHz are BASE, the GPU-offset component is added back on load; pre-tail/unlocked points become offset-derived, while explicit locked-tail points matching `lockMHz` stay absolute | up to 0.25.2 — **read only** now |
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

**INI text encoding (2026-09-24; document transfer updated 2026-11-13).** Legacy whole-file writers
(`write_config_sections_atomic`, and `write_config_text_atomic` used by profile
save/clear) encode UTF-8 text through `gc_utf8_to_ini_file_bytes()` into
the ANSI code page that `GetPrivateProfileStringW` decodes a BOM-less INI with,
and refuses characters that code page cannot hold. Profile save/clear rebuild
the file by reading every section back as UTF-8 and used to write it raw, which
re-encoded every non-ASCII value (auto-profile patterns) on every save. See
[auto-profiles](auto-profiles.md).
Document transfer instead preserves the bounded raw snapshot's original
encoding through the shared adapter above; its generated tuning keys are ASCII
and names/metadata are hex-encoded, so it introduces no ANSI conversion.

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

### The PCI location is a tiebreaker, not the identity (2026-10-22)

Firmware assigns PCI bus numbers at boot. Disabling an on-board NIC in the
firmware setup moved a single GPU from bus 07 to bus 06; the resolver then
required an exact bus match, so logon apply, Apply and profile save all failed
closed with "The configured GPU identity is missing or ambiguous" although the
same card was the only one present. Adding or removing any upstream device
(NVMe drive, capture card, bifurcation change) can do the same.

- The model identity (device, subsystem, revision, extended device) must always
  match. The location only picks between adapters that share it.
- `selected_pci_location_role` (`advisory` | `required`, written only beside a
  valid location) records what the location did when the GPU was selected:
  `advisory` when no other enumerated adapter had the same model, `required`
  when an identical one did (`configured_gpu_location_role_for_selection()`).
  The version stays 1: older builds ignore the key and keep the strict rule, and
  any other spelling is malformed.
- `resolve_configured_gpu_selection()`: one adapter at the stored location wins;
  two adapters claiming it are AMBIGUOUS. If the stored location no longer
  holds the model, the model is followed to its one live adapter when
  `configured_gpu_location_may_move()` allows it: always for `advisory`, never
  for `required`, and for a record without a role (`UNRECORDED`, every record
  written before this change) only while exactly one adapter is present (the
  legacy-ordinal tolerance). Several adapters of the model and no location
  match is AMBIGUOUS when moving is allowed, NOT_FOUND otherwise.
- `maintain_configured_gpu_selection_record()` (GUI only, after each service
  snapshot whose adapter list differs from the last one checked) rewrites a
  record that resolved but moved, or has no role, from the adapter it resolved
  to. That makes the location current again (so an older build reading the
  file matches too) and records the role. It never rewrites an unresolved
  record, never runs in the service (the account config is the user's), and
  skips an adapter whose location is unreadable. It also owns the older
  single-adapter auto-save of a missing `[gpu]`.
- Published machine slots carry the role through the same serializer, and the
  publication readback compares it. The service never rewrites them; a stale
  shared binding keeps resolving under the same rules.
- Deliberately preserved: identical cards whose locations all changed fail
  closed (only a per-board ID such as the NVML UUID could tell them apart), and
  an identical card that later lands exactly on a stale stored location is
  indistinguishable from the original, as before. The refresh shortens that
  window to the first GUI start after the move.
- Linux applies the same rule to its stored targets (config.ini, daemon
  records, requests); see linux-scaffold.md, "GPU selection across PCI bus
  renumbering".
- Diagnostics: `format_configured_gpu_resolution_detail()` prints one bounded
  line (stored model@location, role, first four live adapters). It is logged by
  the in-process enumeration on failure and on a move, by the service logon
  path once per distinct picture, by the shared-binding resolver, by client
  validation failures, and by each persist (`location role=`) and refresh.
- Tests: `tests/gpu_location_role_tests.cpp` (7300-7329); 7300 is the reported
  case and fails on the previous resolver.
- Service apply/reset requests carry the selected `GpuAdapterInfo`, so a stale GUI or mismatched service target cannot silently apply to a different active adapter.
- Published machine slots use the same serialized identity schema under
  `[profileN_gpu]`; one parser/formatter owns both formats.

## Source of truth

- `source/configured_gpu_selection.h` (`ConfiguredGpuSelection`,
  `GpuLocationRole`), `source/gpu_selection_policy.h` (resolver, location role,
  diagnostics line), `source/gpu_selection_config.cpp` (GUI persist, client
  validation, record refresh), and the `[gpu]` reader/writer in
  `source/config_utils.cpp`

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

## Editor profile visibility (2026-10-24)

Source anchors: `source/config_profiles.cpp`,
`source/vf_writable_point_policy.h`, `source/config_profile_repair.cpp`,
`tests/vf_point_visibility_tests.cpp`.
Profile editor projection calls `vf_filter_unlisted_editor_curve_points()`
(2026-11-12; formerly `vf_filter_hidden_editor_curve_points()`): it uses the
grid's listed-point rule over the bound floor topology, removes unlisted
points' mask, MHz and provenance together, keeps writable points below
700 mV, and keeps populated floor points from 700 mV upward. A zero-frequency
entry is unpopulated even if its voltage is above the threshold. The
projection runs only for an editor read with a loaded live curve; ownership
reads retain the saved claim. `profile load: editor projection dropped`
reports the count only when something was removed. A stored `pointN_visible`
flag is read as written; without the key, `profile_point_saved_visible()`
answers with the 700 mV voltage rule its writer used. Fixtures 7390-7392
verify the projection; 7380-7389 cover the voltage predicate. Real-window
profile loading is not part of these pure units.

## Save names what it leaves out (2026-11-15)

Source anchors: `source/profile_save_policy.h`, `source/gpu_lock_ownership_policy.h`,
`source/ui_profile_save_notice.cpp`, `source/gui_profile_commands.cpp`,
`source/main_runtime_control.cpp`, `source/linux_tui_actions.cpp`,
`tests/curve_plan_tests.cpp` (7880-7884, 7894-7899).

- A Save with no curve or lock edit stays a partial live save of the scalar
  domains (unchanged contract), but counts populated graphics points whose
  programmed offset is neither zero nor the saved GPU-offset component
  (`profile_save_count_unsaved_programmed_points()`) and never borrows unowned
  live hardware offsets (`g_app.freqOffsets`) or external flat tails to expand
  a draft:
  - Windows GUI asks whether to open Capture (`MB_YESNO`, invoking
    `gui_profile_capture_begin()` on `IDYES` after save completes) when
    omitting external curves or approximated locks (`OMITTED_EXTERNAL_CURVE` /
    `DETECTED_APPROXIMATION`), and TUI sets a status line pointing to Capture.
  - Pre-tail preservation during Save (`gui_pre_tail_point_decision()`) only
    retains Green Curve's own previously applied/active-service curve intent
    (including `curvePointCaptured`), never synthesizing `preTailInferred`
    entries from live `g_app.freqOffsets`.
- Live lock detection (`detect_locked_tail_from_curve()`, gated by
  `should_auto_detect_locked_tail_from_live_curve()` and
  `gpu_live_lock_ownership_proven()`) refuses to adopt unowned external flat
  tails into `g_app.locked*` or `g_app.appliedLock*` (`ownership=external`
  log once), and preserves Green Curve's own service/client lock intent while
  only refining `preferredCi = gpu_live_lock_preferred_ci()` when proven.
  *(Reversed for the Windows editor only on 2026-11-19; see below.)*
- Diagnostics: one `profile-save: slot=... source=... notice=...
  omittedProgrammedPoints=... firstOmitted=ci... inferredPreAnchor=...` line
  per Save (Windows); the TUI logs `profile-save: slot=...
  source=staged-draft draftCurve=... omittedProgrammedPoints=...`.
- With the detector fix (`gpu-backend.md`, "Curve feasibility planner"), an
  explicit Capture flow -> Save -> Reset -> Load -> Apply plans as
  representable at the true anchor (ROUND-01, 7885-7889; ownership exclusion
  7894-7899).

## Source curve assistance and closest-target search (2026-11-16)

Source anchors: `source/profile_curve_assistance.h`,
`source/curve_prefix_repair.h`, `source/profile_check_commands.h`,
`source/gui_profile_source_review.cpp`, `source/profile_source_window.h`.

- Portable source drafts checked against a live destination support three
  explicit draft-only preparation modes (`prefix`, `closest-target`,
  `combined`), exposed in Windows `ProfileSourceWindow` (`ASSIST_CHOICE` /
  `PREPARE_CURVE` confirmation), Linux TUI (`ACTION_ASSIST_CURVE_CYCLE`,
  `ACTION_ASSIST_CURVE_PREPARE`, two-step `ACTION_ASSIST_CURVE_ACCEPT`), and CLI
  (`--check-profile ... --prepare-curve ... --output ...`).
- Detailed CRT grid intersection, `<= 65536` descending candidate budget,
  captured/projected prefix rules, status taxonomy, and regression fixtures
  (`9800-9874`) live in `profile-document.md` ("Compatible-curve assistance").

## User-started workload pin observation (Tier 3, 2026-11-16)

Source anchors: `source/profile_pin_observation.h`,
`source/nvml_pin_observation_sampler.h`, `source/profile_capture_proposals.h`,
`source/profile_capture_commands.h`, `source/profile_source_window.h`,
`source/gui_profile_capture.cpp`, `source/linux_tui_capture_draft.h`.

- Passive capture leaves hard-pin evidence absent (`PROFILE_PIN_UNKNOWN`) unless
  Tier 1 driver getter or Tier 2 retained service intent proves it; Tier 3 runs
  only on explicit user action in CLI (`--observe-pin` + `--confirm-pin-mhz`),
  Windows GUI (`Observe pin` -> editable MHz -> `Confirm pin`), or Linux TUI
  (`ACTION_CAPTURE_OBSERVE_PIN` -> step MHz -> `ACTION_CAPTURE_CONFIRM_PIN`),
  never overriding Tier 1/2 evidence or auto-selecting `pin`. Since 2026-11-19
  Tier 1 is the driver's client lock table, read by every passive capture, so
  the Tier 3 action appears only when that table cannot be read.
- Stale-risk: the burst takes its samples back to back, so "sustained" covers
  one instant rather than seconds; the dialog no longer claims a timed sample.
- Read-only NVML burst sampling stays isolated to the exact captured GPU PCI
  address, requires sustained active load (`>= 3` active samples, spread `<= 15 MHz`)
  well below the captured VF curve ceiling at operating voltage, reports driver
  clock/power/thermal limit reasons alongside warnings, and distinguishes
  confirmed observations (`user_confirmed`) from edited targets (`user`); detailed
  rules and tests (`9900-9946`) live in `profile-document.md`.

## Editor projection of another tool's flatten (2026-11-19)

Source anchors: `source/gui_observed_lock_policy.h`, `source/main_observed_lock.cpp`,
`source/main_state_sync.cpp`, `source/profile_save_policy.h`,
`source/ui_profile_save_notice.cpp`, `source/gui_profile_commands.cpp`,
`tests/gui_observed_lock_tests.cpp` (`12290-12299`), `tools/ui_gates.py`.

- **Reverses the 2026-11-15 rule for the Windows editor only:** the service
  still never adopts an external flatten (`gpu_lock_ownership_policy.h`), and
  its snapshot still reports no lock for it.
- **Display & recognition (`gui_observed_lock_recognize()`):** when the service
  reports no lock, the editor projects a live external flatten into
  `g_app.locked*` (checkbox on the anchor, greyed tail, plateau target) if the
  anchor is listed with at least one listed point after it and at least one
  tail point carries an offset different from the anchor's (a stock level top
  or uniform VF-table offset is never shown as a lock).
- **Safety & Apply rules (`gui_observed_lock_decide()`):** never touches an
  editor with unsaved edits; withdraws the projection when the flatten leaves
  the GPU; yields when Green Curve's own lock takes over. The projected lock is
  mirrored into `g_app.appliedLock*`, so it is not a pending change (sparse
  power/fan Applies leave the curve alone; a full Apply sends the lock like any
  other).
- **Save notice:** `PROFILE_SAVE_NOTICE_OBSERVED_FLATTEN` labels the saved slot
  as a flatten another tool set, counts unsaved programmed points below the lock
  (`profile_save_count_unsaved_pre_anchor_points()`), and offers
  `Import current GPU settings` for a full copy.
- **Diagnostics:** `lock observe: action=...` (`debug_log_on_change`), plus
  `lock observe: showing...` / `withdrawn` and `profile-save: ... observedLock=...
  unsavedBelowLock=... firstBelow=ci...`.
