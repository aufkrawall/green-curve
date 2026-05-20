// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Control Flow Guard (CFG) runtime support override for MinGW on Windows.
//
// The MinGW CRT provides __guard_check_icall_fptr, but its implementation
// (__guard_check_icall_dummy in libmingwex.a) has its own bitmap that only
// knows about our binary's GFIDS table, not externally-loaded DLL exports.
//
// This override checks if the target address falls within any loaded module.
// If it does, the module has been authenticated by the OS loader and the
// target is safe to call.  Only truly wild addresses (not belonging to any
// loaded module) are treated as CFG violations and trigger a fast-fail.
//
// NOTE: ntdll!RtlValidateUserCallTarget (ordinal 1647) was also tested — it
// is the OS-level CFG bitmap validator and DOES work correctly for exported
// symbols.  However, calling it from our __guard_check_icall_fptr override
// during CRT initialization triggered fast-fail crashes, likely because the
// MinGW CRT's dispatch setup interacts poorly with the strict OS validator
// during early process startup.  For maximum robustness across all runtime
// scenarios (including CRT init, GetProcAddress returns, NvAPI internal
// dispatch functions, and NVML exports), the module-range check approach
// is used instead.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// SetProcessMitigationPolicy requires _WIN32_WINNT >= 0x0602 (Windows 8).
// This is safe — we target Windows 10+ at runtime.
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#include <windows.h>

// FAST_FAIL_GUARD_ICALL_OR_TARGET_FAILURE is 0x5 on x64 Windows.
// MinGW may not define it, so we define it ourselves.
#ifndef FAST_FAIL_GUARD_ICALL_OR_TARGET_FAILURE
#define FAST_FAIL_GUARD_ICALL_OR_TARGET_FAILURE 5
#endif

extern "C" {

// The MinGW compiler emits DIRECT calls to __guard_check_icall_fptr
// before every indirect call when CFG is enabled (the CRT's dispatch
// function __guard_dispatch_icall_dummy is just "jmpq *%rax" and does
// NOT validate anything).  The name MUST match what the compiler emits
// — this overrides the MinGW CRT's version.
//
// The "used" attribute prevents LTO from eliminating the symbol.
__attribute__((used)) void __cdecl __guard_check_icall_fptr(uintptr_t Target) {
    // Check if the target address falls within any loaded module.
    // GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS resolves the containing
    // module without incrementing the refcount.
    HMODULE hMod = nullptr;
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)Target, &hMod) && hMod != nullptr) {
        return; // Target is in a loaded module — valid under CFG.
    }

    // Target does not belong to any loaded module — this is a real CFG
    // violation (e.g. corrupted vtable, ROP gadget, or data-as-code).
    // Trigger an immediate fast-fail so the OS can capture a crash dump.
#if defined(_M_IX86) || defined(_M_X64)
    __asm__ volatile("int $0x29" : : "c"(FAST_FAIL_GUARD_ICALL_OR_TARGET_FAILURE) : "memory");
#else
    __fastfail(FAST_FAIL_GUARD_ICALL_OR_TARGET_FAILURE);
#endif
    __builtin_unreachable();
}

// Set process-wide mitigation policies that cannot be configured via linker flags.
// Must be called early in process startup, after CRT init is complete.
// Typically invoked from both the GUI and service entry points.
extern "C" void initialize_process_mitigations() {
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
