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
    # F-VEH-CET: the driver-fault VEH ends the faulting thread with a direct
    # ExitThread call.  Rewriting the context's instruction pointer and
    # continuing is refused under the /cetcompat shadow stacks and fast-fails
    # the whole service (0xC0000409), which turned every driver update into a
    # non-replaying crash restart.  tests/veh_driver_fault_tests.cpp proves the
    # flow in a child with shadow stacks forced on.
    veh_h = _p(ctx, "veh_driver_fault_win32.h")
    require_text(veh_h, "ExitThread(GC_VEH_DRIVER_FAULT_THREAD_EXIT_CODE);",
                 "the driver-fault flow ends the faulting thread directly")
    require_text(crash_cpp, "return gc_veh_handle_driver_fault(info, &kGreenCurveVehDriverFault);",
                 "the service VEH runs the shared, fixture-tested driver-fault flow")
    for path in (crash_cpp, veh_h):
        for needle in ("ContextRecord->Rip", "ContextRecord->Pc", "EXCEPTION_CONTINUE_EXECUTION"):
            forbid_text(path, needle,
                        "no VEH continues at a rewritten instruction pointer (CET refuses it)")
    # Ending the service main thread (the watchdog) or the SCM dispatcher
    # would leave a hung, never-restarted service; their faults crash instead.
    require_text(veh_h, "if (handler->mayEndCurrentThread && !handler->mayEndCurrentThread(handler->context))",
                 "the driver-fault flow asks before ending the faulting thread")
    require_text(crash_cpp, "return threadId != g_serviceMainThreadId && threadId != g_serviceDispatcherThreadId;",
                 "the service VEH never ends its main or dispatcher thread")
    require_order(main_cpp, "g_serviceDispatcherThreadId = GetCurrentThreadId();",
                  "StartServiceCtrlDispatcherW(table);",
                  "the dispatcher thread is recorded before it can dispatch")
    # A poisoned runtime with nothing to restore restarts plainly; latching the
    # lockout there blocked the next logon's profile.
    restart_cpp = _p(ctx, "main_service_controlled_restart.cpp")
    recovery_cpp = _p(ctx, "main_service_recovery.cpp")
    require_text(restart_cpp, "service_poisoned_runtime_plan_or_exit(reason, corroboratedDriverFailure);",
                 "the poisoned-runtime exit follows the shared, tested plan")
    require_text(recovery_cpp, "service_poisoned_runtime_exit_plan(",
                 "the plan comes from the policy header")
    require_text(recovery_cpp, "service_exit_poisoned_runtime_for_ordinary_restart(reason);",
                 "a poisoned runtime without intent exits for an ordinary restart")
    # A write in flight has committed only its ownership marker; that is
    # evidence of a failed apply, not "nothing to restore".
    require_text(recovery_cpp, "corroboratedDriverFailure, snapshot, liveIntent, ownedState);",
                 "the poisoned-runtime plan weighs GPU state this process owns")
    # The helper may wait while a newer generation runs; its cleanup removes
    # only files still bound to its own nonce.
    require_text_count(restart_cpp, "service_clear_controlled_recovery_files_if_owned(nonceHex);", 4,
                       "every recovery-helper failure path clears only its own generation's files")

    # A driver update stops/starts the selected GPU without removing it; the
    # exact-device arrival decides through the shared, tested policy.
    coordinator_cpp = _p(ctx, "main_service_logon_coordinator.cpp")
    require_text(coordinator_cpp, "service_selected_gpu_arrival_action(wasRemoved, hasIntent, recoveryPending);",
                 "the selected-GPU arrival follows the shared arrival policy")
    forbid_text(coordinator_cpp, "selected-GPU arrival without a corroborated removal; diagnostic/readiness only",
                "a started-only driver restart with applied intent is not ignored")

    # A verified driver update waives the awake-time window (it is not
    # stability evidence); the stamp records the driver the apply ran under.
    clock_cpp = _p(ctx, "main_service_recovery_clock.cpp")
    require_text(clock_cpp, "service_get_module_version(g_app.hNvApi, &driverMs, &driverLs)",
                 "the proof stamp records the driver the settings were applied under")
    require_text(clock_cpp, "return service_driver_recovery_proof_accepts(ageMs, SERVICE_OC_STABILIZATION_WINDOW_MS, updated);",
                 "the driver-recovery proof gate uses the shared waiver policy")
    # The on-disk side must be the image the loader maps (nvapia64.dll on
    # ARM64), never a fixed name the native ARM64 driver does not use.
    identity_cpp = _p(ctx, "main_service_driver_version.cpp")
    require_text(identity_cpp, "nvapi_module_first(nvapi_module_host_arch(),",
                 "the proof's disk image follows the loader's module walk")
    require_text(_p(ctx, "nvapi_loader.cpp"), "nvapi_module_first(nvapi_module_host_arch(),",
                 "the loader takes the same module walk as the proof")
    forbid_text(identity_cpp, "\\\\nvapi64.dll\"",
                "no fixed NvAPI image name in the driver-version proof")
    # "In-process" means the mapped image: after an in-place update the
    # module's load path names the new file.
    require_text(identity_cpp, "return gc_module_mapped_version(mod, outMs, outLs);",
                 "the in-process driver version is read from the mapped image")

    # The SCM failure-action start (2 s after the controlled exit) yields to a
    # live recovery helper before the startup gate can clear its authorization.
    host_cpp = _p(ctx, "main_service_host.cpp")
    require_order(host_cpp, "service_yield_to_controlled_recovery_helper_if_pending(argc, argv);",
                  "service_prepare_controlled_recovery_startup(argc, argv);",
                  "the failure-action yield runs before the startup gate clears recovery state")
    require_text(_p(ctx, "main_service_recovery.cpp"),
                 "service_ordinary_start_yields_to_recovery_helper(",
                 "the yield follows the shared, tested policy")

    # The user-facing crash-dump notice: scanned with the service's rights,
    # written under the user's token, and the dumps themselves never leave the
    # admin-only directory (a SYSTEM dump can hold other accounts' data).
    paths_cpp = _p(ctx, "main_data_paths.cpp")
    notice_cpp = _p(ctx, "main_service_crash_notice.cpp")
    require_order(paths_cpp, "service_compose_crash_dump_notice(",
                  "GcDebugLogIdentity userIdentity(",
                  "the dump folders are scanned before the user's token takes over")
    require_order(paths_cpp, "debug_log_set_route_path(g_debugLogPath, hToken);",
                  "service_store_crash_dump_notice_as_user(",
                  "the notice is written under the user's token, logged to the user's route")
    require_text(notice_cpp, "GcDebugLogIdentity self(nullptr, false);",
                 "the scan runs as the service even inside a client impersonation")
    for needle in ("CopyFile", "MoveFile", "write_text_file_atomic"):
        forbid_text(notice_cpp, needle,
                    "the notice never copies dumps or writes with the service's own rights")
    # The notice hands an administrator a command built from folder names the
    # user controls: every path goes in as a single-quoted literal.
    notice_policy_h = _p(ctx, "crash_dump_notice_policy.h")
    require_text_count(notice_policy_h, "gc_powershell_append_literal(out, outSize, used,", 3,
                       "every path in the elevated command is a PowerShell literal")
    forbid_text(notice_policy_h, "= \\\"%s\\\"",
                "no path is formatted into an expandable PowerShell string")
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

    # A fast-fail raised OUTSIDE our code (ntdll CFG dispatch, a CET
    # shadow-stack mismatch, heap-corruption detection, /GS in a driver DLL)
    # reaches none of the paths above; only a WER LocalDumps key keeps a dump.
    # Registered by every service install door, removed by removal and by a
    # rolled-back first install, and never touching an administrator's own key.
    install_cpp = _p(ctx, "main_service_install.cpp")
    wer_policy_h = _p(ctx, "wer_local_dumps_policy.h")
    require_order(install_cpp, "service_configure_description(svc.get());",
                  "service_register_wer_local_dumps();",
                  "the WER backstop is registered before the first service start")
    require_order(install_cpp, "service_register_wer_local_dumps();",
                  "if (!StartServiceW(svc.get(), 1, startArgs)) {",
                  "the WER backstop is registered before the first service start")
    require_text_count(install_cpp, "service_unregister_wer_local_dumps();", 2,
                       "both service-removal exits remove the WER backstop")
    require_text(_p(ctx, "installer_transaction.cpp"), "gc_wer_unregister_all(wer);",
                 "a rolled-back first install removes the WER backstop")
    require_text(wer_policy_h, "return state == GC_WER_KEY_OURS ? GC_WER_ACTION_DELETE : GC_WER_ACTION_LEAVE;",
                 "only a key carrying our marker is ever deleted")
    forbid_text(wer_policy_h, "ProgramData",
                "SYSTEM dumps never go to a world-readable folder")


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
