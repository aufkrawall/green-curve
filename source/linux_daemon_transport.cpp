// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included by linux_daemon.cpp; do not compile separately.

// Header-first, deadline-bounded Unix-socket transport.  Keeping the fixed
// eight-byte magic/version prefix independently readable lets a newly upgraded
// client diagnose an older, shorter daemon response without first waiting for
// a body that can never arrive.  The server follows the same rule for requests.

#include "linux_daemon_transport_policy.h"
// Every deadline below is DERIVED there, per role, instead of being one literal
// shared by four roles that ask different questions.  See that header for the
// 2026-09-12 defect this replaced.
#include "linux_daemon_deadline_policy.h"

static unsigned long long monotonic_ms() {
    struct timespec ts = {};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (unsigned long long)ts.tv_sec * 1000ULL +
        (unsigned long long)(ts.tv_nsec / 1000000ULL);
}

static bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

enum DaemonWaitResult {
    DAEMON_WAIT_READY = 0,
    DAEMON_WAIT_TIMEOUT,
    DAEMON_WAIT_ERROR,
};

static DaemonWaitResult wait_fd_ready(int fd, short events,
                                      unsigned long long deadlineMs,
                                      int* errorNumber) {
    if (errorNumber) *errorNumber = 0;
    for (;;) {
        unsigned long long now = monotonic_ms();
        if (now >= deadlineMs) return DAEMON_WAIT_TIMEOUT;
        unsigned long long remaining = deadlineMs - now;
        int timeout = remaining > 2147483647ULL
            ? 2147483647 : (int)remaining;
        struct pollfd pfd = {};
        pfd.fd = fd;
        pfd.events = events;
        int result = poll(&pfd, 1, timeout);
        if (result > 0) {
            // A read must be attempted on HUP so buffered bytes are drained and
            // a zero return can be classified as EOF versus truncation.
            if (pfd.revents & (events | POLLHUP)) return DAEMON_WAIT_READY;
            if (errorNumber) *errorNumber = EIO;
            return DAEMON_WAIT_ERROR;
        }
        if (result == 0) return DAEMON_WAIT_TIMEOUT;
        if (errno == EINTR) continue;
        if (errorNumber) *errorNumber = errno;
        return DAEMON_WAIT_ERROR;
    }
}

// Absolute-deadline I/O.  The deadline, not a per-call timeout, is the unit of
// budgeting, because one request is several transfers: giving each transfer its
// own fresh timeout silently multiplies the bound nobody wrote down.  Before
// this a client response cost "2000 ms" on paper and up to 4000 ms in practice
// (prefix, then body), and the daemon's per-peer stall bound had the same flaw.
static DaemonIoResult daemon_read_exact_until(
    int fd, void* buffer, size_t length, unsigned long long deadline) {
    DaemonIoResult result = {DAEMON_IO_NONE, 0, length, 0};
    unsigned char* bytes = (unsigned char*)buffer;
    while (result.transferred < length) {
        int waitError = 0;
        DaemonWaitResult wait = wait_fd_ready(fd, POLLIN, deadline, &waitError);
        if (wait == DAEMON_WAIT_TIMEOUT) {
            result.failure = DAEMON_IO_TIMEOUT;
            return result;
        }
        if (wait == DAEMON_WAIT_ERROR) {
            result.failure = DAEMON_IO_ERROR;
            result.errorNumber = waitError;
            return result;
        }
        ssize_t count = read(fd, bytes + result.transferred,
                             length - result.transferred);
        if (count > 0) {
            result.transferred += (size_t)count;
            continue;
        }
        if (count == 0) {
            result.failure = DAEMON_IO_EOF;
            return result;
        }
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
        result.failure = DAEMON_IO_ERROR;
        result.errorNumber = errno;
        return result;
    }
    return result;
}

