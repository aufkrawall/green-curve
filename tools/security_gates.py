"""Fuzzing and CET-instrumentation gates for Green Curve.

Split out of build.py so the build script stays under its size ratchet; build.py
owns the CLI and passes its paths/config in through `ctx`.  Nothing here imports
build.py, so the dependency runs one way only.

`ctx` is any object exposing: SCRIPT_DIR, SOURCE_DIR, LLVM_MINGW_DIR,
LLVM_MINGW_CLANG, ZIG_EXE, APP_VERSION, APP_BUILD_NUMBER,
prepare_work_subdir(name), cleanup_work_subdir(path).
"""
import bisect
import glob
import io
import itertools
import os
import re
import shutil
import struct
import subprocess
import sys
import tarfile

import release_manifest  # same one-way dependency: it never imports build.py
import static_analysis  # ditto; owns the clang-tidy ratchet's own self-tests
import build_scheduler
import update_signing  # ditto; owns the update signer's RFC 6979 vectors
import toolchain  # ditto; owns pinned-toolchain verification
import zig_cache  # ditto; owns the cross-process Zig link lock + cache repair
import arch_package  # ditto; builds pacman-installable Arch Linux packages

# Fuzz targets built from tests/fuzz_main.cpp.  The key is the GC_FUZZ_TARGET
# macro suffix and the corpus directory name; the value is the macro's numeric
# value.  check_fuzz_harness_in_sync() enforces that this table and the
# #defines in tests/fuzz_main.cpp stay in agreement.
FUZZ_TARGETS = {
    "service_request": 1,
    "vf_snapshot": 2,
    "task_xml": 3,
    "config_strings": 4,
    "wire_prefix": 5,
    "update_manifest": 6,
}

# Targets that build on a native Linux host.  The one omission is not an
# oversight and must not be "fixed" by stubbing: task_xml #includes
# main_startup_task_definition.cpp, a Win32 shard.
#
# config_strings joined this set on 2026-07-28, when parse_fan_value,
# parse_cli_point_arg_w and config_section_header_matches_ascii moved out of the
# Win32-only config_utils.cpp into config_text_utils.cpp.  That same move
# deleted linux_port.cpp's private duplicate of parse_fan_value, so this target
# now fuzzes the code the Linux daemon actually runs.
#
# update_manifest is header-only pure policy with no Win32 dependency at all,
# so it builds and runs on both hosts.
FUZZ_LINUX_TARGETS = frozenset({"service_request", "vf_snapshot", "wire_prefix",
                                "config_strings", "update_manifest"})

# Extra translation units a Linux fuzz target needs, keyed by target name.  Most
# resolve entirely inside the harness and its headers; an empty/absent entry
# means "link nothing extra", so a target that grows a dependency on a Win32
# shard fails at link rather than being papered over.
#
# The Win32 side links one shared FUZZ_WIN32_SOURCES list, which is why a
# Windows host never noticed that service_request needed fan_curve.cpp: the
# harness #includes gpu_core.h unconditionally, and that header's static-inline
# validate_service_request_for_ipc() calls the out-of-line
# fan_curve_normalize_for_ipc() (which itself needs set_message() from
# config_text_utils.cpp).  Keeping these lists per-target and minimal is
# deliberate -- it is what makes an accidental Win32 dependency fail loudly --
# so the answer is an accurate entry, plus check_fuzz_linux_link_lines() below
# so every entry is proven on every host instead of only in Linux CI.
FUZZ_LINUX_EXTRA_SOURCES = {
    "service_request": ("fan_curve.cpp", "config_text_utils.cpp"),
    "config_strings": ("config_text_utils.cpp", "app_shared.cpp",
                       "fan_curve.cpp", "platform_posix.cpp"),
}

# Native-Linux regression fixtures under tests/ (stem -> human label), and the
# extra translation units each one must LINK beyond its own .cpp.  The fixtures
# #include real product shards (linux_daemon_transport.cpp,
# linux_crash_report.cpp) and, through gpu_core.h, the IPC trust boundary --
# whose static-inline validate_desired_settings_for_ipc() calls the OUT-OF-LINE
# fan_curve_normalize_for_ipc() (source/fan_curve.cpp), which in turn calls
# set_message() (source/config_text_utils.cpp).  gpu_core.h documents that every
# consumer of that boundary links fan_curve.cpp; this table is how the fixtures
# hold up their end.  An empty tuple means "link nothing extra", so a fixture
# that grows a dependency on a Win32 shard fails at link rather than being
# papered over -- the same contract as FUZZ_LINUX_EXTRA_SOURCES.
LINUX_FIXTURES = (
    ("linux_transport_regression", "socket transport"),
    ("linux_crash_report_regression", "crash report"),
)
LINUX_FIXTURE_EXTRA_SOURCES = {
    "linux_transport_regression": ("fan_curve.cpp", "config_text_utils.cpp"),
    "linux_crash_report_regression": (),
}

# Translation units every Win32 fuzz target links.  Hoisted out of the command
# builder so check_fuzz_target_wiring() can assert against it: when the shared
# text helpers moved into config_text_utils.cpp, LINUX_SOURCE_FILES,
# WINDOWS_SOURCE_FILES and FUZZ_LINUX_EXTRA_SOURCES were all updated and this
# list was not, so every Win32 fuzz target failed to link on set_message,
# trim_ascii and parse_int_strict -- and stayed that way, because --fuzz was
# being verified on a Linux host.
FUZZ_WIN32_SOURCES = (
    "fan_curve.cpp",
    "config_utils.cpp",
    "config_text_utils.cpp",
    "app_shared.cpp",
    "service_acl.cpp",
    "vf_backends.cpp",
    "platform_win32.cpp",
)

# Coverage instrumentation for libFuzzer.  llvm-mingw's clang driver rejects the
# -fsanitize=fuzzer convenience flag for x86_64-w64-windows-gnu, but the gate is
# only on that flag: these instrumentation options and the runtime archive both
# work on this target, so they are passed directly.  MSYS2's clang64 targets the
# same triple and hits the identical driver gate, so switching to it would not
# help.  See llm-wiki/testing.md.
FUZZ_COVERAGE_FLAGS = [
    "-fsanitize-coverage=inline-8bit-counters,trace-cmp,trace-div,trace-gep,pc-table",
]

DEFAULT_FUZZ_RUNS = 20000


def add_arguments(parser):
    """Register the gate CLI flags, kept next to the code that implements them."""
    parser.add_argument(
        "--fuzz", action="store_true",
        help="Build and briefly run the libFuzzer targets over the "
             "untrusted-input boundaries (ASan + UBSan)")
    parser.add_argument(
        "--fuzz-target", choices=tuple(sorted(FUZZ_TARGETS)), default=None,
        help="Restrict --fuzz to a single target (default: all)")
    parser.add_argument(
        "--fuzz-runs", type=int, default=None,
        help=f"Iterations per fuzz target (default: {DEFAULT_FUZZ_RUNS} for the "
             f"bounded gate); pass a large value for a real fuzzing session")
    parser.add_argument(
        "--check-cet", nargs="?", const="", default=None, metavar="EXE",
        help="Verify -fcf-protection=full is effective on our own code in a "
             "built PE (default: dist/windows-x64/greencurve/greencurve.exe)")


def fuzz_corpus_dir(ctx):
    return os.path.join(ctx.SCRIPT_DIR, "tests", "fuzz-corpus")


def fuzzer_runtime_archive(ctx):
    """Path to libclang_rt.fuzzer for the host toolchain, or None.

    Windows only.  The Linux host uses `-fsanitize=fuzzer`, which clang accepts
    for x86_64-linux-gnu and which resolves its own runtime; the flag is
    rejected only for the x86_64-w64-windows-gnu triple.
    """
    if sys.platform != "win32":
        return None
    pattern = os.path.join(ctx.LLVM_MINGW_DIR, "lib", "clang", "*", "lib",
                           "windows", "libclang_rt.fuzzer-x86_64.a")
    matches = sorted(glob.glob(pattern))
    return matches[-1] if matches else None


def host_fuzz_compiler():
    """Path to a native clang++ able to build the Linux fuzz/sanitizer targets.

    The bundled Zig is a complete compiler but ships no ASan or libFuzzer
    runtime (it carries only tsan), so a Zig-built target fails at link with
    undefined __asan_register_elf_globals.  A host clang is required, and its
    absence is reported rather than silently skipped.
    """
    return shutil.which("clang++")


def sanitizer_build_requested(extra_flags):
    """True when the flags ask for ASan.  Pure, so it is directly testable."""
    return any(flag.startswith("-fsanitize") and "address" in flag
               for flag in (extra_flags or []))


def fuzz_targets_for_host(platform):
    """The {name: macro} subset buildable on `platform`.  Pure and testable.

    Windows builds everything; Linux builds FUZZ_LINUX_TARGETS; any other host
    builds nothing, and run_fuzz_targets() reports that rather than claiming a
    pass.
    """
    if platform == "win32":
        return dict(FUZZ_TARGETS)
    if platform.startswith("linux"):
        return {name: value for name, value in FUZZ_TARGETS.items()
                if name in FUZZ_LINUX_TARGETS}
    return {}


def posix_test_compiler(ctx, extra_flags):
    """Argv prefix for compiling the regression fixtures on a POSIX host.

    Plain builds use the pinned Zig.  ASan builds cannot: Zig has no ASan
    runtime, so `--test --asan` died at link on undefined
    __asan_register_elf_globals and had therefore never actually run on Linux
    before 2026-07-28.  Those fall back to a host clang, and a missing one is a
    hard error — a gate that cannot run must say so, not report success.
    """
    if not sanitizer_build_requested(extra_flags):
        return [ctx.ZIG_EXE, "c++"]
    clang = host_fuzz_compiler()
    if not clang:
        print("ERROR: --asan needs a host clang++ with libclang_rt.asan; the "
              "bundled Zig ships no ASan runtime.")
        print("       Install clang (Arch: pacman -S clang) or drop --asan.")
        sys.exit(1)
    print(f"ASan build: using host {clang} (Zig ships no ASan runtime)")
    return [clang]


def _linux_fixture_link_units(ctx, stem, label):
    """Resolve one fixture's own .cpp plus its declared extra link units.

    A fixture missing from LINUX_FIXTURE_EXTRA_SOURCES is a hard error rather
    than an implicit empty list: the whole point of the table is that a
    fixture's link line is declared, so a new one cannot silently inherit
    "links nothing" and then fail only on the Linux CI host.
    """
    fixture_source = os.path.join(ctx.SCRIPT_DIR, "tests", f"{stem}.cpp")
    if not os.path.exists(fixture_source):
        print(f"Linux {label} fixture source is missing: {fixture_source}")
        sys.exit(1)
    if stem not in LINUX_FIXTURE_EXTRA_SOURCES:
        print(f"Linux {label} fixture has no LINUX_FIXTURE_EXTRA_SOURCES entry; "
              f"add one (an empty tuple means 'link nothing extra') so its link "
              f"line stays declared, not implied")
        sys.exit(1)
    extra_sources = [os.path.join(ctx.SOURCE_DIR, name)
                     for name in LINUX_FIXTURE_EXTRA_SOURCES[stem]]
    for extra in extra_sources:
        if not os.path.exists(extra):
            print(f"Linux {label} fixture names a missing translation unit: "
                  f"{extra}")
            sys.exit(1)
    return fixture_source, extra_sources


