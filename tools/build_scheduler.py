"""Parallel build helpers for Green Curve.

Split out of build.py so the build script stays under its size ratchet; the
dependency is one-way (nothing here imports build.py).  The scheduler only
adds concurrency -- every compiler command is byte-for-byte the same as the
serial path, and ``--jobs 1`` keeps the legacy single-invocation behaviour.
"""

import builtins
import ctypes
import os
import sys
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import contextmanager

_PRINT_LOCK = threading.RLock()
_ORIGINAL_PRINT = builtins.print


class JobLimiter:
    """Bounds concurrent compiler/linker subprocesses across nested pools."""

    def __init__(self, jobs):
        self._slots = threading.BoundedSemaphore(max(1, jobs))

    @contextmanager
    def slot(self):
        self._slots.acquire()
        try:
            yield
        finally:
            self._slots.release()


def _total_memory_gb():
    """Best-effort physical RAM in GB; None when it cannot be read."""
    try:
        if os.name == "nt":
            class _MemoryStatusEx(ctypes.Structure):
                _fields_ = [
                    ("dwLength", ctypes.c_ulong),
                    ("dwMemoryLoad", ctypes.c_ulong),
                    ("ullTotalPhys", ctypes.c_ulonglong),
                    ("ullAvailPhys", ctypes.c_ulonglong),
                    ("ullTotalPageFile", ctypes.c_ulonglong),
                    ("ullAvailPageFile", ctypes.c_ulonglong),
                    ("ullTotalVirtual", ctypes.c_ulonglong),
                    ("ullAvailVirtual", ctypes.c_ulonglong),
                    ("ullAvailExtendedVirtual", ctypes.c_ulonglong),
                ]
            status = _MemoryStatusEx()
            status.dwLength = ctypes.sizeof(status)
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
                return status.ullTotalPhys / (1024 ** 3)
        else:
            with open("/proc/meminfo", "r", encoding="utf-8") as handle:
                for line in handle:
                    if line.startswith("MemTotal:"):
                        return int(line.split()[1]) / (1024 * 1024)
    except Exception:
        pass
    return None


