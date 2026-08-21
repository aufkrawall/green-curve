// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_debug_log.cpp — see linux_debug_log.h.

#include "linux_debug_log.h"

#include "linux_daemon.h"
#include "log_redaction_policy.h"
#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif
#ifndef APP_BUILD_NUMBER
#define APP_BUILD_NUMBER 0
#endif

namespace {

pl_mutex g_logLock;
bool g_lockReady = false;
bool g_enabled = false;
bool g_stderrMirror = false;
int g_fd = -1;
char g_path[4096] = {};

void ensure_lock() {
    if (g_lockReady) return;
    pl_mutex_init(&g_logLock);
    g_lockReady = true;
}

bool write_all(int fd, const char* data, size_t length) {
    size_t done = 0;
    while (done < length) {
        ssize_t written = write(fd, data + done, length - done);
        if (written > 0) { done += (size_t)written; continue; }
        if (written < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

// Keep exactly one previous generation.  An always-on log that a user never
// clears must not fill the partition the daemon writes its state to.
//
// The descriptor *number* is deliberately preserved across a rotation with
// dup2(): linux_crash_breadcrumb.h publishes it once and writes to it from a
// signal handler, so a closed-and-reallocated fd could later point at the
// daemon's control socket. Reusing the number keeps that impossible.
void rotate_if_oversized_locked() {
    if (g_fd < 0) return;
    struct stat status = {};
    if (fstat(g_fd, &status) != 0) return;
    if (status.st_size < (off_t)LINUX_DEBUG_LOG_MAX_BYTES) return;
    char rotated[sizeof(g_path) + 4] = {};
    gc_snprintf(rotated, sizeof(rotated), "%s.1", g_path);
    if (rename(g_path, rotated) != 0) {
        // Rotation failed (read-only parent, races). Truncate through the
        // existing descriptor rather than growing forever; losing history beats
        // filling the disk, and the fd number still stays valid.
        if (ftruncate(g_fd, 0) == 0) lseek(g_fd, 0, SEEK_SET);
        return;
    }
    int fresh = open(g_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fresh < 0) return;  // keep writing to the rotated file; never lose the fd
    // On failure the original descriptor still points at the rotated file,
    // which remains a safe logging destination.
    (void)dup2(fresh, g_fd);
    close(fresh);
}

void open_locked() {
    if (g_fd >= 0 || !g_path[0]) return;
    g_fd = open(g_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (g_fd < 0) return;
    rotate_if_oversized_locked();
}

}  // namespace

void linux_debug_log_set_enabled(bool enabled) {
    ensure_lock();
    pl_mutex_lock(&g_logLock);
    g_enabled = enabled;
    if (!enabled && g_fd >= 0) { close(g_fd); g_fd = -1; }
    pl_mutex_unlock(&g_logLock);
}

bool linux_debug_log_is_enabled() { return g_enabled; }

void linux_debug_log_set_stderr_mirror(bool mirror) { g_stderrMirror = mirror; }

const char* linux_debug_log_path() { return g_enabled ? g_path : ""; }

int linux_debug_log_raw_fd() { return g_fd; }

void linux_debug_log_configure(const char* configPath, bool daemonRole,
                               const char* role) {
    ensure_lock();
    char resolved[sizeof(g_path)] = {};
    if (!linux_debug_log_resolve_path(configPath, daemonRole,
                                      GC_DAEMON_STATE_DIR,
                                      resolved, sizeof(resolved))) {
        return;
    }
    pl_mutex_lock(&g_logLock);
    if (strcmp(resolved, g_path) != 0) {
        if (g_fd >= 0) { close(g_fd); g_fd = -1; }
        gc_strlcpy(g_path, sizeof(g_path), resolved);
    }
    // Opened eagerly rather than on first write: the fatal-signal breadcrumb can
    // only write to a descriptor that already exists, and a crash during
    // startup is exactly the case that most needs recording.
    if (g_enabled) open_locked();
    pl_mutex_unlock(&g_logLock);
    if (g_enabled) linux_debug_log_session(role, configPath, nullptr);
}

void linux_debug_logf(const char* fmt, ...) {
    if (!g_enabled || !fmt) return;
    ensure_lock();

    char line[2048] = {};
    struct timespec now = {};
    clock_gettime(CLOCK_REALTIME, &now);
    struct tm broken = {};
    localtime_r(&now.tv_sec, &broken);
    int prefix = gc_snprintf(line, sizeof(line),
        "%04d-%02d-%02d %02d:%02d:%02d.%03ld [%ld] ",
        broken.tm_year + 1900, broken.tm_mon + 1, broken.tm_mday,
        broken.tm_hour, broken.tm_min, broken.tm_sec,
        now.tv_nsec / 1000000L, (long)getpid());
    if (prefix < 0) prefix = 0;
    if ((size_t)prefix >= sizeof(line)) prefix = (int)sizeof(line) - 1;

    va_list ap;
    va_start(ap, fmt);
    gc_vsnprintf(line + prefix, sizeof(line) - (size_t)prefix, fmt, ap);
    va_end(ap);

    size_t length = strlen(line);
    if (length == 0) return;
    if (line[length - 1] != '\n' && length + 1 < sizeof(line)) {
        line[length] = '\n';
        line[length + 1] = 0;
        length++;
    }

    if (g_stderrMirror) {
        // The journal/terminal copy keeps the message verbatim: systemd already
        // stamps its own timestamp and PID.
        (void)write_all(STDERR_FILENO, line + prefix, length - (size_t)prefix);
    }

    pl_mutex_lock(&g_logLock);
    open_locked();
    if (g_fd >= 0) {
        if (!write_all(g_fd, line, length)) {
            close(g_fd);
            g_fd = -1;
        } else {
            rotate_if_oversized_locked();
        }
    }
    pl_mutex_unlock(&g_logLock);
}

void linux_debug_log_session(const char* role, const char* configPath,
                             const char* detail) {
    if (!g_enabled) return;
    char sessionConfigToken[32] = {};
    char sessionLogToken[32] = {};
    gc_log_path_token(configPath, sessionConfigToken,
                      sizeof(sessionConfigToken));
    gc_log_path_token(g_path, sessionLogToken, sizeof(sessionLogToken));
    linux_debug_logf("===== SESSION BEGIN role=%s =====", role ? role : "unknown");
    linux_debug_logf(
        "version=%s build=%u protocol=%u pid=%ld uid=%u euid=%u config=%s log=%s",
        APP_VERSION, (unsigned int)APP_BUILD_NUMBER,
        (unsigned int)SERVICE_PROTOCOL_VERSION, (long)getpid(),
        (unsigned int)getuid(), (unsigned int)geteuid(),
        sessionConfigToken, sessionLogToken);
    linux_debug_logf(
        "debug logging is enabled by default; it records GPU identifiers, "
        "path fingerprints and applied settings. Set %s=0 or [debug] enabled=0 to "
        "disable it.", LINUX_DEBUG_LOG_ENV);
    if (detail && detail[0]) linux_debug_logf("details=%s", detail);
}

void linux_debug_log_close() {
    if (!g_lockReady) return;
    pl_mutex_lock(&g_logLock);
    if (g_fd >= 0) { close(g_fd); g_fd = -1; }
    pl_mutex_unlock(&g_logLock);
}
