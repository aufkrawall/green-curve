// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_gpu_target_config.cpp -- the client side of the persisted GPU
// selection (config.ini [gpu]).
//
// Firmware assigns PCI bus numbers at boot, so the stored BDF goes stale when
// an upstream device appears or disappears (an on-board NIC disabled in the
// firmware setup, an added NVMe drive).  These functions run in the
// unprivileged client, which owns config.ini: they resolve the stored
// selection against the daemon's live adapters with the recorded location role
// (linux_resolve_stored_gpu_target, the Windows rule), rewrite a record that
// moved or predates the role, and hand back the live adapter, so every request
// names a GPU the daemon matches exactly.  Split out of linux_main.cpp, which
// sits on its size ratchet.

#include "linux_daemon.h"
#include "linux_debug_log.h"
#include "linux_gpu_selection.h"
#include "linux_port.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

bool linux_gpu_target_select_exact(const char* configPath,
                                   GpuAdapterInfo* target,
                                   char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!target) return false;
    ServiceSnapshot snapshot = {};
    char snapshotErr[256] = {};
    if (!linux_daemon_snapshot(&snapshot, snapshotErr, sizeof(snapshotErr))) {
        snprintf(err, errSize, "Cannot validate GPU selection: %s", snapshotErr);
        return false;
    }
    int match = -1;
    for (unsigned int i = 0; i < snapshot.adapterCount && i < MAX_GPU_ADAPTERS;
         ++i) {
        const GpuAdapterInfo& adapter = snapshot.adapters[i];
        if (!adapter.valid || adapter.pciDomain != target->pciDomain ||
            adapter.pciBus != target->pciBus ||
            adapter.pciDevice != target->pciDevice ||
            adapter.pciFunction != target->pciFunction)
            continue;
        if (match >= 0) { match = -2; break; }
        match = (int)i;
    }
    if (match < 0 || !snapshot.adapters[match].pciInfoValid) {
        snprintf(err, errSize,
                 "Cannot validate GPU selection: exact BDF/PCI identity is %s",
                 match == -2 ? "ambiguous" : "not available");
        return false;
    }
    *target = snapshot.adapters[match];
    GpuLocationRole role = linux_gpu_location_role_for_selection(
        target, snapshot.adapters, snapshot.adapterCount);
    char saveErr[256] = {};
    if (!save_linux_gpu_selection(configPath, target, role, saveErr,
                                  sizeof(saveErr))) {
        snprintf(err, errSize, "Cannot persist GPU selection: %s", saveErr);
        return false;
    }
    linux_debug_logf("gpu selection: --gpu persisted %04x:%02x:%02x.%u "
                     "role=%s adapters=%u",
                     target->pciDomain, target->pciBus, target->pciDevice,
                     target->pciFunction,
                     configured_gpu_location_role_name(role),
                     snapshot.adapterCount);
    return true;
}

void linux_gpu_target_resolve_configured(const char* configPath,
                                         GpuAdapterInfo* target,
                                         GpuLocationRole role) {
    if (!target || !target->valid) return;
    ServiceSnapshot snapshot = {};
    char snapshotErr[256] = {};
    if (!linux_daemon_snapshot(&snapshot, snapshotErr, sizeof(snapshotErr)) ||
        snapshot.adapterCount == 0) {
        // The request that follows reports the daemon failure itself; the
        // stored target is sent unchanged and the daemon applies its own
        // single-adapter rule to it.
        linux_debug_logf("gpu selection: config.ini target left unresolved: %s",
                         snapshotErr[0] ? snapshotErr
                                        : "daemon reported no adapters");
        return;
    }
    bool moved = false;
    int index = linux_resolve_stored_gpu_target(target, role, snapshot.adapters,
        snapshot.adapterCount, &moved);
    char detail[256] = {};
    linux_format_stored_gpu_detail(target, role, snapshot.adapters,
        snapshot.adapterCount, detail, sizeof(detail));
    if (index < 0) {
        linux_debug_logf("gpu selection: config.ini GPU %s (%s)",
                         index == -2 ? "is ambiguous" : "is not present", detail);
        fprintf(stderr,
                "Warning: the GPU saved in config.ini is %s; select one with "
                "--gpu <domain:bus:device.function>.\n",
                index == -2 ? "ambiguous" : "not present");
        return;
    }
    const GpuAdapterInfo live = snapshot.adapters[index];
    const bool refresh = moved || role == GPU_LOCATION_ROLE_UNRECORDED;
    if (refresh && geteuid() == 0) {
        // An unasked rewrite as root could hand a user's config.ini to root
        // (sudo -E, portable mode). The request still uses the live adapter.
        linux_debug_logf("gpu selection: config.ini refresh skipped as root "
                         "moved=%d (%s)", moved ? 1 : 0, detail);
    } else if (refresh && linux_gpu_bdf_valid(&live) && live.pciInfoValid) {
        GpuLocationRole liveRole = linux_gpu_location_role_for_selection(
            &live, snapshot.adapters, snapshot.adapterCount);
        char saveErr[256] = {};
        if (save_linux_gpu_selection(configPath, &live, liveRole, saveErr,
                                     sizeof(saveErr)))
            linux_debug_logf("gpu selection: refreshed config.ini moved=%d "
                             "role=%s (was %s)", moved ? 1 : 0,
                             configured_gpu_location_role_name(liveRole),
                             detail);
        else
            linux_debug_logf("gpu selection: refreshing config.ini failed: "
                             "%s (%s)", saveErr[0] ? saveErr : "unknown error",
                             detail);
    } else if (moved) {
        linux_debug_logf("gpu selection: config.ini GPU moved; record kept "
                         "because the live location is unreadable (%s)",
                         detail);
    }
    *target = live;
}
