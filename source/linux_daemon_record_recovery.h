// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
// Included after the daemon state store; preserves pending recovery before a new journal.

// Capture before PREPARED replaces the durable record. An I/O failure does
// not justify discarding evidence; abort the operation before a hardware write.
static bool capture_uncertain_record(bool uncertain, LinuxDaemonStateRecord* record,
                                     bool* captured, char* err, size_t errSize, bool forReset = false) {
    *captured = false;
    if (!uncertain) return true;
    bool migrated = false;
    const LinuxDaemonStateLoadResult result = linux_daemon_state_load(
        GC_DAEMON_STATE_FILE, record, err, errSize, &migrated);
    if (result == LINUX_DAEMON_STATE_IO_ERROR) {
        dlog("daemon state: cannot preserve uncertain intent before mutation: %s\n",
             err[0] ? err : "state read failed");
        return false;
    }
    *captured = result == LINUX_DAEMON_STATE_LOADED;
    if (*captured && record->graphicsControlSurfaces) {
        if (!linux_graphics_adopt_pending_record(&g_gpu, record, forReset)) {
            gc_strlcpy(err, errSize, "Pending named graphics recovery requires Reset on its bound GPU");
            dlog("graphics-offset: recovery adoption refused reset=%d mask=%u\n",
                forReset ? 1 : 0, record->graphicsControlSurfaces);
            return false;
        }
    }
    dlog("daemon state: prior uncertain intent captured=%d load=%d\n",
         *captured ? 1 : 0, (int)result);
    return true;
}
