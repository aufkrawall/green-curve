# Auto-Profiles, Hotkeys, and Tray Profile Selection

Automatically switch the active GPU tuning profile based on the foreground
process/window, plus per-slot global hotkeys and a tray/main-window profile
picker. GUI-process only; the SYSTEM service is untouched.

## Why it lives in the GUI process

Foreground detection and `RegisterHotKey` require a windowed, message-pumping
process in the interactive session — that is the GUI (which already runs
resident via tray + start-on-logon). The elevated SYSTEM service cannot cleanly
observe the session's foreground window, and **no injection into other
processes** is a hard anti-cheat constraint (locked by the `F-NO-INJECT` build
guard). Foreground observation and policy decisions run on the GUI message
thread. Hardware mutations are handed to the shared serialized GUI mutation
worker, so the window remains responsive while client-side pending work is
coalesced.

## Files

- `source/auto_profile_rules.{h,cpp}` — **pure** rule model + `resolve_auto_profile_slot()`
  (ordered, first-match-wins over exe/title/class/fullscreen matchers, per-rule
  `require_focus`, default-slot fallback) + INI persistence (`[auto_profiles]`,
  `[auto_ruleN]`). No OS deps in the resolver (reusable by a future Linux port).
- `source/auto_profile_controller.{h,cpp}` — **pure** AUTO/MANUAL-pin state
  machine: debounce coalescing, cooldown (`minSwitchIntervalMs`), the "same
  hotkey twice → resume auto" semantics. Transitions return an `AutoProfileAction`
  the driver executes; no timers/IO. `ap_apply_config_change()` is the sole
  entry point for adopting an edited configuration: it syncs values and, when
  the master enable actually *changed*, delegates to `ap_set_enabled()` so the
  enable/disable transition happens exactly once no matter which surface flipped
  it.
- `source/auto_profile_detect.cpp` — Win32 read-only probing that never touches
  another process: `GetForegroundWindow` + `GetWindowText/ClassNameW` (window
  metadata); the foreground **exe base name is resolved from a
  `CreateToolhelp32Snapshot` process-list lookup by PID — NOT `OpenProcess`** (so
  no handle is opened to the game, and it works for elevated processes too);
  fullscreen = window rect vs monitor rect (shell classes excluded); the same
  snapshot mechanism drives focus-optional exe-rule presence.
- `source/auto_profile_win32.cpp` — the **driver**. Owns config + controller,
  an OUTOFCONTEXT `EVENT_SYSTEM_FOREGROUND` WinEvent hook, the debounce +
  backstop timers, hotkey registration, and the apply glue.
- `source/hotkeys.{h,cpp}` — `"ctrl+alt+f2"` parse/format + `RegisterHotKey`
  wrappers (`MOD_NOREPEAT`).
- `source/auto_profile_dialog.cpp` — pseudo-modal rule editor (mirrors
  `fan_curve_dialog.cpp`). The per-slot hotkey fields are **capture** controls:
  each hotkey EDIT is `SetWindowSubclass`ed (`apd_hotkey_subclass_proc`) to grab
  `WM_KEYDOWN`, read live modifier state + key, format via `hotkey_format`, and
  swallow the raw keystrokes — the user presses a combo rather than typing it.
  The dialog is centered over its owner and clamped to the monitor work area.
  Its checkboxes use the same dark owner-draw renderer as the main window, and
  `WM_CTLCOLORLISTBOX` themes the opened combo dropdowns instead of leaving a
  white native list surface.
- Tray "Profiles" submenu + main-window "Auto-Profiles..." button:
  `build_auto_profile_menu()` / `show_profiles_popup()` in `main_fan_runtime.cpp`.
  The tray menu that hosts the submenu lives in `gui_tray_menu.cpp` because,
  unlike the button popup, it must not raise the main window (F-TRAY-MENU in
  `windows-ui-layout.md`); its picks reach the window as posted `WM_COMMAND`s
  exactly as before.
  The submenu shows a checkmark via `auto_profile_active_slot()` which reads
  `[profiles] applied_slot` — the last slot actually applied to the GPU, not
  the combo-selected `selected_slot`.
- All `#include`d into the `main.cpp` unity chain after `entry.cpp` (GUI-only
  block), so the driver sees the already-defined profile-apply/UI statics.

## Control flow

1. A foreground change (WinEvent, event-driven, zero polling) or a ~3s **backstop
   timer** (catches game exits + focus-optional presence) resolves the target
   slot via `ap_resolve_current_target()` → `resolve_auto_profile_slot()`.
2. `ap_on_target_resolved()` arms the debounce (`switchDebounceMs`, default 800ms)
   only when the target differs from the applied slot — holding just the latest
   pending target, so alt-tab bursts coalesce to at most one apply.
