# Windows UI layout

## Summary

The main Win32 window uses a single responsive layout plan. It must fit inside
the selected monitor's **work area** (not its nominal screen bounds), keep every
control at a readable DPI-scaled size, and expose scrolling rather than clipping
content when the effective work area is physically too small.

Normal size pressure is absorbed in this order:

1. The VF grid uses as many complete columns as the available width permits.
2. The graph height flexes between 260 and 520 logical pixels; 420 is the
   preferred initial height.
3. If minimum readable geometry still cannot fit, the complete content canvas
   gets standard horizontal and/or vertical window scrollbars.

No feature group is selectively removed or collapsed. Keyboard focus on an
off-screen child automatically scrolls that child fully into the viewport.
On first launch the normal window is centered in the work area of the monitor
under the cursor; later launches restore the last validated normal placement.

## Source anchors

| Source | Role |
|---|---|
| `source/main_layout_policy.h` | Pure physical-pixel layout policy, graph bounds, column/row selection, section boundaries, and overflow decisions |
| `source/ui_main_layout.cpp` | Monitor work-area clamping, stable scrollbar convergence, complete child placement, focus visibility, DPI resource rebuild, and layout diagnostics |
| `source/ui_main_controls.cpp` | Main dynamic control/header creation and registration with the layout engine |
| `source/ui_oc_hints.cpp` / `source/oc_range_hint_policy.h` | Themed per-field range tooltips for the overclock row; pure bound derivation and tooltip text |
| `source/ui_message_box.cpp` / `source/message_box_policy.h` | OS-theme-aware replacement for `MessageBox`; pure button sets, default/escape mapping, and geometry |
| `source/ui_main_apply.cpp` / `source/oc_high_warning_policy.h` | Apply/Refresh/Reset commands and the high-overclock confirmation; pure warn decision |
| `source/ui_mutation_completion.cpp` / `source/gui_mutation_result_policy.h` | Where a manual Apply/Reset result lands: the pure dialog-vs-status-line decision (F-RESULT), the profile label, and the queued/completed status wording |
| `source/ui_main_control_lifecycle.cpp` | Redraw-suppressed destruction/recreation of dynamic controls |
| `source/ui_main.cpp` | Editor population, lock commands, and the process-owned system-memory DIB backbuffer plus GDI generation |
| `source/ui_main_graph.cpp` | VF graph rendering: applied and pending series, axes, labels, and the locked-tail drift diagnostic (split out of `ui_main.cpp`). Materialises the resolved preview against live readback; decides nothing about it |
| `source/ui_pending_changes.cpp` / `source/gui_pending_changes_policy.h` | F-PENDING cached per-domain diff, the resolved graph preview (`GuiGraphPreviewPoint`) that gates the graph repaint, minimal repaint, and the Apply enable; pure per-domain decisions |
| `source/ui_main_ctlcolor.cpp` | `WM_CTLCOLOR*` palette handling incl. the orange pending field text (split out of `ui_main_window.cpp`) |
| `source/ui_theme_button.cpp` | Owner-drawn button/checkbox painting, the themed-control id tables, and the derived checked-state + last-painted mirror resolvers (split out of `main_runtime_ui.cpp`) |
| `source/ui_checkbox_state.h` | `UiCheckboxState` plus `ui_checkbox_state_needs_repaint()`, the only correct repaint gate for an owner-draw checkbox |
| `source/ui_control_projection.h` | Inert text/enable projections; deliberately holds **no** check-state helper (see F-CHECKBOX-PAINT) |
| `source/gui_render_forward.h` | Forward declarations for the curve renderers and the F-PENDING queries, needed by shards amalgamated before their definitions |
| `source/ui_main_window.cpp` | Resize, scroll, reconnect overlay, paint fallback, display/composition/device, focus, and `WM_DPICHANGED` routing |
| `source/config_profiles_gui_state.cpp` | Profile-intent projection onto the editor, profile status/state labels, and background-service controls (split out of `config_profiles_ui.cpp`) |
| `source/gui_service_state.cpp` | Main-thread state acceptance, draft reconciliation, reconnect presentation, and one-transaction rendering |
| `source/gui_service_actionability_policy.h` | Pure per-capability enablement decision from service/draft state, plus the `gui_service_actionability_from_app()` glue in `app_shared.h` |
| `source/tray_presentation.cpp` / `source/gui_tray_callback_policy.h` | Tray tooltip text, the active-profile label, and the live-hardware gate; pure label decision (split out of `main_gpu_front.cpp`) |
| `source/gui_tray_menu.cpp` | The tray context menu and the neutral, never-shown foreground owner it is tracked against (F-TRAY-MENU); split out of `main_fan_runtime.cpp`, which is at its size ratchet |
| `source/gui_selected_gpu_pnp.cpp` | Exact selected-device presentation invalidation and GDI retirement cue |
| `source/entry.cpp` | Main-window creation from the centered/restored placement policy |
| `source/ui_theme_metrics.h` / `source/ui_theme_checkbox.cpp` | Canonical DPI-scaled checkbox box/inset/gap metrics, the labeled-checkbox hit width, the shared dark owner-draw renderer, and the caption-width fit |
| `source/app_shared.cpp` | Per-Monitor-V2 process-awareness request with legacy fallback |
| `source/xbar_dialog.cpp` | Advanced XBAR dialog geometry and themed controls; converts desired client size to the outer `CreateWindowExA` frame |

## Invariants

- A dialog's child layout is expressed in **client** coordinates, while
  `CreateWindowExA` takes an **outer** rectangle. Every fixed-size themed dialog
  must convert the desired client rect through
  `adjusted_window_size_for_client()` (DPI-aware) before creation. The Advanced
  XBAR dialog previously passed its 320 dp client height directly, so the caption
  plus border consumed the bottom button row. Its client width is `dp(540)`
  (508 dp content width between 16 dp margins, pinned by `tools/xbar_gates.py`),
  preventing the 4-domain `Measured now: XBAR ... | SYS ... | VIDEO ... | MSVDD ...`
  label from truncating MSVDD on the right across 100%-200% display scaling.
- The outer window rectangle is clamped to `MONITORINFO.rcWork` on creation and
  after display or work-area changes.
- The per-user `[ui]` normal placement is committed with
  `main_window_placement_version=1` written last. Missing, partial, implausible,
  or off-screen state is ignored/clamped; minimized/maximized coordinates are
  never persisted in place of `WINDOWPLACEMENT.rcNormalPosition`.
- The minimum tracking size is itself capped by the current work area. A small
  or high-DPI display can therefore reach overflow mode instead of forcing the
  window behind the taskbar.
- All layout dimensions are physical pixels derived from the current window
  DPI. Fractional/custom DPI offsets are scaled individually; combined-offset
  rounding must not drift from actual child placement.
- The content canvas is never narrower than 1180 logical pixels. This preserves
  label/edit widths; narrow effective viewports scroll horizontally instead of
  truncating controls.
- **One side inset governs both gutters.** `MAIN_LAYOUT_SIDE_MARGIN_LOGICAL`
  (8 logical px) is the left inset every row starts at *and* the right inset
  every right-anchored control ends at. The column-count `sideAllowance` is
  derived from it (`2 *`), not written out again.
- **Right-anchored controls share one right edge.** The GPU selector, the
  fan-curve button, and the License button are all placed by
  `main_layout_right_anchored_x(contentWidth, controlWidth, margin, minX)` — a
  pure, physical-pixel decision — so they line up vertically at
  `contentWidth - margin` at every DPI and width. `minX` is an overlap floor
  against each one's left neighbour (the "GPU:" label, `hFanEdit` ending at
  `dp(998)`, the Reset button); none binds at the 1180-logical base canvas.
  Before this, the three used three different rules — a 12 px inset, a hardcoded
  `dp(1006)`, and the correct 8 px — and the fan-curve button never tracked
  `contentWidth` at all, so its gap to the edge grew on every wider window.
  Arguments are physical pixels on both sides so runtime callers can pass `dp()`
  values and tests can pass `main_layout_scale_px()` values without either
  caring which DPI source the other uses.
- VF columns range from the number actually needed up to a capacity of 12. The
  rows-per-column calculation and visible header count always agree.
- Graph height stays within 260..520 logical pixels. Its axes, labels, and full
  voltage/frequency domain remain visible at the minimum.
- Scrollbars are resolved iteratively because showing one changes the
  perpendicular client dimension and may require the other.
- When VF data first appears after an empty/loading-state window was sized, the
  outer window grows (never shrinks) toward the populated layout's preferred
  client height around its current center and is clamped to the current work
  area. Growth is deferred
  while minimized or maximized and applied on restoration. This prevents a
  vertical scrollbar from unnecessarily inducing a horizontal scrollbar.
- Actual overflow scrolling moves the parent pixels and all child HWNDs in one
  `ScrollWindowEx` operation, erases newly exposed pixels, and synchronously
  redraws the settled parent/child frame. Thumb tracking must not present a
  mixture of old pixels and newly positioned native controls.
- Tab/focus navigation posts `APP_WM_ENSURE_LAYOUT_FOCUS`; an off-screen focused
  child is scrolled into view without hiding any sibling controls.
- `WM_DPICHANGED` adopts the suggested monitor rectangle after work-area
  clamping, rebuilds DPI-dependent fonts, and performs a complete relayout.
