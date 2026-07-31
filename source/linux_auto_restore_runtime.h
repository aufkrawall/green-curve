// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_auto_restore_runtime.h -- the one code path that performs an unattended
// hardware write.
//
// Three things used to write the GPU without a user asking: the `restore-last`
// boot replay, the `profile N` boot apply, and (as of this file) the
// standby-resume restore.  The first two each built their own request straight
// from the persisted DesiredSettings, which meant neither of them set
// resetOcBeforeApply -- service_merge_desired_after_mutation() strips that flag
// before the intent is persisted, because it is a one-shot transaction
// instruction and not durable state.  So every automatic Linux write layered a
// curve on top of whatever the driver already had, which is precisely the
// "apply on top of apply" case Windows resets the baseline to avoid.
//
// Routing all three through service_build_full_restore_request() -- the same
// pure builder the Windows standby and driver-recovery paths use -- fixes that
// at the source and keeps the rule in one place.
//
// Included by linux_daemon.cpp after store_daemon_record(), the guard helpers,
// and the backend.  It is addressed as part of that surface by the source
// guards, like linux_daemon_serve.h.

#ifndef GREEN_CURVE_LINUX_AUTO_RESTORE_RUNTIME_H
#define GREEN_CURVE_LINUX_AUTO_RESTORE_RUNTIME_H

struct LinuxAutoRestoreOutcome {
    bool authorized;  // the guard allowed it
    bool attempted;   // a hardware write was actually issued
    bool success;
    char message[256];
};

static bool persist_auto_restore_guard(const char* why) {
    char err[256] = {};
    bool ok = linux_daemon_guard_store(GC_DAEMON_GUARD_FILE,
                                       &g_autoRestoreGuard, err, sizeof(err));
    dlog("daemon auto-restore: guard persisted (%s) lockedOut=%d reason=%u (%s) "
         "startAttempts=%u boot=%s ok=%d%s%s\n",
         why ? why : "-", (int)g_autoRestoreGuard.lockedOut,
         (unsigned int)g_autoRestoreGuard.lockoutReason,
         linux_auto_restore_lockout_reason_name(g_autoRestoreGuard.lockoutReason),
         (unsigned int)g_autoRestoreGuard.startAttempts,
         g_autoRestoreGuard.bootId[0] ? g_autoRestoreGuard.bootId : "<unknown>",
         ok ? 1 : 0, ok ? "" : ": ", ok ? "" : (err[0] ? err : "unknown error"));
    return ok;
}

// The single gate every unattended hardware write passes through.  It records
// and PERSISTS the attempt before returning true, because a crash during the
// write must still count: an attempt remembered only on the way out counts
// nothing at all in exactly the loop this exists to break.  A guard that cannot
// be committed refuses the write, mirroring the Windows rule that an
// uncommittable proof invalidation aborts before touching the GPU.
static bool auto_restore_authorize(LinuxAutoRestoreTrigger trigger,
                                   LinuxAutoRestoreVerdict* verdictOut) {
    LinuxAutoRestoreVerdict verdict =
        linux_auto_restore_decide(&g_autoRestoreGuard, trigger);
    // Reported to the caller rather than recomputed by it: latching the lockout
    // below changes what a second call would answer, so "attempts exhausted"
    // would come back as the less specific "locked out".
    if (verdictOut) *verdictOut = verdict;
    if (verdict != LINUX_AUTO_RESTORE_ALLOW) {
        if (verdict == LINUX_AUTO_RESTORE_DENY_ATTEMPTS_EXHAUSTED &&
            !g_autoRestoreGuard.lockedOut) {
            // Latch it, so the next boot does not hand the same crash three
            // fresh attempts.  Only an explicit Apply or Reset clears this.
            //
            // UNSTABLE_APPLY, not AUTOMATIC_APPLY_FAILED: nothing was written
            // here.  What this arm knows is that replaying the stored settings
            // did not survive repeated starts, which is the same statement
            // Windows makes when an apply fails its proving period.
            linux_auto_restore_note_lockout(&g_autoRestoreGuard,
                SERVICE_AUTO_RESTORE_LOCKOUT_UNSTABLE_APPLY);
            persist_auto_restore_guard("attempts exhausted");
        }
        dlog("daemon auto-restore: %s %s (startAttempts=%u lockedOut=%d)\n",
             linux_auto_restore_trigger_name(trigger),
             linux_auto_restore_verdict_name(verdict),
             (unsigned int)g_autoRestoreGuard.startAttempts,
             (int)g_autoRestoreGuard.lockedOut);
        return false;
    }
    if (!linux_auto_restore_trigger_is_start(trigger)) {
        dlog("daemon auto-restore: %s allowed (not a start-time write)\n",
             linux_auto_restore_trigger_name(trigger));
        return true;
    }
    LinuxAutoRestoreGuard previous = g_autoRestoreGuard;
    linux_auto_restore_note_start_attempt(&g_autoRestoreGuard);
    if (!persist_auto_restore_guard("start attempt")) {
        g_autoRestoreGuard = previous;
        dlog("daemon auto-restore: %s refused; the attempt could not be recorded, "
             "so the write is not attempted\n",
             linux_auto_restore_trigger_name(trigger));
        return false;
    }
    dlog("daemon auto-restore: %s allowed (attempt %u of %d this boot)\n",
         linux_auto_restore_trigger_name(trigger),
         (unsigned int)g_autoRestoreGuard.startAttempts,
         (int)LINUX_AUTO_RESTORE_MAX_START_ATTEMPTS);
    return true;
}

