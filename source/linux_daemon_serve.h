// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_daemon_serve.h — listening-socket construction, the shutdown-aware
// accept loop, and the fan handback performed on the way out.  Included by
// linux_daemon.cpp after handle_request/log_peer exist.

#ifndef GREEN_CURVE_LINUX_DAEMON_SERVE_H
#define GREEN_CURVE_LINUX_DAEMON_SERVE_H

struct DaemonListener {
    int socketFd;
    int directoryFd;
};

// Root-owned, group-accessible.  The daemon updates and verifies the
// filesystem entry created by bind() as root:greencurve mode 0660.  Anyone who
// can connect is authorized; every request is still clamped by
// validate_desired_settings_for_ipc.
static bool daemon_open_listener(DaemonListener* out) {
    if (!out) return false;
    out->socketFd = -1;
    out->directoryFd = -1;

    umask(0077);
    if (mkdir(GC_DAEMON_SOCKET_DIR, 0755) != 0 && errno != EEXIST) {
        dlog("daemon: cannot create socket directory: %s\n", strerror(errno));
        return false;
    }
    int socketDir = open(GC_DAEMON_SOCKET_DIR,
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat socketDirStat = {};
    if (socketDir < 0 || fchmod(socketDir, 0755) != 0 ||
        fstat(socketDir, &socketDirStat) != 0 ||
        !S_ISDIR(socketDirStat.st_mode) || socketDirStat.st_uid != 0 ||
        (socketDirStat.st_mode & 0777) != 0755) {
        dlog("daemon: socket directory is not root-owned and protected\n");
        if (socketDir >= 0) close(socketDir);
        return false;
    }
    unlinkat(socketDir, GC_DAEMON_SOCKET_NAME, 0);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) {
        dlog("daemon: socket() failed\n");
        close(socketDir);
        return false;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    gc_strlcpy(addr.sun_path, sizeof(addr.sun_path), GC_DAEMON_SOCKET_PATH);
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        dlog("daemon: bind(%s) failed: %s\n", GC_DAEMON_SOCKET_PATH,
             strerror(errno));
        close(srv);
        close(socketDir);
        return false;
    }
    // Named separately: these fail for completely different reasons, and
    // reporting a permission failure as "listen() failed" -- with whatever
    // errno happened to survive from an earlier call -- sends the reader after
    // the wrong problem. Socket ownership is exactly the thing that goes wrong
    // here, so it has to say so.
    if (!configure_daemon_socket_permissions(socketDir)) {
        dlog("daemon: socket ownership/mode could not be applied to %s; "
             "expected root:greencurve mode 0660\n", GC_DAEMON_SOCKET_PATH);
        close(srv);
        unlinkat(socketDir, GC_DAEMON_SOCKET_NAME, 0);
        close(socketDir);
        return false;
    }
    if (listen(srv, GC_DAEMON_LISTEN_BACKLOG) != 0) {
        dlog("daemon: listen() failed: %s\n", strerror(errno));
        close(srv);
        unlinkat(socketDir, GC_DAEMON_SOCKET_NAME, 0);
        close(socketDir);
        return false;
    }
    out->socketFd = srv;
    out->directoryFd = socketDir;
    dlog("daemon: listening on %s\n", GC_DAEMON_SOCKET_PATH);
    return true;
}

static void daemon_close_listener(const DaemonListener* listener) {
    if (!listener) return;
    if (listener->socketFd >= 0) close(listener->socketFd);
    if (listener->directoryFd >= 0) {
        unlinkat(listener->directoryFd, GC_DAEMON_SOCKET_NAME, 0);
        close(listener->directoryFd);
    }
}

