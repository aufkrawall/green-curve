// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_startup_policy.h — the daemon's boot-apply policy.
//
// "What, if anything, does the daemon write to the GPU when it starts?"  Every
// build before protocol v13 answered that with a single hard-coded rule: replay
// the committed active state.  That is still the default (RESTORE_LAST is zero,
// so an absent record behaves exactly as before), but it is now one of three
// administrator-selectable modes, and the choice is persisted with the same
// root-owned checksummed atomic write as the committed intent because it
// authorizes an unattended hardware write.
//
// Included by linux_daemon.cpp; it works on the GPU controller it is handed
// (ctl.startupPolicy, ctl.gpu, ctl.activeDesired, ...) and is addressed together
// with that file by the source guards.  Do not compile separately.

#ifndef GREEN_CURVE_LINUX_STARTUP_POLICY_H
#define GREEN_CURVE_LINUX_STARTUP_POLICY_H

// ---------------------------------------------------------------------------
// Daemon side
// ---------------------------------------------------------------------------

// Publish the committed boot-apply snapshot on a startup-policy response.
//
// Read from ctl.startupPolicy, never from the request: a set or refresh that
// failed to reach disk leaves the stored record untouched, and the client must
// see what will actually boot rather than what it asked for.  resp->desired is
// deliberately NOT touched here -- it belongs to the active intent, and
// daemon_stamp_state_envelope() fills it on the way out.  v15 returned the
// snapshot in that member instead, so the stamp overwrote it and every
// staleness check ran against the applied settings.
static void daemon_publish_startup_snapshot(LinuxGpuController& ctl, ServiceResponse* resp) {
    if (ctl.startupPolicy.mode != SERVICE_STARTUP_POLICY_PROFILE) return;
    resp->startupProfile = ctl.startupPolicy.desired;
    resp->startupProfileValid = true;
}

static void daemon_handle_get_startup_policy(LinuxGpuController& ctl, ServiceResponse* resp) {
    // The mode/slot pair is stamped onto every envelope; this command
    // additionally returns the stored settings and the profile name, so
    // a client can show *what* would be applied, not just that it would.
    if (ctl.startupPolicy.mode == SERVICE_STARTUP_POLICY_PROFILE) {
        gc_snprintf(resp->message, sizeof(resp->message),
            "startup policy: apply profile %u (%s) to %s",
            (unsigned int)ctl.startupPolicy.profileSlot,
            ctl.startupPolicy.profileName[0] ? ctl.startupPolicy.profileName
                                           : "unnamed",
            ctl.startupPolicy.targetGpu.name[0]
                ? ctl.startupPolicy.targetGpu.name : "the stored GPU");
    } else {
        gc_snprintf(resp->message, sizeof(resp->message),
            "startup policy: %s",
            service_startup_policy_mode_name(ctl.startupPolicy.mode));
    }
    resp->status = SERVICE_STATUS_OK;
}

