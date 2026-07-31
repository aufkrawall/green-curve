// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_crash_breadcrumb.h — async-signal-safe fatal-signal and std::terminate
// reporting for the Linux binary.
//
// Why: Windows installs SetUnhandledExceptionFilter plus a vectored handler and
// writes minidumps (main_crash_artifacts.cpp).  The Linux side had none of that,
// yet it builds with -fexceptions/-frtti and uses std::string in the INI parser
// (linux_port.cpp), so a bad_alloc or length_error aborted the *root daemon*
// with no journal entry at all.  Everything here writes with write(2) only, so
// it is safe from a signal handler; systemd captures the stderr copy.
//
// stderr alone is not enough for a desktop-launched client: a TUI started from
// a file manager has no terminal to print to, so the breadcrumb went nowhere.
// The report is therefore written to three places — stderr, the already-open
// debug-log descriptor (linux_debug_log.cpp), and a dedicated crash-report file
// next to config.ini (which on Linux defaults to the binary's own directory;
// see linux_crash_report.cpp).
//
// WHY THE REPORT FILE IS CREATED HERE AND NOT AT STARTUP.  Only the report's
// *path* is formatted up front, because open() is async-signal-safe but string
// formatting is not.  (The guards in tools/linux_gates.py reject the formatting
// calls by name, so this file cannot even mention them.)  The open itself
// happens on the first byte a fatal path actually emits.
//
// It used to happen at startup instead, and that was wrong in a way that took
// the whole feature down with it: every run of every role left a 0-byte
// placeholder in the user's directory, and the only thing that removed one was
// linux_crash_report_close() — which no caller ever invoked.  Even a correct
// caller could not have fixed it, because the placeholder outlives the paths
// that cannot run cleanup at all: execve() (the TUI's terminal relaunch closes
// the O_CLOEXEC descriptor without ever returning to main), _exit(), SIGKILL,
// and power loss.  Creating the file only when there is something to put in it
// makes the invariant structural rather than something an exit path can forget:
//
//     a report file exists  <=>  a crash wrote bytes into it.
//
// That is also what makes the directory worth looking at — every file present
// is real evidence — and what keeps rotation honest, since placeholders would
// otherwise push genuine reports out of the GC_CRASH_ARTIFACT_MAX_KEEP budget.
//
// WHAT THIS IS AND IS NOT.  There is no in-process minidump writer on Linux and
// deliberately so: the binary artifact stays the kernel core (core_pattern /
// systemd-coredump), which the kernel writes correctly for a process whose own
// memory may already be corrupt.  What this file adds is everything needed to
// make an address actionable without the core — the faulting address, the
// signal's own si_code, the PC/SP at the fault, and the full /proc/self/maps —
// so `llvm-symbolizer --obj=greencurve.debug <pc minus module base>` resolves a
// line number from the report alone.  The build emits that matching .debug file
// and a build-id (build.py, dist/symbols/linux-<arch>).
//
// The handler NEVER suppresses the crash: SA_RESETHAND restores the default
// disposition and the handler re-raises, so core dumps and systemd crash
// accounting are exactly what they were.  RLIMIT_CORE is deliberately not
// touched — overriding a user's own `ulimit -c 0` would be a side effect this
// program has no business having.

#ifndef GREEN_CURVE_LINUX_CRASH_BREADCRUMB_H
#define GREEN_CURVE_LINUX_CRASH_BREADCRUMB_H

#include <errno.h>
#include <exception>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
// abort() below.  The bundled Zig headers happen to pull <stdlib.h> in through
// one of the others, so omitting it built fine for the shipped cross-compile
// and broke only under a stock glibc/libc++ host clang.
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

// Points at a string literal with static storage duration.  Never allocates,
// never copies, so it stays readable from a signal handler.
static volatile sig_atomic_t g_crashBreadcrumbInstalled = 0;
static volatile sig_atomic_t g_crashLogFd = -1;
static const char* volatile g_crashPhase = "startup";
static const char* volatile g_crashRole = "greencurve";

// The crash-report state is deliberately NOT one of the per-TU statics above.
// Those force every translation unit that installs handlers to re-arm its own
// copy — the reason linux_main.cpp and linux_daemon.cpp each call
// linux_set_crash_log_fd() — and a TU that forgets simply writes nothing on a
// crash, which is invisible until the day it matters.
//
// A non-static inline function's local static has exactly one instance across
// the whole program, so arming it once is enough.  `volatile sig_atomic_t` with
// a constant initialiser is constant-initialised, so no thread-safe-statics
// guard is emitted and reading it from a signal handler stays safe.

// Descriptor for the report file, populated on the first fatal write.  Three
// states, because "we tried and it failed" has to be distinguishable from "we
// have not tried yet": >= 0 is open, -1 is not yet attempted, and
// GC_CRASH_REPORT_FD_FAILED means a doomed open must not be retried once per
// emitted fragment while the process is dying.
#define GC_CRASH_REPORT_FD_FAILED (-2)
inline volatile sig_atomic_t& gc_crash_report_fd_slot() {
    static volatile sig_atomic_t reportFd = -1;
    return reportFd;
}

