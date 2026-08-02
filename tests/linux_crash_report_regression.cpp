// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Native Linux filesystem coverage for the crash report's create-on-crash rule.
//
// The bug this exists to keep fixed: arming the report used to open() the file,
// so every run of every role left a 0-byte greencurve_crash_<stamp>_pid<N>.txt
// next to config.ini.  Only linux_crash_report_close() removed one, nothing
// called it, and the paths that leak hardest cannot call it at all -- execve()
// (the TUI relaunching itself into a terminal), _exit(), SIGKILL.  These tests
// assert the invariant that replaced it from both sides:
//
//     a report file exists  <=>  a crash wrote bytes into it.
//
// The crash half really crashes: a forked child raises SIGSEGV and the parent
// inspects what the handler left on disk.  fork/raise/waitpid is fully ordered,
// so there is nothing timing-dependent to tune.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "platform.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// linux_crash_report.cpp logs through the shared debug sink; the fixture only
// needs the calls to be harmless, not to produce a log.
void linux_debug_logf(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
void linux_debug_logf(const char* fmt, ...) { (void)fmt; }

#include "linux_crash_report.cpp"

namespace {

int g_failures = 0;

void fail(const char* what) {
    fprintf(stderr, "FAIL: %s\n", what);
    g_failures++;
}

void expect(bool condition, const char* what) {
    if (!condition) fail(what);
}

char g_root[512] = {};
char g_configPath[640] = {};

// Every report this fixture writes goes under one private directory, so a
// counting assertion can never be confused by an unrelated file.
bool make_root() {
    if (gc_snprintf(g_root, sizeof(g_root), "/tmp/greencurve-crash-report-regression-%ld",
                    (long)getpid()) < 0) {
        return false;
    }
    if (mkdir(g_root, 0700) != 0 && errno != EEXIST) return false;
    return gc_snprintf(g_configPath, sizeof(g_configPath), "%s/config.ini", g_root) >= 0;
}

void remove_root() {
    DIR* dir = opendir(g_root);
    if (dir) {
        struct dirent* entry = nullptr;
        while ((entry = readdir(dir)) != nullptr) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            char victim[1024] = {};
            if (gc_snprintf(victim, sizeof(victim), "%s/%s", g_root, entry->d_name) < 0) continue;
            unlink(victim);
        }
        closedir(dir);
    }
    rmdir(g_root);
}

// Count report files the project itself would recognise, so a stray file in the
// directory cannot make a leak look like a pass.
unsigned int count_reports(unsigned long long* totalBytesOut) {
    if (totalBytesOut) *totalBytesOut = 0;
    DIR* dir = opendir(g_root);
    if (!dir) return 0;
    unsigned int count = 0;
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        char stamp[GC_CRASH_STAMP_LENGTH + 1] = {};
        if (!gc_crash_artifact_stamp(entry->d_name, stamp)) continue;
        count++;
        char full[1024] = {};
        if (gc_snprintf(full, sizeof(full), "%s/%s", g_root, entry->d_name) < 0) continue;
        struct stat info = {};
        if (totalBytesOut && stat(full, &info) == 0)
            *totalBytesOut += (unsigned long long)info.st_size;
    }
    closedir(dir);
    return count;
}