static void daemon_handle_set_startup_policy(LinuxGpuController& ctl, const ServiceRequest* req,
                                            ServiceResponse* resp) {
    LinuxDaemonStartupRecord record = {};
    DesiredSettings requested = req->desired;
    validate_desired_settings_for_ipc(&requested);
    linux_daemon_startup_initialize(&record, req->startupMode,
        req->profileSlot, req->source, &req->targetGpu, &requested,
        linux_gpu_location_role_for_selection(&req->targetGpu,
            ctl.gpu.adapters, ctl.gpu.adapterCount),
        linux_gpu_uuid_for_target(&req->targetGpu, ctl.gpu.adapters, ctl.gpu.adapterCount,
            ctl.gpu.adapterUuids));
    char storeErr[256] = {};
    if (!linux_daemon_startup_valid(&record)) {
        resp->status = SERVICE_STATUS_ERROR;
        gc_strlcpy(resp->message, sizeof(resp->message),
            "startup policy request is incomplete");
        dlog("daemon startup policy: rejected incomplete request mode=%u slot=%u\n",
             (unsigned int)req->startupMode,
             (unsigned int)req->profileSlot);
    } else if (!linux_daemon_startup_store(GC_DAEMON_STARTUP_FILE,
                                           &record, storeErr,
                                           sizeof(storeErr))) {
        // The in-memory copy is deliberately left untouched: a policy
        // that did not reach disk must not be advertised as committed.
        resp->status = SERVICE_STATUS_ERROR;
        gc_snprintf(resp->message, sizeof(resp->message),
            "cannot persist startup policy: %s",
            storeErr[0] ? storeErr : "unknown error");
        dlog("daemon startup policy: store failed: %s\n",
             storeErr[0] ? storeErr : "unknown error");
    } else {
        ctl.startupPolicy = record;
        resp->status = SERVICE_STATUS_OK;
        gc_snprintf(resp->message, sizeof(resp->message),
            "startup policy set to %s%s",
            service_startup_policy_mode_name(record.mode),
            record.mode == SERVICE_STARTUP_POLICY_PROFILE ? " snapshot" : "");
        dlog("daemon startup policy: committed mode=%s slot=%u role=%s name=%s "
             "[fanMode=%d fanPct=%d pollMs=%d hysteresisC=%d gpuOffset=%d "
             "memOffset=%d powerPct=%d]\n",
             service_startup_policy_mode_name(record.mode),
             (unsigned int)record.profileSlot,
             configured_gpu_location_role_name((GpuLocationRole)record.targetLocationRole),
             record.profileName,
             record.desired.fanMode, record.desired.fanPercent,
             record.desired.fanCurve.pollIntervalMs,
             record.desired.fanCurve.hysteresisC,
             record.desired.gpuOffsetMHz, record.desired.memOffsetMHz,
             record.desired.powerLimitPct);
    }
    // Snapshot, controls and active intent are the end-of-request stamp's to
    // fill (daemon_stamp_state_envelope); duplicating them here is what made it
    // easy to believe a handler owned resp->desired.
}