def run_linux_fixtures(ctx, tmp_dir, extra_flags, test_env):
    """Build every native-Linux fixture, and run them on a Linux host.

    Non-Linux hosts CROSS-LINK the fixtures to a real x86_64-linux-gnu ELF that
    is never executed here.  They used to only compile them to an object, and a
    compile can never see an undefined symbol: that is exactly how the
    2026-09 gpu_core.h change -- a static-inline IPC validator growing a call to
    the out-of-line fan_curve_normalize_for_ipc() -- passed a Windows host and
    broke the Linux CI job at `ld.lld: undefined symbol`.  Linking on every host
    moves that failure back to the machine that introduced it.
    """
    for stem, label in LINUX_FIXTURES:
        fixture_source, extra_sources = _linux_fixture_link_units(ctx, stem, label)
        linked = ", ".join([os.path.basename(fixture_source)]
                           + [os.path.basename(path) for path in extra_sources])
        fixture_exe = os.path.join(tmp_dir, stem)
        native = sys.platform.startswith("linux")
        if native:
            cmd = [*posix_test_compiler(ctx, extra_flags)]
        else:
            cmd = [ctx.ZIG_EXE, "c++", "-target", ctx.LINUX_TARGET,
                   "-Wall", "-Wextra", "-Wno-unused-function",
                   "-Wno-unused-parameter", "-Werror"]
        cmd.extend([
            "-std=c++17", "-DNDEBUG",
            f'-DAPP_VERSION="{ctx.APP_VERSION}"',
            f"-DAPP_BUILD_NUMBER={ctx.APP_BUILD_NUMBER}",
            "-fno-exceptions", "-fno-rtti",
            f"-I{ctx.SOURCE_DIR}",
            "-o", fixture_exe,
            fixture_source,
            *extra_sources,
        ])
        if native and extra_flags:
            cmd.extend(extra_flags)
        verb = "Compiling" if native else f"Cross-linking ({ctx.LINUX_TARGET})"
        print(f"{verb} Linux {label} regression tests [{linked}]")
        # _run_zig_link serializes against the shared Zig cache, repairs a
        # poisoned one, and audits the output for unexpected duplicate symbols.
        returncode = ctx._run_zig_link(cmd)
        if returncode != 0:
            print(f"Linux {label} test link FAILED (see the diagnostic above; "
                  f"an undefined symbol means a translation unit is missing "
                  f"from LINUX_FIXTURE_EXTRA_SOURCES['{stem}'])")
            sys.exit(returncode)
        if not native:
            continue
        print(f"Running Linux {label} regression tests")
        result = subprocess.run([fixture_exe], cwd=ctx.SCRIPT_DIR, env=test_env)
        if result.returncode != 0:
            print(f"Linux {label} regression FAILED ({result.returncode})")
            sys.exit(result.returncode)


def check_fuzz_linux_link_lines(ctx, tmp_dir):
    """Cross-link every Linux fuzz target on a non-Linux host (link check only).

    `--fuzz` builds the Linux targets only on a Linux host, because libFuzzer
    and the ASan runtime come from a host clang that the bundled Zig does not
    ship.  That left FUZZ_LINUX_EXTRA_SOURCES -- a hand-maintained per-target
    link line -- unproven anywhere but the Linux CI job, and on 2026-09-15 it
    cost two consecutive red runs on `main`: `service_request` reaches
    gpu_core.h's IPC validator, which grew a call to the out-of-line
    fan_curve_normalize_for_ipc(), and nothing on the Windows host could see it.

    This links each target for x86_64-linux-gnu with its DECLARED extra sources
    plus tests/fuzz_link_check_main.cpp (libFuzzer's main() is absent without
    -fsanitize=fuzzer).  It deliberately drops the sanitizer and coverage flags,
    so it is a link-line check and NOT a substitute for `--fuzz`: it proves the
    declared translation units resolve every symbol the target emits, nothing
    more.  On a Linux host it is skipped, because the real `--fuzz` run already
    links exactly these lines with the real instrumentation.
    """
    if sys.platform.startswith("linux"):
        return
    entry = os.path.join(ctx.SCRIPT_DIR, "tests", "fuzz_link_check_main.cpp")
    harness = os.path.join(ctx.SCRIPT_DIR, "tests", "fuzz_main.cpp")
    for path in (entry, harness):
        if not os.path.exists(path):
            print(f"Fuzz link check FAILED: missing {path}")
            sys.exit(1)
    for name in sorted(FUZZ_LINUX_TARGETS):
        if name not in FUZZ_TARGETS:
            print(f"Fuzz link check FAILED: {name} is in FUZZ_LINUX_TARGETS but "
                  f"not in FUZZ_TARGETS")
            sys.exit(1)
        extra_sources = [os.path.join(ctx.SOURCE_DIR, source)
                         for source in FUZZ_LINUX_EXTRA_SOURCES.get(name, ())]
        for source in extra_sources:
            if not os.path.exists(source):
                print(f"Fuzz link check FAILED: {name} names a missing "
                      f"translation unit: {source}")
                sys.exit(1)
        cmd = [
            ctx.ZIG_EXE, "c++", "-std=c++17", "-DNDEBUG",
            f'-DAPP_VERSION="{ctx.APP_VERSION}"',
            f"-DAPP_BUILD_NUMBER={ctx.APP_BUILD_NUMBER}",
            f"-DGC_FUZZ_TARGET={FUZZ_TARGETS[name]}",
            "-fno-exceptions", "-fno-rtti", "-O1",
            "-target", ctx.LINUX_TARGET,
            f"-I{ctx.SOURCE_DIR}",
            "-Wall", "-Wextra", "-Wshadow", "-Wno-unused-function",
            "-Wno-unused-parameter", "-Werror",
            # Same force-include the real Linux fuzz build uses: the harness
            # names WCHAR and must stay unmodified for the Windows build.
            "-include", os.path.join(ctx.SOURCE_DIR, "win32_compat.h"),
            "-o", os.path.join(tmp_dir, f"fuzz_link_{name}"),
            harness, entry, *extra_sources,
        ]
        linked = ", ".join(["fuzz_main.cpp"]
                           + [os.path.basename(p) for p in extra_sources])
        print(f"Cross-linking ({ctx.LINUX_TARGET}) fuzz target {name} "
              f"[{linked}]")
        if ctx._run_zig_link(cmd) != 0:
            print(f"Fuzz target {name} cross-link FAILED (an undefined symbol "
                  f"means a translation unit is missing from "
                  f"FUZZ_LINUX_EXTRA_SOURCES['{name}'])")
            sys.exit(1)


def run_windows_pipe_fixture(ctx, tmp_dir, extra_flags):
    """Build and run the native Windows named-pipe fixture on win32 hosts.

    The 2026-08-22 incident (impersonation moved before the first pipe read,
    error 1368, plus an active-session SDDL that broke scheduled logon
    handoffs) is exactly the class of regression a pure test cannot see: it
    lives in Win32 message-mode read semantics. tests/
    windows_pipe_regression.cpp drives real named pipes in-process and proves
    the 12-byte header probe is a valid first read, impersonation succeeds
    after it, and one stalled client cannot block another.
    """
    if sys.platform != "win32":
        return
    fixture_source = os.path.join(ctx.SCRIPT_DIR, "tests",
                                  "windows_pipe_regression.cpp")
    fixture_exe = os.path.join(tmp_dir, "windows_pipe_regression")
    cmd = [
        ctx.LLVM_MINGW_CLANG, "-std=c++17", "-DNDEBUG",
        f'-DAPP_VERSION="{ctx.APP_VERSION}"',
        f"-DAPP_BUILD_NUMBER={ctx.APP_BUILD_NUMBER}",
        "-fno-exceptions", "-fno-rtti",
        f"-I{ctx.SOURCE_DIR}",
        "-o", fixture_exe,
        fixture_source,
    ]
    if extra_flags:
        cmd.extend(extra_flags)
    print("Compiling Windows named-pipe regression tests")
    result = subprocess.run(cmd, cwd=ctx.SCRIPT_DIR)
    if result.returncode != 0:
        print("Windows named-pipe test compilation FAILED")
        sys.exit(result.returncode)
    env = os.environ.copy()
    env["PATH"] = os.path.dirname(ctx.LLVM_MINGW_CLANG) + os.pathsep +         env.get("PATH", "")
    print("Running Windows named-pipe regression tests")
    result = subprocess.run([fixture_exe], cwd=ctx.SCRIPT_DIR, env=env)
    if result.returncode != 0:
        print(f"Windows named-pipe regression FAILED ({result.returncode})")
        sys.exit(result.returncode)


def run_cli_console_fixture(ctx, built_exe=None):
    """Prove the built GUI-subsystem binary actually writes to its caller (F-01-001).

    This is the one check that could not be a source guard or a pure assertion:
    the defect was that `greencurve.exe --help` produced ZERO bytes on stdout
    and stderr and returned exit code 0, because a -subsystem:windows image has
    no console and nothing ever attached one. Only running the real artifact
    with a captured stdout answers it.

    A captured pipe is also the exact path the fix's step 2 handles -- the child
    sees a FILE_TYPE_PIPE standard handle -- so this covers redirection at the
    same time. The interactive CONOUT$ path cannot be driven from a test and is
    verified by hand; see llm-wiki/windows-architecture.md.

    Skipped (not failed) when no built binary is present, because --test is
    expected to run without a prior build.
    """
    if sys.platform != "win32":
        return
    if built_exe is None:
        built_exe = os.path.join(ctx.SCRIPT_DIR, "dist", "windows-x64",
                                 "greencurve", "greencurve.exe")
    if not os.path.exists(built_exe):
        print("Skipping CLI console fixture: no built greencurve.exe")
        return
    print("Running CLI console output fixture")
    result = subprocess.run([built_exe, "--help"], cwd=ctx.SCRIPT_DIR,
                            capture_output=True, text=True, timeout=60)
    combined = (result.stdout or "") + (result.stderr or "")
    if not combined.strip():
        print("CLI console fixture FAILED: --help wrote nothing to the caller's "
              "stdout/stderr (F-01-001 regression)")
        sys.exit(1)
    for needle in ("NVIDIA VF Curve Editor", "--service-install", "--help"):
        if needle not in combined:
            print(f"CLI console fixture FAILED: --help output is missing {needle!r}")
            sys.exit(1)
    if result.returncode != 0:
        print(f"CLI console fixture FAILED: --help exited {result.returncode}")
        sys.exit(1)


def run_fuzz_targets(ctx, runs=None, target_filter=None):
    """Build and briefly exercise every libFuzzer target.

    The default run is bounded in iterations rather than seconds, so the gate is
    deterministic and carries no timing assumption.
    """
    linux_host = sys.platform.startswith("linux")
    if sys.platform != "win32" and not linux_host:
        print(f"Fuzzing is not wired for this host ({sys.platform}); "
              f"supported hosts are Windows and Linux")
        sys.exit(1)
    harness = os.path.join(ctx.SCRIPT_DIR, "tests", "fuzz_main.cpp")
    if not os.path.exists(harness):
        print(f"Fuzz harness missing: {harness}")
        sys.exit(1)
    runtime = None
    host_clang = None
    if linux_host:
        host_clang = host_fuzz_compiler()
        if not host_clang:
            # A missing toolchain used to be an unconditional `return 0`, so
            # `--fuzz` reported success on every non-Windows host without
            # building anything.  Failing loudly is the point of a gate.
            print("clang++ not found on PATH; the Linux fuzz targets need a host "
                  "clang with libclang_rt.fuzzer/asan (the bundled Zig ships "
                  "neither). Install clang or run --fuzz on the Windows host.")
            sys.exit(1)
    else:
        runtime = fuzzer_runtime_archive(ctx)
        if not runtime:
            print("libFuzzer runtime (libclang_rt.fuzzer-x86_64.a) not found in llvm-mingw")
            sys.exit(1)

    selected = dict(FUZZ_TARGETS)
    if target_filter:
        if target_filter not in FUZZ_TARGETS:
            print(f"Unknown fuzz target {target_filter!r}; "
                  f"choose from {', '.join(sorted(FUZZ_TARGETS))}")
            sys.exit(1)
        if linux_host and target_filter not in FUZZ_LINUX_TARGETS:
            # Explicitly asked for a target this host cannot build: say so
            # instead of quietly running nothing.
            print(f"Fuzz target {target_filter!r} is Windows-only (it links "
                  f"Win32-only shards); Linux targets are "
                  f"{', '.join(sorted(FUZZ_LINUX_TARGETS))}")
            sys.exit(1)
        selected = {target_filter: FUZZ_TARGETS[target_filter]}
    else:
        selected = fuzz_targets_for_host(sys.platform)
        skipped = sorted(set(FUZZ_TARGETS) - set(selected))
        if skipped:
            print(f"Linux host: skipping Windows-only targets ({', '.join(skipped)})")

    iterations = runs if runs is not None else DEFAULT_FUZZ_RUNS
    source_dir = ctx.SOURCE_DIR
    tmp = ctx.prepare_work_subdir("fuzz")
    env = os.environ.copy()
    if not linux_host:
        env["PATH"] = os.path.dirname(ctx.LLVM_MINGW_CLANG) + os.pathsep + env.get("PATH", "")
    # A sanitizer finding must fail the build, not merely print.
    env["ASAN_OPTIONS"] = "abort_on_error=1:allocator_may_return_null=0"
    env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    try:
        for name, value in sorted(selected.items(), key=lambda kv: kv[1]):
            exe = os.path.join(tmp, f"fuzz_{name}" + ("" if linux_host else ".exe"))
            cmd = [
                host_clang if linux_host else ctx.LLVM_MINGW_CLANG,
                "-std=c++17",
                "-DNDEBUG",
                f'-DAPP_VERSION="{ctx.APP_VERSION}"',
                f"-DAPP_BUILD_NUMBER={ctx.APP_BUILD_NUMBER}",
                f"-DGC_FUZZ_TARGET={value}",
                "-fno-exceptions",
                "-fno-rtti",
                "-O1",
                "-g",
                f"-I{source_dir}",
                "-Wall", "-Wextra", "-Wshadow", "-Wno-unused-function",
                "-Wno-unused-parameter", "-Werror",
                # ASan finds the memory errors, UBSan the arithmetic ones; the
                # combination is the configuration that actually matters here.
                "-fsanitize=address",
                "-fsanitize=undefined",
                "-fno-sanitize-recover=all",
            ]
            if linux_host:
                # clang accepts the convenience flag for x86_64-linux-gnu and
                # links its own runtime; only the mingw triple needs the manual
                # instrumentation + archive pair below.
                cmd.append("-fsanitize=fuzzer")
                # win32_compat.h is force-included rather than #included, the
                # same way tests/regression_main.cpp gets it: the harness names
                # WCHAR (parse_cli_point_arg_w) and must stay unmodified for the
                # Windows build, which must never see the shim.
                cmd.extend(["-include", os.path.join(source_dir, "win32_compat.h")])
            else:
                cmd.extend(FUZZ_COVERAGE_FLAGS)
            cmd.extend(["-o", exe, harness])
            if linux_host:
                cmd.extend(os.path.join(source_dir, extra)
                           for extra in FUZZ_LINUX_EXTRA_SOURCES.get(name, ()))
            else:
                cmd.extend(os.path.join(source_dir, extra)
                           for extra in FUZZ_WIN32_SOURCES)
                cmd.extend([
                    runtime,
                    "-luser32", "-lgdi32", "-luuid", "-ladvapi32", "-lshell32",
                ])
            print(f"Compiling fuzz target {name}")
            # Fuzz fixture links may drive Zig on a POSIX host; route them
            # through the same lock/repair wrapper as every other Zig link.
            returncode = zig_cache.run_zig_link(
                cmd, ctx.SCRIPT_DIR, ctx.ZIG_CACHE_ROOTS)
            if returncode != 0:
                print(f"Fuzz target {name} FAILED to compile")
                sys.exit(returncode)

            # Seeds are committed, read-only inputs.  libFuzzer writes any new
            # coverage-increasing input to the scratch corpus instead, so a test
            # run never mutates the repository.
            seeds = os.path.join(fuzz_corpus_dir(ctx), name)
            scratch = os.path.join(tmp, f"corpus_{name}")
            os.makedirs(scratch, exist_ok=True)
            run_cmd = [exe, scratch]
            if os.path.isdir(seeds):
                run_cmd.append(seeds)
            run_cmd.extend([
                f"-runs={iterations}",
                "-print_final_stats=1",
                # Fixed entropy so a CI failure reproduces locally.
                "-seed=1",
                "-max_len=8192",
                # An input that hangs is a real bug, but the limit is generous
                # enough that it never becomes a timing assumption.
                "-timeout=60",
                f"-artifact_prefix={os.path.join(tmp, name + '-')}",
            ])
            print(f"Fuzzing {name} for {iterations} runs")
            result = subprocess.run(run_cmd, cwd=ctx.SCRIPT_DIR, env=env)
            if result.returncode != 0:
                print(f"Fuzz target {name} FAILED ({result.returncode}) — "
                      f"reproducer written under {tmp}")
                sys.exit(result.returncode)
        print("Fuzz targets passed")
        return 0
    finally:
        ctx.cleanup_work_subdir(tmp)