static DaemonIoResult daemon_write_exact_until(
    int fd, const void* buffer, size_t length, unsigned long long deadline) {
    DaemonIoResult result = {DAEMON_IO_NONE, 0, length, 0};
    const unsigned char* bytes = (const unsigned char*)buffer;
    while (result.transferred < length) {
        int waitError = 0;
        DaemonWaitResult wait = wait_fd_ready(fd, POLLOUT, deadline, &waitError);
        if (wait == DAEMON_WAIT_TIMEOUT) {
            result.failure = DAEMON_IO_TIMEOUT;
            return result;
        }
        if (wait == DAEMON_WAIT_ERROR) {
            result.failure = DAEMON_IO_ERROR;
            result.errorNumber = waitError;
            return result;
        }
        // MSG_NOSIGNAL prevents a legacy peer that closes after reading its
        // shorter v8/v11 structure from terminating the daemon with SIGPIPE.
        ssize_t count = send(fd, bytes + result.transferred,
                             length - result.transferred, MSG_NOSIGNAL);
        if (count > 0) {
            result.transferred += (size_t)count;
            continue;
        }
        if (count == 0) {
            result.failure = DAEMON_IO_EOF;
            return result;
        }
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
        result.failure = DAEMON_IO_ERROR;
        result.errorNumber = errno;
        return result;
    }
    return result;
}

// Relative-timeout wrappers.  Kept because one transfer with its own budget is
// the natural shape for a test fixture; production callers thread a deadline so
// a multi-transfer exchange stays bounded as a whole.
static DaemonIoResult daemon_read_exact_with_timeout(
    int fd, void* buffer, size_t length, unsigned int timeoutMs) {
    return daemon_read_exact_until(fd, buffer, length,
                                   monotonic_ms() + timeoutMs);
}

static DaemonIoResult daemon_write_exact_with_timeout(
    int fd, const void* buffer, size_t length, unsigned int timeoutMs) {
    return daemon_write_exact_until(fd, buffer, length,
                                    monotonic_ms() + timeoutMs);
}

static void format_io_failure(char* error, size_t errorSize,
                              const char* phase, const DaemonIoResult& io,
                              size_t alreadyTransferred,
                              size_t totalExpected) {
    if (!error || errorSize == 0) return;
    size_t total = alreadyTransferred + io.transferred;
    const char* classification = daemon_io_failure_classification(
        io.failure, total);
    if (io.failure == DAEMON_IO_ERROR && io.errorNumber) {
        gc_snprintf(error, errorSize,
            "daemon %s %s after %zu of %zu bytes: %s",
            phase, classification, total, totalExpected,
            strerror(io.errorNumber));
    } else {
        gc_snprintf(error, errorSize,
            "daemon %s %s after %zu of %zu bytes",
            phase, classification, total, totalExpected);
    }
}

static bool process_has_supplementary_group(gid_t groupId, bool* known) {
    if (known) *known = false;
    gid_t stackGroups[64];
    int count = getgroups(0, nullptr);
    if (count < 0 || count > 1024) return false;
    gid_t* groups = stackGroups;
    gid_t* heapGroups = nullptr;
    if (count > (int)(sizeof(stackGroups) / sizeof(stackGroups[0]))) {
        heapGroups = (gid_t*)malloc((size_t)count * sizeof(gid_t));
        if (!heapGroups) return false;
        groups = heapGroups;
    }
    int received = count > 0 ? getgroups(count, groups) : 0;
    if (received < 0) {
        free(heapGroups);
        return false;
    }
    bool found = false;
    for (int i = 0; i < received; ++i) {
        if (groups[i] == groupId) { found = true; break; }
    }
    free(heapGroups);
    if (known) *known = true;
    return found;
}

