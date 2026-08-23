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
FUZZ_LINUX_EXTRA_SOURCES = {
    "config_strings": ("config_text_utils.cpp", "app_shared.cpp",
                       "fan_curve.cpp", "platform_posix.cpp"),
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
            result = subprocess.run(cmd, cwd=ctx.SCRIPT_DIR)
            if result.returncode != 0:
                print(f"Fuzz target {name} FAILED to compile")
                sys.exit(result.returncode)

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
    """Fail if a tracked text file hardcodes somebody's real Windows profile.

    A wiki entry once recorded a developer's real profile directory under the
    Windows Users folder, which is exactly the private-user-data leak the
    project rules forbid and is permanent once pushed.  Placeholders, single
    letter fixtures and synthetic test accounts are fine; anything else is
    assumed to be a real account name.

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
            for match in _PROFILE_PATH_RE.finditer(line):
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
              "Windows user profile path (use %USERPROFILE% or a placeholder):")
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
        build_script = os.path.join(ctx.SCRIPT_DIR, "build.py")
        with open(build_script, "r", encoding="utf-8", errors="replace") as handle:
            build_script_text = handle.read()
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
    finally:
        ctx.cleanup_work_subdir(tmp)
    check_linux_release_packaging(ctx)


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
