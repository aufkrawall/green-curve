"""Source gates for the Linux TUI, terminal integration and daemon policy.

Split out of build.py so the build script stays under its size ratchet; build.py
owns the CLI and passes its paths and check helpers in through `ctx`.  Nothing
here imports build.py, so the dependency runs one way only.

`ctx` is any object exposing SOURCE_DIR and SCRIPT_DIR.
"""
import os
import sys


def _p(ctx, name):
    return os.path.join(ctx.SOURCE_DIR, name)


def check_tui_layout(ctx, require_text, forbid_text, require_order):
    """F-LNX-TUI: root-cause fix for the reported mouse-offset bug plus the
    keyboard/daemon parity that came with it.  One cell grid owns both painting
    and hit testing, so there is no second row counter to drift out of step."""
    tui_cpp = _p(ctx, "linux_tui.cpp")
    layout_cpp = _p(ctx, "linux_tui_layout.cpp")
    render_cpp = _p(ctx, "linux_tui_render.cpp")
    actions_cpp = _p(ctx, "linux_tui_actions.cpp")
    forbid_text(tui_cpp, "Daemon not running. Install it with",
                "Linux TUI preserves detailed daemon errors instead of masking permission denial")
    require_text(layout_cpp, "tui_layout_actions_valid",
                 "TUI validates disjoint in-bounds hitboxes from its cell grid")
    require_text(layout_cpp, "register_action",
                 "TUI hitboxes are registered by the shared cell-grid builder")
    forbid_text(tui_cpp, "int y = 1;",
                "TUI no longer hand-tracks a row counter that drifts from the printed rows")
    require_text(render_cpp, "build_tui_layout",
                 "TUI renders and hit-tests from the shared pure layout builder")
    require_text(render_cpp, "event.mouseButton & 64",
                 "TUI handles wheel scrolling separately from clicks")
    require_text(render_cpp, "(event.mouseButton & 3) != 0",
                 "TUI mouse activation accepts only the left button")
    require_text(render_cpp, "focus_spatial",
                 "TUI has spatial keyboard navigation")
    require_text(render_cpp, "renderedRows",
                 "TUI redraws only changed terminal rows")
    require_text(actions_cpp, "linux_daemon_apply_checked",
                 "TUI Apply carries reconnect-safe daemon preconditions")
    forbid_text(actions_cpp, "memcmp(&left, &right",
                "TUI dirty-state comparison ignores struct padding")
    require_order(actions_cpp, "bool flushed = fflush(file) == 0;",
                  "bool closed = fclose(file) == 0;",
                  "TUI live export always closes its output after flushing")


def check_tui_graph_axes(ctx, require_text, forbid_text):
    """The VF graph is a scale, not a picture: both axes carry labels.

    The panel used to end with one centred "450-1240 mV - 180-3187 MHz"
    summary, so the two endpoints were the only numbers on it and nothing
    between them could be read off the trace.  The clock scale is reserved out
    of the plot's own width, so a narrow terminal drops the labels rather than
    the graph, and the voltage ticks share the inverse of the mapping
    tui_nearest_graph_point() uses so a tick cannot name a different point than
    a click on that column selects.
    """
    vf_cpp = _p(ctx, "linux_tui_layout_vf.cpp")
    require_text(vf_cpp, "void draw_vf_graph_y_axis(",
                 "the VF graph labels its clock axis")
    require_text(vf_cpp, "void draw_vf_graph_x_axis(",
                 "the VF graph labels its voltage axis")
    require_text(vf_cpp, "TUI_GRAPH_MIN_PLOT_WIDTH",
                 "the axis margin is only reserved while the plot keeps its "
                 "minimum width")
    require_text(vf_cpp, "graph_column_voltage",
                 "voltage ticks invert the same mapping the trace and hit "
                 "testing use")
    forbid_text(vf_cpp, '"%u–%u mV  •  %d–%d MHz"',
                "the endpoints-only range summary was replaced by real axes")


def check_tui_field_selection(ctx, require_text, forbid_text):
    """Entering a numeric field pre-selects it; a second click drops that.

    tui_begin_edit() seeds the buffer with the current value, and the character
    handler used to APPEND unconditionally -- so clicking a field showing 100
    and typing 50 left 10050 and the value had to be cleared by hand first.
    The rule is pure and unit-tested; these gates pin that the TUI actually
    routes through it rather than growing a second copy of the append logic.
    """
    policy_h = _p(ctx, "linux_tui_edit_policy.h")
    actions_cpp = _p(ctx, "linux_tui_actions.cpp")
    render_cpp = _p(ctx, "linux_tui_render.cpp")
    layout_cpp = _p(ctx, "linux_tui_layout.cpp")

    require_text(policy_h, "tui_edit_insert_character",
                 "typing into a field is a pure decision")
    require_text(policy_h, "tui_edit_click_targets_active_field",
                 "the second-click rule is a pure decision")
    require_text(actions_cpp, "state->edit.selectAll = true;",
                 "opening a field pre-selects its contents")
    require_text(actions_cpp, "tui_edit_insert_character(state->edit.text",
                 "the character handler routes through the shared rule")
    forbid_text(actions_cpp, "state->edit.text[length] = character;",
                "the append logic is not duplicated next to the shared rule")
    require_text(render_cpp, "tui_edit_backspace(state->edit.text",
                 "backspace routes through the shared rule so a selection is "
                 "cleared as one edit")
    require_text(render_cpp, "state->edit.selectAll = false;",
                 "a click into the open field drops the pre-selection")
    # Committing and reopening would push the value through the field's
    # clamping and discard a partially typed number, so the second click must
    # not take the ordinary commit path.
    require_text(render_cpp, "tui_edit_click_targets_active_field(state->edit.active",
                 "the second click is recognised before the commit path")
    require_text(layout_cpp, "TUI_STYLE_FIELD_SELECTED",
                 "a pre-selected value renders highlighted rather than being "
                 "discoverable only by typing")