// Called after a successful EXPLICIT Apply or Reset only.  Automatic success
// deliberately does not re-arm: on Windows that is what stops a lockout from
// being cleared by the very replay that caused it.
static void auto_restore_note_explicit_success(const char* origin) {
    if (!linux_auto_restore_note_explicit_success(&g_autoRestoreGuard)) return;
    dlog("daemon auto-restore: explicit %s succeeded; automatic restoration re-armed\n",
         origin ? origin : "operation");
    persist_auto_restore_guard("explicit success");
}

// Read the guard once per daemon start, before anything can write.
//
// A record that is present but unreadable fails CLOSED to locked out, matching
// the Windows rule that an unreadable lockout fallback is treated as locked
// out.  The alternative -- treating a damaged counter as "no attempts yet" --
// would hand a crash loop unlimited retries precisely when the filesystem is
// already misbehaving.  An ABSENT record is the ordinary first-run state and
// means nothing has gone wrong yet.
static void load_auto_restore_guard_at_boot() {
    char bootId[LINUX_BOOT_ID_MAX] = {};
    bool haveBootId = linux_read_boot_id(bootId, sizeof(bootId));
    bool corrupt = false;
    char err[256] = {};
    if (!linux_daemon_guard_load(GC_DAEMON_GUARD_FILE, &g_autoRestoreGuard,
                                 &corrupt, err, sizeof(err))) {
        memset(&g_autoRestoreGuard, 0, sizeof(g_autoRestoreGuard));
        if (corrupt) {
            // Latched through the policy helper rather than by assigning the
            // flag, so the fail-closed state is as coherent as any other and
            // publishes a reason instead of an unexplained lockout.  This is
            // also the version-1 upgrade path: an older record fails the
            // validator and arrives here.
            linux_auto_restore_note_lockout(&g_autoRestoreGuard,
                SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED);
            dlog("daemon auto-restore: guard unreadable (%s); automatic "
                 "restoration is locked out until an explicit Apply or Reset\n",
                 err[0] ? err : "invalid record");
        } else {
            dlog("daemon auto-restore: no guard on disk; starting a fresh boot\n");
        }
    }
    if (!haveBootId) {
        // Without a boot identity a per-boot counter cannot be trusted not to
        // be a cross-boot one.  Keep counting rather than resetting: over-
        // counting stops an unattended write, under-counting authorizes one.
        dlog("daemon auto-restore: boot id unavailable; the per-boot attempt "
             "counter carries over (startAttempts=%u lockedOut=%d)\n",
             (unsigned int)g_autoRestoreGuard.startAttempts,
             (int)g_autoRestoreGuard.lockedOut);
        return;
    }
    bool sameBoot = linux_auto_restore_guard_adopt_boot(&g_autoRestoreGuard,
                                                        bootId);
    dlog("daemon auto-restore: guard loaded boot=%s (%s) startAttempts=%u "
         "lockedOut=%d reason=%u (%s) maxStartAttempts=%d\n",
         bootId, sameBoot ? "same boot" : "new boot",
         (unsigned int)g_autoRestoreGuard.startAttempts,
         (int)g_autoRestoreGuard.lockedOut,
         (unsigned int)g_autoRestoreGuard.lockoutReason,
         linux_auto_restore_lockout_reason_name(g_autoRestoreGuard.lockoutReason),
         (int)LINUX_AUTO_RESTORE_MAX_START_ATTEMPTS);
}

// Reaching the ordinary teardown proves this process ran long enough to be
// asked to stop, so the starts that led here were not a crash loop.  Called
// from the shutdown path only -- a crash never gets here, which is the point.
static void clear_auto_restore_attempts_on_clean_stop() {
    if (!linux_auto_restore_note_clean_stop(&g_autoRestoreGuard)) return;
    dlog("daemon auto-restore: clean stop; per-boot attempt counter cleared "
         "(lockedOut=%d is unchanged and needs an explicit Apply or Reset)\n",
         (int)g_autoRestoreGuard.lockedOut);
    persist_auto_restore_guard("clean stop");
}