static void format_permission_diagnostic(char* error, size_t errorSize,
                                         int connectError) {
    if (!error || errorSize == 0) return;
    // strerror() may return thread-local/static storage that is replaced by
    // the next call. Copy each message immediately so the later metadata
    // lookup cannot silently rewrite the earlier connect diagnostic.
    char connectErrorText[128] = {};
    gc_strlcpy(connectErrorText, sizeof(connectErrorText),
               strerror(connectError));
    struct stat socketStat = {};
    bool statOk = lstat(GC_DAEMON_SOCKET_PATH, &socketStat) == 0;
    int metadataError = statOk ? 0 : errno;
    char metadataErrorText[128] = {};
    gc_strlcpy(metadataErrorText, sizeof(metadataErrorText),
               statOk ? "none" : strerror(metadataError));
    struct group* adminGroup = getgrnam("greencurve");
    bool supplementaryKnown = false;
    bool supplementary = adminGroup && process_has_supplementary_group(
        adminGroup->gr_gid, &supplementaryKnown);
    struct group* socketGroup = statOk ? getgrgid(socketStat.st_gid) : nullptr;
    LinuxDaemonPermissionFacts facts = {};
    facts.socketMetadataAvailable = statOk;
    facts.socketPath = GC_DAEMON_SOCKET_PATH;
    facts.connectError = connectErrorText;
    facts.metadataError = metadataErrorText;
    facts.socketOwnerUid = (unsigned int)socketStat.st_uid;
    facts.socketGroupName = socketGroup && socketGroup->gr_name
        ? socketGroup->gr_name : "?";
    facts.socketGroupId = (unsigned int)socketStat.st_gid;
    facts.socketMode = (unsigned int)(socketStat.st_mode & 07777);
    facts.processEuid = (unsigned int)geteuid();
    facts.processPrimaryGid = (unsigned int)getegid();
    facts.supplementaryGreencurve = supplementaryKnown
        ? (supplementary ? 1 : 0) : -1;
    linux_daemon_format_permission_facts(&facts, error, errorSize);
}

// Client-side failures repeat once per refresh tick (the TUI polls at 1 Hz), so
// logging every one would bury the first occurrence under thousands of copies.
// Deduplicated on the message text: a *changed* failure is always recorded.
static void log_client_failure(const char* stage, const char* detail) {
    static char lastLogged[512] = {};
    if (!detail || !detail[0]) return;
    if (strcmp(lastLogged, detail) == 0) return;
    gc_strlcpy(lastLogged, sizeof(lastLogged), detail);
    dlog("daemon client: %s failed: %s\n", stage ? stage : "request", detail);
}

// Written once per process, before anything can fail, so the log always answers
// "was this user actually able to talk to the daemon, and why not" -- including
// the runs where a later request happened to succeed.
// The enabled check lives at the call site, not here: this file is also
// compiled standalone by tests/linux_transport_regression.cpp, which supplies
// its own dlog() stub and does not link the debug-log sink.
void linux_daemon_log_client_environment() {
    struct group* adminGroup = getgrnam("greencurve");
    bool supplementaryKnown = false;
    bool supplementary = adminGroup && process_has_supplementary_group(
        adminGroup->gr_gid, &supplementaryKnown);
    dlog("client environment: euid=%u primary_gid=%u greencurve group=%s "
         "membership=%s\n",
         (unsigned int)geteuid(), (unsigned int)getegid(),
         adminGroup ? "present" : "MISSING (daemon never installed?)",
         !supplementaryKnown ? "unknown" : supplementary ? "yes" : "no");

    struct stat socketStatus = {};
    if (lstat(GC_DAEMON_SOCKET_PATH, &socketStatus) != 0) {
        dlog("client environment: socket %s unavailable: %s "
             "(is greencurve.service running? check: systemctl status greencurve)\n",
             GC_DAEMON_SOCKET_PATH, strerror(errno));
        return;
    }
    struct group* socketGroup = getgrgid(socketStatus.st_gid);
    dlog("client environment: socket %s type=%s owner_uid=%u group=%s(%u) mode=%04o\n",
         GC_DAEMON_SOCKET_PATH,
         S_ISSOCK(socketStatus.st_mode) ? "socket" : "UNEXPECTED",
         (unsigned int)socketStatus.st_uid,
         socketGroup && socketGroup->gr_name ? socketGroup->gr_name : "?",
         (unsigned int)socketStatus.st_gid,
         (unsigned int)(socketStatus.st_mode & 07777));

    // access() answers the question the user actually has ("can I use it?")
    // instead of making them derive it from the mode bits above.  The remedy is
    // printed only when access really fails: advertising `usermod` on a run that
    // works would send someone chasing a non-problem.
    if (access(GC_DAEMON_SOCKET_PATH, R_OK | W_OK) == 0) return;
    dlog("client environment: this process CANNOT use the socket: %s\n",
         strerror(errno));
    if (supplementaryKnown && !supplementary) {
        dlog("client environment: remedy: sudo usermod -aG greencurve \"$USER\", "
             "then sign out and back in (or run: newgrp greencurve). A group "
             "added in this session does not apply to already-running shells.\n");
    } else if (!adminGroup) {
        dlog("client environment: remedy: the greencurve group does not exist; "
             "install the daemon first (sudo ./greencurve-setup.sh install, or "
             "sudo greencurve --service-install).\n");
    } else {
        dlog("client environment: the greencurve group is present and this "
             "process has it, so the socket permissions themselves are wrong; "
             "expected root:greencurve mode 0660. Restart the daemon: "
             "sudo systemctl restart greencurve\n");
    }
}