3. `ap_on_debounce_fire()` applies the freshly re-resolved target, or defers for
   the remaining cooldown (`minSwitchIntervalMs`, default 4000ms ≥ the ~3s
   single-apply floor).
4. `ap_do_apply_slot()` reuses the existing TDR-safe path: `load_profile_from_config`
   → **idempotency skip** via `desired_settings_match_active_service_intent()`
   (same guard as `maybe_load_app_launch_profile_to_gui`) → enqueue with
   `resetOcBeforeApply=true`. The worker performs only IPC; completion is posted
   to the GUI thread, which adopts the response and updates the tray/UI.
5. The queue permits one active and one latest pending request. Pending
   auto-profile work is superseded deterministically; Reset replaces pending
   Apply and cannot be overtaken. A completion re-resolves foreground state and
   arms a follow-up if the target moved while hardware I/O was active.
6. **Failure backoff (2026-09-24).** `ap_do_apply_slot()` returns
   `ApApplyStart` (QUEUED / SKIPPED / ALREADY_APPLIED / FAILED). A FAILED start
   (empty slot, profile load error, unresolved GPU identity, queue refusal) and
   a failed completion both call `ap_on_apply_failed()`, which charges the
   cooldown and arms a per-slot backoff: `minSwitchIntervalMs`, doubling per
   consecutive failure of that slot, capped at
   `AUTO_PROFILE_FAILURE_BACKOFF_MAX_MS` (300 s). `ap_on_target_resolved()` and
   `ap_on_debounce_fire()` arm the remaining backoff instead of applying.
   Cleared by a success, an explicit hotkey/tray pick, an enable transition, or
   any config change. Before this, failure never touched `appliedSlot` or
   `lastApplyMs`, so the completion's re-resolve re-armed the 800 ms debounce
   and the same failing switch ran about once a second for as long as the
   matching app kept focus. Grep `auto-profile: slot N switch failed (...);
   consecutive=... next automatic attempt in ... ms`.

## Manual pin / hotkeys / suppression

- A per-slot hotkey or a tray/menu profile pick → `ap_on_hotkey()`: pins the slot
  and applies immediately (no debounce/cooldown); **pressing the same slot again
  resumes AUTO** and re-converges. A different slot moves the pin.
- **A pick while auto is disabled records no pin** — it is just an apply. The pin
  only exists to override automatic switching, and while disabled its documented
  release ("same slot again") is unreachable, so a pin taken then would be
  permanent. See the 2026-07-30 entry below for what that cost.
- `suppressWhenWindowOpen` (default on): while the main window is visible auto
  does not switch (so it never clobbers live edits); resumes when minimized.
- **The master enable is a transition, not a stored value.** Enabling clears any
  pin and re-converges; disabling reverts to the default slot (never stranded).
  Both surfaces that can flip it — the tray "Auto-switch profiles" toggle and the
  configuration dialog's checkbox (which reaches the driver through
  `auto_profile_reload_config()`) — go through `ap_apply_config_change()`, so
  they cannot disagree. An unchanged enable adopts the edited values and leaves a
  deliberate pin alone.

## Config (`%LOCALAPPDATA%\Green Curve\config.ini`, per-user)

```
[auto_profiles]
enabled, default_slot, switch_debounce_ms, min_switch_interval_ms,
suppress_when_window_open, rule_count
[auto_ruleN]           ; N = 1..rule_count, evaluated in order (first match wins)
match_type=exe|title|class|fullscreen ; pattern= ; require_focus=0|1 ; slot=
[hotkeys]
slotN=ctrl+alt+fN
```

**Text encoding (2026-09-24).** Patterns are UTF-8 end to end: detection
converts window title/class/exe from UTF-16 with `gc_wide_to_utf8_truncating()`
(cut at a whole code point), the dialog's pattern fields are Unicode edit
controls read/written through UTF-8, and the loader returns UTF-8 through the
profile wrappers. The file itself is in the ANSI code page, because that is what
`GetPrivateProfileStringW` decodes a BOM-less INI as; every whole-file writer
(`write_config_sections_atomic`, `write_config_text_atomic`) encodes through
`gc_utf8_to_ini_file_bytes()` and refuses a character the code page cannot hold
instead of storing `?`. Before, detection and the dialog used the ANSI code page
while the loader returned UTF-8, so a non-ASCII rule stopped matching after the
first restart, displayed as mojibake, and was re-encoded on every save (profile
save/clear rewrote the whole file raw, so even a save that never touched the
rules corrupted them). Tests 6400-6409 (Windows): round trip through the real
profile API over two save generations, detection match, encoder refusal, and
code-point truncation; verified failing (6401) with the encoder disabled.

`AUTO_PROFILE_MAX_RULES = 8` (fixed dialog grid, no scrolling/data loss).
`require_focus` is meaningful for exe rules (foreground vs merely running);
title/class/fullscreen are foreground-by-nature.

