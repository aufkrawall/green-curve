"""Crash-artifact source gates and Linux debug-symbol extraction.

Split out of build.py so the build script stays under its size ratchet; build.py
owns the CLI and passes its paths, tool locations and check helpers in through
`ctx`.  Nothing here imports build.py, so the dependency runs one way only —
the same contract security_gates/ui_gates/fan_gates/linux_gates follow.

Two subjects, one module, because they are two halves of the same guarantee:
a crash is only diagnosable if the artifact lands somewhere findable AND
something can resolve its addresses.  release_manifest.py sets the precedent for
a module owning both a build step and the guards that describe it.

`ctx` is any object exposing SOURCE_DIR, SCRIPT_DIR, DIST_DIR, ZIG_EXE and
LLVM_MINGW_DIR.
"""
import os
import re
import subprocess


def _p(ctx, name):
    return os.path.join(ctx.SOURCE_DIR, name)


# ---------------------------------------------------------------------------
# Source gates
# ---------------------------------------------------------------------------

def check_windows_crash_artifacts(ctx, require_text, forbid_text, require_order,
                                  require_text_count):
    """F-CRASH-1/2: where a Windows crash artifact goes, and which crashes make one.

    Two independent failure modes, neither visible after the fact:

      * A dump written to the current working directory is a dump nobody finds
        — %SystemRoot%\\System32 for the service, an arbitrary folder for a
        shell-launched GUI — and for the LocalSystem service it also leaves a
        SYSTEM-process dump outside the admin-only directory.
      * __fastfail and __stack_chk_fail bypass every exception handler Windows
        offers, so a CFG violation or a smashed stack — the two highest-signal
        crashes the program can have — used to leave nothing on disk at all.
    """
    crash_cpp = _p(ctx, "main_crash_artifacts.cpp")
    policy_h = _p(ctx, "crash_artifact_policy.h")
    hook_h = _p(ctx, "fatal_dump_hook.h")
    cfg_glue = _p(ctx, "cfg_glue.cpp")
    hardening = _p(ctx, "process_hardening.cpp")
    ssp_glue = _p(ctx, "ssp_glue.cpp")
    entry_cpp = _p(ctx, "entry.cpp")
    main_cpp = _p(ctx, "main.cpp")

    require_text(crash_cpp, "green_curve_unhandled_exception_filter", "crash filter exists")
    require_text(crash_cpp, "MiniDumpWriteDump", "crash filter writes minidump")
    require_text(crash_cpp, "MiniDumpWithThreadInfo",
                 "crash dumps retain actionable thread records")
    require_text(crash_cpp, "MiniDumpWithUnloadedModules",
                 "crash dumps retain unloaded-module history")
    require_text(crash_cpp, "ContextRecord->Pc",
                 "VEH thread-exit redirect supports arm64 (Pc/X0/Sp)")
    require_order(main_cpp,
                  "install_crash_handlers(false);",
                  "service_try_dispatch_controlled_restart_helper(&helperExitCode)",
                  "standalone recovery helper installs crash dumping before dispatch")

    # F-CRASH-1: fail closed on location.
    require_text(crash_cpp, "gc_crash_dir_source(",
                 "the crash artifact directory comes from the shared fail-closed policy")
    forbid_text(crash_cpp, 'StringCchCopyA(out, outSize, ".")',
                "crash artifacts never fall back to the current working directory")
    require_text(policy_h, "GC_CRASH_DIR_NONE",
                 "an unresolvable artifact directory is a real outcome, not a fallback")
    require_text(policy_h, "gc_crash_artifact_is_older",
                 "rotation orders by the embedded timestamp, not the filename")
    # One writer for every crash path: three near-identical MiniDumpWriteDump
    # blocks are how the VEH copy drifted onto a different directory rule.
    require_text_count(crash_cpp, "MiniDumpWriteDump(", 1,
                       "every crash path shares one minidump writer")

    # F-CRASH-2: report before the uncatchable instruction.
    require_text(hook_h, "gc_invoke_fatal_dump_hook",
                 "the fast-fail reporting seam exists")
    require_order(cfg_glue,
                  "gc_invoke_fatal_dump_hook(GC_FATAL_CFG_VIOLATION",
                  "FAST_FAIL_GUARD_ICALL_OR_TARGET_FAILURE)",
                  "a CFG violation is reported before the uncatchable fast-fail")
    require_order(ssp_glue,
                  "gc_invoke_fatal_dump_hook(GC_FATAL_STACK_SMASH",
                  "__debugbreak();",
                  "a smashed stack is reported without depending on breakpoint dispatch")
    # The one-shot reporter lives in process_hardening.cpp: the toolchain-
    # neutral half of the former cfg_glue.cpp, linked by BOTH toolchains.
    require_text(hardening, "InterlockedCompareExchange(&g_gcFatalDumpReported, 1, 0)",
                 "the fatal reporter is one-shot so the CFG validator cannot recurse")
    require_text(crash_cpp, "green_curve_report_fatal_dump",
                 "the fatal reporter synthesises a record from the live register context")
    require_text(crash_cpp, "RtlCaptureContext(&context)",
                 "the synthesised dump carries a real thread context for .ecxr")

    # F-REL-2: every artifact kind is bounded, in every process that writes one.
    # The GUI used to write uncapped dumps into the user directory, and only the
    # VEH prefix was ever swept.
    require_text(crash_cpp, "rotate_crash_artifacts_for_process",
                 "crash artifacts are rotated to bound disk usage")
    require_text(crash_cpp, "GC_VEH_DUMP_PREFIX, GC_CRASH_ARTIFACT_MAX_KEEP",
                 "recovered VEH dumps are rotated against their own budget")
    require_text(crash_cpp, "GC_CRASH_DUMP_PREFIX, GC_CRASH_ARTIFACT_MAX_KEEP",
                 "terminal crash dumps are rotated against their own budget")
    require_text(crash_cpp, "rotate_crash_breadcrumb_in_dir",
                 "the append-only crash breadcrumb is capped")
    require_text(entry_cpp, "rotate_crash_artifacts_for_process();",
                 "GUI startup rotates its own crash artifacts")