// Connect while preserving errno for actionable unprivileged diagnostics.
//
// The socket is non-blocking BEFORE connect(), not after.  A blocking AF_UNIX
// connect() against a full listen backlog waits for the daemon to drain it,
// with no bound at all -- the one wait in this client that no deadline covered,
// and precisely the situation a saturated single-threaded daemon produces.
// Non-blocking turns it into EAGAIN, which is reported as the truthful
// "daemon is saturated" instead of a hang, and adds no retry loop: the kernel
// answers AF_UNIX connect() immediately in every other case too.
static int client_connect(int* connectErrno) {
    if (connectErrno) *connectErrno = 0;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        if (connectErrno) *connectErrno = errno;
        return -1;
    }
    struct sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    gc_strlcpy(address.sun_path, sizeof(address.sun_path),
               GC_DAEMON_SOCKET_PATH);
    int rc = connect(fd, (struct sockaddr*)&address, sizeof(address));
    if (rc != 0 && (errno == EINPROGRESS || errno == EINTR)) {
        // Completion is collected through poll() rather than by retrying
        // connect(), and it is bounded by the availability budget -- which is
        // deliberately NOT the turnaround budget: "is the daemon up?" must
        // still be answered promptly however patient the response deadline is.
        int waitError = 0;
        unsigned long long deadline =
            monotonic_ms() + GC_DAEMON_CONNECT_BUDGET_MS;
        if (wait_fd_ready(fd, POLLOUT, deadline, &waitError) ==
                DAEMON_WAIT_READY) {
            int soError = 0;
            socklen_t soLength = (socklen_t)sizeof(soError);
            rc = (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &soLength) == 0 &&
                  soError == 0) ? 0 : -1;
            if (rc != 0) errno = soError ? soError : EIO;
        } else {
            errno = waitError ? waitError : ETIMEDOUT;
            rc = -1;
        }
    }
    if (rc != 0) {
        int failure = errno;
        close(fd);
        if (connectErrno) *connectErrno = failure;
        return -1;
    }
    return fd;
}

static void log_daemon_identity_transition(const ServiceResponse* response) {
    static gc_u32 loggedPid = 0;
    static gc_u32 loggedBuild = 0;
    static gc_u32 loggedProtocol = 0;
    static char loggedVersion[32] = {};
    if (!response ||
        (loggedPid == response->servicePid &&
         loggedBuild == response->serviceBuildNumber &&
         loggedProtocol == response->version &&
         strcmp(loggedVersion, response->serviceVersion) == 0)) return;
    loggedPid = response->servicePid;
    loggedBuild = response->serviceBuildNumber;
    loggedProtocol = response->version;
    gc_strlcpy(loggedVersion, sizeof(loggedVersion), response->serviceVersion);
    dlog("daemon client: connected version=%s build=%u protocol=%u pid=%u\n",
         response->serviceVersion, response->serviceBuildNumber,
         response->version, response->servicePid);
}

