// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_daemon.cpp — root GPU-control daemon + thin-client transport.
// See linux_daemon.h.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // struct ucred + SO_PEERCRED
#endif

#include "linux_daemon.h"
#include "log_redaction_policy.h"
#include "linux_backend.h"
#include "linux_gpu_controller.h"
#include "linux_crash_breadcrumb.h"
#include "linux_daemon_state.h"
#include "linux_graphics_recovery_policy.h"
#include "linux_debug_log.h"
#include "linux_gpu_selection.h"
// The SAME per-identity admission policy the Windows pipe transport runs.  It is
// host-neutral pure logic (a monotonic clock in, a decision out), so reusing it
// here is a port of an existing, tested control rather than a second design.
#include "service_ipc_throttle_policy.h"
// What the bucket cannot ration on a single-threaded loop: time spent waiting
// on a stalled peer, repaid in time.
#include "linux_daemon_stall_policy.h"
#include "linux_mutation_authority.h"
#include "profile_persistence_policy.h"
// The platform-neutral "replay the complete active intent" request builder the
// Windows standby and driver-recovery paths use.  Shared deliberately: it is
// what decides that a replay resets the OC baseline first, and that decision
// must not be reimplemented per platform.
#include "service_lifecycle_policy.h"
#include "service_scoped_apply_policy.h"
#include "startup_snapshot_policy.h"
#include "platform.h"
#include "fan_curve.h"
#include "fan_runtime_policy.h"
#include "fan_intent_policy.h"

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

#include "linux_daemon_client.h"


#include "linux_daemon_client_verbs.cpp"

// ===========================================================================
// Daemon side
// ===========================================================================
// Process-wide: the driver libraries every GPU state binds against
// (linux_driver_context.h) and the one lock serializing requests, the fan
// worker and backend (re)initialization.
static LinuxDriverContext g_driver = {};
static pl_mutex g_lock;
// The GPU this daemon binds is a LinuxGpuController owned by
// daemon_run_owned(); every other function receives it as `ctl`.

static bool persist_daemon_operation(gc_u64 operationId, gc_u32 state,
    gc_u32 responseStatus, gc_u32 outcomeSeverity, const char* message) {
    LinuxDaemonOperationRecord record = {};
    linux_daemon_operation_initialize(&record, operationId, state,
        responseStatus, outcomeSeverity, message);
    char err[160] = {};
    bool ok = linux_daemon_operation_store(GC_DAEMON_OPERATION_FILE,
        &record, err, sizeof(err));
    if (!ok) dlog("daemon operation: persistence failed: %s\n",
        err[0] ? err : "unknown error");
    return ok;
}

#include "linux_operation_runtime.h"
#include "linux_daemon_lifecycle.h"

static GpuLocationRole daemon_target_location_role(LinuxGpuController& ctl, const GpuAdapterInfo* target) {
    if (ctl.hasActiveDesired && linux_gpu_identity_matches(target, &ctl.activeTarget) &&
        ctl.activeTargetRole == GPU_LOCATION_ROLE_REQUIRED)
        return GPU_LOCATION_ROLE_REQUIRED;
    return linux_gpu_location_role_for_selection(target, ctl.gpu.adapters, ctl.gpu.adapterCount);
}

static bool store_daemon_record(LinuxGpuController& ctl, LinuxDaemonRecordState state,
                                const GpuAdapterInfo* target,
                                const DesiredSettings* desired,
                                char* err, size_t errSize,
                                gc_u64 operationId = 0,
                                gc_u32 operationState = SERVICE_OPERATION_NONE,
                                GpuLocationRole role = GPU_LOCATION_ROLE_UNRECORDED,
                                const LinuxGpuUuid* uuid = nullptr) {
    if (state == LINUX_DAEMON_RECORD_ACTIVE && !ctl.gpu.writeIdentityResolved) return false;  // never an unproven card
    if (role == GPU_LOCATION_ROLE_UNRECORDED)
        role = daemon_target_location_role(ctl, target);
    LinuxDaemonStateRecord record = {};
    const LinuxGpuUuid* seen = ctl.gpu.writeIdentityResolved && linux_gpu_identity_matches(target, &ctl.gpu.selectedGpu)
        ? linux_backend_selected_uuid(&ctl.gpu) : nullptr;  // a revoked binding's read names no card
    const LinuxGpuUuid binding = uuid ? *uuid : linux_gpu_uuid_retain(seen, nullptr);  // a pinned binding is kept
    linux_daemon_record_initialize(&record, state, target, desired,
        operationId, operationState, role, &binding,
        ctl.gpu.graphicsRecovery.covered | (state == LINUX_DAEMON_RECORD_PREPARED && desired
            ? graphics_scalar_recovery_bit(desired_named_graphics_surface(*desired)) : 0u));
    if (state == LINUX_DAEMON_RECORD_PREPARED)
        return graphics_scalar_operation_prepare(ctl.gpu.graphicsRecovery, record.graphicsControlSurfaces,
            [&] { return linux_daemon_state_store(GC_DAEMON_STATE_FILE, &record, err, errSize); });
    return linux_daemon_state_store(GC_DAEMON_STATE_FILE, &record, err, errSize);
}