## Invariants / guards

- **No injection / no process handles** (`F-NO-INJECT`): the detect/driver files
  must not contain `WriteProcessMemory`/`CreateRemoteThread`/`VirtualAllocEx` and
  must not call `OpenProcess` at all (foreground exe name comes from the process
  snapshot); the hook must be `WINEVENT_OUTOFCONTEXT`.  Purely passive
  observation with standard APIs — no stealth/evasion.
- **Wiring** (`F-AUTO-PROFILE`): `auto_profile_init/shutdown` on WM_CREATE/WM_DESTROY,
  `WM_HOTKEY` routed to `auto_profile_on_hotkey`, driver compiled into the unity build.
- **Dialog theme parity:** every dialog checkbox is `BS_OWNERDRAW`, its click
  toggles explicit `UiCheckboxState` owned by the dialog, synchronously redraws
  the control, and opened combo lists use `COL_INPUT`. `BS_OWNERDRAW` is a
  button type rather than a checkbox type, so these controls must never depend
  on unsupported `BM_GETCHECK`/`BM_SETCHECK` storage. Source guards prevent that
  regression, `BS_AUTOCHECKBOX`, or an unthemed listbox. As of 2026-07-27 those
  guards cover every shard that paints or projects an owner-draw checkbox, not
  just this dialog — the main window had the same defect (F-CHECKBOX-PAINT in
  `windows-ui-layout.md`).
- **One definition of enabling** (`F-AUTO-PROFILE`, guarded in
  `tools/ui_gates.py::check_auto_profile_enable_is_a_transition`): every
  enable-flipping surface must contain `ap_apply_config_change(&g_apCtrl,
  &g_apConfig)` and must **not** call `ap_controller_sync_config()` directly —
  the value-only sync is exactly the regression that shipped.
- **Don't needlessly re-apply**: the idempotency skip means switching to an
  already-active profile is a GUI-only no-op (no reset+reapply). An explicit
  tray/hotkey pick that hits this skip now says `Profile N is already applied.`
  on the profile status line — the skip is correct, but silently doing nothing
  in answer to a deliberate pick reads as a broken menu.
- **An explicit pick reports on the profile status line; an automatic switch
  does not.** `service_apply_origin_is_explicit()` is the discriminator on all
  three sites (queue, idempotency skip, completion). A tray/hotkey pick is a
  user action and gets the same `Applying Profile N to the GPU...` → outcome
  wording the Apply button produces (F-RESULT in windows-ui-layout.md); a
  rule-driven foreground switch stays silent because the user did not ask for
  it and must not have a line they are reading overwritten.
- **Presentation-silent background completion:** tray, hotkey, and automatic
  profile results may refresh hidden model/control/tray state and the resident
  status label, but must not show, activate/focus/flash a window, open a dialog
  or taskbar surface, allocate a console, launch a helper process, or
  synchronously paint the main owner. Setting the text of a child label that
  already exists creates no surface and takes no focus, which is why it sits
  inside this contract rather than against it.
  Top-level redraw suppression is used only when that owner was initially
  visible; hidden completion uses deferred invalidation for the next explicit
  show. Source guards enforce the no-presentation/no-process-launch API boundary
  in the apply and completion operations.
- Pure resolver + controller are exhaustively unit-tested in `build.py`
  (`F-AUTO-PROFILE`, returns **220–272**): matcher/order/require_focus/default,
  coalescing/cooldown/manual-pin, the enable transition, config round-trip,
  hotkey parse/format.

## Diagnostics / failure modes

- All decisions log at `debug_log` (`auto-profile: ...`): init params, resolved
  applies, idempotency skips, apply failures, unparseable/unregisterable hotkeys.
- **Why nothing switched**, the two lines to grep for first (both
  `debug_log_on_change`, so a steady desktop costs one line, not one per 3s
  backstop tick):
  - `auto-profile: not driving (enabled=… mode=… pinned=… suppressed=… inFlight=…
    windowVisible=… suppressWhenOpen=…)` — names the gate that is blocking.
  - `auto-profile: resolved target=… applied=… fgValid=… exe=… class=…
    fullscreen=… present=…` — what detection saw and what the rules made of it;
    `present=` is the per-rule presence bitstring, so a focus-optional rule is
    visible even when its process never comes to the foreground.
  Plus `auto-profile: runtime state enabled=… foregroundHook=… backstop=…` and an
  explicit `SetWinEventHook FAILED` / `SetTimer FAILED` line — a hookless session
  still converges on the 3s backstop, which looks like a slow rule rather than a
  failure unless it is logged.
- `auto-profile: config reloaded (…)` now carries `mode`, `pinned`, `applied` and
  the transition kind, so a dialog OK shows what it did to the controller.