def check_tui_exit(ctx, require_text, forbid_text):
    """F-LNX-EXIT: quitting the TUI must hand the terminal back in one step.

    The exit byte stream used to end with two identical presentation restores
    preceded by empty synchronized-update batches, and finished exactly at the
    alternate-screen exit.  On Konsole the next write then went unpainted until
    more output arrived, so the shell prompt only appeared after one extra
    keypress.  Three invariants keep that from coming back."""
    tui_cpp = _p(ctx, "linux_tui.cpp")
    render_cpp = _p(ctx, "linux_tui_render.cpp")
    # One definition of the restore sequence, ending on a fresh line.
    require_text(tui_cpp, "#define TUI_PRESENTATION_RESTORE",
                 "the TUI presentation restore exists once, not per call site")
    require_text(tui_cpp, "\\x1b[?1049l\\r\\n",
                 "the presentation restore ends on a fresh line so the next write is painted")
    # The supervisor restores only when the child left the terminal modified.
    require_text(tui_cpp, "terminal_modes_equal(afterChild, original)",
                 "the TUI supervisor only restores a terminal the child did not restore")
    forbid_text(tui_cpp, "memcmp(&afterChild",
                "terminal equality never compares padding bytes")
    # No frame is drawn once quitting has been decided.
    require_text(tui_cpp, "if (handled && state.running) tui_render(&state);",
                 "the TUI does not render another frame after the quit action")
    require_text(render_cpp, "if (!anyRowChanged)",
                 "an unchanged frame emits no synchronized-update batch at all")


def check_scroll_and_reveal(ctx, require_text, forbid_text):
    """F-LNX-VFSCROLL: the VF/fan lists stop at both ends and stay where the
    user scrolled them.

    Three separate defects made the wheel unusable and each has its own guard:
    the clamp used the raw VF_NUM_POINTS window instead of the last page of
    *populated* points; the per-frame auto-reveal undid any scroll that moved
    the selection off screen; and the 1 Hz refresh clamped the offset back down
    to the selected point once a second."""
    layout_cpp = _p(ctx, "linux_tui_layout.cpp")
    layout_h = _p(ctx, "linux_tui_layout.h")
    render_cpp = _p(ctx, "linux_tui_render.cpp")
    actions_cpp = _p(ctx, "linux_tui_actions.cpp")
    refresh_cpp = _p(ctx, "linux_tui_refresh.cpp")
    require_text(layout_cpp, "int tui_vf_max_first_visible(",
                 "the end-of-list scroll bound is a pure, testable function")
    require_text(layout_h, "static inline bool tui_selection_needs_reveal(",
                 "the reveal-on-select rule is pure and testable")
    require_text(layout_cpp, "int tui_vf_reveal_first_visible(",
                 "the reveal offset is a pure, testable function")
    forbid_text(render_cpp, "int remaining = state->layout.vfVisibleRows - 1;",
                "the reveal does not bottom-align the selection behind a full "
                "page of the flat low-voltage floor")
    require_text(render_cpp, "bool reveal_selected_point(TuiState* state)",
                 "a reveal that could not run reports it, so it is not "
                 "recorded as done before the daemon arrives")
    # The flat low-voltage floor is not listed, and the count is stated.
    require_text(layout_cpp, "int tui_vf_first_listed_point(",
                 "the flat low-voltage floor bound is a pure, testable function")
    require_text(_p(ctx, "linux_tui_layout_vf.cpp"), "flat pts hidden",
                 "the table states how many low points it leaves out")
    require_text(_p(ctx, "linux_tui_layout_vf.cpp"),
                 "if (index < firstListed) index = firstListed;",
                 "the table never draws below the first listed point")
    # A layout with no VF table carries no scroll information; treating it as a
    # maximum of zero rewound the list every time another tab was showing.
    require_text(render_cpp,
                 "if (state.layout.vfVisibleRows <= 0) return candidate",
                 "clamping without a VF table in the layout leaves the offset alone")
    for path in (render_cpp, actions_cpp):
        require_text(path, "tui_clamp_vf_scroll(",
                     "every VF scroll path clamps against the end of the list")
        forbid_text(path, "VF_NUM_POINTS - 1)",
                    "VF scrolling is not clamped to the raw point window")
    require_text(render_cpp,
                 "tui_selection_needs_reveal(state->selectedPoint, state->revealedPoint)",
                 "the auto-reveal fires only when the selection moved")
    forbid_text(refresh_cpp, "state->vfScroll = state->selectedPoint;",
                "the live refresh never drags the scroll back to the selection")


