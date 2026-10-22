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

// Per-peer admission control, reusing the SAME pure policy the Windows pipe
// transport runs (service_ipc_throttle_policy.h).  That module is host-neutral
// by construction -- a monotonic millisecond clock in, a decision out, no OS
// types and no allocation -- so this is a port of a control the project already
// ships and tests, not a second design.
//
// The gap it closes: the serve loop is single-threaded and handles one request
// to completion before the next accept(), with a backlog of 8, so a peer
// flooding requests fills the backlog and every other client's connect() then
// returns EAGAIN, which the client is told to read as "the daemon is busy".
// The bucket bounds the request RATE per account.  Time the loop spends waiting
// on a stalled peer is rationed separately, in time (linux_daemon_stall_policy.h).
// Handler time is not rationed: an account issuing back-to-back legitimate
// mutations keeps other clients waiting, which is a design limit, not a fault.
//
// Identity is the peer uid from the ucred log_peer() already reads, formatted
// as a stable string.  Deliberately NOT the bare pid: a client that forks gets
// a fresh pid every time and would never be metered.
static ServiceIpcAdmissionTable g_daemonAdmissionTable;

static void daemon_admission_key(int connFd, ServiceIpcThrottleKey* key) {
    key->clear();
    struct ucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(connFd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 || len != sizeof(cred)) return;
    char uid[32] = {};
    int written = snprintf(uid, sizeof(uid), "uid:%lu", (unsigned long)cred.uid);
    if (written <= 0) return;
    // UID alone bounds the account: changing a primary group cannot buy a
    // fresh quota. SO_PEERCRED supplies the kernel-authenticated identity.
    key->fill(0, 0, uid);
}

static bool daemon_admission_admit(int connFd, ServiceIpcThrottleKey* key) {
    daemon_admission_key(connFd, key);
    if (!key->valid) {
        dlog("daemon: refusing a peer with unavailable kernel identity\n");
        return false;
    }
    const ServiceIpcAdmissionDecision decision = service_ipc_decide_admission(
        &g_daemonAdmissionTable, *key, SERVICE_IPC_CLASS_NORMAL,
        monotonic_ms());
    if (decision != SERVICE_IPC_ADMITTED) {
        dlog("daemon: refusing a rate-limited peer (the daemon is serving a "
             "burst from one client; this is a per-peer budget, not a policy "
             "decision)\n");
    }
    return decision == SERVICE_IPC_ADMITTED;
}

static void daemon_admission_charge(const ServiceIpcThrottleKey* key,
                                    unsigned int costTokens) {
    if (!key || !key->valid) return;
    service_ipc_charge(&g_daemonAdmissionTable, *key, SERVICE_IPC_CLASS_NORMAL,
                       costTokens, monotonic_ms());
}

// An admitted exchange, successful or not: the outcome's token cost (the
// weighting the Windows transport uses), plus the time the peer made the loop
// wait in the request read and response write, repaid as a hold.  One line per
// hold, no identity in it; a peer can earn at most one per hold period.
static void daemon_admission_settle(const ServiceIpcThrottleKey* key,
                                    GcDaemonExchangeOutcome outcome,
                                    unsigned long long readMs,
                                    unsigned long long writeMs) {
    if (!key) return;
    const GcDaemonExchangeSettlement settled = gc_daemon_exchange_account(
        &g_daemonAdmissionTable, *key, outcome, readMs, writeMs, monotonic_ms());
    if (!key->valid || settled.holdMs == 0) return;
    dlog("daemon: peer held the loop %llu ms in transport (request read %llu ms, "
         "response write %llu ms, %s); refusing that account for %llu ms "
         "(stall rationing)\n", settled.transportMs, readMs,
         outcome == GC_DAEMON_EXCHANGE_READ_FAILED ? 0ull : writeMs,
         gc_daemon_exchange_outcome_name(outcome), settled.holdMs);
}

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
        // Metered BEFORE the request is read, so a peer that floods spends its
        // own budget rather than the backlog's.  The charge happens after the
        // outcome is known, so a transport fault weighs more than a completed
        // request -- the same weighting the Windows transport uses.  A failed
        // response write is a fault too: a peer that stops reading its answer
        // holds the loop exactly like one that never sends its request.  The
        // hold is decided by the transport time of BOTH phases whatever the
        // outcome, so a request that completes just inside its deadline is
        // rationed like one that misses it.
        ServiceIpcThrottleKey throttleKey;
        const bool admitted = daemon_admission_admit(conn, &throttleKey);
        if (!admitted) {
            // Refused peers never get a read or write deadline in the accept
            // loop. Best-effort nonblocking output tells healthy clients busy.
            daemon_answer_busy(conn);
            daemon_admission_charge(&throttleKey, 0);
        } else {
            GcDaemonExchangeOutcome outcome = GC_DAEMON_EXCHANGE_READ_FAILED;
            const unsigned long long readStartMs = monotonic_ms();
            const bool requestRead = daemon_read_request(conn, &req);
            const unsigned long long readMs = monotonic_ms() - readStartMs;
            unsigned long long writeMs = 0;
            if (requestRead) {
                ServiceResponse resp;
                handle_request(&req, &resp);
                // Measured from here so handler time never counts as a stall.
                const unsigned long long writeStartMs = monotonic_ms();
                const bool responded = daemon_write_response(conn, &resp);
                writeMs = monotonic_ms() - writeStartMs;
                outcome = responded ? GC_DAEMON_EXCHANGE_COMPLETED
                                    : GC_DAEMON_EXCHANGE_WRITE_FAILED;
            }
            daemon_admission_settle(&throttleKey, outcome, readMs, writeMs);
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
          g_activeDesired.fanMode != FAN_MODE_AUTO)) {
        // Nothing manual is held.  A marker left from a handback that could
        // not run yet (GPU not ready) is kept for the next start in this boot.
        if (g_fanOwnershipMarkerCommitted)
            daemon_fan_ownership_retire("clean stop with no manual fan held");
        return;
    }
    bool autoOk = linux_backend_set_fan_auto(&g_gpu);
    dlog("daemon: shutdown fan handback: driver auto restore ok=%d\n",
         autoOk ? 1 : 0);
    // Only a confirmed handback retires the marker; a forced emergency duty is
    // still a manual fan the next start should return.
    if (autoOk) daemon_fan_ownership_retire("shutdown fan handback");
    if (fan_runtime_escalation_after_auto_restore(autoOk) ==
            FAN_RUNTIME_ESCALATION_EMERGENCY_MAX) {
        bool emergencyOk = linux_backend_set_curve_fan_percent(
            &g_gpu, (unsigned int)FAN_RUNTIME_EMERGENCY_PERCENT);
        dlog("daemon: shutdown fan handback failed; forced %d%% ok=%d\n",
             FAN_RUNTIME_EMERGENCY_PERCENT, emergencyOk ? 1 : 0);
    }
}

#endif // GREEN_CURVE_LINUX_DAEMON_SERVE_H
