// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// A driver update, scripted end to end (assertion codes 7210-7239): the old
// process's exit plan, the fresh process's start gate and handback scope, then
// the shared driver_recovery_flow.h -- arming, queueing behind the proof and
// spam gates, and the lifecycle worker's attempts -- against a simulated
// service, with the real lifecycle reducer, proof waiver and arrival policy.
//
// The service shards are not compiled here; they bind the same flow to the
// real runtime (main_service_driver_recovery.cpp), which source gates pin.
// What this file proves is the order and the outcome: exactly one hardware
// write, no write before readiness, proof/snapshot/in-progress retired only
// after a success, and every refusal latching its lockout with zero writes.

#include <stdio.h>
#include <string.h>

#include "fan_curve.h"
#include "service_protocol.h"
#include "service_lifecycle_policy.h"
#include "service_recovery_policy.h"
#include "ownership_handback_policy.h"
#include "driver_recovery_flow.h"

namespace {

// MAX_RECOVERIES_BEFORE_BACKOFF in main_service_recovery_ledger.cpp.
const unsigned kSpamThreshold = 3;
const uint32_t kDriverMs = 0x00200000u;      // 32.0
const uint32_t kOlderDriverLs = 0x000F17CEu; // .15.6094
const uint32_t kNewerDriverLs = 0x000F1FF4u; // .15.8180

// The simulated service: persisted protected state, process memory, the GPU,
// and what happened.
struct World {
    uint32_t lockout;
    bool snapshotOnDisk;
    bool proofValid;
    uint64_t proofAgeMs;
    uint32_t stampMs, stampLs;  // driver the proven apply ran under
    uint32_t diskMs, diskLs;    // driver image on disk now
    unsigned recentRecoveries;

    bool validated;
    bool hasIntent;
    bool reapplyInProgress;
    ServiceLifecycleState lifecycle;

    bool gpuReady;
    bool targetCurrent;
    bool applySucceeds;
    bool updateInstallPending;

    int lockDepth;
    int maxLockDepth;
    int unlockedMutations;  // intent/state changed without the runtime lock
    int hardwareWrites;
    int signals;
    int proofCommits;
    bool proofPreserved;
    int snapshotWrites;
    int failureLatches;
    const char* trace[96];
    int traceCount;
};

void step(World& w, const char* name) {
    if (w.traceCount < (int)(sizeof(w.trace) / sizeof(w.trace[0]))) w.trace[w.traceCount++] = name;
}

int position(const World& w, const char* name) {
    for (int i = 0; i < w.traceCount; ++i) {
        if (strcmp(w.trace[i], name) == 0) return i;
    }
    return -1;
}

bool ordered(const World& w, const char* first, const char* second) {
    const int a = position(w, first), b = position(w, second);
    return a >= 0 && b >= 0 && a < b;
}

bool traced(const World& w, const char* name) { return position(w, name) >= 0; }

struct FakeOps {
    World& w;