def auto_job_count(per_job_memory_gb=4.0):
    """Best-effort job count bounded by cores and RAM.

    Unity/LTO compiles are memory-heavy, so a 32 GB / 16-thread machine gets
    eight jobs rather than sixteen.  Falls back to a conservative four-job cap
    when RAM cannot be read.
    """
    cores = os.cpu_count() or 1
    memory_gb = _total_memory_gb()
    if memory_gb is not None:
        return max(1, min(cores, int(memory_gb // per_job_memory_gb)))
    return max(1, min(cores, 4))


def resolve_jobs(requested):
    """Resolve --jobs: None means auto, anything else is clamped to >= 1."""
    if requested is None:
        return auto_job_count()
    return max(1, int(requested))


@contextmanager
def serialized_output():
    """Make every print atomic while concurrent workers are running.

    The lock is held only for the duration of one print call, so worker
    threads can never deadlock on it, and large compiler diagnostic blocks
    cannot interleave mid-line.
    """
    if builtins.print is locked_print:
        yield
        return
    builtins.print = locked_print
    try:
        yield
    finally:
        builtins.print = _ORIGINAL_PRINT


def locked_print(*args, **kwargs):
    with _PRINT_LOCK:
        _ORIGINAL_PRINT(*args, **kwargs)
        try:
            sys.stdout.flush()
        except Exception:
            pass


def run_parallel(tasks, jobs):
    """Run zero-arg callables with at most *jobs* in flight.

    Results are returned in task order.  The first failure cancels still
    queued tasks and is re-raised in the caller; SystemExit (build.py's normal
    failure channel) propagates unchanged.
    """
    if jobs <= 1 or len(tasks) <= 1:
        return [task() for task in tasks]
    results = [None] * len(tasks)
    with serialized_output(), ThreadPoolExecutor(
            max_workers=min(jobs, len(tasks))) as executor:
        pending = {executor.submit(task): index
                   for index, task in enumerate(tasks)}
        first_error = None
        for future in as_completed(pending):
            if future.cancelled():
                continue
            index = pending[future]
            try:
                results[index] = future.result()
            except BaseException as exc:
                if first_error is None:
                    first_error = exc
                for other in pending:
                    other.cancel()
    if first_error is not None:
        raise first_error
    return results


def compile_only_flags(flags, keep_lto=False):
    """Front-end flags for ``-c``: drop linker-only arguments.

    Unlike build.py's ``_compile_only_flags``, ``-flto`` can optionally be
    kept because the Windows x64 object-first path must emit LLVM bitcode
    objects.
    """
    out = []
    for flag in flags:
        if flag.startswith("-Wl,") or flag.startswith("-l"):
            continue
        if flag in ("-static", "-s", "-pie"):
            continue
        if flag == "-flto" and not keep_lto:
            continue
        out.append(flag)
    return out


def compile_objects(compiles, run_compiler, jobs, limiter=None):
    """Compile ``(label, cmd, cwd)`` tuples, up to *jobs* concurrently.

    *run_compiler* must be ``fn(cmd, cwd) -> returncode``.  When *limiter* is
    set every subprocess also takes a global job slot, so nested per-binary
    pools cannot oversubscribe the machine.
    """
    if not compiles:
        return
    if jobs <= 1 or len(compiles) <= 1:
        for label, cmd, cwd in compiles:
            if run_compiler(cmd, cwd) != 0:
                raise RuntimeError(f"object compilation failed: {label}")
        return

    def _one(item):
        label, cmd, cwd = item
        if limiter is not None:
            with limiter.slot():
                return label, run_compiler(cmd, cwd)
        return label, run_compiler(cmd, cwd)

    first_failure = None
    with ThreadPoolExecutor(max_workers=min(jobs, len(compiles))) as executor:
        pending = {executor.submit(_one, item): item[0] for item in compiles}
        for future in as_completed(pending):
            if future.cancelled():
                continue
            label, returncode = future.result()
            if returncode != 0 and first_failure is None:
                first_failure = label
                for other in pending:
                    other.cancel()
    if first_failure is not None:
        raise RuntimeError(f"object compilation failed: {first_failure}")


def run_self_tests():
    """Deterministic self-tests run by ``python build.py --test``."""
    failures = []

    flags = ["-Oz", "-flto", "-Wl,--icf=safe", "-static", "-s", "-pie",
             "-luser32", "-target", "x86_64-linux-gnu"]
    stripped = compile_only_flags(flags)
    if stripped != ["-Oz", "-target", "x86_64-linux-gnu"]:
        failures.append("compile_only_flags must strip linker-only flags and -flto")
    kept = compile_only_flags(flags, keep_lto=True)
    if kept != ["-Oz", "-flto", "-target", "x86_64-linux-gnu"]:
        failures.append("compile_only_flags(keep_lto=True) must keep -flto")

    if auto_job_count() < 1:
        failures.append("auto_job_count must return at least one job")

    ordered = run_parallel([lambda value=value: value for value in range(4)], 4)
    if ordered != [0, 1, 2, 3]:
        failures.append("run_parallel must preserve task order")

    def _boom():
        raise RuntimeError("expected scheduler failure")

    try:
        run_parallel([_boom, lambda: 1], 2)
    except RuntimeError:
        pass
    else:
        failures.append("run_parallel must propagate task failures")

    calls = []

    def _ok_compiler(cmd, cwd):
        calls.append((cmd, cwd))
        return 0

    compile_objects([("a", ["cc", "a"], "dir"), ("b", ["cc", "b"], "dir")],
                    _ok_compiler, 2, JobLimiter(2))
    if len(calls) != 2:
        failures.append("compile_objects must run every compile")

    def _failing_compiler(cmd, _cwd):
        return 1 if cmd[-1] == "bad" else 0

    try:
        compile_objects([("bad", ["cc", "bad"], "dir")],
                        _failing_compiler, 2, JobLimiter(2))
    except RuntimeError:
        pass
    else:
        failures.append("compile_objects must fail on a non-zero returncode")

    original_print = builtins.print
    with serialized_output():
        pass
    if builtins.print is not original_print:
        failures.append("serialized_output must restore builtins.print")

    if failures:
        for message in failures:
            print(f"Build-scheduler regression FAILED: {message}")
        sys.exit(1)