// One exchange took an unusual share of its own budget.  Logged on a per-class
// high-water mark so a persistently slow daemon produces a handful of lines
// rather than one per refresh tick, and so the first occurrence is never buried.
//
// This is the instrumentation whose absence is why the Linux deadlines could
// only be argued about: the 2026-09-11 Windows incident was diagnosable because
// the log carried real turnaround percentiles, and nothing here recorded any.
static void note_exchange_duration(unsigned int command,
                                   unsigned long long elapsedMs,
                                   unsigned long budgetMs) {
    static unsigned long long highWater[4] = {};
    LinuxDaemonDeadlineClass commandClass = linux_daemon_deadline_class(command);
    unsigned long long& seen = highWater[(int)commandClass];
    if (budgetMs == 0 || elapsedMs <= seen) return;
    seen = elapsedMs;
    // Below half the budget the derivation is comfortably right and the line
    // would be noise; above it, the number is the evidence a future audit needs.
    if (elapsedMs * 2ULL < (unsigned long long)budgetMs) return;
    dlog("daemon client: command=%u class=%d slow exchange %llums of %lums "
         "budget (new high-water for this class)\n",
         command, (int)commandClass, elapsedMs, budgetMs);
}

// A deadline expiry on the RESPONSE says the daemon accepted the request and
// then failed its own contract.  Reporting only "timeout" left the reader
// unable to tell that from an unreachable daemon, which is the confusion that
// made a slow apply present as a failed one.
static void format_response_timeout(char* error, size_t errorSize,
                                    const char* phase, unsigned int command,
                                    unsigned long budgetMs,
                                    unsigned long long elapsedMs) {
    if (!error || errorSize == 0) return;
    gc_snprintf(error, errorSize,
        "daemon accepted the request (command %u) but did not answer within "
        "%lu ms (%s, waited %llu ms); it is busy or wedged, not absent",
        command, budgetMs, phase, elapsedMs);
}