- Initial system-DPI discovery accepts only a positive API result. Its legacy
  `GetDeviceCaps` fallback first requires a valid screen DC and otherwise keeps
  the safe 96-DPI baseline, so a rare resource failure cannot collapse scaling
  to zero.
- Ordinary main/dialog checkboxes use one 14-logical-pixel square metric. A
  checkbox does not become larger merely because it carries a caption; the box
  is the same square whether the control is labeled or not.
- **F-CHECKBOX-HIT** Every labeled checkbox owns its caption: the caption text
  is drawn by the owner-draw `BUTTON`, and the `BUTTON` is exactly
  `inset + box + gap + text + inset` wide
  (`ui_theme_labeled_checkbox_width()`), measured in the control's own font.
  Both bounds are deliberate. Clicking the caption, and the gap in between, must
  toggle the box — demanding pixel-accurate aim at a 14-logical-pixel square is
  bad targeting. But a button sized "comfortably wide" by eye turns the empty
  background right of the text into a silent toggle, which is worse than the
  small target because nothing there looks clickable, so the width is fitted
  rather than generous. The renderer and the fit read the same
  `ui_theme_checkbox_box_inset()` / `ui_theme_checkbox_label_gap()` metrics, so
  pixels and hit rectangle cannot drift apart. `draw_themed_button()` decides
  "labeled" purely from whether the control has text — there is no per-id list
  to keep in sync, and the VF lock checkboxes carry none (they also render
  through `draw_lock_checkbox()` and are deliberately untouched: their neighbours
  are the point label and the MHz edit, not a caption of their own).
- **A disabled STATIC cannot be themed, which is why the two main-window
  captions moved inside their buttons.** `hStartOnLogonLabel` and
  `hServiceEnableLabel` were `SS_LEFT | SS_NOTIFY` statics forwarding
  `STN_CLICKED`; user32 paints a disabled static in the system grey-text colour
  and ignores whatever `WM_CTLCOLORSTATIC` returns, so "Start program to tray on
  log in" was the only greyed caption in the window that did not match the
  others. The owner-draw renderer dims a caption to `COL_LABEL` like every other
  label instead. Both control ids (2036, 2038) are retired, not reused. Enabled
  captions now read `COL_TEXT`, matching their two sibling checkboxes rather than
  the surrounding field labels.
- **F-CHECKBOX-PAINT Every checkbox is `BS_OWNERDRAW`, so its tick is DERIVED on
  each paint and a repaint may only be gated on the value last PAINTED.** Button
  type bits are mutually exclusive: an owner-draw button is not a native
  checkbox, Windows stores no check state for it, `BM_GETCHECK` always answers
  `BST_UNCHECKED`, and `BM_SETCHECK` is a no-op. Gating on it made the projection
  directional — setting a tick compared "checked" against a permanent
  "unchecked", saw a difference, and invalidated; *clearing* one compared
  "unchecked" against "unchecked", concluded nothing had changed, and never
  invalidated. Unsharing a profile slot therefore left the tick on screen until
  the next full-window redraw or a GUI restart, even though the machine config
  was already correct (the user-visible "share slot N is bugged" report).
  `draw_themed_button()` now records what it painted into the
  `UiCheckboxState` returned by `themed_checkbox_painted_state()`
  (`shareAllUsersPainted` / `serviceEnablePainted` / `startOnLogonPainted` in
  `AppState`), and projections ask `ui_checkbox_state_needs_repaint()`. Recording
  in the *painter* rather than at the projection is what makes the mirror
  unable to drift: an out-of-band `RDW_ALLCHILDREN` redraw updates it too.
  `gui_set_button_check_if_changed()` is deleted, not fixed — it cannot be made
  correct for these controls.
- The fan-dialog point checkboxes and the VF lock checkboxes need no mirror:
  `g_fanCurveDialog.working.points[].enabled` and
  `g_app.lockedVi`/`lockMode` **are** their source of truth, so those paths
  invalidate after the model change instead. The corollary is an ordering rule —
  flip the model first, then repaint. Several sites did the reverse and worked
  only by accident of `InvalidateRect` being asynchronous.
- A caption that changes at runtime must re-fit its button, because a text
  change does not run the layout pass: `update_background_service_controls()`
  does it for "... (repair needed)" and `update_share_all_users_check_state()`
  for "Share slot N with all users". Without it the control would ellipsize its
  own label. Both calls are gated on an actual text change, so stable telemetry
  still touches nothing, and the fit uses `SWP_NOMOVE | SWP_NOZORDER |
  SWP_NOACTIVATE` — no `SWP_SHOWWINDOW`, so it cannot resurrect a tray-hidden
  window.
- The DPI path is safe by ordering: `reset_main_window_dpi_resources()` applies
  the rebuilt UI font to the children *before* the relayout, so the caption is
  measured in the font it will actually be drawn with.
- **F-ACTIONABLE** Control enablement is decided by
  `gui_service_capability_enabled()` from the state
  `gui_service_actionability_from_app()` snapshots. Two rules, in precedence
  order: **an escape hatch is never blocked**, and **everything whose purpose
  needs the service is greyed — except a control that already holds a
  service-dependent setting, which stays enabled so the setting can be undone.**
  - `PROFILE_EDIT` (Load, Save, Clear, the slot combo, the shared picker) and
    `AUTOMATION` (Auto-Profiles) require service **reachability** —
    `installed && available && !toggleInFlight`, never the protocol phase. Save
    captures live GPU state, and with none `populate_control_state_locked()`
    hardcodes `valid`/`hasPowerLimit` over all-zero fields, so it would persist
    `power_limit_pct=0`; Load and the picker write an editor that is disabled and
    blank; Clear and the slot combo exist only to target those.
  - `RECOVERY` — Refresh, the service-install checkbox, License — is
    **unconditionally true**. Greying any of them would leave no way out of an
    unavailable service.
  - `gui_service_config_control_actionable(state, assignmentPresent)` covers the
    four controls that *persist* something: share-with-all-users, apply-on-GUI-
    start, apply-on-logon, start-to-tray. Greyed at their default, enabled while
    set. **That exemption prevents a genuinely unreachable state, it is not
    politeness:** `startup_task_config_state()` keeps a Task Scheduler entry
    registered for any `logon_slot`/`logon_shared_slot`, `hLogonCombo` is the only
    GUI path that can clear one, and `clear_profile_from_config()` deliberately
    preserves `logon_shared_slot` — so deleting the profile is not an escape
    either. You cannot create a new service-dependent assignment while the service
    is unreachable, but you can always remove one you have.
  - `EDITOR` is `ready && draftAttached && !draftDetached`;
    `HARDWARE_MUTATION` is that plus `loaded`. Both variants existed as
    hand-written expressions; the three GUI-only copies now call the policy.
    `main_runtime_capture.cpp` and `main_fan_runtime.cpp` deliberately still do
    not — they compile into the service binary too and use a third
    `app_is_service_process() ? loaded : ready` form, and those two already disagree with
    each other. Migrating them would change unverifiable service-side behavior.
- **The gate is reachability, never the protocol phase.** `SYNCING`,
  `RECOVERING` and `DEVICE_MISSING` keep profile editing available on purpose: a
  profile loaded then is preserved as a pre-READY overlay in
  `GuiDraft::pendingDesired` and rebased onto the first coherent snapshot.
- Two mechanical hazards the gate has to respect. The shared-profiles button and
  the share-with-all-users checkbox have **two writers each** —
  `update_share_all_users_check_state()` owns both and runs *after*
  `update_profile_action_buttons()` inside
  `refresh_profile_controls_from_config()`, plus standalone from the share
  toggle — so both must use the identical expression or one silently re-enables
  the other. That owner is on no service-transition path at all, which is why the
  projection re-asserts both from `sharedProfileCountCache` /
  `machineProfileSlotSavedCache`. And `update_profile_action_buttons()` is now called from
  `gui_set_editor_enabled()`, which runs ~2x/second while not READY, so the
  per-slot saved state, the assignment flags and the machine-bank scan are all
  cached in `AppData` (invalidated alongside the tray cache on any config write)
  instead of costing INI reads per tick.
- Re-gating happens in `gui_set_editor_enabled()` (every phase change, transport
  failure and READY adoption) and in `gui_service_handle_admin_completion()`
  before its branch — `available` is cleared for all three outcomes there but
  only the uninstall branch reaches the phase-only render.
- Live status is shown only from an accepted coherent `READY` service envelope.
  `SYNCING`, device-missing, recovery, and degraded phases draw a neutral
  overlay, neutralize tray/live claims, and disable hardware actions. Dirty
  controls remain dimmed and explicitly labeled as a preserved unsaved draft.
- Dynamic VF controls are rebound only for a valid `READY` topology change.
  Topology equality includes every `visibleMap` entry, not merely the visible
  count. Rebuilds suppress parent/child redraw and finish with one coherent
  redraw transaction; reconnect phases never rebuild from partial data.
- The graph is a projection of the accepted model plus `GuiDraft`, never a
  parse of edit-control text. Draft data therefore survives destroyed/rebuilt
  child HWNDs without making those HWNDs an alternate source of truth. It draws
  up to two series: the **applied** curve solid in `COL_CURVE` (drift-free
  `appliedCurveMHz`, live readback for points Green Curve does not own) and,
  only while something is pending, the **pending** curve dashed in
  `COL_PENDING` with orange markers on exactly the changed points. With nothing
  pending the two are identical and only the applied one is drawn, so a clean
  window is pixel-identical to the pre-F-PENDING build.
