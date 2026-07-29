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
#include "linux_crash_breadcrumb.h"
#include "linux_daemon_state.h"
#include "linux_debug_log.h"
#include "linux_gpu_selection.h"
#include "linux_mutation_authority.h"
#include "profile_persistence_policy.h"
#include "startup_snapshot_policy.h"
#include "platform.h"
#include "fan_curve.h"
#include "fan_runtime_policy.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>  // getpwnam: a primary-group member never appears in gr_mem
#include <poll.h>
#include <signal.h>
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

// Every daemon and client-transport diagnostic goes through the shared file
// sink.  The daemon additionally mirrors to stderr (systemd routes it to the
// journal); interactive clients do not, because stderr is the terminal the TUI
// is about to take over and transport chatter used to land on top of the user's
// shell before the alternate screen was even entered.
static void dlog(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
static void dlog(const char* fmt, ...) {
    if (!linux_debug_log_is_enabled()) return;
    char text[1024] = {};
    va_list ap;
    va_start(ap, fmt);
    gc_vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    linux_debug_logf("%s", text);
}

#include "linux_daemon_transport.cpp"

// ===========================================================================
// Client side convenience requests
// ===========================================================================

static const GpuAdapterInfo* daemon_response_selected_gpu(
    const ServiceResponse* response) {
    if (!response ||
        (response->state.validSections &
            SERVICE_STATE_SECTION_ADAPTER_IDENTITY) == 0 ||
        response->snapshot.selectedAdapterIndex >=
            response->snapshot.adapterCount ||
        response->snapshot.selectedAdapterIndex >= MAX_GPU_ADAPTERS)
        return nullptr;
    const GpuAdapterInfo* gpu = &response->snapshot.adapters[
        response->snapshot.selectedAdapterIndex];
    return gpu->valid &&
        (linux_gpu_bdf_valid(gpu) || gpu->pciInfoValid) ? gpu : nullptr;
}

static bool send_simple(unsigned int command, const DesiredSettings* desired,
                         const GpuAdapterInfo* target, bool interactive,
                         ServiceApplyOrigin applyOrigin,
                         const ServiceStateEnvelope* expected,
                         ServiceResponse* response,
                         char* result, size_t resultSize) {
    ServiceRequest req;
    memset(&req, 0, sizeof(req));
    req.magic = SERVICE_PROTOCOL_MAGIC;
    req.version = SERVICE_PROTOCOL_VERSION;
    req.command = command;
    req.flags = service_request_flags_for_command(command, interactive);
    req.callerPid = (gc_u32)getpid();
    if (command == SERVICE_CMD_APPLY) req.applyOrigin = applyOrigin;
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
    ServiceResponse current = {};
    char error[256] = {};
    if (!linux_daemon_get_state(target, &current, error, sizeof(error))) {
        if (result) gc_strlcpy(result, resultSize,
            error[0] ? error : "cannot attach Apply to live daemon state");
        return false;
    }
    const GpuAdapterInfo* authoritative =
        daemon_response_selected_gpu(&current);
    if (!authoritative) {
        if (result) gc_strlcpy(result, resultSize,
            "daemon did not publish an exact GPU identity for Apply");
        return false;
    }
    return send_simple(SERVICE_CMD_APPLY, desired, authoritative, interactive,
                       SERVICE_APPLY_ORIGIN_CLI, &current.state,
                       nullptr, result, resultSize);
}
bool linux_daemon_reset(const GpuAdapterInfo* target, char* result, size_t resultSize) {
    ServiceResponse current = {};
    char error[256] = {};
    if (!linux_daemon_get_state(target, &current, error, sizeof(error))) {
        if (result) gc_strlcpy(result, resultSize,
            error[0] ? error : "cannot attach Reset to live daemon state");
        return false;
    }
    const GpuAdapterInfo* authoritative =
        daemon_response_selected_gpu(&current);
    if (!authoritative) {
        if (result) gc_strlcpy(result, resultSize,
            "daemon did not publish an exact GPU identity for Reset");
        return false;
    }
    return send_simple(SERVICE_CMD_RESET, nullptr, authoritative, true,
                       SERVICE_APPLY_ORIGIN_UNSPECIFIED, &current.state,
                       nullptr, result, resultSize);
}

bool linux_daemon_apply_checked(const GpuAdapterInfo* target,
                                const DesiredSettings* desired, bool interactive,
                                const ServiceStateEnvelope* expected,
                                ServiceResponse* response,
                                char* result, size_t resultSize) {
    gc_u32 domains = service_desired_mutation_domains(desired);
    if (!target || !target->valid || !desired || !expected ||
        !expected->serviceInstanceId || !expected->gpuGeneration ||
        (service_mutation_domains_require_vf(domains) &&
         !expected->topologySignature)) {
        if (result) gc_strlcpy(result, resultSize,
            "Apply is not attached to the required daemon/GPU/topology generation");
        return false;
    }
    return send_simple(SERVICE_CMD_APPLY, desired, target, interactive,
                       SERVICE_APPLY_ORIGIN_GUI,
                       expected, response, result, resultSize);
}

bool linux_daemon_reset_checked(const GpuAdapterInfo* target,
                                const ServiceStateEnvelope* expected,
                                ServiceResponse* response,
                                char* result, size_t resultSize) {
    if (!target || !target->valid || !expected ||
        !expected->serviceInstanceId || !expected->gpuGeneration ||
        !expected->topologySignature) {
        if (result) gc_strlcpy(result, resultSize,
            "Reset is not attached to a complete READY daemon/GPU/topology generation");
        return false;
    }
    return send_simple(SERVICE_CMD_RESET, nullptr, target, true,
                       SERVICE_APPLY_ORIGIN_UNSPECIFIED,
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

bool linux_daemon_resolve_write_target(const GpuAdapterInfo* preferred,
                                       GpuAdapterInfo* out,
                                       char* err, size_t errSize) {
    ServiceResponse current = {};
    if (!linux_daemon_get_state(preferred, &current, err, errSize)) return false;
    const GpuAdapterInfo* authoritative = daemon_response_selected_gpu(&current);
    if (!authoritative) {
        gc_strlcpy(err, errSize,
            "daemon has not published an exact GPU identity yet");
        return false;
    }
    if (out) *out = *authoritative;
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
// The committed startup-apply policy.  Zero-initialized means RESTORE_LAST,
// which is exactly what every build before protocol v13 did.
static LinuxDaemonStartupRecord g_startupPolicy = {};

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
#include "linux_daemon_lifecycle.h"

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
// The whole boot-apply policy feature: client requests, the two daemon request
// handlers, the startup write, and the once-per-start record load.  Needs
// store_daemon_record() and populate_snapshot() above it.
#include "linux_startup_policy.h"


static void handle_request(const ServiceRequest* wireReq, ServiceResponse* resp) {
    memset(resp, 0, sizeof(*resp));
    resp->magic = SERVICE_PROTOCOL_MAGIC;
    resp->version = SERVICE_PROTOCOL_VERSION;
    resp->servicePid = (gc_u32)getpid();
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
        // Names the caller, so a stale binary left behind by a partial upgrade
        // can be found instead of guessed at.
        dlog("daemon: rejected pid=%u command=%u magic=0x%08x version=%u "
             "(this daemon is protocol %u); the client binary is out of date\n",
             (unsigned int)wireReq->callerPid, (unsigned int)wireReq->command,
             (unsigned int)wireReq->magic, (unsigned int)wireReq->version,
             (unsigned int)SERVICE_PROTOCOL_VERSION);
    } else if (!requestValid) {
        resp->status = SERVICE_STATUS_ERROR;
        gc_strlcpy(resp->message, sizeof(resp->message),
            "invalid protocol fields");
        dlog("daemon: rejected malformed request pid=%u command=%u flags=0x%08x "
             "operationId=%llu applyOrigin=%u profileSlot=%u startupMode=%u "
             "targetValid=%u preconditions=%llu/%llu/%llu\n",
             (unsigned int)req->callerPid, (unsigned int)req->command,
             (unsigned int)req->flags,
             (unsigned long long)req->operationId,
             (unsigned int)req->applyOrigin, (unsigned int)req->profileSlot,
             (unsigned int)req->startupMode, (unsigned int)req->targetGpu.valid,
             (unsigned long long)req->expectedServiceInstanceId,
             (unsigned long long)req->expectedGpuGeneration,
             (unsigned long long)req->expectedTopologySignature);
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
        case SERVICE_CMD_GET_STARTUP_POLICY:
        case SERVICE_CMD_SET_STARTUP_POLICY:
        case SERVICE_CMD_REFRESH_STARTUP_PROFILE:
            daemon_handle_startup_policy_command(req, resp);
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
            DesiredSettings committedDesired = service_merge_desired_after_mutation(
                hadPrevious ? &previousDesired : nullptr, &d);
            if (d.resetOcBeforeApply)
                dlog("daemon apply: reset baseline intent merge previousCurvePoints=%d requestedCurvePoints=%d "
                     "committedCurvePoints=%d preservedPower=%d preservedFan=%d\n",
                     service_desired_curve_point_count(
                         hadPrevious ? &previousDesired : nullptr),
                     service_desired_curve_point_count(&d),
                     service_desired_curve_point_count(&committedDesired),
                     committedDesired.hasPowerLimit ? 1 : 0,
                     committedDesired.hasFan ? 1 : 0);
            LinuxHardwareSnapshot before = {};
            char stateErr[256] = {};
            if (!linux_backend_capture_snapshot(&g_gpu, &before, stateErr, sizeof(stateErr)) ||
                !store_daemon_record(LINUX_DAEMON_RECORD_PREPARED, &g_gpu.selectedGpu,
                                     &committedDesired, stateErr, sizeof(stateErr),
                                     req->operationId, SERVICE_OPERATION_IN_PROGRESS)) {
                resp->status = SERVICE_STATUS_ERROR;
                gc_snprintf(resp->message, sizeof(resp->message),
                            "Apply aborted before hardware write: %s", stateErr);
                break;
            }
            LinuxMutationResult mutation = linux_backend_apply(&g_gpu, &d,
                hadPrevious ? &previousDesired : nullptr, &committedDesired,
                resp->message, sizeof(resp->message));
            bool committed = false;
            if (mutation.success) {
                committed = store_daemon_record(LINUX_DAEMON_RECORD_ACTIVE,
                    &g_gpu.selectedGpu, &committedDesired,
                    stateErr, sizeof(stateErr),
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
                                            &committedDesired, stateErr, sizeof(stateErr));
                    gc_snprintf(resp->message, sizeof(resp->message),
                        "Apply persistence failed; hardware rollback %s",
                        rollbackOk ? "succeeded" : "is uncertain");
                }
            } else if (mutation.rollbackSucceeded || !mutation.anyWrite) {
                bool recordOk = restore_committed_record(hadPrevious, &previousTarget, &previousDesired);
                if (!recordOk) {
                    g_stateUncertain = true;
                    store_daemon_record(LINUX_DAEMON_RECORD_UNCERTAIN, &g_gpu.selectedGpu,
                                        &committedDesired, stateErr, sizeof(stateErr));
                }
            } else {
                g_stateUncertain = true;
                store_daemon_record(LINUX_DAEMON_RECORD_UNCERTAIN, &g_gpu.selectedGpu,
                                    &committedDesired, stateErr, sizeof(stateErr));
            }
            if (committed) {
                g_activeDesired = committedDesired;
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
    // Stamp every response as one daemon/GPU generation. The envelope derives
    // READY versus DEGRADED from the complete atomic snapshot and advertises
    // only independently rollback-safe mutation domains.
    daemon_stamp_state_envelope(resp);
    pl_mutex_unlock(&g_lock);
}

static void log_peer(int connFd) {
    struct ucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(connFd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) {
        dlog("daemon: connection from pid=%d uid=%d primary_gid=%d\n",
             (int)cred.pid, (int)cred.uid, (int)cred.gid);
    }
}

#include "linux_socket_permissions.h"
#include "linux_systemd_notify.cpp"
#include "linux_daemon_serve.h"

int linux_daemon_run(const char* configPath) {
    pl_mutex_init(&g_lock);
    if (!init_fan_wake_condition()) {
        dlog("daemon: cannot initialize monotonic fan wake condition\n");
        return 1;
    }
    if (!install_daemon_signal_handlers()) {
        dlog("daemon: cannot install stop signal handlers\n");
        return 1;
    }
    // Re-label the breadcrumbs installed in main(): this process is the root
    // daemon, and its crashes are the ones that matter most in the journal.
    linux_install_crash_breadcrumbs("daemon");
    // Re-published for this translation unit: linux_crash_breadcrumb.h keeps
    // its state in `static` storage, so the daemon's copy needs the descriptor
    // too or a daemon crash would only reach the journal.
    linux_set_crash_log_fd(linux_debug_log_raw_fd());
    linux_set_crash_phase("daemon-init");
    dlog("daemon: version=%s build=%u protocol=%u pid=%ld config=%s log=%s\n",
         APP_VERSION, (unsigned int)APP_BUILD_NUMBER,
         (unsigned int)SERVICE_PROTOCOL_VERSION, (long)getpid(),
         configPath && configPath[0] ? configPath : "<none>",
         linux_debug_log_path());

    char err[256] = {};
    if (linux_backend_init(&g_gpu, nullptr, err, sizeof(err))) {
        g_gpuReady = true;
        dlog("daemon: GPU backend online (%s family=%d match=%s architectureSource=%s "
             "vfFresh=%d infoStatus=%d statusStatus=%d controlStatus=%d "
             "health=%s)\n",
             g_gpu.gpuName, (int)g_gpu.family,
             linux_gpu_match_method_name(g_gpu.nvapiMatchMethod),
             linux_gpu_architecture_source_name(g_gpu.architectureSource),
             g_gpu.vfSnapshotFresh ? 1 : 0,
             g_gpu.vfInfoStatus, g_gpu.vfStatusStatus,
             g_gpu.vfControlStatus,
             service_gpu_health_reason_name(g_gpu.health.reason));
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

    load_startup_policy_at_boot();

    if (g_startupPolicy.mode == SERVICE_STARTUP_POLICY_PROFILE) {
        apply_startup_profile_policy();
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
        if (g_startupPolicy.mode != SERVICE_STARTUP_POLICY_RESTORE_LAST) {
            // The administrator asked for something other than "replay what was
            // applied last", so the committed record stays on disk untouched
            // and is simply not written to the GPU.  A later explicit Apply or
            // a policy change back to restore-last picks it up again.
            dlog("daemon: committed intent present but startup policy is %s; "
                 "not replaying it\n",
                 service_startup_policy_mode_name(g_startupPolicy.mode));
        } else if (saved.state != LINUX_DAEMON_RECORD_ACTIVE) {
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
                nullptr, &saved.desired, msg, sizeof(msg));
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

    DaemonListener listener = {};
    if (!daemon_open_listener(&listener)) {
        g_running = 0;
        wake_fan_runtime();
        if (fanThreadOk) pl_thread_join(fanThread);
        if (g_gpu.nvmlLib || g_gpu.nvapiLib) linux_backend_shutdown(&g_gpu);
        return 1;
    }

    char readinessError[256] = {};
    ServiceSnapshot readinessSnapshot = {};
    ControlState readinessControls = {};
    populate_snapshot(&readinessSnapshot, &readinessControls);
    bool ready = linux_systemd_notify_ready(&readinessSnapshot.health,
                                            readinessError,
                                            sizeof(readinessError));
    if (!ready) dlog("daemon: startup readiness failed: %s\n", readinessError);

    linux_set_crash_phase("daemon-serving");
    int exitStatus = ready ? daemon_serve_until_stopped(listener.socketFd) : 1;

    linux_set_crash_phase("daemon-shutdown");
    g_running = 0;
    wake_fan_runtime();
    if (fanThreadOk) pl_thread_join(fanThread);
    daemon_release_fan_to_driver();
    daemon_close_listener(&listener);
    close_daemon_shutdown_pipe();
    if (g_gpu.nvmlLib || g_gpu.nvapiLib) linux_backend_shutdown(&g_gpu);
    dlog("daemon: exiting with status %d\n", exitStatus);
    return exitStatus;
}

#include "linux_service_install.cpp"