// `totalTimeoutMs` bounds the whole request/response exchange, not each
// transfer.  `outcome` reports what a failure proves about the daemon's
// existence and whether it was a deadline expiry; see
// linux_daemon_deadline_policy.h.
static bool linux_daemon_send_deadline(const ServiceRequest* request,
                                       ServiceResponse* response,
                                       unsigned long totalTimeoutMs,
                                       LinuxDaemonSendOutcome* outcome,
                                       char* error, size_t errorSize) {
    if (error && errorSize) error[0] = 0;
    if (outcome) {
        outcome->reachability = LINUX_DAEMON_REACHABILITY_UNKNOWN;
        outcome->deadlineExpired = false;
    }
    if (!request || !response) {
        if (error) gc_strlcpy(error, errorSize, "invalid daemon request buffer");
        return false;
    }
    memset(response, 0, sizeof(*response));
    const unsigned int command = (unsigned int)request->command;
    if (totalTimeoutMs == 0ul) {
        // The recovery loop exhausted its total budget. Attempting the exchange
        // anyway would be a zero-length deadline dressed up as a request.
        if (error) gc_snprintf(error, errorSize,
            "daemon request (command %u) has no response budget left", command);
        log_client_failure("deadline", error);
        return false;
    }
    int connectErrno = 0;
    int fd = client_connect(&connectErrno);
    if (fd < 0) {
        if (outcome)
            outcome->reachability =
                linux_daemon_connect_reachability(connectErrno);
        if (connectErrno == EACCES || connectErrno == EPERM) {
            format_permission_diagnostic(error, errorSize, connectErrno);
        } else if (linux_daemon_connect_is_saturated(connectErrno)) {
            // Positive evidence of a listening daemon whose backlog is full.
            // Requests are serviced one at a time, so this is what a long
            // mutation with several waiting clients looks like from outside.
            if (error) gc_snprintf(error, errorSize,
                "daemon at %s is not accepting connections right now "
                "(backlog of %d full): it is busy, not stopped",
                GC_DAEMON_SOCKET_PATH, GC_DAEMON_LISTEN_BACKLOG);
        } else if (error) {
            gc_snprintf(error, errorSize,
                "daemon not reachable at %s: %s (is greencurve.service running?)",
                GC_DAEMON_SOCKET_PATH,
                connectErrno ? strerror(connectErrno) : "unknown error");
        }
        // The caller may only have room for a truncated status line; the log
        // gets the whole diagnostic, including the group remedy.
        log_client_failure("connect", error);
        return false;
    }
    // Past this point a daemon demonstrably owns the socket.  Nothing that
    // happens next is evidence that it does not.
    if (outcome) outcome->reachability = LINUX_DAEMON_REACHABILITY_CONNECTED;

    // Availability and turnaround are separate budgets, so the exchange
    // deadline starts HERE: time spent finding the daemon is not charged
    // against the daemon's time to answer.  Within the exchange, ONE deadline
    // covers request write + response header + response body together.
    const unsigned long long exchangeStart = monotonic_ms();
    const unsigned long long exchangeDeadline = exchangeStart + totalTimeoutMs;

    DaemonIoResult writeResult = daemon_write_exact_until(
        fd, request, sizeof(*request), exchangeDeadline);
    if (writeResult.failure != DAEMON_IO_NONE) {
        format_io_failure(error, errorSize, "request write", writeResult,
                          0, sizeof(*request));
        log_client_failure("request write", error);
        close(fd);
        return false;
    }

    ServiceWirePrefix prefix = {};
    DaemonIoResult prefixResult = daemon_read_exact_until(
        fd, &prefix, sizeof(prefix), exchangeDeadline);
    if (prefixResult.failure != DAEMON_IO_NONE) {
        if (prefixResult.failure == DAEMON_IO_TIMEOUT) {
            if (outcome) outcome->deadlineExpired = true;
            format_response_timeout(error, errorSize, "response header",
                                    command, totalTimeoutMs,
                                    monotonic_ms() - exchangeStart);
        } else {
            format_io_failure(error, errorSize, "response header read",
                              prefixResult, 0, sizeof(prefix));
        }
        log_client_failure("response header read", error);
        close(fd);
        return false;
    }
    response->magic = prefix.magic;
    response->version = prefix.version;
    ServiceWirePrefixDisposition disposition =
        service_wire_prefix_disposition(&prefix);
    if (disposition == SERVICE_WIRE_PREFIX_BAD_MAGIC) {
        if (error) gc_snprintf(error, errorSize,
            "bad daemon response magic 0x%08x (expected 0x%08x)",
            prefix.magic, SERVICE_PROTOCOL_MAGIC);
        log_client_failure("response header", error);
        close(fd);
        return false;
    }
    if (disposition == SERVICE_WIRE_PREFIX_VERSION_MISMATCH) {
        if (error) gc_snprintf(error, errorSize,
            "daemon protocol mismatch (client %u, daemon %u); reinstall/restart greencurve.service",
            (unsigned int)SERVICE_PROTOCOL_VERSION,
            (unsigned int)prefix.version);
        log_client_failure("protocol handshake", error);
        close(fd);
        return false;
    }

    const size_t prefixSize = sizeof(prefix);
    DaemonIoResult bodyResult = daemon_read_exact_until(
        fd, (unsigned char*)response + prefixSize,
        sizeof(*response) - prefixSize, exchangeDeadline);
    close(fd);
    if (bodyResult.failure != DAEMON_IO_NONE) {
        if (bodyResult.failure == DAEMON_IO_TIMEOUT) {
            if (outcome) outcome->deadlineExpired = true;
            format_response_timeout(error, errorSize, "response body",
                                    command, totalTimeoutMs,
                                    monotonic_ms() - exchangeStart);
        } else {
            format_io_failure(error, errorSize, "response body read", bodyResult,
                              prefixSize, sizeof(*response));
        }
        log_client_failure("response body read", error);
        return false;
    }
    note_exchange_duration(command, monotonic_ms() - exchangeStart,
                           totalTimeoutMs);
    if (!validate_service_response_for_ipc(response)) {
        if (error) gc_strlcpy(error, errorSize,
            "daemon returned an invalid state envelope");
        log_client_failure("response validation", error);
        return false;
    }
    log_daemon_identity_transition(response);
    if (response->status == SERVICE_STATUS_VERSION_MISMATCH) {
        if (error) gc_snprintf(error, errorSize,
            "daemon rejected protocol %u despite a v%u response",
            (unsigned int)request->version,
            (unsigned int)response->version);
        log_client_failure("protocol handshake", error);
        return false;
    }
    if (response->status != SERVICE_STATUS_OK) {
        if (error) gc_strlcpy(error, errorSize,
            response->message[0] ? response->message : "daemon request failed");
        // Includes the daemon's own reason (stale state, degraded GPU, an
        // incomplete request), which is otherwise only visible for a moment in
        // the TUI status row.
        log_client_failure("daemon rejected the request", error);
        return false;
    }
    return true;
}