# Profile-directory names that are obviously not a real person: placeholders,
# environment variables, and the synthetic accounts the tests and docs use.
_ALLOWED_PROFILE_NAMES = frozenset({
    "test", "tester", "testuser", "user", "username", "public", "default",
    "all users", "%username%", "%userprofile%", "$user", "<name>", "<user>",
    "<username>", "<admin>", "<youruser>",
})

_PROFILE_PATH_RE = re.compile(r"[Cc]:[\\/]{1,2}Users[\\/]{1,2}([^\\/\s\"'`)<>]+|<[^>]+>)")

# The POSIX and macOS spelling of the same thing.  The pattern above requires
# a `C:` drive letter, so it only ever sees the Windows form -- but a home
# directory names a person on every operating system, and a check that knows
# one spelling is a check the next platform walks straight past.  Both share
# the allowlist and placeholder handling below so the two cannot drift.
#
# A bare `/home` with no segment does not match, and the lookbehind keeps
# this from firing on a path that is part of a URL.
_POSIX_HOME_PATH_RE = re.compile(
    r"(?<![A-Za-z0-9_/])/(?:home|Users)/([^/\s\"'`)<>:]+|<[^>]+>)")

# Update-signing private key material.  Names first, because a filename match
# has no false positives and catches the file before its contents matter.
_SIGNING_KEY_NAME_RE = re.compile(
    r"(signing-key|update-key|private-key)|\.(pem|p8|pfx|p12)$", re.IGNORECASE)
# A PEM private key block, in any of the spellings OpenSSL emits.
_PEM_PRIVATE_RE = re.compile(r"-----BEGIN (?:[A-Z ]+ )?PRIVATE KEY-----")
# A line that is nothing but 64 hex characters is what a raw P-256 scalar looks
# like on disk.  Deliberately anchored to the WHOLE line: SHA-256 digests are
# also 64 hex characters and appear legitimately in the manifest fixtures and
# in tools/update_signing.py's test vectors, but always with surrounding text.
_BARE_HEX_KEY_RE = re.compile(r"^[0-9a-fA-F]{64}$")


def check_no_signing_key_material(ctx, tracked):
    """Fail if update-signing private key material is tracked by git.

    The updater's whole security model rests on a key GitHub has never held:
    the build-provenance attestation only proves that CI built an artifact from
    some commit, so an attacker who can push a commit can mint a valid
    attestation for hostile code.  A key that never leaves the maintainer's
    machine is what closes that, and committing it -- once, briefly, then
    reverted -- destroys the property permanently, because git history is
    forever and the repository is public.

    `.gitignore` is not sufficient on its own: `git add -f` bypasses it, and so
    does a rename into a pattern that was never listed.  This gate looks at
    what is actually tracked.

    `tracked` is build.py's tracked-file list, or None when git is unavailable
    (tarball/export build), in which case this cannot be assessed."""
    if tracked is None:
        return
    offenders = []
    for rel in tracked:
        base = os.path.basename(rel)
        # The verifier's PUBLIC keys are meant to be in the repository; only
        # the private halves are forbidden, and they never carry that name.
        if base == "update_verify_keys.h":
            continue
        if _SIGNING_KEY_NAME_RE.search(base):
            offenders.append(f"{rel}: filename looks like private key material")
            continue
        path = os.path.join(ctx.SCRIPT_DIR, rel)
        try:
            with open(path, "r", encoding="utf-8") as handle:
                text = handle.read()
        except (OSError, UnicodeDecodeError):
            continue  # binary fuzz corpora and unreadable files hold no PEM
        if _PEM_PRIVATE_RE.search(text):
            offenders.append(f"{rel}: contains a PEM private key block")
            continue
        for line_no, line in enumerate(text.splitlines(), 1):
            if _BARE_HEX_KEY_RE.match(line.strip()):
                offenders.append(
                    f"{rel}:{line_no}: bare 64-hex line (raw P-256 scalar?)")
                break
    if offenders:
        print("Regression source check FAILED: update-signing private key "
              "material must never be tracked (see llm-wiki/updates.md):")
        for offender in offenders[:20]:
            print(f"  {offender}")
        sys.exit(1)


def check_no_developer_profile_paths(ctx, tracked):
    """Fail if a tracked text file hardcodes somebody's real home directory.

    A wiki entry once recorded a developer's real profile directory under the
    Windows Users folder, which is exactly the private-user-data leak the
    project rules forbid and is permanent once pushed.  Placeholders, single
    letter fixtures and synthetic test accounts are fine; anything else is
    assumed to be a real account name.

    Covers the Windows, POSIX and macOS spellings together.  A gate that
    knows only one of them is one that the same value walks past in another
    platform's notation, so all three share this function's allowlist and
    placeholder rules rather than being checked in separate places.

    `tracked` is build.py's tracked-file list, or None when git is unavailable
    (tarball/export build), in which case this cannot be assessed and must not
    fail an otherwise legitimate build."""
    if tracked is None:
        return
    offenders = []
    for rel in tracked:
        path = os.path.join(ctx.SCRIPT_DIR, rel)
        try:
            with open(path, "r", encoding="utf-8") as handle:
                text = handle.read()
        except (OSError, UnicodeDecodeError):
            continue  # binary fuzz corpora and unreadable files are not docs
        for line_no, line in enumerate(text.splitlines(), 1):
            for match in itertools.chain(
                    _PROFILE_PATH_RE.finditer(line),
                    _POSIX_HOME_PATH_RE.finditer(line)):
                name = match.group(1).strip().rstrip(".,;:").lower()
                # Any angle-bracketed segment is a placeholder by construction:
                # '<' and '>' are invalid in a Windows account name, so it can
                # never be a real profile.  Checked structurally rather than by
                # spelling, because the allowlist below could not keep up --
                # this gate flagged the very wiki line documenting its own fix
                # ("recorded a real C:\\Users\\<dev>\\... path") for using a
                # placeholder that happened not to be enumerated.
                if name.startswith("<") and name.endswith(">"):
                    continue
                # A one-character segment is a test fixture, never an account
                # name worth protecting.
                if len(name) > 1 and name not in _ALLOWED_PROFILE_NAMES:
                    offenders.append(f"{rel}:{line_no}: {match.group(0)}")
    if offenders:
        print("Regression source check FAILED: tracked files hardcode a real "
              "user home directory (use %USERPROFILE%, $HOME, or a "
              "placeholder such as /home/testuser):")
        for offender in offenders[:20]:
            print(f"  {offender}")
        sys.exit(1)


def _workflow_structure_errors(path, text):
    """Return structural errors that would make a GitHub workflow unloadable.

    GitHub Actions is not exercised when this repository runs locally, and the
    project deliberately avoids a YAML dependency in the release toolchain.
    This gate is intentionally narrow: it pins the step-list indentation used
    by both workflows and catches the concrete failure mode where a step is
    accidentally emitted at file scope, which makes the entire CI workflow
    unavailable and silently disables every merge gate.
    """
    errors = []
    if "\t" in text:
        errors.append(f"{path}: tab characters are not allowed")
    for line_number, line in enumerate(text.splitlines(), 1):
        match = re.match(r"^(\s*)- name:", line)
        if match and len(match.group(1)) != 6:
            errors.append(
                f"{path}:{line_number}: workflow step must be indented exactly "
                "six spaces")
    return errors


def check_workflow_structure(ctx):
    """Keep the security/test merge workflows loadable."""
    all_errors = []
    for name in ("ci.yml", "release.yml"):
        path = os.path.join(ctx.SCRIPT_DIR, ".github", "workflows", name)
        with open(path, "r", encoding="utf-8") as handle:
            all_errors.extend(_workflow_structure_errors(path, handle.read()))
    ci_path = os.path.join(ctx.SCRIPT_DIR, ".github", "workflows", "ci.yml")
    with open(ci_path, "r", encoding="utf-8") as handle:
        ci_text = handle.read()
    if "permissions:\n  contents: read\n" not in ci_text:
        all_errors.append(f"{ci_path}: CI must explicitly request contents:read")
    if 'PYTHONUNBUFFERED: "1"' not in ci_text:
        all_errors.append(f"{ci_path}: CI must flush Python diagnostics immediately")
    if "python build.py --test --sanitizer" in ci_text:
        all_errors.append(
            f"{ci_path}: --test already enables UBSan; do not duplicate the suite")
    masked_sequence = (
        "run: |\n"
        "          python build.py --test\n"
        "          python build.py --check --target all"
    )
    if masked_sequence in ci_text:
        all_errors.append(
            f"{ci_path}: test and check must be separate fail-fast steps")

    # Exercise the checker itself with the exact malformed indentation that
    # previously made the whole CI workflow unparsable.
    malformed = "jobs:\n  job:\n    steps:\n- name: Broken\n      run: true\n"
    if not any(":4:" in error for error in
               _workflow_structure_errors("fixture.yml", malformed)):
        print("Build-script regression FAILED: workflow indentation checker "
              "accepted a file-scope step")
        sys.exit(1)

    if all_errors:
        print("Workflow structure regression FAILED:")
        for error in all_errors:
            print(f"  {error}")
        sys.exit(1)


