// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_crash_report.h — startup half of the Linux crash report.
//
// linux_crash_breadcrumb.h owns everything that runs inside the signal handler
// and is therefore restricted to write(2)/read(2)/open(2).  This is the other
// half: it runs at startup, where formatting a path, reading a directory and
// deleting an old file are all allowed, and it hands the handler a path that is
// ready to open.
//
// The split is exactly at the async-signal-safety line and nowhere else.  In
// particular the file itself is NOT created here — the handler opens it on the
// first byte it writes, so a run that does not crash leaves no artifact at all.
// See linux_crash_breadcrumb.h for why the startup open had to go.

#ifndef GREEN_CURVE_LINUX_CRASH_REPORT_H
#define GREEN_CURVE_LINUX_CRASH_REPORT_H

#include <stdbool.h>

// Resolve the report directory, rotate old reports and format the path this
// process would report to.  Creates nothing on disk.
//
// `configPath` is the resolved config.ini path — its directory is where the
// report goes, which on Linux is the binary's own folder by default
// (default_linux_config_path).  `daemonRole` switches to the daemon state
// directory, because systemd mounts /usr read-only for the unit.
//
// Best-effort by design: a directory that cannot be resolved simply means no
// report file, and the stderr/debug-log copies still carry the breadcrumb.
// Returns true when a path was resolved.  Whether that path can actually be
// written is settled at crash time by the open, not guessed at here.
//
// This does NOT arm any signal handler by itself.  The caller publishes the
// result with linux_set_crash_report_path(linux_crash_report_path()); unlike
// the per-TU debug-log descriptor, one such call arms the whole program (see
// gc_crash_report_path_slot()), so linux_main.cpp does it and linux_daemon.cpp
// inherits it.
bool linux_crash_report_configure(const char* configPath, bool daemonRole);

// The path this process would write a crash report to, or "" when none was
// resolved.  Logged at startup so a user can be told where to look before they
// need it — the file itself will not exist until something goes wrong.
const char* linux_crash_report_path();

// Detach the report path and release the descriptors held for it.
//
// Correctness does not depend on this being called: no exit path can leave a
// stray artifact behind, because a file only ever exists once a crash has
// written to it.  It exists so linux_crash_report_configure() can re-arm onto a
// different directory cleanly.
void linux_crash_report_close();

#endif // GREEN_CURVE_LINUX_CRASH_REPORT_H