- **The frequency axis is sized from the plotted data**, not a fixed
  500..3400 MHz band. `gui_graph_frequency_axis()` (pure rule in
  `gui_graph_axis_policy.h`) rounds `dataMax + 200` up to the next 500 MHz
  grid boundary for the ceiling and `dataMin - 200` down for the floor
  (clamped at 500 MHz), with a 1000 MHz minimum span. The scan covers both the
  applied and the pending series over the **visible VF list**
  (`g_app.visibleMap`, the same points the editor shows), so a high
  pending offset or flatten target never runs flat against the top edge and a
  low-clock curve does not float in empty space. The **voltage axis follows
  the same idea** on the 50 mV grid: `gui_graph_voltage_axis()` starts at the
  first visible point's voltage (rounded down, clamped to 700) and ends at the
  last visible point (rounded up, clamped to 1250), with a 300 mV minimum
  span, so a GPU whose curve only becomes meaningful at 750 mV does not waste
  the whole 700-750 mV cell. Hidden low points (voltage < 700 mV or base
  frequency < 500 MHz) are never plotted or labelled, and on-curve MHz labels
  that would spill left of the plot are skipped. The 500 MHz / 50 mV grids and
  labels are unchanged; the rounding uses true floor/ceil grid helpers
  (`gui_grid_floor()` / `gui_grid_ceil()`), so a negative pre-clamp value steps
  the mathematically correct way instead of truncating toward zero (the clamps
  are belt-and-braces, not the rounding's correctness). The resolved ranges are
  logged change-gated (`gui
  graph frequency axis: ... voltage ...`), so a reverted offset or a new GPU
  shows the axes following the data without spamming the log.  The y-axis
  title is drawn rotated bottom-to-top (font escapement 900) in the left
  margin, centred beside the labels, so "Frequency (MHz)" can never overlap
  the top clock number the way the old horizontal title at the top-left did.
- **The pending curve models the global GPU offset**, not just typed points. Two
  profiles can own *no* explicit curve points and still produce completely
  different curves purely through `gpu_offset_mhz` /
  `gpu_offset_exclude_low_count`. Mirroring
  [gpu_backend_apply.cpp](../source/gpu_backend_apply.cpp) exactly: an
  editor-owned point is written as an **absolute** target only under a
  **uniform** offset (no exclude-low on either side), so a uniform offset does
  not stack onto it; a **selective** offset (`gpuPolicyViaCurveBatch`, either
  side excludes low VF points) re-places **every** populated point through
  per-point deltas regardless of `guiCurvePointExplicit`. The selective mode
  itself is a pure mirror (`gui_pending_offset_mode_selective()`): the applied
  side consumes the resolver's effective exclude count (not re-gated on the
  applied offset, matching the backend's read of
  `current_applied_gpu_offset_excludes_low_points()`), and the pending side
  normalizes like the backend's desired settings (exclude counts count only
  while the offset is nonzero). The locked tail is pinned to the lock target
  regardless. Every point the offset path re-places
  is previewed at **stock + the pending offset component**, where stock is
  `curve_base_khz_for_point()` (`curve[ci] - freqOffsets[ci]`). Such a point
  currently *displays* stock + the **applied** component, so without this
  projection the preview silently misses every point a changed offset is about
  to move. This is the reported regression: an applied +475 MHz / exclude-first
  70 profile decodes into owned absolute curve points, and after that the old
  "owned point is absolute" rule hid every later offset or exclude-count
  retype. The per-point diff compares offset **components**, not frequencies,
  which keeps it exact and drift-free.
- **A hard NVML pin is a whole-curve flat line, not a tail flatten.** With
  `LOCK_MODE_HARD` the pending preview resolves **every** plotted point to the
  lock target (`GuiGraphPreviewInput.hardPinned`), the pending diff marks the
  whole visible curve (`gui_pending_mark_locked_tail()` starts at visible
  index 0), and the applied series returns `appliedLockFreq` for every point
  while `appliedLockMode == HARD`. The GPU really does run min=max at one
  clock, so the graph reads as the flat line it is instead of rising to the
  anchor. Both series use the light-blue `COL_CURVE_PINNED` in that state;
  default and flatten keep the normal applied green / pending orange. The
  point markers keep their red `COL_POINT` centers (only the ring follows the
  curve colour), and pending markers stay orange. Editor repopulation
  (`populate_desired_into_gui`) preserves the service's applied lock state, so
  a Save/Load cannot let live curve detection downgrade a hard pin to FLATTEN
  and leave the applied curve green until the next Refresh.