def check_diagnostic_probe_gates(ctx, require_text, forbid_text):
    """Pin fail-closed parsing for the write-capable clock-domain probe."""
    self_test = os.path.join(ctx.SOURCE_DIR, "main_self_test.cpp")
    forbid_text(self_test, "atoi(",
                "the clock probe never maps malformed selectors to entry zero")
    require_text(self_test, "gc_clk_probe_entry::parse(",
                 "the clock probe strictly range-checks its entry selector")
    # The ClkDomains correlation arrays were once sized [16] while the fill and
    # read loops range over CLK_PROBE_MAX_DOMAINS (32): a silent stack overflow
    # that MinGW's stack layout absorbed and the MSVC-ABI layout crashed on.
    # Pin both arrays to the loop-bound constant so they cannot drift apart.
    require_text(self_test, "unsigned int measuredKhz[CLK_PROBE_MAX_DOMAINS]",
                 "the ClkDomains correlation arrays are sized by the domain loop bound")
    require_text(self_test, "bool measuredValid[CLK_PROBE_MAX_DOMAINS]",
                 "the ClkDomains validity flags are sized by the domain loop bound")
    forbid_text(self_test, "measuredKhz[16]",
                "the ClkDomains correlation overflow must never return")


# Expressions that carry a Windows account name, a user-profile path, or a
# machine name.  F-03-001: source/log_redaction_policy.h has existed since
# 2026-08 to keep exactly these out of the default-on support log, and it was
# applied in the identity code and nowhere else -- because nothing enforced it.
# A 21 MB live log carried "C:\\Users\\<account>\\..." from four sites and the
# Task Scheduler task name ("Green Curve Startup - <HOST>_<account>") from a
# fifth.  This gate is the enforcement the policy never had.
IDENTITY_BEARING_LOG_ARGUMENTS = (
    "g_userDataDir",
    "g_app.configPath",
    "g_debugLogPath",
    "g_forcedStartupUserSam",
    "taskName",
)

# The tokenizers from log_redaction_policy.h.  A call that names one of the
# expressions above is fine as long as it goes through one of these.
LOG_REDACTION_TOKENIZERS = (
    "gc_log_path_token(",
    "gc_log_identifier_token(",
    "gc_log_wide_identifier_token(",
    "gc_log_u64_token(",
)


def _balanced_call_text(text, open_paren_index):
    """The source text of one call, from '(' to its matching ')'.

    Returns None for an unbalanced tail rather than guessing: a truncated span
    would make the gate silently stop checking the rest of the call.
    """
    depth = 0
    for index in range(open_paren_index, len(text)):
        char = text[index]
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return text[open_paren_index:index + 1]
    return None


def check_service_command_authority_gates(ctx, require_text, service_server_cpp):
    """Pin the command -> authorization-tier contract (F-03-002).

    This replaced an inline `caller->integrityRid < SECURITY_MANDATORY_MEDIUM_RID`
    comparison over a hand-written command list in the pipe switch. The list had
    nowhere to record that SERVICE_CMD_SET_UPDATE_POLICY mutates MACHINE-wide
    persistent state while every one of its neighbours is per-session hardware
    intent, which is how a standard console user came to be able to disable
    automatic update checking for the whole machine.

    The tier of every command is asserted exhaustively by the regression harness
    (5259-5269); these gates pin the wiring the harness cannot see -- that the
    pipe actually consults the table, with the right bound and the right caller
    fact, and that the table still fails closed.
    """
    require_text(service_server_cpp, "service_command_authority_reject_reason(",
                 "control and file-output requests reject low-integrity clients")
    require_text(service_server_cpp, "(unsigned int)SECURITY_MANDATORY_MEDIUM_RID",
                 "the integrity bound handed to the authority policy is the medium-integrity RID")
    require_text(service_server_cpp, "caller->isAdmin",
                 "machine-scope commands are gated on local-administrator membership")
    policy = os.path.join(ctx.SOURCE_DIR, "service_command_authority_policy.h")
    require_text(policy, "case SERVICE_CMD_SET_UPDATE_POLICY:",
                 "the machine-wide update policy has an explicit authorization tier")
    require_text(policy, "return SERVICE_COMMAND_TIER_MACHINE_ADMIN;",
                 "a machine-admin tier exists and is reachable")
    require_text(policy, "default:\n            return SERVICE_COMMAND_TIER_MACHINE_ADMIN;",
                 "an unclassified command fails closed rather than into the weakest tier")


def check_path_protection_gates(ctx, require_text, service_ipc_cpp):
    """Pin the F-SEC-1 hardening and its location policy (chain proof + consent).

    The old "direct child of Program Files" rule was a proxy for one property:
    nothing unprivileged may substitute the LocalSystem service binary or any
    directory above it.  service_path_chain_policy.h states that property
    directly, and the administrator decides about everything else.  The
    escalation sentence is pinned verbatim so the one statement that says WHY
    the location matters cannot silently vanish from setup's folder page and
    its acknowledgment.
    """
    require_text(service_ipc_cpp, "apply_protected_service_binary_dacl(targetPath",
                 "service install hardens the installed binary DACL")
    require_text(service_ipc_cpp, "restore_inherited_dacl(targetPath",
                 "service uninstall reverts the binary DACL to inherited")
    service_acl_cpp = os.path.join(ctx.SOURCE_DIR, "service_acl.cpp")
    require_text(service_acl_cpp, "PROTECTED_DACL_SECURITY_INFORMATION",
                 "binary DACL hardening disables inheritance")
    require_text(service_acl_cpp, "(A;;0x1200a9;;;BU)",
                 "binary DACL grants BUILTIN\\Users read+execute only")
    path_chain_policy = os.path.join(ctx.SOURCE_DIR, "service_path_chain_policy.h")
    require_text(path_chain_policy,
        "anything that can write this folder - other accounts or software running as you - "
        "can replace the LocalSystem background service and gain SYSTEM rights",
        "the path-risk consent keeps the SYSTEM-escalation sentence")
    require_text(path_chain_policy, "gc_path_protection_classify",
        "the path-protection classifier exists and is pure")
    path_chain_cpp = os.path.join(ctx.SOURCE_DIR, "service_path_chain.cpp")
    require_text(path_chain_cpp,
        "classify_path_protection", "the Win32 path-protection gatherer exists")
    require_text(service_ipc_cpp, "service_log_path_protection_at_startup",
        "service startup records the service directory's protection verdict")
    # The walk must begin at the DRIVE root, never at GetVolumePathNameW's
    # answer.  A volume mounted into a directory (C:\mnt\data) hangs below
    # ordinary renameable directories; starting at the mount path skipped them
    # entirely AND applied the "a root cannot be renamed" DELETE relaxation to
    # a directory that can be, so a substitutable chain read as protected.
    require_text(path_chain_cpp, "full[1] == L':'",
        "the chain walk starts at the drive root, not at a volume mount path")
    forbid_in_walk = "GetVolumePathNameW(full, volume"
    with open(path_chain_cpp, "r", encoding="utf-8", errors="replace") as handle:
        chain_text = handle.read()
    if forbid_in_walk in chain_text and "const size_t rootEnd = 3;" not in chain_text:
        print("Regression source check FAILED: the path-chain walk derives its "
              "root from the volume mount path again (mount-point ancestors "
              "would go unproven)")
        sys.exit(1)
    # Create-only rights are harmless on an ancestor and decisive on the leaf:
    # the leaf is where the LocalSystem binary resolves DLL imports from.
    require_text(path_chain_cpp, "kCreateMask",
        "the walk records who may create files beside the service binary")
    require_text(path_chain_policy, "non_admin_create_danger",
        "leaf create rights are part of the protection verdict")
    # Consent for a LocalSystem service registered out of an unprotected
    # folder exists on BOTH interactive paths, setup and the GUI checkbox.
    require_text(os.path.join(ctx.SOURCE_DIR, "ui_main_window.cpp"),
        "gc_path_protection_requires_acknowledgment",
        "the GUI service-install confirmation carries the path risk")
    # A remedy line the user pastes: every path quote-closed, /setowner its own
    # invocation (icacls rejects it combined with /grant with error 87), every
    # SID:permission argument quoted so PowerShell does not read (OI)(CI) as a
    # subexpression.
    require_text(path_chain_policy, "GC_PATH_PROTECTION_REMEDY_TAIL_PATH",
        "the icacls remedy issues /setowner as its own command")
    require_text(path_chain_policy, "\\\" /setowner \\\"*S-1-5-32-544\\\"",
        "the remedy closes the path quote before /setowner and quotes the SID")
    # Registering the service REWRITES the install directory's DACL. Which
    # folders that may be done to is a gate, not a warning, and it has to stand
    # in front of every path that reaches the hardening: the portable/GUI path
    # (ensure_secure_service_binary_path), setup's folder page, and setup's own
    # execute -- the last being the only check a silent `/S /D=<path>` run gets.
    location_policy = os.path.join(ctx.SOURCE_DIR, "service_install_location_policy.h")
    require_text(location_policy, "GC_SVC_LOCATION_DRIVE_ROOT",
        "the install-location policy refuses a drive root")
    require_text(location_policy, "GC_SVC_LOCATION_KNOWN_FOLDER",
        "the install-location policy refuses a well-known shell folder")
    require_text(path_chain_cpp, "gc_service_install_location_verdict",
        "the Win32 half of the install-location gate exists")
    require_text(path_chain_cpp, "FOLDERID_Downloads",
        "the location gate refuses the Downloads folder by name")
    require_text(path_chain_cpp, "same_directory_identity(candidate, known)",
        "the location gate recognizes short names and ancestor aliases by directory identity")
    require_text(path_chain_cpp, "gc_service_location_is_profile_shell_folder(",
        "the location gate recognizes another UAC account's profile shell folders")
    require_text(service_ipc_cpp, "gc_service_install_location_verdict(installDir)",
        "service install refuses to harden a folder that is not its own")
    require_text(os.path.join(ctx.SOURCE_DIR, "installer_apply.cpp"),
        "gc_service_install_location_verdict(targetDirectory)",
        "setup refuses the same folders, including on the silent path")
    require_text(os.path.join(ctx.SOURCE_DIR, "ui_main_window.cpp"),
        "running_exe_dir_install_location_verdict",
        "the GUI service checkbox refuses before it asks for consent")
    # The install-location check and the uninstall DACL revert must share ONE
    # predicate. They did not, and the asymmetry was unrecoverable: install
    # hardened a drive root, uninstall refused to revert one.
    require_text(service_ipc_cpp, "gc_service_location_shape_is_acceptable(dir)",
        "uninstall's revert skip uses the same shape rule install enforces")


def check_service_admin_reason_gates(ctx, require_text, service_ipc_cpp):
    """Pin the reason taxonomy that crosses the elevation boundary.

    The GUI drives `--service-install` through ShellExecuteEx("runas") and can
    observe NOTHING of that process but its exit code. Every failure used to
    exit 1, so the user saw "Elevated service helper failed (exit code 1)" and
    the real reason sat in a log under the LocalAppData of whichever account
    approved the UAC prompt -- unreachable on a standard-user machine. The exit
    code IS the reason now, and these gates keep it that way.
    """
    policy = os.path.join(ctx.SOURCE_DIR, "service_admin_reason_policy.h")
    require_text(policy, "gc_service_admin_reason_exit_code",
        "a failure reason is encodable as an exit code")
    require_text(policy, "gc_service_admin_reason_from_exit_code",
        "an exit code decodes back to a failure reason")
    require_text(policy, "GC_SVC_ADMIN_MARKED_FOR_DELETE",
        "the 1072 dead end has its own reason and remedy")
    require_text(policy, "approved the elevation prompt",
        "the log pointer names whose profile the log is in")
    cli_admin_cpp = os.path.join(ctx.SOURCE_DIR, "main_cli_admin.cpp")
    require_text(cli_admin_cpp, "gc_service_admin_reason_exit_code(reason)",
        "the CLI exits with the classified reason, not a bare 1")
    require_text(service_ipc_cpp, "gc_service_admin_reason_from_exit_code(exitCode)",
        "the GUI decodes the helper's exit code into a reason")
    require_text(service_ipc_cpp, "GC_SVC_ADMIN_ELEVATION_DECLINED",
        "a declined UAC prompt is told apart from a failure")
    # Elevation is checked up front. "Failed opening service manager (error 5)"
    # was the least self-explanatory way this failed and the easiest to fix.
    require_text(os.path.join(ctx.SOURCE_DIR, "main_service_install.cpp"),
        "GC_SVC_ADMIN_NOT_ELEVATED",
        "service install refuses unelevated with a remedy, not an error number")
    # A pending SCM state must carry progress, or nothing can tell a slow start
    # from a hung one -- the condition behind "did not respond to the start or
    # control request in a timely fashion".
    host_cpp = os.path.join(ctx.SOURCE_DIR, "main_service_host.cpp")
    require_text(host_cpp, "service_report_start_progress",
        "the service publishes START_PENDING progress checkpoints")
    require_text(host_cpp, "dwWaitHint = SERVICE_START_WAIT_HINT_MS",
        "START_PENDING carries a wait hint")
    require_text(host_cpp, "dwWaitHint = SERVICE_STOP_WAIT_HINT_MS",
        "STOP_PENDING carries a wait hint")
    # Every parent of the helper waits on the SAME derived budget.  A private
    # timeout below stop+start+staging terminates a healthy slow install and
    # reports failure for work that then completes anyway -- which is exactly
    # the "merely slow to start" case this release stops reporting as failed.
    require_text(os.path.join(ctx.SOURCE_DIR, "installer_apply.cpp"),
        "GC_APP_CLI_TIMEOUT_MS GC_SVC_ADMIN_HELPER_TIMEOUT_MS",
        "setup waits on the shared admin-helper budget")
    require_text(os.path.join(ctx.SOURCE_DIR, "installer_register.cpp"),
        "GC_SVC_ADMIN_HELPER_TIMEOUT_MS",
        "the uninstaller waits on the shared admin-helper budget")
    # Setup runs the same helper and must render the same classified sentence
    # from its exit code, not a bare number.
    require_text(os.path.join(ctx.SOURCE_DIR, "installer_apply.cpp"),
        "gc_service_admin_reason_from_exit_code(exitCode)",
        "setup decodes the helper exit code into the remedy sentence")