// A failed unattended write is terminal for that event and latches automatic
// restoration off, whatever its origin -- the Windows rule, verbatim.  The
// difference between "we could not start" and "we wrote and it went wrong" is
// exactly the difference between retrying being safe and being reckless.
static void auto_restore_note_automatic_failure(LinuxAutoRestoreTrigger trigger) {
    if (g_autoRestoreGuard.lockedOut) return;
    linux_auto_restore_note_lockout(&g_autoRestoreGuard,
        SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED);
    dlog("daemon auto-restore: %s failed at the hardware; automatic restoration "
         "is latched off until an explicit Apply or Reset succeeds\n",
         linux_auto_restore_trigger_name(trigger));
    persist_auto_restore_guard("automatic write failed");
}

// Bring the backend back to a usable state for `target` without sleeping or
// polling.  The resume edge IS the readiness signal (greencurve-resume.service
// is ordered after the driver's own resume unit); if the driver still is not
// there, this fails and the caller refuses to write rather than guessing.
static bool auto_restore_prepare_backend(const GpuAdapterInfo* target,
                                         char* err, size_t errSize) {
    if (!g_gpuReady) {
        // One re-init attempt: across a suspend the NVIDIA character devices
        // are torn down and rebuilt, so a daemon that started before the driver
        // was usable can legitimately become usable here.
        //
        // Into a replacement, not over g_gpu, and using the same idiom as
        // linux_backend_select_target(): linux_backend_init() memsets the state
        // it is handed but can RETURN failure with the NVML/NvAPI handles
        // already open (the "nvmlInit_v2 failed" arm does exactly that), so
        // re-initializing in place would drop the previous attempt's dlopen
        // handles on the floor.
        LinuxGpuState replacement = {};
        if (!linux_backend_init(&replacement, nullptr, err, errSize)) {
            linux_backend_shutdown(&replacement);
            return false;
        }
        linux_backend_shutdown(&g_gpu);
        g_gpu = replacement;
        g_gpuReady = true;
        dlog("daemon auto-restore: GPU backend re-initialized (%s health=%s)\n",
             g_gpu.gpuName,
             service_gpu_health_reason_name(g_gpu.health.reason));
    }
    if (!linux_backend_select_target(&g_gpu, target, err, errSize)) return false;
    // Same-PCI rebind plus a fresh VF snapshot; linux_backend_refresh() already
    // performs exactly one read-only re-enumeration when a refresh fails.
    linux_backend_refresh(&g_gpu);
    return true;
}

