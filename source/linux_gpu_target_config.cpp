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
// (linux_configured_gpu_resolve, the Windows rule), rewrite a record that
// moved or predates the role, and hand back the live adapter, so every request
// names a GPU the daemon matches exactly.  A selection that does not resolve
// stops every write (linux_configured_gpu_request_target says why).  Split out
// of linux_main.cpp, which sits on its size ratchet.

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

void linux_configured_gpu_load(const char* configPath,
                               const GpuAdapterInfo* explicitTarget,
                               LinuxConfiguredGpu* gpu) {
    if (!gpu) return;
    *gpu = LinuxConfiguredGpu{};
    gpu->role = GPU_LOCATION_ROLE_UNRECORDED;
    LinuxGpuSelectionRecord record = LINUX_GPU_SELECTION_RECORD_ABSENT;
    if (explicitTarget) gpu->target = *explicitTarget;
    else record = load_linux_gpu_selection(configPath, &gpu->target, &gpu->role);
    gpu->status = linux_configured_gpu_status_for_record(
        record, explicitTarget && explicitTarget->valid);
    if (gpu->status != LINUX_CONFIGURED_GPU_NONE)
        linux_debug_logf("gpu selection: loaded record=%s status=%s role=%s",
                         explicitTarget ? "explicit"
                                        : linux_gpu_selection_record_name(record),
                         linux_configured_gpu_status_name(gpu->status),
                         configured_gpu_location_role_name(gpu->role));
}

void linux_configured_gpu_resolve_against(const char* configPath,
                                          LinuxConfiguredGpu* gpu,
                                          const GpuAdapterInfo* adapters,
                                          unsigned int count) {
    if (!gpu || (gpu->status != LINUX_CONFIGURED_GPU_PENDING &&
                 gpu->status != LINUX_CONFIGURED_GPU_UNVALIDATED)) return;
    const LinuxConfiguredGpuStatus before = gpu->status;
    char detail[256] = {};
    linux_format_stored_gpu_detail(&gpu->target, gpu->role, adapters, count,
                                   detail, sizeof(detail));
    bool moved = false;
    if (linux_configured_gpu_resolve(gpu, adapters, count, &moved) < 0) {
        // The TUI retries an unvalidated selection on every refresh; only a
        // change of outcome is worth a line.
        if (gpu->status != before)
            linux_debug_logf("gpu selection: config.ini GPU %s; writes stop "
                             "until a GPU is selected (%s)",
                             linux_configured_gpu_status_name(gpu->status),
                             detail);
        return;
    }
    const GpuAdapterInfo& live = gpu->target;
    const bool refresh = moved || gpu->role == GPU_LOCATION_ROLE_UNRECORDED;
    if (refresh && geteuid() == 0) {
        // An unasked rewrite as root could hand a user's config.ini to root
        // (sudo -E, portable mode). The request still uses the live adapter.
        linux_debug_logf("gpu selection: config.ini refresh skipped as root "
                         "moved=%d (%s)", moved ? 1 : 0, detail);
    } else if (refresh && linux_gpu_bdf_valid(&live) && live.pciInfoValid) {
        GpuLocationRole liveRole = linux_gpu_location_role_for_selection(
            &live, adapters, count);
        char saveErr[256] = {};
        if (save_linux_gpu_selection(configPath, &live, liveRole, saveErr,
                                     sizeof(saveErr))) {
            linux_debug_logf("gpu selection: refreshed config.ini moved=%d "
                             "role=%s (was %s)", moved ? 1 : 0,
                             configured_gpu_location_role_name(liveRole),
                             detail);
            gpu->role = liveRole;
        } else {
            linux_debug_logf("gpu selection: refreshing config.ini failed: "
                             "%s (%s)", saveErr[0] ? saveErr : "unknown error",
                             detail);
        }
    } else if (moved) {
        linux_debug_logf("gpu selection: config.ini GPU moved; record kept "
                         "because the live location is unreadable (%s)",
                         detail);
    }
}

// The CLI resolves once per process, against an untargeted snapshot taken
// right before the first request that names config.ini's GPU.
static void resolve_configured_from_daemon(const char* configPath,
                                           LinuxConfiguredGpu* gpu) {
    if (!gpu || gpu->status != LINUX_CONFIGURED_GPU_PENDING) return;
    ServiceSnapshot snapshot = {};
    char snapshotErr[256] = {};
    if (!linux_daemon_snapshot(&snapshot, snapshotErr, sizeof(snapshotErr))) {
        gpu->status = LINUX_CONFIGURED_GPU_UNVALIDATED;
        linux_debug_logf("gpu selection: config.ini GPU left unvalidated: %s",
                         snapshotErr[0] ? snapshotErr : "daemon request failed");
        return;
    }
    linux_configured_gpu_resolve_against(configPath, gpu, snapshot.adapters,
                                         snapshot.adapterCount);
}

static bool configured_gpu_needs_reselection(const LinuxConfiguredGpu* gpu) {
    return gpu && (gpu->status == LINUX_CONFIGURED_GPU_NOT_PRESENT ||
                   gpu->status == LINUX_CONFIGURED_GPU_AMBIGUOUS ||
                   gpu->status == LINUX_CONFIGURED_GPU_MALFORMED);
}

const GpuAdapterInfo* linux_gpu_target_for_read(const char* configPath,
                                                LinuxConfiguredGpu* gpu) {
    resolve_configured_from_daemon(configPath, gpu);
    const GpuAdapterInfo* target = nullptr;
    linux_configured_gpu_request_target(gpu, false, &target);
    if (configured_gpu_needs_reselection(gpu))
        fprintf(stderr,
                "Warning: %s; showing the daemon's current GPU. Select one "
                "with --gpu <domain:bus:device.function>.\n",
                linux_configured_gpu_problem(gpu->status));
    return target;
}

bool linux_gpu_target_for_write(const char* configPath,
                                LinuxConfiguredGpu* gpu,
                                const GpuAdapterInfo** target,
                                char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    resolve_configured_from_daemon(configPath, gpu);
    if (linux_configured_gpu_request_target(gpu, true, target)) return true;
    linux_debug_logf("gpu selection: write stopped before the daemon; "
                     "config.ini GPU status=%s",
                     linux_configured_gpu_status_name(gpu->status));
    snprintf(err, errSize, "Not sent to the daemon: %s; %s",
             linux_configured_gpu_problem(gpu->status),
             configured_gpu_needs_reselection(gpu)
                 ? "select the intended GPU with "
                   "--gpu <domain:bus:device.function>"
                 : "check that the daemon is running and reports its GPUs");
    return false;
}
