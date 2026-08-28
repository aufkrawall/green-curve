"""Zig cache integrity and link serialization for the Green Curve build.

Root cause of the 2026-08-28 native-Windows build failure: the release matrix
runs up to four Zig-driven links concurrently (windows-arm64 GUI/service,
linux-x64, linux-arm64) sharing one ZIG_GLOBAL_CACHE_DIR.  The pinned Zig
0.13.0 cache machinery can leave that shared cache poisoned on Windows --
upstream acknowledges a residual Windows-specific cache race in the
ziglang/zig#14815 thread, and cache-race regressions as late as 0.14.0
(#23110): a cache manifest survives while its artifact directory
(``o/<hash>/compiler_rt.lib`` or ``libcompiler_rt.a``) is absent.  Zig never
self-heals that state -- a manifest "hit" whose file is missing is a hard
error -- so every later link of that target fails with::

    ld.lld: error: cannot open .../zig-global-cache/o/<hash>/libcompiler_rt.a
    lld-link: error: could not open '.../o/<hash>/compiler_rt.lib': ...

The manifest file names under ``h/`` are not derivable from the artifact
directory name (verified: 0/10 name pairs match in a real cache), so a
poisoned manifest cannot be located surgically from outside Zig.  Two
defenses instead:

1. Every Zig *link* invocation holds a cross-process lock, so only one Zig
   process ever mutates the shared global cache at a time.  Plain ``-c``
   compiles never write the global cache (verified empirically on 0.13.0),
   so compile parallelism is unaffected.
2. When a link fails on a missing artifact inside a Zig cache root, the
   affected root is removed and the link is retried exactly once.  This is
   the upstream-documented remedy (a damaged cache cannot be partially
   pruned), applied only to a precisely parsed, verified-missing state, and
   logged loudly.

Compiles cache their objects in ``ZIG_LOCAL_CACHE_DIR``; a poisoned *local*
cache cannot be repaired automatically while sibling compiles are running,
so :func:`diagnose_missing_cache_artifacts` turns that failure into an
explicit, actionable error instead of a cryptic one.
"""
import ctypes
import os
import re
import shutil
import subprocess
import sys
import tempfile

if sys.platform == "win32":
    _KERNEL32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _KERNEL32.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint, ctypes.c_uint,
                                      ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint,
                                      ctypes.c_void_p]
    _KERNEL32.CreateFileW.restype = ctypes.c_void_p
    _KERNEL32.LockFileEx.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint,
                                     ctypes.c_uint, ctypes.c_uint,
                                     ctypes.c_void_p]
    _KERNEL32.LockFileEx.restype = ctypes.c_int
    _KERNEL32.UnlockFileEx.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint,
                                       ctypes.c_uint, ctypes.c_void_p]
    _KERNEL32.UnlockFileEx.restype = ctypes.c_int
    _KERNEL32.CloseHandle.argtypes = [ctypes.c_void_p]
    _KERNEL32.CloseHandle.restype = ctypes.c_int

    class _OVERLAPPED(ctypes.Structure):
        class _OFFSET(ctypes.Union):
            _fields_ = [("Offset", ctypes.c_uint),
                        ("OffsetHigh", ctypes.c_uint),
                        ("Pointer", ctypes.c_void_p)]
        _anonymous_ = ["offset"]
        _fields_ = [("Internal", ctypes.c_void_p),
                    ("InternalHigh", ctypes.c_void_p),
                    ("offset", _OFFSET),
                    ("hEvent", ctypes.c_void_p)]

    _GENERIC_READWRITE = 0xC0000000
    _FILE_SHARE_READWRITE = 0x3
    _OPEN_ALWAYS = 4
    _FILE_ATTRIBUTE_NORMAL = 0x80
    _INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
    _LOCKFILE_EXCLUSIVE_LOCK = 0x2
    _LOCKFILE_FAIL_IMMEDIATELY = 0x1
else:
    import fcntl

_OPEN_SIGNALS = ("cannot open", "could not open", "unable to open")
_ARTIFACT_RE = re.compile(r"[\\/]o[\\/]([0-9a-f]{32})(?:[\\/](.+))?$")
_ZIG_CACHE_NAME_RE = re.compile(r"zig-[a-z0-9-]+-cache")


