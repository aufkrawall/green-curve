"""Source gates for the debug log's write path (F-LOG-ASYNC).

Split out of build.py so the build script stays under its size ratchet; build.py
owns the CLI and passes its paths and check helpers in through `ctx`. Nothing
here imports build.py, so the dependency runs one way only.

Why these rules exist, and why they are gates rather than unit tests:

Until 2026-09-12 `debug_log()` performed the whole file write inline, on the
caller's thread, and in the service that write is durable by construction --
FILE_FLAG_WRITE_THROUGH plus a FlushFileBuffers() after every single line.  The
service calls it from inside `service_handle_telemetry_request()`, which runs
under both the runtime lock and the process-wide serialized dispatch lock.  When
the C: volume stalled (Windows logged Volsnap event 25, "reduce the I/O load on
the system") one such flush blocked for 19.078 seconds, the service could not
answer any request for that whole time, and the GUI presented a healthy service
as a lost connection.

Every rule below therefore guards a NEGATIVE.  If the producer/writer split
regresses, nothing fails: the log still works, it is still complete, it is still
in order -- it has merely become able to wedge the service again, and the next
person finds out from a user report about a window that disconnected itself.

`ctx` is any object exposing SOURCE_DIR.
"""
import os


def _p(ctx, name):
    return os.path.join(ctx.SOURCE_DIR, name)


# Calls that can block for an unbounded time and therefore must not appear on a
# producer thread. GetFileSizeEx is in the list because the rotation check it
# serves is a metadata read against the same possibly-stalled volume.
_BLOCKING_CALLS = (
    "WriteFile(",
    "FlushFileBuffers(",
    "OutputDebugStringA(",
    "GetFileSizeEx(",
)

_DEBUG_LOG_ANCHOR = "static void debug_log(const char* fmt, ...)"


def check_all(ctx, require_text, require_text_in_operation,
              forbid_text_in_operation, require_order_in_operation):
    diagnostics_cpp = _p(ctx, "main_diagnostics.cpp")
    writer_cpp = _p(ctx, "main_debug_log_writer.cpp")
    crash_cpp = _p(ctx, "main_crash_artifacts.cpp")

    require_text(writer_cpp, "close_debug_log_file",
                 "debug log file cleanup exists")
    require_text(diagnostics_cpp, "open_debug_log_file_locked",
                 "debug log file open helper exists")
    # Debug logs are size-capped: every append passes the rotation check, and a
    # rotated file opens with an explanatory marker (2026-08 unbounded-growth
    # fix). The check moved with the handle, so the gate follows it.
    require_text(diagnostics_cpp, '#include "debug_log_rotation_policy.h"',
                 "debug log size-cap policy is compiled into the diagnostics shard")
    require_order_in_operation(
        writer_cpp,
        "static HANDLE debug_log_acquire_handle_locked()",
        "gc_debug_log_rotation::should_rotate(",
        "return g_debugLogFile;",
        "debug log lines append only after the size-cap rotation check")
    require_text(writer_cpp, "gc_debug_log_rotation::marker_line",
                 "a truncated debug log opens with an explanatory marker")

    # The producer half: format, hand off, return.
    require_text(diagnostics_cpp, '#include "debug_log_queue_policy.h"',
                 "debug log hand-off policy is compiled into the diagnostics shard")
    require_text_in_operation(
        diagnostics_cpp, _DEBUG_LOG_ANCHOR, "debug_log_enqueue(buf)",
        "debug_log() hands the formatted line to the writer thread")
    for call in _BLOCKING_CALLS:
        forbid_text_in_operation(
            diagnostics_cpp, _DEBUG_LOG_ANCHOR, call,
            f"debug_log() must not call {call.rstrip('(')} on the producer thread")
    # The file lock is the one that is held across I/O. A producer that takes it
    # has undone the split even if it performs no I/O of its own.
    forbid_text_in_operation(
        diagnostics_cpp, _DEBUG_LOG_ANCHOR,
        "EnterCriticalSection(&g_debugLogFileLock)",
        "debug_log() must not take the log FILE lock on the producer thread")

    # The writer half, and the crash path that makes the queue safe to have.
    require_text(writer_cpp, "static DWORD WINAPI debug_log_writer_thread_proc",
                 "the debug log has a dedicated writer thread")
    require_text(writer_cpp, "debug_log_drain_pending_for_crash",
                 "queued debug lines can be flushed to the crash breadcrumb")
    require_text(crash_cpp, "debug_log_drain_pending_for_crash()",
                 "the crash paths drain what the log writer has not written yet")

    check_steady_state_lines(ctx, require_text)
    check_cli_log_appends(ctx, require_text)
    check_auto_apply_failure_notice(ctx, require_text)


