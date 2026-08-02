// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Stack-smashing protection (SSP) runtime support for Zig/MinGW on Windows.
//
// The compiler emits canary prologue/epilogue code (-fstack-protector-strong)
// referencing __stack_chk_guard and __stack_chk_fail.  The MinGW CRT does
// not provide these symbols on Windows, so the canary code gets dead-code-
// eliminated at link time.  This small glue file supplies both symbols,
// enabling stack buffer overflow detection in the final binary.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdlib>

#include "fatal_dump_hook.h"

extern "C" {

// Canary value: a reasonably unpredictable pointer-sized constant.
// Using the address of the guard variable itself gives us ASLR-derived
// entropy without relying on RDTSC or OS entropy at startup.
// The "used" attribute prevents LTO from eliminating the symbols even when
// no function in the current translation unit triggers a canary check.
__attribute__((used)) unsigned long long __stack_chk_guard = (unsigned long long)&__stack_chk_guard;

__attribute__((used)) __attribute__((noreturn)) void __stack_chk_fail(void) {
    OutputDebugStringA("*** STACK SMASHING DETECTED: greencurve ***\n");
    // Write the dump explicitly rather than relying on __debugbreak() below
    // being caught by the unhandled-exception filter.  That did work — an
    // unhandled STATUS_BREAKPOINT reaches the filter — but only incidentally,
    // and not at all when a debugger is attached (it swallows the breakpoint
    // and the process then dies in abort() with nothing written).  A smashed
    // stack is the single most valuable dump this program can produce, so it
    // must not depend on which handler happens to see a breakpoint first.
    gc_invoke_fatal_dump_hook(GC_FATAL_STACK_SMASH,
        "stack protector: canary mismatch on function return");
    __debugbreak();
    abort();
}

}
