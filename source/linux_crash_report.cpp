// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_crash_report.cpp — see linux_crash_report.h.

#include "linux_crash_report.h"

#include "crash_artifact_policy.h"
#include "linux_crash_breadcrumb.h"
#include "linux_daemon.h"
#include "linux_debug_log.h"
#include "platform.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace {

// Static storage, because the handler reads this buffer through the path slot
// published to linux_crash_breadcrumb.h and must never chase a pointer into
// anything with a shorter lifetime than the process.
char g_reportPath[4096] = {};

// Rotate this process's report directory before adding to it.
//
// Same budget and the same "never delete a file we did not write" rule as the
// Windows sweep (crash_artifact_policy.h); a daemon that crash-loops under
// systemd's Restart=on-failure is exactly the case that would otherwise fill
// /var/lib.  Ordering is by the embedded timestamp, so the newest report always
// survives regardless of which prefix it carries.
void rotate_reports(const char* directory) {
    for (;;) {
        DIR* dir = opendir(directory);
        if (!dir) return;
        unsigned int count = 0;
        char oldest[NAME_MAX + 1] = {};
        struct dirent* entry = nullptr;
        while ((entry = readdir(dir)) != nullptr) {
            char stamp[GC_CRASH_STAMP_LENGTH + 1] = {};
            if (!gc_crash_artifact_stamp(entry->d_name, stamp)) continue;
            size_t nameLength = strlen(entry->d_name);
            if (nameLength < sizeof(GC_CRASH_REPORT_SUFFIX) - 1) continue;
            if (strcmp(entry->d_name + nameLength - (sizeof(GC_CRASH_REPORT_SUFFIX) - 1),
                       GC_CRASH_REPORT_SUFFIX) != 0) {
                continue;
            }
            count++;
            if (oldest[0] == 0 || gc_crash_artifact_is_older(entry->d_name, oldest)) {
                gc_strlcpy(oldest, sizeof(oldest), entry->d_name);
            }
        }
        closedir(dir);
        if (!gc_crash_rotation_needed(count, GC_CRASH_ARTIFACT_MAX_KEEP) || oldest[0] == 0) return;
        char victim[sizeof(g_reportPath)] = {};
        if (gc_snprintf(victim, sizeof(victim), "%s/%s", directory, oldest) < 0) return;
        if (unlink(victim) != 0) return; // stop on failure rather than spin
        linux_debug_logf("crash report: rotated out %s (%u present, keeping %d)",
                         oldest, count, GC_CRASH_ARTIFACT_MAX_KEEP);
    }
}

} // namespace

bool linux_crash_report_configure(const char* configPath, bool daemonRole) {
    linux_crash_report_close();

    char directory[sizeof(g_reportPath)] = {};
    if (!gc_linux_crash_dir(configPath, daemonRole, GC_DAEMON_STATE_DIR,
                            directory, sizeof(directory))) {
        linux_debug_logf("crash report: no artifact directory resolved "
                         "(configPath=%s daemonRole=%d); stderr and the debug log "
                         "remain the only crash sinks",
                         configPath && configPath[0] ? configPath : "<unset>",
                         daemonRole ? 1 : 0);
        return false;
    }

    rotate_reports(directory);

    // The filename is formatted ONCE, here, because the signal handler cannot
    // call snprintf.  A process only ever writes one fatal report, so the pid
    // plus the start timestamp is unique without any per-crash formatting.
    struct timespec now = {};
    clock_gettime(CLOCK_REALTIME, &now);
    struct tm broken = {};
    localtime_r(&now.tv_sec, &broken);
    if (gc_snprintf(g_reportPath, sizeof(g_reportPath),
                    "%s/%s%04d%02d%02d_%02d%02d%02d_%03ld_pid%ld%s",
                    directory, GC_CRASH_DUMP_PREFIX,
                    broken.tm_year + 1900, broken.tm_mon + 1, broken.tm_mday,
                    broken.tm_hour, broken.tm_min, broken.tm_sec,
                    now.tv_nsec / 1000000L, (long)getpid(),
                    GC_CRASH_REPORT_SUFFIX) < 0) {
        g_reportPath[0] = 0;
        return false;
    }

    // Nothing is created here.  The file is opened by the crash handler on the
    // first byte it emits (gc_crash_report_open_on_demand), so a process that
    // exits normally — or is execve'd over, _exit()ed, or SIGKILLed — leaves
    // nothing behind.  See linux_crash_breadcrumb.h for why arming used to
    // create the file and why that was wrong.
    //
    // The writability probe is diagnostics only: it is the difference between a
    // log that says the report is armed and one that says it is armed but the
    // directory cannot be written, which is a bug report's whole answer.  It is
    // deliberately not a gate — access() answers for the real uid and misreads
    // ACLs, so a false negative must not disarm a report that would in fact be
    // written, and by crash time the answer may have changed either way.
    bool writable = access(directory, W_OK) == 0;
    linux_debug_logf("crash report: armed for %s (created only on a crash, "
                     "directory %swritable, keeping %d per directory)",
                     g_reportPath, writable ? "" : "NOT ",
                     GC_CRASH_ARTIFACT_MAX_KEEP);
    return true;
}

const char* linux_crash_report_path() { return g_reportPath; }

void linux_crash_report_close() {
    // Detach the published path first: once it is gone the handler can no
    // longer materialise a file, which is the ordering that matters if a crash
    // lands in the middle of a re-arm.
    linux_set_crash_report_path(nullptr);
    g_reportPath[0] = 0;

    // Both descriptors are cleared before they are closed.  A handler must
    // never hold a descriptor number that has been closed and could be
    // reallocated to, say, the daemon's control socket — the same hazard
    // linux_debug_log.cpp's dup2 rotation avoids.  The report descriptor is
    // normally still -1 here: it only exists if a crash already opened it and
    // the process somehow continued.
    volatile sig_atomic_t& reportSlot = gc_crash_report_fd_slot();
    int reportFd = (int)reportSlot;
    reportSlot = -1;
    if (reportFd >= 0) close(reportFd);

    volatile sig_atomic_t& reserveSlot = gc_crash_report_reserve_slot();
    int reserveFd = (int)reserveSlot;
    reserveSlot = -1;
    if (reserveFd >= 0) close(reserveFd);
}