def _root_variants(root):
    """Separator variants of a cache root, because Zig prints paths with
    forward slashes even on Windows while build.py's constants use
    ``os.sep``."""
    return (root, root.replace("\\", "/"), root.replace("/", "\\"))


def _artifact_from_line(line, root):
    """Parse one diagnostic line against one cache root.

    Returns ``(digest, filename_or_None)`` when the named path lies inside
    ``<root>/o/<32 hex digest>/[filename]``.  The path may be quoted (lld-link
    style) or bare (ld.lld style) and always ends before ``': '`` or a quote,
    because Windows paths cannot contain either; the drive colon sits inside
    the matched root already.
    """
    for variant in _root_variants(root):
        idx = line.find(variant)
        if idx < 0:
            continue
        rest = line[idx + len(variant):]
        end = len(rest)
        for stop in ("'", '"', ": "):
            pos = rest.find(stop)
            if pos != -1 and pos < end:
                end = pos
        match = _ARTIFACT_RE.fullmatch(rest[:end].strip())
        if match:
            return (match.group(1), match.group(2) or None)
        return None
    return None


def missing_cache_artifacts(text, cache_roots):
    """Extract every missing-artifact report that points inside a Zig cache.

    Returns a list of ``(root, digest, filename_or_None)``.  Diagnostics about
    files outside the cache roots (our objects, system libraries) are ignored.
    """
    found = []
    seen = set()
    for line in text.splitlines():
        if not any(signal in line for signal in _OPEN_SIGNALS):
            continue
        for root in cache_roots:
            hit = _artifact_from_line(line, root)
            if hit and (root, hit[0], hit[1]) not in seen:
                seen.add((root, hit[0], hit[1]))
                found.append((root, hit[0], hit[1]))
    return found


def artifact_is_missing(root, digest, filename):
    """Verify the reported artifact really is absent from the cache.

    A link failure is only repaired when the state on disk matches the
    diagnostic; anything else (a locked file, a bad flag) must fail loudly
    instead of triggering a cache wipe.
    """
    artifact_dir = os.path.join(root, "o", digest)
    if filename is None:
        return not os.path.isdir(artifact_dir)
    return not os.path.isfile(os.path.join(artifact_dir, filename))


def remove_cache_root(root, log=None):
    """Remove one Zig cache root and recreate it empty.

    This is the upstream-documented remedy for a poisoned cache: partial
    pruning is unsupported, and the poisoned manifest cannot be located by
    name from outside Zig, so the whole root must go.  The guard refuses
    paths that do not name a Zig cache directory.
    """
    say = log or print
    base = os.path.basename(root.rstrip("\\/"))
    if not _ZIG_CACHE_NAME_RE.fullmatch(base):
        raise RuntimeError(f"refusing to remove non-Zig-cache path: {root}")
    if os.path.isdir(root):
        shutil.rmtree(root)
    os.makedirs(root, exist_ok=True)
    say(f"Removed and recreated Zig cache root: {root}")


def _lockfile_dir(cache_roots):
    """Directory holding the lock file: the caches' common parent.

    It must sit OUTSIDE every wipeable cache root, otherwise a repair could
    delete the lock file while other acquirers hold handles to it and two of
    them could briefly both believe they hold the lock.
    """
    if not cache_roots:
        return None
    return os.path.dirname(os.path.abspath(cache_roots[0]))