def check_log_redaction(ctx):
    """No debug_log call may name an identity-bearing value in the clear.

    Deliberately a real scan rather than a list of forbidden literals: the
    defect was four INDEPENDENT sites drifting from a policy, so the gate has
    to cover sites nobody has written yet.
    """
    offenders = []
    for name in sorted(os.listdir(ctx.SOURCE_DIR)):
        if not name.endswith((".cpp", ".h")):
            continue
        path = os.path.join(ctx.SOURCE_DIR, name)
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            text = handle.read()
        for match in re.finditer(r"\bdebug_log(?:_on_change)?\s*\(", text):
            call = _balanced_call_text(text, match.end() - 1)
            if call is None:
                offenders.append(f"{name}: unbalanced debug_log call near offset {match.start()}")
                continue
            if any(tokenizer in call for tokenizer in LOG_REDACTION_TOKENIZERS):
                continue
            for argument in IDENTITY_BEARING_LOG_ARGUMENTS:
                # Whole-identifier match: `taskNameToken` is the FIX for
                # `taskName`, so a substring test would flag every fixed site.
                pattern = r"(?<![A-Za-z0-9_])" + re.escape(argument) + r"(?![A-Za-z0-9_])"
                if re.search(pattern, call):
                    line = text.count("\n", 0, match.start()) + 1
                    offenders.append(f"{name}:{line}: logs {argument} without a redaction token")
                    break
    if offenders:
        print("Regression source check FAILED: identity-bearing values reach the debug log")
        for offender in offenders:
            print(f"  {offender}")
        print("  Route them through source/log_redaction_policy.h (F-03-001).")
        sys.exit(1)


def run_build_script_regression_tests(ctx):
    """Self-tests for build.py's own invariants.

    Lives here rather than in build.py so the build script stays under
    BUILD_SCRIPT_SIZE_RATCHET; it is the same one-way dependency as every other
    gate in this module.
    """
    tmp = ctx.prepare_work_subdir("build_script_regression")
    try:
        check_workflow_structure(ctx)
        build_scheduler.run_self_tests()
        zig_cache.run_self_tests()
        arch_package.run_self_tests()
        build_script = os.path.join(ctx.SCRIPT_DIR, "build.py")
        with open(build_script, "r", encoding="utf-8", errors="replace") as handle:
            build_script_text = handle.read()
        installer_script = os.path.join(ctx.SCRIPT_DIR, "tools", "installer_build.py")
        with open(installer_script, "r", encoding="utf-8", errors="replace") as handle:
            installer_script_text = handle.read()
        # Every Zig link must run under zig_cache's cross-process lock and its
        # poisoned-cache repair; the 2026-08-28 matrix failure showed a racing
        # or poisoned shared global cache fails four links at once and never
        # self-heals.  A future link call site that bypasses the wrapper is
        # exactly how that returns.
        if "zig_cache.run_zig_link" not in build_script_text:
            print("Build-script regression FAILED: build.py Zig links bypass the "
                  "cross-process zig-cache lock/repair wrapper")
            sys.exit(1)
        if "_run_zig_link(cmd, cwd=work)" not in build_script_text:
            print("Build-script regression FAILED: the ARM64 link paths bypass the "
                  "cross-process zig-cache lock/repair wrapper")
            sys.exit(1)
        if "zig_cache.run_zig_link" not in installer_script_text:
            print("Build-script regression FAILED: the arm64 installer link bypasses "
                  "the cross-process zig-cache lock/repair wrapper")
            sys.exit(1)
        # The generated .PKGINFO may only carry keys pacman's install-time
        # parser knows: the first generated packages shipped `makepkgopt` and
        # an `install` key, and pacman logged "unknown key" for each on every
        # install.  The self-tests' negative fixtures interpolate these as
        # format placeholders, so a literal occurrence is always a generator.
        arch_script = os.path.join(ctx.SCRIPT_DIR, "tools", "arch_package.py")
        with open(arch_script, "r", encoding="utf-8", errors="replace") as handle:
            arch_package_text = handle.read()
        for banned in ("makepkgopt = ", "install = .INSTALL"):
            if banned in arch_package_text:
                print(f"Build-script regression FAILED: arch_package.py emits the "
                      f"invalid .PKGINFO key {banned!r} pacman warns about on "
                      "every install")
                sys.exit(1)
        if "validate_pkginfo_keys(text)" not in arch_package_text:
            print("Build-script regression FAILED: verify_arch_package does not "
                  "run the .PKGINFO key allowlist gate")
            sys.exit(1)
        # The single-command Windows builders must emit every flag as ONE
        # argument. A bare `*"-DFOO=1"` conditional explodes the string into
        # single characters: that corrupted the LSP/clang-tidy database (silent
        # clang-tidy execution failure on main.cpp on every host) and would
        # break the legacy jobs==1 llvm-mingw x64 service build
        # (2026-08-29 toolchain commit).
        for arch in ("x64", "arm64"):
            gui_cmd = ctx.get_windows_gui_compile_command(
                os.path.join(tmp, "lsp-gui.out"), arch)
            service_cmd = ctx.get_windows_service_compile_command(
                os.path.join(tmp, "lsp-service.out"), arch)
            for label, cmd in (("GUI", gui_cmd), ("service", service_cmd)):
                exploded = [arg for arg in cmd if len(arg) == 1]
                if exploded:
                    print(f"Build-script regression FAILED: {label} command "
                          f"({arch}) contains exploded single-character "
                          f"arguments {exploded[:8]}")
                    sys.exit(1)
            if service_cmd.count("-DGREEN_CURVE_SERVICE_BINARY=1") != 1:
                print(f"Build-script regression FAILED: service command ({arch}) "
                      "must carry the service define exactly once")
                sys.exit(1)
            if "-DGREEN_CURVE_SERVICE_BINARY=1" in gui_cmd:
                print(f"Build-script regression FAILED: GUI command ({arch}) "
                      "must not carry the service define")
                sys.exit(1)
        signing_script = os.path.join(ctx.SCRIPT_DIR, "tools", "windows_key_acl.py")
        with open(signing_script, "r", encoding="utf-8", errors="replace") as handle:
            signing_script_text = handle.read()
        if "ctypes.create_string_buffer(needed.value)" not in signing_script_text:
            print("Build-script regression FAILED: TOKEN_USER query lacks its "
                  "variable-length result buffer")
            sys.exit(1)
        if ("OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION" not in
                signing_script_text):
            print("Build-script regression FAILED: signing-key hardener does not "
                  "set the hosted token user as file owner")
            sys.exit(1)
        if "buffer = TokenUser()" in signing_script_text:
            print("Build-script regression FAILED: TOKEN_USER query can overflow "
                  "a fixed ctypes structure")
            sys.exit(1)
        for needle, label in (
                ('"--jobs"', "--jobs CLI"),
                ("build_scheduler.run_parallel", "parallel task scheduler wiring"),
                ("object-first clang x64", "object-first Windows x64 path")):
            if needle not in build_script_text:
                print(f"Build-script regression FAILED: {label} missing")
                sys.exit(1)
        tool_path = os.path.join(tmp, "tool.bin")
        with open(tool_path, "wb") as handle:
            handle.write(b"trusted tool bytes")
        trusted = ctx._sha256_file(tool_path)
        ctx._write_integrity_sentinel(tool_path, trusted)
        if not ctx._verify_cached_tool_binary(tool_path, "test-tool", trusted):
            print("Build-script regression FAILED: trusted cached tool rejected")
            sys.exit(1)
        with open(tool_path, "wb") as handle:
            handle.write(b"attacker replacement")
        ctx._write_integrity_sentinel(tool_path, ctx._sha256_file(tool_path))
        # mismatch_expected: this call IS the assertion, so its rejection must
        # not print as an ERROR and make a passing build look like a
        # supply-chain incident.
        if ctx._verify_cached_tool_binary(tool_path, "test-tool", trusted,
                                          mismatch_expected=True):
            print("Build-script regression FAILED: adjacent sentinel trusted over pinned digest")
            sys.exit(1)
        if ctx.requested_arches("all") != ["x64", "arm64"]:
            print("Build-script regression FAILED: --arch all does not select both architectures")
            sys.exit(1)
        payload = os.path.join(tmp, "payload")
        os.makedirs(payload)
        expected = {"greencurve.exe", "greencurve-service.exe"}
        for name in expected:
            with open(os.path.join(payload, name), "wb") as handle:
                handle.write(b"fixture")
        ctx.validate_payload_file_names(payload, expected)
        with open(os.path.join(payload, "main.lib"), "wb") as handle:
            handle.write(b"unexpected linker side product")
        try:
            ctx.validate_payload_file_names(payload, expected)
        except RuntimeError:
            pass
        else:
            print("Build-script regression FAILED: unexpected package file accepted")
            sys.exit(1)
        check_hardening_and_gate_wiring(ctx)
        # The clang-tidy ratchet decides whether a build fails, so its matching
        # rules are covered here rather than only by running clang-tidy itself:
        # these self-tests need no toolchain and run on every host.
        static_analysis.run_self_tests()
        # The update signer's known-answer vectors.  These run here rather than
        # only on demand because the signer is one half of a cross-language
        # agreement -- the other half is the CNG verifier, asserted at 4230-4249
        # -- and a break in either is invisible until every published update is
        # refused in the field.  They need no toolchain and no network.
        if not update_signing.run_self_tests():
            print("Build-script regression FAILED: update signer self-tests")
            sys.exit(1)
        toolchain.run_self_tests()
        run_debug_tool_discovery_tests(ctx)
    finally:
        ctx.cleanup_work_subdir(tmp)
    check_linux_release_packaging(ctx)
    check_arch_package_roundtrip(ctx)


def run_debug_tool_discovery_tests(ctx):
    """Run tools/test-debug-tool-discovery.ps1 when it can run at all.

    The script it exercises, tools/discover-debug-tools.ps1, is untracked on
    purpose (it records this machine's SDK/MSVC locations), so the test is
    reachable only on a developer box that has one.  Without this call the
    test was committed and then never invoked by anything, which is the same
    as not having it.  A missing PowerShell 7 or a missing discovery script is
    a loud SKIP; a real assertion failure is fatal like every other gate.
    """
    if sys.platform != "win32":
        return
    script = os.path.join(ctx.SCRIPT_DIR, "tools", "test-debug-tool-discovery.ps1")
    if not os.path.exists(script):
        print("Debug-tool discovery tests: SKIPPED (test script not present)")
        return
    pwsh = shutil.which("pwsh")
    if not pwsh:
        print("Debug-tool discovery tests: SKIPPED (pwsh not on PATH)")
        return
    result = subprocess.run(
        [pwsh, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
         "-File", script],
        capture_output=True, text=True, errors="replace")
    output = (result.stdout or "") + (result.stderr or "")
    if result.returncode != 0:
        print("Build-script regression FAILED: debug-tool discovery tests")
        print(output.strip())
        sys.exit(1)
    print(output.strip() or "Debug-tool discovery tests passed.")


