// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once

static void daemon_apply_boot_policy() {
    LinuxDaemonStateRecord saved = {};
    char err[256] = {};
    bool migrated = false;
    const LinuxDaemonStateLoadResult load = linux_daemon_state_load(
        GC_DAEMON_STATE_FILE, &saved, err, sizeof(err), &migrated);
    if (load == LINUX_DAEMON_STATE_LOADED) {
        if (err[0]) dlog("daemon: %s\n", err);
        if (saved.operationId != 0) {
            const gc_u32 state = saved.state != LINUX_DAEMON_RECORD_ACTIVE ||
                saved.operationState == SERVICE_OPERATION_IN_PROGRESS
                ? (gc_u32)SERVICE_OPERATION_OUTCOME_UNKNOWN : saved.operationState;
            const gc_u32 status = state == SERVICE_OPERATION_SUCCEEDED
                ? (gc_u32)SERVICE_STATUS_OK : (gc_u32)SERVICE_STATUS_ERROR;
            service_operation_restore(&g_operationTracker, saved.operationId, state, status,
                service_response_resolve_outcome_severity(status, SERVICE_OUTCOME_SEVERITY_SUCCESS),
                state == SERVICE_OPERATION_SUCCEEDED
                    ? "operation restored from committed daemon state"
                    : "operation outcome became uncertain across daemon restart");
        }
    }
    switch (linux_daemon_boot_action(g_startupPolicy.mode, load, saved)) {
        case LINUX_DAEMON_BOOT_RECOVERY:
            g_stateUncertain = true;
            dlog("daemon: startup journal requires explicit recovery (load=%d state=%u named=%u): %s\n",
                 (int)load, saved.state, saved.graphicsControlSurfaces,
                 err[0] ? err : "automatic write withheld");
            break;
        case LINUX_DAEMON_BOOT_PROFILE:
            if (!apply_startup_profile_policy(load == LINUX_DAEMON_STATE_LOADED ? &saved : nullptr) &&
                saved.graphicsControlSurfaces && !g_stateUncertain)
                daemon_enter_uncertain_state("saved named graphics controls were not transitioned");
            break;
        case LINUX_DAEMON_BOOT_RESTORE_LAST: {
            const LinuxAutoRestoreOutcome outcome = daemon_automatic_restore_write(
                LINUX_AUTO_RESTORE_TRIGGER_BOOT_RESTORE_LAST,
                &saved.targetGpu, &saved.desired, (GpuLocationRole)saved.targetLocationRole,
                &saved.targetUuid, &saved);
            if (!outcome.success) {
                if (saved.graphicsControlSurfaces && !g_stateUncertain)
                    daemon_enter_uncertain_state("saved named graphics controls were not restored");
                dlog("daemon: startup reapply did not commit -> %s\n",
                     outcome.message[0] ? outcome.message : "unknown reason");
            }
            break;
        }
        case LINUX_DAEMON_BOOT_IDLE:
            if (load == LINUX_DAEMON_STATE_LOADED)
                dlog("daemon: committed intent present; startup replay disabled\n");
            break;
    }
}
