// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_debug_log.h — file-backed debug log for every Linux role.
//
// Windows has had `greencurve_debug.txt` next to the executable since the
// beginning (see main_diagnostics.cpp); Linux had nothing but stderr, which is
// invisible for a desktop-launched TUI and mixed into the journal for the
// daemon.  This gives the Linux binary the same durable log, gated by the same
// `[debug] enabled` config key and the same GREEN_CURVE_DEBUG environment
// override, so a user report can be diagnosed from a file instead of a
// screenshot.

#ifndef GREEN_CURVE_LINUX_DEBUG_LOG_H
#define GREEN_CURVE_LINUX_DEBUG_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "platform.h"

// File name written next to the resolved config (client roles) or into the
// daemon state directory.  Deliberately identical to the Windows name.
#define LINUX_DEBUG_LOG_FILE_NAME "greencurve_debug.txt"
#define LINUX_DEBUG_LOG_ENV       "GREEN_CURVE_DEBUG"
// Opt-out, matching APP_DEBUG_DEFAULT_ENABLED on Windows: the log is the first
// thing asked for in a bug report, so it must exist without being turned on.
#define LINUX_DEBUG_DEFAULT_ENABLED 1
// Rotated at this size so an always-on log cannot grow without bound; one
// previous generation is kept as <name>.1.
#define LINUX_DEBUG_LOG_MAX_BYTES (4 * 1024 * 1024)

// Resolve the debug log path for a role without touching the filesystem.
//
// `configPath` is the resolved config.ini path (its directory is the binary
// folder by default).  `daemonRole` selects the daemon's state directory
// instead, because systemd mounts /usr read-only for the unit
// (ProtectSystem=full) while StateDirectory=greencurve stays writable — the
// daemon literally cannot write next to its own binary.
//
// Returns false only when the inputs cannot produce a bounded path.  Pure and
// header-only so the regression harness covers it on any host.
static inline bool linux_debug_log_resolve_path(const char* configPath,
                                                bool daemonRole,
                                                const char* stateDir,
                                                char* dst, size_t dstSize) {
    if (!dst || dstSize == 0) return false;
    dst[0] = 0;
    if (daemonRole) {
        if (!stateDir || !stateDir[0]) return false;
        int written = gc_snprintf(dst, dstSize, "%s/%s", stateDir,
                                  LINUX_DEBUG_LOG_FILE_NAME);
        return written > 0 && (size_t)written < dstSize;
    }
    // Directory component of the config path; "." when it has none, which
    // matches the fallback config name used when /proc/self/exe is unreadable.
    char directory[4096] = {};
    const char* lastSlash = configPath ? strrchr(configPath, '/') : nullptr;
    if (!configPath || !configPath[0] || !lastSlash) {
        gc_strlcpy(directory, sizeof(directory), ".");
    } else if (lastSlash == configPath) {
        gc_strlcpy(directory, sizeof(directory), "/");
    } else {
        size_t length = (size_t)(lastSlash - configPath);
        if (length >= sizeof(directory)) length = sizeof(directory) - 1;
        memcpy(directory, configPath, length);
        directory[length] = 0;
    }
    int written = gc_snprintf(dst, dstSize, "%s/%s", directory,
                              LINUX_DEBUG_LOG_FILE_NAME);
    return written > 0 && (size_t)written < dstSize;
}

// Decide whether logging is on from the two inputs Windows also uses.
// `envValue` is the raw GREEN_CURVE_DEBUG value (nullptr when unset): an
// explicit "0" wins over the config, anything else present forces it on.
static inline bool linux_debug_log_enabled_for(const char* envValue,
                                               int configEnabled) {
    if (envValue && envValue[0] == '0' && envValue[1] == '\0') return false;
    if (envValue && envValue[0]) return true;
    return configEnabled != 0;
}

// Configure the sink.  Safe to call more than once; a later call with a
// different path reopens the file.  `role` labels the session marker
// ("cli", "tui", "daemon", ...).
void linux_debug_log_configure(const char* configPath, bool daemonRole,
                               const char* role);

// Force the sink on/off without consulting config (used by the daemon before
// its config is resolvable, and by tests).
void linux_debug_log_set_enabled(bool enabled);
bool linux_debug_log_is_enabled();

// Mirror every line to stderr as well.  The daemon wants this (journald is its
// primary log); interactive clients do not, because stderr is the terminal the
// TUI is about to take over.
void linux_debug_log_set_stderr_mirror(bool mirror);

// The active log path, or "" when logging is disabled/unresolved.
const char* linux_debug_log_path();

// Raw append descriptor for the log file, or -1 when it is not open.  Exposed
// so the fatal-signal breadcrumb can write(2) into it without formatting or
// locking anything (linux_crash_breadcrumb.h).
int linux_debug_log_raw_fd();

void linux_debug_logf(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

// Session banner: version/build/protocol/pid/role plus the resolved paths.
void linux_debug_log_session(const char* role, const char* configPath,
                             const char* detail);

void linux_debug_log_close();

#endif
