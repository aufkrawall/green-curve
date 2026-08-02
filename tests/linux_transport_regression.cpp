// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Native Linux socket-pair coverage for the header-first daemon transport.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "gpu_core.h"
#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define GC_DAEMON_SOCKET_PATH "/tmp/greencurve-transport-regression.sock"
// Mirrors linux_daemon.h; that header cannot be included here because this
// fixture deliberately overrides GC_DAEMON_SOCKET_PATH above.  A source guard
// in build.py keeps the two values in sync.
#define GC_DAEMON_LISTEN_BACKLOG 8

#include "linux_socket_path_permissions.h"

static void dlog(const char* format, ...) {
    (void)format;
}

#include "linux_daemon_transport.cpp"

static bool native_write_all(int fd, const void* buffer, size_t size) {
    const unsigned char* bytes = (const unsigned char*)buffer;
    size_t written = 0;
    while (written < size) {
        ssize_t count = write(fd, bytes + written, size - written);
        if (count > 0) {
            written += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

static bool pair(int sockets[2]) {
    return socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0 &&
        set_nonblocking(sockets[0]) && set_nonblocking(sockets[1]);
}

static int socket_path_permission_regression() {
    char directoryTemplate[] = "/tmp/greencurve-socket-path-XXXXXX";
    char* directory = mkdtemp(directoryTemplate);
    if (!directory) return 17;

    int result = 0;
    int directoryFd = open(directory,
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int socketFd = -1;
    const char* socketName = "daemon.sock";
    char socketPath[sizeof(((struct sockaddr_un*)0)->sun_path)] = {};
    int pathLength = snprintf(socketPath, sizeof(socketPath), "%s/%s",
                              directory, socketName);
    if (directoryFd < 0 || pathLength <= 0 ||
        (size_t)pathLength >= sizeof(socketPath)) {
        result = 18;
    }
    if (!result) {
        socketFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (socketFd < 0) result = 19;
    }
    if (!result) {
        struct sockaddr_un address = {};
        address.sun_family = AF_UNIX;
        memcpy(address.sun_path, socketPath, (size_t)pathLength + 1);
        mode_t previousMask = umask(0077);
        int bindResult = bind(socketFd, (struct sockaddr*)&address,
                              sizeof(address));
        umask(previousMask);
        if (bindResult != 0) result = 20;
    }
    if (!result) {
        struct stat pathnameStatus = {};
        if (lstat(socketPath, &pathnameStatus) != 0 ||
            !S_ISSOCK(pathnameStatus.st_mode) ||
            (pathnameStatus.st_mode & 0777) != 0700)
            result = 21;
    }
    if (!result) {
        // This is the original defect: chmod/fstat of the socket descriptor
        // concerns a sockfs inode, not the pathname clients traverse.
        struct stat pathnameStatus = {};
        if (fchmod(socketFd, 0660) != 0 ||
            lstat(socketPath, &pathnameStatus) != 0 ||
            (pathnameStatus.st_mode & 0777) != 0700)
            result = 22;
    }
    if (!result) {
        char error[256] = {};
        if (linux_verify_socket_path_permissions_at(
                directoryFd, socketName, geteuid(), getegid(), 0660,
                error, sizeof(error)) || !strstr(error, "mode=0700"))
            result = 23;
    }
    if (!result) {
        char error[256] = {};
        if (!linux_configure_socket_path_permissions_at(
                directoryFd, socketName, geteuid(), getegid(), 0660,
                error, sizeof(error)) ||
            !linux_verify_socket_path_permissions_at(
                directoryFd, socketName, geteuid(), getegid(), 0660,
                error, sizeof(error)))
            result = 24;
    }

    if (socketFd >= 0) close(socketFd);
    if (directoryFd >= 0) {
        unlinkat(directoryFd, socketName, 0);
        close(directoryFd);
    }
    rmdir(directory);
    return result;
}

int main() {
    // An old, shorter response is decided from the complete eight-byte prefix;
    // no current-version body read is attempted.
    {
        int sockets[2] = {};
        if (!pair(sockets)) return 1;
        ServiceWirePrefix old = {SERVICE_PROTOCOL_MAGIC, 8};
        if (!native_write_all(sockets[0], &old, sizeof(old))) return 2;
        close(sockets[0]);
        ServiceWirePrefix received = {};
        DaemonIoResult readResult = daemon_read_exact_with_timeout(
            sockets[1], &received, sizeof(received), 1000);
        close(sockets[1]);
        if (readResult.failure != DAEMON_IO_NONE ||
            service_wire_prefix_disposition(&received) !=
                SERVICE_WIRE_PREFIX_VERSION_MISMATCH) return 3;
    }

    // The server likewise accepts an old request prefix for an immediate
    // VERSION_MISMATCH response without waiting for that version's short body.
    {
        int sockets[2] = {};
        if (!pair(sockets)) return 4;
        ServiceWirePrefix old = {SERVICE_PROTOCOL_MAGIC, 11};
        if (!native_write_all(sockets[0], &old, sizeof(old))) return 5;
        shutdown(sockets[0], SHUT_WR);
        ServiceRequest request = {};
        bool readOk = daemon_read_request(sockets[1], &request);
        close(sockets[0]);
        close(sockets[1]);
        if (!readOk || request.magic != SERVICE_PROTOCOL_MAGIC ||
            request.version != 11 || request.command != SERVICE_CMD_NONE)
            return 6;
    }

    // A same-version response that ends during its body is precise truncation,
    // including bytes transferred across header and body.
    {
        int sockets[2] = {};
        if (!pair(sockets)) return 7;
        ServiceWirePrefix current = {
            SERVICE_PROTOCOL_MAGIC, SERVICE_PROTOCOL_VERSION};
        gc_u32 partialBody = 0x12345678u;
        if (!native_write_all(sockets[0], &current, sizeof(current)) ||
            !native_write_all(sockets[0], &partialBody, sizeof(partialBody)))
            return 8;
        close(sockets[0]);
        ServiceWirePrefix received = {};
        if (daemon_read_exact_with_timeout(sockets[1], &received,
                sizeof(received), 1000).failure != DAEMON_IO_NONE) return 9;
        ServiceResponse response = {};
        DaemonIoResult body = daemon_read_exact_with_timeout(
            sockets[1], (unsigned char*)&response + sizeof(received),
            sizeof(response) - sizeof(received), 1000);
        char detail[192] = {};
        format_io_failure(detail, sizeof(detail), "response body read", body,
                          sizeof(received), sizeof(response));
        close(sockets[1]);
        if (body.failure != DAEMON_IO_EOF || body.transferred != 4 ||
            !strstr(detail, "truncated EOF") ||
            !strstr(detail, "12 of")) return 10;
    }

    // Timeout and clean EOF classification are deterministic: zero timeout
    // requires no sleeping, and a closed peer produces immediate EOF.
    {
        int sockets[2] = {};
        if (!pair(sockets)) return 11;
        unsigned char byte = 0;
        DaemonIoResult timeout = daemon_read_exact_with_timeout(
            sockets[1], &byte, 1, 0);
        if (timeout.failure != DAEMON_IO_TIMEOUT || timeout.transferred != 0 ||
            strcmp(daemon_io_failure_classification(timeout.failure, 0),
                   "timeout") != 0) return 12;
        close(sockets[0]);
        DaemonIoResult eof = daemon_read_exact_with_timeout(
            sockets[1], &byte, 1, 1000);
        close(sockets[1]);
        if (eof.failure != DAEMON_IO_EOF || eof.transferred != 0 ||
            strcmp(daemon_io_failure_classification(eof.failure, 0),
                   "EOF") != 0) return 13;
    }

    {
        int sockets[2] = {};
        if (!pair(sockets)) return 14;
        ServiceResponse response = {};
        response.magic = SERVICE_PROTOCOL_MAGIC;
        response.version = SERVICE_PROTOCOL_VERSION;
        if (!daemon_write_response(sockets[0], &response)) return 15;
        ServiceResponse received = {};
        DaemonIoResult readResult = daemon_read_exact_with_timeout(
            sockets[1], &received, sizeof(received), 1000);
        close(sockets[0]);
        close(sockets[1]);
        if (readResult.failure != DAEMON_IO_NONE ||
            received.magic != SERVICE_PROTOCOL_MAGIC ||
            received.version != SERVICE_PROTOCOL_VERSION) return 16;
    }

    {
        // A real listener with a peer that connects and then sends nothing.
        // The old serve loop called blocking accept() and then read the
        // request inline, so this client held the daemon for the full I/O
        // deadline and a second client could not be served at all.  poll()
        // over the listener must still report the second pending connection
        // immediately, with no sleeps and no wall-clock assumptions.
        char directoryTemplate[] = "/tmp/greencurve-accept-XXXXXX";
        char* directory = mkdtemp(directoryTemplate);
        if (!directory) return 30;
        char socketPath[128] = {};
        snprintf(socketPath, sizeof(socketPath), "%s/accept.sock", directory);

        int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listener < 0) return 31;
        struct sockaddr_un address = {};
        address.sun_family = AF_UNIX;
        snprintf(address.sun_path, sizeof(address.sun_path), "%s", socketPath);
        if (bind(listener, (struct sockaddr*)&address, sizeof(address)) != 0)
            return 32;
        if (listen(listener, GC_DAEMON_LISTEN_BACKLOG) != 0) return 33;
        if (!set_nonblocking(listener)) return 34;

        int stalled = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        int active = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (stalled < 0 || active < 0) return 35;
        if (connect(stalled, (struct sockaddr*)&address, sizeof(address)) != 0)
            return 36;
        if (connect(active, (struct sockaddr*)&address, sizeof(address)) != 0)
            return 37;

        // Accept the silent peer first and deliberately do not read from it.
        int stalledServer = accept(listener, nullptr, nullptr);
        if (stalledServer < 0) return 38;

        // The second connection must already be pending; poll() returns
        // without waiting out the stalled peer's deadline.
        struct pollfd waiter = {};
        waiter.fd = listener;
        waiter.events = POLLIN;
        if (poll(&waiter, 1, 0) != 1 || !(waiter.revents & POLLIN)) return 39;
        int activeServer = accept(listener, nullptr, nullptr);
        if (activeServer < 0) return 40;

        // And it is fully serviceable while the first peer is still silent.
        ServiceResponse response = {};
        response.magic = SERVICE_PROTOCOL_MAGIC;
        response.version = SERVICE_PROTOCOL_VERSION;
        if (!set_nonblocking(activeServer)) return 41;
        if (!daemon_write_response(activeServer, &response)) return 42;
        ServiceResponse received = {};
        if (!set_nonblocking(active)) return 43;
        if (daemon_read_exact_with_timeout(active, &received, sizeof(received),
                                           1000).failure != DAEMON_IO_NONE)
            return 44;
        if (received.magic != SERVICE_PROTOCOL_MAGIC) return 45;

        // A non-fatal accept() errno on a drained listener must classify as a
        // retry, not as a reason to end the serve loop.
        if (accept(listener, nullptr, nullptr) >= 0) return 46;
        if (daemon_accept_error_is_fatal(errno)) return 47;

        close(stalledServer);
        close(activeServer);
        close(stalled);
        close(active);
        close(listener);
        unlink(socketPath);
        rmdir(directory);
    }

    return socket_path_permission_regression();
}