def check_linux_release_packaging(ctx):
    """F-LNX-EOL / F-LNX-MODE: the Linux archive must be correct off any host.

    Both halves of this used to be host-dependent, and both were fatal.  A
    Windows working tree is CRLF (git's autocrlf smudge) and shutil.copy2 put
    that straight into the archive, so the shipped shebang was
    `#!/usr/bin/env bash\\r` -- an interpreter literally named "bash\\r".  And
    os.chmod(0o755) is a no-op on Windows while 7-Zip records no Unix mode
    there, so the daemon and the setup script both extracted non-executable,
    which greencurve-setup.sh rejects itself via `[ -x "$BINARY" ]`.  The Linux
    archive was therefore only usable when packaged on a Linux host.

    Everything below runs the real packaging code over a deliberately CRLF,
    mode-less fixture, so it fails against the pre-fix behaviour.
    """
    tmp = ctx.prepare_work_subdir("linux_release_packaging")
    try:
        if release_manifest.release_archive_extension("linux") != ".tar.xz":
            print("Build-script regression FAILED: the Linux container cannot record Unix modes")
            sys.exit(1)
        # A lone CR is not a line ending this project writes; rewriting it would
        # silently corrupt content, so it must be refused rather than converted.
        try:
            release_manifest.normalize_release_text(b"before\rafter")
        except RuntimeError:
            pass
        else:
            print("Build-script regression FAILED: a lone CR was silently rewritten")
            sys.exit(1)

        source = os.path.join(tmp, "source")
        staging = os.path.join(tmp, "greencurve")
        os.makedirs(source)
        os.makedirs(staging)
        # Exactly what a Windows checkout hands the packager.
        fixtures = {
            "greencurve": b"\x7fELF\x02\x01\x01 fixture \r\n raw bytes \r\n",
            "greencurve-setup.sh": b"#!/usr/bin/env bash\r\nset -euo pipefail\r\n",
            "README.md": b"# Green Curve\r\n",
            "LICENSE": b"MIT\r\n",
        }
        expected = release_manifest.expected_release_names("linux")
        if set(fixtures) != expected:
            print("Build-script regression FAILED: packaging fixture does not match "
                  f"the release manifest ({sorted(expected)})")
            sys.exit(1)
        for name, data in fixtures.items():
            with open(os.path.join(source, name), "wb") as handle:
                handle.write(data)
            release_manifest.stage_release_file(os.path.join(source, name),
                                                os.path.join(staging, name), normalize=True)
        # Normalization is for text only: the ELF payload must survive verbatim,
        # CR bytes included.
        with open(os.path.join(staging, "greencurve"), "rb") as handle:
            if handle.read() != fixtures["greencurve"]:
                print("Build-script regression FAILED: release staging rewrote a binary")
                sys.exit(1)

        archive = os.path.join(tmp, "release.tar.xz")
        release_manifest.write_linux_tarball(archive, staging, "greencurve", expected)
        release_manifest.verify_linux_tarball(archive, expected, "greencurve")
        with tarfile.open(archive, "r:xz") as tar:
            members = {m.name: (m.mode, m.uid, m.gid) for m in tar.getmembers()}
            script = tar.extractfile("greencurve/greencurve-setup.sh").read()
        if not script.startswith(b"#!/usr/bin/env bash\n") or b"\r" in script:
            print("Build-script regression FAILED: the archived setup script is not an "
                  f"LF shell script (starts {script[:24]!r})")
            sys.exit(1)
        for name, mode in (("greencurve/greencurve", 0o755),
                           ("greencurve/greencurve-setup.sh", 0o755),
                           ("greencurve/README.md", 0o644),
                           ("greencurve/LICENSE", 0o644)):
            if members.get(name) != (mode, 0, 0):
                print(f"Build-script regression FAILED: {name} archived as "
                      f"{members.get(name)}, expected ({oct(mode)}, uid 0, gid 0)")
                sys.exit(1)
        _check_linux_tarball_rejects_broken_members(tmp, expected)
    finally:
        ctx.cleanup_work_subdir(tmp)


def _check_linux_tarball_rejects_broken_members(tmp, expected):
    """The read-back must reject each defect that actually shipped.

    Verification reads the finished archive rather than the staging tree, so a
    writer bug cannot pass by agreeing with the code that fed it."""
    defects = (
        ("a CRLF shell script", 0o755, b"#!/usr/bin/env bash\r\nexit 0\r\n"),
        ("a non-executable setup script", 0o644, b"#!/usr/bin/env bash\nexit 0\n"),
        ("a setup script with no shebang", 0o755, b"exit 0\n"),
    )
    for label, mode, body in defects:
        archive = os.path.join(tmp, "broken.tar.xz")
        with tarfile.open(archive, "w:xz") as tar:
            top = tarfile.TarInfo("greencurve")
            top.type = tarfile.DIRTYPE
            top.mode = 0o755
            tar.addfile(top)
            for name in sorted(expected):
                script = name.endswith(".sh")
                data = body if script else b"placeholder\n"
                info = tarfile.TarInfo(f"greencurve/{name}")
                info.size = len(data)
                info.mode = mode if script else release_manifest.release_member_mode(name)
                tar.addfile(info, io.BytesIO(data))
        try:
            release_manifest.verify_linux_tarball(archive, expected, "greencurve")
        except RuntimeError:
            os.remove(archive)
            continue
        print(f"Build-script regression FAILED: the archive read-back accepted {label}")
        sys.exit(1)


def check_arch_package_roundtrip(ctx):
    """F-ARCH-PKG: Arch Linux package round-trip and structure verification."""
    tmp = ctx.prepare_work_subdir("arch_package_roundtrip")
    try:
        bin_path = os.path.join(tmp, "greencurve")
        with open(bin_path, "wb") as f:
            f.write(b"\x7fELF\x02\x01\x01 fixture binary \n")
        pkg_path = arch_package.build_arch_package(
            ctx.SCRIPT_DIR, ctx.APP_VERSION, "x64", bin_path, output_dir=tmp)
        if not os.path.isfile(pkg_path):
            print(f"Build-script regression FAILED: Arch package not created: {pkg_path}")
            sys.exit(1)
        sha_path = pkg_path + ".sha256"
        if not os.path.isfile(sha_path):
            print(f"Build-script regression FAILED: Arch checksum not created: {sha_path}")
            sys.exit(1)
    finally:
        ctx.cleanup_work_subdir(tmp)


def check_hardening_and_gate_wiring(ctx):
    """Linux hardening flags and per-host gate selection (F-LNX-HARDEN).

    Guards the 2026-07-28 findings: the Linux build shipped with zero endbr64
    because -fcf-protection=full sat in WINDOWS_FLAGS only, and --fuzz reported
    success on every non-Windows host without building anything.
    """
    def fail(message):
        print(f"Build-script regression FAILED: {message}")
        sys.exit(1)

    x64 = ctx.linux_flags_for_arch("x64")
    arm64 = ctx.linux_flags_for_arch("arm64")
    if "-fcf-protection=full" not in x64:
        fail("Linux x64 lost the CET flag")
    if "-flto" not in x64 or "-flto" not in ctx._linux_object_compile_flags("x64"):
        fail("Linux x64 lost LTO in its compile or link flags")
    # aarch64 does not merely ignore it -- clang hard-errors with "option
    # 'cf-protection=return' cannot be specified on this target".
    if "-fcf-protection=full" in arm64:
        fail("x86-only CET flag reached aarch64")
    if "-mbranch-protection=standard" not in arm64:
        fail("aarch64 lost branch protection")
    if "-flto" in arm64 or "-fno-lto" not in arm64:
        fail("Linux arm64 lost its branch-protection-preserving no-LTO policy")
    for arch, flags in (("x64", x64), ("arm64", arm64)):
        if "-ftrivial-auto-var-init=pattern" not in flags:
            fail(f"Linux {arch} lost auto-var-init")
    # The ASan fallback decision, independent of whether clang is installed.
    if sanitizer_build_requested([]):
        fail("empty flags requested a sanitizer build")
    if sanitizer_build_requested(["-fsanitize=undefined", "-g"]):
        fail("UBSan alone selected the ASan toolchain")
    for flags in (["-fsanitize=address"], ["-fsanitize=address,undefined"]):
        if not sanitizer_build_requested(flags):
            fail(f"ASan not detected in {flags}")
    # Per-host fuzz target selection.
    if fuzz_targets_for_host("win32") != FUZZ_TARGETS:
        fail("Windows host lost fuzz targets")
    linux_targets = fuzz_targets_for_host("linux")
    if not linux_targets or not set(linux_targets) <= set(FUZZ_TARGETS):
        fail("Linux fuzz target set is empty or unknown")
    if fuzz_targets_for_host("darwin"):
        fail("unsupported host claims fuzz targets")
    # F-LNX-DEDUP: the shared helpers must be in the Linux link, or
    # linux_port.cpp silently needs its private copies back.
    linux_sources = {os.path.basename(path) for path in ctx.LINUX_SOURCE_FILES}
    for shared in ("config_text_utils.cpp", "fan_curve.cpp"):
        if shared not in linux_sources:
            fail(f"{shared} is not in LINUX_SOURCE_FILES; the Linux binary "
                 f"would fall back to a duplicated private copy")
    # The same move has to reach the Win32 fuzz link, which is a THIRD list.
    # It did not, and every Win32 fuzz target failed to link for a week because
    # --fuzz was only ever run on a Linux host afterwards.
    windows_sources = {os.path.basename(path) for path in ctx.WINDOWS_SOURCE_FILES}
    for shared in ("config_text_utils.cpp", "fan_curve.cpp"):
        if shared not in windows_sources:
            fail(f"{shared} is not in WINDOWS_SOURCE_FILES")
        if shared not in FUZZ_WIN32_SOURCES:
            fail(f"{shared} is not in FUZZ_WIN32_SOURCES; the Win32 fuzz "
                 f"targets would fail to link on its definitions")
    for extra in FUZZ_WIN32_SOURCES:
        if not os.path.exists(os.path.join(ctx.SOURCE_DIR, extra)):
            fail(f"FUZZ_WIN32_SOURCES names a missing file {extra!r}")
    # Every supported host owns the full cross-build matrix by default.
    if ctx.resolve_targets("linux") != ["linux"]:
        fail("resolve_targets rejects an ordinary Linux target")
    if ctx.resolve_targets("windows") != ["windows"]:
        fail("resolve_targets rejects a Windows cross-build")
    if ctx.resolve_targets("all") != ["windows", "linux"]:
        fail("resolve_targets does not preserve the full default matrix")
    if sys.platform.startswith("linux"):
        if ctx.LLVM_MINGW_ARCHIVE_EXT != ".tar.xz":
            fail("Linux host did not select the native llvm-mingw archive")
        for tool in (ctx.LLVM_MINGW_CLANG, ctx.LLVM_MINGW_RC,
                     ctx.LLVM_MINGW_OBJCOPY, ctx.LLVM_MINGW_STRIP,
                     ctx.LLVM_MINGW_READOBJ, ctx.LLVM_MINGW_PDBUTIL,
                     ctx.LLVM_MINGW_NM):
            if tool.endswith(".exe"):
                fail(f"Linux host selected a PE build tool: {tool}")
    # Every extra-source entry must name a real target and a real file.
    for target, extras in FUZZ_LINUX_EXTRA_SOURCES.items():
        if target not in FUZZ_LINUX_TARGETS:
            fail(f"fuzz extra sources named for non-Linux target {target!r}")
        for extra in extras:
            if not os.path.exists(os.path.join(ctx.SOURCE_DIR, extra)):
                fail(f"fuzz target {target!r} lists a missing source {extra!r}")


def check_ipc_transport_and_probe_gates(ctx, require_text, forbid_text,
                                        require_text_count):
    """Pins for the transition-safe pipe transport and the capability probe.

    Split out of build.py to keep the build script under its size ratchet.
    Three review findings (2026-08-23) each escaped every existing gate
    because they were silent in the happy path:

    - two function-local admission tables made rate limiting inert: decisions
      read a table no charge ever touched;
    - the duplicated client token was closed nowhere after the worker-pool
      refactor dropped the historical explicit close;
    - b544f7d deleted both g_app.gpuCapability publication sites, so every
      snapshot carried a zeroed probe while the real results were discarded.
    """
    service_pipe_cpp = os.path.join(ctx.SOURCE_DIR, "main_service_pipe.cpp")
    capability_probe_cpp = os.path.join(ctx.SOURCE_DIR,
                                        "gpu_capability_probe.cpp")

    # One shared admission table. Decisions and charges must observe the same
    # bucket state; forbid the historical per-function declaration shape.
    require_text(service_pipe_cpp,
                 "ServiceIpcAdmissionTable& service_admission_table()",
                 "admission decisions and charges observe ONE shared bucket table")
    forbid_text(service_pipe_cpp, "static ServiceIpcAdmissionTable table",
                "function-local duplicate admission tables must stay removed")
    # Exactly one charge per connection, derived from its outcome: both the
    # admitted path and the refusal fall-through go through the cost table.
    require_text_count(service_pipe_cpp,
                       "service_ipc_connection_cost_tokens(", 2,
                       "admitted and refusal paths both charge exactly once by outcome")
    # The capture runs before magic/admission checks, so EVERY connection owns
    # a duplicated token handle; only a destructor closes it on all exits.
    require_text(service_pipe_cpp, "~ServiceClientIdentity()",
                 "duplicated client tokens are closed on every connection exit path")
    # The probe publishes on BOTH exits (early NVML-not-ready return included).
    require_text_count(capability_probe_cpp,
                       "g_app.gpuCapability = probe;", 2,
                       "the probe publishes its result on BOTH exit paths")

    # The native pipe fixture must stay race-free: the server disconnects only
    # after the client consumed (or terminally failed to read) the response,
    # because DisconnectNamedPipe discards unread buffered pipe data and would
    # hand the client a spurious ERROR_BROKEN_PIPE (2026-08-29 CI, exit 907).
    # Both client terminal paths release the server, and the client verifies
    # the answer, so a broken/short/garbage response can never pass as a pong.
    pipe_fixture_cpp = os.path.join(ctx.SCRIPT_DIR, "tests",
                                    "windows_pipe_regression.cpp")
    require_text(pipe_fixture_cpp,
                 "if (outcome->responded && outcome->responseConsumedEvent)",
                 "the fixture server waits for response consumption before disconnecting")
    require_text_count(pipe_fixture_cpp, "notify_response_consumed(ctx);", 2,
                       "both fixture client terminal paths release the server")
    require_text(pipe_fixture_cpp,
                 "if (!readOk || transferred != sizeof(response)) return 909;",
                 "the fixture client rejects failed or short response reads")
    require_text(pipe_fixture_cpp, "response.status != SERVICE_STATUS_OK",
                 "the fixture client verifies the response content")