// The pre-formatted report path.  Points at static storage owned by
// linux_crash_report.cpp, so reading it from a handler never allocates.
inline const char* volatile& gc_crash_report_path_slot() {
    static const char* volatile reportPath = nullptr;
    return reportPath;
}

// A descriptor held open purely to be given away.  Deferring the open to crash
// time costs one guarantee the startup open had: that a descriptor was
// available at all.  Descriptor exhaustion is a plausible cause of the very
// crash being reported (EMFILE turns into a bad_alloc or a null return that
// nothing checks), and that is exactly when the report matters most.  Taking a
// slot up front and releasing it immediately before the real open buys it back
// for the per-process limit.  A system-wide ENFILE is still lost — nothing this
// process can do about that — and stderr plus the debug log still carry the
// breadcrumb either way.
inline volatile sig_atomic_t& gc_crash_report_reserve_slot() {
    static volatile sig_atomic_t reserveFd = -1;
    return reserveFd;
}

static inline void linux_set_crash_phase(const char* phase) {
    if (phase) g_crashPhase = phase;
}

// Publish the debug-log descriptor so a fatal signal leaves a record in the
// file too.  Pass -1 to detach before the log is closed.
static inline void linux_set_crash_log_fd(int fd) {
    g_crashLogFd = (sig_atomic_t)fd;
}

// Publish the dedicated crash-report path (linux_crash_report.cpp formats it at
// startup so the handler never has to).  Pass nullptr or "" to detach.  One
// call arms every translation unit; see gc_crash_report_path_slot().
//
// Nothing is created on disk here.  The descriptor reserve is taken now because
// this is the last moment that is not a signal handler.
static inline void linux_set_crash_report_path(const char* path) {
    gc_crash_report_path_slot() = (path && path[0]) ? path : nullptr;
    // Re-arming clears a previous path's sticky open failure; the new path may
    // well be writable when the old one was not.
    if (gc_crash_report_fd_slot() == GC_CRASH_REPORT_FD_FAILED)
        gc_crash_report_fd_slot() = -1;
    volatile sig_atomic_t& reserve = gc_crash_report_reserve_slot();
    if (gc_crash_report_path_slot() && reserve < 0) {
        int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) reserve = (sig_atomic_t)fd;
    }
}

