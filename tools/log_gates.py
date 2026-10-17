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
            "debug_log() must not call %s on the producer thread"
            % call.rstrip("("))
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