def check_client_diagnostics(ctx, require_text, forbid_text):
    """F-LNX-DIAGCLIENT: a user who cannot reach the daemon must be able to find
    out why from the log alone.

    The permission diagnostic already existed but only ever reached the caller,
    where the TUI shows it truncated in a status row for one frame."""
    transport = _p(ctx, "linux_daemon_transport.cpp")
    daemon_cpp = _p(ctx, "linux_daemon.cpp")
    main_cpp = _p(ctx, "linux_main.cpp")
    require_text(transport, "void linux_daemon_log_client_environment()",
                 "the client records its daemon-access context up front")
    require_text(transport, "sudo usermod -aG greencurve",
                 "the log names the group remedy verbatim")
    require_text(transport, "static void log_client_failure(",
                 "client failures are logged, deduplicated against the 1 Hz refresh")
    require_text(transport, "log_client_failure(\"connect\", error)",
                 "a refused connection is logged with the full diagnostic")
    require_text(main_cpp, "linux_daemon_log_client_environment()",
                 "every client role records its access context")
    require_text(daemon_cpp, "the client binary is out of date",
                 "the daemon names a rejected caller instead of silently erroring")
    require_text(daemon_cpp, "rejected malformed request pid=%u",
                 "a malformed request is logged with the fields that failed")


def check_terminal_relaunch(ctx, require_text, forbid_text):
    """F-LNX-TERM: a launch from a graphical file manager opens a terminal.

    The selection table stays in the pure policy header so the regression
    harness can cover it, and the launcher execs a fixed argv -- never a shell
    string -- so a path containing spaces or quotes cannot be re-parsed."""
    policy_h = _p(ctx, "linux_terminal_policy.h")
    launch_cpp = _p(ctx, "linux_terminal_launch.cpp")
    main_cpp = _p(ctx, "linux_main.cpp")
    require_text(policy_h, "linux_terminal_should_relaunch",
                 "the relaunch decision is pure and testable")
    require_text(policy_h, "linux_terminal_select",
                 "terminal selection is pure and testable")
    require_text(launch_cpp, "execvp(choice.command, relaunchArgs)",
                 "the terminal is started with a fixed argument vector")
    forbid_text(launch_cpp, "system(",
                "the terminal relaunch never goes through a shell")
    forbid_text(launch_cpp, "popen(",
                "the terminal relaunch never goes through a shell")
    require_text(launch_cpp, "--from-desktop",
                 "the relaunched process carries the guard that prevents recursion")
    require_text(main_cpp, "linux_terminal_relaunch_wanted(opts.fromDesktop)",
                 "the TUI path checks for a graphical launch before failing")
    require_text(main_cpp, "Press Enter to close this window",
                 "a desktop-launched failure holds the window open instead of vanishing")


def check_debug_log(ctx, require_text, forbid_text):
    """F-LNX-DEBUGLOG: the Linux binary writes the same durable log Windows has.

    The daemon cannot log next to its own binary: systemd mounts /usr read-only
    for the unit (ProtectSystem=full), so its log goes to the state directory
    instead.  The fatal-signal breadcrumb reuses the already-open descriptor,
    which is why rotation must preserve the descriptor number."""
    log_h = _p(ctx, "linux_debug_log.h")
    log_cpp = _p(ctx, "linux_debug_log.cpp")
    main_cpp = _p(ctx, "linux_main.cpp")
    daemon_cpp = _p(ctx, "linux_daemon.cpp")
    breadcrumb_h = _p(ctx, "linux_crash_breadcrumb.h")
    require_text(log_h, "static inline bool linux_debug_log_resolve_path",
                 "the debug log path rule is pure and covered by the harness")
    require_text(log_h, "static inline bool linux_debug_log_enabled_for",
                 "the debug enable rule is pure and covered by the harness")
    require_text(log_cpp, "dup2(fresh, g_fd)",
                 "log rotation preserves the descriptor the crash handler writes to")
    require_text(main_cpp, "linux_debug_log_enabled_for(",
                 "debug logging honours [debug] enabled and GREEN_CURVE_DEBUG")
    require_text(main_cpp, "linux_set_crash_log_fd(linux_debug_log_raw_fd())",
                 "a fatal signal leaves a breadcrumb in the log file too")
    require_text(daemon_cpp, "linux_set_crash_log_fd(linux_debug_log_raw_fd())",
                 "the daemon publishes the log descriptor to its own breadcrumb copy")
    require_text(daemon_cpp, "if (!linux_debug_log_is_enabled()) return;",
                 "daemon diagnostics route through the shared debug sink")
    # The breadcrumb must stay async-signal-safe: write(2) only, no formatting.
    for forbidden in ("snprintf", "fprintf", "strsignal"):
        forbid_text(breadcrumb_h, forbidden,
                    "crash breadcrumbs stay async-signal-safe (write(2) only)")