// The public entry point derives its deadline from the command, so every
// existing caller lands in the right lane without naming a number.  That is the
// point of the policy header: there is no longer a place to pick one.
bool linux_daemon_send(const ServiceRequest* request, ServiceResponse* response,
                       char* error, size_t errorSize) {
    return linux_daemon_send_deadline(request, response,
        request ? linux_daemon_command_response_timeout_ms(
                      (unsigned int)request->command)
                : linux_daemon_response_timeout_ms(
                      LINUX_DAEMON_DEADLINE_STATE_READ),
        nullptr, error, errorSize);
}

bool linux_daemon_get_startup_policy(ServiceResponse* response,
                                     char* err, size_t errSize) {
    ServiceRequest req = {};
    ServiceResponse resp = {};
    req.magic = SERVICE_PROTOCOL_MAGIC;
    req.version = SERVICE_PROTOCOL_VERSION;
    req.command = SERVICE_CMD_GET_STARTUP_POLICY;
    req.callerPid = (gc_u32)getpid();
    if (!linux_daemon_send(&req, &resp, err, errSize)) return false;
    if (response) *response = resp;
    return resp.status == SERVICE_STATUS_OK;
}

bool linux_daemon_set_startup_policy(unsigned int mode, int profileSlot,
                                     const char* profileName,
                                     const GpuAdapterInfo* target,
                                     const DesiredSettings* desired,
                                     char* result, size_t resultSize) {
    ServiceRequest req = {};
    ServiceResponse resp = {};
    req.magic = SERVICE_PROTOCOL_MAGIC;
    req.version = SERVICE_PROTOCOL_VERSION;
    req.command = SERVICE_CMD_SET_STARTUP_POLICY;
    req.callerPid = (gc_u32)getpid();
    req.startupMode = (gc_u32)mode;
    // Slot, GPU and settings ride along only for PROFILE; the validator on both
    // ends rejects a policy that still carries irrelevant stale settings.
    if (mode == SERVICE_STARTUP_POLICY_PROFILE) {
        req.profileSlot = (gc_u32)profileSlot;
        if (profileName)
            gc_strlcpy(req.source, sizeof(req.source), profileName);
        if (target) req.targetGpu = *target;
        if (desired) req.desired = *desired;
    }
    char err[256] = {};
    bool ok = linux_daemon_send(&req, &resp, err, sizeof(err));
    if (result) {
        gc_strlcpy(result, resultSize,
            resp.message[0] ? resp.message : (ok ? "OK" : err));
    }
    return ok && resp.status == SERVICE_STATUS_OK;
}

