// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// fatal_dump_hook.h — the one seam that lets a fast-fail crash still produce a
// dump.
//
// Windows dispatches most crashes through SetUnhandledExceptionFilter, which
// main_crash_artifacts.cpp uses to write a minidump next to config.ini.  Two
// classes of fatal failure deliberately BYPASS that path:
//
//   * __fastfail / int 0x29 (cfg_glue.cpp's Control Flow Guard violation).  By
//     design it hands control straight to the kernel — no vectored handler, no
//     unhandled-exception filter, no SEH unwind.  Nothing we install can catch
//     it after the fact.
//   * __stack_chk_fail (ssp_glue.cpp).  It happened to reach the filter only
//     because __debugbreak() raises a catchable STATUS_BREAKPOINT first; that
//     is incidental, and it stops working the moment a debugger is attached.
//
// Both are the highest-signal crashes the program can have — a smashed stack or
// a corrupted call target — and both used to leave nothing behind but a Windows
// Error Reporting event this project does not collect.  So they now report
// BEFORE they die, through this hook.
//
// It is a function POINTER, defaulting to null, and not a direct call, because
// ssp_glue.cpp and cfg_glue.cpp are also linked into the setup program, which
// contains none of the diagnostics code.  A null hook means "no dump", which is
// exactly the installer's previous behaviour.

#ifndef GREEN_CURVE_FATAL_DUMP_HOOK_H
#define GREEN_CURVE_FATAL_DUMP_HOOK_H

// Application-defined exception codes for failures that never produced a real
// EXCEPTION_RECORD.  Severity 0b11 (error) + the customer bit, so they can
// never collide with a system status value.
#define GC_FATAL_CFG_VIOLATION   0xE0C50001UL
#define GC_FATAL_STACK_SMASH     0xE0C50002UL

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*GcFatalDumpHook)(unsigned long reason, const char* label);

// Install the reporter.  Called once per process from every entry point that
// also installs the unhandled-exception filter.
void gc_set_fatal_dump_hook(GcFatalDumpHook hook);

// Report a fatal failure that is about to terminate the process.
//
// One-shot: the first caller wins and every later or re-entrant call returns
// immediately.  Re-entrancy is not theoretical here — __guard_check_icall_fptr
// runs before EVERY indirect call in the program, including the ones the hook
// itself makes, so an unguarded call would recurse until the stack ran out and
// replace the crash we wanted to record with one we caused.
void gc_invoke_fatal_dump_hook(unsigned long reason, const char* label);

#ifdef __cplusplus
}
#endif

#endif // GREEN_CURVE_FATAL_DUMP_HOOK_H