static bool restore_committed_record(LinuxGpuController& ctl, bool wasUncertain, bool hadPrevious,
                                     const GpuAdapterInfo* previousTarget,
                                     const DesiredSettings* previousDesired,
                                     const LinuxDaemonStateRecord* uncertainRecord,
                                     const LinuxDaemonStateRecord* diskPreimage) {
    char err[256] = {};
    switch (linux_failed_operation_record(wasUncertain, hadPrevious)) {
        case LINUX_FAILED_OPERATION_KEEP_UNCERTAIN: {
            if (uncertainRecord) {
                const LinuxDaemonStateRecord restored =
                    linux_daemon_record_as_uncertain(uncertainRecord);
                const bool ok = linux_daemon_state_store(GC_DAEMON_STATE_FILE,
                                                         &restored, err, sizeof(err));
                dlog("daemon state: restored prior uncertain intent ok=%d%s%s\n",
                     ok ? 1 : 0, err[0] ? ": " : "", err);
                return ok;
            }
            const bool ok = store_daemon_record(ctl, LINUX_DAEMON_RECORD_UNCERTAIN,
                hadPrevious ? previousTarget : &ctl.gpu.selectedGpu, previousDesired,
                err, sizeof(err), 0, SERVICE_OPERATION_NONE,  // the previous intent's binding, if any
                hadPrevious ? ctl.activeTargetRole : GPU_LOCATION_ROLE_UNRECORDED,
                hadPrevious ? &ctl.activeTargetUuid : nullptr);
            dlog("daemon state: failed operation kept the uncertain record ok=%d%s%s\n",
                 ok ? 1 : 0, err[0] ? ": " : "", err);
            return ok;
        }
        case LINUX_FAILED_OPERATION_RESTORE_ACTIVE:
            return store_daemon_record(ctl, LINUX_DAEMON_RECORD_ACTIVE, previousTarget,
                                       previousDesired, err, sizeof(err), 0, SERVICE_OPERATION_NONE,
                                       ctl.activeTargetRole, &ctl.activeTargetUuid);
        case LINUX_FAILED_OPERATION_REMOVE_RECORD:
            break;
    }
    // A journal not replayed (policy "none", no intent in memory) goes back intact on a failed write.
    if (diskPreimage) {
        const bool ok = linux_daemon_state_store(GC_DAEMON_STATE_FILE, diskPreimage, err, sizeof(err));
        dlog("daemon state: failed operation restored the journal it replaced ok=%d state=%u%s%s\n",
             ok ? 1 : 0, diskPreimage->state, err[0] ? ": " : "", err);
        return ok;
    }
    return linux_daemon_state_remove(GC_DAEMON_STATE_FILE, err, sizeof(err));
}

#include "linux_daemon_record_recovery.h"
#include "linux_daemon_journal.h"
// The fan ownership marker and once-per-crash startup handback.  Before the
// unattended-write path below, which records ownership through it.
#include "linux_fan_ownership.h"
// The suspend window and the copy a fan lockout inside it keeps; used by the
// fan worker and the resume restore below.
#include "linux_standby_fan_stash.h"
#include "linux_daemon_snapshot_runtime.cpp"
// The single unattended-write path, shared by both boot-apply modes and the
// standby-resume restore.  Needs store_daemon_record(), populate_snapshot(),
// the fan runtime and the guard helpers above it, and must precede the boot
// policy below, which calls into it.
#include "linux_daemon_scoped_apply.h"
#include "linux_auto_restore_runtime.h"
// The whole boot-apply policy feature: client requests, the two daemon request
// handlers, the startup write, and the once-per-start record load.  Needs
// store_daemon_record() and populate_snapshot() above it.
#include "linux_startup_policy.h"
#include "linux_daemon_capture.h"