bool linux_daemon_refresh_startup_profile(int profileSlot,
                                          const DesiredSettings* desired,
                                          char* result, size_t resultSize) {
    ServiceRequest req = {};
    ServiceResponse resp = {};
    req.magic = SERVICE_PROTOCOL_MAGIC;
    req.version = SERVICE_PROTOCOL_VERSION;
    req.command = SERVICE_CMD_REFRESH_STARTUP_PROFILE;
    req.callerPid = (gc_u32)getpid();
    req.profileSlot = (gc_u32)profileSlot;
    // No targetGpu and no startupMode by contract: the binding and the mode are
    // the daemon's, and the validator on both ends rejects a refresh that tries
    // to carry either.
    if (desired) req.desired = *desired;
    char err[256] = {};
    bool ok = linux_daemon_send(&req, &resp, err, sizeof(err));
    if (result) {
        gc_strlcpy(result, resultSize,
            resp.message[0] ? resp.message : (ok ? "OK" : err));
    }
    return ok && resp.status == SERVICE_STATUS_OK;
}

// The daemon's own per-peer bound.  Deliberately small, and deliberately NOT a
// turnaround budget: no honest client pauses between connect() and one
// fixed-size write, and requests are serviced one at a time, so a generous
// bound here is a local denial-of-service surface rather than patience.  ONE
// deadline covers header and body, so a peer cannot buy a second full window by
// stalling after the prefix.
static bool daemon_read_request(int fd, ServiceRequest* request) {
    if (!request) return false;
    memset(request, 0, sizeof(*request));
    const unsigned long long frameDeadline =
        monotonic_ms() + GC_DAEMON_PEER_STALL_BUDGET_MS;
    ServiceWirePrefix prefix = {};
    DaemonIoResult prefixResult = daemon_read_exact_until(
        fd, &prefix, sizeof(prefix), frameDeadline);
    if (prefixResult.failure != DAEMON_IO_NONE) {
        char detail[192] = {};
        format_io_failure(detail, sizeof(detail), "request header read",
                          prefixResult, 0, sizeof(prefix));
        dlog("daemon: %s\n", detail);
        return false;
    }
    request->magic = prefix.magic;
    request->version = prefix.version;
    if (service_wire_prefix_disposition(&prefix) !=
        SERVICE_WIRE_PREFIX_CURRENT) {
        dlog("daemon: request protocol mismatch client_magic=0x%08x client=%u daemon=%u\n",
             prefix.magic, prefix.version,
             (unsigned int)SERVICE_PROTOCOL_VERSION);
        // Do not read a version-specific body. handle_request returns a current
        // header/status immediately, including to an older shorter client.
        return true;
    }
    const size_t prefixSize = sizeof(prefix);
    DaemonIoResult bodyResult = daemon_read_exact_until(
        fd, (unsigned char*)request + prefixSize,
        sizeof(*request) - prefixSize, frameDeadline);
    if (bodyResult.failure != DAEMON_IO_NONE) {
        char detail[192] = {};
        format_io_failure(detail, sizeof(detail), "request body read",
                          bodyResult, prefixSize, sizeof(*request));
        dlog("daemon: %s\n", detail);
        return false;
    }
    return true;
}

static bool daemon_write_response(int fd, const ServiceResponse* response) {
    DaemonIoResult result = daemon_write_exact_until(
        fd, response, sizeof(*response),
        monotonic_ms() + GC_DAEMON_PEER_STALL_BUDGET_MS);
    if (result.failure == DAEMON_IO_NONE) return true;
    char detail[192] = {};
    format_io_failure(detail, sizeof(detail), "response write", result,
                      0, sizeof(*response));
    dlog("daemon: %s\n", detail);
    return false;
}