- **Elevated games**: the snapshot-based PID→exe-name lookup works across
  integrity levels, so elevated games match by exe like any other. (If the
  process vanishes between the foreground query and the snapshot, `exeName` is
  empty and title/class/fullscreen rules still apply.)
- **No apply pump stall:** manual, app-launch, hotkey/tray, and foreground
  mutations share the sole runtime service-I/O coordinator in
  `gui_mutation_worker.cpp`. Its worker procedure may not read or mutate
  `g_app`; it owns typed transport/admin execution and posts immutable
  completions. The GUI thread alone reduces protocol-v13 envelopes and changes
  visible state. Before dispatch, pending work is checked against session/GPU
  epochs and stamped with the accepted service instance, GPU generation,
  topology, and selected identity; the service checks those preconditions again
  around its runtime lock. Supersession/generation invalidation is logged and
  clears the auto-profile in-flight state. Full sync coalesces and telemetry is
  dropped behind writes or full sync.
- Hotkey `RegisterHotKey` failure (combo owned by another app) is non-fatal:
  logged, that slot stays unbound.

## Open questions / stale-risk

- Title/class matching is case-insensitive **substring** (regex is a possible
  follow-up).
- Live GPU/desktop behavior (real switching under game load, TDR margin) needs
  hardware validation — unit tests cover the logic only.

## Last verified

- 2026-09-24: failure backoff and UTF-8 pattern handling (see Control flow
  step 6 and Config). Controller tests 6410-6424, verified failing (6412) with
  the backoff disabled. Software only: no live auto-switch run with a failing
  profile or a non-ASCII rule on hardware.
- 2026-07-30: **Auto-switching never ran for a user who enabled it in the
  dialog.** Diagnosed from the config + debug log, not by reasoning: the config
  was correct (`enabled=1`, `exe=<app>.exe`, `require_focus=0`, `slot=1`,
  `default_slot=2`), the GUI was alive, and the log went completely silent after
  `config reloaded (enabled=1 rules=1)`. The preceding lines were the answer —
  a tray pick of slot 1 about a minute earlier.

  `ap_set_enabled()` owns the reset that clears a manual pin, and only the tray
  toggle called it. The dialog persists `enabled` and reaches the driver through
  `auto_profile_reload_config()`, which called `ap_controller_sync_config()` —
  values only. So the controller stayed in `AP_MODE_MANUAL`,
  `ap_controller_is_driving()` stayed false, and every foreground event and
  backstop tick returned before doing anything, forever. Worse, the pin was
  taken while auto was *disabled*, where a pin means nothing and cannot be
  released the documented way ("same slot again").

  Two fixes, both in the pure controller: `ap_apply_config_change()` makes the
  enable a transition for every surface, and a pick while disabled no longer
  pins. The lesson generalizes past this bug: **when a flag has both a stored
  value and a transition, a second UI that writes the value is a silent
  regression** — there is no error, only a feature that stops existing.

  Also fixed the reason it took a log-dive at all: the whole detect→resolve→
  decide path logged nothing unless it applied. It now logs the gate and the
  resolution on change.
- 2026-07-15: Tray-profile completion no longer flashes the hidden main owner.
  Native Win32 coverage proves the old top-level `WM_SETREDRAW(TRUE)` visibility
  side effect and the replacement visibility-preserving transaction. (Superseded
  2026-07-31: the transaction no longer sends either half of that pair to a
  top-level window — see `windows-ui-layout.md`.) Background
  apply/completion paths are source-guarded against window, dialog, activation,
  focus, taskbar-flash, console, and child-process APIs.
- 2026-07-15: Auto/app-launch decisions now wait for one coherent accepted
  READY envelope and use the generation-stamped sole runtime I/O coordinator;
  state reads, writes, and admin waits never block the window thread.
- 2026-07-13: Auto-profile and app-launch applies now use the shared one-active,
  one-latest-pending background mutation path. Completion/UI adoption is
  GUI-thread-only; Reset precedence, supersession cleanup, and stale
  session/GPU rejection are covered by pure policy tests and source guards.
- 2026-07-12: Fixed owner-draw checkbox clicks that did not reliably change or
  save state. The dialog now owns checked state explicitly and immediately
  redraws after toggles; pure state tests, normal/ASan suites, Windows
  x64/ARM64 checks, and the full build-398 release rebuild pass.
- 2026-07-12: Dialog placement/theme parity verified by warnings-as-errors x64
  and ARM64 builds, the full build-397 release rebuild, and owner-draw/listbox
  source guards. Native visual confirmation remains useful on the reporting
  desktop.
- 2026-07-04: Implemented (build 357+). `python build.py` (all 4 targets) +
  `python build.py --test` green. Logic unit-tested; live GPU/foreground behavior
  pending user hardware validation.
