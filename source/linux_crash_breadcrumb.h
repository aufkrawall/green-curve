// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_crash_breadcrumb.h — async-signal-safe fatal-signal and std::terminate
// breadcrumbs for the Linux binary.
//
// Why: Windows installs SetUnhandledExceptionFilter plus a vectored handler and
// writes minidumps (main_diagnostics.cpp).  The Linux side had none of that,
// yet it builds with -fexceptions/-frtti and uses std::string in the INI parser
// (linux_port.cpp), so a bad_alloc or length_error aborted the *root daemon*
// with no journal entry at all.  Everything here writes to fd 2 with write(2)
// only, so it is safe from a signal handler; systemd captures it in the journal.
//
// stderr alone is not enough for a desktop-launched client: a TUI started from
// a file manager has no terminal to print to, so the breadcrumb went nowhere.
// The breadcrumb is therefore also appended to the already-open debug log
// descriptor (linux_debug_log.cpp).  Only the raw fd is used here — open() is
// async-signal-safe but formatting a fresh crash-file name is not, so the
// descriptor is established up front and never touched from the handler.
//
// Note this is a breadcrumb, not a minidump: the actual core dump is left to
// the kernel's core_pattern (systemd-coredump on most distributions, readable
// with `coredumpctl`), because SA_RESETHAND + raise() keeps the default crash
// behaviour intact.

#ifndef GREEN_CURVE_LINUX_CRASH_BREADCRUMB_H
#define GREEN_CURVE_LINUX_CRASH_BREADCRUMB_H

#include <exception>
#include <signal.h>
// abort() below.  The bundled Zig headers happen to pull <stdlib.h> in through
// one of the others, so omitting it built fine for the shipped cross-compile
// and broke only under a stock glibc/libc++ host clang.
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Points at a string literal with static storage duration.  Never allocates,
// never copies, so it stays readable from a signal handler.
static volatile sig_atomic_t g_crashBreadcrumbInstalled = 0;
static volatile sig_atomic_t g_crashLogFd = -1;
static const char* volatile g_crashPhase = "startup";
static const char* volatile g_crashRole = "greencurve";

static inline void linux_set_crash_phase(const char* phase) {
    if (phase) g_crashPhase = phase;
}

// Publish the debug-log descriptor so a fatal signal leaves a record in the
// file too.  Pass -1 to detach before the log is closed.
static inline void linux_set_crash_log_fd(int fd) {
    g_crashLogFd = (sig_atomic_t)fd;
}

static inline void gc_signal_safe_write(const char* text) {
    if (!text) return;
    size_t length = 0;
    while (text[length]) length++;
    ssize_t ignored = write(STDERR_FILENO, text, length);
    (void)ignored;
    int logFd = (int)g_crashLogFd;
    if (logFd >= 0) {
        ignored = write(logFd, text, length);
        (void)ignored;
    }
}

// The usual formatting and signal-name helpers are not async-signal-safe, so
// integers are rendered by hand straight into a stack buffer.
static inline void gc_signal_safe_write_int(long value) {
    char digits[24];
    size_t index = sizeof(digits);
    bool negative = value < 0;
    unsigned long magnitude = negative
        ? (unsigned long)(-(value + 1)) + 1ul : (unsigned long)value;
    if (magnitude == 0) digits[--index] = '0';
    while (magnitude > 0 && index > 0) {
        digits[--index] = (char)('0' + (magnitude % 10ul));
        magnitude /= 10ul;
    }
    if (negative && index > 0) digits[--index] = '-';
    ssize_t ignored = write(STDERR_FILENO, digits + index, sizeof(digits) - index);
    (void)ignored;
    int logFd = (int)g_crashLogFd;
    if (logFd >= 0) {
        ignored = write(logFd, digits + index, sizeof(digits) - index);
        (void)ignored;
    }
}

static inline void gc_write_crash_breadcrumb(const char* kind, long detail) {
    gc_signal_safe_write("greencurve FATAL [");
    gc_signal_safe_write((const char*)g_crashRole);
    gc_signal_safe_write("] ");
    gc_signal_safe_write(kind);
    gc_signal_safe_write("=");
    gc_signal_safe_write_int(detail);
    gc_signal_safe_write(" pid=");
    gc_signal_safe_write_int((long)getpid());
    gc_signal_safe_write(" phase=");
    gc_signal_safe_write((const char*)g_crashPhase);
    gc_signal_safe_write(" version=" APP_VERSION "\n");
}

// SA_RESETHAND restores the default disposition before this runs, so the
// re-raise below produces the normal core dump / systemd crash accounting.
// The breadcrumb never suppresses the crash; it only makes it diagnosable.
static void gc_fatal_signal_handler(int signalNumber) {
    gc_write_crash_breadcrumb("signal", (long)signalNumber);
    raise(signalNumber);
}

static void gc_terminate_handler() {
    gc_write_crash_breadcrumb("terminate", 0);
    // Uncaught exception or a failed allocation.  Abort so the failure is still
    // visible to systemd as a crash rather than a silent clean exit.
    abort();
}

static inline void linux_install_crash_breadcrumbs(const char* role) {
    if (role) g_crashRole = role;
    if (g_crashBreadcrumbInstalled) return;
    g_crashBreadcrumbInstalled = 1;

    struct sigaction fatalAction = {};
    fatalAction.sa_handler = gc_fatal_signal_handler;
    sigemptyset(&fatalAction.sa_mask);
    fatalAction.sa_flags = SA_RESETHAND | SA_NODEFER;
    const int fatalSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    for (size_t i = 0; i < sizeof(fatalSignals) / sizeof(fatalSignals[0]); i++)
        sigaction(fatalSignals[i], &fatalAction, nullptr);

    std::set_terminate(gc_terminate_handler);
}

#endif // GREEN_CURVE_LINUX_CRASH_BREADCRUMB_H
