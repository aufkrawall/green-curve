// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_daemon.cpp — root GPU-control daemon + thin-client transport.
// See linux_daemon.h.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // struct ucred + SO_PEERCRED
#endif

#include "linux_daemon.h"
#include "linux_backend.h"
#include "linux_daemon_state.h"
#include "linux_gpu_selection.h"
#include "profile_persistence_policy.h"
#include "platform.h"
#include "fan_curve.h"
#include "fan_runtime_policy.h"

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
#include <sys/random.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif
#ifndef APP_BUILD_NUMBER
#define APP_BUILD_NUMBER 0
#endif

// ===========================================================================
// Framed blocking read/write of the fixed-size protocol structs.
// ===========================================================================
#define GC_DAEMON_IO_TIMEOUT_MS 2000

static unsigned long long monotonic_ms() {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)(ts.tv_nsec / 1000000ULL);
}

static bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) return false;
    return true;
}

static bool wait_fd_ready(int fd, short events, unsigned long long deadlineMs) {
    for (;;) {
        unsigned long long now = monotonic_ms();
        if (now >= deadlineMs) return false;
        unsigned long long remaining = deadlineMs - now;
        int timeout = remaining > 2147483647ULL ? 2147483647 : (int)remaining;
        struct pollfd pfd;
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = fd;
        pfd.events = events;
        int r = poll(&pfd, 1, timeout);
        if (r > 0) {
            if (pfd.revents & events) return true;
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
        } else if (r == 0) {
            return false;
        } else if (errno != EINTR) {
            return false;
        }
    }
}