def check_crash_report(ctx, require_text, forbid_text, require_order):
    """F-LNX-CRASH: the Linux crash report, the counterpart of a Windows minidump.

    Linux ships stripped and leaves the core file to the kernel, so the two
    things that decide whether a crash is diagnosable are (a) a report carrying
    the fault address, PC/SP and /proc/self/maps, and (b) an extracted .debug
    file with a matching build-id.  Neither existed before; a Linux crash left a
    signal number and nothing else.
    """
    breadcrumb_h = _p(ctx, "linux_crash_breadcrumb.h")
    report_h = _p(ctx, "linux_crash_report.h")
    report_cpp = _p(ctx, "linux_crash_report.cpp")
    main_cpp = _p(ctx, "linux_main.cpp")
    daemon_cpp = _p(ctx, "linux_daemon.cpp")
    policy_h = _p(ctx, "crash_artifact_policy.h")

    # SA_SIGINFO is what makes the report actionable rather than a bare signal
    # number: si_code, si_addr and the register context all come from it.  The
    # crash itself is still never suppressed -- SA_RESETHAND + re-raise is
    # asserted by the pre-existing F-LNX-DIAG gates in build.py.
    require_text(breadcrumb_h, "SA_RESETHAND | SA_NODEFER | SA_SIGINFO",
                 "the handler takes siginfo without losing the default-disposition reset")
    require_text(breadcrumb_h, "gc_fault_registers",
                 "the report records the PC/SP at the fault")
    require_text(breadcrumb_h, "gc_signal_safe_write_maps",
                 "the report records /proc/self/maps so a PIE address is attributable")
    require_text(breadcrumb_h, "si_addr",
                 "the report records the faulting address")
    # Still async-signal-safe after the additions.  build.py forbids the
    # formatting calls; these are the allocating/unwinding ones a backtrace
    # helper would drag in.
    for forbidden in ("malloc(", "backtrace"):
        forbid_text(breadcrumb_h, forbidden,
                    "the crash report stays async-signal-safe (write/read/open only)")
    # RLIMIT_CORE belongs to the user.  Raising it to force a core would be a
    # side effect this program has no business having, and a rejected approach
    # is worth a guard so it does not get "fixed" back in.
    # Forbid the CALL, not the constant: both files discuss the decision in
    # prose, and a guard that trips on its own rationale is one that gets
    # deleted rather than obeyed.
    for path in (report_cpp, breadcrumb_h):
        forbid_text(path, "setrlimit(",
                    "the user's own ulimit -c is never overridden")

    # Unlike the per-TU log descriptor, the report state lives in one
    # program-wide slot, so a translation unit that installs handlers cannot
    # forget to arm it and silently write nothing on a crash.
    require_text(breadcrumb_h, "inline volatile sig_atomic_t& gc_crash_report_fd_slot()",
                 "the report descriptor has a single program-wide instance")
    require_text(breadcrumb_h, "inline const char* volatile& gc_crash_report_path_slot()",
                 "the report path has a single program-wide instance")
    forbid_text(breadcrumb_h, "static volatile sig_atomic_t g_crashReportFd",
                "the report descriptor is not a per-TU static")
    forbid_text(daemon_cpp, "linux_set_crash_report_path(",
                "the daemon inherits the armed report path rather than re-arming")

    # F-LNX-CRASH-LAZY: the report file is created by the crash handler and
    # NOWHERE else.  Arming used to open it, which meant every run of every role
    # dropped a 0-byte placeholder next to config.ini -- removed only by
    # linux_crash_report_close(), which nothing called, and which execve()
    # (the TUI's terminal relaunch), _exit() and SIGKILL cannot call anyway.
    # The invariant that replaced it is structural: a report file exists if and
    # only if a crash wrote bytes into it.  These three guards are what keep an
    # "arm it earlier so we know it works" change from bringing the spam back.
    require_text(breadcrumb_h, "gc_crash_report_open_on_demand",
                 "the report file is created on the first fatal write, not at startup")
    require_text(breadcrumb_h, "if (!data || length == 0) return;",
                 "a zero-length emit cannot create a 0-byte report")
    forbid_text(report_cpp, "O_CREAT",
                "arming the report must not create the file; only the handler does")
    require_text(breadcrumb_h, "O_CLOEXEC",
                 "a forked helper cannot inherit the report descriptor")
    require_text(breadcrumb_h, "gc_crash_report_reserve_slot",
                 "a descriptor is reserved up front so a crash under EMFILE can "
                 "still open its report")

    require_text(report_h, "linux_crash_report_path",
                 "the report path is reachable from the arming call site")
    require_text(report_cpp, "gc_linux_crash_dir(",
                 "the report directory comes from the shared fail-closed policy")
    require_text(report_cpp, "gc_crash_artifact_is_older",
                 "report rotation orders by the embedded timestamp")
    require_text(policy_h, "gc_linux_crash_dir",
                 "the Linux report directory rule is pure and covered by the harness")
    require_text(main_cpp, "linux_set_crash_report_path(linux_crash_report_path())",
                 "the report path is armed at startup")
    require_order(main_cpp, "linux_crash_report_configure(",
                  "linux_set_crash_report_path(linux_crash_report_path())",
                  "the path is resolved before the handler is armed with it")

    # The invariant is only worth as much as the fixture that executes it, and
    # that fixture needs real fork/signal/file behaviour, so it can only run on
    # a Linux host.  Guard both halves: that the fixture asserts the two
    # directions, and that build.py actually compiles and runs it.
    fixture_cpp = os.path.join(ctx.SCRIPT_DIR, "tests",
                               "linux_crash_report_regression.cpp")
    build_script = os.path.join(ctx.SCRIPT_DIR, "build.py")
    require_text(fixture_cpp, "arming does not create the report file",
                 "the fixture asserts that a normal run leaves no artifact")
    require_text(fixture_cpp, "a fatal write creates the report file",
                 "the fixture asserts that a crash still produces its report")
    require_text(fixture_cpp, "an execve'd process leaves no report behind",
                 "the fixture covers the relaunch path that cleanup cannot reach")
    require_text(build_script, '"linux_crash_report_regression"',
                 "build.py compiles and runs the crash-report fixture")