# The per-tick lines measured at ~97% of the 0.28 field log.  Each must route
# through the steady-state tracker (debug_log_steady_state_policy.h): a plain
# debug_log or a debug_log_on_change whose key carries a per-read counter
# brings back a log that holds two days.  (file, anchor that must be present)
_STEADY_STATE_SITES = (
    ("gui_service_io_log.h", "observe(&s_telemetry"),
    ("gui_service_io_log.h", "observe(&s_adoption"),
    ("main_service_state_envelope.cpp", "observe(\n        &s_envelopeLog"),
    ("xbar_telemetry.h", "observe(&s_xbarLog"),
    ("main_service_fan_worker.cpp", "observe(&s_pulseLog"),
    ("main_service_fan_worker.cpp", "observe(&s_waitLog"),
    ("main_service_fan_worker.cpp", "observe(&s_ensureLog"),
    # Fan-curve duty steps under load: one write per fan per poll.
    ("main_fan_runtime.cpp", "observe(&g_fanRuntimeWriteLog"),
)

# Text that, if it reappears, is the old unconditional per-tick form.
_STEADY_STATE_FORBIDDEN = (
    ("gui_service_state.cpp", '"GUI service state: phase=%s epoch=%llu instance=%llu revision='),
    ("main_service_state_envelope.cpp", '"service envelope: instance=%llu revision='),
    ("gui_mutation_worker.cpp", '"GUI service I/O: read kind='),
)


def _read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return handle.read()


def _fail(label):
    import sys
    print(f"Regression source check FAILED: {label}")
    sys.exit(1)


def check_steady_state_lines(ctx, require_text):
    require_text(_p(ctx, "app_shared.h"), '#include "debug_log_steady_state_policy.h"',
                 "every shard sees the steady-state log policy")
    require_text(_p(ctx, "gui_mutation_worker.cpp"), '#include "gui_service_io_log.h"',
                 "the GUI worker's read line goes through the summarizing helper")
    for name, anchor in _STEADY_STATE_SITES:
        if anchor not in _read(_p(ctx, name)):
            _fail(f"{name}: per-tick line must use the steady-state tracker ({anchor!r})")
    for name, needle in _STEADY_STATE_FORBIDDEN:
        if needle in _read(_p(ctx, name)):
            _fail(f"{name}: unsummarized per-tick log line is back ({needle!r})")
    # The runtime's duty writes keep the crash/wedge phase marker but do not log
    # an "apply phase:" line each; both runtime callers say they are runtime.
    require_text(_p(ctx, "main_runtime_nvml.cpp"), "set_last_apply_phase(phase, !runtimeTick);",
                 "a runtime fan duty write updates the phase marker without logging it")
    fan_runtime = _read(_p(ctx, "main_fan_runtime.cpp"))
    if fan_runtime.count("sizeof(detail), true) || !exact)") != 2:
        _fail("main_fan_runtime.cpp: both fan runtime ticks must mark their duty writes as runtime")
    # Verbose must be wired in both processes or the per-tick detail is gone.
    require_text(_p(ctx, "entry.cpp"), "refresh_debug_log_verbose();",
                 "the GUI decides verbose per-tick logging after enabling the log")
    require_text(_p(ctx, "main_data_paths.cpp"), "static void refresh_debug_log_verbose()",
                 "verbose per-tick logging has one decision point")
    require_text(_p(ctx, "main_service_host.cpp"), "refresh_debug_log_verbose();",
                 "the service decides verbose per-tick logging at startup")
    # Fan intent lines name Green Curve's own state accurately: a manual policy
    # seen before any readback (boot) or left by our own runtime (shutdown) is
    # not another controller's.
    require_text(_p(ctx, "main_state_sync.cpp"),
                 "!g_app.fanIsAuto && live_fan_policy_readback_complete()",
                 "the snapshot's external-fan-policy line waits for a fan policy readback")
    require_text(_p(ctx, "main_fan_runtime.cpp"),
                 "with the driver fan still manual from Green Curve's own runtime",
                 "a stopped runtime attributes the manual fan to itself")
    if "treating it as external policy" in _read(_p(ctx, "main_fan_runtime.cpp")):
        _fail("main_fan_runtime.cpp: runtime stop misattributes its own manual fan")


