// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// crash_artifacts_forward.h — declarations for main_crash_artifacts.cpp.
//
// The Windows binary is one amalgamated translation unit, and the crash
// handlers are used by shards (main_service_host.cpp, entry.cpp) that the
// aggregator includes BEFORE the file defining them.  Same role as
// gui_render_forward.h / gui_mutation_forward.h: keep the forward block out of
// main.cpp, which sits at the source-size guideline.
//
// Windows-only; included after <windows.h>.

#ifndef GREEN_CURVE_CRASH_ARTIFACTS_FORWARD_H
#define GREEN_CURVE_CRASH_ARTIFACTS_FORWARD_H

// Terminal crashes: writes the breadcrumb + minidump, then lets the process die.
static LONG WINAPI green_curve_unhandled_exception_filter(EXCEPTION_POINTERS* info);

// Service-only: survives an nvml/nvapi stale-handle access violation by killing
// the faulting thread, after dumping it.  See main_crash_artifacts.cpp.
static LONG CALLBACK green_curve_vectored_handler(EXCEPTION_POINTERS* info);

// Installs the unhandled-exception filter and the fast-fail reporter, plus the
// vectored handler when asked.  One entry point so the GUI, the service and the
// restart helper cannot drift apart on which handlers they have.
static void install_crash_handlers(bool installVectoredNvmlRecovery);

// One rotation pass over this process's artifact directory, bounding each
// artifact kind.  Call once per fresh process, after path resolution.
static void rotate_crash_artifacts_for_process();

#endif // GREEN_CURVE_CRASH_ARTIFACTS_FORWARD_H