def check_fuzz_harness_in_sync(ctx, require_text, forbid_text):
    """The fuzz target table here must match tests/fuzz_main.cpp.

    A target whose macro value drifts out of sync compiles to a file with no
    LLVMFuzzerTestOneInput, or worse, to a different target than the corpus it
    is fed.  Both tables and the seed corpora are checked so a rename cannot
    quietly disable coverage.
    """
    harness = os.path.join(ctx.SCRIPT_DIR, "tests", "fuzz_main.cpp")
    if not os.path.exists(harness):
        print("Regression source check FAILED: tests/fuzz_main.cpp is missing")
        sys.exit(1)
    with open(harness, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    declared = dict(re.findall(r"^#define\s+GC_FUZZ_([A-Z_]+)\s+(\d+)\s*$",
                               text, re.MULTILINE))
    declared.pop("TARGET", None)
    expected = {name.upper(): str(value) for name, value in FUZZ_TARGETS.items()}
    if declared != expected:
        print("Regression source check FAILED: fuzz target tables disagree")
        print(f"  tools/security_gates.py FUZZ_TARGETS: {expected}")
        print(f"  tests/fuzz_main.cpp #defines        : {declared}")
        sys.exit(1)
    for name in FUZZ_TARGETS:
        if f"GC_FUZZ_TARGET == GC_FUZZ_{name.upper()}" not in text:
            print(f"Regression source check FAILED: no harness body for fuzz target {name}")
            sys.exit(1)
        seeds = os.path.join(fuzz_corpus_dir(ctx), name)
        if not os.path.isdir(seeds) or not os.listdir(seeds):
            print(f"Regression source check FAILED: fuzz target {name} has no seed "
                  f"corpus (expected files under tests/fuzz-corpus/{name})")
            sys.exit(1)
    # Every target must assert post-conditions, not merely avoid crashing.
    if text.count("GC_FUZZ_CHECK(") < 2 * len(FUZZ_TARGETS):
        print("Regression source check FAILED: fuzz targets lack post-condition assertions")
        sys.exit(1)
    gates = os.path.join(ctx.SCRIPT_DIR, "tools", "security_gates.py")
    require_text(gates, "-fsanitize-coverage=",
                 "fuzzing uses the ungated coverage flag, not the rejected "
                 "-fsanitize=fuzzer driver flag")
    require_text(gates, "def check_cet_instrumentation",
                 "CET instrumentation effectiveness stays verifiable from build.py")
    # F-LNX-HARDEN source guards.
    build_script = os.path.join(ctx.SCRIPT_DIR, "build.py")
    require_text(build_script, "x86 CET instrumentation missing",
                 "Linux x64 release artifacts are gated on endbr64 presence, the "
                 "analogue of the ARM64 BTI/PAC artifact gate")
    require_text(build_script, 'flags.remove("-fcf-protection=full")',
                 "aarch64 drops the x86-only CET flag; clang hard-errors on it")
    require_text(gates, "FUZZ_LINUX_TARGETS",
                 "the fuzz driver knows which targets a Linux host can build")
    # Every Linux fuzz link line must be proven on every host.  --fuzz builds
    # the Linux targets only ON Linux, so FUZZ_LINUX_EXTRA_SOURCES used to be
    # verified nowhere but the Linux CI job; that is how service_request shipped
    # without fan_curve.cpp and failed two consecutive runs on main.
    require_text(gates, "def check_fuzz_linux_link_lines",
                 "the Linux fuzz link lines are cross-linked on non-Linux hosts")
    require_text(build_script, "security_gates.check_fuzz_linux_link_lines(",
                 "--test runs the Linux fuzz link check")
    require_text(gates,
                 '"service_request": ("fan_curve.cpp", "config_text_utils.cpp")',
                 "the service_request fuzz target links the IPC trust "
                 "boundary's out-of-line fan-curve normalizer")
    entry = os.path.join(ctx.SCRIPT_DIR, "tests", "fuzz_link_check_main.cpp")
    require_text(entry, "return LLVMFuzzerTestOneInput(",
                 "the link-check entry really calls the fuzz entry point; a "
                 "mere declaration is collected away with the undefined "
                 "references the check exists to find")
    require_text(gates, "def posix_test_compiler",
                 "sanitizer builds resolve a host clang; Zig ships no ASan runtime")
    require_text(gates,
                 'supported hosts are Windows and Linux")\n        sys.exit(1)',
                 "an unsupported fuzz host fails instead of reporting a false-green gate")
    forbid_text(gates, "Fuzzing is currently wired for the Windows host "
                       "toolchain only",
                "--fuzz must not return success without building anything")
    # F-LNX-DEDUP: linux_port.cpp must not re-grow private copies of the shared
    # ASCII/INI helpers.  Matching the definition (name + '(' at column 0, i.e.
    # a non-static file-scope function) rather than any mention, so call sites
    # and comments stay legal.
    linux_port = os.path.join(ctx.SOURCE_DIR, "linux_port.cpp")
    with open(linux_port, "r", encoding="utf-8", errors="replace") as handle:
        linux_port_text = handle.read()
    for symbol, signature in (("fan_curve_normalize",
                               "void fan_curve_normalize(FanCurveConfig* config) {"),
                              ("fan_curve_set_default",
                               "void fan_curve_set_default(FanCurveConfig* config) {"),
                              ("fan_curve_interpolate_percent",
                               "int fan_curve_interpolate_percent(const "
                               "FanCurveConfig* config, int temperatureC) {"),
                              ("fan_curve_validate",
                               "bool fan_curve_validate(const FanCurveConfig* "
                               "config, char* err, size_t errSize) {"),
                              ("trim_ascii", "void trim_ascii(char* s) {"),
                              ("streqi_ascii",
                               "bool streqi_ascii(const char* a, const char* b) {"),
                              ("parse_int_strict",
                               "bool parse_int_strict(const char* s, int* out) {"),
                              ("set_message",
                               "void set_message(char* dst, size_t dstSize, "
                               "const char* fmt, ...) {"),
                              ("parse_fan_value",
                               "bool parse_fan_value(const char* text, bool* "
                               "isAuto, int* pct) {")):
        if signature in linux_port_text:
            print(f"Regression source check FAILED: linux_port.cpp redefines "
                  f"{symbol}; it must come from config_text_utils.cpp so the "
                  f"Linux daemon runs the code the tests and fuzzers cover")
            sys.exit(1)
    require_text(os.path.join(ctx.SOURCE_DIR, "config_text_utils.cpp"),
                 "bool parse_fan_value(",
                 "the shared TU owns parse_fan_value for both platforms")
    forbid_text(os.path.join(ctx.SOURCE_DIR, "config_utils.cpp"),
                "bool parse_fan_value(",
                "parse_fan_value must not move back into the Win32-only shard")
    # The shared fan-curve text is displayed by a UTF-8 terminal on Linux and by
    # ANSI Win32 on Windows, so it may not hardcode either degree encoding.
    fan_curve_cpp = os.path.join(ctx.SOURCE_DIR, "fan_curve.cpp")
    require_text(fan_curve_cpp, "GC_DEGREE",
                 "shared fan-curve text uses the per-platform degree sign")
    forbid_text(fan_curve_cpp, "\\xB0\"",
                "a hardcoded 0xB0 is invalid UTF-8 in the Linux TUI")


# Prebuilt llvm-mingw runtime archives (libc++, libc++abi with its bundled
# itanium demangler, libunwind, the MinGW CRT) are compiled by the toolchain
# vendor WITHOUT -fcf-protection, so their address-taken functions land in the
# linker's Guard CF table uninstrumented.  Those are not our regressions.
_VENDOR_RUNTIME = re.compile(
    r"libunwind|^_Unwind_|^unw_|^__libunwind|^__gxx_personality"
    r"|itanium_demangle|OutputBuffer|__cxxabiv1|^__cxa_|^_ZN?K?St|^_ZSt"
    r"|demangling_terminate_handler"
    r"|CRTStartup|^_pei386|^__mingw|^_gnu_exception|^__C_specific|^_vsnwprintf"
    r"|^__ms_|^_amsg|^__report|^atexit$|^_cexit|^__acrt|^__p_|^_initterm"
    r"|^__do_global|^__dyn_tls|^__tlreg|^fpreset|^_matherr|^safe_flush"
    r"|^__main$|^__getmainargs|^_setargv|^__write_memory|^___chkstk"
    r"|^__local_stdio|^fprintf$|^vfprintf$|^__guard_.*dummy|^_onexit"
    r"|^__gcc|^__mingwthr|^_GLOBAL__|^_assert$|^dtoa_lock_cleanup$")

_ENDBR64 = b"\xf3\x0f\x1e\xfa"


def _pe_sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        return None
    machine = struct.unpack_from("<H", data, pe + 4)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    if struct.unpack_from("<H", data, opt)[0] != 0x20B:
        return None
    imagebase = struct.unpack_from("<Q", data, opt + 24)[0]
    sections = []
    for i in range(nsec):
        base = opt + optsz + i * 40
        vsz, va, rsz, ptr = struct.unpack_from("<IIII", data, base + 8)
        sections.append((data[base:base + 8].rstrip(b"\0").decode("latin1"),
                         va, vsz, ptr, rsz))
    return machine, imagebase, opt, sections


def _build_unstripped_probe(ctx):
    """Compile the shipped Windows x64 GUI flags minus -s.

    Release binaries are stripped, so they carry no symbol table and a target
    cannot be attributed to our source rather than the vendor runtime.  Building
    the probe here keeps the gate self-contained and exact: it uses the real
    hardening flag set rather than trusting whatever happens to sit in dist/.
    """
    ctx.configure_build_number(False)  # defines APP_BUILD_NUMBER, no bump
    tmp = ctx.prepare_work_subdir("cet")
    out = os.path.join(tmp, "greencurve-cet-probe.exe")
    cmd = [arg for arg in ctx.get_windows_gui_compile_command(out, "x64")
           if arg != "-s"]
    print("CET check: compiling an unstripped probe with the shipped flags")
    result = subprocess.run(cmd, cwd=ctx.SCRIPT_DIR, capture_output=True, text=True)
    if result.returncode != 0 or not os.path.exists(out):
        print(result.stdout)
        print(result.stderr)
        print("CET check: probe compilation FAILED")
        return None, tmp
    return out, tmp


# GNU_PROPERTY_X86_FEATURE_1_AND and its IBT/SHSTK bits.
_GNU_PROPERTY_X86_FEATURE_1_AND = 0xC0000002
_X86_FEATURE_1_IBT = 1
_X86_FEATURE_1_SHSTK = 2


def _elf_sections(data):
    """(machine, [(name, sh_type, addr, offset, size, entsize, link)]) or None."""
    if len(data) < 64 or data[:6] != b"\x7fELF\x02\x01":
        return None
    machine = struct.unpack_from("<H", data, 18)[0]
    shoff, = struct.unpack_from("<Q", data, 40)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 58)
    if not shoff or not shnum:
        return machine, []
    name_hdr = shoff + shstrndx * shentsize
    strtab_off, = struct.unpack_from("<Q", data, name_hdr + 24)
    sections = []
    for i in range(shnum):
        base = shoff + i * shentsize
        name_idx, sh_type = struct.unpack_from("<II", data, base)
        addr, offset, size = struct.unpack_from("<QQQ", data, base + 16)
        link, = struct.unpack_from("<I", data, base + 40)
        entsize, = struct.unpack_from("<Q", data, base + 56)
        end = data.find(b"\0", strtab_off + name_idx)
        name = data[strtab_off + name_idx:end].decode("latin1")
        sections.append((name, sh_type, addr, offset, size, entsize, link))
    return machine, sections