// The single unattended-write entry point.  Callers own *deciding* that an
// automatic restore is wanted; this owns whether it is allowed, what request it
// becomes, and what the failure means.
static LinuxAutoRestoreOutcome daemon_automatic_restore_write(
    LinuxAutoRestoreTrigger trigger, const GpuAdapterInfo* target,
    const DesiredSettings* intent) {
    LinuxAutoRestoreOutcome outcome = {};
    if (!target || !intent) {
        gc_strlcpy(outcome.message, sizeof(outcome.message),
                   "no intent to restore");
        return outcome;
    }
    LinuxAutoRestoreVerdict verdict = LINUX_AUTO_RESTORE_ALLOW;
    if (!auto_restore_authorize(trigger, &verdict)) {
        gc_snprintf(outcome.message, sizeof(outcome.message),
            "%s %s", linux_auto_restore_trigger_name(trigger),
            linux_auto_restore_verdict_name(verdict));
        return outcome;
    }
    outcome.authorized = true;

    char err[256] = {};
    if (!auto_restore_prepare_backend(target, err, sizeof(err))) {
        // Nothing was written, so this is a missing prerequisite rather than a
        // failed write: it does not latch the lockout.  The attempt it consumed
        // is still spent, which is deliberate -- a daemon that cannot reach its
        // GPU on three consecutive starts must stop trying by itself.
        gc_snprintf(outcome.message, sizeof(outcome.message),
                    "GPU not available for automatic restore: %s", err);
        dlog("daemon auto-restore: %s aborted before any write: %s\n",
             linux_auto_restore_trigger_name(trigger), err);
        g_stateUncertain = true;
        return outcome;
    }

    DesiredSettings committed = *intent;
    LockMode storedLockMode = committed.lockMode;
    committed.lockMode = profile_lock_mode_after_load(committed.hasLock, true,
                                                      committed.lockMode);
    if (storedLockMode != committed.lockMode) {
        dlog("daemon auto-restore: migrated persisted enabled lock mode %d to %d\n",
             (int)storedLockMode, (int)committed.lockMode);
    }
    validate_desired_settings_for_ipc(&committed);

    // The request is the intent PLUS the reset-to-stock instruction, exactly as
    // the Windows standby/driver-recovery restore builds it.  `committed` stays
    // the ownership declaration; only the request carries the transaction flag.
    DesiredSettings request = {};
    if (!service_build_full_restore_request(&committed, &request)) {
        gc_strlcpy(outcome.message, sizeof(outcome.message),
                   "could not build the restore request");
        return outcome;
    }
    dlog("daemon auto-restore: %s writing intent [resetBaseline=%d curvePoints=%d "
         "gpuOffset=%d memOffset=%d powerPct=%d lock=%d lockMode=%d fanMode=%d fanPct=%d]\n",
         linux_auto_restore_trigger_name(trigger),
         request.resetOcBeforeApply ? 1 : 0,
         service_desired_curve_point_count(&request),
         request.hasGpuOffset ? request.gpuOffsetMHz : 0,
         request.hasMemOffset ? request.memOffsetMHz : 0,
         request.hasPowerLimit ? request.powerLimitPct : 0,
         request.hasLock ? 1 : 0, (int)request.lockMode,
         request.hasFan ? (int)request.fanMode : -1,
         request.hasFan ? request.fanPercent : 0);

    outcome.attempted = true;
    LinuxMutationResult mutation = linux_backend_apply(&g_gpu, &request,
        &committed, &committed, outcome.message, sizeof(outcome.message));
    if (!mutation.success) {
        outcome.success = false;
        g_stateUncertain = true;
        char stateErr[256] = {};
        store_daemon_record(LINUX_DAEMON_RECORD_UNCERTAIN, target, &committed,
                            stateErr, sizeof(stateErr));
        auto_restore_note_automatic_failure(trigger);
        dlog("daemon auto-restore: %s failed -> %s (rollback=%d anyWrite=%d)\n",
             linux_auto_restore_trigger_name(trigger), outcome.message,
             mutation.rollbackSucceeded ? 1 : 0, mutation.anyWrite ? 1 : 0);
        return outcome;
    }

    outcome.success = true;
    g_activeDesired = committed;
    g_activeTarget = g_gpu.selectedGpu;
    g_hasActiveDesired = true;
    g_fanFailureCount = 0;
    // Re-committed rather than assumed: a lock-mode migration above, or a v1
    // record read back through the compatibility path, would otherwise leave
    // the file disagreeing with what is now on the hardware.
    char stateErr[256] = {};
    if (!store_daemon_record(LINUX_DAEMON_RECORD_ACTIVE, &g_activeTarget,
                             &g_activeDesired, stateErr, sizeof(stateErr))) {
        g_stateUncertain = true;
        dlog("daemon auto-restore: %s applied but could not be recorded: %s\n",
             linux_auto_restore_trigger_name(trigger),
             stateErr[0] ? stateErr : "unknown error");
    }
    wake_fan_runtime();
    dlog("daemon auto-restore: %s committed -> %s\n",
         linux_auto_restore_trigger_name(trigger), outcome.message);
    return outcome;
}

// SERVICE_CMD_RESUME_RESTORE.  The Linux counterpart of the Windows
// PBT_APMRESUME* path: restore the complete in-memory intent exactly once,
// with no proof gate, using nothing the caller supplied.
static void daemon_handle_resume_restore(ServiceResponse* resp) {
    if (!resp) return;
    if (!g_hasActiveDesired) {
        // Not an error: a machine with no Green Curve settings applied has
        // nothing to lose across a suspend, and saying so keeps the resume unit
        // from reporting a failure on every wake.
        resp->status = SERVICE_STATUS_OK;
        gc_strlcpy(resp->message, sizeof(resp->message),
            "no active Green Curve intent; nothing to restore after resume");
        dlog("daemon resume: no active intent; nothing to restore\n");
        populate_snapshot(&resp->snapshot, &resp->controlState);
        return;
    }
    dlog("daemon resume: restoring the complete active intent after suspend\n");
    LinuxAutoRestoreOutcome outcome = daemon_automatic_restore_write(
        LINUX_AUTO_RESTORE_TRIGGER_RESUME, &g_activeTarget, &g_activeDesired);
    resp->status = outcome.success ? SERVICE_STATUS_OK : SERVICE_STATUS_ERROR;
    gc_strlcpy(resp->message, sizeof(resp->message),
               outcome.message[0] ? outcome.message : "resume restore failed");
    populate_snapshot(&resp->snapshot, &resp->controlState);
    if (g_hasActiveDesired) resp->desired = g_activeDesired;
    if (g_stateUncertain) wake_fan_runtime();
}

#endif // GREEN_CURVE_LINUX_AUTO_RESTORE_RUNTIME_H