def check_startup_policy(ctx, require_text, forbid_text):
    """F-LNX-STARTUP: the daemon's boot-apply policy.

    The record authorizes an unattended hardware write, so it is persisted with
    the same root-owned checksummed atomic write as the committed intent, and a
    present-but-unreadable record must stop the write rather than silently
    falling back to replaying old intent."""
    protocol_h = _p(ctx, "service_protocol.h")
    state_h = _p(ctx, "linux_daemon_state.h")
    state_cpp = _p(ctx, "linux_daemon_state.cpp")
    daemon_cpp = _p(ctx, "linux_daemon.cpp")
    policy_h = _p(ctx, "linux_startup_policy.h")
    cli_cpp = _p(ctx, "linux_cli_options.cpp")
    require_text(protocol_h, "SERVICE_PROTOCOL_VERSION = 18",
                 "the startup-policy, readback-validity and outcome-severity "
                 "fields each moved the protocol version with them")
    # F-LNX-STARTUP-SNAPSHOT: the boot-apply snapshot must travel in its own
    # response member. It used to ride in `desired`, which the end-of-request
    # stamp overwrites, so every staleness check compared the applied settings
    # against the profile instead of the snapshot.
    require_text(protocol_h, "DesiredSettings startupProfile",
                 "the boot-apply snapshot has its own response field")
    require_text(policy_h, "daemon_publish_startup_snapshot",
                 "the daemon publishes the committed boot-apply snapshot")
    forbid_text(policy_h, "resp->desired = g_startupPolicy.desired",
                "the boot-apply snapshot no longer borrows the active-intent field")
    require_text(_p(ctx, "linux_startup_sync.cpp"), "policy.startupProfile",
                 "the staleness report reads the snapshot, not the active intent")
    forbid_text(_p(ctx, "linux_startup_sync.cpp"), "report.snapshot = policy.desired",
                "the staleness report never compares the applied settings again")
    require_text(protocol_h, "SERVICE_CMD_SET_STARTUP_POLICY",
                 "the startup policy is configured over the authenticated socket")
    require_text(protocol_h, "startupPolicyMode",
                 "every state envelope publishes the startup policy")
    require_text(state_h, "linux_daemon_startup_valid",
                 "the startup record has an explicit validity contract")
    require_text(state_h, "record->targetGpu.valid && record->targetGpu.pciInfoValid",
                 "a boot-apply profile must name an exact GPU identity")
    require_text(state_cpp, "store_record_atomic(path, record, sizeof(*record), \"startup policy\"",
                 "the startup policy uses the shared atomic root-owned store")
    require_text(policy_h, "g_startupPolicy.mode = SERVICE_STARTUP_POLICY_NONE;",
                 "an unreadable startup policy stops the boot write instead of guessing")
    require_text(daemon_cpp, "g_startupPolicy.mode != SERVICE_STARTUP_POLICY_RESTORE_LAST",
                 "a non-default policy suppresses the committed-intent replay")
    # The in-memory copy is assigned only on the success arm, after the store.
    ctx.require_order_in_operation(
        policy_h, "static void daemon_handle_set_startup_policy(",
        "linux_daemon_startup_store(", "g_startupPolicy = record;",
        "an unpersisted startup policy is never advertised as committed")
    require_text(policy_h, "// per daemon start, only for the exact GPU identity recorded with the policy,",
                 "the boot write is a configured event, not drift correction")
    require_text(cli_cpp, "--startup-profile",
                 "the startup policy is reachable from the CLI")
    # F-LNX-STARTUP-SYNC: the boot snapshot must follow the profile it names.
    # The daemon cannot read config.ini, so a profile write that does not push
    # the new content leaves the GPU booting last week's settings while the
    # control still says "PROFILE N".
    snapshot_h = _p(ctx, "startup_snapshot_policy.h")
    sync_cpp = _p(ctx, "linux_startup_sync.cpp")
    tui_actions = _p(ctx, "linux_tui_actions.cpp")
    main_cpp = _p(ctx, "linux_main.cpp")
    require_text(protocol_h, "SERVICE_CMD_REFRESH_STARTUP_PROFILE",
                 "a bound boot-apply snapshot can be refreshed without re-binding it")
    require_text(snapshot_h, "startup_snapshot_refresh_allowed",
                 "the daemon-side refresh guard is a pure, testable decision")
    require_text(policy_h, "startup_snapshot_refresh_allowed(g_startupPolicy.mode,",
                 "a refresh only ever updates a policy that already names the slot")
    require_text(policy_h, "&g_startupPolicy.targetGpu, &requested);",
                 "a refresh reuses the stored GPU binding instead of trusting the caller")
    require_text(sync_cpp, "linux_daemon_refresh_startup_profile(",
                 "the shared client path pushes the reloaded profile to the daemon")
    require_text(sync_cpp, "load_profile_from_config_path(configPath, slot",
                 "the pushed snapshot is reloaded from disk, not taken from memory")
    require_text(tui_actions, "sync_startup_snapshot_after_profile_write(state,",
                 "the TUI pushes the new content after writing a bound profile slot")
    require_text(main_cpp, "linux_startup_sync_after_profile_write(",
                 "--save-config refreshes a boot-apply snapshot of the same slot")
    require_text(main_cpp, "print_startup_snapshot_report(stdout,",
                 "--show-startup reports a snapshot that drifted from its profile")
    # The store must succeed before the in-memory policy adopts the refresh, or
    # a policy that never reached disk would be advertised as committed.
    ctx.require_order_in_operation(
        policy_h, "static void daemon_handle_refresh_startup_profile(",
        "linux_daemon_startup_store(", "g_startupPolicy = record;",
        "an unpersisted startup refresh is never advertised as committed")


