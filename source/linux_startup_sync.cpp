// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "linux_startup_sync.h"

#include "linux_daemon.h"
#include "linux_debug_log.h"
#include "linux_port.h"

#include <stdio.h>
#include <string.h>

LinuxStartupSyncResult linux_startup_sync_after_profile_write(
    const char* configPath, int slot, bool slotStillHasContent,
    bool daemonReachable, unsigned int policyMode, unsigned int policySlot) {
    LinuxStartupSyncResult result = {};
    result.action = startup_snapshot_sync_after_profile_write(
        daemonReachable, policyMode, policySlot, slot, slotStillHasContent);
    result.ok = true;
    if (result.action == STARTUP_SNAPSHOT_SYNC_NONE) return result;

    if (result.action == STARTUP_SNAPSHOT_SYNC_UNREACHABLE) {
        result.ok = false;
        snprintf(result.message, sizeof(result.message),
                 "daemon offline: the boot-apply snapshot for slot %d still "
                 "holds the previous values", slot);
        linux_debug_logf("startup sync: slot %d boots but the daemon is "
                         "unreachable; snapshot left stale", slot);
        return result;
    }

    char daemonResult[512] = {};
    if (result.action == STARTUP_SNAPSHOT_SYNC_UNBIND) {
        // The bound profile no longer exists. Leaving the policy in place would
        // keep writing a deleted profile at every boot, so boot-apply is turned
        // off rather than left pointing at a ghost.
        result.ok = linux_daemon_set_startup_policy(SERVICE_STARTUP_POLICY_NONE,
            0, nullptr, nullptr, nullptr, daemonResult, sizeof(daemonResult));
        snprintf(result.message, sizeof(result.message), result.ok
            ? "slot %d was the boot-apply profile; startup apply is now NOTHING"
            : "slot %d was the boot-apply profile and could not be unbound; it "
              "still applies at boot",
            slot);
        linux_debug_logf("startup sync: unbind of cleared slot %d ok=%d -> %s",
                         slot, result.ok ? 1 : 0,
                         daemonResult[0] ? daemonResult : "-");
        return result;
    }

    // Reloaded from disk rather than reusing the caller's in-memory copy: what
    // must boot is what the file now says, including anything the save path
    // normalized on the way out.
    DesiredSettings saved = {};
    char loadErr[256] = {};
    if (!load_profile_from_config_path(configPath, slot, &saved, loadErr,
                                       sizeof(loadErr))) {
        result.ok = false;
        snprintf(result.message, sizeof(result.message),
                 "boot-apply snapshot NOT refreshed: %s",
                 loadErr[0] ? loadErr : "profile reload failed");
        linux_debug_logf("startup sync: reload of slot %d failed: %s", slot,
                         loadErr[0] ? loadErr : "unknown error");
        return result;
    }
    normalize_desired_settings_for_ui(&saved);
    result.ok = linux_daemon_refresh_startup_profile(slot, &saved, daemonResult,
                                                     sizeof(daemonResult));
    snprintf(result.message, sizeof(result.message), result.ok
        ? "boot-apply snapshot for slot %d refreshed"
        : "boot-apply snapshot for slot %d NOT refreshed; it still holds the "
          "previous values",
        slot);
    linux_debug_logf("startup sync: refresh of slot %d ok=%d pollMs=%d "
                     "hysteresisC=%d fanMode=%d -> %s",
                     slot, result.ok ? 1 : 0, saved.fanCurve.pollIntervalMs,
                     saved.fanCurve.hysteresisC, saved.fanMode,
                     daemonResult[0] ? daemonResult : "-");
    return result;
}

static void print_desired_summary_line(FILE* out, const char* label,
                                       const DesiredSettings* d) {
    fprintf(out, "  %-7s fanMode=%d fanPct=%d pollMs=%d hysteresisC=%d "
            "gpuOffset=%d memOffset=%d powerPct=%d\n", label, d->fanMode,
            d->fanPercent, d->fanCurve.pollIntervalMs, d->fanCurve.hysteresisC,
            d->gpuOffsetMHz, d->memOffsetMHz, d->powerLimitPct);
}

