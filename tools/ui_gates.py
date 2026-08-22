"""Source gates for the Windows GDI overclock row.

Split out of build.py so the build script stays under its size ratchet; build.py
owns the CLI and passes its paths and check helpers in through `ctx`.  Nothing
here imports build.py, so the dependency runs one way only.

`ctx` is any object exposing SOURCE_DIR.
"""
import os


def _p(ctx, name):
    return os.path.join(ctx.SOURCE_DIR, name)


# check_all() is handed the two checks every gate needs.  The braced-region
# variants are rarer, so they are read off `ctx` (the live build module) rather
# than widening that signature for every caller.
def _require_in_operation(ctx, path, anchor, needle, label):
    ctx.require_text_in_operation(path, anchor, needle, label)


def _forbid_in_operation(ctx, path, anchor, needle, label):
    ctx.forbid_text_in_operation(path, anchor, needle, label)


def check_overclock_range_hints(ctx, require_text, forbid_text):
    """F-OC-HINT: the supported ranges are advertised before the user types.

    The advertised bounds are the driver range INTERSECTED with the IPC and
    apply-path caps, because that intersection is what actually gets accepted;
    advertising the raw driver range would promise values the service then
    rejects with "outside the supported range".

    EM_SETCUEBANNER is deliberately unused.  Windows paints a cue banner only
    while an edit control is empty, and the GPU offset / memory offset / power
    limit fields always hold a value ("0"/"0"/"100" at creation, then
    repopulated from every service snapshot), so in-field hint text would never
    be visible.  The ranges therefore live in hover tooltips, which are painted
    in the application palette rather than the system info-tip yellow.
    """
    policy_h = _p(ctx, "oc_range_hint_policy.h")
    hints_cpp = _p(ctx, "ui_oc_hints.cpp")
    ui_main_cpp = _p(ctx, "ui_main.cpp")
    capture_cpp = _p(ctx, "main_runtime_capture.cpp")

    require_text(policy_h, "OC_RANGE_IPC_GPU_OFFSET_ABS_MHZ",
                 "advertised GPU offset range is capped by the IPC bound")
    require_text(policy_h, "OC_RANGE_IPC_MEM_OFFSET_ABS_MHZ",
                 "advertised memory offset range is capped by the IPC bound")
    require_text(policy_h, "oc_range_power_pct",
                 "power limit percent range is derived from the driver "
                 "milliwatt constraints")
    require_text(policy_h, "Advisory only:",
                 "memory hint stays advisory because F-DOM-1 applies beyond "
                 "the reported range")
    require_text(hints_cpp, "oc_range_hints_invalidate",
                 "range hints drop cached state when the controls are rebuilt")
    require_text(hints_cpp, "TTM_UPDATETIPTEXTA",
                 "range tooltips are refreshed in place when the bounds change")
    require_text(hints_cpp, "TTM_SETTIPBKCOLOR",
                 "tooltips are painted in the application palette")
    require_text(hints_cpp, "TTM_SETTIPTEXTCOLOR",
                 "tooltip text color is set alongside the background")
    forbid_text(hints_cpp, "EM_SETCUEBANNER",
                "cue banners never paint on these always-populated edits")
    require_text(ui_main_cpp, "apply_tooltip_theme(tip);",
                 "every rebuilt tooltip window is themed before it is shown")
    require_text(capture_cpp, "refresh_oc_range_hints();",
                 "range hints follow the snapshot that drives the enable gates")


def check_high_overclock_confirmation(ctx, require_text, forbid_text):
    """F-OC-WARN: manual Apply confirms once before RAISING a hand-typed clock.

    All three conditions are load-bearing.  Profile-loaded values are the user's
    own already-reviewed intent, so they are exempt; a value that does not
    exceed what is already applied cannot be a new risk, so it stays silent.
    Automation must never raise the dialog, so it must never consult the policy
    either -- that is enforced here as well as by the presentation-silent gate.
    """
    policy_h = _p(ctx, "oc_high_warning_policy.h")
    apply_cpp = _p(ctx, "ui_main_apply.cpp")
    profiles_ui_cpp = _p(ctx, "config_profiles_gui_state.cpp")
    window_cpp = _p(ctx, "ui_main_window.cpp")

    require_text(policy_h, "OC_HIGH_WARN_DEFAULT_GPU_OFFSET_MHZ",
                 "high-overclock warning has a default GPU offset threshold")
    require_text(policy_h, "OC_HIGH_WARN_DEFAULT_MEM_OFFSET_MHZ",
                 "high-overclock warning has a default memory offset threshold")
    require_text(policy_h, "return value > current;",
                 "high-overclock warning only fires when the clock is raised")
    require_text(policy_h, "if (!present || !handTyped) return false;",
                 "high-overclock warning exempts profile-sourced values")
    require_text(apply_cpp, "oc_high_warn_decide(",
                 "manual Apply consults the high-overclock policy")
    require_text(apply_cpp, "high_oc_warn_gpu_offset_mhz",
                 "high-overclock thresholds are configurable per user")
    require_text(profiles_ui_cpp, "g_app.guiGpuOffsetFromProfileLoad = true;",
                 "profile population marks the GPU offset as profile-sourced")
    require_text(profiles_ui_cpp, "g_app.guiMemOffsetFromProfileLoad = true;",
                 "profile population marks the memory offset as profile-sourced")
    require_text(window_cpp, "g_app.guiGpuOffsetFromProfileLoad = false;",
                 "hand-editing the GPU offset clears its profile provenance")
    require_text(window_cpp, "g_app.guiMemOffsetFromProfileLoad = false;",
                 "hand-editing the memory offset clears its profile provenance")
    forbid_text(_p(ctx, "auto_profile_win32.cpp"), "oc_high_warn_decide",
                "auto-profile applies never raise the high-overclock dialog")
    forbid_text(_p(ctx, "main_startup_profiles.cpp"), "oc_high_warn_decide",
                "logon/app-launch applies never raise the high-overclock dialog")


def check_manual_apply_origin(ctx, require_text):
    """The GUI Apply button stamps its own origin, distinct from automation.

    The service accepts only a whitelist of client origins, and the completion
    router uses the origin plus the UI context to decide between a visible
    result and a silent one, so the manual path must keep declaring itself.
    """
    require_text(_p(ctx, "ui_main_apply.cpp"), "SERVICE_APPLY_ORIGIN_GUI",
                 "GUI Apply uses its explicit origin")
    require_text(_p(ctx, "ui_main_apply.cpp"),
                 "GUI_MUTATION_CONTEXT_MANUAL_APPLY",
                "GUI Apply queues under the manual UI context")