// Content-only refresh of the stored `profile N` snapshot, sent by the client
// right after it rewrote slot N in config.ini.  The daemon cannot read that
// file (ProtectHome=yes), so without this the boot-apply kept writing the
// snapshot captured when the policy was first set: a fan hysteresis or poll
// interval edited afterwards silently reverted at every boot.
//
// The stored mode, slot, display name and — most importantly — the exact GPU
// binding are reused verbatim.  Only the settings change, and only for a policy
// that already names the same slot, so a refresh can never create a boot-apply
// or move one to another GPU.
static void daemon_handle_refresh_startup_profile(LinuxGpuController& ctl, const ServiceRequest* req,
                                                  ServiceResponse* resp) {
    if (!startup_snapshot_refresh_allowed(ctl.startupPolicy.mode,
                                          ctl.startupPolicy.profileSlot,
                                          req->profileSlot)) {
        resp->status = SERVICE_STATUS_ERROR;
        gc_snprintf(resp->message, sizeof(resp->message),
            "no startup profile %u to refresh (policy is %s slot %u)",
            (unsigned int)req->profileSlot,
            service_startup_policy_mode_name(ctl.startupPolicy.mode),
            (unsigned int)ctl.startupPolicy.profileSlot);
        dlog("daemon startup policy: refresh rejected for slot=%u; stored mode=%s slot=%u\n",
             (unsigned int)req->profileSlot,
             service_startup_policy_mode_name(ctl.startupPolicy.mode),
             (unsigned int)ctl.startupPolicy.profileSlot);
    } else {
        if (req->expectedTopologySignature) {
            ServiceSnapshot current{};
            populate_snapshot(ctl, &current, nullptr);
            if (!daemon_selected_startup_topology(ctl) || !linux_mutation_authority_matches(req,
                    daemon_service_instance_id(), ctl.gpuGeneration, service_snapshot_topology_signature(&current),
                    &ctl.gpu.selectedGpu, SERVICE_MUTATION_DOMAIN_VF_CURVE)) {
                resp->status = SERVICE_STATUS_STALE_STATE;
                gc_strlcpy(resp->message, sizeof(resp->message), "startup GPU topology changed during refresh");
                dlog("startup sync: refusing stale topology attachment\n");
                return;
            }
        }
        DesiredSettings requested = req->desired;
        validate_desired_settings_for_ipc(&requested);
        LinuxDaemonStartupRecord record = {};
        linux_daemon_startup_initialize(&record, ctl.startupPolicy.mode,
            ctl.startupPolicy.profileSlot, ctl.startupPolicy.profileName,
            &ctl.startupPolicy.targetGpu, &requested,
            (GpuLocationRole)ctl.startupPolicy.targetLocationRole, &ctl.startupPolicy.targetUuid);
        char storeErr[256] = {};
        if (!linux_daemon_startup_valid(&record)) {
            resp->status = SERVICE_STATUS_ERROR;
            gc_strlcpy(resp->message, sizeof(resp->message),
                "refreshed startup profile is incomplete");
            dlog("daemon startup policy: refreshed record for slot=%u failed validation\n",
                 (unsigned int)req->profileSlot);
        } else if (!linux_daemon_startup_store(GC_DAEMON_STARTUP_FILE, &record,
                                               storeErr, sizeof(storeErr))) {
            // Same rule as the set path: a record that did not reach disk is
            // never advertised as committed, so the in-memory copy stands.
            resp->status = SERVICE_STATUS_ERROR;
            gc_snprintf(resp->message, sizeof(resp->message),
                "cannot persist refreshed startup profile: %s",
                storeErr[0] ? storeErr : "unknown error");
            dlog("daemon startup policy: refresh store failed: %s\n",
                 storeErr[0] ? storeErr : "unknown error");
        } else {
            bool changed = !desired_settings_equal(&ctl.startupPolicy.desired,
                                                   &record.desired);
            ctl.startupPolicy = record;
            resp->status = SERVICE_STATUS_OK;
            gc_snprintf(resp->message, sizeof(resp->message),
                "startup profile %u snapshot %s",
                (unsigned int)record.profileSlot,
                changed ? "refreshed" : "already current");
            dlog("daemon startup policy: refreshed slot=%u changed=%d "
                 "fanMode=%d fanPct=%d pollMs=%d hysteresisC=%d gpuOffset=%d "
                 "memOffset=%d powerPct=%d\n",
                 (unsigned int)record.profileSlot, changed ? 1 : 0,
                 record.desired.fanMode, record.desired.fanPercent,
                 record.desired.fanCurve.pollIntervalMs,
                 record.desired.fanCurve.hysteresisC,
                 record.desired.gpuOffsetMHz, record.desired.memOffsetMHz,
                 record.desired.powerLimitPct);
        }
    }
    // See daemon_handle_set_startup_policy: the stamp owns snapshot, controls
    // and active intent.
}

// Single entry point for the three startup-policy commands, so the daemon's
// command switch does not grow a case per verb.
//
// All three publish the committed snapshot afterwards, from one place: a get
// answers "what boots", and a set/refresh answers "what boots NOW", which is
// what lets a client verify the write it just made instead of re-reading.
static void daemon_handle_startup_policy_command(LinuxGpuController& ctl, const ServiceRequest* req,
                                                 ServiceResponse* resp) {
    if (req->command == SERVICE_CMD_GET_STARTUP_POLICY)
        daemon_handle_get_startup_policy(ctl, resp);
    else if (req->command == SERVICE_CMD_SET_STARTUP_POLICY)
        daemon_handle_set_startup_policy(ctl, req, resp);
    else
        daemon_handle_refresh_startup_profile(ctl, req, resp);
    daemon_publish_startup_snapshot(ctl, resp);
}

