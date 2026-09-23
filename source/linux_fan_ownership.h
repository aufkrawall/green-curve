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

static bool daemon_desired_takes_manual_fan(const DesiredSettings* desired) {
    return desired && desired->hasFan && desired->fanMode != FAN_MODE_AUTO;
}

// Before a write whose committed intent takes manual fan control.  Fails
// closed: a manual fan that a crash could strand must be recorded first.
static bool daemon_fan_ownership_ensure_before_write(
    const DesiredSettings* committed, char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!daemon_desired_takes_manual_fan(committed) ||
        g_fanOwnershipMarkerCommitted) return true;
    char bootId[LINUX_FAN_OWNERSHIP_BOOT_ID_MAX] = {};
    if (!linux_read_boot_id(bootId, sizeof(bootId))) {
        gc_strlcpy(err, errSize,
                   "boot identity unavailable; fan ownership cannot be recorded");
        return false;
    }
    LinuxFanOwnershipMarker marker = {};
    linux_fan_ownership_marker_initialize(&marker, bootId);
    if (!linux_fan_ownership_marker_store(GC_DAEMON_FAN_MARKER_FILE, &marker,
                                          err, errSize)) {
        dlog("daemon fan ownership: marker could not be committed (%s); "
             "refusing the write\n", err && err[0] ? err : "unknown error");
        return false;
    }
    g_fanOwnershipMarkerCommitted = true;
    dlog("daemon fan ownership: marker committed before taking manual fan control\n");
    return true;
}

// The fan is the driver's again (or was never ours): nothing to hand back.
static void daemon_fan_ownership_retire(const char* reason) {
    char err[256] = {};
    bool ok = linux_daemon_state_remove(GC_DAEMON_FAN_MARKER_FILE, err, sizeof(err));
    if (ok) g_fanOwnershipMarkerCommitted = false;
    dlog("daemon fan ownership: marker %s (%s)%s%s\n",
         ok ? "retired" : "could not be retired",
         reason && reason[0] ? reason : "unspecified",
         err[0] ? ": " : "", err);
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
    if (plan.deleteMarker) {
        if (plan.verdict == OWNERSHIP_HANDBACK_GIVE_UP_PREVIOUS_ATTEMPT_DIED) {
            dlog("daemon fan ownership: every allowed handback attempt died or "
                 "hung (systemd start timeout); NOT retrying (restart-loop guard). "
                 "Reset returns the fan.\n");
            // Published to clients (protocol v28) so the TUI/CLI say what to do.
            daemon_fan_ownership_latch_incomplete(
                "every allowed fan handback attempt died");
        }
        daemon_fan_ownership_retire(ownership_handback_verdict_name(plan.verdict));
        return;
    }
    if (!g_gpuReady) {
        // Kept for the next start in this boot; nothing can be written now.
        dlog("daemon fan ownership: GPU backend not ready; handback deferred\n");
        return;
    }
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