def check_auto_profile_log_privacy(ctx):
    """Auto-profile diagnostics must not become a window/application ledger."""
    auto_cpp = _p(ctx, "auto_profile_win32.cpp")
    with open(auto_cpp, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    for needle in ("gc_log_identifier_token(fg.exeName",
                   "gc_log_identifier_token(fg.className"):
        if needle not in text:
            print("Regression source check FAILED: "
                  "auto-profile foreground identity is not fingerprinted")
            sys.exit(1)
    if "exe='%.40s'" in text or "class='%.40s'" in text:
        print("Regression source check FAILED: "
              "auto-profile logs raw foreground application identity")
        sys.exit(1)


def check_themed_message_box(ctx, require_text, forbid_text):
    """F-THEMED-DIALOG: confirmations follow the OS theme, like the title bars.

    The stock message box is painted by user32 from system colors with no way to
    influence them, so every prompt appeared as a bright light-mode window in
    front of the dark main window even with Windows set to dark.  GUI shards
    therefore call gc_message_box instead.  The two exceptions in entry.cpp are
    deliberate: they report that the window class or the main window itself could
    not be created, which is exactly when a custom window cannot be trusted.
    """
    box_cpp = _p(ctx, "ui_message_box.cpp")
    policy_h = _p(ctx, "message_box_policy.h")

    require_text(policy_h, "message_box_button_set",
                 "button sets, default, and escape mapping are a pure decision")
    require_text(policy_h, "set.escapeId = GC_ID_NO;",
                 "dismissing a Yes/No prompt answers No, never Yes")
    require_text(box_cpp, "is_system_dark_theme_active()",
                 "the message box palette follows the OS theme")
    require_text(box_cpp, "apply_system_titlebar_theme(hwnd);",
                 "the message box title bar follows the OS theme")
    require_text(box_cpp, "IsDialogMessageA(hwnd, &msg)",
                 "the modal loop keeps Tab/Enter/Escape keyboard navigation")
    require_text(box_cpp, "PostQuitMessage((int)msg.wParam);",
                 "a quit during the modal loop is reposted, not swallowed")
    require_text(box_cpp, "return MessageBoxA(owner, text, caption, type);",
                 "the themed box falls back to the system box if it cannot build")

    # Every GUI shard that prompts must go through the themed box.  entry.cpp is
    # excluded on purpose (see the docstring); ui_message_box.cpp holds the
    # fallback itself.
    for name in ("ui_main_window.cpp", "ui_main_apply.cpp", "ui_main.cpp",
                 "ui_mutation_completion.cpp", "config_profiles_gui_state.cpp",
                 "gui_service_state.cpp", "fan_curve_dialog.cpp",
                 "auto_profile_dialog.cpp", "auto_profile_win32.cpp",
                 "main_gpu_state.cpp"):
        forbid_text(_p(ctx, name), "MessageBoxA(",
                    f"{name} prompts through the themed gc_message_box")


def check_pending_changes(ctx, require_text, forbid_text):
    """F-PENDING: unapplied edits are orange, and Apply greys out without them.

    One predicate feeds both the field colouring and the Apply enable so the
    button and the colours can never contradict each other.  That makes the
    predicate load-bearing for reachability, hence the two invariants pinned
    here.

    First, unparseable draft text must always count as a pending change.  If a
    half-typed value ever resolved to "unchanged", Apply would grey out mid-edit
    and the user could never reach the real validation error that
    capture_gui_apply_settings() produces -- which remains the authoritative
    backstop, so its "No changes to apply" path stays in place.

    Second, the diff compares against drift-free applied INTENT.  The NVIDIA VF
    curve legitimately shifts under boost and temperature; comparing the editor
    against live readback (g_app.curve) would light points orange on their own
    and repaint the window on every telemetry tick.  This is the same rule
    capture_gui_apply_settings() already follows for fan-only detection.
    """
    policy_h = _p(ctx, "gui_pending_changes_policy.h")
    pending_cpp = _p(ctx, "ui_pending_changes.cpp")
    ctlcolor_cpp = _p(ctx, "ui_main_ctlcolor.cpp")
    graph_cpp = _p(ctx, "ui_main_graph.cpp")
    state_cpp = _p(ctx, "gui_service_state.cpp")
    lock_checkbox_cpp = _p(ctx, "ui_lock_checkbox.cpp")
    capture_cpp = _p(ctx, "main_runtime_capture.cpp")
    ui_main_cpp = _p(ctx, "ui_main.cpp")

    require_text(policy_h, "if (!in.draftValid) return true;",
                 "unparseable draft text always counts as a pending change, so "
                 "a typo can never grey out Apply")
    require_text(policy_h, "gui_pending_apply_button_enabled",
                 "the Apply enable is a pure decision over the same summary "
                 "that colours the fields")
    require_text(policy_h, "return mutationReady && gui_pending_any(summary);",
                 "pending changes only remove Apply; they never override the "
                 "service/draft readiness gate")

    require_text(pending_cpp, "g_app.appliedCurveMHz[ci]",
                 "curve points are diffed against drift-free applied intent")
    require_text(policy_h,
                 "if (!in.ownedByEditor) return in.appliedMHz != 0;",
                 "a point the applied state owns but the editor releases is a "
                 "pending change (profile ownership release back to stock)")
    require_text(pending_cpp, "gui_applied_curve_mhz_for_pending",
                 "the pending diff folds an applied HARD NVML pin into "
                 "per-point applied ownership")
    require_text(pending_cpp, "in.releasedToStock =",
                 "the graph preview projects released points from the stock "
                 "base instead of a stale draft/applied value")
    require_text(policy_h,
                 "if (in.releasedToStock && !in.ownedByEditor) {",
                 "release-to-stock outranks the stale draft in the pure graph "
                 "preview policy")
    # The existence check (freq_kHz != 0) is fine and mirrors the apply diff;
    # what must never appear is the live frequency as a comparable VALUE, which
    # is what displayed_curve_mhz() produces.
    forbid_text(pending_cpp, "displayed_curve_mhz(",
                "the pending diff never compares against live curve readback, "
                "which drifts under boost and temperature")
    require_text(pending_cpp, "bool identical = gui_pending_changes_equal(",
                 "the refresh is change-gated so stable telemetry repaints "
                 "nothing")
    require_text(pending_cpp, "if (!gui_state_dirty()) return;",
                 "a clean editor is rebased from applied state and has nothing "
                 "pending")

    # The pending state comes from GuiDraft and the applied baselines, never
    # from the controls themselves -- the same rule the graph already follows.
    for path in (pending_cpp, ctlcolor_cpp, graph_cpp):
        forbid_text(path, "GetWindowTextA(",
                    "F-PENDING presentation reads GuiDraft, not control text")

    require_text(state_cpp, "gui_pending_changes_refresh();",
                 "the draft mutators re-evaluate the pending state, so every "
                 "EN_CHANGE and profile load is covered by one hook")
    require_text(state_cpp, "gui_pending_apply_button_enabled(allowDraft",
                 "the editor-enable pass gates Apply on pending changes")

    require_text(graph_cpp, "applied_curve_mhz_for_gui_point",
                 "the graph draws the applied curve as its own series")
    require_text(graph_cpp, "pendingColor, true);",
                 "the pending curve is drawn dashed, so the two series stay "
                 "distinguishable without relying on colour alone")
    require_text(graph_cpp, "gui_pending_next_changed_run(pendingChangedForPt",
                 "the pending curve covers only the stretches that differ; "
                 "drawing it full-length hid the applied curve everywhere")
    require_text(graph_cpp, "pendingChangedForPt[i] ? pendingColor : COL_TEXT",
                 "on-curve MHz labels are coloured per point, not per graph")
    require_text(graph_cpp, "COL_CURVE_PINNED",
                 "a hard NVML pin recolours the graph curve so it cannot be "
                 "mistaken for the normal applied green")
    require_text(graph_cpp, "appliedLockMode == LOCK_MODE_HARD",
                 "only an applied hard pin recolours the solid curve; flatten "
                 "and default keep the normal curve colour")
    require_text(graph_cpp, "appliedColor, COL_POINT);",
                 "applied VF point markers keep their red centers; only the "
                 "ring follows the curve colour")
    require_text(graph_cpp, "COL_PENDING, COL_PENDING);",
                 "pending point markers stay orange even when the pending "
                 "curve itself is drawn in the pinned colour")

    # A global GPU offset moves points nobody typed. Apply re-places an unowned
    # point at stock + the new offset component (gpu_backend_apply.cpp), so the
    # preview has to project it the same way instead of showing the value it
    # currently holds under the offset that is still applied.
    require_text(policy_h, "gui_pending_offset_shift_changed",
                 "a changed global GPU offset marks the points it moves, even "
                 "though none of them was typed")
    require_text(policy_h, "if (in.ownedByEditor && !in.selectiveOffsetActive) return false;",
                 "the editor-owned carve-out applies only to uniform offsets; "
                 "a selective offset re-places owned points through the curve "
                 "batch and must still preview the move")
    require_text(pending_cpp, "gui_pending_offset_mode_is_selective",
                 "the pending diff derives the selective/curve-batch offset "
                 "mode from both applied and pending exclude state")
    require_text(policy_h, "gui_pending_offset_mode_selective(",
                 "the selective/curve-batch offset mode is a pure, "
                 "unit-tested mirror of gpuPolicyViaCurveBatch")
    require_text(pending_cpp, "return gui_pending_offset_mode_selective(",
                 "the GUI wrapper delegates the selective mode to the pure "
                 "policy instead of re-deriving it")
    require_text(policy_h, "int appliedExcludeLow = appliedExcludeLowCount;",
                 "the applied side consumes the resolver's effective exclude "
                 "count instead of re-gating on a nonzero applied offset, "
                 "matching gpuPolicyViaCurveBatch")
    require_text(policy_h, "pendingGpuOffsetMHz != 0",
                 "the pending side normalizes exclude counts like the "
                 "backend's desired settings")
    forbid_text(pending_cpp, "out->appliedGpuOffsetMHz != 0",
                "the applied side of the selective mirror must not re-gate on "
                "the applied offset; the resolver count is already effective")
    require_text(pending_cpp, "shift.selectiveOffsetActive = gui_pending_offset_mode_is_selective(out);",
                 "the changed-point diff marks owned points under a selective "
                 "offset, matching gpuPolicyViaCurveBatch")
    require_text(pending_cpp, "in.offsetMovesOwnedPoints = gui_pending_offset_mode_is_selective(out);",
                 "the graph preview projects owned points through a selective "
                 "offset, matching the curve batch")
    require_text(pending_cpp, "gui_pending_mark_gpu_offset_shift",
                 "the pending diff accounts for the global GPU offset")
    require_text(graph_cpp, "curve_base_khz_for_point(ci)",
                 "an offset-moved point is projected from stock, matching the "
                 "apply path rather than its current displayed value")

    # The graph repaint gate has to move when the plotted VALUE moves, not only
    # when the SET of pending points moves.  Retyping the global GPU offset from
    # +100 to +150 shifts every unowned point while marking exactly the same
    # ones, so a gate built on the mask and the changed-point set alone never
    # invalidated the graph and the preview waited for an unrelated repaint.
    # The fix is structural: the editor half of every plotted point is resolved
    # once, the graph reads it, and the gate compares it -- so a future preview
    # input cannot be added to one and forgotten in the other.
    require_text(policy_h, "gui_graph_preview_point_equal",
                 "what the graph draws for a point is one comparable record, "
                 "so the repaint gate cannot drift from the renderer")
    require_text(pending_cpp, "gui_pending_resolve_graph_preview(out);",
                 "the graph preview is resolved into the pending model on every "
                 "evaluation path, including the clean-editor early returns")
    require_text(pending_cpp, "if (curveOrLockFlipped || previewMoved > 0)",
                 "the graph repaints when the pending VALUES move, not only "
                 "when the pending presentation flips")
    require_text(graph_cpp, "gui_pending_graph_preview(ci)",
                 "the graph plots the resolved preview instead of re-deriving "
                 "it behind the repaint gate's back")
    forbid_text(graph_cpp, "g_app.guiCurvePointExplicit",
                "point ownership is resolved once into the preview; a second "
                "copy of that rule in the renderer is what the gate misses")

    # The graph reads a CACHED projection, so the rule that every editor
    # mutation re-evaluates it is load-bearing, not tidiness.  The two lock
    # transitions that only flip the mode (or unlock, which refreshes while
    # still clean and is then marked dirty again) used to return without the
    # hook, leaving a stale Apply enable -- and, once the graph became a reader,
    # a stale curve.
    require_text(lock_checkbox_cpp, "gui_pending_changes_refresh();",
                 "the lock checkbox re-evaluates the pending state after its "
                 "dirty transition re-snapshots GuiDraft")

    # g_app.lockedFreq lags a profile projection: apply_lock() infers it from
    # GuiDraft, which populate_desired_into_gui() fills in only afterwards. The
    # preview, the diff, and Apply must therefore resolve the lock target the same
    # way capture_gui_desired_settings() does -- draft anchor first.
    require_text(policy_h, "gui_pending_lock_target_mhz",
                 "the lock target is resolved by one pure decision")
    require_text(policy_h, "in.hardPinned",
                 "a hard pin flattens the whole preview, not only the locked "
                 "tail")
    require_text(pending_cpp, "hardPinWholeCurve",
                 "a hard pin marks the whole visible curve pending because "
                 "min=max locked clocks change every plotted point")
    # A lock that TRACKS its anchor is not an absolute target: Apply adds the
    # anchor's GPU-offset component delta (pending minus applied) to the base,
    # and the preview must do the same or the flat tail draws at stock while
    # Apply writes stock+offset.  Equal components mean no adjustment, and the
    # clamp mirrors capture's `<= 0 -> 1`.
    require_text(policy_h, "appliedOffsetComponentMHz == pendingOffsetComponentMHz",
                 "a tracking lock adjusts only when the anchor's GPU-offset "
                 "component actually moved")
    require_text(policy_h, "if (target <= 0) target = 1;",
                 "the tracking-lock adjustment clamps to 1 like "
                 "capture_gui_desired_settings()")
    require_text(pending_cpp, "g_app.guiLockTracksAnchor",
                 "the resolved lock target consults tracks-anchor, so the "
                 "preview matches Apply's offset-adjusted target")

    # The apply path and the editor's diff must reach the same lock verdict, or a
    # greyed Apply could hide work the apply path would still do -- and both need
    # the VF-grid tolerance, without which re-loading the applied profile looked
    # permanently changed.
    require_text(policy_h, "return (hi - lo) > in.toleranceMHz;",
                 "the lock frequency is compared within one VF grid step")
    require_text(capture_cpp, "bool lockChanged = gui_pending_lock_changed(lockState);",
                 "the apply path reaches its lock verdict through the same "
                 "policy as the editor's pending diff")
    require_text(capture_cpp, "curve_point_verify_tolerance_mhz(",
                 "the apply path allows the same VF grid step the verify pass "
                 "already treats as on target")
    require_text(pending_cpp, "gui_editor_lock_target_mhz",
                 "the pending lock diff uses the resolved lock target")
    require_text(graph_cpp, "gui_pending_graph_lock_target_mhz()",
                 "the headline lock line reads the cached resolved lock target "
                 "the drawn tail uses, not a re-derived value")
    forbid_text(graph_cpp, "gui_editor_lock_target_mhz",
                "the renderer must not re-derive the lock target; it reads the "
                "cached projection like the rest of the graph")
    # Precise on purpose: the remaining g_app.lockedFreq reads in the graph are
    # the locked-tail DRIFT diagnostic, which legitimately compares live readback
    # against the applied lock. Only returning it as a previewed value is wrong.
    forbid_text(graph_cpp, "return g_app.lockedFreq;",
                "the previewed tail no longer returns g_app.lockedFreq directly")
    # The VF MHz boxes show the same pending values the graph draws (offset-
    # shifted unowned points, the resolved flat lock target).  The pending
    # refresh is the single hook every editor mutation and lock transition
    # funnels through, and the helper must live in the control layer that owns
    # set_edit_value so a programmatic write can never re-enter the draft.
    require_text(pending_cpp, "sync_vf_curve_field_values();",
                 "the pending refresh syncs the VF MHz boxes whenever the "
                 "pending model actually changed")
    require_text(ui_main_cpp, "static void sync_vf_curve_field_values",
                 "the VF-field sync is defined once, in the control layer "
                 "next to set_edit_value")
    require_text(ui_main_cpp, "pending_curve_mhz_for_gui_point",
                 "the field sync projects offset-shifted points through the "
                 "same resolved preview the graph draws, so a pending GPU "
                 "offset is visible in the fields, not only in the graph")
    require_text(ui_main_cpp,
                 "gui_pending_offset_mode_is_selective(&g_guiPendingChanges)",
                 "owned VF fields are projected too while a selective offset "
                 "is active, so the numbers agree with the dashed curve")
    require_text(ui_main_cpp, "sync_vf_curve_field_values();",
                 "populate_edits re-projects the fields after every full "
                 "render, even when the pending summary did not change")
    require_text(_p(ctx, "config_profiles_gui_state.cpp"),
                 "g_app.lockedFreq = lockMHz;",
                 "a profile projection states its own lock target instead of "
                 "letting apply_lock() infer a stale one from the draft")
    ctx.require_order_in_operation(
        _p(ctx, "config_profiles_gui_state.cpp"),
        "if (lockCi >= 0 && lockMHz > 0) {",
        "g_app.guiLockTracksAnchor = desired->hasLock ? desired->lockTracksAnchor : true;",
        "set_edit_value(g_app.hEditsMhz[vi], lockMHz);",
        "a profile projection re-states the lock anchor field after "
        "apply_lock()'s refresh, so the stale previous-profile draft cannot "
        "stay in the VF MHz box")


def check_service_actionability(ctx, require_text, forbid_text):
    """Controls that cannot work without a service are greyed; escape hatches are not.

    With no service installed the editor was correctly dead while the whole
    profile row read as live, and two of those controls misbehaved rather than
    merely looking wrong: Save captures live GPU state and would have persisted an
    all-zero control state as a profile, and Load writes an editor that is
    disabled and blank.

    The line is capability, not visual consistency.  Two rules are load-bearing
    and pinned here:

    Escape hatches are never blocked.  Refresh and the service-install checkbox
    are the only ways out of an unavailable-service state, and greying a purely
    local config action can strand a persisted setting -- logon_shared_slot is
    deliberately preserved when a profile slot is cleared, and the logon combo is
    the only GUI path that can change it.

    The gate is service REACHABILITY, not readiness.  SYNCING / RECOVERING /
    DEVICE_MISSING must stay usable, because a profile loaded during those is
    preserved as a pre-READY overlay and rebased onto the first coherent snapshot.
    """
    policy_h = _p(ctx, "gui_service_actionability_policy.h")
    gui_state_cpp = _p(ctx, "config_profiles_gui_state.cpp")
    profiles_ui_cpp = _p(ctx, "config_profiles_ui.cpp")
    state_cpp = _p(ctx, "gui_service_state.cpp")

    require_text(policy_h, "case GUI_SERVICE_CAP_RECOVERY:",
                 "the escape hatches are decided by the same pure policy as the "
                 "gated capabilities")
    require_text(policy_h, "in->installed && in->available && !in->toggleInFlight",
                 "the gate is service reachability -- installed, answering, and "
                 "not mid-toggle -- never the protocol phase")
    require_text(policy_h,
                 "return assignmentPresent || gui_service_reachable(in);",
                 "a control holding a service-dependent assignment stays enabled "
                 "so the assignment can always be cleared; without that a logon "
                 "assignment would keep a scheduled task with no way to remove it")

    require_text(gui_state_cpp, "GUI_SERVICE_CAP_PROFILE_EDIT",
                 "Load/Save/Clear/slot combo/shared picker are gated on a "
                 "reachable service")
    require_text(gui_state_cpp, "gui_service_config_control_actionable(",
                 "the persisted assignments use the clearable-when-set gate")
    require_text(gui_state_cpp, "g_app.hShareAllUsersCheck,",
                 "the share checkbox is re-gated from the service-state "
                 "projection, which its owner is not on")
    # Positive form on purpose: ensure_profile_slot_cache() is the one place
    # allowed to touch the INI, so pinning that the gating reads the cached
    # accessor is what actually keeps the projection off the file system.
    require_text(gui_state_cpp, "ensure_profile_slot_cache",
                 "the per-slot saved state is cached, because the projection "
                 "runs on the non-READY retry cadence")
    require_text(gui_state_cpp, "bool saved = profile_slot_is_saved_cached(slot);",
                 "the gating reads the cache, not the INI, on every projection "
                 "pass")

    # Both writers of the shared button must use the same expression: this one
    # runs after update_profile_action_buttons() inside
    # refresh_profile_controls_from_config() and standalone from the share
    # toggle, so a weaker expression here silently re-enables the button.
    require_text(profiles_ui_cpp, "sharedCount > 0 && gui_service_capability_enabled(",
                 "the shared-profiles button owner applies the same service "
                 "gate the projection re-asserts")
    require_text(profiles_ui_cpp, "g_app.sharedProfileCountCache = sharedCount;",
                 "the machine-config scan caches its count for the projection")

    require_text(state_cpp, "update_profile_action_buttons();",
                 "the profile row is re-gated from the service-state projection")
    require_text(state_cpp, "if (g_app.hRefreshBtn) EnableWindow(g_app.hRefreshBtn, TRUE);",
                 "Refresh stays enabled: it is the way out of an unavailable "
                 "service")


def check_tray_active_profile(ctx, require_text, forbid_text):
    """The tray tooltip names the APPLIED profile, not the selected one.

    Loading a profile deliberately does not touch hardware, so reporting the
    combo's selection made the tray claim "Profile 1" while slot 2 was actually
    running.  `[profiles] applied_slot` is written by every apply path and is
    the same authority the tray menu checkmark reads, so the tooltip and the
    tick directly next to it cannot disagree.
    """
    policy_h = _p(ctx, "gui_tray_callback_policy.h")
    tray_cpp = _p(ctx, "tray_presentation.cpp")

    require_text(policy_h, "gui_tray_format_active_profile",
                 "the active-profile label is a pure decision")
    require_text(policy_h, "Shared profile %d",
                 "shared banks are named explicitly, never folded into the "
                 "same-numbered personal slot")
    require_text(tray_cpp, '"profiles", "applied_slot", 0',
                 "the tray tooltip reads the applied slot")
    forbid_text(tray_cpp, 'get_config_int(g_app.configPath, "profiles", "selected_slot"',
                "the tray tooltip never reports the editing selection as active")


def check_tray_menu_does_not_raise_the_main_window(ctx, require_text,
                                                   forbid_text):
    """F-TRAY-MENU: opening the tray menu is not a window-state change.

    The shell needs a foreground window in this process before
    `TrackPopupMenu()`, or the popup survives the click that should dismiss it.
    Satisfying that with the main window is what made a tray right-click yank an
    open but occluded Green Curve window in front of whatever the user was
    working in for as long as the menu was up: `SetForegroundWindow()` raises
    its target as well as focusing it.

    The requirement is for *a* foreground window, so the menu gets its own --
    zero-sized, never shown, out of Alt-Tab, and on its own window class so the
    single-instance `FindWindowA(APP_CLASS_NAME)` lookup cannot land on it.
    `TPM_RETURNCMD` then carries the pick back to the main window by hand.
    """
    policy_h = _p(ctx, "gui_tray_callback_policy.h")
    menu_cpp = _p(ctx, "gui_tray_menu.cpp")
    fan_runtime = _p(ctx, "main_fan_runtime.cpp")
    window_cpp = _p(ctx, "ui_main_window.cpp")
    show_menu = "static void show_tray_menu(HWND hwnd)"

    require_text(policy_h, "gui_tray_menu_owner_is_neutral",
                 "the tray menu's foreground owner obeys a pure neutrality "
                 "rule")
    require_text(_p(ctx, "main.cpp"), '#include "gui_tray_menu.cpp"',
                 "the tray-menu shard is compiled into the Windows shell")
    require_text(menu_cpp, "gui_tray_menu_owner_is_neutral(",
                 "the created owner window is checked against that rule, not "
                 "assumed to satisfy it")
    # One ex-style and nothing else at creation: no WS_EX_APPWINDOW taskbar
    # button, and no WS_EX_NOACTIVATE, which would refuse the activation the
    # popup depends on.
    require_text(menu_cpp, "WS_EX_TOOLWINDOW, TRAY_MENU_OWNER_CLASS_NAME",
                 "the tray menu owner has no Alt-Tab entry and no other "
                 "extended style")
    require_text(menu_cpp, 'define TRAY_MENU_OWNER_CLASS_NAME "GreenCurve',
                 "the owner keeps its own window class so the single-instance "
                 "lookup cannot find it instead of the main window")
    require_text(menu_cpp, "WS_POPUP, 0, 0, 0, 0,",
                 "the tray menu owner is created zero-sized")
    forbid_text(menu_cpp, "ShowWindow",
                "the tray menu owner is never shown")
    require_text(menu_cpp, "(style & WS_VISIBLE) != 0",
                 "the neutrality check reads the real visibility bit")
    require_text(menu_cpp, "(exStyle & WS_EX_APPWINDOW) != 0",
                 "the neutrality check reads the real taskbar-presence bit")
    require_text(menu_cpp, "(exStyle & WS_EX_NOACTIVATE) != 0",
                 "the neutrality check reads the real activation bit")
    _require_in_operation(ctx, menu_cpp, show_menu, "SetForegroundWindow(owner)",
                          "the tray menu takes the foreground through its own "
                          "owner")
    _forbid_in_operation(ctx, menu_cpp, show_menu, "SetForegroundWindow(hwnd)",
                         "the tray menu never raises the main window")
    _require_in_operation(ctx, menu_cpp, show_menu, "TPM_RETURNCMD",
                          "the pick is returned to this function rather than "
                          "posted to the owner")
    _require_in_operation(ctx, menu_cpp, show_menu,
                          "PostMessageA(hwnd, WM_COMMAND, MAKEWPARAM((WORD)cmd, 0), 0)",
                          "the returned pick is forwarded to the main window")
    _require_in_operation(ctx, menu_cpp, show_menu, "allow_dark_mode_for_window(owner)",
                          "the throwaway owner carries the main window's "
                          "dark-mode opt-in so the menu keeps its theme")
    forbid_text(fan_runtime, "TRAY_MENU_EXIT_ID",
                "the tray menu is built in exactly one place")
    require_text(window_cpp, "destroy_tray_menu_owner_window();",
                 "the owner window is released with the rest of the tray state")


def check_auto_profile_enable_is_a_transition(ctx, require_text, forbid_text):
    """F-AUTO-PROFILE: flipping the master enable is a transition, not a value.

    Enabling auto-switching has to CLEAR whatever manual pin the controller is
    holding, or `ap_controller_is_driving()` stays false and no rule ever
    switches anything.  That reset lives in `ap_set_enabled()`, and originally
    only the tray toggle called it: the configuration dialog persisted
    `enabled=1` and reached the controller through `auto_profile_reload_config`,
    which merely synced values.  A profile picked from the tray beforehand
    therefore survived as a pin and killed auto for the whole session — with no
    visible symptom beyond "it just does not switch".

    Both surfaces now route through `ap_apply_config_change()`, which owns the
    comparison and delegates the transition, so a third caller cannot
    reintroduce the split.  Syncing values without it inside an enable-flipping
    surface is what regressed, so it is forbidden there by name.
    """
    win32_cpp = _p(ctx, "auto_profile_win32.cpp")
    controller_cpp = _p(ctx, "auto_profile_controller.cpp")
    enable_surfaces = ("void auto_profile_reload_config(",
                       "void auto_profile_toggle_enabled(")

    require_text(controller_cpp, "AutoProfileAction ap_apply_config_change(",
                 "the config-change transition is one pure decision")
    _require_in_operation(ctx, controller_cpp,
                          "AutoProfileAction ap_apply_config_change(",
                          "ap_set_enabled(c, cfg->enabled)",
                          "a changed master enable delegates to the transition")
    for surface in enable_surfaces:
        _require_in_operation(ctx, win32_cpp, surface,
                              "ap_apply_config_change(&g_apCtrl, &g_apConfig)",
                              "every surface that flips the master enable "
                              "performs the enable transition")
        _forbid_in_operation(ctx, win32_cpp, surface, "ap_controller_sync_config(",
                             "an enable-flipping surface must not sync config "
                             "values without the transition")

    # A pick while auto is off must not record a pin that outlives it.
    _require_in_operation(ctx, controller_cpp,
                          "AutoProfileAction ap_on_hotkey(",
                          "if (!c->autoEnabled)",
                          "a profile pick while auto is disabled records no "
                          "manual pin")


def check_applied_profile_indicator_is_drift_free(ctx, require_text, forbid_text):
    """The applied-profile tick is decided without live hardware readback.

    Twice now the indicator has been broken by live VF state leaking into it.
    The direct comparison against readback was removed years ago; it then came
    back indirectly, because load_profile_from_config() projects a stored
    profile onto the LIVE curve before returning it (visibility strip, plus a
    lockTracksAnchor re-derived from curve_base_khz_for_point(), which is
    readback minus live offsets).  The verdict therefore flipped with ordinary
    boost drift, and the sync's cache key covered none of it -- so the flip was
    applied later, by whatever next invalidated the cache.  A plain profile Load
    does that (it writes selected_slot), which is why "load a profile and the
    tray tick disappears" was the reported symptom.

    So: the ownership read is explicit, and the decision itself is a pure
    function with its own unit coverage.
    """
    policy_h = _p(ctx, "applied_profile_indicator_policy.h")
    profiles_ui = _p(ctx, "config_profiles_ui.cpp")
    profiles_cpp = _p(ctx, "config_profiles.cpp")
    repair_cpp = _p(ctx, "config_profile_repair.cpp")
    sync_cache = _p(ctx, "config_profile_sync_cache.cpp")

    require_text(policy_h, "PROFILE_READ_FOR_OWNERSHIP",
                 "the drift-free read mode is named where the invariant is "
                 "documented")
    require_text(policy_h, "applied_profile_indicator_slot",
                 "the indicator is a pure decision with unit coverage")

    # The one call that matters: the ownership comparison must ask for the
    # drift-free read.  Anchored to the sync function so an editor-mode read
    # somewhere else in the same file cannot satisfy it.
    ctx.require_text_in_operation(
        profiles_ui,
        "static void sync_applied_profile_from_service_metadata()",
        "PROFILE_READ_FOR_OWNERSHIP",
        "the applied-profile comparison reads the stored profile drift-free")
    ctx.require_text_in_operation(
        profiles_ui,
        "static void sync_applied_profile_from_service_metadata()",
        "applied_profile_indicator_slot(decision, &reason)",
        "the applied-profile verdict comes from the pure policy")

    # Both live-readback projections inside the loader must be gated, not
    # merely commented.  These are the exact two that flipped the verdict.
    require_text(profiles_cpp,
                 "const bool projectOntoLiveCurve = mode == PROFILE_READ_FOR_EDITOR;",
                 "the loader derives one gate for every live-curve projection")
    require_text(profiles_cpp,
                 "} else if (projectOntoLiveCurve && desired->hasLock &&",
                 "the lock-anchor re-derivation is skipped for an ownership read")
    require_text(profiles_cpp,
                 "bool haveLiveCurveVisibility = projectOntoLiveCurve &&",
                 "the live visibility strip is skipped for an ownership read")
    require_text(repair_cpp,
                 "if (mode == PROFILE_READ_FOR_OWNERSHIP) return true;",
                 "the profile repair does not consult the live curve for an "
                 "ownership read")

    # A cache that omits an input the decision reads is how the last one hid.
    require_text(sync_cache, "populatedMask",
                 "the sync cache key covers the VF topology the ownership read "
                 "still depends on")


def check_service_profile_identity_survives_a_delta_apply(ctx, require_text,
                                                          forbid_text):
    """A profile applied from the main window keeps its slot identity.

    The service only ever proved a claimed slot against the request PAYLOAD.
    A GUI Apply is deliberately a delta -- capture_gui_apply_settings() drops
    domains the editor is not changing, most often the fan, because re-writing
    an unchanged fan policy would disturb a running curve mid-game -- so it
    could never equal a complete stored record.  Every Apply from the main
    window was therefore recorded as SERVICE_PROFILE_SOURCE_AD_HOC, the GUI's
    indicator saw "not a personal slot", applied_slot was cleared, and the tray
    menu showed no tick until the profile was re-picked from the tray, the one
    path that had ever sent a whole record.

    The fix asks the honest question after the write instead: is the profile
    what is now in force.  Both halves are pinned here -- the post-write
    re-check must exist, and the record it compares against must still be read
    drift-free, or this becomes the third incarnation of the boost-drift bug.
    """
    policy_h = _p(ctx, "service_profile_identity_policy.h")
    request_policy = _p(ctx, "main_service_request_policy.cpp")
    pipe_cpp = _p(ctx, "main_service_pipe.cpp")

    require_text(policy_h, "service_profile_identity_outcome",
                 "the recorded identity is a pure decision with unit coverage")
    require_text(policy_h, "SERVICE_PROFILE_IDENTITY_FROM_ACTIVE_INTENT",
                 "a delta Apply can prove its slot from the resulting intent")

    # The comparison itself, and the read that feeds it.
    ctx.require_text_in_operation(
        request_policy,
        "static bool service_profile_record_describes_intent(",
        "PROFILE_READ_FOR_OWNERSHIP",
        "the service compares stored records drift-free, like the GUI does")
    ctx.require_text_in_operation(
        request_policy,
        "static void service_confirm_profile_metadata_from_active_intent(",
        "g_serviceActiveDesired",
        "the post-write re-check asks the intent that is actually in force")

    # The success path must go through the recorder; writing the pre-write
    # verdict straight into the published identity is exactly the defect.
    require_text(pipe_cpp, "service_record_apply_profile_identity(&request,",
                 "a successful APPLY records its identity through the "
                 "two-stage recorder")
    forbid_text(pipe_cpp, "g_serviceActiveProfileSource = profileSource;",
                "the pipe handler no longer publishes the pre-write verdict "
                "directly")

    # Only a whole-record payload may replace ownership.  Treating a delta as a
    # complete declaration would return every domain it omits to defaults --
    # which is how an Apply that never mentioned the fan would reset a running
    # fan curve.
    require_text(pipe_cpp, "service_profile_identity_replaces_active_intent(",
                 "intent replacement follows the pure policy, not the "
                 "recorded source")


def check_apply_in_flight_presentation(ctx, require_text, forbid_text):
    """An apply that is still running says so, on every surface.

    An apply deliberately takes seconds: reset to a stock OC baseline, let the
    curve settle so expected NVIDIA boost drift cannot be mistaken for a failed
    write, then write the new intent.  Without a transitional presentation the
    tray kept showing the OLD profile's theme and tooltip for that whole
    window, which reads as "nothing happened".

    All three surfaces derive from one policy header, and the state itself is
    driven from the mutation queue -- the single point every apply path passes
    through -- so no path can leave one of them stale.
    """
    policy_h = _p(ctx, "gui_apply_in_flight_policy.h")
    fan_runtime = _p(ctx, "main_fan_runtime.cpp")
    tray_presentation = _p(ctx, "tray_presentation.cpp")
    gui_state = _p(ctx, "config_profiles_gui_state.cpp")
    mutation_worker = _p(ctx, "gui_mutation_worker.cpp")

    require_text(policy_h, "GUI_APPLY_IN_FLIGHT_PHRASE",
                 "one phrase feeds every surface, so they cannot disagree")
    require_text(policy_h, "TRAY_ICON_STATE_PENDING",
                 "the transitional theme has its own icon slot")
    require_text(policy_h, "gui_tray_live_state_has_custom_oc(",
                 "the tray's OC domain is a pure, unit-tested rule")
    require_text(tray_presentation, "gui_tray_live_state_has_custom_oc(",
                 "the live tray OC classification uses the pure policy")
    require_text(tray_presentation, "appliedLockMode != LOCK_MODE_NONE",
                 "an applied lock/pin counts as custom OC for the tray, so a "
                 "pinned-clock profile with a custom fan reads as OC + Custom "
                 "Fan rather than plain Custom Fan")
    require_text(fan_runtime, "live_state_has_custom_oc()",
                 "the tray theme is driven by the same live-state "
                 "classification the tooltip uses")

    ctx.require_text_in_operation(
        fan_runtime, "static void update_tray_icon()",
        "gui_apply_in_flight_tray_icon_state(",
        "the tray theme comes from the pure policy, in-flight state included")
    # The in-flight branch lives in the BASE builder since the tooltip gained
    # its update suffix: the base owns the mutually exclusive shapes (in
    # flight / service down / ordinary), the wrapper appends the update note to
    # whichever one came back.  Both halves are pinned, because a base nothing
    # composed from would satisfy the first assertion while the tooltip the
    # user sees lost the branch entirely.
    ctx.require_text_in_operation(
        tray_presentation, "static void build_tray_tooltip_base(",
        "gui_apply_in_flight_tray_tooltip(",
        "the tray tooltip reports a write in flight")
    ctx.require_text_in_operation(
        tray_presentation, "static void build_tray_tooltip(char* tip",
        "build_tray_tooltip_base(",
        "the tooltip the tray actually shows is built from that base")
    require_text(gui_state, "gui_apply_in_flight_status_text(",
                 "the main window reports a write in flight too")

    # The status line alone was reported as invisible: it sits at the bottom
    # edge of the window, and the tray tooltip only exists while the cursor
    # hovers the icon. The banner over the graph is the surface that gets
    # noticed, so it is pinned separately from the text surfaces above.
    banner_cpp = _p(ctx, "gui_apply_in_flight.cpp")
    scene_cpp = _p(ctx, "ui_main.cpp")
    window_cpp = _p(ctx, "ui_main_window.cpp")
    require_text(policy_h, "gui_apply_in_flight_sweep",
                 "the sweep geometry is pure and unit-tested")
    # The banner shipped once pinned to the top of the CLIENT area, straight
    # over the GPU selector row: child controls paint over their parent, so the
    # selector appeared to bleed through it. The band is pure and asserted, the
    # header strip is one shared constant, and the banner is painted under the
    # graph's own scroll transform so no control can land on it at any scroll
    # position.
    require_text(policy_h, "gui_apply_in_flight_banner_band",
                 "the banner's vertical band is pure and unit-tested")
    require_text(banner_cpp, "MAIN_LAYOUT_GRAPH_TOP_MARGIN_LOGICAL",
                 "the banner starts below the strip the GPU selector row owns")
    require_text(_p(ctx, "ui_main_graph.cpp"),
                 "mt = dp(MAIN_LAYOUT_GRAPH_TOP_MARGIN_LOGICAL)",
                 "the graph's top margin is that same shared strip")
    require_text(banner_cpp, "SetViewportOrgEx(hdc, -main_layout_scroll_x()",
                 "the banner is painted in the graph's scrolled content space")
    ctx.require_text_in_operation(
        scene_cpp, "static void draw_gui_scene(",
        "gui_draw_apply_in_flight_banner(",
        "the main window paints the in-flight banner")
    require_text(banner_cpp, "if (!hdc || !g_app.applyInFlight) return;",
                 "the banner is drawn only while a write is actually running")
    require_text(window_cpp, "APPLY_IN_FLIGHT_TIMER_ID",
                 "the animation timer is dispatched")
    # Presentation only: the timer must never be the thing that decides state.
    forbid_text(banner_cpp, "Sleep(",
                "the banner animates from a timer, never by blocking the pump")
    require_text(banner_cpp, "if (!g_app.applyInFlight) {",
                 "a timer that outlived the state it animates stops itself")

    # Both transitions are driven from the queue, not from the apply paths.
    require_text(mutation_worker,
                 "gui_apply_in_flight_presentation_changed(true,",
                 "queueing a mutation enters the transitional presentation")
    require_text(mutation_worker,
                 "gui_apply_in_flight_presentation_changed(false,",
                 "draining the queue leaves it again")


def check_manual_mutation_result_presentation(ctx, require_text, forbid_text):
    """A manual Apply/Reset interrupts the user only when it has to.

    Every manual mutation used to end in an OK-box, the ordinary success
    included.  A confirmation that always appears is not information -- it is a
    click -- and it is exactly how the box that DOES matter becomes something to
    dismiss unread.  The clean case is now confirmed on the profile status line
    and raises nothing; a warning or an error still opens the box, carrying the
    service's own wording.

    The severity behind that decision is service-side and travels on the wire
    (ServiceOutcomeSeverity), because only the apply backend can tell a fully
    verified write from one that committed while the driver declined some
    points -- both of which answer SERVICE_STATUS_OK.  Deriving it in the window
    would mean parsing `message`, which is prose.  Hence the two rules pinned
    here: the window reads the field, and it never reconstructs it from text.
    """
    policy_h = _p(ctx, "gui_mutation_result_policy.h")
    completion_cpp = _p(ctx, "ui_mutation_completion.cpp")
    worker_cpp = _p(ctx, "gui_mutation_worker.cpp")
    protocol_h = _p(ctx, "service_protocol.h")
    validation_h = _p(ctx, "service_protocol_validation.h")
    pipe_cpp = _p(ctx, "main_service_pipe.cpp")
    daemon_cpp = _p(ctx, "linux_daemon.cpp")
    apply_cpp = _p(ctx, "gpu_backend_apply.cpp")

    require_text(protocol_h, "SERVICE_OUTCOME_SEVERITY_WARNING",
                 "the wire distinguishes a clean success from a warned one")
    require_text(protocol_h, "service_response_resolve_outcome_severity",
                 "severity is derived from status by one shared pure rule")
    require_text(validation_h, "service_outcome_severity_matches_status(",
                 "a response whose severity contradicts its status is damaged")
    # One stamp per producer, at the single point each writes a response out.
    # Per-handler assignment is what would eventually ship a branch that forgot.
    ctx.require_order_in_operation(
        pipe_cpp, "static DWORD WINAPI service_pipe_server_thread_proc(",
        "response.outcomeSeverity = service_response_resolve_outcome_severity(",
        "service_pipe_write_exact(pipe, &response, sizeof(response)",
        "the Windows service resolves severity before it writes the response")
    ctx.require_text_in_operation(
        daemon_cpp, "static void handle_request(",
        "resp->outcomeSeverity = service_response_resolve_outcome_severity(",
        "the Linux daemon resolves severity on every response it answers")
    require_text(apply_cpp, "service_apply_outcome_severity_for_lock_mode(",
                 "the apply backend reports a committed-but-unmatched write as "
                 "a warning instead of a silent success, while a hard NVML pin "
                 "ignores VF tail readback")

    require_text(policy_h, "gui_mutation_result_needs_prompt",
                 "whether to interrupt the user is a pure, unit-tested rule")
    require_text(policy_h, "if (!successForUi) return (gc_u32)SERVICE_OUTCOME_SEVERITY_ERROR;",
                 "a result the window could not adopt is never presented as a "
                 "success, whatever the envelope claims")
    ctx.require_text_in_operation(
        completion_cpp, "static gc_u32 set_mutation_result_status_line(",
        "completion->response.outcomeSeverity",
        "the window reads the severity the service published")
    ctx.require_text_in_operation(
        completion_cpp, "static gc_u32 set_mutation_result_status_line(",
        "set_profile_status_text(",
        "every reported result reaches the status line, dialog or not")
    ctx.require_text_in_operation(
        completion_cpp, "static void present_manual_mutation_result(",
        "if (!gui_mutation_result_needs_prompt(severity)) return;",
        "a clean success raises no dialog at all")
    ctx.require_text_in_operation(
        completion_cpp, "static void present_manual_mutation_result(",
        "set_mutation_result_status_line(work, completion,",
        "a manual result always updates the status line before deciding on a "
        "dialog")
    # The status line is the surface that now carries the ordinary case, so the
    # completion must not be able to leave the queue's wording standing.
    for context in ("GUI_MUTATION_CONTEXT_MANUAL_APPLY",
                    "GUI_MUTATION_CONTEXT_MANUAL_RESET"):
        ctx.require_order_in_operation(
            completion_cpp, "static void handle_gui_mutation_completion(",
            context, "present_manual_mutation_result(&work, completion,",
            f"{context} routes its result through the presentation policy")
    require_text(worker_cpp, "gui_mutation_queued_status_text(",
                 "the queued line names what is being applied")
    forbid_text(worker_cpp, "GPU operation started in the background",
                "the queue no longer announces an unnamed background operation")

    # A tray/hotkey pick is an explicit user action and reports its outcome on
    # the same line the Apply button uses -- switching profiles from the tray
    # was reported as leaving the status line describing something else
    # entirely.  Writing a resident child label creates no surface and takes no
    # focus, so this stays inside the presentation-silent contract (the
    # F-PRESENTATION-SILENT token gate in build.py is what enforces that).
    # A rule-driven foreground switch must stay silent, hence the origin guard
    # on all three sites rather than an unconditional write.
    auto_cpp = _p(ctx, "auto_profile_win32.cpp")
    ctx.require_text_in_operation(
        completion_cpp,
        "static void handle_auto_profile_mutation_completion_presentation_silent(",
        "if (service_apply_origin_is_explicit(work->origin))",
        "only an explicit tray/hotkey pick reports its result on the status line")
    ctx.require_text_in_operation(
        completion_cpp,
        "static void handle_auto_profile_mutation_completion_presentation_silent(",
        "set_mutation_result_status_line(work, completion, successForUi);",
        "and it reports it through the same formatter the Apply button uses")
    ctx.require_text_in_operation(
        auto_cpp, "static bool ap_do_apply_slot(",
        "if (service_apply_origin_is_explicit(origin) && queueStatus[0])",
        "an explicit pick shows the queued line, an automatic switch does not")
    ctx.require_text_in_operation(
        auto_cpp, "static bool ap_do_apply_slot(",
        "set_profile_status_text(\"Profile %d is already applied.\", slot)",
        "an explicit pick that is already applied says so instead of looking "
        "like nothing happened")
    # Automation stays presentation-silent: it must not reach the box at all.
    forbid_text(_p(ctx, "auto_profile_win32.cpp"),
                "present_manual_mutation_result",
                "auto-profile applies never present a manual result")
    forbid_text(_p(ctx, "main_startup_profiles.cpp"),
                "present_manual_mutation_result",
                "logon/app-launch applies never present a manual result")


def check_lock_checkbox_render(ctx, require_text, forbid_text):
    """The VF lock checkbox shares the themed anti-aliased checkmark renderer.

    A raw-GDI Polyline tick looked corrupted next to the themed checkboxes
    (service install / share-all-users / tray), so the lock reuses their
    renderer.  The tick COLOUR became a variable when F-PENDING started drawing
    an unapplied lock in COL_PENDING; an applied one must keep the standard
    themed colour, and the shared renderer must stay in use either way.
    """
    main_shell_cpp = _p(ctx, "main_shell.cpp")
    require_text(main_shell_cpp, "draw_checkbox_tick_smooth(hdc, &box, tick)",
                 "lock FLATTEN tick uses the shared anti-aliased checkmark "
                 "renderer")
    require_text(main_shell_cpp, "COLORREF tick = RGB(0xE8, 0xF2, 0xFF);",
                 "an applied lock keeps the standard themed tick colour")
    forbid_text(main_shell_cpp, "Polyline(hdc, pts, 3)",
                "lock checkbox no longer draws the jagged raw-GDI checkmark")


def check_labeled_checkbox_hit_area(ctx, require_text, forbid_text):
    """F-CHECKBOX-HIT: a checkbox is clickable on its caption -- and stops there.

    Both bounds are deliberate.  Requiring pixel-accurate aim at a 14-logical-
    pixel square is bad targeting, so the caption and the gap in between must
    toggle it too.  But a BUTTON sized wider than its own caption turns empty
    background into a silent toggle, which is worse than the small target
    because nothing there looks clickable -- hence the measured fit rather than
    a generous fixed width.

    The two main-window captions used to be neighbouring STATICs with SS_NOTIFY,
    which left the gap between box and text dead and, worse, made the greyed
    state foreign: user32 paints a disabled static in the system grey-text
    colour and ignores the WM_CTLCOLORSTATIC palette entirely, so one caption
    did not match any other dimmed label in the dark window.  The captions now
    live inside their own BUTTON, where the owner-draw renderer dims them to
    COL_LABEL like everything else.

    Renderer and hit rectangle must keep deriving from the same pure metrics, or
    the pixels and the clickable area silently drift apart.
    """
    metrics_h = _p(ctx, "ui_theme_metrics.h")
    checkbox_cpp = _p(ctx, "ui_theme_checkbox.cpp")

    require_text(metrics_h, "ui_theme_labeled_checkbox_width",
                 "the labeled-checkbox hit width is one shared pure metric")
    require_text(checkbox_cpp, "ui_theme_checkbox_box_inset(g_dpi)",
                 "the renderer places its box with the shared inset metric")
    require_text(checkbox_cpp, "ui_theme_checkbox_label_gap(g_dpi)",
                 "the renderer places its caption with the shared gap metric")
    require_text(checkbox_cpp, "ui_theme_labeled_checkbox_width(",
                 "the hit-rect fit derives from the same pure metric")
    require_text(checkbox_cpp, "disabled ? COL_LABEL : COL_TEXT",
                 "a greyed caption uses the themed label colour, not the system "
                 "grey-text a disabled STATIC would force")
    require_text(_p(ctx, "ui_main_layout.cpp"),
                 "main_layout_labeled_checkbox_width",
                 "main-window checkboxes are laid out at their caption width")
    require_text(_p(ctx, "config_profiles_gui_state.cpp"),
                 "fit_themed_checkbox_to_label(g_app.hServiceEnableCheck)",
                 "the service checkbox re-fits when its caption gains "
                 "'(repair needed)', instead of ellipsizing its own label")
    require_text(_p(ctx, "config_profiles_ui.cpp"),
                 "fit_themed_checkbox_to_label(g_app.hShareAllUsersCheck)",
                 "the share checkbox re-fits when the selected slot changes")
    forbid_text(_p(ctx, "entry.cpp"), "SS_NOTIFY",
                "checkbox captions live inside their BUTTON, never in a "
                "click-forwarding STATIC")
    forbid_text(_p(ctx, "ui_main_window.cpp"), "STN_CLICKED",
                "no main-window checkbox is toggled through a separate caption "
                "control")


def check_owner_draw_checkbox_repaint(ctx, require_text, forbid_text):
    """A checkbox repaint is gated on the last PAINTED tick, never on BM_GETCHECK.

    Every checkbox in this program is BS_OWNERDRAW and derives its tick at paint
    time -- from the machine config, from AppState, or from the fan dialog's
    working curve.  Button type bits are mutually exclusive, so such a control is
    not a native checkbox and Windows keeps no check state for it: BM_GETCHECK
    always answers BST_UNCHECKED and BM_SETCHECK is a no-op.

    Gating a repaint on that made the projection directional.  Setting a tick
    compared "checked" against a permanent "unchecked", saw a difference, and
    invalidated; CLEARING one compared "unchecked" against "unchecked", concluded
    nothing had changed, and never invalidated.  Unsharing a profile slot left
    the tick on screen until the next full-window redraw or a GUI restart, even
    though the machine config was already correct.

    The painter therefore records what it put on screen, and projections compare
    against that.  The mirror cannot drift, because an out-of-band
    RDW_ALLCHILDREN repaint updates it too.
    """
    # The auto-profile dialog was converted first (2026-07-12) and is the model
    # the main window now follows: explicit state, synchronous redraw on click.
    dialog_cpp = _p(ctx, "auto_profile_dialog.cpp")
    require_text(dialog_cpp, "draw_themed_checkbox_control",
                 "auto-profile checkboxes use the main-window themed renderer")
    require_text(dialog_cpp, "BS_OWNERDRAW",
                 "auto-profile checkboxes are owner-drawn")
    require_text(dialog_cpp, "UiCheckboxState",
                 "auto-profile owner-draw checkboxes keep explicit dialog-model "
                 "state")
    require_text(dialog_cpp, "RDW_INVALIDATE | RDW_UPDATENOW",
                 "auto-profile checkbox clicks synchronously repaint their new "
                 "state")
    forbid_text(dialog_cpp, "BS_AUTOCHECKBOX",
                "auto-profile dialog no longer uses unmatched native checkboxes")

    require_text(_p(ctx, "ui_checkbox_state.h"), "ui_checkbox_state_needs_repaint",
                 "owner-draw checkbox repaints are gated on the last painted "
                 "value")
    button_cpp = _p(ctx, "ui_theme_button.cpp")
    require_text(button_cpp,
                 "ui_checkbox_state_set(themed_checkbox_painted_state("
                 "dis->CtlID), checked)",
                 "the owner-draw painter records the tick it painted")
    require_text(button_cpp, "static UiCheckboxState* themed_checkbox_painted_state",
                 "one table maps a themed checkbox id to its painted mirror")
    require_text(_p(ctx, "config_profiles_ui.cpp"),
                 "&g_app.shareAllUsersPainted, shared)",
                 "the share-with-all-users tick repaints in both directions")
    require_text(_p(ctx, "config_profiles_gui_state.cpp"),
                 "ui_checkbox_state_needs_repaint(\n            "
                 "&g_app.serviceEnablePainted, checked)",
                 "the background-service tick repaints in both directions")
    # ui_control_projection.h is listed too: it must not grow a replacement for
    # the gui_set_button_check_if_changed() helper that caused this.
    for name in ("ui_control_projection.h", "ui_theme_button.cpp",
                 "ui_theme_checkbox.cpp", "main_runtime_ui.cpp",
                 "config_profiles_ui.cpp", "config_profiles_gui_state.cpp",
                 "ui_main_window.cpp", "ui_main.cpp", "ui_lock_checkbox.cpp",
                 "gui_service_state.cpp", "fan_curve_dialog.cpp",
                 "auto_profile_dialog.cpp"):
        # The trailing comma is what makes this the CALL form: the message id is
        # always followed by wParam, on the same line or its own.  Matching the
        # bare word would also ban the comments that explain why this rule
        # exists, and that history is worth keeping next to the code.
        forbid_text(_p(ctx, name), "BM_GETCHECK,",
                    f"{name} does not query check state an owner-draw button "
                    "never stores")
        forbid_text(_p(ctx, name), "BM_SETCHECK,",
                    f"{name} does not write check state an owner-draw button "
                    "never stores")


def check_right_anchored_controls(ctx, require_text, forbid_text):
    """The right-hand controls share one right edge mirroring the left inset.

    Three controls sit at the right of the window: the GPU selector, the
    fan-curve button, and the License button.  They used three different rules --
    a 12 px inset, a hardcoded x of 1006, and the correct 8 px inset -- so they
    were visibly ragged, and the fan-curve button drifted further from the edge
    on every window wider than the base canvas because it never tracked
    contentWidth at all.

    One pure helper now decides all three, and MAIN_LAYOUT_SIDE_MARGIN_LOGICAL is
    the single inset the left gutter uses as well.
    """
    policy_h = _p(ctx, "main_layout_policy.h")
    layout_cpp = _p(ctx, "ui_main_layout.cpp")
    place = "static void main_layout_place_controls("

    require_text(policy_h, "MAIN_LAYOUT_SIDE_MARGIN_LOGICAL",
                 "one canonical side inset drives both the left and right "
                 "gutters")
    require_text(policy_h, "main_layout_right_anchored_x",
                 "right-anchored placement is a pure, unit-tested decision")
    for control in ("g_app.hGpuSelectCombo", "g_app.hFanCurveBtn",
                    "g_app.hLicenseBtn"):
        _require_in_operation(
            ctx, layout_cpp, place, control,
            f"{control} is placed by the shared layout pass")
    _require_in_operation(
        ctx, layout_cpp, place,
        "main_layout_right_anchored_x(plan.contentWidth, dp(160)",
        "the fan-curve button is right-anchored against the content width")
    # License and Updates share the right edge of the button row.  The widths
    # are named constants now rather than literals, because Updates is anchored
    # as the full pair (its width + gap + License) so the two stay adjacent at
    # every window width instead of drifting apart as the content area grows.
    _require_in_operation(
        ctx, layout_cpp, place,
        "main_layout_right_anchored_x(plan.contentWidth, licenseW",
        "the License button is right-anchored against the content width")
    _require_in_operation(
        ctx, layout_cpp, place,
        "main_layout_right_anchored_x(\n        plan.contentWidth, updateW + dp(8) + licenseW",
        "the Updates button is right-anchored as the License pair")
    _forbid_in_operation(
        ctx, layout_cpp, place, "dp(1006), ocY - dp(1)",
        "the fan-curve button is right-anchored, not pinned to a fixed x")
    _forbid_in_operation(
        ctx, layout_cpp, place, "gpuSelectW - dp(12)",
        "the GPU selector uses the canonical side inset, not its own 12 px")


def check_visibility_neutral_projection(ctx, require_text, forbid_text):
    """A coherent projection never changes what the shell can observe.

    `DefWindowProc` implements `WM_SETREDRAW` by clearing (FALSE) and setting
    (TRUE) the target window's `WS_VISIBLE` bit.  On a top-level HWND that bit
    is what the shell derives the taskbar button, the Alt-Tab entry and its
    z-order bookkeeping from, so the suppression pair read as a hide followed by
    a show.  It produced two separate reports: a tray-hidden owner resurrected
    as a ghost window, and -- because the first fix only skipped the toggle
    while hidden -- an open window dropping out of the taskbar window list for
    the length of every structural projection, Refresh included.

    Painting is therefore suppressed internally (a depth gate honoured by
    `WM_PAINT`/`WM_ERASEBKGND` and by `invalidate_main_window()`), and no window
    style is touched at all.
    """
    policy_h = _p(ctx, "gui_window_redraw_policy.h")
    redraw_cpp = _p(ctx, "gui_window_redraw.cpp")
    window_cpp = _p(ctx, "ui_main_window.cpp")
    runtime_gpu_cpp = _p(ctx, "main_runtime_gpu.cpp")
    invalidate = "static void invalidate_main_window()"

    require_text(policy_h, "gui_redraw_toggle_is_visibility_safe",
                 "only a child window's WS_VISIBLE bit may be redraw-toggled")
    require_text(policy_h, "gui_top_level_redraw_may_paint_synchronously",
                 "top-level redraw policy distinguishes visible from "
                 "tray-hidden windows")
    require_text(redraw_cpp, "RDW_INVALIDATE | RDW_ALLCHILDREN",
                 "hidden top-level redraw transactions defer painting until "
                 "explicit show")
    forbid_text(redraw_cpp, "WM_SETREDRAW",
                "a coherent projection transaction never redraw-toggles the "
                "top-level window")
    require_text(redraw_cpp, "g_guiTopLevelRedrawDepth",
                 "projection transactions suppress painting through the depth "
                 "gate, not window styles")
    _require_in_operation(ctx, window_cpp, "case WM_PAINT:",
                          "gui_top_level_paint_suppressed()",
                          "the main window paints no half-projected frame "
                          "during a transaction")
    require_text(window_cpp, "if (gui_top_level_paint_suppressed()) return 1;",
                 "a suppressed transaction never erases the window behind its "
                 "controls")
    _require_in_operation(ctx, runtime_gpu_cpp, invalidate,
                          "gui_window_invalidation_must_defer(",
                          "invalidation defers while a projection transaction "
                          "owns the frame")
    ctx.require_order_in_operation(
        runtime_gpu_cpp, invalidate, "g_app.trayWindowHiddenIntent",
        "redraw_window_sync(g_app.hMainWnd);",
        "main-window invalidation defers painting while tray-hidden")
    _forbid_in_operation(ctx, runtime_gpu_cpp, invalidate, "RDW_UPDATENOW",
                         "tray-hidden invalidation cannot synchronously paint "
                         "the owner")
    forbid_text(_p(ctx, "gui_service_state.cpp"), "WM_SETREDRAW",
                "service projection cannot resurrect a hidden top-level window")
    forbid_text(_p(ctx, "ui_main_control_lifecycle.cpp"), "WM_SETREDRAW",
                "control rebuild cannot resurrect a hidden top-level window")


def check_manual_refresh_preserves_presentation(ctx, require_text, forbid_text):
    """Refresh re-reads the GPU on screen; it is not a reconnect.

    Routing it through `gui_service_begin_full_sync()` forced the model to
    SYNCING and threw live authority away before the read had even been sent,
    so one click flashed the "Synchronizing GPU state" overlay and dropped the
    tray icon from its OC/fan theme to the neutral one -- claiming for a round
    trip that no Green Curve settings were in effect.  The completion paths
    already transition truthfully when the answer really is "not READY".
    """
    apply_cpp = _p(ctx, "ui_main_apply.cpp")
    state_cpp = _p(ctx, "gui_service_state.cpp")

    require_text(apply_cpp,
                 'gui_service_request_resync("manual refresh", false)',
                 "manual refresh re-reads without a presentation transition")
    forbid_text(apply_cpp, 'gui_service_begin_full_sync("manual refresh")',
                "manual refresh cannot flash the syncing presentation or the "
                "neutral tray theme")
    require_text(_p(ctx, "gui_service_resync_policy.h"), "identityMayChange",
                 "only a read aimed at another GPU/service forces a "
                 "presentation transition")
    ctx.require_order_in_operation(
        state_cpp, "static void gui_service_request_resync(",
        "gui_service_io_queue_full_sync(reason)",
        "gui_service_begin_full_sync(reason);",
        "a preserved-presentation resync falls back to the visible transition "
        "only if the read cannot be queued")
    require_text(state_cpp, "g_app.guiManualResyncPending = false;",
                 "a real presentation transition supersedes the silent "
                 "refresh's status line")


def check_graph_frequency_axis(ctx, require_text, forbid_text):
    """The VF graph's frequency axis is derived from the plotted data.

    A fixed 500..3400 MHz band clamped high-clock curves (a large GPU offset, a
    high flatten target, an unsupported GPU's boosted curve) flat against the
    top edge, while low-clock curves floated in empty space.  The pure rule in
    gui_graph_axis_policy.h rounds dataMax+headroom up / dataMin-headroom down
    to the 500 MHz grid with a minimum span; the renderer feeds it from both
    plotted series and logs the resolved range change-gated.  The voltage axis
    follows the same idea on the 50 mV grid, and both axes are fed from the
    VISIBLE VF list only, so hidden low points and their labels cannot appear
    and a GPU whose curve starts at 750 mV does not waste the 700-750 mV cell.
    """
    graph_cpp = _p(ctx, "ui_main_graph.cpp")
    require_text(graph_cpp, "gui_graph_frequency_axis",
                 "the VF graph frequency axis is derived from the plotted data")
    require_text(graph_cpp, "gui_graph_voltage_axis",
                 "the VF graph voltage axis starts at the first visible point "
                 "so an empty 700-750 mV cell cannot waste space")
    require_text(graph_cpp, "g_app.visibleMap[vi]",
                 "the graph plots exactly the visible VF list, so hidden low "
                 "points and their labels cannot appear")
    require_text(graph_cpp, "sz2.cx / 2 < ml",
                 "on-curve labels that would spill left of the plot are "
                 "skipped instead of wasting the left margin")
    forbid_text(graph_cpp, "MAX_FREQ_MHz = 3400",
                "the fixed 3400 MHz ceiling truncated high-clock curves")
    require_text(graph_cpp, "gui graph frequency axis:",
                 "the VF graph logs the resolved frequency axis change-gated")
    require_text(graph_cpp, "900, 900",
                 "the y-axis title uses a rotated font so it cannot overlap "
                 "the top clock label")
    forbid_text(graph_cpp, "place horizontally left of Y labels",
                "the horizontal y-axis title overlapped the clock labels")
    require_text(_p(ctx, "gui_graph_axis_policy.h"),
                 "if (maxMHz - minMHz < minSpanMHz) maxMHz = minMHz + minSpanMHz;",
                 "the axis rule enforces a minimum span so tight curves do not "
                 "fill the whole plot height")
    require_text(_p(ctx, "gui_graph_axis_policy.h"), "gui_grid_floor(",
                 "axis floors round down on the grid with true floor "
                 "semantics, never truncating toward zero")
    require_text(_p(ctx, "gui_graph_axis_policy.h"), "gui_grid_ceil(",
                 "axis ceilings round up on the grid with true ceil semantics")


def check_all(ctx, require_text, forbid_text):
    check_visibility_neutral_projection(ctx, require_text, forbid_text)
    check_manual_refresh_preserves_presentation(ctx, require_text, forbid_text)
    check_manual_apply_origin(ctx, require_text)
    check_auto_profile_log_privacy(ctx)
    check_owner_draw_checkbox_repaint(ctx, require_text, forbid_text)
    check_right_anchored_controls(ctx, require_text, forbid_text)
    check_labeled_checkbox_hit_area(ctx, require_text, forbid_text)
    check_themed_message_box(ctx, require_text, forbid_text)
    check_overclock_range_hints(ctx, require_text, forbid_text)
    check_high_overclock_confirmation(ctx, require_text, forbid_text)
    check_pending_changes(ctx, require_text, forbid_text)
    check_graph_frequency_axis(ctx, require_text, forbid_text)
    check_lock_checkbox_render(ctx, require_text, forbid_text)
    check_tray_active_profile(ctx, require_text, forbid_text)
    check_tray_menu_does_not_raise_the_main_window(ctx, require_text, forbid_text)
    check_applied_profile_indicator_is_drift_free(ctx, require_text, forbid_text)
    check_auto_profile_enable_is_a_transition(ctx, require_text, forbid_text)
    check_service_profile_identity_survives_a_delta_apply(
        ctx, require_text, forbid_text)
    check_apply_in_flight_presentation(ctx, require_text, forbid_text)
    check_manual_mutation_result_presentation(ctx, require_text, forbid_text)
    check_service_actionability(ctx, require_text, forbid_text)
