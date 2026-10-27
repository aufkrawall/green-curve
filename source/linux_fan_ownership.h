// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_fan_ownership.h -- the daemon half of ownership_handback_policy.h.
//
// A graceful daemon stop hands the fan back to the driver
// (daemon_release_fan_to_driver).  A crash, SIGKILL or OOM kill does not, and
// systemd's restart used to be non-mutating unless the startup policy replayed
// a fan intent -- so with `none`, a guard lockout or a failed replay the fan
// stayed frozen at its last manual duty with nothing tracking temperature.
//
// The marker is written before any write that takes manual fan control and
// retired once the fan is the driver's again.  At start, a marker from this
// boot is handed back once: fan to driver auto (100% if the driver refuses),
// exactly what the graceful stop does.  Clocks are left alone, as the graceful
// stop leaves them.  Included by linux_daemon.cpp after its globals.

#ifndef GREEN_CURVE_LINUX_FAN_OWNERSHIP_H
#define GREEN_CURVE_LINUX_FAN_OWNERSHIP_H

static bool g_fanOwnershipMarkerCommitted = false;
static bool g_fanOwnershipHandbackPending = false;
static LinuxFanOwnershipMarker g_fanOwnershipRecord = {};

// Loaded ownership has the same authority as a new marker. Resolve its full
// binding so a moved card can be recovered without accepting a replacement.
// A legacy marker has no binding: only an explicit Reset may name its GPU
// among several adapters; automatic recovery still requires a lone adapter.
static bool daemon_fan_ownership_allows_selected_write(char* err, size_t errSize,
                                                       bool explicitReset = false) {
    if (!g_fanOwnershipMarkerCommitted) return true;
    bool matches = false;
    if (g_gpu.writeIdentityResolved) {
        if (!g_fanOwnershipRecord.targetGpu.valid) {
            matches = explicitReset || g_gpu.adapterCount == 1;
        } else {
            const auto selected = linux_gpu_restore_selection(
                &g_fanOwnershipRecord.targetGpu,
                (GpuLocationRole)g_fanOwnershipRecord.targetLocationRole,
                g_gpu.adapters, g_gpu.adapterCount,
                &g_fanOwnershipRecord.targetUuid, g_gpu.adapterUuids, nullptr);
            matches = selected.writeResolved &&
                (unsigned int)selected.index == g_gpu.selectedAdapterIndex;
        }
    }
    if (!matches) {
        gc_strlcpy(err, errSize,
            "recover the previous GPU's fan ownership with Reset before writing another GPU");
        dlog("daemon fan ownership: write refused; outstanding target does not resolve to the selected adapter\n");
    }
    return matches;
}

static bool daemon_fan_ownership_shutdown_should_release() {
    // An inherited handback belongs to startup. A write this process took
    // ownership of still needs release after a failed Apply cleared intent.
    return g_fanOwnershipMarkerCommitted && !g_fanOwnershipHandbackPending;
}

static bool daemon_desired_takes_manual_fan(const DesiredSettings* desired) {
    return desired && desired->hasFan && desired->fanMode != FAN_MODE_AUTO;
}

// Before a write whose committed intent takes manual fan control.  Fails
// closed: a manual fan that a crash could strand must be recorded first.
static bool daemon_fan_ownership_ensure_before_write(
    const DesiredSettings* committed, char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!daemon_desired_takes_manual_fan(committed)) return true;
    if (!g_gpu.writeIdentityResolved) {
        gc_strlcpy(err, errSize, "fan ownership requires a resolved GPU target");
        return false;
    }
    if (!daemon_fan_ownership_allows_selected_write(err, errSize)) return false;
    if (g_fanOwnershipMarkerCommitted && !g_fanOwnershipHandbackPending) return true;
    char bootId[LINUX_FAN_OWNERSHIP_BOOT_ID_MAX] = {};
    if (!linux_read_boot_id(bootId, sizeof(bootId))) {
        gc_strlcpy(err, errSize,
                   "boot identity unavailable; fan ownership cannot be recorded");
        return false;
    }
    LinuxFanOwnershipMarker marker = {};
    linux_fan_ownership_marker_initialize(&marker, bootId, &g_gpu.selectedGpu,
        linux_gpu_location_role_for_selection(&g_gpu.selectedGpu, g_gpu.adapters,
                                              g_gpu.adapterCount),
        linux_backend_selected_uuid(&g_gpu));
    if (!linux_fan_ownership_marker_store(GC_DAEMON_FAN_MARKER_FILE, &marker,
                                          err, errSize)) {
        dlog("daemon fan ownership: marker could not be committed (%s); "
             "refusing the write\n", err && err[0] ? err : "unknown error");
        return false;
    }
    g_fanOwnershipMarkerCommitted = true;
    g_fanOwnershipHandbackPending = false;
    g_fanOwnershipRecord = marker;
    dlog("daemon fan ownership: marker committed before taking manual fan control\n");
    return true;
}

