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
    capture_cpp = _p(ctx, "main_runtime_capture.cpp")

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
    require_text(graph_cpp, "COL_PENDING, true);",
                 "the pending curve is drawn dashed, so the two series stay "
                 "distinguishable without relying on colour alone")
    require_text(graph_cpp, "gui_pending_next_changed_run(pendingChangedForPt",
                 "the pending curve covers only the stretches that differ; "
                 "drawing it full-length hid the applied curve everywhere")
    require_text(graph_cpp, "pendingChangedForPt[i] ? COL_PENDING : COL_TEXT",
                 "on-curve MHz labels are coloured per point, not per graph")

    # A global GPU offset moves points nobody typed. Apply re-places an unowned
    # point at stock + the new offset component (gpu_backend_apply.cpp), so the
    # preview has to project it the same way instead of showing the value it
    # currently holds under the offset that is still applied.
    require_text(policy_h, "gui_pending_offset_shift_changed",
                 "a changed global GPU offset marks the points it moves, even "
                 "though none of them was typed")
    require_text(pending_cpp, "gui_pending_mark_gpu_offset_shift",
                 "the pending diff accounts for the global GPU offset")
    require_text(graph_cpp, "curve_base_khz_for_point(ci)",
                 "an offset-moved point is projected from stock, matching the "
                 "apply path rather than its current displayed value")

    # g_app.lockedFreq lags a profile projection: apply_lock() infers it from
    # GuiDraft, which populate_desired_into_gui() fills in only afterwards. The
    # preview, the diff, and Apply must therefore resolve the lock target the same
    # way capture_gui_desired_settings() does -- draft anchor first.
    require_text(policy_h, "gui_pending_lock_target_mhz",
                 "the lock target is resolved by one pure decision")

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
    require_text(graph_cpp, "gui_editor_lock_target_mhz()",
                 "the previewed locked tail uses the resolved lock target, not "
                 "the possibly stale g_app.lockedFreq")
    # Precise on purpose: the remaining g_app.lockedFreq reads in the graph are
    # the locked-tail DRIFT diagnostic, which legitimately compares live readback
    # against the applied lock. Only returning it as a previewed value is wrong.
    forbid_text(graph_cpp, "return g_app.lockedFreq;",
                "the previewed tail no longer returns g_app.lockedFreq directly")
    require_text(_p(ctx, "config_profiles_gui_state.cpp"),
                 "g_app.lockedFreq = lockMHz;",
                 "a profile projection states its own lock target instead of "
                 "letting apply_lock() infer a stale one from the draft")


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

    ctx.require_text_in_operation(
        fan_runtime, "static void update_tray_icon()",
        "gui_apply_in_flight_tray_icon_state(",
        "the tray theme comes from the pure policy, in-flight state included")
    ctx.require_text_in_operation(
        tray_presentation, "static void build_tray_tooltip(",
        "gui_apply_in_flight_tray_tooltip(",
        "the tray tooltip reports a write in flight")
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
    _require_in_operation(
        ctx, layout_cpp, place,
        "main_layout_right_anchored_x(plan.contentWidth, dp(118)",
        "the License button is right-anchored against the content width")
    _forbid_in_operation(
        ctx, layout_cpp, place, "dp(1006), ocY - dp(1)",
        "the fan-curve button is right-anchored, not pinned to a fixed x")
    _forbid_in_operation(
        ctx, layout_cpp, place, "gpuSelectW - dp(12)",
        "the GPU selector uses the canonical side inset, not its own 12 px")


def check_all(ctx, require_text, forbid_text):
    check_manual_apply_origin(ctx, require_text)
    check_owner_draw_checkbox_repaint(ctx, require_text, forbid_text)
    check_right_anchored_controls(ctx, require_text, forbid_text)
    check_labeled_checkbox_hit_area(ctx, require_text, forbid_text)
    check_themed_message_box(ctx, require_text, forbid_text)
    check_overclock_range_hints(ctx, require_text, forbid_text)
    check_high_overclock_confirmation(ctx, require_text, forbid_text)
    check_pending_changes(ctx, require_text, forbid_text)
    check_lock_checkbox_render(ctx, require_text, forbid_text)
    check_tray_active_profile(ctx, require_text, forbid_text)
    check_applied_profile_indicator_is_drift_free(ctx, require_text, forbid_text)
    check_auto_profile_enable_is_a_transition(ctx, require_text, forbid_text)
    check_service_profile_identity_survives_a_delta_apply(
        ctx, require_text, forbid_text)
    check_apply_in_flight_presentation(ctx, require_text, forbid_text)
    check_service_actionability(ctx, require_text, forbid_text)