def check_release_packaging(ctx, require_text, forbid_text):
    """F-LNX-PKG: the Linux archive ships the setup script, and never a log.

    The debug log records GPU identifiers, config paths and applied settings, so
    a developer who ran the binary out of dist/ must not be able to publish it
    inside a release archive."""
    build_script = os.path.join(ctx.SCRIPT_DIR, "build.py")
    manifest = os.path.join(ctx.SCRIPT_DIR, "tools", "release_manifest.py")
    setup_script = os.path.join(ctx.SCRIPT_DIR, "tools", "greencurve-setup.sh")
    require_text(build_script, "purge_runtime_artifacts(payload)",
                 "runtime artifacts are removed from a payload before it is archived")
    require_text(manifest, "greencurve_debug.txt",
                 "the debug log is on the runtime-artifact purge list")
    require_text(manifest, '"greencurve-setup.sh"',
                 "the Linux archive manifest includes the setup script")
    require_text(setup_script, "--service-install",
                 "the setup script defers privileged work to the verified binary path")
    require_text(setup_script, "usermod -aG",
                 "the setup script performs the group enrollment the binary refuses to do")
    require_text(setup_script, 'runuser -u "$user" -- sh -c',
                 "desktop files in a user-controlled tree are never written with root privileges")
    forbid_text(setup_script, 'cat > "$entry"',
                "root setup must not follow a planted desktop-entry symlink")
    # F-LNX-GROUPADVICE.  The install summary used to prescribe usermod to
    # everyone, including users who already had the group and users the wrapper
    # was about to enroll on the very next line.
    main_cpp = os.path.join(ctx.SOURCE_DIR, "linux_main.cpp")
    install_cpp = os.path.join(ctx.SOURCE_DIR, "linux_service_install.cpp")
    require_text(main_cpp, "linux_describe_group_enrollment(groupAdvice",
                 "the install summary asks the policy instead of assuming")
    forbid_text(main_cpp, "To control the GPU without sudo, add your user",
                "the unconditional group instruction must stay out of the "
                "install summary; the advice is decided per account")
    require_text(install_cpp, "GREENCURVE_SETUP_OWNS_GROUP",
                 "the wrapper can claim ownership of the group message it acts on")
    require_text(setup_script, "GREENCURVE_SETUP_OWNS_GROUP=1",
                 "the setup script claims that ownership when it resolved an account")
    require_text(setup_script, "logname",
                 "a plain root shell still resolves the login account to enroll")
    require_text(setup_script, "pass --purge to remove it",
                 "uninstall keeps persisted settings unless --purge is given")
    check_setup_script_line_endings(setup_script)
    # F-LNX-EOL / F-LNX-MODE: the archive must be identical off any build host.
    gates = os.path.join(ctx.SCRIPT_DIR, "tools", "security_gates.py")
    require_text(gates, "def check_linux_release_packaging",
                 "a real packaging round-trip covers the Linux archive")
    require_text(gates, "check_linux_release_packaging(ctx)",
                 "that round-trip is actually wired into the build-script regressions")
    require_text(manifest, "def write_linux_tarball",
                 "the Linux archive is written by the standard library, which records Unix modes")
    forbid_text(build_script, 'os.chmod(staged, 0o755)',
                "the executable bit must come from the archive header, not from a "
                "chmod that is a no-op on the Windows build host")


def check_setup_script_line_endings(setup_script):
    """The shipped wrapper must be LF in the working tree, not merely in git.

    `.gitattributes` pins `*.sh` to `eol=lf`, but a clone made before that file
    existed keeps its CRLF working copy until the next checkout.  Reading the
    bytes is the only check that notices, and the failure it prevents is total:
    Linux resolves `#!/usr/bin/env bash\\r` as an interpreter named "bash\\r"."""
    with open(setup_script, "rb") as handle:
        data = handle.read()
    if b"\r" not in data:
        return
    print("Regression source check FAILED: tools/greencurve-setup.sh has CRLF line "
          "endings in the working tree")
    print("  A CRLF shell script cannot run on Linux (bad interpreter: bash^M).")
    print("  Fix this checkout with:")
    print("    git add --renormalize . && git checkout -- tools/greencurve-setup.sh")
    sys.exit(1)