class ZigLinkLock:
    """Cross-process mutual exclusion around Zig link invocations.

    One exclusive byte-range lock on a lock file: ``LockFileEx`` on Windows,
    ``flock`` on POSIX.  The lock is per handle, so it excludes other threads
    of the same process as well as other build invocations, and the OS
    releases it when a process dies.  A blocking acquire waits inside the
    kernel; there is no polling and no timeout.
    """

    _LOCKFILE_NAME = "zig-link-cache.lock"

    def __init__(self, lockfile_dir=None):
        self._lockfile_dir = lockfile_dir
        self._handle = None

    def _lockfile_path(self):
        directory = self._lockfile_dir or tempfile.gettempdir()
        os.makedirs(directory, exist_ok=True)
        return os.path.join(directory, self._LOCKFILE_NAME)

    def acquire(self, blocking=True):
        if sys.platform == "win32":
            handle = _KERNEL32.CreateFileW(
                self._lockfile_path(), _GENERIC_READWRITE, _FILE_SHARE_READWRITE,
                None, _OPEN_ALWAYS, _FILE_ATTRIBUTE_NORMAL, None)
            if handle in (None, _INVALID_HANDLE_VALUE):
                raise OSError(ctypes.get_last_error(),
                              "CreateFileW failed for the Zig link lock file")
            overlapped = _OVERLAPPED()
            flags = _LOCKFILE_EXCLUSIVE_LOCK
            if not blocking:
                flags |= _LOCKFILE_FAIL_IMMEDIATELY
            if not _KERNEL32.LockFileEx(handle, flags, 0, 1, 0,
                                        ctypes.byref(overlapped)):
                _KERNEL32.CloseHandle(handle)
                return False
            self._handle = handle
            return True
        fd = os.open(self._lockfile_path(), os.O_CREAT | os.O_RDWR, 0o644)
        flags = fcntl.LOCK_EX if blocking else fcntl.LOCK_EX | fcntl.LOCK_NB
        try:
            fcntl.flock(fd, flags)
        except OSError:
            os.close(fd)
            return False
        self._handle = fd
        return True

    def release(self):
        handle, self._handle = self._handle, None
        if handle is None:
            return
        if sys.platform == "win32":
            overlapped = _OVERLAPPED()
            _KERNEL32.UnlockFileEx(handle, 0, 1, 0, ctypes.byref(overlapped))
            _KERNEL32.CloseHandle(handle)
        else:
            fcntl.flock(handle, fcntl.LOCK_UN)
            os.close(handle)

    def __enter__(self):
        if not self.acquire(blocking=True):
            raise RuntimeError("could not acquire the Zig link lock")
        return self

    def __exit__(self, *exc_info):
        self.release()
        return False


def _log_text(log, text):
    (log or print)(text if text.endswith("\n") else text + "\n")


def run_zig_link(cmd, cwd, cache_roots, log=None, audit=None, runner=None):
    """Run one Zig link serialized against the shared cache roots.

    The link runs under :class:`ZigLinkLock`, so two Zig links can never
    mutate the shared global cache concurrently.  If the link fails with the
    poisoned-cache signature (a diagnostic naming an artifact inside a cache
    root whose file is verifiably absent), the affected root is removed and
    the link is retried exactly once -- loudly, and still under the lock.

    ``audit(combined_text, returncode) -> int`` is the caller's output policy
    (build.py's duplicate-symbol gate), applied to the final attempt only.
    ``runner`` replaces ``subprocess.run`` for deterministic self-tests.
    """
    run = runner or subprocess.run
    lock = ZigLinkLock(lockfile_dir=_lockfile_dir(cache_roots))
    with lock:
        result = run(cmd, cwd=cwd, text=True, capture_output=True)
        combined = (result.stdout or "") + (result.stderr or "")
        if combined:
            _log_text(log, combined)
        missing = [entry for entry in missing_cache_artifacts(combined, cache_roots)
                   if artifact_is_missing(entry[0], entry[1], entry[2])]
        if result.returncode != 0 and missing:
            for root, digest, filename in missing:
                say = log or print
                say(f"ERROR: Zig cache is poisoned: {root} lost o/{digest}/"
                    f"{filename or '<dir>'} while its cache manifest survived.")
                say("       Zig 0.13 never rebuilds a manifest hit whose "
                    "artifact is missing (upstream zig #14815/#23110 "
                    "cache-race class), so the affected cache root is "
                    "removed and this link is retried once.")
            for root in sorted({entry[0] for entry in missing}):
                remove_cache_root(root, log=log)
            say = log or print
            say("Retrying the failed link once with a fresh cache root.")
            result = run(cmd, cwd=cwd, text=True, capture_output=True)
            combined = (result.stdout or "") + (result.stderr or "")
            if combined:
                _log_text(log, combined)
    if audit is not None:
        return audit(combined, result.returncode)
    return result.returncode