# ---------------------------------------------------------------------------
# Linux debug symbols
# ---------------------------------------------------------------------------

def linux_symbol_output_path(ctx, output_path, arch):
    """Same rule as Windows: private DWARF stays outside the release payload.

    Linux shipped stripped binaries with NO extracted symbols at all until this
    existed, which made a systemd-coredump core — and the PC in the crash
    report — unsymbolizable.  The addresses were correct and useless.
    """
    name = os.path.basename(output_path)
    if name.endswith(".new"):
        name = name[:-4]
    stem = os.path.splitext(name)[0]
    return os.path.join(ctx.DIST_DIR, "symbols", f"linux-{arch}", stem + ".debug")


def extract_linux_symbols(ctx, binary_path, arch):
    """Split DWARF out of a Linux binary, then strip the binary itself.

    Leaves three things in a consistent state:
      * dist/symbols/linux-<arch>/<name>.debug — the private DWARF.
      * a .gnu_debuglink in the shipped binary naming that file, so a debugger
        sitting next to it finds symbols with no arguments.
      * a fully stripped shipped binary, the same shape the old bare "-s"
        produced, so nothing about the release payload changes.

    `zig objcopy` is used rather than llvm-objcopy because Zig is already the
    Linux compiler; requiring llvm-mingw here would make the Linux build depend
    on the Windows toolchain.  A failure is fatal: shipping a stripped binary
    with no recoverable symbols is the exact regression this exists to prevent,
    so it must not degrade quietly.
    """
    symbol_path = linux_symbol_output_path(ctx, binary_path, arch)
    os.makedirs(os.path.dirname(symbol_path), exist_ok=True)
    if os.path.exists(symbol_path):
        os.remove(symbol_path)
    # objcopy must not read and write the same path: it opens the output for
    # writing first, which truncates the input out from under itself and fails
    # with TRUNCATED_ELF.  Strip into a sibling and swap it in afterwards.
    stripped_path = binary_path + ".stripped"
    if os.path.exists(stripped_path):
        os.remove(stripped_path)
    result = subprocess.run(
        [ctx.ZIG_EXE, "objcopy", "--strip-all", "--extract-to", symbol_path,
         binary_path, stripped_path],
        cwd=ctx.SCRIPT_DIR, text=True, capture_output=True)
    if result.returncode != 0:
        if os.path.exists(stripped_path):
            os.remove(stripped_path)
        raise RuntimeError(
            "Linux symbol extraction failed: "
            f"{(result.stderr or result.stdout or '').strip()}")
    os.replace(stripped_path, binary_path)
    if not os.path.isfile(symbol_path) or os.path.getsize(symbol_path) < 4096:
        raise RuntimeError(f"extracted Linux symbols are missing or empty: {symbol_path}")
    _verify_linux_private_symbols(ctx, symbol_path, binary_path, arch)


