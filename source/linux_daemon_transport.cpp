// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included by linux_daemon.cpp; do not compile separately.

// Header-first, deadline-bounded Unix-socket transport.  Keeping the fixed
// eight-byte magic/version prefix independently readable lets a newly upgraded
// client diagnose an older, shorter daemon response without first waiting for
// a body that can never arrive.  The server follows the same rule for requests.

#include "linux_daemon_transport_policy.h"

#define GC_DAEMON_IO_TIMEOUT_MS 2000

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

static DaemonIoResult daemon_read_exact_with_timeout(
    int fd, void* buffer, size_t length, unsigned int timeoutMs) {
    DaemonIoResult result = {DAEMON_IO_NONE, 0, length, 0};
    unsigned char* bytes = (unsigned char*)buffer;
    unsigned long long deadline = monotonic_ms() + timeoutMs;
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

static DaemonIoResult daemon_write_exact_with_timeout(
    int fd, const void* buffer, size_t length, unsigned int timeoutMs) {
    DaemonIoResult result = {DAEMON_IO_NONE, 0, length, 0};
    const unsigned char* bytes = (const unsigned char*)buffer;
    unsigned long long deadline = monotonic_ms() + timeoutMs;
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

static DaemonIoResult daemon_read_exact(int fd, void* buffer, size_t length) {
    return daemon_read_exact_with_timeout(
        fd, buffer, length, GC_DAEMON_IO_TIMEOUT_MS);
}

static DaemonIoResult daemon_write_exact(int fd, const void* buffer,
                                         size_t length) {
    return daemon_write_exact_with_timeout(
        fd, buffer, length, GC_DAEMON_IO_TIMEOUT_MS);
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
    int count = getgroups(0, nullptr);
    if (count < 0) return false;
    gid_t* groups = count > 0
        ? (gid_t*)malloc((size_t)count * sizeof(gid_t)) : nullptr;
    if (count > 0 && !groups) return false;
    int received = count > 0 ? getgroups(count, groups) : 0;
    if (received < 0) {
        free(groups);
        return false;
    }
    bool found = false;
    for (int i = 0; i < received; ++i) {
        if (groups[i] == groupId) { found = true; break; }
    }
    free(groups);
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
static int client_connect(int* connectErrno) {
    if (connectErrno) *connectErrno = 0;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        if (connectErrno) *connectErrno = errno;
        return -1;
    }
    struct sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    gc_strlcpy(address.sun_path, sizeof(address.sun_path),
               GC_DAEMON_SOCKET_PATH);
    if (connect(fd, (struct sockaddr*)&address, sizeof(address)) != 0) {
        int failure = errno;
        close(fd);
        if (connectErrno) *connectErrno = failure;
        return -1;
    }
    if (!set_nonblocking(fd)) {
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

bool linux_daemon_send(const ServiceRequest* request, ServiceResponse* response,
                       char* error, size_t errorSize) {
    if (error && errorSize) error[0] = 0;
    if (!request || !response) {
        if (error) gc_strlcpy(error, errorSize, "invalid daemon request buffer");
        return false;
    }
    memset(response, 0, sizeof(*response));
    int connectErrno = 0;
    int fd = client_connect(&connectErrno);
    if (fd < 0) {
        if (connectErrno == EACCES || connectErrno == EPERM) {
            format_permission_diagnostic(error, errorSize, connectErrno);
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

    DaemonIoResult writeResult = daemon_write_exact(
        fd, request, sizeof(*request));
    if (writeResult.failure != DAEMON_IO_NONE) {
        format_io_failure(error, errorSize, "request write", writeResult,
                          0, sizeof(*request));
        log_client_failure("request write", error);
        close(fd);
        return false;
    }

    ServiceWirePrefix prefix = {};
    DaemonIoResult prefixResult = daemon_read_exact(fd, &prefix, sizeof(prefix));
    if (prefixResult.failure != DAEMON_IO_NONE) {
        format_io_failure(error, errorSize, "response header read", prefixResult,
                          0, sizeof(prefix));
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
    DaemonIoResult bodyResult = daemon_read_exact(
        fd, (unsigned char*)response + prefixSize,
        sizeof(*response) - prefixSize);
    close(fd);
    if (bodyResult.failure != DAEMON_IO_NONE) {
        format_io_failure(error, errorSize, "response body read", bodyResult,
                          prefixSize, sizeof(*response));
        log_client_failure("response body read", error);
        return false;
    }
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

static bool daemon_read_request(int fd, ServiceRequest* request) {
    if (!request) return false;
    memset(request, 0, sizeof(*request));
    ServiceWirePrefix prefix = {};
    DaemonIoResult prefixResult = daemon_read_exact(fd, &prefix, sizeof(prefix));
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
    DaemonIoResult bodyResult = daemon_read_exact(
        fd, (unsigned char*)request + prefixSize,
        sizeof(*request) - prefixSize);
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
    DaemonIoResult result = daemon_write_exact(fd, response, sizeof(*response));
    if (result.failure == DAEMON_IO_NONE) return true;
    char detail[192] = {};
    format_io_failure(detail, sizeof(detail), "response write", result,
                      0, sizeof(*response));
    dlog("daemon: %s\n", detail);
    return false;
}