// The fan is the driver's again (or was never ours): nothing to hand back.
static void daemon_fan_ownership_retire(const char* reason, bool explicitReset = false) {
    char err[256] = {};
    if (!daemon_fan_ownership_allows_selected_write(err, sizeof(err), explicitReset)) {
        dlog("daemon fan ownership: marker retained (%s); selected adapter has not recovered its target\n",
             reason ? reason : "unspecified");
        return;
    }
    bool ok = linux_daemon_state_remove(GC_DAEMON_FAN_MARKER_FILE, err, sizeof(err));
    if (ok) {
        g_fanOwnershipMarkerCommitted = false;
        g_fanOwnershipHandbackPending = false;
        g_fanOwnershipRecord = {};
    }
    dlog("daemon fan ownership: marker %s (%s)%s%s\n",
         ok ? "retired" : "could not be retired",
         reason && reason[0] ? reason : "unspecified",
         err[0] ? ": " : "", err);
}

// Uncertain state idles the fan worker, so a manual duty this daemon may hold
// would stop tracking temperature. Return it to the driver first, as Windows
// rollback does; only an explicit Apply/Reset takes manual control again.
static void daemon_fan_ownership_release_for_uncertain(const char* reason) {
    const char* why = reason && reason[0] ? reason : "unspecified";
    if (!g_fanOwnershipMarkerCommitted) {
        dlog("daemon fan ownership: uncertain state (%s); no manual fan recorded\n", why);
        return;
    }
    char err[192] = {};
    if (!g_gpuReady || !daemon_fan_ownership_allows_selected_write(err, sizeof(err))) {
        dlog("daemon fan ownership: uncertain state (%s) left the owned fan untouched; "
             "gpuReady=%d target not selected\n", why, g_gpuReady ? 1 : 0);
        return;
    }
    bool autoOk = linux_backend_set_fan_auto(&g_gpu);
    bool emergency = false;
    if (fan_runtime_escalation_after_auto_restore(autoOk) ==
            FAN_RUNTIME_ESCALATION_EMERGENCY_MAX) {
        emergency = linux_backend_set_curve_fan_percent(
            &g_gpu, (unsigned int)FAN_RUNTIME_EMERGENCY_PERCENT);
    }
    dlog("daemon fan ownership: uncertain state (%s) returned the fan: driver auto ok=%d%s\n",
         why, autoOk ? 1 : 0,
         autoOk ? "" : (emergency ? "; forced emergency duty"
                                  : "; emergency duty ALSO failed"));
    if (autoOk) daemon_fan_ownership_retire(why);
}

// Every transition into uncertainty after a hardware write goes through here.
static void daemon_enter_uncertain_state(const char* reason) {
    if (!g_stateUncertain)
        dlog("daemon state: entering uncertain state (%s)\n",
             reason && reason[0] ? reason : "unspecified");
    g_stateUncertain = true;
    daemon_fan_ownership_release_for_uncertain(reason);
}

// Latch the automatic-restore guard with the reason clients show as "run
// Reset".  The guard is loaded before this runs (linux_daemon_run order).
static void daemon_fan_ownership_latch_incomplete(const char* context) {
    linux_auto_restore_note_lockout(&g_autoRestoreGuard,
        SERVICE_AUTO_RESTORE_LOCKOUT_HANDBACK_INCOMPLETE);
    char guardErr[256] = {};
    bool stored = linux_daemon_guard_store(GC_DAEMON_GUARD_FILE,
        &g_autoRestoreGuard, guardErr, sizeof(guardErr));
    dlog("daemon fan ownership: latched handback-incomplete lockout (%s) "
         "persisted=%d%s%s\n", context ? context : "", stored ? 1 : 0,
         guardErr[0] ? " detail=" : "", guardErr);
}