def _verify_linux_private_symbols(ctx, symbol_path, binary_path, arch):
    """Require the symbol file to be structurally usable and actually matched.

    Mirrors verify_windows_private_symbols.  The build-id equality check is the
    part that matters operationally: a .debug file whose build-id differs from
    the shipped binary's is one coredumpctl and gdb silently refuse to use,
    which looks identical to having no symbols at all.
    """
    readobj = ctx.LLVM_MINGW_READOBJ
    if not os.path.isfile(readobj):
        # The Linux build must not hard-depend on the Windows toolchain being
        # present; the extraction and size checks above already ran.
        print(f"  Extracted private symbols: {symbol_path} "
              f"({os.path.getsize(symbol_path):,} bytes, build-id unverified)")
        return
    ids = []
    for path in (binary_path, symbol_path):
        result = subprocess.run([readobj, "--notes", path], text=True, capture_output=True)
        if result.returncode != 0:
            raise RuntimeError(f"private Linux debug file failed structural verification: {path}")
        match = re.search(r"Build ID:\s*([0-9a-fA-F]+)", result.stdout or "")
        if not match:
            raise RuntimeError(
                f"no GNU build-id found in {path}; a core cannot be matched to symbols")
        ids.append(match.group(1).lower())
    if ids[0] != ids[1]:
        raise RuntimeError(
            f"Linux {arch} symbol file does not match the shipped binary "
            f"(binary build-id {ids[0]}, symbols {ids[1]})")
    print(f"  Verified private symbols: {symbol_path} "
          f"({os.path.getsize(symbol_path):,} bytes, build-id {ids[0]})")


def check_linux_symbols(ctx, require_text):
    """F-LNX-SYMBOLS: a stripped Linux binary must still be symbolizable.

    LINUX_FLAGS used to carry a bare "-s" with no extraction step, so the
    shipped binary had neither .symtab nor DWARF and nothing anywhere matched
    it.  Debug info is now emitted, split out post-link, and verified to carry
    the same build-id as the binary that ships.
    """
    build_script = os.path.join(ctx.SCRIPT_DIR, "build.py")
    module = os.path.join(ctx.SCRIPT_DIR, "tools", "crash_artifacts.py")
    require_text(build_script, "-Wl,--build-id=sha1",
                 "Linux binaries carry a build-id so a core can be matched to symbols")
    require_text(build_script, "crash_artifacts.extract_linux_symbols(",
                 "Linux debug info is split out instead of discarded at link time")
    require_text(module, "def linux_symbol_output_path",
                 "Linux symbols land outside the release payload")
    require_text(module, "extracted Linux symbols are missing or empty",
                 "an empty Linux symbol file fails the build instead of shipping")
    require_text(module, "does not match the shipped binary",
                 "a build-id mismatch between binary and symbols fails the build")
    require_text(module, 'f"linux-{arch}"',
                 "Linux symbols are separated per architecture")