static void handle_request(LinuxGpuController& ctl, const ServiceRequest* wireReq, ServiceResponse* resp) {
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
    linux_backend_expire_session_uuids(&ctl.gpu);  // no handler sees an older session's UUIDs
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
        const char* reason = service_request_reject_reason(req);
        gc_snprintf(resp->message, sizeof(resp->message),
            "invalid protocol fields: %s", reason);
        dlog("daemon: rejected malformed request pid=%u command=%u flags=0x%08x "
             "operationId=%llu applyOrigin=%u profileSlot=%u startupMode=%u "
             "targetValid=%u preconditions=%llu/%llu/%llu reason=%s\n",
             (unsigned int)req->callerPid, (unsigned int)req->command,
             (unsigned int)req->flags,
             (unsigned long long)req->operationId,
             (unsigned int)req->applyOrigin, (unsigned int)req->profileSlot,
             (unsigned int)req->startupMode, (unsigned int)req->targetGpu.valid,
             (unsigned long long)req->expectedServiceInstanceId,
             (unsigned long long)req->expectedGpuGeneration,
             (unsigned long long)req->expectedTopologySignature, reason);
    } else if (((req->command == SERVICE_CMD_APPLY ||
                 req->command == SERVICE_CMD_RESET) &&
                (!daemon_control_preconditions_match(ctl, req, resp) ||
                 !mutation_preconditions_match(ctl, req, resp))) ||
               !daemon_route_admits(ctl, req, resp)) {
        // State and message were filled by the fail-closed precondition gate,
        // or by the route gate for a GPU this daemon is not serving.
    } else switch (req->command) {
        case SERVICE_CMD_PING:
            resp->status = SERVICE_STATUS_OK;
            gc_strlcpy(resp->message, sizeof(resp->message), "pong");
            break;
        case SERVICE_CMD_GET_SNAPSHOT:
        case SERVICE_CMD_GET_TELEMETRY:
            if (!ctl.gpuReady && !req->targetGpu.valid)
                daemon_retry_backend_for_read(ctl,
                    (req->flags & SERVICE_REQUEST_FLAG_INTERACTIVE) != 0);
            // Only the full state read moves the daemon; a routed telemetry
            // read already addresses the served GPU (daemon_route_admits).
            if (req->targetGpu.valid && req->command == SERVICE_CMD_GET_SNAPSHOT) {
                char targetErr[256] = {};
                if (!select_request_gpu(ctl, req, targetErr, sizeof(targetErr))) {
                    resp->status = SERVICE_STATUS_ERROR;
                    gc_strlcpy(resp->message, sizeof(resp->message), targetErr);
                    break;
                }
            }
            if (ctl.gpuReady) linux_backend_refresh(&ctl.gpu);
            populate_snapshot(ctl, &resp->snapshot, &resp->controlState);
            if (ctl.hasActiveDesired) resp->desired = ctl.activeDesired;
            resp->status = SERVICE_STATUS_OK;
            break;
#include "linux_daemon_read_switch.h"
        case SERVICE_CMD_RESUME_RESTORE:
            daemon_handle_resume_restore(ctl, resp);
            break;
        case SERVICE_CMD_GET_STARTUP_POLICY:
        case SERVICE_CMD_SET_STARTUP_POLICY:
        case SERVICE_CMD_REFRESH_STARTUP_PROFILE:
            daemon_handle_startup_policy_command(ctl, req, resp);
            break;
        case SERVICE_CMD_GET_OPERATION_RESULT: {
            resp->operationId = req->operationId;
            const ServiceOperationRecord* record = service_operation_find(
                &ctl.operationTracker, req->operationId);
            if (!record) {
                resp->status = SERVICE_STATUS_ERROR;
                resp->operationState = SERVICE_OPERATION_OUTCOME_UNKNOWN;
                gc_strlcpy(resp->message, sizeof(resp->message),
                    "operation outcome is unknown to this daemon generation");
            } else {
                resp->status = record->responseStatus;
                resp->operationState = record->state;
                resp->outcomeSeverity = record->outcomeSeverity;
                gc_strlcpy(resp->message, sizeof(resp->message),
                    record->message[0] ? record->message :
                    "operation result available");
            }
            if (ctl.hasActiveDesired) resp->desired = ctl.activeDesired;
            populate_snapshot(ctl, &resp->snapshot, &resp->controlState);
            break;
        }
        case SERVICE_CMD_APPLY: {
            LinuxOperationRequestGuard operation(ctl.operationTracker, req, resp, "apply");
            if (!operation.execute()) {
                if (ctl.hasActiveDesired) resp->desired = ctl.activeDesired;
                populate_snapshot(ctl, &resp->snapshot, &resp->controlState);
                break;
            }
            daemon_standby_fan_stash_clear(ctl, "an explicit Apply");
            daemon_standby_window_close(ctl, "an explicit Apply");
            if (!ctl.gpuReady) {
                resp->status = SERVICE_STATUS_ERROR;
                gc_strlcpy(resp->message, sizeof(resp->message), "GPU backend not initialized");
                break;
            }
            char targetErr[256] = {};
            if (!select_request_gpu(ctl, req, targetErr, sizeof(targetErr))) {
                resp->status = SERVICE_STATUS_ERROR;
                gc_strlcpy(resp->message, sizeof(resp->message), targetErr);
                break;
            }
            DesiredSettings d = req->desired;
            validate_desired_settings_for_ipc(&d);
            if (d.applicationScope == DESIRED_SCOPE_LEGACY && d.hasLock && d.lockMode == LOCK_MODE_NONE) {
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
            bool hadPrevious = ctl.hasActiveDesired;
            const bool wasUncertain = ctl.stateUncertain;
            DesiredSettings previousDesired = ctl.activeDesired;
            GpuAdapterInfo previousTarget = ctl.activeTarget;
            const GpuLocationRole committedRole = linux_gpu_location_role_for_selection(
                &ctl.gpu.selectedGpu, ctl.gpu.adapters, ctl.gpu.adapterCount);
            DesiredSettings committedDesired{};
            if (!daemon_prepare_apply_ownership(ctl, hadPrevious ? &previousDesired : nullptr,
                    &d, &committedDesired, targetErr, sizeof(targetErr))) {
                resp->status = SERVICE_STATUS_ERROR;
                gc_strlcpy(resp->message, sizeof(resp->message), targetErr);
                break;
            }
            LinuxJournalPin journal = {};
            if (!daemon_journal_admit(ctl, LINUX_JOURNAL_WRITE_APPLY, &journal, targetErr, sizeof(targetErr))) {
                resp->status = SERVICE_STATUS_ERROR;
                gc_snprintf(resp->message, sizeof(resp->message), "Apply refused: %s", targetErr);
                break;
            }
            const LinuxDaemonStateRecord* journalPreimage = journal.present ? &journal.record : nullptr;
            // The UUID verified before the write is pinned through commit (a later failed read never drops it).
            const LinuxGpuUuid learned = linux_gpu_uuid_retain(linux_backend_selected_uuid(&ctl.gpu),
                journal.owner == LINUX_JOURNAL_OWNER_SERVED ? &journal.record.targetUuid
                : hadPrevious ? &ctl.activeTargetUuid : nullptr), *learnedUuid = &learned;
            LinuxHardwareSnapshot before = {};
            char stateErr[256] = {};
            GraphicsScalarOperation graphicsOperation(ctl.gpu.graphicsRecovery);
            LinuxDaemonStateRecord uncertainRecord = {};
            bool capturedUncertain = false;
            if (!capture_uncertain_record(ctl, wasUncertain, &uncertainRecord,
                                          &capturedUncertain, stateErr, sizeof(stateErr)) ||
                !daemon_fan_ownership_allows_selected_write(ctl, stateErr, sizeof(stateErr)) ||
                !linux_backend_capture_apply_snapshot(&ctl.gpu, &d, &before, stateErr, sizeof(stateErr),
                                                      hadPrevious ? &previousDesired : nullptr, &committedDesired) ||
                !daemon_fan_ownership_ensure_before_write(ctl, &committedDesired,
                                                          stateErr, sizeof(stateErr)) ||
                !store_daemon_record(ctl, LINUX_DAEMON_RECORD_PREPARED, &ctl.gpu.selectedGpu,
                                     &committedDesired, stateErr, sizeof(stateErr),
                                     req->operationId, SERVICE_OPERATION_IN_PROGRESS, committedRole, learnedUuid)) {
                resp->status = SERVICE_STATUS_ERROR;
                gc_snprintf(resp->message, sizeof(resp->message),
                            "Apply aborted before hardware write: %s", stateErr);
                break;
            }
            // One ceiling transaction for the apply and any rollback of it.
            LinuxApplyCeilingTransaction ceiling = {};
            LinuxMutationResult mutation = linux_backend_apply(&ctl.gpu, ceiling, &d,
                hadPrevious ? &previousDesired : nullptr, &committedDesired,
                resp->message, sizeof(resp->message));
            bool committed = false;
            if (mutation.success) {
                committed = ctl.gpu.writeIdentityResolved && (service_scoped_has_owned_intent(committedDesired)
                    ? store_daemon_record(ctl, LINUX_DAEMON_RECORD_ACTIVE, &ctl.gpu.selectedGpu, &committedDesired,
                    stateErr, sizeof(stateErr),  // a binding that lost its identity is never committed
                    req->operationId, SERVICE_OPERATION_SUCCEEDED, committedRole, learnedUuid)
                    : linux_daemon_state_remove(GC_DAEMON_STATE_FILE, stateErr, sizeof(stateErr)));
                if (!committed) {
                    char rollbackErr[256] = {};
                    bool rollbackOk = linux_backend_restore_snapshot(&ctl.gpu, ceiling, &before,
                        mutation.attemptedPhases, rollbackErr, sizeof(rollbackErr));
                    bool recordOk = rollbackOk && restore_committed_record(ctl, wasUncertain, hadPrevious,
                                                              &previousTarget, &previousDesired,
                                                         capturedUncertain ? &uncertainRecord : nullptr, journalPreimage);
                    if (!rollbackOk || !recordOk)
                        store_daemon_record(ctl, LINUX_DAEMON_RECORD_UNCERTAIN, &ctl.gpu.selectedGpu,
                                            &committedDesired, stateErr, sizeof(stateErr), 0, SERVICE_OPERATION_NONE,
                                            committedRole, learnedUuid);
                    if (linux_failed_operation_leaves_uncertain(wasUncertain, rollbackOk, recordOk))
                        daemon_enter_uncertain_state(ctl, "apply persistence failed");
                    gc_snprintf(resp->message, sizeof(resp->message),
                        "Apply persistence failed; hardware rollback %s",
                        rollbackOk ? "succeeded" : "is uncertain");
                }
            } else if (mutation.rollbackSucceeded || !mutation.anyWrite) {
                bool recordOk = restore_committed_record(ctl, wasUncertain, hadPrevious,
                                                         &previousTarget, &previousDesired,
                                                         capturedUncertain ? &uncertainRecord : nullptr, journalPreimage);
                if (!recordOk) {
                    store_daemon_record(ctl, LINUX_DAEMON_RECORD_UNCERTAIN, &ctl.gpu.selectedGpu,
                                        &committedDesired, stateErr, sizeof(stateErr), 0, SERVICE_OPERATION_NONE,
                                        committedRole, learnedUuid);
                    daemon_enter_uncertain_state(ctl, "apply record restore failed");
                }
            } else {
                store_daemon_record(ctl, LINUX_DAEMON_RECORD_UNCERTAIN, &ctl.gpu.selectedGpu,
                                    &committedDesired, stateErr, sizeof(stateErr), 0, SERVICE_OPERATION_NONE,
                                    committedRole, learnedUuid);
                daemon_enter_uncertain_state(ctl, "apply rollback failed");
            }
            if (committed) {
                ctl.activeDesired = committedDesired;
                ctl.activeTargetRole = committedRole;
                ctl.activeTarget = ctl.gpu.selectedGpu;
                ctl.activeTargetUuid = learned;
                ctl.hasActiveDesired = service_scoped_has_owned_intent(committedDesired);
                if (!ctl.hasActiveDesired) {
                    ctl.activeTarget = {}; ctl.activeTargetUuid = {};
                    ctl.activeTargetRole = GPU_LOCATION_ROLE_REQUIRED;
                }
                ctl.stateUncertain = false;
                if (req->desired.hasFan) daemon_fan_runtime_note_commit(ctl, "a committed Apply"); else ctl.fanFailureCount = 0;
                if (ctl.fanOwnershipMarkerCommitted &&
                    ((d.hasFan && d.fanMode == FAN_MODE_AUTO) ||
                     (d.applicationScope != DESIRED_SCOPE_LEGACY && !committedDesired.hasFan)))
                    daemon_fan_ownership_retire(ctl, "committed intent leaves the fan to the driver");
                char guardErr[256] = {};
                if (!auto_restore_note_explicit_success(ctl, "apply", guardErr,
                                                        sizeof(guardErr))) {
                    resp->outcomeSeverity = SERVICE_OUTCOME_SEVERITY_WARNING;
                    gc_snprintf(resp->message, sizeof(resp->message),
                        "Apply committed, but the automatic restore guard "
                        "could not be cleared durably: %s",
                        guardErr[0] ? guardErr : "unknown error");
                }
                wake_fan_runtime(ctl);
            }
            populate_snapshot(ctl, &resp->snapshot, &resp->controlState);
            if (ctl.hasActiveDesired) resp->desired = ctl.activeDesired;
            resp->status = committed ? SERVICE_STATUS_OK : SERVICE_STATUS_ERROR;
            if (ctl.stateUncertain) wake_fan_runtime(ctl);
            break;
        }
        case SERVICE_CMD_RESET: {
            LinuxOperationRequestGuard operation(ctl.operationTracker, req, resp, "reset");
            if (!operation.execute()) {
                if (ctl.hasActiveDesired) resp->desired = ctl.activeDesired;
                populate_snapshot(ctl, &resp->snapshot, &resp->controlState);
                break;
            }
            daemon_standby_fan_stash_clear(ctl, "an explicit Reset");
            daemon_standby_window_close(ctl, "an explicit Reset");
            if (!ctl.gpuReady) {
                resp->status = SERVICE_STATUS_ERROR;
                gc_strlcpy(resp->message, sizeof(resp->message), "GPU backend not initialized");
                break;
            }
            char targetErr[256] = {};
            if (!select_request_gpu(ctl, req, targetErr, sizeof(targetErr))) {
                resp->status = SERVICE_STATUS_ERROR;
                gc_strlcpy(resp->message, sizeof(resp->message), targetErr);
                break;
            }
            bool hadPrevious = ctl.hasActiveDesired;
            const bool wasUncertain = ctl.stateUncertain;
            DesiredSettings previousDesired = ctl.activeDesired;
            GpuAdapterInfo previousTarget = ctl.activeTarget;
            const bool ownsSelected = hadPrevious && linux_gpu_identity_matches(&previousTarget, &ctl.gpu.selectedGpu);
            const DesiredSettings* previous = ownsSelected ? &previousDesired : nullptr;
            LinuxJournalPin journal = {};
            if (!daemon_journal_admit(ctl, LINUX_JOURNAL_WRITE_RESET, &journal, targetErr, sizeof(targetErr))) {
                resp->status = SERVICE_STATUS_ERROR;
                gc_snprintf(resp->message, sizeof(resp->message), "Reset refused: %s", targetErr);
                break;
            }
            const LinuxDaemonStateRecord* journalPreimage = journal.present ? &journal.record : nullptr;
            const LinuxGpuUuid resetUuid = linux_gpu_uuid_retain(linux_backend_selected_uuid(&ctl.gpu),
                journal.owner == LINUX_JOURNAL_OWNER_SERVED ? &journal.record.targetUuid
                : ownsSelected ? &ctl.activeTargetUuid : nullptr), *learnedUuid = &resetUuid;
            if (journal.decision == LINUX_JOURNAL_RELEASE) {
                if (!daemon_journal_release_for_reset(ctl, journal, resp)) break;
                journalPreimage = nullptr;
            }
            LinuxHardwareSnapshot before = {};
            char stateErr[256] = {};
            DesiredSettings empty = {};
            GraphicsScalarOperation graphicsOperation(ctl.gpu.graphicsRecovery);
            LinuxDaemonStateRecord uncertainRecord = {};
            bool capturedUncertain = false;
            if (!capture_uncertain_record(ctl, wasUncertain, &uncertainRecord,
                                          &capturedUncertain, stateErr, sizeof(stateErr), true) ||
                !daemon_fan_ownership_allows_selected_write(ctl, stateErr, sizeof(stateErr), true) ||
                !linux_backend_capture_reset_snapshot(&ctl.gpu, previous, &before, stateErr, sizeof(stateErr)) ||
                !store_daemon_record(ctl, LINUX_DAEMON_RECORD_PREPARED, &ctl.gpu.selectedGpu,
                                     &empty, stateErr, sizeof(stateErr),
                                     req->operationId, SERVICE_OPERATION_IN_PROGRESS,
                                     GPU_LOCATION_ROLE_UNRECORDED, learnedUuid)) {
                resp->status = SERVICE_STATUS_ERROR;
                gc_snprintf(resp->message, sizeof(resp->message),
                            "Reset aborted before hardware write: %s", stateErr);
                break;
            }
            LinuxApplyCeilingTransaction ceiling = {};
            LinuxMutationResult mutation = linux_backend_reset(&ctl.gpu, ceiling, previous,
                resp->message, sizeof(resp->message));
            bool committed = mutation.success && ctl.gpu.writeIdentityResolved &&  // as Apply's
                linux_daemon_state_remove(GC_DAEMON_STATE_FILE, stateErr, sizeof(stateErr));
            if (mutation.success && !committed) {
                char rollbackErr[256] = {};
                bool rollbackOk = linux_backend_restore_snapshot(&ctl.gpu, ceiling, &before,
                    mutation.attemptedPhases, rollbackErr, sizeof(rollbackErr));
                bool recordOk = rollbackOk && restore_committed_record(ctl, wasUncertain, hadPrevious,
                                                         &previousTarget, &previousDesired,
                                                         capturedUncertain ? &uncertainRecord : nullptr, journalPreimage);
                if (!rollbackOk || !recordOk)  // as Apply: a restart must not replay a record the hardware left
                    store_daemon_record(ctl, LINUX_DAEMON_RECORD_UNCERTAIN, &ctl.gpu.selectedGpu, &empty, stateErr,
                        sizeof(stateErr), 0, SERVICE_OPERATION_NONE, GPU_LOCATION_ROLE_UNRECORDED, learnedUuid);
                // The rollback put back the pre-Reset fan, possibly a manual duty.
                if (linux_failed_operation_leaves_uncertain(wasUncertain, rollbackOk, recordOk))
                    daemon_enter_uncertain_state(ctl, "reset persistence failed");
                gc_snprintf(resp->message, sizeof(resp->message),
                    "Reset persistence failed; hardware rollback %s",
                    rollbackOk ? "succeeded" : "is uncertain");
            } else if (!mutation.success && !(mutation.rollbackSucceeded || !mutation.anyWrite)) {
                store_daemon_record(ctl, LINUX_DAEMON_RECORD_UNCERTAIN, &ctl.gpu.selectedGpu,
                                    &empty, stateErr, sizeof(stateErr), 0, SERVICE_OPERATION_NONE,
                                    GPU_LOCATION_ROLE_UNRECORDED, learnedUuid);
                daemon_enter_uncertain_state(ctl, "reset rollback failed");
            } else if (!mutation.success) {
                bool recordOk = restore_committed_record(ctl, wasUncertain, hadPrevious,
                                                         &previousTarget, &previousDesired,
                                                         capturedUncertain ? &uncertainRecord : nullptr, journalPreimage);
                if (!recordOk) {
                    store_daemon_record(ctl, LINUX_DAEMON_RECORD_UNCERTAIN, &ctl.gpu.selectedGpu,
                                        &empty, stateErr, sizeof(stateErr), 0, SERVICE_OPERATION_NONE,
                                        GPU_LOCATION_ROLE_UNRECORDED, learnedUuid);
                    daemon_enter_uncertain_state(ctl, "reset record restore failed");
                }
            }
            if (committed) {
                ctl.hasActiveDesired = false;
                memset(&ctl.activeDesired, 0, sizeof(ctl.activeDesired));
                memset(&ctl.activeTarget, 0, sizeof(ctl.activeTarget));
                // The role belongs to the cleared target: back to the
                // fail-closed default, like a daemon that never had one.
                ctl.activeTargetRole = GPU_LOCATION_ROLE_REQUIRED;
                ctl.activeTargetUuid = {};
                ctl.stateUncertain = false;
                daemon_fan_runtime_note_commit(ctl, "a committed Reset");
                // Reset returned the fan to the driver: nothing to hand back.
                daemon_fan_ownership_retire(ctl, "reset committed", true);
                char guardErr[256] = {};
                if (!auto_restore_note_explicit_success(ctl, "reset", guardErr,
                                                        sizeof(guardErr))) {
                    resp->outcomeSeverity = SERVICE_OUTCOME_SEVERITY_WARNING;
                    gc_snprintf(resp->message, sizeof(resp->message),
                        "Reset committed, but the automatic restore guard "
                        "could not be cleared durably: %s",
                        guardErr[0] ? guardErr : "unknown error");
                }
                wake_fan_runtime(ctl);
            }
            populate_snapshot(ctl, &resp->snapshot, &resp->controlState);
            resp->status = committed ? SERVICE_STATUS_OK : SERVICE_STATUS_ERROR;
            if (ctl.stateUncertain) wake_fan_runtime(ctl);
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
    daemon_stamp_state_envelope(ctl, resp, req->command == SERVICE_CMD_CAPTURE_CURRENT);
    // The daemon's counterpart of the Windows write-out stamp, and for the same
    // reason: every branch above sets `status` and breaks, so severity is
    // derived once here instead of in each of them.  The Linux hardware
    // mutation path is transactional and has no partial-verify warning class,
    // but an otherwise committed explicit Apply/Reset warns when its separate
    // automatic-restore guard cannot be re-armed durably.
    resp->outcomeSeverity = service_response_resolve_outcome_severity(
        resp->status, resp->outcomeSeverity);
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
#include "linux_daemon_boot.h"

static int daemon_run_owned(const char* configPath) {
    // The only place the controller is named. Static storage: it is large and
    // outlives the fan worker, which is joined before this returns.
    static LinuxGpuController controller;
    LinuxGpuController& ctl = controller;
    linux_gpu_controller_init(&ctl);
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
    // its state in `static` storage, so the daemon's copy needs the descriptors
    // too or a daemon crash would only reach the journal.
    linux_set_crash_log_fd(linux_debug_log_raw_fd());
    linux_set_crash_phase("daemon-init");
    char daemonConfigToken[32] = {};
    char daemonLogToken[32] = {};
    gc_log_path_token(configPath, daemonConfigToken, sizeof(daemonConfigToken));
    gc_log_path_token(linux_debug_log_path(), daemonLogToken,
                      sizeof(daemonLogToken));
    dlog("daemon: version=%s build=%u protocol=%u pid=%ld config=%s log=%s\n",
         APP_VERSION, (unsigned int)APP_BUILD_NUMBER,
         (unsigned int)SERVICE_PROTOCOL_VERSION, (long)getpid(),
         daemonConfigToken,
         daemonLogToken);

    char err[256] = {};
    if (linux_backend_init(&ctl.gpu, &g_driver, nullptr, err, sizeof(err))) {
        ctl.gpuReady = true;
        dlog("daemon: GPU backend online (%s family=%d match=%s architectureSource=%s "
             "vfFresh=%d infoStatus=%d statusStatus=%d controlStatus=%d "
             "health=%s)\n",
             ctl.gpu.gpuName, (int)ctl.gpu.family,
             linux_gpu_match_method_name(ctl.gpu.nvapiMatchMethod),
             linux_gpu_architecture_source_name(ctl.gpu.architectureSource),
             ctl.gpu.vfSnapshotFresh ? 1 : 0,
             ctl.gpu.vfInfoStatus, ctl.gpu.vfStatusStatus,
             ctl.gpu.vfControlStatus,
             service_gpu_health_reason_name(ctl.gpu.health.reason));
    } else {
        dlog("daemon: GPU backend init failed: %s (serving telemetry-less)\n", err);
    }

    LinuxDaemonOperationRecord operation = {};
    if (linux_daemon_operation_load(GC_DAEMON_OPERATION_FILE, &operation,
            err, sizeof(err))) {
        gc_u32 restoredState = operation.state == SERVICE_OPERATION_IN_PROGRESS
            ? SERVICE_OPERATION_OUTCOME_UNKNOWN : operation.state;
        service_operation_restore(&ctl.operationTracker, operation.operationId,
            restoredState, operation.responseStatus, operation.outcomeSeverity,
            restoredState == SERVICE_OPERATION_OUTCOME_UNKNOWN
                ? "operation outcome became uncertain across daemon restart"
                : operation.message);
    }

    load_auto_restore_guard_at_boot(ctl);
    // After the guard load (a give-up latches it), before any startup write
    // and before the fan worker exists: a previous daemon in this boot that
    // died with the fan under manual control gets it handed back to the
    // driver, at most twice (linux_fan_ownership.h).
    daemon_fan_ownership_handback_at_start(ctl);
    load_startup_policy_at_boot(ctl);

    daemon_apply_boot_policy(ctl);
    // Before the fan worker exists: a lockout opens the standby window only
    // after a suspend this daemon lived through.
    daemon_standby_window_close(ctl, "daemon start");

    pl_thread fanThread;
    const bool workerStarted = pl_thread_start(&fanThread, fan_reassert_thread, &ctl);
    if (!workerStarted) {
        // Fatal, not "degraded".  Without this worker a curve or fixed duty is
        // written once and never re-asserted, and the driver silently takes the
        // fan back at the next opportunity -- a running daemon that reports
        // healthy while quietly not controlling the fan is worse than one that
        // is visibly absent.  Windows treats lifecycle-worker creation failure
        // the same way: it stops startup.  Exiting non-zero also lets the unit's
        // Restart= net make another attempt.
        dlog("daemon: fan reassertion worker could not be started; refusing to "
             "serve without it\n");
    }
    DaemonListener listener = {};
    bool listenerOpened = false;
    int exitStatus = 1;
    if (workerStarted) {
        listenerOpened = daemon_open_listener(&listener);
        if (listenerOpened) {
            char readinessError[256] = {};
            ServiceSnapshot readinessSnapshot = {};
            ControlState readinessControls = {};
            pl_mutex_lock(&g_lock);
            populate_snapshot(ctl, &readinessSnapshot, &readinessControls);
            pl_mutex_unlock(&g_lock);
            if (linux_systemd_notify_ready(&readinessSnapshot.health,
                    readinessError, sizeof(readinessError))) {
                linux_set_crash_phase("daemon-serving");
                exitStatus = daemon_serve_until_stopped(ctl, listener.socketFd);
            } else {
                dlog("daemon: startup readiness failed: %s\n", readinessError);
            }
        }
    }

    linux_set_crash_phase("daemon-shutdown");
    g_running = 0;
    if (exitStatus == 0) clear_auto_restore_attempts_on_clean_stop(ctl);
    if (workerStarted) {
        wake_fan_runtime(ctl);
        pl_thread_join(fanThread);
    }
    daemon_release_fan_to_driver(ctl);
    if (listenerOpened) daemon_close_listener(&listener);
    close_daemon_shutdown_pipe();
    linux_backend_shutdown(&ctl.gpu);
    linux_driver_close(&g_driver);
    dlog("daemon: exiting with status %d\n", exitStatus);
    return exitStatus;
}

#include "linux_service_install.cpp"

int linux_daemon_run(const char* configPath) {
    // StateDirectory outlives systemd's per-instance RuntimeDirectory.
    char lockError[256] = {};
    const int lockFd = linux_daemon_lock_instance(GC_DAEMON_STATE_FILE,
        lockError, sizeof(lockError));
    if (lockFd < 0) {
        fprintf(stderr, "greencurve daemon: %s\n", lockError);
        return 1;
    }
    const int result = daemon_run_owned(configPath);
    close(lockFd);
    return result;
}
