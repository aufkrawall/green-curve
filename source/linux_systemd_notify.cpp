// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included by linux_daemon.cpp; do not compile separately.

// Minimal sd_notify-compatible sender.  This intentionally avoids a link-time
// libsystemd dependency while retaining Type=notify startup semantics and,
// since the unit gained WatchdogSec=, the liveness protocol that goes with it.
#include "linux_systemd_notify_policy.h"

// Resolved once from NOTIFY_SOCKET and then owned by this file.  The
// environment variable is deliberately cleared afterwards so a child process
// cannot notify the manager on our behalf -- but the watchdog needs to keep
// sending long after that, so the address itself has to survive the unset.
static struct sockaddr_un g_notifyAddress = {};
static socklen_t g_notifyAddressLength = 0;
static bool g_notifyResolved = false;
// Half of WatchdogSec, in milliseconds; zero when the manager did not ask for
// a watchdog (a manually launched --daemon, or a unit without WatchdogSec=).
static unsigned int g_watchdogIntervalMs = 0;

static bool linux_systemd_resolve_notify_socket(char* error, size_t errorSize) {
    if (g_notifyResolved) return g_notifyAddressLength != 0;
    g_notifyResolved = true;
    const char* notifySocket = getenv("NOTIFY_SOCKET");
    if (!notifySocket || !notifySocket[0]) return false;

    size_t nameLength = strlen(notifySocket);
    if (nameLength == 0 || nameLength >= sizeof(g_notifyAddress.sun_path)) {
        if (error) gc_strlcpy(error, errorSize,
            "NOTIFY_SOCKET path is empty or too long");
        return false;
    }
    memset(&g_notifyAddress, 0, sizeof(g_notifyAddress));
    g_notifyAddress.sun_family = AF_UNIX;
    if (notifySocket[0] == '@') {
        g_notifyAddress.sun_path[0] = '\0';
        memcpy(g_notifyAddress.sun_path + 1, notifySocket + 1, nameLength - 1);
    } else {
        memcpy(g_notifyAddress.sun_path, notifySocket, nameLength + 1);
    }
    g_notifyAddressLength = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
        nameLength + (notifySocket[0] == '@' ? 0 : 1));
    return true;
}

static bool linux_systemd_send(const char* payload, char* error,
                               size_t errorSize) {
    if (!payload || !payload[0]) return false;
    if (g_notifyAddressLength == 0) return false;
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        if (error) gc_snprintf(error, errorSize,
            "cannot open systemd notify socket: %s", strerror(errno));
        return false;
    }
    size_t length = strlen(payload);
    ssize_t sent = sendto(fd, payload, length, MSG_NOSIGNAL,
        (const struct sockaddr*)&g_notifyAddress, g_notifyAddressLength);
    int sendError = errno;
    close(fd);
    if (sent != (ssize_t)length) {
        if (error) gc_snprintf(error, errorSize,
            "systemd notification failed: %s",
            sent < 0 ? strerror(sendError) : "short datagram write");
        return false;
    }
    return true;
}

// WATCHDOG_USEC is the full deadline; the documented contract is to ping at
// half of it, which leaves a whole interval of slack for a late wakeup before
// the manager decides the daemon is wedged.  WATCHDOG_PID, when present, names
// the process the manager expects the pings from: a daemon that ignored it
// would keep a unit alive from the wrong process after a re-exec.
static void linux_systemd_arm_watchdog() {
    g_watchdogIntervalMs = 0;
    const char* usecText = getenv("WATCHDOG_USEC");
    const char* pidText = getenv("WATCHDOG_PID");
    if (pidText && pidText[0]) {
        long expectedPid = strtol(pidText, nullptr, 10);
        if (expectedPid != (long)getpid()) {
            dlog("daemon: WATCHDOG_PID=%ld is not this process (%ld); "
                 "not sending watchdog pings\n",
                 expectedPid, (long)getpid());
            return;
        }
    }
    if (!usecText || !usecText[0]) return;
    long long usec = strtoll(usecText, nullptr, 10);
    if (usec <= 0) {
        dlog("daemon: WATCHDOG_USEC='%s' is not a usable interval; "
             "not sending watchdog pings\n", usecText);
        return;
    }
    long long halfMs = (usec / 2) / 1000;
    // A sub-millisecond half-interval would turn the serve loop into a spin.
    // Anything the manager configures below 20 ms is treated as 20 ms; that is
    // still two orders of magnitude tighter than the unit's own WatchdogSec.
    if (halfMs < 20) halfMs = 20;
    if (halfMs > 3600000) halfMs = 3600000;
    g_watchdogIntervalMs = (unsigned int)halfMs;
    dlog("daemon: systemd watchdog armed; deadline=%lldus ping interval=%ums\n",
         usec, g_watchdogIntervalMs);
}

// Zero means "no watchdog": the serve loop then keeps its infinite poll() and
// stays purely event-driven, exactly as before this file grew a timer.
static unsigned int linux_systemd_watchdog_interval_ms() {
    return g_watchdogIntervalMs;
}

static bool linux_systemd_notify_watchdog() {
    if (g_watchdogIntervalMs == 0) return true;
    char error[160] = {};
    if (linux_systemd_send("WATCHDOG=1", error, sizeof(error))) return true;
    // Logged deduplicated by dlog's caller-side change detection upstream; a
    // permanently failing ping is a real fault (the manager will restart us),
    // and a transient one must not bury the journal at twice per interval.
    dlog("daemon: watchdog ping failed: %s\n",
         error[0] ? error : "unknown error");
    return false;
}

static bool linux_systemd_notify_ready(const ServiceGpuHealth* health,
                                       char* error, size_t errorSize) {
    if (error && errorSize) error[0] = 0;
    if (!linux_systemd_resolve_notify_socket(error, errorSize)) {
        // A manually launched --daemon has no systemd notification endpoint.
        // A malformed NOTIFY_SOCKET fills `error` and is a real failure.
        return error && errorSize && error[0] ? false : true;
    }

    char payload[384] = {};
    if (!linux_systemd_build_ready_payload(
            health, payload, sizeof(payload))) {
        if (error) gc_strlcpy(error, errorSize,
            "systemd READY payload exceeded its bounded buffer");
        return false;
    }
    if (!linux_systemd_send(payload, error, errorSize)) return false;

    linux_systemd_arm_watchdog();
    // Do not accidentally notify a manager inherited by later child processes.
    // The resolved address above keeps the watchdog working past this point.
    unsetenv("NOTIFY_SOCKET");
    unsetenv("WATCHDOG_USEC");
    unsetenv("WATCHDOG_PID");
    dlog("daemon: systemd readiness sent after startup replay and socket listen\n");
    return true;
}
