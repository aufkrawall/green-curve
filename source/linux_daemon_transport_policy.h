// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Pure wire-prefix and I/O failure classification for daemon transport tests.

#ifndef GREEN_CURVE_LINUX_DAEMON_TRANSPORT_POLICY_H
#define GREEN_CURVE_LINUX_DAEMON_TRANSPORT_POLICY_H

#include "gpu_core.h"
#include <errno.h>

struct ServiceWirePrefix {
    gc_u32 magic;
    gc_u32 version;
};
static_assert(sizeof(ServiceWirePrefix) == 8,
              "service wire prefix must remain exactly eight bytes");

enum ServiceWirePrefixDisposition {
    SERVICE_WIRE_PREFIX_CURRENT = 0,
    SERVICE_WIRE_PREFIX_BAD_MAGIC,
    SERVICE_WIRE_PREFIX_VERSION_MISMATCH,
};

static inline ServiceWirePrefixDisposition service_wire_prefix_disposition(
    const ServiceWirePrefix* prefix) {
    if (!prefix || prefix->magic != SERVICE_PROTOCOL_MAGIC)
        return SERVICE_WIRE_PREFIX_BAD_MAGIC;
    return prefix->version == SERVICE_PROTOCOL_VERSION
        ? SERVICE_WIRE_PREFIX_CURRENT
        : SERVICE_WIRE_PREFIX_VERSION_MISMATCH;
}

enum DaemonIoFailure {
    DAEMON_IO_NONE = 0,
    DAEMON_IO_TIMEOUT,
    DAEMON_IO_EOF,
    DAEMON_IO_ERROR,
};

struct DaemonIoResult {
    DaemonIoFailure failure;
    size_t transferred;
    size_t expected;
    int errorNumber;
};

static inline const char* daemon_io_failure_classification(
    DaemonIoFailure failure, size_t totalTransferred) {
    switch (failure) {
        case DAEMON_IO_NONE: return "none";
        case DAEMON_IO_TIMEOUT: return "timeout";
        case DAEMON_IO_EOF:
            return totalTransferred == 0 ? "EOF" : "truncated EOF";
        default: return "I/O error";
    }
}

// accept() failure classification.  Treating every non-EINTR errno as fatal
// let a transient condition (a peer that closed between connect and accept, a
// descriptor or buffer shortage) end the serve loop permanently — and the exit
// status was zero, so Restart=on-failure never restarted the daemon.
enum DaemonAcceptDisposition {
    DAEMON_ACCEPT_RETRY = 0,       // transient; try again immediately
    DAEMON_ACCEPT_RECLAIM_FD,      // descriptor exhaustion; free the reserve first
    DAEMON_ACCEPT_FATAL,           // listener is unusable; exit non-zero
};

static inline DaemonAcceptDisposition daemon_accept_disposition(int errorNumber) {
    switch (errorNumber) {
        // The listening socket itself is still healthy.
        case EINTR:
        case ECONNABORTED:
        case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
        case EPROTO:
        case ENOBUFS:
        case ENOMEM:
        case EPERM:            // a firewall/LSM rejected one peer, not the listener
            return DAEMON_ACCEPT_RETRY;
        // Per-process or system-wide descriptor exhaustion.  Retrying without
        // freeing a descriptor spins, so the caller drops its reserved fd first.
        case EMFILE:
        case ENFILE:
            return DAEMON_ACCEPT_RECLAIM_FD;
        default:
            return DAEMON_ACCEPT_FATAL;
    }
}

static inline bool daemon_accept_error_is_fatal(int errorNumber) {
    return daemon_accept_disposition(errorNumber) == DAEMON_ACCEPT_FATAL;
}

struct LinuxDaemonPermissionFacts {
    bool socketMetadataAvailable;
    const char* socketPath;
    const char* connectError;
    const char* metadataError;
    unsigned int socketOwnerUid;
    const char* socketGroupName;
    unsigned int socketGroupId;
    unsigned int socketMode;
    unsigned int processEuid;
    unsigned int processPrimaryGid;
    int supplementaryGreencurve; // -1 unknown, 0 absent, 1 present
};

static inline void linux_daemon_format_permission_facts(
    const LinuxDaemonPermissionFacts* facts,
    char* output, size_t outputSize) {
    if (!output || outputSize == 0) return;
    if (!facts) {
        gc_strlcpy(output, outputSize,
            "permission denied; diagnostic facts unavailable");
        return;
    }
    const char* supplementary = facts->supplementaryGreencurve < 0
        ? "unknown" : facts->supplementaryGreencurve ? "yes" : "no";
    if (facts->socketMetadataAvailable) {
        gc_snprintf(output, outputSize,
            "permission denied accessing %s (%s); socket owner_uid=%u group=%s(%u) mode=%04o; "
            "process euid=%u primary_gid=%u supplementary greencurve=%s. "
            "Run sudo usermod -aG greencurve \"$USER\", then sign out/in or run newgrp greencurve",
            facts->socketPath ? facts->socketPath : "daemon socket",
            facts->connectError ? facts->connectError : "permission denied",
            facts->socketOwnerUid,
            facts->socketGroupName ? facts->socketGroupName : "?",
            facts->socketGroupId, facts->socketMode,
            facts->processEuid, facts->processPrimaryGid, supplementary);
    } else {
        gc_snprintf(output, outputSize,
            "permission denied accessing %s (%s); socket metadata unavailable: %s; "
            "process euid=%u primary_gid=%u supplementary greencurve=%s",
            facts->socketPath ? facts->socketPath : "daemon socket",
            facts->connectError ? facts->connectError : "permission denied",
            facts->metadataError ? facts->metadataError : "unknown error",
            facts->processEuid, facts->processPrimaryGid, supplementary);
    }
}

#endif // GREEN_CURVE_LINUX_DAEMON_TRANSPORT_POLICY_H
