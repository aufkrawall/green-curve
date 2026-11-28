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

// Monotonic milliseconds, truncated to gc_u32 so the resume window's arithmetic
// is a plain wrapsafe subtraction.  linux_daemon_transport.cpp keeps its own
// static copy for the transport deadlines; this one is scoped to the guard.
static inline gc_u32 gc_auto_restore_now_ms(void) {
    struct timespec ts = {};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (gc_u32)((unsigned long long)ts.tv_sec * 1000ULL +
                    (unsigned long long)(ts.tv_nsec / 1000000ULL));
}

struct LinuxAutoRestoreOutcome {
    bool authorized;  // the guard allowed it
    bool attempted;   // a hardware write was actually issued
    bool success;
    char message[256];
};

static bool persist_auto_restore_guard(LinuxGpuController& ctl, const char* why,
                                       char* errorOut = nullptr,
                                       size_t errorSize = 0) {
    char err[256] = {};
    bool ok = linux_daemon_guard_store(GC_DAEMON_GUARD_FILE,
                                       &ctl.autoRestoreGuard, err, sizeof(err));
    if (errorOut && errorSize) {
        gc_strlcpy(errorOut, errorSize,
                   ok ? "" : (err[0] ? err : "unknown error"));
    }
    dlog("daemon auto-restore: guard persisted (%s) lockedOut=%d reason=%u (%s) "
         "startAttempts=%u boot=%s ok=%d%s%s\n",
         why ? why : "-", (int)ctl.autoRestoreGuard.lockedOut,
         (unsigned int)ctl.autoRestoreGuard.lockoutReason,
         linux_auto_restore_lockout_reason_name(ctl.autoRestoreGuard.lockoutReason),
         (unsigned int)ctl.autoRestoreGuard.startAttempts,
         ctl.autoRestoreGuard.bootId[0] ? ctl.autoRestoreGuard.bootId : "<unknown>",
         ok ? 1 : 0, ok ? "" : ": ", ok ? "" : (err[0] ? err : "unknown error"));
    return ok;
}

// The single gate every unattended hardware write passes through.  It records
// and PERSISTS the attempt before returning true, because a crash during the
// write must still count: an attempt remembered only on the way out counts
// nothing at all in exactly the loop this exists to break.  A guard that cannot
// be committed refuses the write, mirroring the Windows rule that an
// uncommittable proof invalidation aborts before touching the GPU.
static bool auto_restore_authorize(LinuxGpuController& ctl, LinuxAutoRestoreTrigger trigger,
                                   bool stateUncertain, LinuxAutoRestoreVerdict* verdictOut) {
    gc_u32 nowMs = gc_auto_restore_now_ms();
    // Count the resume BEFORE deciding, so the very first request in a window
    // already consumes one and the budget cannot be skipped by asking.  In
    // memory only: unlike the per-boot counter this is not persisted, because a
    // one-minute window has no meaning across a daemon restart.
    if (!linux_auto_restore_trigger_is_start(trigger)) {
        linux_auto_restore_note_resume_attempt(&ctl.autoRestoreGuard, nowMs);
    }
    LinuxAutoRestoreVerdict verdict =
        linux_auto_restore_decide(&ctl.autoRestoreGuard, trigger,
                                  stateUncertain, nowMs);
    // Reported to the caller rather than recomputed by it: latching the lockout
    // below changes what a second call would answer, so "attempts exhausted"
    // would come back as the less specific "locked out".
    if (verdictOut) *verdictOut = verdict;
    if (verdict != LINUX_AUTO_RESTORE_ALLOW) {
        if (verdict == LINUX_AUTO_RESTORE_DENY_ATTEMPTS_EXHAUSTED &&
            !ctl.autoRestoreGuard.lockedOut) {
            // Latch it, so the next boot does not hand the same crash three
            // fresh attempts.  Only an explicit Apply or Reset clears this.
            //
            // UNSTABLE_APPLY, not AUTOMATIC_APPLY_FAILED: nothing was written
            // here.  What this arm knows is that replaying the stored settings
            // did not survive repeated starts, which is the same statement
            // Windows makes when an apply fails its proving period.
            linux_auto_restore_note_lockout(&ctl.autoRestoreGuard,
                SERVICE_AUTO_RESTORE_LOCKOUT_UNSTABLE_APPLY);
            persist_auto_restore_guard(ctl, "attempts exhausted");
        }
        dlog("daemon auto-restore: %s %s (startAttempts=%u lockedOut=%d)\n",
             linux_auto_restore_trigger_name(trigger),
             linux_auto_restore_verdict_name(verdict),
             (unsigned int)ctl.autoRestoreGuard.startAttempts,
             (int)ctl.autoRestoreGuard.lockedOut);
        return false;
    }
    if (!linux_auto_restore_trigger_is_start(trigger)) {
        dlog("daemon auto-restore: %s allowed (not a start-time write)\n",
             linux_auto_restore_trigger_name(trigger));
        return true;
    }
    LinuxAutoRestoreGuard previous = ctl.autoRestoreGuard;
    linux_auto_restore_note_start_attempt(&ctl.autoRestoreGuard);
    if (!persist_auto_restore_guard(ctl, "start attempt")) {
        ctl.autoRestoreGuard = previous;
        dlog("daemon auto-restore: %s refused; the attempt could not be recorded, "
             "so the write is not attempted\n",
             linux_auto_restore_trigger_name(trigger));
        return false;
    }
    dlog("daemon auto-restore: %s allowed (attempt %u of %d this boot)\n",
         linux_auto_restore_trigger_name(trigger),
         (unsigned int)ctl.autoRestoreGuard.startAttempts,
         (int)LINUX_AUTO_RESTORE_MAX_START_ATTEMPTS);
    return true;
}

