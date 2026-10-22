// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The driver-recovery flow of a fresh, validated service process as one
// testable unit: arming the validated controlled recovery before RUNNING,
// queueing it behind the awake-time proof and the spam threshold, and the
// lifecycle worker's single restore write.
//
// These used to be three blocks of shard code the regression harness never
// compiles, so their order -- in-progress published before queueing, a mature
// proof captured before the write, proof, snapshot and in-progress retired only
// after a success -- was held by source-text gates alone. Every side effect now
// goes through an `Ops` object: the service passes its runtime
// (main_service_driver_recovery.cpp), tests/driver_recovery_sequence_tests.cpp
// a simulated one, and both run this code with the real lifecycle reducer.
//
// Include after app_shared.h (service_lifecycle_policy.h needs DesiredSettings).
//
// Ops (all called on the caller's thread, in the order written here):
//   proof_allows(uint32_t* lockoutReason, uint64_t* proofAgeMs) -> bool
//   disable_automatic_restore(uint32_t reason, const char* context)
//   reduce(const ServiceLifecycleEvent&) -> ServiceLifecycleDecision
//   recent_recoveries() -> unsigned
//   note_queued(const ServiceDriverRecoveryQueueReport&)
//   signal_worker()
//   controlled_recovery_validated() -> bool
//   lock_runtime(), unlock_runtime()
//   publish_provisional_intent()      intent + in-progress, before queueing
//   auto_restore_locked_out() -> bool
//   drop_provisional_intent(const char* reason)
//   discard_controlled_recovery()
//   driver_restore_pending() -> bool
//   update_install_takes_runtime() -> bool   true: it released the runtime
//   has_owned_intent() -> bool
//   gpu_ready(char* detail, size_t size) -> bool
//   prepare_write()
//   capture_mature_proof(uint64_t* ageMs) -> bool
//   write_target_current() -> bool
//   apply_full_intent(char* detail, size_t size, bool* writeAttempted) -> bool
//   refresh_target_after_success()
//   commit_proof(bool preserveMature) -> bool
//   publish_recovered_profile()
//   write_restart_snapshot()
//   retire_controlled_recovery()      clears in-progress
//   latch_write_failure(const char* detail)
//   finish_write(const ServiceLifecycleEvent&) -> ServiceLifecycleResult
//   note_finished(const ServiceDriverRestoreReport&, const char* detail)

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "service_protocol.h"
#include "service_lifecycle_policy.h"

enum ServiceDriverRecoveryQueueStep {
    SERVICE_DRIVER_QUEUE_PROOF_REFUSED = 0,  // lockout latched
    SERVICE_DRIVER_QUEUE_SPAM_REFUSED,       // TDR_SPAM latched
    SERVICE_DRIVER_QUEUE_REDUCED,            // handed to the reducer; see wake
};

struct ServiceDriverRecoveryQueueReport {
    ServiceDriverRecoveryQueueStep step;
    bool wake;
    uint64_t proofAgeMs;
    unsigned recentRecoveries;
};

enum ServiceDriverRecoveryArmStep {
    SERVICE_DRIVER_ARM_NOT_VALIDATED = 0,  // ordinary start: nothing to arm
    SERVICE_DRIVER_ARM_QUEUED,
    SERVICE_DRIVER_ARM_REFUSED,            // locked out; provisional intent dropped
};

enum ServiceDriverRestoreStep {
    SERVICE_DRIVER_RESTORE_NOT_PENDING = 0,
    SERVICE_DRIVER_RESTORE_UPDATE_BLOCKED,   // an update install holds the runtime
    SERVICE_DRIVER_RESTORE_NOT_VALIDATED,    // old or superseded process: never writes
    SERVICE_DRIVER_RESTORE_PROOF_REFUSED,    // lockout latched
    SERVICE_DRIVER_RESTORE_INTENT_LOST,      // lockout latched
    SERVICE_DRIVER_RESTORE_NOT_READY,        // stays pending for a real readiness cue
    SERVICE_DRIVER_RESTORE_NOT_AUTHORIZED,   // the reducer refused the write
    SERVICE_DRIVER_RESTORE_WRITE_PATH,       // see writeAttempted / success
};

