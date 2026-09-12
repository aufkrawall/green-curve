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