static bool read_full(int fd, void* buf, size_t len) {
    unsigned char* p = (unsigned char*)buf;
    size_t got = 0;
    unsigned long long deadline = monotonic_ms() + GC_DAEMON_IO_TIMEOUT_MS;
    while (got < len) {
        if (!wait_fd_ready(fd, POLLIN, deadline)) return false;
        ssize_t n = read(fd, p + got, len - got);
        if (n > 0) { got += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        return false;
    }
    return true;
}
static bool write_full(int fd, const void* buf, size_t len) {
    const unsigned char* p = (const unsigned char*)buf;
    size_t put = 0;
    unsigned long long deadline = monotonic_ms() + GC_DAEMON_IO_TIMEOUT_MS;
    while (put < len) {
        if (!wait_fd_ready(fd, POLLOUT, deadline)) return false;
        ssize_t n = write(fd, p + put, len - put);
        if (n > 0) { put += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        return false;
    }
    return true;
}

static void dlog(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
static void dlog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    // flawfinder: ignore -- private logger; every call site supplies a constant format.
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

// ===========================================================================
// Client side
// ===========================================================================
// Connect to the root daemon while preserving the failure reason for the
// unprivileged CLI/TUI.  In particular, EACCES/EPERM means the daemon may be
// healthy but the current login session is not in the greencurve admin group.
static int client_connect(int* connectErrno) {
    if (connectErrno) *connectErrno = 0;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        if (connectErrno) *connectErrno = errno;
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    gc_strlcpy(addr.sun_path, sizeof(addr.sun_path), GC_DAEMON_SOCKET_PATH);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        int failureErrno = errno;
        close(fd);
        if (connectErrno) *connectErrno = failureErrno;
        return -1;
    }
    set_nonblocking(fd);
    return fd;
}

bool linux_daemon_send(const ServiceRequest* req, ServiceResponse* resp,
                       char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    int connectErrno = 0;
    int fd = client_connect(&connectErrno);
    if (fd < 0) {
        if (err && (connectErrno == EACCES || connectErrno == EPERM)) {
            gc_snprintf(err, errSize,
                        "permission denied accessing %s; add your user to greencurve: "
                        "sudo usermod -aG greencurve \"$USER\"; then sign out/in or run newgrp greencurve",
                        GC_DAEMON_SOCKET_PATH);
        } else if (err) {
            gc_snprintf(err, errSize, "daemon not reachable at %s: %s (is greencurve.service running?)",
                        GC_DAEMON_SOCKET_PATH, connectErrno ? strerror(connectErrno) : "unknown error");
        }
        return false;
    }
    bool ok = write_full(fd, req, sizeof(*req)) && read_full(fd, resp, sizeof(*resp));
    close(fd);
    if (!ok) { if (err) gc_strlcpy(err, errSize, "daemon I/O error"); return false; }
    if (resp->magic != SERVICE_PROTOCOL_MAGIC) { if (err) gc_strlcpy(err, errSize, "bad daemon response"); return false; }
    if (!validate_service_response_for_ipc(resp)) {
        if (err) gc_strlcpy(err, errSize, "daemon returned an invalid state envelope");
        return false;
    }
    if (resp->status == SERVICE_STATUS_VERSION_MISMATCH) {
        if (err) gc_snprintf(err, errSize, "daemon protocol mismatch (client %u, daemon %u)",
                             (unsigned)SERVICE_PROTOCOL_VERSION, (unsigned)resp->version);
        return false;
    }
    return resp->status == SERVICE_STATUS_OK;
}

static bool send_simple(unsigned int command, const DesiredSettings* desired,
                         const GpuAdapterInfo* target, bool interactive,
                         const ServiceStateEnvelope* expected,
                         ServiceResponse* response,
                         char* result, size_t resultSize) {
    ServiceRequest req;
    memset(&req, 0, sizeof(req));
    req.magic = SERVICE_PROTOCOL_MAGIC;
    req.version = SERVICE_PROTOCOL_VERSION;
    req.command = command;
    req.flags = interactive ? SERVICE_REQUEST_FLAG_INTERACTIVE : 0;
    req.callerPid = (gc_u32)getpid();
    if (command == SERVICE_CMD_APPLY || command == SERVICE_CMD_RESET) {
        ssize_t randomBytes = -1;
        do {
            randomBytes = getrandom(&req.operationId,
                sizeof(req.operationId), 0);
        } while (randomBytes < 0 && errno == EINTR);
        if (randomBytes != (ssize_t)sizeof(req.operationId) ||
            req.operationId == 0) {
            if (result) gc_strlcpy(result, resultSize,
                "failed generating a secure operation ID");
            return false;
        }
    }
    if (desired) req.desired = *desired;
    if (target) req.targetGpu = *target;
    if (expected) {
        req.expectedServiceInstanceId = expected->serviceInstanceId;
        req.expectedGpuGeneration = expected->gpuGeneration;
        req.expectedTopologySignature = expected->topologySignature;
    }
    ServiceResponse resp;
    memset(&resp, 0, sizeof(resp));
    char err[256] = {};
    bool ok = linux_daemon_send(&req, &resp, err, sizeof(err));
    bool receivedServiceError = !ok &&
        resp.magic == SERVICE_PROTOCOL_MAGIC &&
        resp.version == SERVICE_PROTOCOL_VERSION &&
        resp.status != SERVICE_STATUS_OK;
    if (!ok && req.operationId != 0 && !receivedServiceError) {
        ServiceRequest query = {};
        query.magic = SERVICE_PROTOCOL_MAGIC;
        query.version = SERVICE_PROTOCOL_VERSION;
        query.command = SERVICE_CMD_GET_OPERATION_RESULT;
        query.callerPid = (gc_u32)getpid();
        query.operationId = req.operationId;
        char queryErr[256] = {};
        if (linux_daemon_send(&query, &resp, queryErr, sizeof(queryErr)) &&
            resp.operationState != SERVICE_OPERATION_IN_PROGRESS &&
            resp.operationState != SERVICE_OPERATION_OUTCOME_UNKNOWN) {
            ok = resp.status == SERVICE_STATUS_OK;
            dlog("daemon client: operation=%llu recovered state=%u after transport error\n",
                (unsigned long long)req.operationId,
                (unsigned int)resp.operationState);
        } else {
            if (result) gc_snprintf(result, resultSize,
                "operation %llu outcome is pending or unknown after transport timeout; do not retry with a new operation ID",
                (unsigned long long)req.operationId);
            return false;
        }
    }
    if (response) *response = resp;
    if (result) gc_strlcpy(result, resultSize, resp.message[0] ? resp.message : (ok ? "OK" : err));
    return ok;
}

bool linux_daemon_apply(const GpuAdapterInfo* target, const DesiredSettings* desired, bool interactive,
                         char* result, size_t resultSize) {
    return linux_daemon_apply_checked(target, desired, interactive, nullptr,
                                      nullptr, result, resultSize);
}
bool linux_daemon_reset(const GpuAdapterInfo* target, char* result, size_t resultSize) {
    return linux_daemon_reset_checked(target, nullptr, nullptr, result, resultSize);
}

bool linux_daemon_apply_checked(const GpuAdapterInfo* target,
                                const DesiredSettings* desired, bool interactive,
                                const ServiceStateEnvelope* expected,
                                ServiceResponse* response,
                                char* result, size_t resultSize) {
    return send_simple(SERVICE_CMD_APPLY, desired, target, interactive,
                       expected, response, result, resultSize);
}

bool linux_daemon_reset_checked(const GpuAdapterInfo* target,
                                const ServiceStateEnvelope* expected,
                                ServiceResponse* response,
                                char* result, size_t resultSize) {
    return send_simple(SERVICE_CMD_RESET, nullptr, target, true,
                       expected, response, result, resultSize);
}

bool linux_daemon_snapshot(ServiceSnapshot* snapshot, char* err, size_t errSize) {
    ServiceResponse resp = {};
    if (!linux_daemon_get_state(nullptr, &resp, err, errSize)) return false;
    if (snapshot) *snapshot = resp.snapshot;
    return true;
}

bool linux_daemon_get_state(const GpuAdapterInfo* target, ServiceResponse* response,
                            char* err, size_t errSize) {
    ServiceRequest req = {};
    ServiceResponse resp = {};
    req.magic = SERVICE_PROTOCOL_MAGIC;
    req.version = SERVICE_PROTOCOL_VERSION;
    req.command = SERVICE_CMD_GET_SNAPSHOT;
    req.callerPid = (gc_u32)getpid();
    if (target) req.targetGpu = *target;
    if (!linux_daemon_send(&req, &resp, err, errSize)) return false;
    if (response) *response = resp;
    return true;
}

// ===========================================================================
// Daemon side
// ===========================================================================
static LinuxGpuState g_gpu;
static bool g_gpuReady = false;
static pl_mutex g_lock;
static DesiredSettings g_activeDesired;
static bool g_hasActiveDesired = false;
static GpuAdapterInfo g_activeTarget;
static bool g_stateUncertain = false;
static ServiceOperationTracker g_operationTracker = {};

static bool persist_daemon_operation(gc_u64 operationId, gc_u32 state,
    gc_u32 responseStatus, const char* message) {
    LinuxDaemonOperationRecord record = {};
    linux_daemon_operation_initialize(&record, operationId, state,
        responseStatus, message);
    char err[160] = {};
    bool ok = linux_daemon_operation_store(GC_DAEMON_OPERATION_FILE,
        &record, err, sizeof(err));
    if (!ok) dlog("daemon operation: persistence failed: %s\n",
        err[0] ? err : "unknown error");
    return ok;
}

#include "linux_operation_runtime.h"
static unsigned int g_fanFailureCount = 0;
static volatile int g_running = 1;
static pthread_mutex_t g_fanWakeMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_fanWakeCondition = PTHREAD_COND_INITIALIZER;
static unsigned long long g_fanWakeGeneration = 0;

static void wake_fan_runtime() {
    pthread_mutex_lock(&g_fanWakeMutex);
    ++g_fanWakeGeneration;
    pthread_cond_broadcast(&g_fanWakeCondition);
    pthread_mutex_unlock(&g_fanWakeMutex);
}

static bool store_daemon_record(LinuxDaemonRecordState state,
                                const GpuAdapterInfo* target,
                                const DesiredSettings* desired,
                                char* err, size_t errSize,
                                gc_u64 operationId = 0,
                                gc_u32 operationState = SERVICE_OPERATION_NONE) {
    LinuxDaemonStateRecord record = {};
    linux_daemon_record_initialize(&record, state, target, desired,
        operationId, operationState);
    return linux_daemon_state_store(GC_DAEMON_STATE_FILE, &record, err, errSize);
}

static bool restore_committed_record(bool hadPrevious, const GpuAdapterInfo* previousTarget,
                                     const DesiredSettings* previousDesired) {
    char err[256] = {};
    if (!hadPrevious) return linux_daemon_state_remove(GC_DAEMON_STATE_FILE, err, sizeof(err));
    return store_daemon_record(LINUX_DAEMON_RECORD_ACTIVE, previousTarget,
                               previousDesired, err, sizeof(err));
}

#include "linux_daemon_snapshot_runtime.cpp"


static void handle_request(const ServiceRequest* wireReq, ServiceResponse* resp) {
    memset(resp, 0, sizeof(*resp));
    resp->magic = SERVICE_PROTOCOL_MAGIC;
    resp->version = SERVICE_PROTOCOL_VERSION;
    resp->serviceBuildNumber = APP_BUILD_NUMBER;
    gc_strlcpy(resp->serviceVersion, sizeof(resp->serviceVersion), APP_VERSION);

    ServiceRequest validatedRequest = *wireReq;
    const ServiceRequest* req = &validatedRequest;
    bool protocolMatches = req->magic == SERVICE_PROTOCOL_MAGIC &&
        req->version == SERVICE_PROTOCOL_VERSION;
    bool requestValid = protocolMatches &&
        validate_service_request_for_ipc(&validatedRequest);

    pl_mutex_lock(&g_lock);
    if (!protocolMatches) {
        resp->status = SERVICE_STATUS_VERSION_MISMATCH;
        gc_strlcpy(resp->message, sizeof(resp->message), "protocol version mismatch");
    } else if (!requestValid) {
        resp->status = SERVICE_STATUS_ERROR;
        gc_strlcpy(resp->message, sizeof(resp->message),
            "invalid protocol fields");
    } else if ((req->command == SERVICE_CMD_APPLY ||
                req->command == SERVICE_CMD_RESET) &&
               !mutation_preconditions_match(req, resp)) {
        // State and message were filled by the fail-closed precondition gate.
    } else switch (req->command) {
        case SERVICE_CMD_PING:
            resp->status = SERVICE_STATUS_OK;
            gc_strlcpy(resp->message, sizeof(resp->message), "pong");
            break;
        case SERVICE_CMD_GET_SNAPSHOT:
        case SERVICE_CMD_GET_TELEMETRY:
            if (req->targetGpu.valid) {
                char targetErr[256] = {};
                if (!select_request_gpu(req, targetErr, sizeof(targetErr))) {
                    resp->status = SERVICE_STATUS_ERROR;
                    gc_strlcpy(resp->message, sizeof(resp->message), targetErr);
                    break;
                }
            }
            if (g_gpuReady) linux_backend_refresh(&g_gpu);
            populate_snapshot(&resp->snapshot, &resp->controlState);
            if (g_hasActiveDesired) resp->desired = g_activeDesired;
            resp->status = SERVICE_STATUS_OK;
            break;
        case SERVICE_CMD_GET_ACTIVE_DESIRED:
            if (g_hasActiveDesired) resp->desired = g_activeDesired;
            resp->status = SERVICE_STATUS_OK;
            break;
        case SERVICE_CMD_GET_OPERATION_RESULT: {
            resp->operationId = req->operationId;
            const ServiceOperationRecord* record = service_operation_find(
                &g_operationTracker, req->operationId);
            if (!record) {
                resp->status = SERVICE_STATUS_ERROR;
                resp->operationState = SERVICE_OPERATION_OUTCOME_UNKNOWN;
                gc_strlcpy(resp->message, sizeof(resp->message),
                    "operation outcome is unknown to this daemon generation");
            } else {
                resp->status = record->responseStatus;
                resp->operationState = record->state;
                gc_strlcpy(resp->message, sizeof(resp->message),
                    record->message[0] ? record->message :
                    "operation result available");
            }
            if (g_hasActiveDesired) resp->desired = g_activeDesired;
            populate_snapshot(&resp->snapshot, &resp->controlState);
            break;
        }
        case SERVICE_CMD_APPLY: {
            LinuxOperationRequestGuard operation(req, resp, "apply");
            if (!operation.execute()) {
                if (g_hasActiveDesired) resp->desired = g_activeDesired;
                populate_snapshot(&resp->snapshot, &resp->controlState);
                break;
            }
            if (!g_gpuReady) {
                resp->status = SERVICE_STATUS_ERROR;
                gc_strlcpy(resp->message, sizeof(resp->message), "GPU backend not initialized");
                break;
            }
            char targetErr[256] = {};
            if (!select_request_gpu(req, targetErr, sizeof(targetErr))) {
                resp->status = SERVICE_STATUS_ERROR;
                gc_strlcpy(resp->message, sizeof(resp->message), targetErr);
                break;
            }
            DesiredSettings d = req->desired;
            validate_desired_settings_for_ipc(&d);
            if (d.hasLock && d.lockMode == LOCK_MODE_NONE) {
                resp->status = SERVICE_STATUS_ERROR;
                gc_strlcpy(resp->message, sizeof(resp->message),
                    "lock mode is required when a VF lock is enabled");
                dlog("daemon apply: rejected enabled lock without a mode\n");
                break;
            }
            if (d.hasFan && d.fanMode == FAN_MODE_CURVE) {
                fan_curve_normalize(&d.fanCurve);
                char fanValidation[160] = {};
                if (!fan_curve_validate(&d.fanCurve, fanValidation,
                        sizeof(fanValidation))) {
                    resp->status = SERVICE_STATUS_ERROR;
                    gc_snprintf(resp->message, sizeof(resp->message),
                        "fan curve rejected at daemon boundary: %s",
                        fanValidation[0] ? fanValidation : "invalid curve");
                    break;
                }
            }
            bool hadPrevious = g_hasActiveDesired;
            DesiredSettings previousDesired = g_activeDesired;
            GpuAdapterInfo previousTarget = g_activeTarget;
            LinuxHardwareSnapshot before = {};
            char stateErr[256] = {};
            if (!linux_backend_capture_snapshot(&g_gpu, &before, stateErr, sizeof(stateErr)) ||
                !store_daemon_record(LINUX_DAEMON_RECORD_PREPARED, &g_gpu.selectedGpu,
                                     &d, stateErr, sizeof(stateErr),
                                     req->operationId, SERVICE_OPERATION_IN_PROGRESS)) {
                resp->status = SERVICE_STATUS_ERROR;
                gc_snprintf(resp->message, sizeof(resp->message),
                            "Apply aborted before hardware write: %s", stateErr);
                break;
            }
            LinuxMutationResult mutation = linux_backend_apply(&g_gpu, &d,
                resp->message, sizeof(resp->message));
            bool committed = false;
            if (mutation.success) {
                committed = store_daemon_record(LINUX_DAEMON_RECORD_ACTIVE,
                    &g_gpu.selectedGpu, &d, stateErr, sizeof(stateErr),
                    req->operationId, SERVICE_OPERATION_SUCCEEDED);
                if (!committed) {
                    char rollbackErr[256] = {};
                    bool rollbackOk = linux_backend_restore_snapshot(&g_gpu, &before,
                        mutation.attemptedPhases, rollbackErr, sizeof(rollbackErr));
                    bool recordOk = restore_committed_record(hadPrevious, &previousTarget,
                                                              &previousDesired);
                    g_stateUncertain = !rollbackOk || !recordOk;
                    if (g_stateUncertain)
                        store_daemon_record(LINUX_DAEMON_RECORD_UNCERTAIN, &g_gpu.selectedGpu,
                                            &d, stateErr, sizeof(stateErr));
                    gc_snprintf(resp->message, sizeof(resp->message),
                        "Apply persistence failed; hardware rollback %s",
                        rollbackOk ? "succeeded" : "is uncertain");
                }
            } else if (mutation.rollbackSucceeded || !mutation.anyWrite) {
                bool recordOk = restore_committed_record(hadPrevious, &previousTarget, &previousDesired);
                if (!recordOk) {
                    g_stateUncertain = true;
                    store_daemon_record(LINUX_DAEMON_RECORD_UNCERTAIN, &g_gpu.selectedGpu,
                                        &d, stateErr, sizeof(stateErr));
                }
            } else {
                g_stateUncertain = true;
                store_daemon_record(LINUX_DAEMON_RECORD_UNCERTAIN, &g_gpu.selectedGpu,
                                    &d, stateErr, sizeof(stateErr));
            }
            if (committed) {
                g_activeDesired = d;
                g_activeTarget = g_gpu.selectedGpu;
                g_hasActiveDesired = true;
                g_stateUncertain = false;
                g_fanFailureCount = 0;
                wake_fan_runtime();
            }
            populate_snapshot(&resp->snapshot, &resp->controlState);
            if (g_hasActiveDesired) resp->desired = g_activeDesired;
            resp->status = committed ? SERVICE_STATUS_OK : SERVICE_STATUS_ERROR;
            if (g_stateUncertain) wake_fan_runtime();
            break;
        }
        case SERVICE_CMD_RESET: {
            LinuxOperationRequestGuard operation(req, resp, "reset");
            if (!operation.execute()) {
                if (g_hasActiveDesired) resp->desired = g_activeDesired;
                populate_snapshot(&resp->snapshot, &resp->controlState);
                break;
            }
            if (!g_gpuReady) {
                resp->status = SERVICE_STATUS_ERROR;
                gc_strlcpy(resp->message, sizeof(resp->message), "GPU backend not initialized");
                break;
            }
            char targetErr[256] = {};
            if (!select_request_gpu(req, targetErr, sizeof(targetErr))) {
                resp->status = SERVICE_STATUS_ERROR;
                gc_strlcpy(resp->message, sizeof(resp->message), targetErr);
                break;
            }
            bool hadPrevious = g_hasActiveDesired;
            DesiredSettings previousDesired = g_activeDesired;
            GpuAdapterInfo previousTarget = g_activeTarget;
            LinuxHardwareSnapshot before = {};
            char stateErr[256] = {};
            DesiredSettings empty = {};
            if (!linux_backend_capture_snapshot(&g_gpu, &before, stateErr, sizeof(stateErr)) ||
                !store_daemon_record(LINUX_DAEMON_RECORD_PREPARED, &g_gpu.selectedGpu,
                                     &empty, stateErr, sizeof(stateErr),
                                     req->operationId, SERVICE_OPERATION_IN_PROGRESS)) {
                resp->status = SERVICE_STATUS_ERROR;
                gc_snprintf(resp->message, sizeof(resp->message),
                            "Reset aborted before hardware write: %s", stateErr);
                break;
            }
            LinuxMutationResult mutation = linux_backend_reset(&g_gpu, resp->message, sizeof(resp->message));
            bool committed = mutation.success &&
                linux_daemon_state_remove(GC_DAEMON_STATE_FILE, stateErr, sizeof(stateErr));
            if (mutation.success && !committed) {
                char rollbackErr[256] = {};
                bool rollbackOk = linux_backend_restore_snapshot(&g_gpu, &before,
                    mutation.attemptedPhases, rollbackErr, sizeof(rollbackErr));
                bool recordOk = restore_committed_record(hadPrevious, &previousTarget, &previousDesired);
                g_stateUncertain = !rollbackOk || !recordOk;
                gc_snprintf(resp->message, sizeof(resp->message),
                    "Reset persistence failed; hardware rollback %s",
                    rollbackOk ? "succeeded" : "is uncertain");
            } else if (!mutation.success && !(mutation.rollbackSucceeded || !mutation.anyWrite)) {
                g_stateUncertain = true;
                store_daemon_record(LINUX_DAEMON_RECORD_UNCERTAIN, &g_gpu.selectedGpu,
                                    &empty, stateErr, sizeof(stateErr));
            } else if (!mutation.success) {
                bool recordOk = restore_committed_record(hadPrevious, &previousTarget, &previousDesired);
                if (!recordOk) {
                    g_stateUncertain = true;
                    store_daemon_record(LINUX_DAEMON_RECORD_UNCERTAIN, &g_gpu.selectedGpu,
                                        &empty, stateErr, sizeof(stateErr));
                }
            }
            if (committed) {
                g_hasActiveDesired = false;
                memset(&g_activeDesired, 0, sizeof(g_activeDesired));
                memset(&g_activeTarget, 0, sizeof(g_activeTarget));
                g_stateUncertain = false;
                g_fanFailureCount = 0;
                wake_fan_runtime();
            }
            populate_snapshot(&resp->snapshot, &resp->controlState);
            resp->status = committed ? SERVICE_STATUS_OK : SERVICE_STATUS_ERROR;
            if (g_stateUncertain) wake_fan_runtime();
            break;
        }
        default:
            resp->status = SERVICE_STATUS_ERROR;
            gc_strlcpy(resp->message, sizeof(resp->message), "unknown command");
            break;
    }
    // Linux has no interactive GUI recovery surface yet, but it shares the
    // wire ABI.  Stamp every response as one daemon generation and report its
    // current payload conservatively as DEGRADED until the Linux frontend grows
    // the same published-control-state contract.
    daemon_stamp_state_envelope(resp);
    pl_mutex_unlock(&g_lock);
}

static void log_peer(int connFd) {
    struct ucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(connFd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) {
        dlog("daemon: connection from pid=%d uid=%d gid=%d\n", (int)cred.pid, (int)cred.uid, (int)cred.gid);
    }
}

#include "linux_socket_permissions.h"

int linux_daemon_run(const char* configPath) {
    (void)configPath;
    pl_mutex_init(&g_lock);

    char err[256] = {};
    if (linux_backend_init(&g_gpu, nullptr, err, sizeof(err))) {
        g_gpuReady = true;
        dlog("daemon: GPU backend ready (%s, family=%d)\n", g_gpu.gpuName, (int)g_gpu.family);
    } else {
        dlog("daemon: GPU backend init failed: %s (serving telemetry-less)\n", err);
    }

    LinuxDaemonOperationRecord operation = {};
    if (linux_daemon_operation_load(GC_DAEMON_OPERATION_FILE, &operation,
            err, sizeof(err))) {
        gc_u32 restoredState = operation.state == SERVICE_OPERATION_IN_PROGRESS
            ? SERVICE_OPERATION_OUTCOME_UNKNOWN : operation.state;
        service_operation_restore(&g_operationTracker, operation.operationId,
            restoredState, operation.responseStatus,
            restoredState == SERVICE_OPERATION_OUTCOME_UNKNOWN
                ? "operation outcome became uncertain across daemon restart"
                : operation.message);
    }

    // Startup restart-reapply is authorized only by a complete, checksummed
    // ACTIVE record for the exact physical GPU. Prepared/uncertain/legacy state
    // never causes an automatic hardware write.
    LinuxDaemonStateRecord saved = {};
    LinuxDaemonStateLoadResult loadResult = linux_daemon_state_load(
        GC_DAEMON_STATE_FILE, &saved, err, sizeof(err));
    if (loadResult == LINUX_DAEMON_STATE_LEGACY_REMOVED ||
        loadResult == LINUX_DAEMON_STATE_INVALID_REMOVED) {
        dlog("daemon: rejected and removed %s daemon state; explicit Apply/Reset required\n",
             loadResult == LINUX_DAEMON_STATE_LEGACY_REMOVED ? "legacy" : "invalid");
    } else if (loadResult == LINUX_DAEMON_STATE_IO_ERROR) {
        g_stateUncertain = true;
        dlog("daemon: state load failed closed: %s\n", err);
    } else if (loadResult == LINUX_DAEMON_STATE_LOADED) {
        if (saved.operationId != 0) {
            gc_u32 restoredState = saved.operationState;
            if (saved.state != LINUX_DAEMON_RECORD_ACTIVE ||
                restoredState == SERVICE_OPERATION_IN_PROGRESS) {
                restoredState = SERVICE_OPERATION_OUTCOME_UNKNOWN;
            }
            service_operation_restore(&g_operationTracker,
                saved.operationId, restoredState,
                restoredState == SERVICE_OPERATION_SUCCEEDED
                    ? SERVICE_STATUS_OK : SERVICE_STATUS_ERROR,
                restoredState == SERVICE_OPERATION_SUCCEEDED
                    ? "operation restored from committed daemon state"
                    : "operation outcome became uncertain across daemon restart");
        }
        if (saved.state != LINUX_DAEMON_RECORD_ACTIVE) {
            g_stateUncertain = true;
            dlog("daemon: startup state=%u is not ACTIVE; no automatic write\n", saved.state);
        } else if (!linux_backend_select_target(&g_gpu, &saved.targetGpu, err, sizeof(err))) {
            g_stateUncertain = true;
            store_daemon_record(LINUX_DAEMON_RECORD_UNCERTAIN, &saved.targetGpu,
                                &saved.desired, err, sizeof(err));
            dlog("daemon: startup GPU identity rejected; no automatic write: %s\n", err);
        } else {
            g_gpuReady = true;
            LockMode storedLockMode = saved.desired.lockMode;
            saved.desired.lockMode = profile_lock_mode_after_load(
                saved.desired.hasLock, true, saved.desired.lockMode);
            if (storedLockMode != saved.desired.lockMode) {
                dlog("daemon: migrated persisted enabled lock mode %d to %d before startup reapply\n",
                     (int)storedLockMode, (int)saved.desired.lockMode);
            }
            validate_desired_settings_for_ipc(&saved.desired);
            char msg[256] = {};
            LinuxMutationResult mutation = linux_backend_apply(&g_gpu, &saved.desired,
                                                               msg, sizeof(msg));
            if (mutation.success) {
                g_activeDesired = saved.desired;
                g_activeTarget = g_gpu.selectedGpu;
                g_hasActiveDesired = true;
                dlog("daemon: startup reapply committed -> %s\n", msg);
            } else {
                // A failed automatic replay is never safe to retry unattended,
                // even when the hardware rollback was verified.  Require an
                // explicit user Apply/Reset to reclaim ownership.
                g_stateUncertain = true;
                store_daemon_record(LINUX_DAEMON_RECORD_UNCERTAIN, &saved.targetGpu,
                                    &saved.desired, err, sizeof(err));
                dlog("daemon: startup reapply failed and was locked out -> %s\n", msg);
            }
        }
    }

    pl_thread fanThread;
    bool fanThreadOk = pl_thread_start(&fanThread, fan_reassert_thread, nullptr);

    // Socket: root-owned, group-accessible (the systemd unit chowns to the
    // greencurve admin group; mode 0660).  Anyone who can connect is authorized;
    // every request is still clamped by validate_desired_settings_for_ipc.
    umask(0077);
    if (mkdir(GC_DAEMON_SOCKET_DIR, 0755) != 0 && errno != EEXIST) {
        dlog("daemon: cannot create socket directory: %s\n", strerror(errno));
        return 1;
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
        return 1;
    }
    unlinkat(socketDir, "greencurve.sock", 0);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { dlog("daemon: socket() failed\n"); close(socketDir); return 1; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    gc_strlcpy(addr.sun_path, sizeof(addr.sun_path), GC_DAEMON_SOCKET_PATH);
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        dlog("daemon: bind(%s) failed: %s\n", GC_DAEMON_SOCKET_PATH, strerror(errno));
        close(srv);
        close(socketDir);
        return 1;
    }
    if (!configure_daemon_socket_permissions(srv)) {
        close(srv);
        unlinkat(socketDir, "greencurve.sock", 0);
        close(socketDir);
        return 1;
    }
    if (listen(srv, 8) != 0) {
        dlog("daemon: listen() failed\n"); close(srv); close(socketDir); return 1;
    }
    dlog("daemon: listening on %s\n", GC_DAEMON_SOCKET_PATH);

    while (g_running) {
        int conn = accept(srv, nullptr, nullptr);
        if (conn < 0) { if (errno == EINTR) continue; break; }
        set_nonblocking(conn);
        log_peer(conn);
        ServiceRequest req;
        if (read_full(conn, &req, sizeof(req))) {
            ServiceResponse resp;
            handle_request(&req, &resp);
            write_full(conn, &resp, sizeof(resp));
        }
        close(conn);
    }

    g_running = 0;
    wake_fan_runtime();
    if (fanThreadOk) pl_thread_join(fanThread);
    close(srv);
    unlinkat(socketDir, "greencurve.sock", 0);
    close(socketDir);
    if (g_gpu.nvmlLib || g_gpu.nvapiLib) linux_backend_shutdown(&g_gpu);
    return 0;
}

#include "linux_service_install.cpp"
