// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The service side of driver_recovery_flow.h. Arming a validated controlled
// recovery, queueing it, and the lifecycle worker's single restore write run
// the shared flow that tests/driver_recovery_sequence_tests.cpp scripts; this
// file only binds each step to this process's runtime and logs the outcome.
//
// Included after main_service_lifecycle_apply.cpp (the write-failure latch) and
// before the lifecycle worker that runs the restore.

#include "driver_recovery_flow.h"

static void service_forget_controlled_recovery() {
    g_serviceControlledRecoveryValidated = false;
    memset(&g_serviceControlledRecoveryDesired, 0,
        sizeof(g_serviceControlledRecoveryDesired));
    memset(&g_serviceControlledRecoveryTargetGpu, 0,
        sizeof(g_serviceControlledRecoveryTargetGpu));
    g_serviceControlledRecoveryProfileSource = SERVICE_PROFILE_SOURCE_NONE;
    g_serviceControlledRecoveryProfileSlot = 0;
}

struct ServiceDriverRecoveryRuntime {
    // Prepared by prepare_write() for the one write.
    DesiredSettings desired;
    ServiceSelectedGpuWriteEpoch gpuEpoch;
    ServiceOcApplyProofStamp matureProof;

    bool proof_allows(uint32_t* lockoutReason, uint64_t* proofAgeMs) {
        DWORD reason = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
        ULONGLONG ageMs = 0;
        const bool allowed = service_auto_restore_allowed_after_driver_event(&reason, &ageMs);
        *lockoutReason = reason;
        *proofAgeMs = ageMs;
        return allowed;
    }
    void disable_automatic_restore(uint32_t reason, const char* context) {
        service_disable_automatic_restore(reason, context);
    }
    ServiceLifecycleDecision reduce(const ServiceLifecycleEvent& event) {
        EnterCriticalSection(&g_appLock);
        const ServiceLifecycleDecision decision = service_lifecycle_reduce_locked(&event);
        LeaveCriticalSection(&g_appLock);
        return decision;
    }
    unsigned recent_recoveries() { return service_count_recent_restarts(); }
    void note_queued(const ServiceDriverRecoveryQueueReport& queue) {
        debug_log("lifecycle driver recovery: validated proofAgeMs=%llu ledger=%u/%u wake=%d\n",
            (unsigned long long)queue.proofAgeMs, queue.recentRecoveries,
            (unsigned int)SERVICE_RESTART_LOOP_THRESHOLD, queue.wake ? 1 : 0);
    }
    void signal_worker() { service_lifecycle_signal(); }
    bool controlled_recovery_validated() { return g_serviceControlledRecoveryValidated; }
    void lock_runtime() { lock_service_runtime(); }
    void unlock_runtime() { unlock_service_runtime(); }
    void publish_provisional_intent() {
        g_serviceActiveDesired = g_serviceControlledRecoveryDesired;
        g_serviceActiveDesiredGpu = g_serviceControlledRecoveryTargetGpu;
        g_serviceHasActiveDesired = true;
        InterlockedExchange(&g_serviceReapplyInProgress, 1);
        // Ownership is published only after the hardware reapply succeeds.
        g_serviceActiveProfileSource = SERVICE_PROFILE_SOURCE_NONE;
        g_serviceActiveProfileSlot = 0;
    }
    bool auto_restore_locked_out() {
        DWORD reason = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
        return service_auto_restore_is_locked_out(&reason);
    }
    void drop_provisional_intent(const char* reason) {
        service_drop_provisional_recovery_intent(reason);
    }
    void discard_controlled_recovery() { service_forget_controlled_recovery(); }
    bool driver_restore_pending() {
        EnterCriticalSection(&g_appLock);
        const bool pending = g_serviceLifecycleState.driverPending &&
            !g_serviceLifecycleState.driverWriteIssued &&
            !g_serviceLifecycleState.lockedOut;
        LeaveCriticalSection(&g_appLock);
        return pending;
    }
    bool update_install_takes_runtime() { return service_update_install_release_and_block(); }
    bool has_owned_intent() {
        return g_serviceHasActiveDesired && g_serviceActiveDesiredGpu.valid;
    }
    bool gpu_ready(char* detail, size_t detailSize) {
        bool ready = hardware_initialize(detail, detailSize) && !g_app.deviceRemoved;
        if (ready) {
            ready = service_select_restart_reapply_gpu(
                &g_serviceActiveDesiredGpu, detail, detailSize);
        }
        return ready;
    }
    void prepare_write() {
        service_build_full_restore_request(&g_serviceActiveDesired, &desired);
        GpuAdapterInfo driverTarget = g_app.selectedGpu.valid
            ? g_app.selectedGpu : g_serviceActiveDesiredGpu;
        service_refresh_selected_gpu_notification_best_effort(
            &driverTarget, "driver recovery pre-write target");
        gpuEpoch = service_selected_gpu_capture_write_epoch();
    }
    bool capture_mature_proof(uint64_t* ageMs) {
        ULONGLONG age = 0;
        const bool preserve = service_capture_mature_oc_apply_proof(&matureProof, &age);
        *ageMs = age;
        return preserve;
    }
    bool write_target_current() {
        return service_selected_gpu_write_epoch_is_current(gpuEpoch);
    }
    bool apply_full_intent(char* detail, size_t detailSize, bool* writeAttempted) {
        return service_apply_desired_settings(&desired, false,
            detail, detailSize, writeAttempted, true);
    }
    void refresh_target_after_success() {
        service_refresh_selected_gpu_notification_best_effort(
            &g_serviceActiveDesiredGpu, "successful driver recovery target");
    }
    bool commit_proof(bool preserveMature) {
        return preserveMature ? service_restore_mature_oc_apply_proof(matureProof)
                              : service_record_oc_apply_stamp();
    }
    void publish_recovered_profile() {
        g_serviceActiveProfileSource = g_serviceControlledRecoveryProfileSource;
        g_serviceActiveProfileSlot = g_serviceControlledRecoveryProfileSlot;
    }
    void write_restart_snapshot() { service_write_restart_reapply_snapshot(); }
    void retire_controlled_recovery() {
        service_forget_controlled_recovery();
        InterlockedExchange(&g_serviceReapplyInProgress, 0);
    }
    void latch_write_failure(const char* detail) {
        service_lifecycle_latch_write_failure(SERVICE_LIFECYCLE_TRIGGER_DRIVER_RECOVERY,
            detail, "controlled driver-recovery hardware write failed");
    }
    ServiceLifecycleResult finish_write(const ServiceLifecycleEvent& event) {
        EnterCriticalSection(&g_appLock);
        const ServiceLifecycleDecision decision = service_lifecycle_reduce_locked(&event);
        service_lifecycle_set_result_locked(
            SERVICE_LIFECYCLE_TRIGGER_DRIVER_RECOVERY, decision.result);
        LeaveCriticalSection(&g_appLock);
        return decision.result;
    }
    void note_finished(const ServiceDriverRestoreReport& report, const char* detail) {
        debug_log("lifecycle driver recovery: full-intent write attempted=%d success=%d proof=%s priorAgeMs=%llu driver=%s detail=%s\n",
            report.writeAttempted ? 1 : 0, report.success ? 1 : 0,
            !report.success ? "not-committed" : !report.proofCommitted
                ? "commit-failed-lockout" : report.preserveMatureProof
                ? "preserved-mature" : "restarted",
            (unsigned long long)report.matureProofAgeMs, service_loaded_driver_label().text,
            detail && detail[0] ? detail : "none");
    }
};

static void service_arm_validated_controlled_recovery() {
    ServiceDriverRecoveryRuntime runtime = {};
    service_driver_recovery_arm(runtime, SERVICE_RESTART_LOOP_THRESHOLD);
}

static void service_lifecycle_attempt_driver_restore() {
    ServiceDriverRecoveryRuntime runtime = {};
    char detail[512] = {};
    const ServiceDriverRestoreReport report =
        service_driver_restore_attempt(runtime, detail, sizeof(detail));
    if (report.step == SERVICE_DRIVER_RESTORE_NOT_VALIDATED) {
        debug_log("lifecycle driver recovery: old/non-validated or explicitly superseded process will not write\n");
    } else if (report.step == SERVICE_DRIVER_RESTORE_NOT_READY) {
        debug_log("lifecycle driver recovery: GPU prerequisite not ready; waiting for a real PnP/readiness signal: %s\n",
            detail[0] ? detail : "unknown");
    }
}