void print_startup_snapshot_report(FILE* out,
                                   const LinuxStartupSnapshotReport* report,
                                   const char* configPath) {
    if (!out || !report) return;
    switch (report->state) {
        case STARTUP_SNAPSHOT_STATE_IN_SYNC:
            fprintf(out, "Startup snapshot matches profile %u in %s\n",
                    report->slot, configPath ? configPath : "the config");
            break;
        case STARTUP_SNAPSHOT_STATE_DIVERGED:
            fprintf(out, "WARNING: the startup snapshot differs from profile %u "
                    "in %s; re-save that profile to refresh what boots\n",
                    report->slot, configPath ? configPath : "the config");
            print_desired_summary_line(out, "boots:", &report->snapshot);
            print_desired_summary_line(out, "config:", &report->profile);
            break;
        case STARTUP_SNAPSHOT_STATE_PROFILE_MISSING:
            fprintf(out, "WARNING: profile %u no longer loads from %s (%s), but "
                    "its snapshot still applies at boot\n", report->slot,
                    configPath ? configPath : "the config",
                    report->detail[0] ? report->detail : "slot unavailable");
            break;
        case STARTUP_SNAPSHOT_STATE_UNKNOWN:
            fprintf(out, "Startup snapshot could not be read back (%s)\n",
                    report->detail[0] ? report->detail : "daemon request failed");
            break;
        case STARTUP_SNAPSHOT_STATE_NOT_APPLICABLE:
            break;
    }
}

LinuxStartupSnapshotReport linux_startup_snapshot_report(
    const char* configPath, bool daemonReachable, unsigned int policyMode,
    unsigned int policySlot) {
    LinuxStartupSnapshotReport report = {};
    report.slot = policySlot;
    if (daemonReachable && policyMode == SERVICE_STARTUP_POLICY_PROFILE) {
        ServiceResponse policy = {};
        char err[256] = {};
        if (linux_daemon_get_startup_policy(&policy, err, sizeof(err))) {
            // startupProfile, not desired: the latter is the ACTIVE intent that
            // the daemon stamps onto every response, so reading it here compared
            // whatever is applied right now against the profile and called the
            // difference a stale boot snapshot.
            if (policy.startupProfileValid) {
                report.snapshot = policy.startupProfile;
                report.snapshotKnown = true;
            } else {
                gc_strlcpy(report.detail, sizeof(report.detail),
                           "the daemon published no boot-apply snapshot");
                linux_debug_logf("startup sync: daemon reports policy mode=%u "
                                 "slot=%u but carried no snapshot",
                                 policy.state.startupPolicyMode,
                                 policy.state.startupPolicySlot);
            }
        } else {
            gc_strlcpy(report.detail, sizeof(report.detail),
                       err[0] ? err : "daemon request failed");
            linux_debug_logf("startup sync: cannot read the boot-apply "
                             "snapshot: %s", report.detail);
        }
    }
    char loadErr[256] = {};
    report.profileLoaded = startup_snapshot_slot_is_valid(policySlot) &&
        load_profile_from_config_path(configPath, (int)policySlot,
                                      &report.profile, loadErr,
                                      sizeof(loadErr));
    if (report.profileLoaded) normalize_desired_settings_for_ui(&report.profile);
    else if (loadErr[0] && !report.detail[0])
        gc_strlcpy(report.detail, sizeof(report.detail), loadErr);
    report.state = startup_snapshot_state_for(policyMode, policySlot,
        report.snapshotKnown, &report.snapshot, report.profileLoaded,
        &report.profile);
    // Logged on every evaluation: a boot-apply that disagrees with config.ini
    // is exactly what this report exists to expose, and the log outlives the
    // process that noticed.
    if (startup_snapshot_state_is_stale(report.state)) {
        linux_debug_logf("startup sync: boot-apply snapshot for profile %u is "
                         "stale (state=%d): boots pollMs=%d hysteresisC=%d "
                         "fanMode=%d fanPct=%d gpuOffset=%d memOffset=%d "
                         "powerPct=%d; config pollMs=%d hysteresisC=%d "
                         "fanMode=%d fanPct=%d gpuOffset=%d memOffset=%d "
                         "powerPct=%d",
                         policySlot, (int)report.state,
                         report.snapshot.fanCurve.pollIntervalMs,
                         report.snapshot.fanCurve.hysteresisC,
                         report.snapshot.fanMode, report.snapshot.fanPercent,
                         report.snapshot.gpuOffsetMHz,
                         report.snapshot.memOffsetMHz,
                         report.snapshot.powerLimitPct,
                         report.profile.fanCurve.pollIntervalMs,
                         report.profile.fanCurve.hysteresisC,
                         report.profile.fanMode, report.profile.fanPercent,
                         report.profile.gpuOffsetMHz,
                         report.profile.memOffsetMHz,
                         report.profile.powerLimitPct);
    }
    return report;
}