struct ServiceDriverRestoreReport {
    ServiceDriverRestoreStep step;
    bool writeAttempted;
    bool success;
    bool preserveMatureProof;
    bool proofCommitted;
    uint64_t matureProofAgeMs;
    ServiceLifecycleResult result;
};

template <typename Ops>
static inline void service_driver_recovery_reduce_lockout(Ops& ops) {
    ServiceLifecycleEvent lockout = {};
    lockout.type = SERVICE_LIFECYCLE_EVENT_LOCKOUT;
    ops.reduce(lockout);
}

// Queue a validated controlled recovery. The awake-time proof (or a verified
// driver update since the apply) and the persisted recovery ledger's spam
// threshold gate it; a refusal latches the sticky lockout without a write.
template <typename Ops>
static inline ServiceDriverRecoveryQueueReport service_driver_recovery_queue(
    Ops& ops, unsigned spamThreshold) {
    ServiceDriverRecoveryQueueReport report = {};
    uint32_t lockoutReason = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
    if (!ops.proof_allows(&lockoutReason, &report.proofAgeMs)) {
        ops.disable_automatic_restore(
            lockoutReason != SERVICE_AUTO_RESTORE_LOCKOUT_NONE
                ? lockoutReason
                : (uint32_t)SERVICE_AUTO_RESTORE_LOCKOUT_UNSTABLE_APPLY,
            "controlled driver recovery did not satisfy the awake-time proof");
        service_driver_recovery_reduce_lockout(ops);
        report.step = SERVICE_DRIVER_QUEUE_PROOF_REFUSED;
        return report;
    }
    report.recentRecoveries = ops.recent_recoveries();
    if (report.recentRecoveries >= spamThreshold) {
        ops.disable_automatic_restore(SERVICE_AUTO_RESTORE_LOCKOUT_TDR_SPAM,
            "persistent recovery ledger reached the spam threshold");
        service_driver_recovery_reduce_lockout(ops);
        report.step = SERVICE_DRIVER_QUEUE_SPAM_REFUSED;
        return report;
    }
    ServiceLifecycleEvent event = {};
    event.type = SERVICE_LIFECYCLE_EVENT_DRIVER_RECOVERY;
    event.driverProofReady = true;
    report.wake = ops.reduce(event).wakeWorker != 0;
    report.step = SERVICE_DRIVER_QUEUE_REDUCED;
    ops.note_queued(report);
    if (report.wake) ops.signal_worker();
    return report;
}

// Before RUNNING: publish the validated snapshot as provisional intent and
// queue its restore. In-progress goes up first, so the fresh process's own
// device arrival reads as the write's readiness cue rather than as a new
// recovery (service_selected_gpu_arrival_action). A recovery that cannot be
// queued leaves no intent behind: it would keep the GPU phase at REAPPLYING.
template <typename Ops>
static inline ServiceDriverRecoveryArmStep service_driver_recovery_arm(
    Ops& ops, unsigned spamThreshold, ServiceDriverRecoveryQueueReport* queueOut = nullptr) {
    if (queueOut) *queueOut = ServiceDriverRecoveryQueueReport();
    if (!ops.controlled_recovery_validated()) return SERVICE_DRIVER_ARM_NOT_VALIDATED;
    ops.lock_runtime();
    ops.publish_provisional_intent();
    ops.unlock_runtime();
    const ServiceDriverRecoveryQueueReport queue =
        service_driver_recovery_queue(ops, spamThreshold);
    if (queueOut) *queueOut = queue;
    if (queue.wake) return SERVICE_DRIVER_ARM_QUEUED;
    if (!ops.auto_restore_locked_out()) {
        ops.disable_automatic_restore(SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED,
            "validated controlled recovery could not be queued");
    } else {
        ops.drop_provisional_intent("recovery refused under an existing lockout");
    }
    ops.discard_controlled_recovery();
    return SERVICE_DRIVER_ARM_REFUSED;
}

