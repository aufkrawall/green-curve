// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Production ownership orchestration with in-memory records and driver calls.
#include "app_shared.h"
#include "fan_curve.h"
#include "fan_runtime_policy.h"
#include "linux_backend.h"
#include "linux_daemon_state.h"
#include "linux_gpu_controller.h"
#include <cstdio>
#include <cstring>

#define linux_fan_ownership_marker_load fixture_marker_load
#define linux_fan_ownership_marker_store fixture_marker_store
#define linux_daemon_guard_store fixture_guard_store
#define linux_backend_select_target fixture_select_target
#define linux_backend_set_fan_auto fixture_fan_auto
#define linux_backend_set_curve_fan_percent fixture_fan_percent
namespace linux_fan_ownership_fixture {
// The daemon's per-GPU state, passed explicitly like the daemon does.
static LinuxGpuController fixture;
static bool removed, storeFails, autoFails, lockedOut;
static int writtenAdapter, stores, writes, autoWrites, emergencyPercent;
static LinuxFanOwnershipMarker saved;
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
    fixture.gpu = {};
    fixture.gpu.adapterCount = count;
    for (unsigned int i = 0; i < count; ++i) {
        auto& gpu = fixture.gpu.adapters[i];
        gpu.valid = gpu.pciInfoValid = true;
        gpu.deviceId = 0x1234;
        gpu.pciBus = i + 1;
    }
    fixture.gpu.selectedGpu = fixture.gpu.adapters[0];
    fixture.gpu.writeIdentityResolved = count == 1;
    fixture.gpuReady = true;
    fixture.autoRestoreGuard = {};
    fixture.fanOwnershipMarkerCommitted = false;
    fixture.fanOwnershipHandbackPending = false;
    fixture.fanOwnershipHandbackDeferred = false;
    fixture.fanOwnershipRecord = {};
    removed = storeFails = autoFails = lockedOut = fixture.stateUncertain = false;
    stores = writes = autoWrites = emergencyPercent = 0;
    writtenAdapter = -1;
    linux_fan_ownership_marker_initialize(&saved, "fixture-boot",
        &fixture.gpu.adapters[count - 1]);
}
static int run() {
    LinuxAutoRestoreGuard gate = {};
    if (!linux_auto_restore_may_write_after_prepare(
            &gate, false, false, true)) return 7632;
    gate.lockedOut = 1;
    gate.lockoutReason = SERVICE_AUTO_RESTORE_LOCKOUT_HANDBACK_INCOMPLETE;
    if (linux_auto_restore_may_write_after_prepare(
            &gate, false, false, true) ||
        linux_auto_restore_may_write_after_prepare(
            &gate, false, true, true))
        return 7633;
    gate = {};
    if (linux_auto_restore_may_write_after_prepare(
            &gate, true, false, true) ||
        linux_auto_restore_may_write_after_prepare(
            &gate, false, true, true) ||
        linux_auto_restore_may_write_after_prepare(
            &gate, false, false, false)) return 7633;

    initialize();
    daemon_fan_ownership_handback_at_start(fixture);
    if (writtenAdapter != 1 || writes != 1 || !removed || !fixture.gpu.writeIdentityResolved) return 7530;
    initialize();
    saved.targetGpu.deviceId++;
    daemon_fan_ownership_handback_at_start(fixture);
    if (writes || removed || stores || !lockedOut) return 7531;
    initialize();
    saved.targetGpu.pciBus = 9;
    daemon_fan_ownership_handback_at_start(fixture);
    if (writes || removed || !lockedOut) return 7532;
    initialize();
    saved.targetUuid = linux_gpu_uuid_parse("GPU-00000000-0000-0000-0000-000000000001");
    fixture.gpu.adapterUuids[1] = linux_gpu_uuid_parse("GPU-00000000-0000-0000-0000-000000000002");
    daemon_fan_ownership_handback_at_start(fixture);
    if (writes || removed || !lockedOut) return 7533;
    initialize();
    saved.targetUuid = linux_gpu_uuid_parse("GPU-00000000-0000-0000-0000-000000000001");
    fixture.gpu.adapterUuids[0] = saved.targetUuid;
    daemon_fan_ownership_handback_at_start(fixture);
    if (writtenAdapter != 0 || !removed) return 7534;
    initialize();
    storeFails = true;
    daemon_fan_ownership_handback_at_start(fixture);
    if (writes || removed) return 7535;
    initialize();
    autoFails = true;
    daemon_fan_ownership_handback_at_start(fixture);
    if (writes != 2 || writtenAdapter != 1 || removed || !lockedOut || saved.handbackAttempts) return 7536;
    initialize();
    std::memset(&saved.targetGpu, 0, sizeof(saved.targetGpu));
    daemon_fan_ownership_handback_at_start(fixture);
    if (writes || removed || !lockedOut) return 7537;
    initialize(1);
    std::memset(&saved.targetGpu, 0, sizeof(saved.targetGpu));
    daemon_fan_ownership_handback_at_start(fixture);
    if (writes != 1 || writtenAdapter != 0 || !removed || !saved.targetGpu.valid) return 7538;
    initialize();
    DesiredSettings desired = {};
    desired.hasFan = true;
    desired.fanMode = FAN_MODE_CURVE;
    char error[128] = {};
    if (daemon_fan_ownership_ensure_before_write(fixture, &desired, error, sizeof(error))) return 7539;
    fixture.gpu.writeIdentityResolved = true;
    if (!daemon_fan_ownership_ensure_before_write(fixture, &desired, error, sizeof(error)) ||
        !linux_gpu_identity_matches(&saved.targetGpu, &fixture.gpu.selectedGpu)) return 7540;
    fixture.gpu.selectedGpu = fixture.gpu.adapters[1];
    fixture.gpu.selectedAdapterIndex = 1;
    if (daemon_fan_ownership_ensure_before_write(fixture, &desired, error, sizeof(error))) return 7541;
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
    daemon_fan_ownership_handback_at_start(fixture);
    const LinuxFanOwnershipMarker outstanding = saved;
    fixture.gpu.selectedAdapterIndex = 0;
    fixture.gpu.selectedGpu = fixture.gpu.adapters[0];
    if (daemon_fan_ownership_ensure_before_write(fixture, &desired, error, sizeof(error)) ||
        std::memcmp(&saved, &outstanding, sizeof(saved))) return 7560;
    if (daemon_fan_ownership_allows_selected_write(fixture, error, sizeof(error), true)) return 7561;
    daemon_fan_ownership_retire(fixture, "unrelated reset", true);
    if (removed || std::memcmp(&saved, &outstanding, sizeof(saved))) return 7561;
    if (daemon_fan_ownership_shutdown_should_release(fixture)) return 7562;
    fixture.gpu.selectedAdapterIndex = 1;
    fixture.gpu.selectedGpu = fixture.gpu.adapters[1];
    if (!daemon_fan_ownership_allows_selected_write(fixture, error, sizeof(error), true)) return 7563;
    daemon_fan_ownership_retire(fixture, "owned GPU reset", true);
    if (!removed || fixture.fanOwnershipMarkerCommitted || fixture.fanOwnershipHandbackPending) return 7563;

    initialize();
    autoFails = true;
    daemon_fan_ownership_handback_at_start(fixture);
    const int startupStores = stores;
    if (!daemon_fan_ownership_ensure_before_write(fixture, &desired, error, sizeof(error)) ||
        stores != startupStores + 1 || saved.handbackAttempts ||
        fixture.fanOwnershipHandbackPending || !daemon_fan_ownership_shutdown_should_release(fixture)) return 7564;

    initialize();
    saved.targetGpu.deviceId++;
    daemon_fan_ownership_handback_at_start(fixture);
    fixture.gpu.writeIdentityResolved = true;
    if (!fixture.fanOwnershipMarkerCommitted || !fixture.fanOwnershipHandbackPending ||
        daemon_fan_ownership_allows_selected_write(fixture, error, sizeof(error))) return 7565;
    // A recorded GPU missing from enumeration keeps the binding (even for an
    // explicit Reset elsewhere) and says how the user gets out of it.
    if (daemon_fan_ownership_allows_selected_write(fixture, error, sizeof(error), true) ||
        !std::strstr(error, "not present") || removed) return 7548;
    initialize();
    fixture.gpuReady = false;
    daemon_fan_ownership_handback_at_start(fixture);
    if (!fixture.fanOwnershipMarkerCommitted || !fixture.fanOwnershipHandbackPending ||
        daemon_fan_ownership_shutdown_should_release(fixture) || removed || writes) return 7566;

    initialize();
    saved.handbackAttempts = OWNERSHIP_HANDBACK_MAX_ATTEMPTS;
    daemon_fan_ownership_handback_at_start(fixture);
    if (writes || stores || removed || !lockedOut ||
        !fixture.fanOwnershipMarkerCommitted || !fixture.fanOwnershipHandbackPending) return 7567;

    initialize();
    saved.targetUuid = linux_gpu_uuid_parse("GPU-00000000-0000-0000-0000-000000000001");
    fixture.gpu.adapterUuids[1] = linux_gpu_uuid_parse("GPU-00000000-0000-0000-0000-000000000002");
    daemon_fan_ownership_handback_at_start(fixture);
    fixture.gpu.selectedAdapterIndex = 1;
    fixture.gpu.selectedGpu = fixture.gpu.adapters[1];
    fixture.gpu.writeIdentityResolved = true;
    if (daemon_fan_ownership_allows_selected_write(fixture, error, sizeof(error), true)) return 7568;

    initialize();
    saved.targetUuid = linux_gpu_uuid_parse("GPU-00000000-0000-0000-0000-000000000001");
    fixture.gpu.adapterUuids[0] = saved.targetUuid;
    autoFails = true;
    daemon_fan_ownership_handback_at_start(fixture);
    if (!daemon_fan_ownership_allows_selected_write(fixture, error, sizeof(error)) ||
        !linux_gpu_bdf_equal(&fixture.fanOwnershipRecord.targetGpu, &fixture.gpu.selectedGpu)) return 7569;

    initialize();
    saved.targetGpu = {};
    daemon_fan_ownership_handback_at_start(fixture);
    fixture.gpu.writeIdentityResolved = true;
    if (daemon_fan_ownership_allows_selected_write(fixture, error, sizeof(error)) ||
        !daemon_fan_ownership_allows_selected_write(fixture, error, sizeof(error), true)) return 7570;
    daemon_fan_ownership_retire(fixture, "explicit legacy recovery", true);
    if (!removed || fixture.fanOwnershipMarkerCommitted) return 7570;

    // Uncertainty idles the worker: an owned manual fan returns to the driver
    // first. Before this, the curve's last duty stayed pinned untracked.
    initialize();
    fixture.gpu.writeIdentityResolved = true;
    if (!daemon_fan_ownership_ensure_before_write(fixture, &desired, error, sizeof(error))) return 7580;
    writes = 0;
    daemon_enter_uncertain_state(fixture, "fixture apply rollback failed");
    if (!fixture.stateUncertain || writes != 1 || autoWrites != 1 || writtenAdapter != 0 ||
        !removed || fixture.fanOwnershipMarkerCommitted) return 7581;
    initialize();
    fixture.gpu.writeIdentityResolved = true;
    if (!daemon_fan_ownership_ensure_before_write(fixture, &desired, error, sizeof(error))) return 7582;
    writes = autoWrites = 0;
    autoFails = true;
    daemon_enter_uncertain_state(fixture, "fixture refused handback");
    if (writes != 2 || autoWrites != 1 || emergencyPercent != FAN_RUNTIME_EMERGENCY_PERCENT ||
        removed || !fixture.fanOwnershipMarkerCommitted) return 7583;
    initialize();
    fixture.gpu.writeIdentityResolved = true;
    daemon_enter_uncertain_state(fixture, "fixture without manual fan");
    if (!fixture.stateUncertain || writes || removed) return 7584;
    initialize();
    fixture.gpu.writeIdentityResolved = true;
    if (!daemon_fan_ownership_ensure_before_write(fixture, &desired, error, sizeof(error))) return 7585;
    writes = 0;
    fixture.gpu.selectedGpu = fixture.gpu.adapters[1];
    fixture.gpu.selectedAdapterIndex = 1;
    daemon_enter_uncertain_state(fixture, "fixture other adapter selected");
    if (writes || removed || !fixture.fanOwnershipMarkerCommitted) return 7586;
    initialize();
    fixture.gpu.writeIdentityResolved = true;
    if (!daemon_fan_ownership_ensure_before_write(fixture, &desired, error, sizeof(error))) return 7587;
    writes = 0;
    fixture.gpuReady = false;
    daemon_enter_uncertain_state(fixture, "fixture backend unavailable");
    if (writes || removed || !fixture.fanOwnershipMarkerCommitted) return 7588;
    // The fan worker's own lockout already wrote the fan: uncertainty follows
    // without a second write, and a fan the driver took back is not owned.
    initialize();
    fixture.gpu.writeIdentityResolved = true;
    if (!daemon_fan_ownership_ensure_before_write(fixture, &desired, error, sizeof(error))) return 7549;
    writes = 0;
    daemon_enter_uncertain_after_fan_lockout(fixture, "fixture worker lockout", true);
    if (!fixture.stateUncertain || writes || !removed || fixture.fanOwnershipMarkerCommitted) return 7549;
    initialize();
    fixture.gpu.writeIdentityResolved = true;
    if (!daemon_fan_ownership_ensure_before_write(fixture, &desired, error, sizeof(error))) return 7549;
    writes = 0;
    daemon_enter_uncertain_after_fan_lockout(fixture, "fixture worker lockout refused", false);
    if (!fixture.stateUncertain || writes || removed || !fixture.fanOwnershipMarkerCommitted) return 7549;
    // Inherited recovery on its own adapter is returned too.
    initialize();
    autoFails = true;
    daemon_fan_ownership_handback_at_start(fixture);
    autoFails = false;
    writes = autoWrites = 0;
    daemon_enter_uncertain_state(fixture, "fixture inherited ownership");
    if (autoWrites != 1 || writtenAdapter != 1 || fixture.fanOwnershipMarkerCommitted ||
        fixture.fanOwnershipHandbackPending) return 7589;

    // A handback deferred for an unusable backend runs once the backend comes
    // up later in the same process. Before, it waited for the next start.
    initialize();
    fixture.gpuReady = false;
    daemon_fan_ownership_handback_at_start(fixture);
    if (writes || daemon_fan_ownership_resume_deferred_handback(fixture)) return 7595;
    fixture.gpuReady = true;
    if (!daemon_fan_ownership_resume_deferred_handback(fixture) || writes != 1 ||
        writtenAdapter != 1 || !removed || fixture.fanOwnershipMarkerCommitted) return 7596;
    if (daemon_fan_ownership_resume_deferred_handback(fixture) || writes != 1) return 7597;
    initialize();
    fixture.gpuReady = false;
    daemon_fan_ownership_handback_at_start(fixture);
    fixture.gpuReady = true;
    fixture.gpu.writeIdentityResolved = true;
    fixture.gpu.selectedAdapterIndex = 1;
    fixture.gpu.selectedGpu = fixture.gpu.adapters[1];
    if (!daemon_fan_ownership_ensure_before_write(fixture, &desired, error, sizeof(error))) return 7598;
    writes = 0;
    if (daemon_fan_ownership_resume_deferred_handback(fixture) || writes) return 7599;

    // 14230-14233: two GPU controllers are independent. They select different
    // adapters (fixture: 1, other: 0), so a fan write reaching the wrong card
    // is visible. Uncertainty entered on one changes only its own state and
    // hands back only the fan it owns, on its own adapter.
    initialize();
    static LinuxGpuController other;
    linux_gpu_controller_init(&other);
    other.gpu = fixture.gpu;
    other.gpu.writeIdentityResolved = true;
    other.gpu.selectedAdapterIndex = 0;
    other.gpu.selectedGpu = other.gpu.adapters[0];
    other.gpuReady = true;
    fixture.gpu.writeIdentityResolved = true;
    fixture.gpu.selectedAdapterIndex = 1;
    fixture.gpu.selectedGpu = fixture.gpu.adapters[1];
    fixture.gpuReady = true;
    fixture.stateUncertain = false;
    fixture.fanOwnershipMarkerCommitted = true;
    fixture.fanOwnershipRecord.targetGpu = fixture.gpu.adapters[1];
    fixture.fanOwnershipRecord.targetLocationRole = GPU_LOCATION_ROLE_REQUIRED;
    writes = 0;
    writtenAdapter = -1;
    daemon_enter_uncertain_state(other, "fixture: other GPU owns no fan");
    if (!other.stateUncertain || fixture.stateUncertain || writes ||
        !fixture.fanOwnershipMarkerCommitted) return 14230;
    other.stateUncertain = false;
    other.fanOwnershipMarkerCommitted = true;
    other.fanOwnershipRecord.targetGpu = other.gpu.adapters[0];
    other.fanOwnershipRecord.targetLocationRole = GPU_LOCATION_ROLE_REQUIRED;
    removed = false;
    daemon_enter_uncertain_state(other, "fixture: other GPU owns its fan");
    if (writes != 1 || writtenAdapter != 0 || !removed ||
        other.fanOwnershipMarkerCommitted) return 14231;
    if (fixture.stateUncertain || !fixture.fanOwnershipMarkerCommitted) return 14232;
    other.fanOwnershipMarkerCommitted = true;
    writes = 0;
    writtenAdapter = -1;
    daemon_enter_uncertain_state(fixture, "fixture: first GPU owns its fan");
    if (!fixture.stateUncertain || writes != 1 || writtenAdapter != 1 ||
        !other.fanOwnershipMarkerCommitted || !other.stateUncertain) return 14233;
    return 0;
}
} // namespace linux_fan_ownership_fixture

int run_linux_fan_ownership_tests() {
    linux_gpu_controller_init(&linux_fan_ownership_fixture::fixture);
    int result = linux_fan_ownership_fixture::run();
    if (result) std::fprintf(stderr, "regression assertion failed: %d\n", result);
    return result;
}
#ifdef GC_FAN_OWNERSHIP_FIXTURE_MAIN
int main() { return run_linux_fan_ownership_tests(); }
#endif