// Called after a successful EXPLICIT Apply or Reset only.  Automatic success
// deliberately does not re-arm: on Windows that is what stops a lockout from
// being cleared by the very replay that caused it.  The in-memory transition
// is transactional with its record: otherwise the current response/snapshot
// would claim re-armed while the old lockout returns on daemon restart.
static bool auto_restore_note_explicit_success(LinuxGpuController& ctl, const char* origin,
                                               char* errorOut,
                                               size_t errorSize) {
    if (errorOut && errorSize) errorOut[0] = 0;
    if (ctl.fanOwnershipHandbackPending) {
        gc_strlcpy(errorOut, errorSize,
            "fan recovery is still pending; apply a fan setting or Reset the owned GPU");
        dlog("daemon auto-restore: explicit %s left fan recovery pending; retaining the previous guard\n",
             origin ? origin : "operation");
        return false;
    }
    LinuxAutoRestoreGuard previous = ctl.autoRestoreGuard;
    if (!linux_auto_restore_note_explicit_success(&ctl.autoRestoreGuard)) return true;
    if (!persist_auto_restore_guard(ctl, "explicit success", errorOut, errorSize)) {
        ctl.autoRestoreGuard = previous;
        dlog("daemon auto-restore: explicit %s committed, but automatic "
             "restoration could not be re-armed durably; previous guard "
             "restored in memory (%s)\n",
             origin ? origin : "operation",
             errorOut && errorOut[0] ? errorOut : "unknown error");
        return false;
    }
    ctl.autoApplyFailure = ServiceAutoApplyFailure{};
    dlog("daemon auto-restore: explicit %s succeeded; automatic restoration "
         "re-armed durably\n",
         origin ? origin : "operation");
    return true;
}