// The lifecycle worker's attempt. Repeats freely while the GPU is not ready;
// the reducer authorizes exactly one hardware write, and a write that was
// attempted is terminal (success or a latched failure), never retried.
template <typename Ops>
static inline ServiceDriverRestoreReport service_driver_restore_attempt(
    Ops& ops, char* detail, size_t detailSize) {
    ServiceDriverRestoreReport report = {};
    if (detail && detailSize) detail[0] = 0;
    if (!ops.driver_restore_pending()) return report;

    // Every continuation/proof check and any fail-closed transition is
    // serialized with explicit Apply/Reset: a proof read before the runtime
    // lock could race a newly successful Apply and erase the intent it just
    // published.
    ops.lock_runtime();
    if (ops.update_install_takes_runtime()) {
        report.step = SERVICE_DRIVER_RESTORE_UPDATE_BLOCKED;
        return report;
    }
    // Only the fresh process that validated helper, nonce, parent, snapshot,
    // freshness and SCM demand-start may write; never the stale-DLL process.
    if (!ops.controlled_recovery_validated()) {
        ops.unlock_runtime();
        report.step = SERVICE_DRIVER_RESTORE_NOT_VALIDATED;
        return report;
    }
    uint32_t lockoutReason = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
    uint64_t proofAgeMs = 0;
    if (!ops.proof_allows(&lockoutReason, &proofAgeMs)) {
        ops.disable_automatic_restore(
            lockoutReason != SERVICE_AUTO_RESTORE_LOCKOUT_NONE
                ? lockoutReason
                : (uint32_t)SERVICE_AUTO_RESTORE_LOCKOUT_UNSTABLE_APPLY,
            "driver restore authorization changed while waiting for runtime serialization");
        service_driver_recovery_reduce_lockout(ops);
        ops.unlock_runtime();
        report.step = SERVICE_DRIVER_RESTORE_PROOF_REFUSED;
        return report;
    }
    if (!ops.has_owned_intent()) {
        ops.disable_automatic_restore(SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED,
            "validated recovery intent lost its owned GPU identity");
        ops.unlock_runtime();
        report.step = SERVICE_DRIVER_RESTORE_INTENT_LOST;
        return report;
    }
    if (!ops.gpu_ready(detail, detailSize)) {
        ops.unlock_runtime();
        report.step = SERVICE_DRIVER_RESTORE_NOT_READY;
        return report;
    }

    ServiceLifecycleEvent start = {};
    start.type = SERVICE_LIFECYCLE_EVENT_DRIVER_WRITE_STARTED;
    start.driverProofReady = true;
    start.controlledRecoveryValidated = true;
    if (!ops.reduce(start).authorizeDriverWrite) {
        ops.unlock_runtime();
        report.step = SERVICE_DRIVER_RESTORE_NOT_AUTHORIZED;
        return report;
    }
    report.step = SERVICE_DRIVER_RESTORE_WRITE_PATH;
    ops.prepare_write();
    // Captured before the write, whose mandatory pre-write invalidation would
    // otherwise discard a proof that was already mature.
    report.preserveMatureProof = ops.capture_mature_proof(&report.matureProofAgeMs);
    if (ops.write_target_current()) {
        report.success = ops.apply_full_intent(detail, detailSize, &report.writeAttempted);
    } else if (detail && detailSize) {
        snprintf(detail, detailSize, "%s",
            "Selected GPU changed/was removed immediately before driver restore");
    }
    if (report.success) {
        ops.refresh_target_after_success();
        report.proofCommitted = ops.commit_proof(report.preserveMatureProof);
        ops.publish_recovered_profile();
        ops.write_restart_snapshot();
        ops.retire_controlled_recovery();
    } else if (report.writeAttempted) {
        // Still under the runtime lock, so a later explicit success wins.
        ops.latch_write_failure(detail);
    }
    ops.unlock_runtime();

    ServiceLifecycleEvent finish = {};
    finish.type = SERVICE_LIFECYCLE_EVENT_DRIVER_WRITE_FINISHED;
    finish.success = report.success;
    finish.writeAttempted = report.writeAttempted;
    report.result = ops.finish_write(finish);
    ops.note_finished(report, detail);
    // Driver recovery has released precedence: revisit any coalesced logon or
    // standby intent in a new reducer pass.
    if (report.success) ops.signal_worker();
    return report;
}