def check_cli_log_appends(ctx, require_text):
    # greencurve_cli_log.txt is an appending, capped log (F-01-003).  Replacing
    # it erased the earlier records at every logon, including the FAILED one.
    import re
    pattern = re.compile(r"write_text_file_atomic\(\s*cli_log_path\(\)")
    for name in sorted(os.listdir(ctx.SOURCE_DIR)):
        if not name.endswith((".cpp", ".h")):
            continue
        if pattern.search(_read(_p(ctx, name))):
            _fail(f"source/{name}: the CLI log must be appended, never replaced")
    require_text(_p(ctx, "logon_handoff_status.h"), "gc_cli_log_append_linef(",
                 "the logon handoff success record appends to the CLI log")
    require_text(_p(ctx, "entry.cpp"),
                 'gc_cli_log_append_linef("Green Curve logon handoff FAILED',
                 "the logon handoff FAILED record appends to the CLI log")
    require_text(_p(ctx, "cli_console.cpp"), "gc_cli_log_append_record(cli_log_path(),",
                 "the CLI append goes through the capped append policy")


def check_auto_apply_failure_notice(ctx, require_text):
    # A failed automatic logon/standby/driver-recovery write latched the
    # AUTOMATIC_APPLY_FAILED lockout silently.  Every such site records its
    # cause for the GUI (protocol v29), and the GUI announces the new lockout.
    lifecycle = _read(_p(ctx, "main_service_lifecycle_apply.cpp"))
    if lifecycle.count("service_lifecycle_latch_write_failure(") != 4:
        _fail("main_service_lifecycle_apply.cpp: each failed automatic write "
              "(logon, standby, driver recovery) must record its cause")
    require_text(_p(ctx, "gpu_backend_apply_failure.h"), "GC_APPLY_RECOVERY_VERIFIED_TEXT",
                 "the apply's recovery sentence is the shared constant the service matches")
    require_text(_p(ctx, "main_service_pipe_switch.cpp"), "service_auto_apply_failure_clear();",
                 "an explicit successful Apply clears the recorded failure cause")
    notice = _read(_p(ctx, "auto_apply_failure_notice_policy.h"))
    require_text(_p(ctx, "auto_apply_failure_notice_policy.h"),
                 '"Automatic profile restore is paused for safety. "',
                 "a lockout with no recorded cause uses neutral, non-attributing text")
    if "automatic apply failed" in notice.split("if (!when) {", 1)[-1].split("return true;", 1)[0]:
        _fail("auto_apply_failure_notice_policy.h: a causeless lockout must not "
              "claim an automatic apply failed")
    require_text(_p(ctx, "main_state_sync.cpp"),
                 "service_auto_apply_failure_for_snapshot_locked(snapshotLockoutReason)",
                 "the snapshot publishes the failure record only with its lockout")
    state = _p(ctx, "gui_service_state.cpp")
    adoption = "static bool gui_service_accept_response_on_main_thread("
    for earlier in ("gui_apply_ready_envelope(", "apply_logon_startup_behavior();",
                    "maybe_load_app_launch_profile_to_gui();"):
        ctx.require_order_in_operation(state, adoption, earlier,
            "gui_announce_auto_restore_lockout();",
            "the lockout notice follows generic READY adoption and startup status writes")
    ctx.forbid_text_in_operation(state, "static void gui_apply_ready_envelope(",
        "gui_announce_auto_restore_lockout(",
        "generic refresh/adoption cannot consume the notice before final status projection")
    require_text(_p(ctx, "gui_auto_restore_notice.h"), "g_guiNoticeObservedLockout",
                 "notice observation is independent of pre-window snapshot adoption")
    # The Linux daemon publishes the same record and both Linux clients show it.
    require_text(_p(ctx, "linux_daemon_snapshot_runtime.cpp"),
                 "s->autoApplyFailure = g_autoApplyFailure;",
                 "the Linux snapshot publishes the failure record with its lockout")
    runtime = _read(_p(ctx, "linux_auto_restore_runtime.h"))
    if runtime.count("auto_restore_note_automatic_failure(trigger, outcome.message") != 2:
        _fail("linux_auto_restore_runtime.h: each failed unattended write records its cause")
    require_text(_p(ctx, "linux_live_output.cpp"), "Automatic restore notice: %s",
                 "the Linux CLI status shows the automatic-apply failure notice")
    require_text(_p(ctx, "linux_tui_refresh.cpp"), "gc_auto_restore_notice_for_transition(",
                 "the Linux TUI announces a new automatic-restore lockout")