def _elf_x86_feature_bits(data, sections):
    """IBT/SHSTK bits from .note.gnu.property, or None when the note is absent."""
    for name, _t, _a, offset, size, _e, _l in sections:
        if name != ".note.gnu.property":
            continue
        pos = offset
        end = offset + size
        while pos + 12 <= end:
            namesz, descsz, ntype = struct.unpack_from("<III", data, pos)
            desc = pos + 12 + ((namesz + 3) & ~3)
            if ntype != 5:  # NT_GNU_PROPERTY_TYPE_0
                pos = desc + ((descsz + 3) & ~3)
                continue
            walk, limit = desc, desc + descsz
            while walk + 8 <= limit:
                ptype, psize = struct.unpack_from("<II", data, walk)
                if ptype == _GNU_PROPERTY_X86_FEATURE_1_AND and psize >= 4:
                    return struct.unpack_from("<I", data, walk + 8)[0]
                walk += 8 + ((psize + 7) & ~7)
            pos = desc + ((descsz + 3) & ~3)
    return None


def _object_function_symbols(data, sections):
    """[(name, section_index, value, size)] for defined STT_FUNC symbols."""
    out = []
    for _n, sh_type, _a, offset, size, entsize, link in sections:
        if sh_type != 2 or not entsize:  # SHT_SYMTAB
            continue
        strtab = sections[link]
        for pos in range(offset, offset + size, entsize):
            name_idx, info, _other, shndx = struct.unpack_from("<IBBH", data, pos)
            value, sym_size = struct.unpack_from("<QQ", data, pos + 8)
            if (info & 0xF) != 2 or shndx == 0:  # STT_FUNC, defined
                continue
            end = data.find(b"\0", strtab[3] + name_idx)
            out.append((data[strtab[3] + name_idx:end].decode("latin1"),
                        shndx, value, sym_size))
    return out


def _check_elf_cet_objects(ctx):
    """Compile our Linux x64 TUs and verify CET reached every one of them.

    Object-level attribution rather than the Windows path's Guard-CF-table walk:
    ELF has no equivalent table, and a linked image mixes our code with the
    glibc CRT and compiler-rt.  Checking the objects we produce answers the
    actual question -- is -fcf-protection=full effective on OUR code -- with no
    vendor noise to filter.
    """
    flags = ctx._compile_only_flags([*ctx.COMMON_FLAGS,
                                     *ctx.linux_flags_for_arch("x64")])
    # Deliberately the pinned Zig, not a host clang: the question is whether the
    # flag is effective on the objects we actually ship, so the probe has to use
    # the shipping toolchain and the shipping flag set (-target included, from
    # linux_flags_for_arch).
    tmp = ctx.prepare_work_subdir("cet-elf")
    try:
        print(f"ELF CET check: compiling {len(ctx.LINUX_SOURCE_FILES)} probe objects")
        objects = []
        for index, source in enumerate(ctx.LINUX_SOURCE_FILES):
            # One object per TU.  `zig c++ -c a.cpp b.cpp` writes a single
            # output named after the first source, which silently reduced this
            # gate to checking one file.
            stem = os.path.splitext(os.path.basename(source))[0]
            obj = os.path.join(tmp, f"{index:02d}-{stem}.o")
            result = subprocess.run([ctx.ZIG_EXE, "c++", *flags, "-c", source,
                                     "-o", obj],
                                    cwd=tmp, capture_output=True, text=True)
            if result.returncode != 0:
                print(result.stderr[-4000:])
                print(f"ELF CET check: probe compilation FAILED for {source}")
                return 1
            objects.append(obj)
        if not objects:
            print("ELF CET check: probe produced no objects")
            return 1
        missing_property, endbr_functions, checked = [], 0, 0
        for obj in objects:
            with open(obj, "rb") as handle:
                data = handle.read()
            parsed = _elf_sections(data)
            if parsed is None:
                print(f"ELF CET check: {os.path.basename(obj)} is not ELF64")
                return 1
            _machine, sections = parsed
            bits = _elf_x86_feature_bits(data, sections)
            if bits is None or not (bits & _X86_FEATURE_1_IBT) \
                    or not (bits & _X86_FEATURE_1_SHSTK):
                missing_property.append(os.path.basename(obj))
            for sym, shndx, value, _size in _object_function_symbols(data, sections):
                if shndx >= len(sections):
                    continue
                sec = sections[shndx]
                start = sec[3] + value
                if start + 4 > len(data):
                    continue
                checked += 1
                if data[start:start + 4] == _ENDBR64:
                    endbr_functions += 1
        print(f"  probe objects              : {len(objects)}")
        print(f"  our functions              : {checked}")
        # Informational, NOT an invariant: -fcf-protection emits endbr64 only at
        # functions that can be reached by an indirect branch.  A directly-called
        # static function correctly has none, so requiring it everywhere would
        # fail on correct output.  The per-object property bits below are the
        # exact "was the flag applied to this TU" signal, which is why they are
        # what the gate enforces.
        print(f"  ... with endbr64 entry     : {endbr_functions}")
        print(f"  objects missing IBT+SHSTK  : {len(missing_property)}")
        if missing_property:
            print("  FAILED: -fcf-protection=full did not mark these objects:")
            for name in missing_property[:20]:
                print(f"    {name}")
            return 1
        if checked == 0:
            print("  FAILED: no function symbols attributed to our code at all")
            return 1
        if endbr_functions == 0:
            print("  FAILED: not one of our functions carries an endbr64 entry")
            return 1
        print("  OK: every object we ship declares IBT+SHSTK and indirect-branch "
              "targets carry endbr64")
        return 0
    finally:
        ctx.cleanup_work_subdir(tmp)


def _report_elf_linked_state(ctx):
    """Report what survives into the shipped images (advisory, never fatal).

    The per-object property notes above do NOT survive the link: lld intersects
    GNU_PROPERTY_X86_FEATURE_1_AND across all inputs, and the bundled Zig CRT
    objects carry no property, so the linked image has none and no loader will
    ever turn on IBT or shadow stack.  Forcing it is not reachable through the
    pinned toolchain -- `zig cc` rejects both `-z shstk` and `-z force-ibt` as
    unsupported linker extension flags.  Reported rather than papered over.
    """
    for arch in ("x64", "arm64"):
        path = os.path.join(ctx.SCRIPT_DIR, "dist", f"linux-{arch}",
                            "greencurve", "greencurve")
        if not os.path.exists(path):
            continue
        with open(path, "rb") as handle:
            data = handle.read()
        parsed = _elf_sections(data)
        if parsed is None:
            continue
        _machine, sections = parsed
        note = any(name == ".note.gnu.property" for name, *_rest in sections)
        if arch == "x64":
            detail = f"endbr64={data.count(_ENDBR64)}"
        else:
            detail = f"BTI={data.count(bytes((0x5F, 0x24, 0x03, 0xD5)))}"
        state = ("present" if note else
                 "ABSENT (Zig CRT objects unmarked; zig cc rejects "
                 "-z shstk and -z force-ibt)")
        print(f"  linux-{arch}: {detail}, .note.gnu.property={state}")


def check_cet_instrumentation(ctx, binary_path=None):
    """Verify -fcf-protection=full is actually effective on our own code.

    A hardening flag that silently stops applying is the failure mode this
    guards.  On Windows it walks the PE load-config Guard CF function table and
    confirms every address-taken function that came from Green Curve source
    begins with endbr64; vendor runtime functions are reported separately.  On
    Linux it checks our own compiled objects, because ELF has no Guard CF table.
    See llm-wiki/build.md for the measured baseline.
    """
    if sys.platform.startswith("linux") and binary_path is None:
        status = _check_elf_cet_objects(ctx)
        print("ELF CET check: what reaches the shipped images")
        _report_elf_linked_state(ctx)
        return status
    scratch = None
    if binary_path is None:
        binary_path, scratch = _build_unstripped_probe(ctx)
        if binary_path is None:
            ctx.cleanup_work_subdir(scratch)
            return 1
    try:
        return _analyze_cet(ctx, binary_path)
    finally:
        if scratch:
            ctx.cleanup_work_subdir(scratch)


def _analyze_cet(ctx, binary_path):
    if not os.path.exists(binary_path):
        print(f"CET check: binary not found: {binary_path}")
        return 1

    with open(binary_path, "rb") as handle:
        data = handle.read()
    parsed = _pe_sections(data)
    if parsed is None:
        print(f"CET check: {binary_path} is not a PE32+ image")
        return 1
    machine, imagebase, opt, sections = parsed
    if machine != 0x8664:
        print(f"CET check: skipped, not an x64 image (machine=0x{machine:04x})")
        return 0

    def rva_to_offset(rva):
        for _name, va, vsz, ptr, rsz in sections:
            if va <= rva < va + max(vsz, rsz):
                off = ptr + (rva - va)
                return off if off < len(data) else None
        return None

    lc_rva = struct.unpack_from("<II", data, opt + 112 + 10 * 8)[0]
    lc = rva_to_offset(lc_rva) if lc_rva else None
    if lc is None:
        print("CET check: no load config directory (CFG metadata absent)")
        return 1
    lc_size = struct.unpack_from("<I", data, lc)[0]
    if lc_size < 0x94:
        print(f"CET check: load config too small for a Guard CF table ({lc_size})")
        return 1
    table_va = struct.unpack_from("<Q", data, lc + 0x80)[0]
    table_count = struct.unpack_from("<Q", data, lc + 0x88)[0]
    guard_flags = struct.unpack_from("<I", data, lc + 0x90)[0]
    stride = 4 + ((guard_flags >> 28) & 0xF)
    if not table_va or not table_count:
        print("CET check: Guard CF function table is empty")
        return 1

    nm = ctx.LLVM_MINGW_NM
    symbols = []
    if os.path.exists(nm):
        proc = subprocess.run([nm, "--numeric-sort", "--defined-only", binary_path],
                              capture_output=True, text=True)
        for line in proc.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[1] in ("T", "t"):
                try:
                    symbols.append((int(parts[0], 16), parts[2]))
                except ValueError:
                    pass
        symbols.sort()
    addresses = [s[0] for s in symbols]

    def symbol_for(va):
        if not symbols:
            return "?"
        index = bisect.bisect_right(addresses, va) - 1
        return symbols[index][1] if index >= 0 else "?"

    base_off = rva_to_offset(table_va - imagebase)
    ours_ok, ours_missing, vendor_missing, total = 0, [], 0, 0
    for i in range(table_count):
        entry = base_off + i * stride
        if entry + 4 > len(data):
            break
        rva = struct.unpack_from("<I", data, entry)[0]
        func_off = rva_to_offset(rva)
        if func_off is None:
            continue
        total += 1
        instrumented = data[func_off:func_off + 4] == _ENDBR64
        name = symbol_for(imagebase + rva)
        if _VENDOR_RUNTIME.search(name):
            if not instrumented:
                vendor_missing += 1
        elif instrumented:
            ours_ok += 1
        else:
            ours_missing.append(name)

    text = next((s for s in sections if s[0] == ".text"), None)
    endbr_total = data[text[3]:text[3] + text[4]].count(_ENDBR64) if text else 0

    print(f"CET check: {os.path.relpath(binary_path, ctx.SCRIPT_DIR)}")
    print(f"  Guard CF targets           : {total}")
    print(f"  endbr64 in .text           : {endbr_total}")
    print(f"  our targets with endbr64   : {ours_ok}")
    print(f"  our targets WITHOUT endbr64: {len(ours_missing)}")
    print(f"  vendor runtime without endbr64 (expected, prebuilt): {vendor_missing}")
    if not symbols:
        print("  cannot verify: this image carries no symbol table, so a target "
              "cannot be attributed to our source rather than the vendor runtime.")
        print("  Release binaries are linked with -s. Run `python build.py "
              "--check-cet` with no argument to build and check an unstripped probe.")
        return 1
    if ours_missing:
        print("  FAILED: -fcf-protection=full is not effective on these functions:")
        for name in sorted(set(ours_missing))[:40]:
            print(f"    {name}")
        return 1
    if ours_ok == 0:
        print("  FAILED: no instrumented targets attributed to our code at all")
        return 1
    print("  OK: every address-taken function from our source carries endbr64")
    return 0