// Boot-apply of an explicitly configured profile.  This is a deliberate,
// administrator-configured write, not drift correction: it happens exactly once
// per daemon start, only for the exact GPU identity recorded with the policy,
// and a failure locks the daemon out of retrying instead of looping.
static bool apply_startup_profile_policy(LinuxGpuController& ctl, const LinuxDaemonStateRecord* previous) {
    const DesiredSettings& desired = ctl.startupPolicy.desired;
    // The snapshot's own values are logged before the write, not just
    // "applied": when a boot-applied profile disagrees with config.ini, this
    // line is what identifies the stale snapshot without a debugger.
    dlog("daemon: startup profile %u (%s) requested "
         "[fanMode=%d fanPct=%d pollMs=%d hysteresisC=%d gpuOffset=%d "
         "memOffset=%d powerPct=%d]\n",
         (unsigned int)ctl.startupPolicy.profileSlot,
         ctl.startupPolicy.profileName[0] ? ctl.startupPolicy.profileName : "-",
         desired.fanMode, desired.fanPercent,
         desired.fanCurve.pollIntervalMs, desired.fanCurve.hysteresisC,
         desired.gpuOffsetMHz, desired.memOffsetMHz, desired.powerLimitPct);
    // Same unattended-write path as the restore-last replay and the resume
    // restore: it owns the crash-loop guard, the GPU resolution, the reset-to-
    // stock baseline that keeps a boot apply from stacking onto whatever the
    // driver already holds, and the committed record on success.
    LinuxAutoRestoreOutcome outcome = daemon_automatic_restore_write(ctl,
        LINUX_AUTO_RESTORE_TRIGGER_BOOT_PROFILE,
        &ctl.startupPolicy.targetGpu, &ctl.startupPolicy.desired,
        (GpuLocationRole)ctl.startupPolicy.targetLocationRole, &ctl.startupPolicy.targetUuid, previous);
    if (!outcome.success) {
        // daemon_automatic_restore_write owns the uncertainty transition and
        // its fan handback on every path that reached a write.
        dlog("daemon: startup profile %u did not commit -> %s\n",
             (unsigned int)ctl.startupPolicy.profileSlot,
             outcome.message[0] ? outcome.message : "unknown reason");
        return false;
    }
    // The profile booted on its GPU after firmware renumbered the PCI buses
    // (linux_resolve_stored_gpu_target followed the model). Re-bind the record
    // to the live location, as the committed ACTIVE record already was, so it
    // stays exact if an identical GPU is added later.
    if (!linux_gpu_bdf_valid(&ctl.activeTarget) ||
        (linux_gpu_bdf_equal(&ctl.startupPolicy.targetGpu, &ctl.activeTarget) &&
         ctl.startupPolicy.targetLocationRole == (gc_u32)ctl.activeTargetRole &&
         memcmp(&ctl.startupPolicy.targetUuid, &ctl.activeTargetUuid, sizeof(ctl.activeTargetUuid)) == 0) ||
        !linux_gpu_model_matches(&ctl.startupPolicy.targetGpu, &ctl.activeTarget))
        return true;
    LinuxDaemonStartupRecord rebound = {};
    linux_daemon_startup_initialize(&rebound, ctl.startupPolicy.mode,
        ctl.startupPolicy.profileSlot, ctl.startupPolicy.profileName,
        &ctl.activeTarget, &ctl.startupPolicy.desired, ctl.activeTargetRole, &ctl.activeTargetUuid);
    char storeErr[160] = {};
    if (!linux_daemon_startup_valid(&rebound) ||
        !linux_daemon_startup_store(GC_DAEMON_STARTUP_FILE, &rebound,
                                    storeErr, sizeof(storeErr))) {
        // Not fatal: the stale record keeps resolving under the same rule.
        dlog("daemon startup policy: re-binding profile %u to %04x:%02x:%02x.%u "
             "failed: %s\n", (unsigned int)ctl.startupPolicy.profileSlot,
             ctl.activeTarget.pciDomain, ctl.activeTarget.pciBus,
             ctl.activeTarget.pciDevice, ctl.activeTarget.pciFunction,
             storeErr[0] ? storeErr : "record failed validation");
        return true;
    }
    dlog("daemon startup policy: profile %u re-bound from %04x:%02x:%02x.%u to "
         "%04x:%02x:%02x.%u (role=%s)\n",
         (unsigned int)ctl.startupPolicy.profileSlot,
         ctl.startupPolicy.targetGpu.pciDomain, ctl.startupPolicy.targetGpu.pciBus,
         ctl.startupPolicy.targetGpu.pciDevice,
         ctl.startupPolicy.targetGpu.pciFunction, ctl.activeTarget.pciDomain,
         ctl.activeTarget.pciBus, ctl.activeTarget.pciDevice,
         ctl.activeTarget.pciFunction, configured_gpu_location_role_name(ctl.activeTargetRole));
    ctl.startupPolicy = rebound;
    return true;
}

