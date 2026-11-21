// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "linux_backend.h"
#include "linux_daemon_state.h"
#include "desired_graphics_surface.h"

static inline bool linux_graphics_adopt_pending_record(LinuxGpuState* g,
    const LinuxDaemonStateRecord* record, bool forReset) {
    if (!record->graphicsControlSurfaces) return true;
    if (!forReset || !g->writeIdentityResolved) return false;
    const auto selected = linux_gpu_restore_selection(&record->targetGpu,
        (GpuLocationRole)record->targetLocationRole, g->adapters, g->adapterCount,
        &record->targetUuid, g->adapterUuids, nullptr);
    if (selected.index < 0 || (unsigned int)selected.index != g->selectedAdapterIndex ||
        !graphics_scalar_recovery_adopt(g->graphicsRecovery.covered, record->graphicsControlSurfaces, true)) return false;
    g->graphicsRecovery.uncertain |= record->graphicsControlSurfaces;
    return true;
}
static inline bool linux_daemon_record_requires_recovery(const LinuxDaemonStateRecord& record, bool canTransitionNamedControls) {
    return record.state != LINUX_DAEMON_RECORD_ACTIVE || (record.graphicsControlSurfaces && !canTransitionNamedControls);
}

enum LinuxDaemonBootAction {
    LINUX_DAEMON_BOOT_IDLE,
    LINUX_DAEMON_BOOT_RECOVERY,
    LINUX_DAEMON_BOOT_PROFILE,
    LINUX_DAEMON_BOOT_RESTORE_LAST,
};

static inline LinuxDaemonBootAction linux_daemon_boot_action(
    gc_u32 mode, LinuxDaemonStateLoadResult load, const LinuxDaemonStateRecord& saved) {
    if (load != LINUX_DAEMON_STATE_LOADED && load != LINUX_DAEMON_STATE_MISSING)
        return LINUX_DAEMON_BOOT_RECOVERY;
    if (load == LINUX_DAEMON_STATE_LOADED &&
        linux_daemon_record_requires_recovery(saved, mode != SERVICE_STARTUP_POLICY_NONE))
        return LINUX_DAEMON_BOOT_RECOVERY;
    if (mode == SERVICE_STARTUP_POLICY_PROFILE) return LINUX_DAEMON_BOOT_PROFILE;
    if (mode == SERVICE_STARTUP_POLICY_RESTORE_LAST && load == LINUX_DAEMON_STATE_LOADED)
        return LINUX_DAEMON_BOOT_RESTORE_LAST;
    return LINUX_DAEMON_BOOT_IDLE;
}
