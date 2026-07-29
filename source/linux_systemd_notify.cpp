// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included by linux_daemon.cpp; do not compile separately.

// Minimal sd_notify-compatible sender.  This intentionally avoids a link-time
// libsystemd dependency while retaining Type=notify startup semantics.
#include "linux_systemd_notify_policy.h"

static bool linux_systemd_notify_ready(const ServiceGpuHealth* health,
                                       char* error, size_t errorSize) {
    if (error && errorSize) error[0] = 0;
    const char* notifySocket = getenv("NOTIFY_SOCKET");
    if (!notifySocket || !notifySocket[0]) {
        // A manually launched --daemon has no systemd notification endpoint.
        return true;
    }

    struct sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    size_t nameLength = strlen(notifySocket);
    if (nameLength == 0 || nameLength >= sizeof(address.sun_path)) {
        if (error) gc_strlcpy(error, errorSize,
            "NOTIFY_SOCKET path is empty or too long");
        return false;
    }
    if (notifySocket[0] == '@') {
        address.sun_path[0] = '\0';
        memcpy(address.sun_path + 1, notifySocket + 1, nameLength - 1);
    } else {
        memcpy(address.sun_path, notifySocket, nameLength + 1);
    }
    socklen_t addressLength = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
        nameLength + (notifySocket[0] == '@' ? 0 : 1));

    char payload[384] = {};
    if (!linux_systemd_build_ready_payload(
            health, payload, sizeof(payload))) {
        if (error) gc_strlcpy(error, errorSize,
            "systemd READY payload exceeded its bounded buffer");
        return false;
    }

    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        if (error) gc_snprintf(error, errorSize,
            "cannot open systemd notify socket: %s", strerror(errno));
        return false;
    }
    ssize_t sent = sendto(fd, payload, strlen(payload), MSG_NOSIGNAL,
        (const struct sockaddr*)&address, addressLength);
    int sendError = errno;
    close(fd);
    if (sent != (ssize_t)strlen(payload)) {
        if (error) gc_snprintf(error, errorSize,
            "systemd READY notification failed: %s",
            sent < 0 ? strerror(sendError) : "short datagram write");
        return false;
    }
    // Do not accidentally notify a manager inherited by later child processes.
    unsetenv("NOTIFY_SOCKET");
    dlog("daemon: systemd readiness sent after startup replay and socket listen\n");
    return true;
}