def diagnose_missing_cache_artifacts(text, cache_roots, log=None):
    """Loud, actionable diagnosis for a failed compile naming a cache artifact.

    Compiles cache their objects in ``ZIG_LOCAL_CACHE_DIR``; a poisoned local
    cache cannot be wiped automatically while sibling compiles are writing
    it, so unlike the link path this names the exact remedy instead.
    """
    for root, digest, filename in missing_cache_artifacts(text, cache_roots):
        say = log or print
        say(f"ERROR: a failed compiler step named a missing Zig cache "
            f"artifact: {root}/o/{digest}/{filename or '<dir>'}")
        say("       This is the poisoned zig-cache state (upstream zig "
            "#14815/#23110 cache-race class).  Delete the cache root and "
            f"rerun the build: {root}")


def _self_test_parser(failures):
    with tempfile.TemporaryDirectory(prefix="zig-cache-parse-") as temp:
        root = os.path.join(temp, "zig-global-cache")
        digest = "06230a9210c1be855fa2a328196beacf"
        digest2 = "186480f965eb2619bf5a6d9ac654fcec"
        digest3 = "1645de82c8fcbcd24c0abbf4a2aeed63"
        # lld-link quoted form with the mixed separators Zig prints on Windows.
        quoted = (f"lld-link: error: could not open "
                  f"'{root.replace(os.sep, '/')}\\o\\{digest}\\compiler_rt.lib': "
                  "No such file or directory")
        # ld.lld unquoted form.
        unquoted = (f"ld.lld: error: cannot open {root}\\o\\{digest2}"
                    "\\libcompiler_rt.a: No such file or directory")
        # Directory-only form.
        dir_only = f"error: unable to open '{root}/o/{digest3}': FileNotFound"
        found = missing_cache_artifacts(quoted + "\n" + unquoted + "\n" + dir_only,
                                        [root])
        expected = [(root, digest, "compiler_rt.lib"),
                    (root, digest2, "libcompiler_rt.a"),
                    (root, digest3, None)]
        if found != expected:
            failures.append(f"parser must report exactly {expected}, got {found}")
        # Paths outside the cache roots, or not cache artifacts, are ignored.
        if missing_cache_artifacts(
                "ld.lld: error: cannot open C:\\somewhere\\main.o: "
                "No such file or directory", [root]):
            failures.append("parser must ignore paths outside the cache root")
        if missing_cache_artifacts(
                f"ld.lld: error: cannot open {root}\\obj\\abc.o: "
                "No such file or directory", [root]):
            failures.append("parser must ignore non-o/ entries inside the root")
        if missing_cache_artifacts(
                f"ld.lld: error: cannot open {root}\\o\\short\\x.a: "
                "No such file or directory", [root]):
            failures.append("parser must reject non-32-hex artifact digests")
        # artifact_is_missing must agree with the real file system.
        artifact_dir = os.path.join(root, "o", digest)
        os.makedirs(artifact_dir)
        with open(os.path.join(artifact_dir, "compiler_rt.lib"), "wb"):
            pass
        if artifact_is_missing(root, digest, "compiler_rt.lib"):
            failures.append("a present artifact must not count as missing")
        if not artifact_is_missing(root, digest2, "compiler_rt.lib"):
            failures.append("a missing artifact file must count as missing")
        if not artifact_is_missing(root, digest3, None):
            failures.append("a missing artifact directory must count as missing")
        if artifact_is_missing(root, digest, None):
            failures.append("a present artifact directory must not count as missing")
        # remove_cache_root: removes + recreates, and refuses foreign paths.
        remove_cache_root(root, log=lambda text: None)
        if os.listdir(root):
            failures.append("remove_cache_root must recreate the root empty")
        try:
            remove_cache_root(os.path.join(temp, "not-a-zig-cache"),
                              log=lambda text: None)
            failures.append("remove_cache_root must refuse non-zig-cache paths")
        except RuntimeError:
            pass


def _self_test_lock(failures):
    with tempfile.TemporaryDirectory(prefix="zig-cache-lock-") as temp:
        first = ZigLinkLock(lockfile_dir=temp)
        second = ZigLinkLock(lockfile_dir=temp)
        if not first.acquire(blocking=False):
            failures.append("the first lock acquisition must succeed")
        if second.acquire(blocking=False):
            failures.append("a second lock acquisition must fail while held")
            second.release()
        first.release()
        if not second.acquire(blocking=False):
            failures.append("the lock must be acquirable after release")
        second.release()
        with first:
            if second.acquire(blocking=False):
                failures.append("the context manager must hold the lock")
                second.release()
        if not second.acquire(blocking=False):
            failures.append("exiting the context manager must release the lock")
        second.release()