- **Releasing a point the applied state owned is itself a pending curve
  change.** A loaded profile that owns fewer VF points than the applied state
  does not leave the dropped points untouched: the GUI Apply always runs
  reset-before-apply, so those points return to stock (or to stock + the
  pending offset component when the new profile's offset covers them).
  `gui_pending_curve_point_changed()` now treats an applied-owned point the
  editor does not own as changed, and `GuiGraphPreviewInput.releasedToStock`
  makes the preview project it from `curve_base_khz_for_point()` instead of
  the stale draft value the previous profile left behind. Applied hard NVML
  pins fold into that ownership via `gui_applied_curve_mhz_for_pending()`: a
  hard pin owns every visible point even where `appliedCurveMHz` has no entry.
  This is the reported profile 3 (hard 547 MHz flat) -> profile 2
  (exclude-first-70) case: points 49-69 are released and must be dashed back
  to the stock curve, not silently left at 547.
- **The graph repaint gate is derived from what the graph draws, not from a
  hand-picked subset of its inputs.** `gui_pending_resolve_graph_preview()`
  resolves the **editor half** of every plotted point once per evaluation into
  `GuiPendingChanges::preview[]` (`GuiGraphPreviewPoint`: an absolute MHz the
  editor asserts, or "stock base + this offset component", or neither);
  `pending_curve_mhz_for_gui_point()` is now only the materialisation of that
  record against live readback, and `gui_pending_changes_equal()` compares it.
  The same value therefore decides *what is drawn* and *when it is drawn*.
  Before that, the gate was `curveOrLockFlipped` alone -- the `CURVE|LOCK` mask
  plus the changed-point **set** -- which missed every edit that moves values
  without moving the set: retyping the global GPU offset from +100 to +150
  shifts every unowned point but marks exactly the same ones, so nothing
  invalidated and the new curve appeared only once an unrelated repaint
  happened (clicking **Refresh**, which resyncs and calls
  `invalidate_main_window()`). Retyping an already-pending VF point (2900 ->
  2950) had the same hole. Changing the exclude-low count *does* move the set,
  which is why that one field looked healthy and made the bug read as
  offset-specific. The live half (`g_app.curve`, `curve_base_khz_for_point()`)
  is deliberately kept **out** of the record: it drifts under boost and
  temperature and the snapshot path already repaints for it, so folding it in
  would repaint the graph every telemetry tick.
- Because the graph now **reads a cached projection**, the rule that every
  editor mutation calls `gui_pending_changes_refresh()` is load-bearing rather
  than tidiness. Two lock transitions used to skip it: the same-point
  flatten -> hard-pin change, and unlock (`unlock_all()` refreshes while still
  clean and is then marked dirty again, which re-snapshots `GuiDraft`). Both
  reachable from the checkbox and from the right-click lock menu, and both left
  a stale Apply enable even before the graph depended on them. Gated in
  `tools/ui_gates.py`.
- **The lock frequency is compared within one VF grid step**, never exactly. The
  GPU can only represent frequencies on its own grid, so a requested lock target
  and the value the hardware then reports for it routinely differ by a step: a
  profile asking for 2957 MHz lands on 2962 and stays there, and
  `appliedLockFreq` can end up holding the settled value rather than the
  requested one. `gpu_backend_apply.cpp` already treats that as on target
  (`strict tail point ... (tol=8); keeping requested target`), so the diff uses
  the same `curve_point_verify_tolerance_mhz()` step. Comparing exactly left the
  editor **permanently dirty**: re-loading the very profile that was applied
  previewed a tail change forever, because re-applying can never close a gap the
  hardware cannot represent. `capture_gui_apply_settings()` now reaches its
  `lockChanged` verdict through the same `gui_pending_lock_changed()` policy, so
  a greyed Apply can never hide work the apply path would still do. The tolerance
  covers the frequency only -- a moved anchor or a changed mode is always a
  change.
- **`g_app.lockedFreq` is not the lock target.** `capture_gui_desired_settings()`
  prefers the draft value at the anchor whenever a draft is attached, so that is
  what Apply writes and what the preview, the headline `Lock:` text, and the
  pending diff must all use (`gui_pending_lock_target_mhz()`). `lockedFreq` lags
  in two known ways: `apply_lock()` infers a missing target from `GuiDraft`,
  which `populate_desired_into_gui()` fills in only *afterwards* -- so a profile
  projection adopted the previous stock value. A lock that **tracks its anchor**
  (`guiLockTracksAnchor`) is not an absolute target either: capture adds the
  anchor's GPU-offset component delta (pending minus applied) to the base, and
  the resolver mirrors it, clamped to ≥ 1 like capture. Without that, a freshly
  ticked flatten lock previewed the tail at stock while Apply wrote
  stock+offset. The VF MHz fields show the same projected values the graph
  draws (still F-PENDING orange): offset-shifted points display stock + the
  pending offset component while it moved (live readback again once reverted),
  and the locked region displays the resolved flat target. Under a **uniform**
  offset owned points and a retyped flatten anchor keep their field text, so a
  programmatic sync can never steal the caret mid-typing; under a **selective**
  offset owned points are projected too (the curve batch moves them), except
  for the one field the user is actively typing in. The locked-tail **drift
  diagnostic** still reads
  `lockedFreq` directly on purpose: it compares live readback against the
  applied lock, not the pending one.
- **A profile projection re-states the lock anchor field after `apply_lock()`.**
  `apply_lock()` runs `gui_pending_changes_refresh()` before
  `populate_desired_into_gui()` has recorded the profile's lock target in
  GuiDraft, so that refresh can write the PREVIOUS profile's anchor (e.g. 547)
  back into the anchor field. Later refreshes skip an absolute anchor
  (`guiLockTracksAnchor == false`) and therefore leave the stale value on
  screen while the graph preview shows the correct target (2957). The
  projection now re-states the profile's authoritative `lockMHz` in the field
  and seeds GuiDraft at the anchor immediately after `apply_lock()` returns,
  so the field and graph stay in agreement for every profile-load path.
- The pending curve is drawn **only across the stretches that actually differ**
  (`gui_pending_next_changed_run`), each expanded by one point so the dashed
  line departs from and rejoins the applied curve where the two are equal. The
  first implementation overlaid the complete pending curve, which covered the
  applied one along its whole length: loading a profile that changed two points
  painted all 128 orange. On-curve MHz labels are coloured per point for the
  same reason. The bug survived the first hardware pass because it *looks*
  correct whenever most points genuinely differ -- which is exactly what happens
  when the newly loaded profile owns more points than the applied one, and is
  why the two profile-load directions behaved differently.
- **F-PENDING** Anything typed or loaded but not yet applied is orange. One
  cached per-domain summary (`GuiPendingSummary`) drives both the field colours
  and the Apply enable, so the button and the colours can never disagree.
  Coverage is the whole editor: VF point MHz edits, GPU offset, exclude-low,
  memory offset, power limit, fan mode combo, fixed fan percent, the
  "Edit Curve..." button (the fan curve is edited in its own dialog, so this is
  its only visible surface), and the lock checkbox. `COL_PENDING_DIM` is used on
  disabled controls -- the locked-tail MHz edits are disabled and must still
  read as disabled. Backgrounds never change, so the cached `WM_CTLCOLOR*`
  brushes are reused and no GDI lifetime is added. Both colour handlers matter:
  Windows sends `WM_CTLCOLOREDIT` for an ordinary edit but `WM_CTLCOLORSTATIC`
  for a disabled or `ES_READONLY` one. The fan-mode combo is coloured through
  the static path, which works because the manifest declares no comctl32 v6
  dependency -- the same reason `style_combo_control()` picks `DarkMode_CFD`.
- **The F-PENDING predicate is conservative in exactly one direction.** It gates
  a button, so a false negative would strand the user: unparseable draft text
  therefore always counts as a pending change, and Apply stays pressable so the
  real validation error from `capture_gui_apply_settings()` is reachable. That
  function remains the authoritative backstop and keeps its "No changes to
  apply" path. Comparisons are against drift-free applied intent
  (`appliedCurveMHz`, `appliedLock*`, `appliedGpuOffset*`, the merged
  `ControlState`), never live readback -- the same rule the fan-only apply
  detection already follows, because boost/temperature drift in `g_app.curve`
  would otherwise light fields orange on its own and repaint every tick. A clean
  editor, a non-READY model, and a detached/unattached draft all short-circuit
  to an empty summary. Fan is skipped entirely on a GPU that reports no fans:
  Green Curve asserts no fan intent there and the controls are disabled, so the
  editor-vs-applied comparison has nothing to say.
- The pending refresh is change-gated: an identical summary returns before any
  `InvalidateRect`, and a real change invalidates only the controls whose bit
  flipped plus, for a curve or lock change, the graph band. Every genuine edit
  funnels through `gui_draft_capture_curve_value()` /
  `gui_draft_capture_text()`, which is why the six `EN_CHANGE` handlers need no
  per-handler hook; the fan mode combo, the fan curve dialog, the lock commands,
  and every state-adoption path call it explicitly.
- **The tray tooltip names the profile actually APPLIED to the GPU**, from
  `[profiles] applied_slot` -- the same authority the tray menu checkmark reads
  (`auto_profile_active_slot()`), so the tooltip and the tick directly next to
  it cannot disagree. The combo's `selected_slot` is an *editing* selection and
  must never be reported as active: loading a profile deliberately does not
  touch hardware, so with slot 2 applied and slot 1 loaded the tray used to
  claim "Profile 1". Shared and machine banks are named explicitly ("Shared
  profile N") rather than folded into the same-numbered personal slot, matching
  the rule `applied_user_slot_from_service_profile()` already enforces. A
  hand-typed Apply, and a user slot whose saved intent was edited away from what
  is running, both read "Manual settings"; nothing active reads "No profile". No
  `(saved)`/`(empty)` suffix -- an applied slot is saved by definition. The
  cached label is dropped whenever the applied-profile sync inputs change,
  because the service view can move without a config write (switching between
  two shared slots leaves `applied_slot` at 0).
- **The tray's OC/fan class treats an active lock as OC.** The live-state
  classification is now a pure rule (`gui_tray_live_state_has_custom_oc()`)
  covering GPU/memory offsets, power-limit changes, VF curve deltas, and an
  applied lock/pin (`appliedLockMode != NONE` with a resolved frequency). A
  hard NVML pin owns the clocks without writing per-point VF offsets, so a
  pinned-clock profile with a custom fan now reads as **OC + Custom Fan** and
  uses the `TRAY_ICON_STATE_OC_FAN` theme instead of plain **Custom Fan**.
- **The tick is decided from records only, never from live readback.** See
  [config-profiles.md](config-profiles.md#the-applied-indicator-must-be-drift-free)
  -- reported 2026-07-30 as "loading a profile without applying it makes the
  tray tick disappear", root-caused to the confirming profile read being
  projected onto the live VF curve. `applied_profile_indicator_policy.h` now
  owns both the read mode (`PROFILE_READ_FOR_OWNERSHIP`) and the decision
  table, and every evaluation logs its verdict plus the reason.
- **The tick's input comes from the service, and a delta Apply used to destroy
  it.** Also reported 2026-07-30: no tick at GUI start with a profile active, no
  tick after switching profile in the main window, a tick only after picking a
  slot from the tray menu. The GUI indicator was right every time -- the service
  had recorded `AD_HOC`/slot 0, because it proved a claimed slot against the
  request *payload* and a GUI Apply is deliberately a delta. See
  [config-profiles.md](config-profiles.md#the-service-must-not-lose-a-slot-identity-to-a-delta-apply).
  Nothing in this file's presentation changed; the authority feeding it did.

### F-INFLIGHT The transitional presentation while a write is running

Source anchors: `source/gui_apply_in_flight_policy.h`, `update_tray_icon()` in
`main_fan_runtime.cpp`, `build_tray_tooltip()` in `tray_presentation.cpp`,
`update_background_service_controls()` in `config_profiles_gui_state.cpp`,
`gui_apply_in_flight_presentation_changed()` in `gui_mutation_worker.cpp`.

An apply deliberately takes seconds -- reset to a stock OC baseline, let the
curve settle so expected NVIDIA boost drift cannot be mistaken for a failed
write, then write the new intent. Until 2026-07-30 nothing said so: the tray
kept the previous profile's theme and tooltip for that whole window, which reads
as "nothing happened".

- A fifth tray theme, `TRAY_ICON_STATE_PENDING`, is the default artwork in
  greyscale. It is *generated*, not hand-drawn: `tools/icon_render.py` derives
  it from `tray_default` by Rec. 709 luminance, so geometry and alpha stay
  byte-identical and it cannot drift away from the artwork it greys out.
- In-flight **outranks** the OC/fan themes and the tooltip's profile name.
  Mid-write neither the old nor the new OC/fan state is truthfully what the GPU
  has, so claiming either would be a lie for exactly as long as the answer is
  unknown.
- **A banner over the graph is the surface that actually gets noticed.** The
  first version of this feature shipped with only the tray tooltip and the
  background-service status line, and was reported as "I am not seeing any
  status indicator" — correctly. That status label is one clipped 18 px line
  placed ~370 dp into the service row near the bottom of the window, and the
  tray tooltip exists only while the cursor hovers the icon. The banner is
  painted after the phase overlay: a write can still be in flight while the
  model briefly leaves READY, and "the write is running" is the more urgent of
  the two.
- **The banner lives in the graph's scrolled CONTENT space, below
  `MAIN_LAYOUT_GRAPH_TOP_MARGIN_LOGICAL`** — never in client space. It was
  first pinned to client (0, 0) so it could not be scrolled out of view, and
  landed straight on the GPU selector row: reported as "the GPU selection is
  bleeding through the mask". Every control in this window is a **child**
  window, placed at `y - scrollY`, and a child paints *over* its parent instead
  of being clipped by it — so an overlap does not hide the control, it puts the
  control inside the banner. No client-pinned rectangle can avoid this in
  general either, because scrolling moves a different set of controls under it.
  Content coordinates below the graph's top margin are the one region
  guaranteed to hold no child control at any scroll position, and that margin
  is now a single constant shared with `draw_graph()` precisely so the two
  cannot drift apart. `gui_apply_in_flight_banner_band()` is pure and asserts
  the invariant (2259-2274) rather than leaving it to be eyeballed; a graph too
  short to hold the whole banner below the strip gets **no** banner, because
  shrinking it toward the top is what would put it back over the row.
- The banner carries an **indeterminate** sweep (`gui_apply_in_flight_sweep()`,
  pure and unit-tested). Indeterminate on purpose — an apply has no meaningful
  percentage, and inventing one would be a lie about progress. The sweep is
  driven by `APPLY_IN_FLIGHT_TIMER_ID` at
  `GUI_APPLY_IN_FLIGHT_FRAME_MS`, which is **presentation only**: the timer
  advances a frame counter and invalidates the banner rect, so a late,
  coalesced, or missed tick changes how the bar looks and nothing else. Nothing
  waits on it and nothing is sequenced by it. The timer runs only while the
  write does, and a tick that finds the flag already clear stops itself.
- Wording is split by how much room each surface has, all from the single
  `GUI_APPLY_IN_FLIGHT_PHRASE`: the tray tooltip reads
  `Green Curve - changes pending...`, the status line and the banner headline
  read `Applying settings to the GPU - changes pending`, and only the banner has
  the width for the second line explaining that the wait is deliberate.
- The palette gets its own `COL_INFLIGHT_*` entries rather than borrowing
  `COL_CURVE` (applied, green) or `COL_PENDING` (unapplied edit, orange). While
  the write runs neither is true — that is precisely the state the user cannot
  know yet — which is the same reasoning that makes the tray theme greyscale.
- Both transitions are driven from `gui_mutation_enqueue()` /
  `gui_mutation_acknowledge_and_dispatch_next()` -- the one point **every**
  apply path passes through (manual Apply, tray/hotkey profile pick, app-start
  apply), including a request that only dispatches once the active one finishes.
  Driving it from the individual apply paths instead is how one of them would
  eventually leave the icon stale.
- Do not confuse this with **F-PENDING** above. That is "typed into the editor
  but not applied yet" and is a GUI-side draft concept; F-INFLIGHT is "the
  hardware write is running". `gui_apply_in_flight_policy.h` says so at the top
  because the word "pending" is now used for both.
- Apply is greyed unless the summary reports something, **and while the write
  itself is running** (F-INFLIGHT-APPLY, fixed 2026-09-11).
  `GuiServiceActionability::hardwareWriteInFlight` carries `g_app.applyInFlight`
  into `gui_service_actionability_policy.h`, and
  `GUI_SERVICE_CAP_HARDWARE_MUTATION` is gated on it. Until then every other
  surface of F-INFLIGHT greyed out and the button did not: it stayed pressable
  for the whole multi-second write, and a second click queued a second apply the
  user had not asked for, against an editor baseline the first apply had just
  moved. The write was never unsafe -- `gui_mutation_queue_decide()` turns the
  second Apply into a PENDING one that replaces any earlier pending Apply, so
  there were never two concurrent writes -- the button simply lied.
  `gui_apply_in_flight_presentation_changed()` now calls
  `gui_pending_changes_refresh()` so the gate is re-asserted at BOTH
  transitions; the enable gate is only evaluated where something asks for it, so
  without that call the button would keep whatever it had when the write
  started. Reset is deliberately **not** gated -- returning the GPU to stock is
  meaningful with a clean editor, and `gui_mutation_queue_decide()` treats a
  Reset queued behind an active write as a safety action a later Apply can
  neither overtake nor discard, so greying it would remove the one escape hatch
  from an apply that is going badly. Refresh stays unconditionally enabled.
  Covered by regression 1605/1633-1639 and by source gates in
  `tools/ui_gates.py`.
- No legend is drawn inside the graph. Dashed-vs-solid plus the orange fields
  carry the meaning, and an always-visible caption was already rejected once as
  clutter (see the range-caption note above).
- **F-OC-HINT** The overclock row advertises its supported ranges in a hover
  tooltip on each of the three fields. The advertised bounds are the
  driver-reported range INTERSECTED with the caps the apply path enforces
  (`+/-1000` MHz GPU and `+/-3000` MHz memory from
  `validate_desired_settings_for_ipc()`, `50..150 %` power from
  `gpu_backend_apply.cpp`), because that intersection is what is actually
  accepted. The memory tooltip is explicitly advisory: F-DOM-1 applies memory
  offsets past the reported range on purpose. The power percent range is
  derived from the driver's milliwatt constraints (ceil the minimum, floor the
  maximum) and falls back to the enforced gate when the driver reports none.
- **Edit-control cue banners are unusable for these fields** and a build gate
  forbids reintroducing them. Windows paints a cue banner only while the
  control is empty; the GPU offset / memory offset / power limit edits always
  hold a value (`"0"`/`"0"`/`"100"` at creation, then repopulated from every
  service snapshot), so in-field placeholder text would never be visible.
- **The power limit edit is disabled when the board has no power control
  surface**, matching the GPU/memory edits' existing range-known gate:
  `populate_global_controls()` requires `power_limit_surface_available()` (see
  the power-target section in [gpu-backend.md](gpu-backend.md)). Boards whose
  driver refuses the power target — notebook boards whose TGP the OEM/EC owns —
  used to show an editable `0` that no Apply could ever honour, and the
  one-time reduced-control-surface dialog now names that case explicitly.
- A gray always-visible range caption under the overclock row was built first
  and then **removed**: with the tooltips carrying the same information it was
  redundant clutter in an already dense row. Do not reintroduce it without a
  reason the tooltips cannot serve.
- Tooltips are painted in the application palette (`COL_TOOLTIP_BG` /
  `COL_TOOLTIP_TEXT`) with the UI font, via `TTM_SETTIPBKCOLOR` /
  `TTM_SETTIPTEXTCOLOR`. Those messages are honored only while the control has
  no visual styles applied; the app manifest declares no comctl32 v6
  dependency, so that already holds, and `apply_tooltip_theme()` calls
  `SetWindowTheme(L"", L"")` to keep it true if a v6 dependency is ever added.
  Without this the tip renders in the system info-tip yellow, which looked like
  a foreign element in front of the dark window.
- Range hints are recomputed from the same snapshot that drives the edit-enable
  gates (`populate_global_controls`) and written only when the driver bounds
  actually change, so telemetry ticks cannot repaint them. The tooltip window
  does not survive a control rebuild, so `create_edit_controls` and the tooltip
  registration both drop the cached verdict.
- **F-THEMED-DIALOG** Confirmations and warnings go through `gc_message_box()`,
  not `MessageBox`. The stock box is painted by user32 from system colors with
  no way to influence them, so every prompt appeared as a bright light-mode
  window in front of the dark main window even with Windows set to dark. The
  replacement uses the application palette when the OS is dark and a standard
  light palette when it is not, matching what `apply_system_titlebar_theme()`
  already does for window chrome. Escape and the close button always resolve to
  the safest available answer -- `No` for a Yes/No prompt -- so dismissing a
  confirmation can never be read as agreeing to it. Two `MessageBoxA` calls in
  `entry.cpp` are deliberately left alone: they report that the window class or
  the main window could not be created, which is exactly when a custom window
  cannot be trusted. `ui_message_box.cpp` keeps the same fallback internally.
- **F-READ-MISS** A state read that does not come back **is not automatically a
  disconnect**. Until 2026-09-12 it always was, and the presentation cost was
  the whole window: the sync overlay, a disabled editor, blanked VF fields, the
  neutral tray theme, the applied-profile indicator dropping to `Manual
  settings`, and a full control rebuild (78 lock checkboxes re-subclassed, ~2 s)
  — all for a service that was busy and answered normally 19 seconds later. The
  transport now says whether it REACHED the service
  (`service_request_deadline_policy.h`), and `gui_service_stale_read.cpp`
  routes on that alone: a reachable service keeps every surface, and the
  service status line reads `Background service is busy; showing the last live
  GPU state. Values will update when it answers again.` The next successful read
  clears it where the fresh values land, not on a timer. Only an unreachable
  pipe reaches the disconnect presentation. The status-line branch sits below
  `applyInFlight`, the install/uninstall toggle and the not-installed case,
  which all outrank it, and it repaints only on the transition into staleness —
  repainting the service control surface once per missed read would be its own
  visible churn, which is the class of defect this fixes.
- **F-RESULT** A manual Apply / Reset **only interrupts the user when it has
  something to say**. Every manual mutation used to end in an OK-box, the
  ordinary success included; a confirmation that always appears is not
  information, it is a click, and it is precisely how the box that *does*
  matter becomes something to dismiss unread. A clean success is now confirmed
  on the profile status line — `Profile 3 applied.` / `Manual settings
  applied.` / `GPU settings reset to defaults.` — and raises nothing. A warning
  or an error still opens `gc_message_box()` with the service's own wording,
  and the status line names the outcome ahead of that wording so it survives
  being clipped (`Profile 3 applied with warnings: ...`, `Profile 3 was not
  applied: ...`).
  - **A successful hard NVML pin is clean by design.** The pin is verified by
    NVML itself (min=max locked clocks), so VF tail readback differences are
    diagnostics, not a warning. `service_apply_outcome_severity_for_lock_mode()`
    (`service_apply_severity_policy.h`) ignores the tail count for hard pins
    while boost-region partials and failed steps still matter. Without this,
    the old pin flow re-opened the redundant OK-box after every ordinary
    success even though the pin had worked.
  - The severity is **not** derived in the window. It arrives on the wire as
    protocol-v18 `ServiceOutcomeSeverity`, because only the apply backend can
    tell a fully verified write from one that committed while the driver
    declined some points — both answer `SERVICE_STATUS_OK`. See
    windows-architecture.md, "Outcome severity producer".
  - `gui_mutation_result_severity()` in `gui_mutation_result_policy.h` is the
    gate: a completion the window could not fully adopt (transport failure,
    changed service/GPU generation, an envelope it cannot attribute) is an
    ERROR here regardless of what the envelope claims. A zeroed response is
    `SUCCESS` by value, so this rule — not the field — is what stops a lost
    transport from reading as a silent success.
  - The queue line was `GPU operation started in the background`, which named
    neither the profile nor the fact that a result was still coming. It now
    reads `Applying Profile 3 to the GPU...` from the same vocabulary the
    completion uses (`gui_tray_format_active_profile()`, so the status line,
    the tray tooltip and the tray tick all name a profile the same way), and
    the completion overwrites it.
  - **An explicit tray/hotkey pick reports on that same line.** It was reported
    that switching profiles from the tray context menu left the status line
    describing whatever happened before it. A tray pick is a user action, so it
    now writes `Applying Profile 2 to the GPU...` when queued and the outcome
    when it lands — and `Profile 2 is already applied.` when the service
    already owns that exact intent, which is the case that previously looked
    like nothing happened at all. This does **not** breach
    F-PRESENTATION-SILENT: that contract forbids creating/showing a surface and
    taking activation or focus (the token list in build.py is the enforcement),
    and setting the text of a resident child label does neither. No dialog is
    ever raised from these paths.
  - **A rule-driven foreground switch stays silent**, hence the
    `service_apply_origin_is_explicit()` guard on all three sites rather than an
    unconditional write: the user did not ask for it, and it must not overwrite
    a line they are reading. Logon and app-launch applies keep their own
    handlers and are untouched.
- **F-OC-WARN** A manual Apply confirms once before RAISING a hand-typed clock
  past its threshold (defaults 200 MHz GPU, 2000 MHz memory; `[ui]`
  `high_oc_warn_gpu_offset_mhz` / `high_oc_warn_mem_offset_mhz`, `0` disables).
  All three conditions are load-bearing: profile-sourced values are the user's
  own reviewed intent and are exempt per field, and a value that does not
  exceed what is already applied is not a new risk. Automation never consults
  the policy at all, which is enforced by source guards as well as by the
  presentation-silent gate.

## Service status notices live in a tooltip (2026-09-23)

The background-service status label is one clipped 18 px line, but up to four
notices were appended to it in full (shared-profiles restriction; install
folder without file permissions / on a network share / writable by standard
users; user-profile install). On an unsafe install that was several hundred
characters and read as broken text. `source/service_status_notice_policy.h`
now composes `base + " Warning: a, b (hover for details)."` for the label and
puts the full sentences, one paragraph each, into the label's tooltip. The
tooltip is the shared themed main-window tooltip (`ui_oc_hints.cpp`,
`register_service_status_tooltip()`), re-registered on every editor rebuild
because the label outlives it. The tool is the label's RECTANGLE on the main
window (`service_status_tooltip_sync_rect()` after every layout pass), not the
label: a plain STATIC never sees the hover, and `SS_NOTIFY` is banned in
`entry.cpp` by the F-CHECKBOX-HIT gate. Tests 5900-5905; gates in `tools/ui_gates.py`
(`check_service_status_notices`). The handback notice ("Service stopped
unexpectedly; GPU may not be at stock. Press Reset.") goes to the profile
status line instead, once, when the v28 lockout reason appears
(`gui_service_state.cpp`). Not yet looked at on screen.

## The high-OC confirmation covers voltage too (F-05-001, 2026-09-15)

`oc_high_warning_policy.h` warns once before a manual Apply raises a hand-typed
value past a danger line, on three independent conditions per domain:
hand-typed (a profile-loaded value was already reviewed when saved), at or above
the threshold, and raising beyond what is currently applied. Automation
(auto-profile rules, hotkeys, tray picks, logon/app-launch) never consults it —
those paths are required to stay presentation-silent.

It covered the GPU and memory **clock** offsets only. The XBAR **MSVDD rail
voltage** offset — ±100 mV, written through a private, undocumented NvAPI
interface at a reverse-engineered field offset (`XBAR_MSVDD_OFFSET_FIELD`,
`+0x118`) — had no confirmation at all, while a +200 MHz core offset had one.
Voltage is the one domain here that can damage silicon rather than merely
destabilise it, so the coverage was exactly inverted. The XBAR dialog's only
guards were a ±100 mV range check and a static hint line; a hint is not a gate.

The domain is now in the policy (`msvddOffsetMv`, default threshold **25 mV** —
above the +10 mV the dialog's own hint recommends starting at, below a
vendor-typical safe step) with all three conditions preserved, configurable via
`ui/high_oc_warn_msvdd_offset_mv`, and `0` disables it like its siblings. The
message states the damage risk separately from the stability risk because they
are different kinds of risk, and the dialog title becomes "Confirm High
Overclock / Overvolt". Assertions 5270-5285.

Units: microvolts on the wire and in `OcApplyBaseline`, millivolts in the dialog
and the message; the conversion happens once, on the way into the pure policy.

Still uncovered, and a smaller risk by the same reasoning: the XBAR/SYS/VIDEO
**clock** offsets (±1000 MHz each) are stability risks, not damage risks, and
have no confirmation.

## GDI reconnect rules

GDI stays the rendering API, but every retained rendering resource is assumed
disposable after a GPU/display-stack transition. The full-client backbuffer is
a top-down 32-bit `CreateDIBSection` created with a null reference DC, so its
bits are process-owned system memory rather than a device-compatible bitmap
whose driver allocation may become stale after adapter reconnect.

The GUI advances a GDI generation and destroys the old backbuffer on selected-
GPU PnP invalidation, tray reopen, `WM_DISPLAYCHANGE`,
`WM_DWMCOMPOSITIONCHANGED`, and `DBT_DEVNODES_CHANGED`. A paint creates a fresh
surface for the current generation. It validates `CreateCompatibleDC`,
`CreateDIBSection`, `SelectObject`, and final `BitBlt`; if setup fails, it draws
the complete frame directly to the paint DC, and if the final blit fails, it
retires the surface for the next ordinary paint. No failure path posts a
self-sustaining repaint loop.

Ordinary telemetry is not a render transaction. When service/GPU authority,
active intent, topology, and control existence are unchanged, the GUI updates
only live fan/tray projections; it does not suspend painting, rewrite the VF
editor, or invalidate the parent/control tree. Structural changes still use one
suppressed transaction.

**That transaction never redraw-toggles the top-level window, visible or not.**
`DefWindowProc` implements `WM_SETREDRAW` by clearing (FALSE) and setting (TRUE)
`WS_VISIBLE` — that bit *is* the redraw flag — and the shell derives the taskbar
button, the Alt-Tab entry, and its z-order bookkeeping from exactly that bit. So
the old suppression pair read as a hide followed by a show. It produced two
separate reports: a tray-hidden owner resurrected as a ghost window
(2026-07-15), and — because that fix only skipped the toggle *while hidden* — an
open window dropping out of the taskbar window list for the length of every
structural projection, Refresh included (2026-07-31).

Suppression is therefore internal and touches no window style:
`g_guiTopLevelRedrawDepth` (defined in `main_runtime_gpu.cpp` beside the
invalidation helpers, because the service binary compiles them too) is raised
for the duration of the transaction; `WM_PAINT` validates its region without
drawing, `WM_ERASEBKGND` returns without filling, and `invalidate_main_window()`
records the invalidation instead of painting it
(`gui_window_invalidation_must_defer()`). The outermost transaction issues the
one settled `RedrawWindow`; nested transactions defer to it. Painting
synchronously still requires a window that was already on screen and is not
tray-hidden (`gui_top_level_redraw_may_paint_synchronously()`), and a final
pre-paint check preserves initial/explicitly hidden state before any redraw
flags run. The completed DIB is blitted directly without first painting the
physical window background, and redraw requests omit erase and non-client-frame
flags that expose intermediate pixels.

Refresh is a re-read, not a reconnect. `gui_service_request_resync()` routes it
through `gui_service_resync_decide()`: a coherent READY model with live
authority, re-read against the same GPU/service, keeps its whole presentation
and only the accepted completion (or a real transport failure) may change it.
Routing it through `gui_service_begin_full_sync()` instead dropped live
authority before the read was even sent, so one click flashed the
"Synchronizing GPU state" overlay and dropped the tray icon out of its OC/fan
theme to the neutral one — claiming for a round trip that no Green Curve
settings were in effect. The silent read reports itself through the profile
status line (`guiManualResyncPending`), and any real presentation transition
supersedes that line. A GPU-selector change or a service install/removal passes
`identityMayChange`, which can never be preserved.

Periodic reconnect probes are likewise presentation-silent. A failed probe
while already disconnected leaves the current overlay and child controls
untouched; only a visible phase/service/error change schedules rendering. This
is especially important when the service is not installed, because transport
diagnostics are not part of that fixed presentation. Control projection uses
change-gated text/check/enabled setters, invalidates without erase only after a
real change, and never calls `UpdateWindow` to expose an intermediate child
frame. Stable fan telemetry also checks combo selection, edit/button text, and
enablement before touching those native controls. Tray reopen preserves an
already coherent cached frame and queues an asynchronous telemetry refresh;
when the model is not READY it keeps the existing neutral overlay and retries a
full sync without another presentation transition.

Tray callbacks are classified according to whether `NIM_SETVERSION` actually
negotiated version 4. Version-4 `NIN_SELECT`/`NIN_KEYSELECT` and legacy mouse
notifications are mutually exclusive activation paths, and an already-visible
window ignores duplicate activation. A duplicate `--tray-start` process exits
without reopening the resident hidden window; an explicit foreground launch is
routed to the resident GUI thread and follows the normal reconnect-safe show.
Tray residency is a durable presentation intent, not an inference from the
transient `WS_VISIBLE` bit. While that intent is set, `WM_WINDOWPOSCHANGING`
converts unsolicited `SWP_SHOWWINDOW` requests into hidden/non-activating
placement, and display/device/exact-GPU PnP cues defensively reassert `SW_HIDE`.
Windows can also restore `WS_VISIBLE` through a display-reconstruction path
whose `WINDOWPOS` never carries `SWP_SHOWWINDOW`. Therefore every main-window
message checks the same durable postcondition and synchronously reapplies
`SW_HIDE` when visibility and hidden intent contradict each other. The
corrective `ShowWindow` path is reentrancy-guarded against its nested messages.
Only an explicit tray click/menu command or second foreground launch clears the
intent before showing. Accepted tray callbacks and corrective message/style
details are logged separately.

**Opening the tray context menu is not a window-state change (F-TRAY-MENU).**
The shell requires a foreground window in this process before
`TrackPopupMenu()`, or the popup survives the click that should dismiss it, and
`SetForegroundWindow()` *raises* its target as well as focusing it. Tracking the
menu against the main window therefore threw an open but occluded Green Curve
window in front of whatever the user was working in for as long as the menu was
up — a durable-hidden-intent window was unaffected, which is why this survived
the tray-residency work. The requirement is for *a* foreground window, not that
one, so `gui_tray_menu.cpp` owns a dedicated `GreenCurveTrayMenuOwner`: zero
sized, `WS_POPUP`, never shown, `WS_EX_TOOLWINDOW` only, and deliberately
*not* `WS_EX_NOACTIVATE` (being activated is its entire purpose). Its real
styles are read back and checked against `gui_tray_menu_owner_is_neutral()`
before it is kept; a rejected or uncreatable owner falls back to the main window
and logs the raise, because a tray-resident instance has no other way to quit.
Its own window class also keeps the single-instance
`FindWindowA(APP_CLASS_NAME)` lookup from ever landing on it. The popup is
tracked with `TPM_RETURNCMD | TPM_NONOTIFY` and the pick is posted on to the
main window as `WM_COMMAND` by hand, so command routing is unchanged; the owner
is given `allow_dark_mode_for_window()` *before* `refresh_menu_theme_cache()`
flushes the menu theme, the same order `WM_THEMECHANGED` uses, or the menu would
lose its dark palette. Menus opened from the main window's own Profiles button
still nominate that window: it is already foreground, so there is nothing to
raise. Tray/hotkey/automatic profile completion is
presentation-silent: it may update hidden child state and tray metadata but
cannot toggle top-level redraw, synchronously paint the owner, create/show a
dialog or window, take activation/focus, flash a taskbar surface, allocate a
console, or launch a helper process. This covers both observed display-reconnect
visibility paths and background profile completion without relying on a timer
or transient recovery phase.

The complete accepted-`READY` projection runs inside one programmatic-edit
transaction. Synthetic `EN_CHANGE`/selection notifications from nested HWND
updates therefore cannot manufacture a dirty draft that later preserves old OC
values across a service generation. When new authority has no active desired
intent, a clean editor is rebased from its authoritative live controls and the
status text explicitly says that no Green Curve settings are active.

## Diagnostics and failure modes

The default debug log records layout changes with DPI, viewport/content sizes,
graph height, columns, rows, overflow state, and scroll position. It separately
records work-area clamps, DPI changes, user/focus scroll transitions, populated
content-growth decisions (including current/preferred/work-area geometry and
deferred state), centered/restored startup placement, normal-placement commits,
and the rare direct-placement fallback if a `DeferWindowPos` batch fails.

Reconnect diagnostics are change-gated and include GUI/service phase,
connection epoch, service instance, GPU generation, revision, validity mask,
request/queue decision, stale-response rejection, topology signature, draft
attachment, render transaction, GDI generation, surface creation/selection,
and final-blit failures. These values contain no user document paths or other
sensitive payloads.

If a user still reports clipping, collect the `main layout:` and `main DPI
changed:` lines plus monitor resolution, Windows custom scale, taskbar edge and
size, and whether the window moved between monitors. Do not infer usable space
from nominal resolution alone.

## Regression coverage

`python build.py --test` compiles `main_layout_policy.h` into the pure harness.
It covers the reported 3440x1440/custom-140% geometry, wide-column reflow,
impossible 300%-DPI overflow, an empty/loading grid, and a Cartesian matrix of
10 DPIs, 8 widths, and 10 heights. A captured 4K/150%-DPI regression verifies
that changing from zero to 78 points grows to the preferred six-column,
13-row layout without overflow and that growth never shrinks a user-sized
window or exceeds the work area. Assertions also pin readable graph bounds,
column/row consistency, every populated point-cell boundary, monotonic section
boundaries, exact overflow flags, and fractional-DPI fit without hardware access
or timing assumptions. Pure cases also pin work-area centering, oversized
clamping, center-preserving growth, and identical labeled/unlabeled checkbox
box metrics at 150% DPI.

Right-anchored placement (716, 1660-1664) is asserted inside that same DPI x
width sweep: the GPU selector, the fan-curve button, and the License button each
end at exactly `contentWidth - margin`, that margin equals the point grid's own
left inset, and neither overlap floor is ever breached (the fan-curve button
stays clear of `hFanEdit`, the GPU selector of its "GPU:" label). Reinstating the
old 12 px inset in `main_layout_right_anchored_x()` exits 716.

Owner-draw checkbox repaint coverage (1650-1655) pins F-CHECKBOX-PAINT; see
`testing.md`. Emulating the old `BM_GETCHECK` gate exits 1652.

Labeled-checkbox hit-area coverage (1640-1646) pins F-CHECKBOX-HIT across 96 /
120 / 144 / 192 DPI: the caption starts exactly where the renderer draws it, the
control covers every caption pixel, nothing but the balancing inset follows the
caption, the box-to-caption gap is inside the control (no dead stripe), a wider
caption widens the control by exactly that much, and the fit stays below the
fixed widths it replaced. A captionless control still reserves the box instead
of collapsing, and a negative measurement cannot produce a sub-1-pixel width.
Source guards additionally forbid `SS_NOTIFY` in `entry.cpp` and `STN_CLICKED`
in `ui_main_window.cpp`, require both re-fit calls, and require the renderer and
the fit to read the same inset/gap metrics.

Overclock-row coverage (exit codes 1400-1435) pins the advertised ranges and
the high-overclock confirmation: driver windows wider than the IPC caps are
clipped, unreported and inverted windows report unknown rather than a fabricated
interval, power percent rounds inward and clips to the enforced gate (including
the divide-by-zero default), and each tooltip stays ASCII, fits its buffer,
names its own bounds, and keeps the advisory marker. Warn cases pin the
threshold boundary in both directions, the lowering-is-silent rule, the
per-field profile-load exemption, per-domain threshold disabling, the combined
two-domain message, and that a non-warning decision produces no message at all.

Unapplied-change coverage (1480-1519) pins the F-PENDING predicate: scalars,
curve points against the drift-free baseline, every lock transition, the domain
mask and the Apply gate, and -- twice, including a Cartesian sweep -- that
unparseable draft text can never resolve to "unchanged" and grey out Apply.
Source guards additionally forbid the pending diff from turning live curve
readback into a comparable value and forbid the pending/ctlcolor/graph shards
from scraping control text.

Graph-preview coverage (3195-3207, 4005-4009, 4027-4030) pins
`gui_graph_preview_point()`'s resolution order (locked tail, then a moved
offset component on an unowned point -- or on an owned point while a selective
offset re-places it -- then release-to-stock, then the draft value; an
unresolved lock target of 0 is not a request to plot 0 MHz) and
`gui_graph_preview_point_equal()` -- including the three cases the old repaint
gate could not see, where the marked set is identical but the plotted value
moved (offset +100 -> +150, an already-pending point 2900 -> 2950, a lock
retarget over the tail), plus that equality never reads the struct padding.
The release cases pin that an applied-owned point the editor no longer owns
projects from stock even when the stale draft still holds the old applied
value, carries the pending offset component when one applies, and yields to a
pending hard pin.

Themed message box coverage (1440-1471) pins every stock button set and its
default/escape mapping -- including that a Yes/No prompt resolves a dismissal to
`No`, that an out-of-range `MB_DEFBUTTON3` falls back to the first button rather
than indexing off the array, and that an unknown type degrades to a plain OK box
rather than to zero buttons -- plus the geometry: the icon gutter appears only
with an icon, the button group is right-aligned inside the margin, a wide button
row widens the dialog instead of pushing buttons off the left edge, margins
scale with DPI while the caller-supplied icon size does not, and a null input
yields an empty plan instead of a crash.

Reconnect coverage adds deterministic reducer/draft/queue cases; protocol-v13
topology signatures that distinguish same-count/different-map layouts and
valid `0 -> N` recovery; and hidden Win32 projection tests for overlay/control/
tray/action decisions, dirty-draft preservation, detached drafts, and one
coherent redraw. A native memory-DIB fixture verifies that the retained surface
can be selected and blitted without a device-compatible bitmap. Source guards
forbid synchronous runtime service calls from window paths and forbid transport
or worker code from mutating GUI globals/HWNDs. Pure tray-policy cases verify
version-4/legacy callback exclusivity, hidden-intent show suppression, and the
tray menu's foreground-owner neutrality (1155-1158: the main window fails
however it is styled, a visible/taskbar-listed/Alt-Tab-visible owner fails, and
so does a `WS_EX_NOACTIVATE` one that could not hold the foreground at all),
while render-policy cases verify that
stable telemetry is data-only and every authority/intent/topology change is
promoted to a full transaction. Source guards forbid pre-blit physical-window
clears and erase/frame redraw flags in coherent state adoption.

## Open questions / stale-risk

- A physical or virtual 3440x1440 Windows desktop at custom 140% DPI was not
  available locally. Exact geometry is compiled at 134 DPI and the runtime was
  exercised at a real 144-DPI monitor, but final native-control visual
  confirmation should come from the reporting machine. The local service was
  intentionally not installed merely to manufacture populated screenshots.
- The current minimum content width deliberately favors lossless horizontal
  scrolling over wrapping the dense global/profile rows. Revisit only if a
  future UI redesign introduces explicit responsive row groups.
- Native non-client scrollbar appearance follows Windows theming. Behavior and
  reachability are authoritative; cosmetic dark-scrollbar changes are optional.
- Repeated real Device Manager disable/enable, multi-GPU reorder, DWM/display
  transitions, occlusion/minimize/resize, and visible/hidden dirty-draft cycles
  remain release-machine integration checks. Deterministic state, native hidden
  HWND/GDI behavior, and all supported target compilation are automated; tests
  do not disable the developer's active display adapter.

Last verified: 2026-08-21 for the Advanced XBAR dialog's client-to-outer-frame
conversion (`F-XBAR-DIALOG` source gates), regression tests/source checks, and a
Windows x64 check build. The corrected bottom-row placement has not yet been
visually confirmed on screen.

Last verified: 2026-08-02 (build 547) for the profile ownership-release
preview (curve-diff releases 4024-4026, graph-preview release projections
4027-4030), against compiled pure regressions, the extended
`check_pending_changes` source gates (applied HARD-pin ownership folding,
`releasedToStock` outranking the stale draft), `--tidy` (38 baselined, no
new), and a full six-target build. **Not confirmed on hardware**: the reported
profile 3 -> profile 2 preview has not been re-watched on a real GPU.

Last verified: 2026-08-02 (build 548) for the lock-anchor field re-state
after `apply_lock()`'s refresh (profile 3 -> profile 2: the anchor VF MHz box
no longer keeps 547 while the graph previews 2957), against the new
`check_pending_changes` order gate, `--test`, `--tidy` (38 baselined, no new),
and a full six-target build. **Not confirmed on hardware**: the reporter's
profile-switch field has not been re-watched on a real GPU.

Last verified: 2026-08-02 (build 549) for the tray OC/fan classification
change (an applied lock/pin now counts as custom OC, so a pinned-clock +
custom-fan profile reads as "OC + Custom Fan" instead of "Custom Fan"),
against pure cases 4031-4036, the `check_apply_in_flight_presentation` source
gates, `--test`, `--tidy` (38 baselined, no new), and a full six-target build.
**Not confirmed on hardware**: the tray icon/tooltip for profile 3 has not
been re-watched on a real GPU.

Last verified: 2026-07-31 (build 532) for the visible-list plotting, the
data-driven voltage axis, and the label spill guard, against compiled pure
regressions (3222-3227), the extended `check_graph_frequency_axis` source gate
(visible-list plotting, voltage-axis rule, label spill guard), `--tidy` (38
baselined, no new), and a full four-target build.
**Not confirmed on hardware**: the high-clock (>3400 MHz) and low-clock graph
appearances have not been watched on a real GPU.

Last verified: 2026-07-31 (build 531) for the rotated y-axis title and the
data-driven VF graph frequency axis (`gui_graph_frequency_axis()`), against
compiled pure regressions (3215-3221), the `check_graph_frequency_axis` source
gate, `--tidy` (38 baselined, no new), and a full four-target build.
**Not confirmed on
hardware**: the high-clock (>3400 MHz) and
low-clock graph appearances have not been watched on a real GPU.

Last verified: 2026-07-31 (build 529) for the whole-VF-column pending
projection (`sync_vf_curve_field_values()`), against compiled pure regressions,
the updated `check_pending_changes` source gates (cached headline getter,
tracks-anchor consultation, graph-projection reuse in the field sync,
unconditional populate_edits hook), `--tidy` (38 baselined, no new), and a full
four-target build. **Not confirmed on hardware**: the reporter's +475 MHz /
925 mV flatten-lock gesture has not been re-run on a real GPU.

Last verified: 2026-08-02 (build 545) for the pure selective-mode mirror
(`gui_pending_offset_mode_selective()`, cases 4010-4016) and true floor/ceil
grid rounding (`gui_grid_floor()` / `gui_grid_ceil()`, cases 4020-4023),
against compiled pure regressions, the extended `check_pending_changes` /
`check_graph_frequency_axis` source gates, `--tidy` (38 baselined, no new),
and a full six-target build. **Not confirmed on hardware**: unchanged -- the
high/low-clock graph appearances and the +475 MHz flatten-lock gesture have
not been re-watched on a real GPU.

Last verified: 2026-07-31 (build 525) for the tray context menu's neutral
foreground owner (F-TRAY-MENU), against compiled pure regressions (1155-1158),
the new `check_tray_menu_does_not_raise_the_main_window` source gate (verified
to fail when `show_tray_menu()` nominates the main window again), `--tidy` (38
baselined, no new), and a full six-target build. **Not confirmed on screen**:
the menu's dismissal behaviour, its dark palette, and the absent raise were not
watched on a real tray right-click, only reasoned from the identical
already-shipping hidden-window path.

Last verified: 2026-07-31 (build 526) for the manual-result presentation
(F-RESULT) and the explicit tray/hotkey status line, against compiled pure
regressions (2305-2326), the new `check_manual_mutation_result_presentation`
source gate, `--tidy` (38 baselined, no new), and a full four-target build.
**Not confirmed on screen**: the protocol bump to v18 means an already-running
v17 service answers `VERSION_MISMATCH` until it is reinstalled or restarted, so
no real apply has been watched through the new dialog-free path.

Last verified: 2026-07-26 (build 458) for the labeled-checkbox hit area and the
themed greyed caption (F-CHECKBOX-HIT), against compiled pure regressions
(1640-1646), the new source guards in `tools/ui_gates.py`, and a full six-target
build. The on-screen result has NOT been confirmed: an older instance from
`C:\Program Files\greencurve` holds the single-instance mutex on this machine, so
the freshly built binary could not be shown.

Last verified: 2026-07-27 (build 460) for the owner-draw checkbox repaint gate
(F-CHECKBOX-PAINT) and the shared right edge, including the
`main_runtime_ui.cpp` -> `ui_theme_button.cpp` and share-toggle handler ->
`config_profiles_ui.cpp` splits, against compiled pure regressions (1650-1655,
716/1660-1664), both mutation-verified, the new `tools/ui_gates.py` guards, and
a full six-target build. The on-screen result has NOT been confirmed: an older
instance from `C:\Program Files\greencurve` holds the single-instance mutex on
this machine, so the freshly built binary could not be shown.

Last verified: 2026-07-26 (build 446) for the unapplied-change presentation
(F-PENDING), including the `ui_main.cpp` -> `ui_main_graph.cpp` and
`ui_main_window.cpp` -> `ui_main_ctlcolor.cpp` splits, against compiled pure
regressions (1480-1519), the source guards, and a full six-target build. The
orange field/graph rendering and the Apply greying have NOT yet been confirmed
on real hardware.

Last verified: 2026-07-26 (build 442) for the themed tooltips and the
OS-theme-aware `gc_message_box()` (F-THEMED-DIALOG), against compiled pure
regressions and a full six-target build. Confirmed on hardware in dark mode;
the light-mode palette has NOT yet been seen.

Last verified: 2026-07-25 (build 441) for the overclock range hints and the
high-overclock confirmation (F-OC-HINT / F-OC-WARN), including the
`ui_main_window.cpp` -> `ui_main_apply.cpp` split, against compiled pure
regressions and a full six-target build. The caption values, the tooltips, and
the dialog sequence have NOT yet been confirmed on real hardware.

Last verified: 2026-07-15 against the source anchors above, compiled layout/
theme/reconnect regressions, and Windows x64/ARM64 checks.
