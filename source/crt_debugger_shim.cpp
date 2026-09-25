// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// MSVC-ABI-only link shim against the antivirus heuristics of the statically
// linked UCRT: its invalid-parameter/abort fault handler
// (__acrt_call_reportfault, invalid_parameter.cpp:206) calls
// kernel32!IsDebuggerPresent to decide whether to break into an attached
// debugger. Green Curve never checks for a debugger, so that import is pure
// classifier surface: an anti-debug API import with zero references in this
// project's sources. Defining the IAT slot __imp_IsDebuggerPresent here
// satisfies the UCRT's reference before the kernel32 import library is
// scanned, which keeps the import out of every clang-cl image (proven by the
// all-variant import gate in tools/pe_verify.py).
//
// Runtime behavior: invalid-parameter and abort faults always take their
// no-debugger route (Watson/fast-fail/terminate). __fastfail and /GS cookie
// failure never consult this API, and the project's own crash-dump machinery
// does not either. A debugger-attached process loses only the CRT's
// message-to-debugger step on those CRT fault paths.
//
// The llvm-mingw build never imports the API and must not link this file.
//
// ABI note: the UCRT calls it as BOOL IsDebuggerPresent(void); returning int 0
// is the same EAX/W0 register contract without pulling windows.h in here.

extern "C" int gc_crt_reports_no_debugger(void) {
    return 0;
}

// Reserved spelling on purpose: this is the exact IAT symbol the linker fills
// for a dllimport call, and a strong object definition wins before any import
// library is consulted.
extern "C" void* const __imp_IsDebuggerPresent =
    reinterpret_cast<void*>(&gc_crt_reports_no_debugger);
