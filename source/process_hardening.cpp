// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Process hardening shared by EVERY Windows binary this project produces,
// under both supported toolchains (llvm-mingw and clang-cl/MSVC ABI).
//
// Two pieces live here rather than in the diagnostics code because they are
// linked into every binary, including the setup program, which has no
// diagnostics at all.  A null hook keeps the installer behaving exactly as
// before.
//
// This file was split out of cfg_glue.cpp when the MSVC-ABI (clang-cl)
// toolchain was introduced: under that toolchain Control Flow Guard is
// enforced by the OS through the PE load config, so the MinGW
// __guard_check_icall_fptr shim that used to live beside this code has no
// reason to exist — but the fatal-dump hook and the mitigation policies do.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// SetProcessMitigationPolicy requires _WIN32_WINNT >= 0x0602 (Windows 8).
// This is safe — we target Windows 10+ at runtime.
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#include <windows.h>

#include "fatal_dump_hook.h"

extern "C" {

// ---------------------------------------------------------------------------
// Fatal-dump hook (see fatal_dump_hook.h)
// ---------------------------------------------------------------------------

static GcFatalDumpHook g_gcFatalDumpHook = nullptr;
static volatile LONG g_gcFatalDumpReported = 0;

void gc_set_fatal_dump_hook(GcFatalDumpHook hook) {
    g_gcFatalDumpHook = hook;
}

void gc_invoke_fatal_dump_hook(unsigned long reason, const char* label) {
    GcFatalDumpHook hook = g_gcFatalDumpHook;
    if (!hook) return;
    // One-shot.  Guards two distinct hazards: a second thread crashing while the
    // first is still writing its dump (which would interleave two dumps into one
    // file), and the unbounded recursion described in fatal_dump_hook.h, where
    // the hook's own indirect calls re-enter the CFG validator.
    if (InterlockedCompareExchange(&g_gcFatalDumpReported, 1, 0) != 0) return;
    hook(reason, label);
}

// ---------------------------------------------------------------------------
// Set process-wide mitigation policies that cannot be configured via linker
// flags.  Must be called early in process startup, after CRT init is complete.
// Typically invoked from both the GUI and service entry points.
// ---------------------------------------------------------------------------
extern "C" void initialize_process_mitigations() {
    // Harden the DLL search path FIRST, before this process issues any
    // LoadLibrary call.  Restrict the default search to System32 (plus any
    // explicitly AddDllDirectory-registered user dirs, of which we register
    // none) and drop the current working directory.  This blocks DLL planting
    // (e.g. a rogue dbghelp.dll / version.dll dropped next to the binary) from
    // hijacking the process — critical for the LocalSystem service.
    //
    // NVIDIA's nvml.dll / nvapi64.dll and their dependencies live in System32,
    // and our own helper loads use absolute System32 paths (load_system_library_a),
    // so this does not change which DLLs we resolve — it only removes the unsafe
    // application-directory and CWD search entries.
    //
    // Statically-imported DLLs are bound by the loader BEFORE this runs, so the
    // install-directory ACL (ensure_secure_service_binary_path) is the primary
    // defense for those; this call protects every runtime LoadLibrary.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
    SetDllDirectoryW(L"");

    // The application has no JIT or in-process generator, so writable+executable
    // memory has no legitimate use.  Extension-point DLLs are likewise not part
    // of any supported plugin model.  These policies apply before NVIDIA runtime
    // libraries load and do not restrict the separate child installer/update
    // processes.
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dynamicCode = {};
    dynamicCode.ProhibitDynamicCode = 1;
    if (!SetProcessMitigationPolicy(ProcessDynamicCodePolicy, &dynamicCode,
                                    sizeof(dynamicCode))) {
        OutputDebugStringA("[GreenCurve] dynamic-code mitigation was refused\n");
    }

    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY extensionPoints = {};
    extensionPoints.DisableExtensionPoints = 1;
    if (!SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy,
                                    &extensionPoints,
                                    sizeof(extensionPoints))) {
        OutputDebugStringA("[GreenCurve] extension-point mitigation was refused\n");
    }

    // Enable strict handle checking: any use of an invalid handle
    // (double-close, use-after-close, bogus value) raises an exception
    // instead of silently succeeding with unpredictable behavior.
    // This catches handle bugs early during development and testing.
    PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY strictHandle = {};
    strictHandle.RaiseExceptionOnInvalidHandleReference = 1;
    strictHandle.HandleExceptionsPermanentlyEnabled = 1;
    SetProcessMitigationPolicy(
        ProcessStrictHandleCheckPolicy, &strictHandle, sizeof(strictHandle));
}

}