def check_auto_restore(ctx, require_text, forbid_text, require_order):
    """F-LNX-AUTORESTORE: unattended writes and the unit that survives them.

    Three things write the GPU without a user asking -- the restore-last
    replay, the profile boot apply, and the standby-resume restore -- and all
    three must go through one path, because that path is where the OC baseline
    is reset first and where the crash-loop guard lives. Both used to be
    missing: the boot replays applied the persisted DesiredSettings directly
    (and `resetOcBeforeApply` is stripped before persistence, so every
    automatic Linux write stacked onto whatever the driver already held), and
    a setting that hung the driver was replayed by every systemd restart
    forever."""
    protocol_h = _p(ctx, "service_protocol.h")
    lifecycle_h = _p(ctx, "service_lifecycle_policy.h")
    policy_h = _p(ctx, "linux_auto_restore_policy.h")
    runtime_h = _p(ctx, "linux_auto_restore_runtime.h")
    startup_h = _p(ctx, "linux_startup_policy.h")
    daemon_cpp = _p(ctx, "linux_daemon.cpp")
    serve_h = _p(ctx, "linux_daemon_serve.h")
    state_h = _p(ctx, "linux_daemon_state.h")
    state_cpp = _p(ctx, "linux_daemon_state.cpp")
    notify_cpp = _p(ctx, "linux_systemd_notify.cpp")
    install_cpp = _p(ctx, "linux_service_install.cpp")
    install_policy_h = _p(ctx, "linux_service_install_policy.h")
    cli_cpp = _p(ctx, "linux_cli_options.cpp")
    main_cpp = _p(ctx, "linux_main.cpp")

    # --- the reset-before-apply contract -----------------------------------
    # The shared builder is the ONLY thing allowed to decide this, on either
    # platform. A lock composes a VF anchor/tail write, so a lock-only intent
    # owns VF policy exactly as a curve does.
    require_text(lifecycle_h,
                 "requestOut->resetOcBeforeApply = service_intent_owns_vf_cleanup(activeIntent);",
                 "a full restore derives reset-to-stock from the shared ownership predicate")
    require_text(lifecycle_h,
                 "bool nextOwnsVf = service_intent_owns_vf_cleanup(nextIntent);",
                 "a profile transition uses the same ownership predicate on both sides")
    require_text(runtime_h, "service_build_full_restore_request(&committed, &request)",
                 "every unattended Linux write is built by the shared restore-request builder")
    # The three callers must not hand-roll a request around it again.
    forbid_text(startup_h, "linux_backend_apply(",
                "the profile boot apply goes through the shared unattended-write path")
    require_text(startup_h, "LINUX_AUTO_RESTORE_TRIGGER_BOOT_PROFILE",
                 "the profile boot apply is a typed automatic-restore trigger")
    require_text(daemon_cpp, "LINUX_AUTO_RESTORE_TRIGGER_BOOT_RESTORE_LAST",
                 "the restore-last replay is a typed automatic-restore trigger")
    require_text(runtime_h, "LINUX_AUTO_RESTORE_TRIGGER_RESUME",
                 "the resume restore is a typed automatic-restore trigger")

    # --- the crash-loop guard ----------------------------------------------
    require_text(policy_h, "LINUX_AUTO_RESTORE_MAX_START_ATTEMPTS = 3",
                 "the per-boot automatic start-write budget is explicit")
    require_text(policy_h, "guard->lockedOut = guard->lockedOut ? 1 : 0;",
                 "a new boot resets the attempt count but never the sticky lockout")
    require_text(state_h, "linux_daemon_guard_valid",
                 "the automatic-restore guard has an explicit validity contract")
    require_text(state_cpp, "store_record_atomic(path, &record, sizeof(record),",
                 "the guard uses the shared atomic root-owned store")
    require_text(state_cpp, "/proc/sys/kernel/random/boot_id",
                 "the per-boot identity comes from the kernel, not from a clock")
    require_text(runtime_h,
                 "linux_auto_restore_note_lockout(&g_autoRestoreGuard,\n"
                 "                SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED);",
                 "an unreadable guard fails closed to locked out, with a reason")

    # --- the published lockout ---------------------------------------------
    # The snapshot's autoRestoreLockoutReason used to be derived from
    # g_stateUncertain alone.  That flag is set by a failed WRITE; the crash-loop
    # arm latches the guard without issuing one, so a daemon that had
    # permanently stopped restoring settings at boot published LOCKOUT_NONE and
    # read as healthy everywhere.  The guard is the authority.
    snapshot_cpp = _p(ctx, "linux_daemon_snapshot_runtime.cpp")
    require_text(snapshot_cpp,
                 "s->autoRestoreLockoutReason = linux_auto_restore_published_lockout_reason(",
                 "the published lockout comes from the guard, not from a proxy flag")
    forbid_text(snapshot_cpp,
                "s->autoRestoreLockoutReason = g_stateUncertain",
                "the published lockout is never derived from the uncertain flag alone")
    # A locked-out guard that names no reason would publish LOCKOUT_NONE, which
    # is the regression itself; both halves are pinned together.
    require_text(policy_h,
                 "static inline gc_u32 linux_auto_restore_published_lockout_reason(",
                 "the published-lockout decision is pure and testable on both hosts")
    require_text(state_h, "gc_u32 lockoutReason;",
                 "the lockout reason survives a restart with the flag it explains")
    require_text(state_h, "LINUX_DAEMON_GUARD_VERSION = 2",
                 "adding the reason bumped the guard record version")
    # The attempt is persisted BEFORE the write, or a crash mid-write counts
    # nothing at all -- which is the exact case the guard exists for.
    require_order(runtime_h,
                  "linux_auto_restore_note_start_attempt(&g_autoRestoreGuard);",
                  "persist_auto_restore_guard(\"start attempt\")",
                  "the attempt is recorded before it is persisted")
    ctx.require_order_in_operation(
        runtime_h, "static bool auto_restore_authorize(",
        "persist_auto_restore_guard(\"start attempt\")", "return true;",
        "an attempt that could not be persisted refuses the write")
    require_text(daemon_cpp, "auto_restore_note_explicit_success(\"apply\");",
                 "only an explicit Apply re-arms automatic restoration")
    require_text(daemon_cpp, "auto_restore_note_explicit_success(\"reset\");",
                 "only an explicit Reset re-arms automatic restoration")
    require_text(runtime_h, "auto_restore_note_automatic_failure",
                 "a failed unattended write latches automatic restoration off")

    # --- resume ------------------------------------------------------------
    require_text(protocol_h, "SERVICE_CMD_RESUME_RESTORE = 15",
                 "the resume edge has its own command")
    require_text(_p(ctx, "service_protocol_validation.h"),
                 "resume restore carries mutation fields",
                 "a resume request carries no target, id, origin or flags")
    require_text(_p(ctx, "service_protocol_validation.h"),
                 "resume restore carries settings",
                 "a resume request carries no settings either")
    require_text(install_cpp, "greencurve-resume.service",
                 "the installer writes the resume unit")
    # WantedBy + After on the same sleep targets is what makes a unit run on the
    # way back UP rather than before suspending.
    require_text(install_cpp,
                 "\"After=suspend.target hibernate.target hybrid-sleep.target\"",
                 "the resume unit is ordered after the sleep targets")
    require_text(install_cpp,
                 "\"WantedBy=suspend.target hibernate.target hybrid-sleep.target\"",
                 "the resume unit is wanted by the sleep targets")
    require_text(install_cpp, "After=nvidia-resume.service",
                 "the resume restore waits for the driver's own resume unit, not for a delay")
    require_text(install_policy_h, "LINUX_SERVICE_STEP_ENABLE_RESUME",
                 "enabling the resume unit is a named install step")
    require_text(install_cpp, "unlink(GC_RESUME_UNIT_PATH);",
                 "--service-remove removes the resume unit")
    require_text(cli_cpp, "--resume-restore",
                 "the resume edge is reachable from the CLI")
    require_text(main_cpp, "linux_daemon_resume_restore(",
                 "the CLI verb sends the resume command")

    # --- the unit's own persistence ----------------------------------------
    require_text(install_cpp, "\"Restart=always\\n\"",
                 "an out-of-band kill -TERM does not leave the machine without GPU control")
    forbid_text(install_cpp, "Restart=on-failure",
                "the unit no longer declines to restart after a clean-signal exit")
    require_text(install_cpp, "\"StartLimitIntervalSec=600\\n\"",
                 "the start limit uses the SCM's ten-minute window, not systemd's 10s default")
    forbid_text(install_cpp, "\"After=multi-user.target\\n\\n\"",
                "the daemon is no longer ordered dead last in boot")
    require_text(install_cpp, "\"WatchdogSec=120\\n\"",
                 "a wedged daemon is restarted instead of sitting there active")
    require_text(notify_cpp, "WATCHDOG=1",
                 "the daemon answers the watchdog it asked for")
    require_text(notify_cpp, "WATCHDOG_PID",
                 "watchdog pings are refused when the manager expects another process")
    # The poll loop must stay event-driven when no watchdog is configured.
    require_text(serve_h, "watchdogIntervalMs ? (int)watchdogIntervalMs : -1",
                 "without a watchdog the accept loop still waits forever")
    require_text(daemon_cpp,
                 "if (!pl_thread_start(&fanThread, fan_reassert_thread, nullptr)) {",
                 "a daemon that cannot start its fan worker refuses to serve")
    require_text(daemon_cpp, "clear_auto_restore_attempts_on_clean_stop();",
                 "an orderly stop clears the per-boot attempt counter")


def check_all(ctx, require_text, forbid_text, require_order):
    check_tui_layout(ctx, require_text, forbid_text, require_order)
    check_tui_graph_axes(ctx, require_text, forbid_text)
    check_tui_field_selection(ctx, require_text, forbid_text)
    check_tui_exit(ctx, require_text, forbid_text)
    check_scroll_and_reveal(ctx, require_text, forbid_text)
    check_client_diagnostics(ctx, require_text, forbid_text)
    check_terminal_relaunch(ctx, require_text, forbid_text)
    check_debug_log(ctx, require_text, forbid_text)
    check_crash_report(ctx, require_text, forbid_text, require_order)
    check_startup_policy(ctx, require_text, forbid_text)
    check_auto_restore(ctx, require_text, forbid_text, require_order)
    check_release_packaging(ctx, require_text, forbid_text)