// Read the guard once per daemon start, before anything can write.
//
// A record that is present but unreadable fails CLOSED to locked out, matching
// the Windows rule that an unreadable lockout fallback is treated as locked
// out.  The alternative -- treating a damaged counter as "no attempts yet" --
// would hand a crash loop unlimited retries precisely when the filesystem is
// already misbehaving.  An ABSENT record is the ordinary first-run state and
// means nothing has gone wrong yet.
static void load_auto_restore_guard_at_boot(LinuxGpuController& ctl) {
    char bootId[LINUX_BOOT_ID_MAX] = {};
    bool haveBootId = linux_read_boot_id(bootId, sizeof(bootId));
    bool corrupt = false;
    char err[256] = {};
    if (!linux_daemon_guard_load(GC_DAEMON_GUARD_FILE, &ctl.autoRestoreGuard,
                                 &corrupt, err, sizeof(err))) {
        memset(&ctl.autoRestoreGuard, 0, sizeof(ctl.autoRestoreGuard));
        if (corrupt) {
            // Latched through the policy helper rather than by assigning the
            // flag, so the fail-closed state is as coherent as any other and
            // publishes a reason instead of an unexplained lockout.  This is
            // also the version-1 upgrade path: an older record fails the
            // validator and arrives here.
            linux_auto_restore_note_lockout(&ctl.autoRestoreGuard,
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
             (unsigned int)ctl.autoRestoreGuard.startAttempts,
             (int)ctl.autoRestoreGuard.lockedOut);
        return;
    }
    bool sameBoot = linux_auto_restore_guard_adopt_boot(&ctl.autoRestoreGuard,
                                                        bootId);
    dlog("daemon auto-restore: guard loaded boot=%s (%s) startAttempts=%u "
         "lockedOut=%d reason=%u (%s) maxStartAttempts=%d\n",
         bootId, sameBoot ? "same boot" : "new boot",
         (unsigned int)ctl.autoRestoreGuard.startAttempts,
         (int)ctl.autoRestoreGuard.lockedOut,
         (unsigned int)ctl.autoRestoreGuard.lockoutReason,
         linux_auto_restore_lockout_reason_name(ctl.autoRestoreGuard.lockoutReason),
         (int)LINUX_AUTO_RESTORE_MAX_START_ATTEMPTS);
}

// Reaching the ordinary teardown proves this process ran long enough to be
// asked to stop, so the starts that led here were not a crash loop.  Called
// from the shutdown path only -- a crash never gets here, which is the point.
static void clear_auto_restore_attempts_on_clean_stop(LinuxGpuController& ctl) {
    if (!linux_auto_restore_note_clean_stop(&ctl.autoRestoreGuard)) return;
    dlog("daemon auto-restore: clean stop; per-boot attempt counter cleared "
         "(lockedOut=%d is unchanged and needs an explicit Apply or Reset)\n",
         (int)ctl.autoRestoreGuard.lockedOut);
    persist_auto_restore_guard(ctl, "clean stop");
}

// A failed unattended write is terminal for that event and latches automatic
// restoration off, whatever its origin -- the Windows rule, verbatim.  The
// difference between "we could not start" and "we wrote and it went wrong" is
// exactly the difference between retrying being safe and being reckless.
//
// `detail` is the write's own failure text and `rollbackOk` whether returning
// to the pre-write state verified; both become the v29 cause record.  The
// record never claims stock clocks (coreRecovered stays 0): Linux rolls back to
// whatever was on the hardware before the write, not to defaults.
static void auto_restore_note_automatic_failure(LinuxGpuController& ctl, LinuxAutoRestoreTrigger trigger,
                                                const char* detail,
                                                bool rollbackOk) {
    if (ctl.autoRestoreGuard.lockedOut) return;
    ServiceAutoApplyFailure record = {};
    record.trigger = linux_auto_restore_trigger_is_start(trigger)
        ? SERVICE_LIFECYCLE_TRIGGER_DAEMON_START
        : SERVICE_LIFECYCLE_TRIGGER_STANDBY_RESUME;
    gc_snprintf(record.detail, sizeof(record.detail), "%s; rollback to the "
                "previous state %s", detail && detail[0] ? detail : "write failed",
                rollbackOk ? "verified" : "is uncertain");
    // Before the lockout, so no snapshot shows the lockout without its cause.
    ctl.autoApplyFailure = record;
    dlog("daemon auto-restore: failure recorded for clients trigger=%u "
         "rollbackOk=%d detailChars=%u\n", (unsigned int)record.trigger,
         rollbackOk ? 1 : 0, (unsigned int)strlen(record.detail));
    linux_auto_restore_note_lockout(&ctl.autoRestoreGuard,
        SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED);
    dlog("daemon auto-restore: %s failed after reaching hardware or its durable "
         "state did not commit; automatic restoration is latched off until an "
         "explicit Apply or Reset succeeds\n",
         linux_auto_restore_trigger_name(trigger));
    persist_auto_restore_guard(ctl, "automatic write failed");
}

// Bring the backend back to a usable state for `target` without sleeping or
// polling.  The resume edge IS the readiness signal (greencurve-resume.service
// is ordered after the driver's own resume unit); if the driver still is not
// there, this fails and the caller refuses to write rather than guessing.
static bool auto_restore_prepare_backend(LinuxGpuController& ctl, const GpuAdapterInfo* target,
                                         GpuLocationRole role,
                                         const LinuxGpuUuid* uuid,
                                         LinuxGpuUuidVerdict* uuidVerdict,
                                         char* err, size_t errSize) {
    if (!ctl.gpuReady) {
        // One re-init attempt: across a suspend the NVIDIA character devices
        // are torn down and rebuilt, so a daemon that started before the driver
        // was usable can legitimately become usable here.
        //
        // Into a replacement, not over ctl.gpu, and using the same idiom as
        // linux_backend_select_target(): linux_backend_init() memsets the state
        // it is handed, so a failed attempt must not overwrite the published
        // one. The libraries and NVML reference stay with g_driver.
        LinuxGpuState replacement = {};
        if (!linux_backend_init(&replacement, &g_driver, nullptr, err, errSize)) {
            linux_backend_shutdown(&replacement);
            return false;
        }
        linux_backend_preserve_same_gpu_log_state(&ctl.gpu, &replacement);
        linux_backend_shutdown(&ctl.gpu);
        ctl.gpu = replacement;
        ctl.gpuReady = true;
        // The target is selected below, after any deferred fan handback.
        daemon_fan_ownership_resume_deferred_handback(ctl);
        dlog("daemon auto-restore: GPU backend re-initialized (%s health=%s)\n",
             ctl.gpu.gpuName,
             service_gpu_health_reason_name(ctl.gpu.health.reason));
    }
    if (!linux_backend_select_target(&ctl.gpu, target, err, errSize, role, uuid, uuidVerdict)) return false;
    // Same-PCI rebind plus a fresh VF snapshot; linux_backend_refresh() already
    // performs exactly one read-only re-enumeration when a refresh fails.
    linux_backend_refresh(&ctl.gpu);
    return true;
}

// The uncertainty an unattended write is judged against.  Only the resume
// restore that replays a standby fan stash may pass the uncertainty that
// stash's lockout entered (linux_standby_fan_stash_policy.h); the decision is
// taken against the live state each time, so uncertainty entered after the
// lockout (backend preparation included) still refuses the write.
static bool auto_restore_state_uncertain(LinuxGpuController& ctl, bool standbyFanStashReplay) {
    if (!standbyFanStashReplay) return ctl.stateUncertain;
    const LinuxStandbyFanStashUse use = linux_standby_fan_stash_use(
        &ctl.standbyFanStash, ctl.uncertainSerial, ctl.stateUncertain,
        ctl.hasActiveDesired, &ctl.activeDesired);
    return linux_standby_fan_stash_effective_uncertain(ctl.stateUncertain, use);
}

// The single unattended-write entry point.  Callers own *deciding* that an
// automatic restore is wanted; this owns whether it is allowed, what request it
// becomes, and what the failure means.
static LinuxAutoRestoreOutcome daemon_automatic_restore_write(LinuxGpuController& ctl,
    LinuxAutoRestoreTrigger trigger, const GpuAdapterInfo* target,
    const DesiredSettings* intent, GpuLocationRole role, const LinuxGpuUuid* uuid,
    const LinuxDaemonStateRecord* previousRecord = nullptr,
    bool standbyFanStashReplay = false) {
    LinuxAutoRestoreOutcome outcome = {};
    const GpuLocationRole storedRole = role;  // the binding as saved, before selection refines it
    if (!target || !intent) {
        gc_strlcpy(outcome.message, sizeof(outcome.message),
                   "no intent to restore");
        return outcome;
    }
    if (standbyFanStashReplay && trigger != LINUX_AUTO_RESTORE_TRIGGER_RESUME)
        standbyFanStashReplay = false;
    const bool uncertainAtStart = auto_restore_state_uncertain(ctl, standbyFanStashReplay);
    if (standbyFanStashReplay)
        dlog("daemon auto-restore: %s replays a standby fan stash; stateUncertain=%d "
             "waived=%d serial=%llu\n", linux_auto_restore_trigger_name(trigger),
             ctl.stateUncertain ? 1 : 0,
             ctl.stateUncertain && !uncertainAtStart ? 1 : 0, ctl.uncertainSerial);
    LinuxAutoRestoreVerdict verdict = LINUX_AUTO_RESTORE_ALLOW;
    if (!auto_restore_authorize(ctl, trigger, uncertainAtStart, &verdict)) {
        gc_snprintf(outcome.message, sizeof(outcome.message),
            "%s %s", linux_auto_restore_trigger_name(trigger),
            linux_auto_restore_verdict_name(verdict));
        return outcome;
    }
    outcome.authorized = true;

    char err[256] = {};
    LinuxGpuUuidVerdict uuidVerdict = LINUX_GPU_UUID_FALLBACK;
    if (!auto_restore_prepare_backend(ctl, target, role, uuid, &uuidVerdict, err, sizeof(err))) {
        // Nothing was written, so this is a missing prerequisite rather than a
        // failed write: it does not latch the lockout.  The attempt it consumed
        // is still spent, which is deliberate -- a daemon that cannot reach its
        // GPU on three consecutive starts must stop trying by itself.
        // F-PREP-NO-UNCERTAIN: it must also NOT set ctl.stateUncertain.  That
        // flag says the daemon state or a rollback did not settle; a GPU that
        // was merely not ready disturbed no state, and setting it here disabled
        // the fan reassertion thread and published a lockout until an explicit
        // Apply/Reset -- for a failure that wrote nothing.  The next resume is
        // allowed to try again.
        gc_snprintf(outcome.message, sizeof(outcome.message),
                    "GPU not available for automatic restore: %s", err);
        // Judged against the adapters the backend enumerated; a failed
        // re-selection leaves ctl.gpu untouched.
        const bool targetUnresolved = uuidVerdict == LINUX_GPU_UUID_MISMATCH ||
            uuidVerdict == LINUX_GPU_UUID_AMBIGUOUS || (ctl.gpuReady &&
            linux_resolve_stored_gpu_target(target, role, ctl.gpu.adapters,
                                            ctl.gpu.adapterCount, nullptr) < 0);
        dlog("daemon auto-restore: %s aborted before any write: %s "
             "[stored=%04x:%02x:%02x.%u role=%s remedy=%s]\n",
             linux_auto_restore_trigger_name(trigger), err, target->pciDomain,
             target->pciBus, target->pciDevice, target->pciFunction,
             configured_gpu_location_role_name(role),
             linux_auto_restore_rebind_hint(trigger, targetUnresolved));
        return outcome;
    }

    if (role != GPU_LOCATION_ROLE_REQUIRED)
        role = linux_gpu_location_role_for_selection(&ctl.gpu.selectedGpu,
            ctl.gpu.adapters, ctl.gpu.adapterCount);
    const bool bootProfile = trigger == LINUX_AUTO_RESTORE_TRIGGER_BOOT_PROFILE;
    if (bootProfile && previousRecord) {
        const auto previousSelection = linux_gpu_restore_selection(&previousRecord->targetGpu,
            (GpuLocationRole)previousRecord->targetLocationRole, ctl.gpu.adapters, ctl.gpu.adapterCount,
            &previousRecord->targetUuid, ctl.gpu.adapterUuids, nullptr);
        if (previousSelection.index < 0 ||
            (unsigned int)previousSelection.index != ctl.gpu.selectedAdapterIndex) {
            gc_strlcpy(outcome.message, sizeof(outcome.message),
                "Reset the previous GPU before applying a startup profile to a different or unresolved GPU");
            return outcome;
        }
    }
    const DesiredSettings* previous = previousRecord ? &previousRecord->desired
        : bootProfile ? nullptr : intent;
    GraphicsScalarOperation graphicsOperation(ctl.gpu.graphicsRecovery);
    const gc_u32 graphicsCoverage = previousRecord ? previousRecord->graphicsControlSurfaces : 0;
    char postPrepareOwnershipErr[192] = {};
    const bool selectedFanTargetOwned =
        daemon_fan_ownership_allows_selected_write(ctl,
            postPrepareOwnershipErr, sizeof(postPrepareOwnershipErr));
    const bool uncertainAfterPrepare = auto_restore_state_uncertain(ctl, standbyFanStashReplay);
    if (!linux_auto_restore_may_write_after_prepare(
            &ctl.autoRestoreGuard, uncertainAfterPrepare,
            ctl.fanOwnershipHandbackPending, selectedFanTargetOwned)) {
        gc_snprintf(outcome.message, sizeof(outcome.message),
            "automatic restore stopped after backend preparation: %s",
            !selectedFanTargetOwned && postPrepareOwnershipErr[0]
                ? postPrepareOwnershipErr
                : ctl.autoRestoreGuard.lockedOut
                    ? linux_auto_restore_lockout_reason_name(
                          ctl.autoRestoreGuard.lockoutReason)
                    : ctl.fanOwnershipHandbackPending
                        ? "fan ownership recovery is still pending"
                        : "daemon state became uncertain");
        dlog("daemon auto-restore: %s aborted after prepare; lockedOut=%d"
             " reason=%u stateUncertain=%d effectiveUncertain=%d handbackPending=%d ownership=%d%s%s\n",
             linux_auto_restore_trigger_name(trigger),
             ctl.autoRestoreGuard.lockedOut ? 1 : 0,
             (unsigned int)ctl.autoRestoreGuard.lockoutReason,
             ctl.stateUncertain ? 1 : 0, uncertainAfterPrepare ? 1 : 0,
             ctl.fanOwnershipHandbackPending ? 1 : 0,
             selectedFanTargetOwned ? 1 : 0,
             postPrepareOwnershipErr[0] ? ": " : "",
             postPrepareOwnershipErr);
        return outcome;
    }
    // The journal on disk must be this card's (or absent): an automatic write
    // never takes over another card's state (linux_daemon_journal.h).
    LinuxJournalPin journal = {};
    if (!daemon_journal_admit(ctl, LINUX_JOURNAL_WRITE_AUTOMATIC, &journal, err, sizeof(err))) {
        gc_snprintf(outcome.message, sizeof(outcome.message), "automatic restore refused: %s", err);
        // The unreadable journal is gone from disk now; hold the refusal until
        // an explicit Apply/Reset, or the next resume would find nothing left
        // to refuse on.
        if (journal.lost) daemon_enter_uncertain_state(ctl, "automatic restore found an unreadable journal");
        return outcome;
    }
    // The card this unattended write binds must be proven, not only the
    // journal: a boot profile can exist without one. While discovery skipped
    // a device, following the target's model could land on its twin.
    const LinuxJournalOwner targetProof = linux_journal_owner(*target, storedRole, uuid ? *uuid : LinuxGpuUuid{},
        daemon_journal_evidence(ctl), nullptr);
    if (targetProof != LINUX_JOURNAL_OWNER_SERVED) {
        gc_snprintf(outcome.message, sizeof(outcome.message),
            "automatic restore refused: its GPU cannot be proven on the bound card (%s)",
            linux_journal_owner_name(targetProof));
        dlog("daemon auto-restore: %s refused; target proof=%s adapters=%u reported=%u\n",
             linux_auto_restore_trigger_name(trigger), linux_journal_owner_name(targetProof),
             ctl.gpu.adapterCount, ctl.gpu.adapterReportedCount);
        return outcome;
    }
    // The journal's named graphics coverage joins this card's recovery only
    // once the journal and the target are proven to be this card's.
    if (graphicsCoverage) {
        const gc_u32 ownedSurface = desired_named_graphics_surface(*previous);
        if (graphicsCoverage & ~graphics_scalar_recovery_bit(ownedSurface)) {
            daemon_enter_uncertain_state(ctl, "automatic restore has unresolved named graphics controls");
            gc_strlcpy(outcome.message, sizeof(outcome.message), "Named graphics recovery requires explicit Reset");
            return outcome;
        }
        ctl.gpu.graphicsRecovery.covered |= graphicsCoverage;
        ctl.gpu.graphicsRecovery.ownedSurface = ownedSurface;
    }
    const LinuxGpuUuid binding = linux_gpu_uuid_retain(linux_backend_selected_uuid(&ctl.gpu), uuid);
    dlog("daemon auto-restore: UUID binding readable=%d retained=%d\n",
        linux_gpu_uuid_valid(linux_backend_selected_uuid(&ctl.gpu)) ? 1 : 0,
        linux_gpu_uuid_valid(&binding) ? 1 : 0);
    dlog("daemon auto-restore: target role=%s location=%04x:%02x:%02x.%u\n",
        configured_gpu_location_role_name(role), ctl.gpu.selectedGpu.pciDomain,
        ctl.gpu.selectedGpu.pciBus, ctl.gpu.selectedGpu.pciDevice,
        ctl.gpu.selectedGpu.pciFunction);
    DesiredSettings committed = *intent;
    LockMode storedLockMode = committed.lockMode;
    committed.lockMode = profile_lock_mode_after_load(committed.hasLock, true,
                                                      committed.lockMode);
    if (storedLockMode != committed.lockMode) {
        dlog("daemon auto-restore: migrated persisted enabled lock mode %d to %d\n",
             (int)storedLockMode, (int)committed.lockMode);
    }
    validate_desired_settings_for_ipc(&committed);

    // Profile selection transfers ownership from the previous committed intent.
    // Restore-last and resume replay the existing ownership declaration.
    DesiredSettings request = {};
    const bool requestBuilt = bootProfile
        ? service_build_profile_transition_request(previous, &committed, &request)
        : service_build_full_restore_request(&committed, &request);
    if (!requestBuilt) {
        gc_strlcpy(outcome.message, sizeof(outcome.message), "could not build the restore request");
        return outcome;
    }
    if (request.applicationScope != DESIRED_SCOPE_LEGACY &&
        !daemon_resolve_scoped_ownership(ctl, previous, &request, &committed, err, sizeof(err))) {
        gc_strlcpy(outcome.message, sizeof(outcome.message), err);
        return outcome;
    }
    LinuxHardwareSnapshot before = {};
    char captureErr[256] = {};
    if (!linux_backend_capture_apply_snapshot(&ctl.gpu, &request, &before, captureErr,
                                        sizeof(captureErr), previous, &committed)) {
        gc_snprintf(outcome.message, sizeof(outcome.message),
                    "could not capture hardware state before automatic "
                    "restore: %s",
                    captureErr[0] ? captureErr : "unknown error");
        dlog("daemon auto-restore: %s aborted before any write because the "
             "rollback snapshot failed: %s\n",
             linux_auto_restore_trigger_name(trigger),
             captureErr[0] ? captureErr : "unknown error");
        return outcome;
    }
    char ownershipErr[256] = {};
    if (!daemon_fan_ownership_ensure_before_write(ctl, &committed, ownershipErr, sizeof(ownershipErr)) ||
        !store_daemon_record(ctl, LINUX_DAEMON_RECORD_PREPARED, &ctl.gpu.selectedGpu, &committed,
            ownershipErr, sizeof(ownershipErr), 0, SERVICE_OPERATION_NONE, role, &binding)) {
        gc_snprintf(outcome.message, sizeof(outcome.message),
                    "recovery ownership could not be recorded before automatic "
                    "restore: %s",
                    ownershipErr[0] ? ownershipErr : "unknown error");
        dlog("daemon auto-restore: %s aborted before any write: %s\n",
             linux_auto_restore_trigger_name(trigger), outcome.message);
        return outcome;
    }
    dlog("daemon auto-restore: %s writing intent [previous=%d resetBaseline=%d curvePoints=%d "
         "gpuOffset=%d memOffset=%d powerPct=%d lock=%d lockMode=%d fanMode=%d fanPct=%d]\n",
         linux_auto_restore_trigger_name(trigger), previous ? 1 : 0,
         request.resetOcBeforeApply ? 1 : 0,
         service_desired_curve_point_count(&request),
         request.hasGpuOffset ? request.gpuOffsetMHz : 0,
         request.hasMemOffset ? request.memOffsetMHz : 0,
         request.hasPowerLimit ? request.powerLimitPct : 0,
         request.hasLock ? 1 : 0, (int)request.lockMode,
         request.hasFan ? (int)request.fanMode : -1,
         request.hasFan ? request.fanPercent : 0);

    outcome.attempted = true;
    LinuxApplyCeilingTransaction ceiling = {};
    LinuxMutationResult mutation = linux_backend_apply(&ctl.gpu, ceiling, &request,
        previous, &committed, outcome.message, sizeof(outcome.message));
    if (!mutation.success) {
        outcome.success = false;
        char stateErr[256] = {};
        // The card the backend resolved and wrote, not the stored target,
        // which may name the card's location before a PCI renumbering.
        store_daemon_record(ctl, LINUX_DAEMON_RECORD_UNCERTAIN, &ctl.gpu.selectedGpu, &committed,
                            stateErr, sizeof(stateErr), 0, SERVICE_OPERATION_NONE, role, &binding);
        daemon_enter_uncertain_state(ctl, "automatic restore write failed");
        auto_restore_note_automatic_failure(ctl, trigger, outcome.message,
                                            mutation.rollbackSucceeded);
        dlog("daemon auto-restore: %s failed -> %s (rollback=%d anyWrite=%d)\n",
             linux_auto_restore_trigger_name(trigger), outcome.message,
             mutation.rollbackSucceeded ? 1 : 0, mutation.anyWrite ? 1 : 0);
        return outcome;
    }

    // Re-committed rather than assumed: a lock-mode migration above, or a v1
    // record read back through the compatibility path, would otherwise leave
    // the file disagreeing with what is now on the hardware.
    char stateErr[256] = {};
    GpuAdapterInfo committedTarget = ctl.gpu.selectedGpu;
    // A binding that lost its write identity during the write is never
    // committed (store_daemon_record refuses ACTIVE): it takes the failure
    // path below (rollback refused, UNCERTAIN).
    if (!ctl.gpu.writeIdentityResolved)
        gc_strlcpy(stateErr, sizeof(stateErr), "the GPU's identity could no longer be confirmed after the write");
    if (!store_daemon_record(ctl, LINUX_DAEMON_RECORD_ACTIVE, &committedTarget,
                             &committed, stateErr, sizeof(stateErr), 0, SERVICE_OPERATION_NONE, role, &binding)) {
        char commitErr[256] = {};
        gc_strlcpy(commitErr, sizeof(commitErr),
                   stateErr[0] ? stateErr : "unknown error");
        char rollbackErr[256] = {};
        bool rollbackOk = linux_backend_restore_snapshot(
            &ctl.gpu, ceiling, &before, mutation.attemptedPhases,
            rollbackErr, sizeof(rollbackErr));
        char uncertainErr[256] = {};
        bool uncertaintyRecorded = store_daemon_record(ctl,
            LINUX_DAEMON_RECORD_UNCERTAIN, &committedTarget, &committed,
            uncertainErr, sizeof(uncertainErr), 0, SERVICE_OPERATION_NONE, role, &binding);
        daemon_enter_uncertain_state(ctl, "automatic restore persistence failed");
        gc_snprintf(outcome.message, sizeof(outcome.message),
                    "automatic restore persistence failed after hardware "
                    "write; hardware rollback %s: %s",
                    rollbackOk ? "succeeded" : "is uncertain", commitErr);
        auto_restore_note_automatic_failure(ctl, trigger, outcome.message, rollbackOk);
        dlog("daemon auto-restore: %s applied but ACTIVE commit failed: %s; "
             "hardware rollback=%d%s%s; uncertain record persisted=%d%s%s\n",
             linux_auto_restore_trigger_name(trigger),
             commitErr, rollbackOk ? 1 : 0,
             rollbackOk ? "" : ": ",
             rollbackOk ? "" :
                 (rollbackErr[0] ? rollbackErr : "unknown error"),
             uncertaintyRecorded ? 1 : 0,
             uncertaintyRecorded ? "" : ": ",
             uncertaintyRecorded ? "" :
                 (uncertainErr[0] ? uncertainErr : "unknown error"));
        wake_fan_runtime(ctl);
        return outcome;
    }
    outcome.success = true;
    ctl.activeDesired = committed;
    ctl.activeTarget = committedTarget;
    ctl.activeTargetRole = role;
    ctl.activeTargetUuid = binding;
    ctl.hasActiveDesired = true;
    ctl.stateUncertain = false;
    ctl.fanFailureCount = 0;
    if (committed.hasFan && committed.fanMode == FAN_MODE_AUTO)
        daemon_fan_ownership_retire(ctl, "automatic restore returned the fan");
    wake_fan_runtime(ctl);
    dlog("daemon auto-restore: %s committed -> %s\n",
         linux_auto_restore_trigger_name(trigger), outcome.message);
    return outcome;
}

// SERVICE_CMD_RESUME_RESTORE.  The Linux counterpart of the Windows
// PBT_APMRESUME* path: restore the complete in-memory intent exactly once,
// with no proof gate, using nothing the caller supplied.  A fan runtime
// lockout since the suspend kept the intent it replaced; that is replayed
// instead, once (linux_standby_fan_stash_policy.h).
static void daemon_handle_resume_restore(LinuxGpuController& ctl, ServiceResponse* resp) {
    if (!resp) return;
    // This wake's restore is running: a later lockout is not the waking driver.
    daemon_standby_window_close(ctl, "the resume restore");
    const bool replay = daemon_standby_fan_stash_peek(ctl) == LINUX_STANDBY_FAN_STASH_REPLAY;
    if (!ctl.hasActiveDesired && !replay) {
        // Not an error: a machine with no Green Curve settings applied has
        // nothing to lose across a suspend, and saying so keeps the resume unit
        // from reporting a failure on every wake.
        resp->status = SERVICE_STATUS_OK;
        gc_strlcpy(resp->message, sizeof(resp->message),
            "no active Green Curve intent; nothing to restore after resume");
        dlog("daemon resume: no active intent; nothing to restore\n");
        populate_snapshot(ctl, &resp->snapshot, &resp->controlState);
        return;
    }
    // Copies: a successful write replaces ctl.activeDesired and a used-up
    // stash is cleared, so neither may alias the intent being written.
    const LinuxStandbyFanStash stash = ctl.standbyFanStash;
    const DesiredSettings intent = replay ? stash.replaced : ctl.activeDesired;
    const GpuAdapterInfo target = replay ? stash.replacedTarget : ctl.activeTarget;
    const GpuLocationRole role = replay ? stash.replacedRole : ctl.activeTargetRole;
    const LinuxGpuUuid uuid = replay ? stash.replacedUuid : ctl.activeTargetUuid;
    if (replay) {
        dlog("daemon resume: replaying the intent a fan lockout replaced after suspend "
             "(fanMode=%d ownedNow=%d serial=%llu)\n",
             fan_owned_intent_mode(intent.hasFan != 0, intent.fanMode, intent.fanAuto != 0),
             ctl.hasActiveDesired ? 1 : 0, stash.uncertainSerial);
    } else {
        dlog("daemon resume: restoring the complete active intent after suspend\n");
    }
    LinuxAutoRestoreOutcome outcome = daemon_automatic_restore_write(ctl,
        LINUX_AUTO_RESTORE_TRIGGER_RESUME, &target, &intent, role, &uuid,
        nullptr, replay);
    if (linux_standby_fan_stash_used_up(replay, outcome.success, outcome.attempted)) {
        dlog("daemon standby fan stash: used up by the resume restore (success=%d)\n",
             outcome.success ? 1 : 0);
        ctl.standbyFanStash = LinuxStandbyFanStash{};
    }
    // The curve the lockout detached is running again; its fault record is
    // history, as after an explicit Apply that sets the fan.
    if (replay && outcome.success)
        daemon_fan_fault_clear(ctl, "the resume restore's fan stash replay");
    dlog("daemon resume: result success=%d attempted=%d fanStashReplay=%d uncertain=%d: %s\n",
         outcome.success ? 1 : 0, outcome.attempted ? 1 : 0, replay ? 1 : 0,
         ctl.stateUncertain ? 1 : 0, outcome.message);
    resp->status = outcome.success ? SERVICE_STATUS_OK : SERVICE_STATUS_ERROR;
    gc_strlcpy(resp->message, sizeof(resp->message),
               outcome.message[0] ? outcome.message : "resume restore failed");
    populate_snapshot(ctl, &resp->snapshot, &resp->controlState);
    if (ctl.hasActiveDesired) resp->desired = ctl.activeDesired;
    if (ctl.stateUncertain) wake_fan_runtime(ctl);
}

#endif // GREEN_CURVE_LINUX_AUTO_RESTORE_RUNTIME_H