// Read the committed policy once at daemon start.  A present-but-unreadable
// record is deliberately NOT downgraded to "replay the old intent": a policy the
// administrator cannot read back is a reason to leave the GPU alone.
static void load_startup_policy_at_boot(LinuxGpuController& ctl) {
    // The boot-apply policy decides *whether* the block below may write at all.
    // A present-but-unusable record is never downgraded to "replay the old
    // intent": a policy the administrator cannot read back is a reason to leave
    // the GPU alone, not a reason to guess.
    bool startupPolicyCorrupt = false;
    bool startupPolicyMigratedFromLegacy = false;
    char startupErr[256] = {};
    if (linux_daemon_startup_load(GC_DAEMON_STARTUP_FILE, &ctl.startupPolicy,
                                  &startupPolicyCorrupt, startupErr,
                                  sizeof(startupErr),
                                  &startupPolicyMigratedFromLegacy)) {
        if (startupPolicyMigratedFromLegacy) {
            dlog("daemon: %s\n",
                 startupErr[0] ? startupErr : "startup policy migrated");
            // Persist the upgrade so the on-disk generation matches the
            // in-memory record. Failure is not fatal: the migration is a pure
            // function of the stored bytes, so the next boot converts the
            // same record to the same result.
            char storeErr[128] = {};
            if (!linux_daemon_startup_store(GC_DAEMON_STARTUP_FILE,
                                            &ctl.startupPolicy, storeErr,
                                            sizeof(storeErr))) {
                dlog("daemon: startup policy rewrite after generation "
                     "migration failed (%s); re-migrated identically on next "
                     "boot\n",
                     storeErr[0] ? storeErr : "unknown error");
            }
        }
        dlog("daemon: startup policy=%s slot=%u name=%s\n",
             service_startup_policy_mode_name(ctl.startupPolicy.mode),
             (unsigned int)ctl.startupPolicy.profileSlot,
             ctl.startupPolicy.profileName[0] ? ctl.startupPolicy.profileName : "-");
    } else if (startupPolicyCorrupt) {
        memset(&ctl.startupPolicy, 0, sizeof(ctl.startupPolicy));
        ctl.startupPolicy.mode = SERVICE_STARTUP_POLICY_NONE;
        dlog("daemon: startup policy unreadable (%s); applying nothing at "
             "startup until it is set again\n",
             startupErr[0] ? startupErr : "invalid record");
    } else {
        memset(&ctl.startupPolicy, 0, sizeof(ctl.startupPolicy));
        dlog("daemon: no startup policy configured; restoring the last "
             "committed intent\n");
    }
}

#endif  // GREEN_CURVE_LINUX_STARTUP_POLICY_H
