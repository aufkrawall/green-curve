// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Production ownership orchestration with in-memory records and driver calls.
#include "app_shared.h"
#include "fan_curve.h"
#include "fan_runtime_policy.h"
#include "linux_backend.h"
#include "linux_daemon_state.h"
#include <cstdio>
#include <cstring>

#define linux_fan_ownership_marker_load fixture_marker_load
#define linux_fan_ownership_marker_store fixture_marker_store
#define linux_daemon_guard_store fixture_guard_store
#define linux_backend_select_target fixture_select_target
#define linux_backend_set_fan_auto fixture_fan_auto
#define linux_backend_set_curve_fan_percent fixture_fan_percent
namespace linux_fan_ownership_fixture {
static LinuxGpuState g_gpu;
static bool g_gpuReady, g_stateUncertain, removed, storeFails, autoFails, lockedOut;
static int writtenAdapter, stores, writes, autoWrites, emergencyPercent;
static LinuxFanOwnershipMarker saved;
static LinuxAutoRestoreGuard g_autoRestoreGuard;
static void dlog(const char*, ...) {}
static void gc_strlcpy(char* out, size_t size, const char* text) {
    if (out && size) std::snprintf(out, size, "%s", text);
}
static bool linux_read_boot_id(char* out, size_t size) {
    gc_strlcpy(out, size, "fixture-boot"); return true;
}
static int linux_fan_ownership_marker_load(const char*, LinuxFanOwnershipMarker* out, char*, size_t) {
    *out = saved; return 1;
}
static bool linux_fan_ownership_marker_store(const char*, const LinuxFanOwnershipMarker* value, char*, size_t) {
    ++stores;
    if (storeFails) return false;
    saved = *value;
    return true;
}
static bool linux_daemon_state_remove(const char*, char*, size_t) { removed = true; return true; }
static bool linux_daemon_guard_store(const char*, const LinuxAutoRestoreGuard*, char*, size_t) {
    lockedOut = true; return true;
}
static bool linux_backend_select_target(LinuxGpuState* gpu, const GpuAdapterInfo* target,
    char*, size_t, GpuLocationRole role, const LinuxGpuUuid* uuid) {
    auto selected = linux_gpu_restore_selection(target, role, gpu->adapters,
        gpu->adapterCount, uuid, gpu->adapterUuids, nullptr);
    if (!selected.writeResolved) return false;
    gpu->selectedAdapterIndex = (unsigned int)selected.index;
    gpu->selectedGpu = gpu->adapters[selected.index];
    gpu->writeIdentityResolved = true;
    return true;
}
static bool linux_backend_set_fan_auto(LinuxGpuState* gpu) {
    ++writes;
    ++autoWrites;
    writtenAdapter = (int)gpu->selectedAdapterIndex;
    return !autoFails;
}
static bool linux_backend_set_curve_fan_percent(LinuxGpuState* gpu, unsigned int percent) {
    ++writes;
    emergencyPercent = (int)percent;
    writtenAdapter = (int)gpu->selectedAdapterIndex;
    return true;
}
#define GC_DAEMON_FAN_MARKER_FILE "fixture-marker"
#define GC_DAEMON_GUARD_FILE "fixture-guard"
#include "linux_fan_ownership.h"
#undef GC_DAEMON_FAN_MARKER_FILE
#undef GC_DAEMON_GUARD_FILE
#undef linux_fan_ownership_marker_load
#undef linux_fan_ownership_marker_store
#undef linux_daemon_guard_store
#undef linux_backend_select_target
#undef linux_backend_set_fan_auto
#undef linux_backend_set_curve_fan_percent

static void initialize(unsigned int count = 2) {
    g_gpu = {};
    g_gpu.adapterCount = count;
    for (unsigned int i = 0; i < count; ++i) {
        auto& gpu = g_gpu.adapters[i];
        gpu.valid = gpu.pciInfoValid = true;
        gpu.deviceId = 0x1234;
        gpu.pciBus = i + 1;
    }
    g_gpu.selectedGpu = g_gpu.adapters[0];
    g_gpu.writeIdentityResolved = count == 1;
    g_gpuReady = true;
    g_autoRestoreGuard = {};
    g_fanOwnershipMarkerCommitted = false;
    g_fanOwnershipHandbackPending = false;
    g_fanOwnershipRecord = {};
    removed = storeFails = autoFails = lockedOut = g_stateUncertain = false;
    stores = writes = autoWrites = emergencyPercent = 0;
    writtenAdapter = -1;
    linux_fan_ownership_marker_initialize(&saved, "fixture-boot",
        &g_gpu.adapters[count - 1]);
}
static int run() {
    initialize();
    daemon_fan_ownership_handback_at_start();
    if (writtenAdapter != 1 || writes != 1 || !removed || !g_gpu.writeIdentityResolved) return 7530;
    initialize();
    saved.targetGpu.deviceId++;
    daemon_fan_ownership_handback_at_start();
    if (writes || removed || stores || !lockedOut) return 7531;
    initialize();
    saved.targetGpu.pciBus = 9;
    daemon_fan_ownership_handback_at_start();
    if (writes || removed || !lockedOut) return 7532;
    initialize();
    saved.targetUuid = linux_gpu_uuid_parse("GPU-00000000-0000-0000-0000-000000000001");
    g_gpu.adapterUuids[1] = linux_gpu_uuid_parse("GPU-00000000-0000-0000-0000-000000000002");
    daemon_fan_ownership_handback_at_start();
    if (writes || removed || !lockedOut) return 7533;
    initialize();
    saved.targetUuid = linux_gpu_uuid_parse("GPU-00000000-0000-0000-0000-000000000001");
    g_gpu.adapterUuids[0] = saved.targetUuid;
    daemon_fan_ownership_handback_at_start();
    if (writtenAdapter != 0 || !removed) return 7534;
    initialize();
    storeFails = true;
    daemon_fan_ownership_handback_at_start();
    if (writes || removed) return 7535;
    initialize();
    autoFails = true;
    daemon_fan_ownership_handback_at_start();
    if (writes != 2 || writtenAdapter != 1 || removed || !lockedOut || saved.handbackAttempts) return 7536;
    initialize();
    std::memset(&saved.targetGpu, 0, sizeof(saved.targetGpu));
    daemon_fan_ownership_handback_at_start();
    if (writes || removed || !lockedOut) return 7537;
    initialize(1);
    std::memset(&saved.targetGpu, 0, sizeof(saved.targetGpu));
    daemon_fan_ownership_handback_at_start();
    if (writes != 1 || writtenAdapter != 0 || !removed || !saved.targetGpu.valid) return 7538;
    initialize();
    DesiredSettings desired = {};
    desired.hasFan = true;
    desired.fanMode = FAN_MODE_CURVE;
    char error[128] = {};
    if (daemon_fan_ownership_ensure_before_write(&desired, error, sizeof(error))) return 7539;
    g_gpu.writeIdentityResolved = true;
    if (!daemon_fan_ownership_ensure_before_write(&desired, error, sizeof(error)) ||
        !linux_gpu_identity_matches(&saved.targetGpu, &g_gpu.selectedGpu)) return 7540;
    g_gpu.selectedGpu = g_gpu.adapters[1];
    g_gpu.selectedAdapterIndex = 1;
    if (daemon_fan_ownership_ensure_before_write(&desired, error, sizeof(error))) return 7541;
    LinuxFanOwnershipMarker decoded = {};
    if (!linux_fan_ownership_marker_decode(&saved, sizeof(saved), &decoded) ||
        !linux_gpu_identity_matches(&decoded.targetGpu, &saved.targetGpu)) return 7542;
    LinuxFanOwnershipMarkerV1 old = {};
    old.magic = LINUX_FAN_OWNERSHIP_MARKER_MAGIC;
    old.version = 1;
    old.size = sizeof(old);
    old.handbackAttempts = 1;
    std::snprintf(old.bootId, sizeof(old.bootId), "fixture-boot");
    if (!linux_fan_ownership_marker_decode(&old, sizeof(old), &decoded) ||
        decoded.targetGpu.valid || decoded.handbackAttempts != 1) return 7543;
    old.bootId[sizeof(old.bootId) - 1] = 'x';
    if (linux_fan_ownership_marker_decode(&old, sizeof(old), &decoded)) return 7544;
    saved.targetLocationRole = 99;
    if (linux_fan_ownership_marker_decode(&saved, sizeof(saved), &decoded)) return 7545;
    // A failed startup handback keeps the original GPU's durable ownership.
    // A later manual Apply on another adapter must not replace that record.
    initialize();
    autoFails = true;
    daemon_fan_ownership_handback_at_start();
    const LinuxFanOwnershipMarker outstanding = saved;
    g_gpu.selectedAdapterIndex = 0;
    g_gpu.selectedGpu = g_gpu.adapters[0];
    if (daemon_fan_ownership_ensure_before_write(&desired, error, sizeof(error)) ||
        std::memcmp(&saved, &outstanding, sizeof(saved))) return 7560;
    if (daemon_fan_ownership_allows_selected_write(error, sizeof(error), true)) return 7561;
    daemon_fan_ownership_retire("unrelated reset", true);
    if (removed || std::memcmp(&saved, &outstanding, sizeof(saved))) return 7561;
    if (daemon_fan_ownership_shutdown_should_release()) return 7562;
    g_gpu.selectedAdapterIndex = 1;
    g_gpu.selectedGpu = g_gpu.adapters[1];
    if (!daemon_fan_ownership_allows_selected_write(error, sizeof(error), true)) return 7563;
    daemon_fan_ownership_retire("owned GPU reset", true);
    if (!removed || g_fanOwnershipMarkerCommitted || g_fanOwnershipHandbackPending) return 7563;

    initialize();
    autoFails = true;
    daemon_fan_ownership_handback_at_start();
    const int startupStores = stores;
    if (!daemon_fan_ownership_ensure_before_write(&desired, error, sizeof(error)) ||
        stores != startupStores + 1 || saved.handbackAttempts ||
        g_fanOwnershipHandbackPending || !daemon_fan_ownership_shutdown_should_release()) return 7564;

    initialize();
    saved.targetGpu.deviceId++;
    daemon_fan_ownership_handback_at_start();
    g_gpu.writeIdentityResolved = true;
    if (!g_fanOwnershipMarkerCommitted || !g_fanOwnershipHandbackPending ||
        daemon_fan_ownership_allows_selected_write(error, sizeof(error))) return 7565;
    initialize();
    g_gpuReady = false;
    daemon_fan_ownership_handback_at_start();
    if (!g_fanOwnershipMarkerCommitted || !g_fanOwnershipHandbackPending ||
        daemon_fan_ownership_shutdown_should_release() || removed || writes) return 7566;

    initialize();
    saved.handbackAttempts = OWNERSHIP_HANDBACK_MAX_ATTEMPTS;
    daemon_fan_ownership_handback_at_start();
    if (writes || stores || removed || !lockedOut ||
        !g_fanOwnershipMarkerCommitted || !g_fanOwnershipHandbackPending) return 7567;

    initialize();
    saved.targetUuid = linux_gpu_uuid_parse("GPU-00000000-0000-0000-0000-000000000001");
    g_gpu.adapterUuids[1] = linux_gpu_uuid_parse("GPU-00000000-0000-0000-0000-000000000002");
    daemon_fan_ownership_handback_at_start();
    g_gpu.selectedAdapterIndex = 1;
    g_gpu.selectedGpu = g_gpu.adapters[1];
    g_gpu.writeIdentityResolved = true;
    if (daemon_fan_ownership_allows_selected_write(error, sizeof(error), true)) return 7568;

    initialize();
    saved.targetUuid = linux_gpu_uuid_parse("GPU-00000000-0000-0000-0000-000000000001");
    g_gpu.adapterUuids[0] = saved.targetUuid;
    autoFails = true;
    daemon_fan_ownership_handback_at_start();
    if (!daemon_fan_ownership_allows_selected_write(error, sizeof(error)) ||
        !linux_gpu_bdf_equal(&g_fanOwnershipRecord.targetGpu, &g_gpu.selectedGpu)) return 7569;

    initialize();
    saved.targetGpu = {};
    daemon_fan_ownership_handback_at_start();
    g_gpu.writeIdentityResolved = true;
    if (daemon_fan_ownership_allows_selected_write(error, sizeof(error)) ||
        !daemon_fan_ownership_allows_selected_write(error, sizeof(error), true)) return 7570;
    daemon_fan_ownership_retire("explicit legacy recovery", true);
    if (!removed || g_fanOwnershipMarkerCommitted) return 7570;

    // Uncertainty idles the worker: an owned manual fan returns to the driver
    // first. Before this, the curve's last duty stayed pinned untracked.
    initialize();
    g_gpu.writeIdentityResolved = true;
    if (!daemon_fan_ownership_ensure_before_write(&desired, error, sizeof(error))) return 7580;
    writes = 0;
    daemon_enter_uncertain_state("fixture apply rollback failed");
    if (!g_stateUncertain || writes != 1 || autoWrites != 1 || writtenAdapter != 0 ||
        !removed || g_fanOwnershipMarkerCommitted) return 7581;
    initialize();
    g_gpu.writeIdentityResolved = true;
    if (!daemon_fan_ownership_ensure_before_write(&desired, error, sizeof(error))) return 7582;
    writes = autoWrites = 0;
    autoFails = true;
    daemon_enter_uncertain_state("fixture refused handback");
    if (writes != 2 || autoWrites != 1 || emergencyPercent != FAN_RUNTIME_EMERGENCY_PERCENT ||
        removed || !g_fanOwnershipMarkerCommitted) return 7583;
    initialize();
    g_gpu.writeIdentityResolved = true;
    daemon_enter_uncertain_state("fixture without manual fan");
    if (!g_stateUncertain || writes || removed) return 7584;
    initialize();
    g_gpu.writeIdentityResolved = true;
    if (!daemon_fan_ownership_ensure_before_write(&desired, error, sizeof(error))) return 7585;
    writes = 0;
    g_gpu.selectedGpu = g_gpu.adapters[1];
    g_gpu.selectedAdapterIndex = 1;
    daemon_enter_uncertain_state("fixture other adapter selected");
    if (writes || removed || !g_fanOwnershipMarkerCommitted) return 7586;
    initialize();
    g_gpu.writeIdentityResolved = true;
    if (!daemon_fan_ownership_ensure_before_write(&desired, error, sizeof(error))) return 7587;
    writes = 0;
    g_gpuReady = false;
    daemon_enter_uncertain_state("fixture backend unavailable");
    if (writes || removed || !g_fanOwnershipMarkerCommitted) return 7588;
    // Inherited recovery on its own adapter is returned too.
    initialize();
    autoFails = true;
    daemon_fan_ownership_handback_at_start();
    autoFails = false;
    writes = autoWrites = 0;
    daemon_enter_uncertain_state("fixture inherited ownership");
    if (autoWrites != 1 || writtenAdapter != 1 || g_fanOwnershipMarkerCommitted ||
        g_fanOwnershipHandbackPending) return 7589;
    return 0;
}
} // namespace linux_fan_ownership_fixture

int run_linux_fan_ownership_tests() {
    int result = linux_fan_ownership_fixture::run();
    if (result) std::fprintf(stderr, "regression assertion failed: %d\n", result);
    return result;
}
#ifdef GC_FAN_OWNERSHIP_FIXTURE_MAIN
int main() { return run_linux_fan_ownership_tests(); }
#endif