    void guarded(const char* name) {
        step(w, name);
        if (w.lockDepth != 1) ++w.unlockedMutations;
    }
    // As service_auto_restore_allowed_after_driver_event: lockout first, then
    // a valid same-boot stamp, mature or with a verified driver update.
    bool proof_allows(uint32_t* lockoutReason, uint64_t* proofAgeMs) {
        step(w, "proof_allows");
        if (w.lockout != SERVICE_AUTO_RESTORE_LOCKOUT_NONE) {
            *lockoutReason = w.lockout;
            return false;
        }
        if (!w.proofValid) return false;
        *proofAgeMs = w.proofAgeMs;
        if (should_auto_restore_after_driver_event(true, w.proofAgeMs, false)) return true;
        return service_driver_recovery_proof_accepts(w.proofAgeMs, AUTO_RESTORE_STABILITY_WINDOW_MS,
            service_driver_version_changed(w.stampMs, w.stampLs, w.diskMs, w.diskLs));
    }
    // As service_disable_automatic_restore.
    void disable_automatic_restore(uint32_t reason, const char*) {
        step(w, "disable_automatic_restore");
        if (w.lockout == SERVICE_AUTO_RESTORE_LOCKOUT_NONE) w.lockout = reason;
        w.snapshotOnDisk = false;
        w.proofValid = false;
        w.reapplyInProgress = false;
        w.hasIntent = false;
    }
    ServiceLifecycleDecision reduce(const ServiceLifecycleEvent& event) {
        step(w, "reduce");
        return service_lifecycle_reduce(&w.lifecycle, &event);
    }
    unsigned recent_recoveries() { return w.recentRecoveries; }
    void note_queued(const ServiceDriverRecoveryQueueReport&) { step(w, "note_queued"); }
    void signal_worker() {
        step(w, "signal_worker");
        ++w.signals;
    }
    bool controlled_recovery_validated() { return w.validated; }
    void lock_runtime() {
        if (++w.lockDepth > w.maxLockDepth) w.maxLockDepth = w.lockDepth;
    }
    void unlock_runtime() { --w.lockDepth; }
    void publish_provisional_intent() {
        guarded("publish_provisional_intent");
        w.hasIntent = true;
        w.reapplyInProgress = true;
    }
    bool auto_restore_locked_out() { return w.lockout != SERVICE_AUTO_RESTORE_LOCKOUT_NONE; }
    void drop_provisional_intent(const char*) {
        step(w, "drop_provisional_intent");
        w.hasIntent = false;
        w.reapplyInProgress = false;
    }
    void discard_controlled_recovery() {
        step(w, "discard_controlled_recovery");
        w.validated = false;
    }
    bool driver_restore_pending() {
        return w.lifecycle.driverPending && !w.lifecycle.driverWriteIssued && !w.lifecycle.lockedOut;
    }
    bool update_install_takes_runtime() {
        if (!w.updateInstallPending) return false;
        --w.lockDepth;  // service_update_install_release_and_block() releases it
        return true;
    }
    bool has_owned_intent() { return w.hasIntent; }
    bool gpu_ready(char* detail, size_t detailSize) {
        step(w, "gpu_ready");
        if (!w.gpuReady && detail && detailSize) snprintf(detail, detailSize, "Failed to initialize NvAPI");
        return w.gpuReady;
    }
    void prepare_write() { guarded("prepare_write"); }
    bool capture_mature_proof(uint64_t* ageMs) {
        step(w, "capture_mature_proof");
        if (!w.proofValid || w.proofAgeMs < AUTO_RESTORE_STABILITY_WINDOW_MS) return false;
        *ageMs = w.proofAgeMs;
        return true;
    }
    bool write_target_current() { return w.targetCurrent; }
    bool apply_full_intent(char*, size_t, bool* writeAttempted) {
        guarded("apply_full_intent");
        ++w.hardwareWrites;
        *writeAttempted = true;
        return w.applySucceeds;
    }
    void refresh_target_after_success() { step(w, "refresh_target_after_success"); }
    bool commit_proof(bool preserveMature) {
        guarded("commit_proof");
        ++w.proofCommits;
        w.proofPreserved = preserveMature;
        w.proofValid = true;
        if (!preserveMature) {  // a fresh stamp under the driver now loaded
            w.proofAgeMs = 0;
            w.stampMs = w.diskMs;
            w.stampLs = w.diskLs;
        }
        return true;
    }
    void publish_recovered_profile() { guarded("publish_recovered_profile"); }
    void write_restart_snapshot() {
        guarded("write_restart_snapshot");
        w.snapshotOnDisk = true;
        ++w.snapshotWrites;
    }
    void retire_controlled_recovery() {
        guarded("retire_controlled_recovery");
        w.validated = false;
        w.reapplyInProgress = false;
    }
    // As service_lifecycle_latch_write_failure.
    void latch_write_failure(const char* detail) {
        guarded("latch_write_failure");
        ++w.failureLatches;
        service_driver_recovery_reduce_lockout(*this);
        disable_automatic_restore(SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED, detail);
    }
    ServiceLifecycleResult finish_write(const ServiceLifecycleEvent& event) {
        step(w, "finish_write");
        return service_lifecycle_reduce(&w.lifecycle, &event).result;
    }
    void note_finished(const ServiceDriverRestoreReport&, const char*) { step(w, "note_finished"); }
};

// A fresh process the startup gate validated, holding the snapshot in memory
// (startup deleted the files once it had them), the proof the old process's
// apply left, and the driver on disk.
void fresh_process(World& w, uint64_t proofAgeMs, uint32_t stampLs, uint32_t diskLs) {
    memset(&w, 0, sizeof(w));
    w.proofValid = true;
    w.proofAgeMs = proofAgeMs;
    w.stampMs = kDriverMs;
    w.stampLs = stampLs;
    w.diskMs = kDriverMs;
    w.diskLs = diskLs;
    w.recentRecoveries = 1;
    w.validated = true;
    w.targetCurrent = true;
    w.applySucceeds = true;
}

ServiceDriverRestoreReport attempt(World& w, char* detail = nullptr, size_t detailSize = 0) {
    FakeOps ops = { w };
    char local[128] = {};
    return service_driver_restore_attempt(ops, detail ? detail : local,
                                          detail ? detailSize : sizeof(local));
}

ServiceDriverRecoveryArmStep arm(World& w) {
    FakeOps ops = { w };
    return service_driver_recovery_arm(ops, kSpamThreshold);
}

bool runtime_balanced(const World& w) {
    return w.lockDepth == 0 && w.maxLockDepth <= 1 && w.unlockedMutations == 0;
}

int fail(int code, const char* what) {
    printf("  %d: %s\n", code, what);
    return code;
}

// The old process faults in the driver while owning a restorable profile;
// the fresh one is validated and hands back the fan only.
int old_process_to_fresh_start() {
    if (service_poisoned_runtime_exit_plan(true, SERVICE_RESTART_SNAPSHOT_PRESENT, true, true) !=
        SERVICE_POISONED_EXIT_CONTROLLED_RESTART)
        return fail(7210, "a corroborated driver fault with a snapshot restarts under control");
    ServiceControlledRecoveryStartGate gate = {};
    gate.argumentsValid = gate.explicitlyRequested = gate.scmStartReasonKnown = true;
    gate.scmDemandStart = gate.authorizationValid = gate.helperValidated = gate.snapshotValid = true;
    if (!service_controlled_recovery_start_is_authorized(gate))
        return fail(7210, "the helper's demand start with a valid snapshot is authorized");
    if (ownership_handback_windows_start_scope(true) != OWNERSHIP_HANDBACK_SCOPE_FAN_ONLY)
        return fail(7210, "a validated recovery start hands back the fan only");
    return 0;
}

// The driver changed within 10 minutes of the apply: the waiver path, not yet
// observed on a real driver update.
int immature_proof_with_driver_update() {
    World w;
    fresh_process(w, 60000, kNewerDriverLs, kOlderDriverLs);
    if (arm(w) != SERVICE_DRIVER_ARM_QUEUED || w.lockout != SERVICE_AUTO_RESTORE_LOCKOUT_NONE ||
        !w.lifecycle.driverPending || !w.hasIntent || !w.reapplyInProgress || w.signals != 1 ||
        !ordered(w, "publish_provisional_intent", "note_queued") || !runtime_balanced(w))
        return fail(7211, "arming publishes in-progress, then queues and wakes the worker once");
    if (service_selected_gpu_arrival_action(false, w.hasIntent, w.reapplyInProgress) !=
        SERVICE_GPU_ARRIVAL_NONE)
        return fail(7212, "the fresh process's own device arrival is a readiness cue, not a recovery");
    char detail[128] = {};
    for (int i = 0; i < 2; ++i) {
        ServiceDriverRestoreReport r = attempt(w, detail, sizeof(detail));
        if (r.step != SERVICE_DRIVER_RESTORE_NOT_READY || w.hardwareWrites != 0 || !detail[0] ||
            !w.lifecycle.driverPending || w.lifecycle.driverWriteIssued || !runtime_balanced(w))
            return fail(7213, "until the GPU is ready every attempt waits without writing");
    }
    w.gpuReady = true;
    w.traceCount = 0;
    ServiceDriverRestoreReport r = attempt(w);
    if (r.step != SERVICE_DRIVER_RESTORE_WRITE_PATH || !r.success || !r.writeAttempted ||
        r.result != SERVICE_LIFECYCLE_RESULT_APPLIED || w.hardwareWrites != 1)
        return fail(7214, "the first ready attempt writes the full intent once and applies");
    if (r.preserveMatureProof || w.proofCommits != 1 || w.proofPreserved ||
        w.stampLs != kOlderDriverLs || w.snapshotWrites != 1 || !w.snapshotOnDisk)
        return fail(7215, "an immature proof is restamped under the new driver and the snapshot rewritten");
    if (w.validated || w.reapplyInProgress || !w.hasIntent || w.lifecycle.driverPending ||
        w.signals != 2 || !runtime_balanced(w))
        return fail(7216, "success retires the recovery and in-progress, keeps the intent, rewakes the worker");
    if (!ordered(w, "capture_mature_proof", "apply_full_intent") ||
        !ordered(w, "apply_full_intent", "commit_proof") ||
        !ordered(w, "commit_proof", "write_restart_snapshot") ||
        !ordered(w, "write_restart_snapshot", "retire_controlled_recovery") ||
        !ordered(w, "retire_controlled_recovery", "finish_write") ||
        !ordered(w, "note_finished", "signal_worker"))
        return fail(7217, "proof capture, write, proof, snapshot, retirement, reducer, wake: in that order");
    if (attempt(w).step != SERVICE_DRIVER_RESTORE_NOT_PENDING || w.hardwareWrites != 1)
        return fail(7218, "a later wake finds nothing pending and never writes twice");
    if (service_selected_gpu_arrival_action(false, w.hasIntent, w.reapplyInProgress) !=
        SERVICE_GPU_ARRIVAL_RECOVER)
        return fail(7219, "after the restore, the next device restart is a new recovery");
    return 0;
}

// Same driver within 10 minutes: the restart is stability evidence.
int immature_proof_same_driver() {
    World w;
    fresh_process(w, 60000, kNewerDriverLs, kNewerDriverLs);
    w.gpuReady = true;
    if (arm(w) != SERVICE_DRIVER_ARM_REFUSED ||
        w.lockout != SERVICE_AUTO_RESTORE_LOCKOUT_UNSTABLE_APPLY || !w.lifecycle.lockedOut ||
        w.hasIntent || w.reapplyInProgress || w.validated || w.signals != 0 ||
        !traced(w, "drop_provisional_intent") || !traced(w, "discard_controlled_recovery") ||
        !runtime_balanced(w))
        return fail(7220, "an unproven restart latches UNSTABLE_APPLY and leaves no intent behind");
    if (attempt(w).step != SERVICE_DRIVER_RESTORE_NOT_PENDING || w.hardwareWrites != 0)
        return fail(7221, "a refused recovery never writes");
    return 0;
}

// The case observed live on 2026-10-22: proof older than the window.
int mature_proof() {
    World w;
    fresh_process(w, 1000941, kNewerDriverLs, kOlderDriverLs);
    w.gpuReady = true;
    if (arm(w) != SERVICE_DRIVER_ARM_QUEUED) return fail(7222, "a mature proof queues");
    ServiceDriverRestoreReport r = attempt(w);
    if (!r.success || r.result != SERVICE_LIFECYCLE_RESULT_APPLIED || w.hardwareWrites != 1 ||
        !r.preserveMatureProof || r.matureProofAgeMs != 1000941 || !w.proofPreserved ||
        w.stampLs != kNewerDriverLs || !runtime_balanced(w))
        return fail(7222, "a mature proof is preserved, not restamped, across the restore");
    return 0;
}

int spam_threshold() {
    World w;
    fresh_process(w, 1000941, kNewerDriverLs, kOlderDriverLs);
    w.recentRecoveries = kSpamThreshold;
    w.gpuReady = true;
    if (arm(w) != SERVICE_DRIVER_ARM_REFUSED ||
        w.lockout != SERVICE_AUTO_RESTORE_LOCKOUT_TDR_SPAM || w.hasIntent || w.reapplyInProgress ||
        attempt(w).step != SERVICE_DRIVER_RESTORE_NOT_PENDING || w.hardwareWrites != 0)
        return fail(7223, "the recovery ledger's threshold latches TDR_SPAM before any write");
    return 0;
}

int failed_write() {
    World w;
    fresh_process(w, 1000941, kNewerDriverLs, kOlderDriverLs);
    w.gpuReady = true;
    w.applySucceeds = false;
    arm(w);
    ServiceDriverRestoreReport r = attempt(w);
    // The latch reduces LOCKOUT before the write is finished, so the reducer
    // is already locked out (the service records LOCKED_OUT) and the finish
    // changes nothing.
    if (r.success || !r.writeAttempted || r.result == SERVICE_LIFECYCLE_RESULT_APPLIED ||
        !w.lifecycle.lockedOut || w.hardwareWrites != 1 || w.failureLatches != 1 ||
        w.lockout != SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED ||
        w.proofCommits != 0 || w.snapshotWrites != 0 || w.lifecycle.driverPending ||
        !runtime_balanced(w))
        return fail(7224, "an attempted write that fails is terminal and latches its cause");
    if (attempt(w).step != SERVICE_DRIVER_RESTORE_NOT_PENDING || w.hardwareWrites != 1)
        return fail(7224, "a failed recovery write is never retried");
    return 0;
}

// The selected GPU changed between preparation and the write.
int target_changed_before_write() {
    World w;
    fresh_process(w, 1000941, kNewerDriverLs, kOlderDriverLs);
    w.gpuReady = true;
    w.targetCurrent = false;
    arm(w);
    char detail[128] = {};
    ServiceDriverRestoreReport r = attempt(w, detail, sizeof(detail));
    if (r.writeAttempted || r.success || r.result != SERVICE_LIFECYCLE_RESULT_TRANSIENT_NOT_READY ||
        w.hardwareWrites != 0 || !strstr(detail, "Selected GPU changed") ||
        !w.lifecycle.driverPending || w.lifecycle.driverWriteIssued || w.proofCommits != 0 ||
        !runtime_balanced(w))
        return fail(7225, "a stale target skips the write and keeps the restore pending");
    w.targetCurrent = true;
    r = attempt(w);
    if (!r.success || w.hardwareWrites != 1 || r.result != SERVICE_LIFECYCLE_RESULT_APPLIED)
        return fail(7225, "the next attempt against the current target writes once");
    return 0;
}

// The proof stopped holding between queueing and the attempt.
int proof_lost_while_waiting() {
    World w;
    fresh_process(w, 60000, kNewerDriverLs, kOlderDriverLs);
    arm(w);
    w.proofValid = false;
    w.gpuReady = true;
    if (attempt(w).step != SERVICE_DRIVER_RESTORE_PROOF_REFUSED ||
        w.lockout != SERVICE_AUTO_RESTORE_LOCKOUT_UNSTABLE_APPLY || !w.lifecycle.lockedOut ||
        w.hardwareWrites != 0 || !runtime_balanced(w))
        return fail(7226, "a proof lost while waiting latches the lockout without a write");
    return 0;
}

// Pending restore work in a process that did not validate a controlled
// recovery (the stale-DLL process after a removal) never writes.
int not_validated_process() {
    World w;
    fresh_process(w, 1000941, kNewerDriverLs, kOlderDriverLs);
    w.validated = false;
    w.gpuReady = true;
    if (arm(w) != SERVICE_DRIVER_ARM_NOT_VALIDATED || w.hasIntent || traced(w, "reduce"))
        return fail(7227, "an ordinary start arms nothing");
    ServiceLifecycleEvent removal = {};
    removal.type = SERVICE_LIFECYCLE_EVENT_DRIVER_RECOVERY;
    service_lifecycle_reduce(&w.lifecycle, &removal);
    if (attempt(w).step != SERVICE_DRIVER_RESTORE_NOT_VALIDATED || w.hardwareWrites != 0 ||
        !runtime_balanced(w))
        return fail(7227, "an unvalidated process never writes, and releases the runtime");
    return 0;
}

int update_install_and_lost_intent() {
    World w;
    fresh_process(w, 1000941, kNewerDriverLs, kOlderDriverLs);
    w.gpuReady = true;
    arm(w);
    w.updateInstallPending = true;
    if (attempt(w).step != SERVICE_DRIVER_RESTORE_UPDATE_BLOCKED || w.hardwareWrites != 0 ||
        !w.lifecycle.driverPending || w.lockDepth != 0)
        return fail(7228, "an update install keeps the restore pending and the runtime released");
    w.updateInstallPending = false;
    w.hasIntent = false;
    if (attempt(w).step != SERVICE_DRIVER_RESTORE_INTENT_LOST ||
        w.lockout != SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED || w.hardwareWrites != 0 ||
        !runtime_balanced(w))
        return fail(7229, "intent lost before the write latches AUTOMATIC_APPLY_FAILED");
    return 0;
}

}  // namespace

int run_driver_recovery_sequence_tests() {
    if (int r = old_process_to_fresh_start()) return r;
    if (int r = immature_proof_with_driver_update()) return r;
    if (int r = immature_proof_same_driver()) return r;
    if (int r = mature_proof()) return r;
    if (int r = spam_threshold()) return r;
    if (int r = failed_write()) return r;
    if (int r = target_changed_before_write()) return r;
    if (int r = proof_lost_while_waiting()) return r;
    if (int r = not_validated_process()) return r;
    if (int r = update_install_and_lost_intent()) return r;
    return 0;
}