// Once per start, before the startup policy runs and before the fan worker
// exists, so nothing races it.
static void daemon_fan_ownership_handback_at_start() {
    LinuxFanOwnershipMarker marker = {};
    char err[256] = {};
    int loaded = linux_fan_ownership_marker_load(GC_DAEMON_FAN_MARKER_FILE,
                                                 &marker, err, sizeof(err));
    char bootId[LINUX_FAN_OWNERSHIP_BOOT_ID_MAX] = {};
    bool bootKnown = linux_read_boot_id(bootId, sizeof(bootId));
    OwnershipHandbackInputs in = {};
    in.markerPresent = loaded != 0;
    in.markerValid = loaded == 1;
    in.currentBootKnown = bootKnown;
    in.sameBoot = loaded == 1 && bootKnown && strcmp(marker.bootId, bootId) == 0;
    in.previousAttempts = loaded == 1 ? (unsigned int)marker.handbackAttempts : 0u;
    OwnershipHandbackPlan plan = ownership_handback_plan(
        in, OWNERSHIP_HANDBACK_SCOPE_FAN_ONLY);
    if (plan.verdict == OWNERSHIP_HANDBACK_NO_MARKER) return;
    dlog("daemon fan ownership: previous daemon left the fan under manual control; "
         "verdict=%s markerValid=%d bootKnown=%d sameBoot=%d "
         "previousAttempts=%u/%u retry=%d%s%s\n",
         ownership_handback_verdict_name(plan.verdict), in.markerValid ? 1 : 0,
         in.currentBootKnown ? 1 : 0, in.sameBoot ? 1 : 0,
         in.previousAttempts, (unsigned int)OWNERSHIP_HANDBACK_MAX_ATTEMPTS,
         plan.retry ? 1 : 0, err[0] ? " detail=" : "", err);
    if (plan.deleteMarker &&
        plan.verdict != OWNERSHIP_HANDBACK_GIVE_UP_PREVIOUS_ATTEMPT_DIED) {
        daemon_fan_ownership_retire(ownership_handback_verdict_name(plan.verdict));
        return;
    }
    // Keep the complete loaded binding even when no startup write can run.
    // Exhausting automatic attempts stops retries, not explicit recovery.
    g_fanOwnershipMarkerCommitted = true;
    g_fanOwnershipHandbackPending = true;
    g_fanOwnershipRecord = marker;
    dlog("daemon fan ownership: loaded outstanding marker retained for target-bound recovery\n");
    if (plan.verdict == OWNERSHIP_HANDBACK_GIVE_UP_PREVIOUS_ATTEMPT_DIED) {
        dlog("daemon fan ownership: every allowed handback attempt died or "
             "hung (systemd start timeout); NOT retrying (restart-loop guard). "
             "Reset returns the fan.\n");
        // Published to clients (protocol v28) so the TUI/CLI say what to do.
        daemon_fan_ownership_latch_incomplete(
            "every allowed fan handback attempt died");
        return;
    }
    if (!g_gpuReady) {
        // Kept for the next start in this boot; nothing can be written now.
        dlog("daemon fan ownership: GPU backend not ready; handback deferred\n");
        return;
    }
    if (!marker.targetGpu.valid) {
        // A legacy record carries no target. Multiple live adapters provide
        // no evidence about which fan the previous instance controlled.
        if (g_gpu.adapterCount != 1 || !g_gpu.writeIdentityResolved) {
            daemon_fan_ownership_latch_incomplete("legacy fan ownership target is ambiguous");
            dlog("daemon fan ownership: legacy target unresolved; marker retained, no write\n");
            return;
        }
        marker.targetGpu = g_gpu.selectedGpu;
        marker.targetUuid = linux_gpu_uuid_retain(linux_backend_selected_uuid(&g_gpu), nullptr);
    }
    if (!linux_backend_select_target(&g_gpu, &marker.targetGpu, err, sizeof(err),
            (GpuLocationRole)marker.targetLocationRole, &marker.targetUuid) ||
        !g_gpu.writeIdentityResolved) {
        daemon_fan_ownership_latch_incomplete("owned fan GPU is unavailable or ambiguous");
        dlog("daemon fan ownership: target resolution failed; marker retained, no write (%s)\n",
             err[0] ? err : "unresolved identity");
        return;
    }
    marker.targetGpu = g_gpu.selectedGpu;
    marker.targetUuid = linux_gpu_uuid_retain(linux_backend_selected_uuid(&g_gpu),
                                             &marker.targetUuid);
    dlog("daemon fan ownership: recorded adapter resolved before handback\n");
    // Counted before writing: a hang here is killed by systemd's start timeout
    // (this runs before READY=1), and the next start retries at most once.
    LinuxFanOwnershipMarker inFlight = marker;
    inFlight.handbackAttempts = marker.handbackAttempts + 1u;
    if (!linux_fan_ownership_marker_store(GC_DAEMON_FAN_MARKER_FILE, &inFlight,
                                          err, sizeof(err))) {
        dlog("daemon fan ownership: in-flight record failed (%s); not writing "
             "(an unguarded handback could loop)\n", err[0] ? err : "unknown");
        return;
    }
    g_fanOwnershipRecord = inFlight;
    bool autoOk = linux_backend_set_fan_auto(&g_gpu);
    bool emergency = false;
    if (fan_runtime_escalation_after_auto_restore(autoOk) ==
            FAN_RUNTIME_ESCALATION_EMERGENCY_MAX) {
        emergency = linux_backend_set_curve_fan_percent(
            &g_gpu, (unsigned int)FAN_RUNTIME_EMERGENCY_PERCENT);
    }
    dlog("daemon fan ownership: startup handback driver auto ok=%d%s\n",
         autoOk ? 1 : 0,
         autoOk ? "" : (emergency ? "; forced emergency duty"
                                  : "; emergency duty ALSO failed"));
    if (autoOk) {
        daemon_fan_ownership_retire("startup handback returned the fan");
    } else {
        // Survived but refused: this attempt returned, so it does not count as
        // one that died; the next start may try again.  Tell clients now.
        inFlight.handbackAttempts = 0u;
        linux_fan_ownership_marker_store(GC_DAEMON_FAN_MARKER_FILE, &inFlight,
                                         err, sizeof(err));
        daemon_fan_ownership_latch_incomplete(
            "the driver refused to take the fan back after an unexpected stop");
    }
}

#endif // GREEN_CURVE_LINUX_FAN_OWNERSHIP_H