bool touch(const char* name) {
    char full[1024] = {};
    if (gc_snprintf(full, sizeof(full), "%s/%s", g_root, name) < 0) return false;
    int fd = open(full, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    close(fd);
    return true;
}

bool exists(const char* path) {
    struct stat info = {};
    return path && path[0] && stat(path, &info) == 0;
}

// Every emit also goes to stderr, which is correct in production and pure noise
// in a test that calls the emit path directly.  Muted around those calls only,
// so a genuine failure message still reaches the terminal.
int g_savedStderr = -1;

void mute_stderr() {
    if (g_savedStderr >= 0) return;
    g_savedStderr = dup(STDERR_FILENO);
    int devNull = open("/dev/null", O_WRONLY);
    if (devNull >= 0) {
        dup2(devNull, STDERR_FILENO);
        close(devNull);
    }
}

void unmute_stderr() {
    if (g_savedStderr < 0) return;
    dup2(g_savedStderr, STDERR_FILENO);
    close(g_savedStderr);
    g_savedStderr = -1;
}

// --- Arming creates nothing -------------------------------------------------
//
// The whole regression in one assertion: configure(), then look at the disk.
void test_arm_creates_no_file() {
    expect(linux_crash_report_configure(g_configPath, false),
           "arming resolves a report path");
    const char* path = linux_crash_report_path();
    expect(path && path[0], "arming publishes a report path");
    expect(strncmp(path, g_root, strlen(g_root)) == 0,
           "the report path is inside the config directory");
    expect(!exists(path), "arming does not create the report file");
    expect(count_reports(nullptr) == 0, "arming leaves the directory empty");

    // Re-arming is what a real process does when it switches directories; it
    // must not leave a file behind either.
    expect(linux_crash_report_configure(g_configPath, false), "re-arming succeeds");
    expect(count_reports(nullptr) == 0, "re-arming leaves the directory empty");

    // ...and neither does the teardown that used to own the cleanup.
    linux_crash_report_close();
    expect(count_reports(nullptr) == 0, "closing leaves the directory empty");
    expect(linux_crash_report_path()[0] == 0, "closing clears the published path");
}

// --- A fatal write creates the file, once -----------------------------------
void test_fatal_write_creates_file() {
    expect(linux_crash_report_configure(g_configPath, false), "arming for the write test");
    linux_set_crash_report_path(linux_crash_report_path());
    char path[sizeof(g_reportPath)] = {};
    gc_strlcpy(path, sizeof(path), linux_crash_report_path());
    expect(!exists(path), "still nothing on disk before the first emit");

    // A zero-length emit must not be enough to materialise the file: creating it
    // is a side effect of emitting, so the guard against a 0-byte artifact has
    // to sit in front of the open.
    mute_stderr();
    gc_signal_safe_emit("", 0);
    gc_signal_safe_write("");
    unmute_stderr();
    expect(!exists(path), "an empty emit does not create a 0-byte report");
    expect(count_reports(nullptr) == 0, "an empty emit leaves the directory empty");

    mute_stderr();
    gc_signal_safe_write("first fragment\n");
    unmute_stderr();
    expect(exists(path), "a fatal write creates the report file");
    int firstFd = (int)gc_crash_report_fd_slot();
    expect(firstFd >= 0, "the report descriptor is cached after the first write");

    // Every subsequent fragment appends through the same descriptor; a handler
    // emits dozens of them and must not reopen per fragment.
    mute_stderr();
    gc_signal_safe_write("second fragment\n");
    unmute_stderr();
    expect((int)gc_crash_report_fd_slot() == firstFd,
           "later fragments reuse the opened descriptor");

    struct stat info = {};
    expect(stat(path, &info) == 0 && info.st_size > 0, "the report is not empty");
    expect((info.st_mode & 0777) == 0600, "the report is created 0600");
    expect(count_reports(nullptr) == 1, "exactly one report exists");

    linux_crash_report_close();
    expect(exists(path), "closing keeps a report that has content");
    unlink(path);
}

// --- A failed open is not retried per fragment ------------------------------
void test_unwritable_directory_is_sticky() {
    char missingConfig[1024] = {};
    if (gc_snprintf(missingConfig, sizeof(missingConfig),
                    "%s/does-not-exist/config.ini", g_root) < 0) {
        fail("formatting the unwritable config path");
        return;
    }
    // The directory resolves fine -- it just is not there, which is what an
    // unwritable or vanished artifact directory looks like at crash time.
    expect(linux_crash_report_configure(missingConfig, false),
           "arming resolves a path even for a missing directory");
    linux_set_crash_report_path(linux_crash_report_path());
    mute_stderr();
    gc_signal_safe_write("fragment into nowhere\n");
    unmute_stderr();
    expect((int)gc_crash_report_fd_slot() == GC_CRASH_REPORT_FD_FAILED,
           "a failed open is recorded as failed, not retried");
    mute_stderr();
    gc_signal_safe_write("another fragment\n");
    unmute_stderr();
    expect((int)gc_crash_report_fd_slot() == GC_CRASH_REPORT_FD_FAILED,
           "the failure stays sticky across fragments");
    expect(count_reports(nullptr) == 0, "a failed open creates nothing");

    // Re-arming onto a good directory must clear the sticky failure, or a
    // process that switched roles would silently lose its report.
    expect(linux_crash_report_configure(g_configPath, false), "re-arming after a failure");
    expect((int)gc_crash_report_fd_slot() == -1, "re-arming clears the sticky failure");
    linux_crash_report_close();
}

// --- Rotation still keeps the newest, and only our own files ----------------
void test_rotation_keeps_newest() {
    // Twelve well-formed reports plus one file this project did not write.
    for (int i = 0; i < GC_CRASH_ARTIFACT_MAX_KEEP + 2; i++) {
        char name[256] = {};
        if (gc_snprintf(name, sizeof(name), "%s202607%02d_101530_250_pid%d%s",
                        GC_CRASH_DUMP_PREFIX, i + 1, 1000 + i,
                        GC_CRASH_REPORT_SUFFIX) < 0) {
            fail("formatting a rotation fixture name");
            return;
        }
        if (!touch(name)) fail("creating a rotation fixture");
    }
    expect(touch("please-do-not-delete.txt"), "creating the foreign fixture");

    expect(linux_crash_report_configure(g_configPath, false), "arming triggers rotation");
    expect(count_reports(nullptr) == (unsigned int)GC_CRASH_ARTIFACT_MAX_KEEP,
           "rotation trims to the keep budget");

    // The two oldest went; the newest survived.  Ordering is by the embedded
    // stamp, which is the property a lexicographic name sweep would break.
    char oldest[256] = {};
    char newest[256] = {};
    gc_snprintf(oldest, sizeof(oldest), "%s/%s20260701_101530_250_pid1000%s",
                g_root, GC_CRASH_DUMP_PREFIX, GC_CRASH_REPORT_SUFFIX);
    gc_snprintf(newest, sizeof(newest), "%s/%s20260712_101530_250_pid1011%s",
                g_root, GC_CRASH_DUMP_PREFIX, GC_CRASH_REPORT_SUFFIX);
    expect(!exists(oldest), "rotation deleted the oldest report");
    expect(exists(newest), "rotation kept the newest report");

    char foreign[1024] = {};
    gc_snprintf(foreign, sizeof(foreign), "%s/please-do-not-delete.txt", g_root);
    expect(exists(foreign), "rotation never touches a file we did not write");

    linux_crash_report_close();
    unlink(foreign);
    // Clear the survivors so the end-to-end test can count from zero.
    DIR* dir = opendir(g_root);
    if (dir) {
        struct dirent* entry = nullptr;
        while ((entry = readdir(dir)) != nullptr) {
            char stamp[GC_CRASH_STAMP_LENGTH + 1] = {};
            if (!gc_crash_artifact_stamp(entry->d_name, stamp)) continue;
            char victim[1024] = {};
            if (gc_snprintf(victim, sizeof(victim), "%s/%s", g_root, entry->d_name) < 0) continue;
            unlink(victim);
        }
        closedir(dir);
    }
}

// --- End to end: a real fatal signal, through the real handler --------------
//
// The child arms exactly the way linux_main.cpp does and then takes a SIGSEGV.
// Everything the parent asserts afterwards is what a user would actually find.
void test_real_crash_writes_report() {
    expect(count_reports(nullptr) == 0, "the directory starts empty");

    pid_t child = fork();
    if (child < 0) {
        fail("fork for the crash child");
        return;
    }
    if (child == 0) {
        // The core file belongs to the user's ulimit everywhere except here:
        // this child exists only to crash, and a core landing in the test
        // directory would make the report count ambiguous.
        struct rlimit noCore = {0, 0};
        setrlimit(RLIMIT_CORE, &noCore);
        // The handler's stderr copy is expected and verified by the file copy;
        // dumping /proc/self/maps into the test output would drown it.
        int devNull = open("/dev/null", O_WRONLY);
        if (devNull >= 0) {
            dup2(devNull, STDERR_FILENO);
            close(devNull);
        }
        linux_install_crash_breadcrumbs("regression");
        if (!linux_crash_report_configure(g_configPath, false)) _exit(3);
        linux_set_crash_report_path(linux_crash_report_path());
        linux_set_crash_phase("regression-crash");
        raise(SIGSEGV);
        _exit(4); // the handler re-raises, so this must be unreachable
    }

    int status = 0;
    expect(waitpid(child, &status, 0) == child, "reaping the crash child");
    expect(WIFSIGNALED(status), "the handler never suppresses the crash");
    expect(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV,
           "the child died from the signal it raised");

    unsigned long long bytes = 0;
    expect(count_reports(&bytes) == 1, "a real crash leaves exactly one report");
    expect(bytes > 0, "the report a real crash leaves is not empty");

    // The report has to be actionable, not merely present: the breadcrumb line,
    // the phase, and the maps block that makes a PIE address symbolizable.
    DIR* dir = opendir(g_root);
    bool checked = false;
    if (dir) {
        struct dirent* entry = nullptr;
        while ((entry = readdir(dir)) != nullptr) {
            char stamp[GC_CRASH_STAMP_LENGTH + 1] = {};
            if (!gc_crash_artifact_stamp(entry->d_name, stamp)) continue;
            char full[1024] = {};
            if (gc_snprintf(full, sizeof(full), "%s/%s", g_root, entry->d_name) < 0) continue;
            FILE* file = fopen(full, "rb");
            if (!file) continue;
            char body[65536] = {};
            size_t got = fread(body, 1, sizeof(body) - 1, file);
            fclose(file);
            body[got] = 0;
            expect(strstr(body, "greencurve FATAL [regression]") != nullptr,
                   "the report carries the fatal breadcrumb");
            expect(strstr(body, "phase=regression-crash") != nullptr,
                   "the report carries the crash phase");
            expect(strstr(body, "--- /proc/self/maps ---") != nullptr,
                   "the report carries the module map");
            checked = true;
            unlink(full);
        }
        closedir(dir);
    }
    expect(checked, "the crash report was readable");
}

// --- A clean exit leaves nothing, even through execve -----------------------
//
// The TUI relaunches itself into a terminal emulator, so its first process ends
// in execve() and never returns to main.  That path is why cleanup-on-exit
// could not have fixed this: O_CLOEXEC closes the descriptor and no destructor,
// atexit handler or explicit close ever runs.
void test_clean_exit_and_exec_leave_nothing() {
    pid_t child = fork();
    if (child < 0) {
        fail("fork for the clean-exit child");
        return;
    }
    if (child == 0) {
        linux_install_crash_breadcrumbs("regression");
        if (!linux_crash_report_configure(g_configPath, false)) _exit(3);
        linux_set_crash_report_path(linux_crash_report_path());
        // Exactly the TUI's relaunch shape: hand the process over to another
        // image without any opportunity to clean up.
        execl("/bin/true", "true", (char*)nullptr);
        _exit(4);
    }
    int status = 0;
    expect(waitpid(child, &status, 0) == child, "reaping the exec child");
    expect(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the exec child exited cleanly");
    expect(count_reports(nullptr) == 0, "an execve'd process leaves no report behind");

    // ...and the same for a process killed outright, which no cleanup can catch.
    child = fork();
    if (child < 0) {
        fail("fork for the killed child");
        return;
    }
    if (child == 0) {
        linux_install_crash_breadcrumbs("regression");
        if (!linux_crash_report_configure(g_configPath, false)) _exit(3);
        linux_set_crash_report_path(linux_crash_report_path());
        raise(SIGKILL);
        _exit(4);
    }
    status = 0;
    expect(waitpid(child, &status, 0) == child, "reaping the killed child");
    expect(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "the child was killed");
    expect(count_reports(nullptr) == 0, "a SIGKILLed process leaves no report behind");
}

} // namespace

int main() {
    if (!make_root()) {
        fprintf(stderr, "FAIL: cannot create the fixture directory\n");
        return 1;
    }
    test_arm_creates_no_file();
    test_fatal_write_creates_file();
    test_unwritable_directory_is_sticky();
    test_rotation_keeps_newest();
    test_real_crash_writes_report();
    test_clean_exit_and_exec_leave_nothing();
    remove_root();
    if (g_failures != 0) {
        fprintf(stderr, "Linux crash report regression FAILED (%d)\n", g_failures);
        return 1;
    }
    printf("Linux crash report regression passed\n");
    return 0;
}