// Create the report file, once, at the moment a fatal path has bytes for it.
//
// open() is async-signal-safe; the path was formatted at startup.  O_CLOEXEC so
// a forked helper cannot inherit the descriptor and report into this process's
// file, and 0600 because the report carries GPU identifiers, config paths and
// register state — the same sensitivity as the debug log beside it.
static inline int gc_crash_report_open_on_demand() {
    volatile sig_atomic_t& slot = gc_crash_report_fd_slot();
    if (slot >= 0) return (int)slot;
    if (slot == GC_CRASH_REPORT_FD_FAILED) return -1;
    const char* path = gc_crash_report_path_slot();
    if (!path || !path[0]) return -1;
    volatile sig_atomic_t& reserve = gc_crash_report_reserve_slot();
    if (reserve >= 0) {
        close((int)reserve);
        reserve = -1;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    slot = (sig_atomic_t)(fd >= 0 ? fd : GC_CRASH_REPORT_FD_FAILED);
    return fd;
}

static inline void gc_signal_safe_write_fd(int fd, const char* text, size_t length) {
    if (fd < 0 || !text || length == 0) return;
    size_t done = 0;
    while (done < length) {
        ssize_t written = write(fd, text + done, length - done);
        if (written > 0) { done += (size_t)written; continue; }
        if (written < 0 && errno == EINTR) continue;
        return; // a dying process cannot do anything useful about a failed write
    }
}

// Every sink the report goes to, in one place: stderr (the journal's copy for
// the daemon), the debug log (the only copy a desktop-launched TUI has), and
// the dedicated crash-report file next to config.ini.
//
// The empty-emit guard is what makes "the file exists" mean "a crash wrote to
// it" without qualification: creating the file is a side effect of this call,
// so a zero-length emit must return before it can leave a 0-byte file behind —
// the exact artifact this design exists to stop producing.
static inline void gc_signal_safe_emit(const char* data, size_t length) {
    if (!data || length == 0) return;
    gc_signal_safe_write_fd(STDERR_FILENO, data, length);
    gc_signal_safe_write_fd((int)g_crashLogFd, data, length);
    gc_signal_safe_write_fd(gc_crash_report_open_on_demand(), data, length);
}

static inline void gc_signal_safe_write(const char* text) {
    if (!text) return;
    size_t length = 0;
    while (text[length]) length++;
    gc_signal_safe_emit(text, length);
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
    gc_signal_safe_emit(digits + index, sizeof(digits) - index);
}

// Addresses are the whole point of the report — they are what gets fed to
// llvm-symbolizer — so they are written in the 0x-prefixed hex form every
// symbolizer and disassembler accepts, again formatted by hand.
static inline void gc_signal_safe_write_hex(unsigned long long value) {
    static const char kDigits[] = "0123456789abcdef";
    char buffer[16];
    size_t index = sizeof(buffer);
    if (value == 0) buffer[--index] = '0';
    while (value > 0 && index > 0) {
        buffer[--index] = kDigits[value & 0xFull];
        value >>= 4;
    }
    gc_signal_safe_emit("0x", 2);
    gc_signal_safe_emit(buffer + index, sizeof(buffer) - index);
}

// Program counter and stack pointer at the fault, per architecture.  Without
// these the report names a signal but not a location, which is the difference
// between a bug report that can be acted on and one that cannot.  An
// unrecognised architecture reports zeroes rather than reading a wrong offset
// out of a struct that is laid out differently.
static inline void gc_fault_registers(void* contextPointer,
                                      unsigned long long* pcOut,
                                      unsigned long long* spOut) {
    *pcOut = 0;
    *spOut = 0;
    if (!contextPointer) return;
    ucontext_t* context = (ucontext_t*)contextPointer;
#if defined(__x86_64__)
    *pcOut = (unsigned long long)context->uc_mcontext.gregs[REG_RIP];
    *spOut = (unsigned long long)context->uc_mcontext.gregs[REG_RSP];
#elif defined(__aarch64__)
    *pcOut = (unsigned long long)context->uc_mcontext.pc;
    *spOut = (unsigned long long)context->uc_mcontext.sp;
#else
    (void)context;
#endif
}

// Stream /proc/self/maps into the report.  A PIE binary's load address is
// randomised, so a raw PC means nothing on its own; the maps line covering it
// is what turns it back into "module + file offset" for the symbolizer.  Uses
// open/read/write only, all async-signal-safe, with a stack buffer.
static inline void gc_signal_safe_write_maps() {
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    gc_signal_safe_write("--- /proc/self/maps ---\n");
    char buffer[1024];
    for (;;) {
        ssize_t got = read(fd, buffer, sizeof(buffer));
        if (got > 0) { gc_signal_safe_emit(buffer, (size_t)got); continue; }
        if (got < 0 && errno == EINTR) continue;
        break;
    }
    close(fd);
    gc_signal_safe_write("--- end maps ---\n");
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
// The report never suppresses the crash; it only makes it diagnosable.
static void gc_fatal_signal_handler(int signalNumber, siginfo_t* info, void* contextPointer) {
    gc_write_crash_breadcrumb("signal", (long)signalNumber);

    // si_code separates the causes a bare signal number conflates: a SIGSEGV
    // from a null dereference (SEGV_MAPERR) and one from a guard-page hit or a
    // write to read-only memory (SEGV_ACCERR) are very different bugs.
    gc_signal_safe_write("  code=");
    gc_signal_safe_write_int(info ? (long)info->si_code : -1);
    gc_signal_safe_write(" fault=");
    gc_signal_safe_write_hex(info ? (unsigned long long)(uintptr_t)info->si_addr : 0ull);

    unsigned long long pc = 0, sp = 0;
    gc_fault_registers(contextPointer, &pc, &sp);
    gc_signal_safe_write(" pc=");
    gc_signal_safe_write_hex(pc);
    gc_signal_safe_write(" sp=");
    gc_signal_safe_write_hex(sp);
    gc_signal_safe_write("\n");

    // Emitted with the report rather than only in the docs: the person holding
    // this file is the one who needs the command, and a static string costs
    // nothing here.
    gc_signal_safe_write("  symbolize: llvm-symbolizer --obj=greencurve.debug "
                         "<pc minus the greencurve module base from the map below>\n");
    gc_signal_safe_write_maps();

    raise(signalNumber);
}

static void gc_terminate_handler() {
    gc_write_crash_breadcrumb("terminate", 0);
    gc_signal_safe_write_maps();
    // Uncaught exception or a failed allocation.  Abort so the failure is still
    // visible to systemd as a crash rather than a silent clean exit.  SIGABRT is
    // handled above, so this still routes through the signal report.
    abort();
}

static inline void linux_install_crash_breadcrumbs(const char* role) {
    if (role) g_crashRole = role;
    if (g_crashBreadcrumbInstalled) return;
    g_crashBreadcrumbInstalled = 1;

    struct sigaction fatalAction = {};
    fatalAction.sa_sigaction = gc_fatal_signal_handler;
    sigemptyset(&fatalAction.sa_mask);
    // SA_SIGINFO is what makes si_code/si_addr and the register context
    // available; the handler is otherwise unchanged, and SA_RESETHAND still
    // guarantees the re-raise reaches the default disposition.
    fatalAction.sa_flags = SA_RESETHAND | SA_NODEFER | SA_SIGINFO;
    const int fatalSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    for (size_t i = 0; i < sizeof(fatalSignals) / sizeof(fatalSignals[0]); i++)
        sigaction(fatalSignals[i], &fatalAction, nullptr);

    std::set_terminate(gc_terminate_handler);
}

#endif // GREEN_CURVE_LINUX_CRASH_BREADCRUMB_H