// Returns the process exit status.  Non-zero matters: the unit restarts on a
// failed exit, so a listener that has genuinely failed must not exit zero or
// systemd leaves the machine without GPU control.
//
// The loop is event-driven and stays that way.  poll() only grows a timeout
// when the manager asked for a watchdog (WatchdogSec= in the unit): the ping is
// a liveness protocol the manager owns the period of, not a poll for work, and
// with no watchdog configured the timeout is still -1 and nothing wakes this
// thread but a connection or the shutdown pipe.
static int daemon_serve_until_stopped(int srv) {
    // Reserved descriptor for the EMFILE/ENFILE recovery idiom: close it,
    // accept the pending connection, close that connection, then reopen the
    // reserve.  Drains the backlog without sleeps, polling delays, or spinning.
    int reservedFd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (reservedFd < 0)
        dlog("daemon: no reserved descriptor; fd exhaustion recovery is degraded\n");

    int exitStatus = 0;
    // poll() covers the listener and the shutdown self-pipe together, so
    // SIGTERM breaks the wait without a timeout, and one stalled peer cannot
    // hold the loop hostage before it is even accepted.
    set_nonblocking(srv);
    const unsigned int watchdogIntervalMs = linux_systemd_watchdog_interval_ms();
    const int pollTimeoutMs = watchdogIntervalMs ? (int)watchdogIntervalMs : -1;
    // The first ping is sent before the first wait, so a manager that set a
    // short WatchdogSec cannot time the daemon out during its own first
    // interval on a machine with no client traffic.
    linux_systemd_notify_watchdog();
    while (g_running) {
        struct pollfd waiters[2] = {};
        waiters[0].fd = srv;
        waiters[0].events = POLLIN;
        waiters[1].fd = g_shutdownPipe[0];
        waiters[1].events = POLLIN;
        int ready = poll(waiters, g_shutdownPipe[0] >= 0 ? 2 : 1, pollTimeoutMs);
        if (ready < 0) {
            if (errno == EINTR) continue;
            dlog("daemon: poll() failed: %s\n", strerror(errno));
            exitStatus = 1;
            break;
        }
        // Every wakeup pings, not only the timeout: reaching this line at all
        // is the evidence the watchdog exists to collect, and a busy daemon
        // would otherwise never hit its timeout branch and would be killed for
        // doing its job.
        linux_systemd_notify_watchdog();
        if (ready == 0) continue;
        if (g_shutdownPipe[0] >= 0 && (waiters[1].revents & POLLIN)) {
            dlog("daemon: stop signal received; shutting down\n");
            break;
        }
        if (!(waiters[0].revents & (POLLIN | POLLERR | POLLHUP))) continue;

        int conn = accept(srv, nullptr, nullptr);
        if (conn < 0) {
            int acceptErrno = errno;
            DaemonAcceptDisposition disposition =
                daemon_accept_disposition(acceptErrno);
            if (disposition == DAEMON_ACCEPT_RETRY) {
                if (acceptErrno != EINTR && acceptErrno != EAGAIN)
                    dlog("daemon: accept() transient failure, retrying: %s\n",
                         strerror(acceptErrno));
                continue;
            }
            if (disposition == DAEMON_ACCEPT_RECLAIM_FD) {
                dlog("daemon: accept() hit descriptor exhaustion (%s); reclaiming reserve\n",
                     strerror(acceptErrno));
                if (reservedFd >= 0) {
                    close(reservedFd);
                    int drained = accept(srv, nullptr, nullptr);
                    if (drained >= 0) close(drained);
                    // Reopen unconditionally; -1 here simply means the next
                    // exhaustion is handled without a reserve.
                    reservedFd = open("/dev/null", O_RDONLY | O_CLOEXEC);
                }
                continue;
            }
            dlog("daemon: accept() failed fatally: %s\n", strerror(acceptErrno));
            exitStatus = 1;
            break;
        }
        set_nonblocking(conn);
        log_peer(conn);
        ServiceRequest req;
        if (daemon_read_request(conn, &req)) {
            ServiceResponse resp;
            handle_request(&req, &resp);
            daemon_write_response(conn, &resp);
        }
        close(conn);
    }
    if (reservedFd >= 0) close(reservedFd);
    return exitStatus;
}

// The fan runtime holds the GPU at a manual duty and nothing re-asserts it once
// this process exits, so hand the fan back to the driver before leaving.
// Otherwise `systemctl stop` strands the fan at the last written percentage.
// Call only after the fan worker is joined, so the two cannot race.
static void daemon_release_fan_to_driver() {
    if (!(g_gpuReady && g_hasActiveDesired && g_activeDesired.hasFan &&
          g_activeDesired.fanMode != FAN_MODE_AUTO))
        return;
    bool autoOk = linux_backend_set_fan_auto(&g_gpu);
    dlog("daemon: shutdown fan handback: driver auto restore ok=%d\n",
         autoOk ? 1 : 0);
    if (fan_runtime_escalation_after_auto_restore(autoOk) ==
            FAN_RUNTIME_ESCALATION_EMERGENCY_MAX) {
        bool emergencyOk = linux_backend_set_curve_fan_percent(
            &g_gpu, (unsigned int)FAN_RUNTIME_EMERGENCY_PERCENT);
        dlog("daemon: shutdown fan handback failed; forced %d%% ok=%d\n",
             FAN_RUNTIME_EMERGENCY_PERCENT, emergencyOk ? 1 : 0);
    }
}

#endif // GREEN_CURVE_LINUX_DAEMON_SERVE_H