class _FakeResult:
    def __init__(self, returncode, stdout="", stderr=""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def _self_test_repair(failures):
    with tempfile.TemporaryDirectory(prefix="zig-cache-repair-") as temp:
        root = os.path.join(temp, "zig-global-cache")
        os.makedirs(root)
        digest = "0" * 32
        failure_line = (f"lld-link: error: could not open '{root}\\o\\{digest}"
                        "\\compiler_rt.lib': No such file or directory")
        calls = []

        def runner(cmd, cwd=None, text=None, capture_output=None):
            calls.append(cmd)
            if len(calls) == 1:
                return _FakeResult(1, stdout=failure_line)
            return _FakeResult(0)

        messages = []
        rc = run_zig_link(["zig", "c++"], cwd=temp, cache_roots=[root],
                          log=messages.append, runner=runner)
        if rc != 0:
            failures.append("a repaired link must report success")
        if len(calls) != 2:
            failures.append(f"the failed link must be retried exactly once, "
                            f"got {len(calls)} attempts")
        if os.path.exists(os.path.join(root, "o", digest)):
            failures.append("the poisoned artifact dir must not survive the repair")
        if not os.path.isdir(root):
            failures.append("the cache root must be recreated by the repair")
        if not any("poisoned" in message for message in messages):
            failures.append("the repair must log the poisoning loudly")
        # A failure that does not match the poisoned signature must fail
        # loudly without touching the cache.
        other_root = os.path.join(temp, "zig-local-cache")
        os.makedirs(other_root)

        def stubborn_runner(cmd, cwd=None, text=None, capture_output=None):
            return _FakeResult(1, stderr="ld.lld: error: undefined symbol: foo")

        messages = []
        rc = run_zig_link(["zig", "c++"], cwd=temp, cache_roots=[other_root],
                          log=messages.append, runner=stubborn_runner)
        if rc != 1:
            failures.append("a non-cache failure must propagate its code")
        if not os.path.isdir(other_root):
            failures.append("a non-cache failure must not wipe the cache root")
        # The caller's audit sees the final attempt's output only.
        def audit(text, returncode):
            return 5 if "duplicate symbol" in text else returncode

        rc = run_zig_link(["zig", "c++"], cwd=temp, cache_roots=[other_root],
                          log=messages.append, runner=stubborn_runner,
                          audit=audit)
        if rc != 1:
            failures.append("the audit must pass a clean failure through")
        rc = run_zig_link(["zig", "c++"], cwd=temp, cache_roots=[root],
                          log=messages.append,
                          runner=lambda cmd, cwd=None, text=None,
                          capture_output=None: _FakeResult(
                              0, stderr="duplicate symbol: __guard_check_icall_fptr"),
                          audit=audit)
        if rc != 5:
            failures.append("the audit must see the final attempt's output")
        # A manifest hit whose file still exists must never trigger a wipe.
        kept = os.path.join(temp, "zig-keep-cache")
        os.makedirs(os.path.join(kept, "o", digest))
        with open(os.path.join(kept, "o", digest, "libcompiler_rt.a"), "wb"):
            pass
        keep_line = (f"ld.lld: error: cannot open {kept}\\o\\{digest}"
                     "\\libcompiler_rt.a: No such file or directory")

        def keep_runner(cmd, cwd=None, text=None, capture_output=None):
            return _FakeResult(1, stdout=keep_line)

        run_zig_link(["zig", "c++"], cwd=temp, cache_roots=[kept],
                     log=messages.append, runner=keep_runner)
        if not os.path.isdir(os.path.join(kept, "o", digest)):
            failures.append("a cache root with the artifact present must not "
                            "be wiped on a stale diagnostic")


def run_self_tests():
    """Deterministic regression tests for the parser, the lock, and repair."""
    failures = []
    _self_test_parser(failures)
    _self_test_lock(failures)
    _self_test_repair(failures)
    if failures:
        for failure in failures:
            print(f"zig_cache self-test FAILED: {failure}")
        sys.exit(1)
    print("  zig_cache self-tests passed (parser, lock, repair)")
