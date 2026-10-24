// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Durable restore authority and pre-role layout migration (7420-7429).
#include <initializer_list>
#include "linux_daemon_state.h"
#include "linux_gpu_selection.h"

namespace {
GpuAdapterInfo adapter(unsigned int bus) {
    GpuAdapterInfo gpu = {};
    gpu.valid = true;
    gpu.pciInfoValid = true;
    gpu.deviceId = 0x2f0410deu;
    gpu.subSystemId = 0x123410deu;
    gpu.pciBus = bus;
    return gpu;
}
DesiredSettings intent() {
    DesiredSettings d = {};
    d.hasCurvePoint[74] = true;
    d.curvePointMHz[74] = 2700;
    d.curvePointFromGpuOffset[74] = true;
    d.hasGpuOffset = true;
    d.gpuOffsetMHz = 100;
    d.hasMemOffset = true;
    d.memOffsetMHz = -123;
    d.hasLock = true;
    d.lockCi = 74;
    d.lockMHz = 2700;
    d.lockMode = LOCK_MODE_HARD;
    d.hasVideoClkOffsetKhz = true;
    d.videoClkOffsetKhz = 15000;
    return d;
}
int current_records() {
    const GpuAdapterInfo selected = adapter(7);
    const GpuAdapterInfo live[] = {adapter(6)};
    const DesiredSettings desired = intent();
    for (GpuLocationRole role : {GPU_LOCATION_ROLE_REQUIRED, GPU_LOCATION_ROLE_ADVISORY}) {
        LinuxDaemonStateRecord active = {};
        LinuxDaemonStartupRecord startup = {};
        linux_daemon_record_initialize(&active, LINUX_DAEMON_RECORD_ACTIVE,
            &selected, &desired, 42, SERVICE_OPERATION_SUCCEEDED, role);
        linux_daemon_startup_initialize(&startup, SERVICE_STARTUP_POLICY_PROFILE,
            1, "test", &selected, &desired, role);
        if (!linux_daemon_record_valid(&active) || !linux_daemon_startup_valid(&startup) ||
            active.targetLocationRole != (gc_u32)role ||
            startup.targetLocationRole != (gc_u32)role) return 7420;
        // This is the backend init decision, after either restore record was read.
        // A selected twin removed from bus 7 must never hand its intent to bus 6.
        const bool allowed = role == GPU_LOCATION_ROLE_ADVISORY;
        if (linux_gpu_init_selection(&active.targetGpu, live, 1,
                (GpuLocationRole)active.targetLocationRole).writeResolved != allowed ||
            linux_gpu_init_selection(&startup.targetGpu, live, 1,
                (GpuLocationRole)startup.targetLocationRole).writeResolved != allowed)
            return 7421;
        active.targetLocationRole = role == GPU_LOCATION_ROLE_REQUIRED
            ? GPU_LOCATION_ROLE_ADVISORY : GPU_LOCATION_ROLE_REQUIRED;
        startup.targetLocationRole = active.targetLocationRole;
        if (linux_daemon_record_valid(&active) || linux_daemon_startup_valid(&startup))
            return 7422; // The checksum must cover the location role.
        active.targetLocationRole = 99;
        startup.targetLocationRole = 99;
        active.checksum = linux_daemon_record_checksum(&active);
        startup.checksum = linux_daemon_startup_checksum(&startup);
        if (linux_daemon_record_valid(&active) || linux_daemon_startup_valid(&startup))
            return 7422;
    }
    return 0;
}
int legacy_records() {
    const DesiredSettings desired = intent();
    LinuxDaemonStateRecordSchema2 old = {};
    old.magic = LINUX_DAEMON_RECORD_MAGIC;
    old.version = LINUX_DAEMON_RECORD_PRE_LOCATION_ROLE_VERSION;
    old.size = sizeof(old);
    old.state = LINUX_DAEMON_RECORD_ACTIVE;
    old.targetGpu = adapter(7);
    old.operationId = 42;
    old.operationState = SERVICE_OPERATION_SUCCEEDED;
    desired_settings_narrow_schema2(&old.desired, &desired);
    old.checksum = linux_daemon_record_hash_bytes(&old, offsetof(LinuxDaemonStateRecordSchema2, checksum));
    LinuxDaemonStateRecord widened = {};
    if (!linux_daemon_state_record_widen_schema2(&widened, &old) ||
        !linux_daemon_record_valid(&widened) ||
        widened.targetLocationRole != GPU_LOCATION_ROLE_REQUIRED ||
        widened.operationId != 42 || widened.operationState != SERVICE_OPERATION_SUCCEEDED ||
        memcmp(&widened.desired, &desired, sizeof(desired)) != 0) return 7423;
    const GpuAdapterInfo remaining[] = {adapter(6)};
    const GpuAdapterInfo exact[] = {adapter(7)};
    if (linux_gpu_init_selection(&widened.targetGpu, remaining, 1,
            (GpuLocationRole)widened.targetLocationRole).writeResolved ||
        !linux_gpu_init_selection(&widened.targetGpu, exact, 1,
            (GpuLocationRole)widened.targetLocationRole).writeResolved) return 7424;
    old.desired.memOffsetMHz++;
    if (linux_daemon_state_record_widen_schema2(&widened, &old)) return 7425;

    LinuxDaemonStartupRecordSchema2 boot = {};
    boot.magic = LINUX_DAEMON_STARTUP_MAGIC;
    boot.version = LINUX_DAEMON_STARTUP_PRE_LOCATION_ROLE_VERSION;
    boot.size = sizeof(boot);
    boot.mode = SERVICE_STARTUP_POLICY_PROFILE;
    boot.profileSlot = 2;
    boot.targetGpu = adapter(7);
    memcpy(boot.profileName, "profile", 8);
    desired_settings_narrow_schema2(&boot.desired, &desired);
    boot.checksum = linux_daemon_record_hash_bytes(&boot, offsetof(LinuxDaemonStartupRecordSchema2, checksum));
    LinuxDaemonStartupRecord migrated = {};
    if (!linux_daemon_startup_widen_schema2(&migrated, &boot) ||
        !linux_daemon_startup_valid(&migrated) || migrated.profileSlot != 2 ||
        strcmp(migrated.profileName, "profile") != 0 ||
        migrated.targetLocationRole != GPU_LOCATION_ROLE_REQUIRED ||
        memcmp(&migrated.desired, &desired, sizeof(desired)) != 0) return 7426;
    boot.version++;
    boot.checksum = linux_daemon_record_hash_bytes(&boot, offsetof(LinuxDaemonStartupRecordSchema2, checksum));
    if (linux_daemon_startup_widen_schema2(&migrated, &boot)) return 7427;
    boot.version--;
    boot.mode = SERVICE_STARTUP_POLICY_NONE;
    boot.checksum = linux_daemon_record_hash_bytes(&boot, offsetof(LinuxDaemonStartupRecordSchema2, checksum));
    if (linux_daemon_startup_widen_schema2(&migrated, &boot)) return 7428;
    boot.profileSlot = 0;
    boot.targetGpu = {};
    boot.checksum = linux_daemon_record_hash_bytes(&boot, offsetof(LinuxDaemonStartupRecordSchema2, checksum));
    if (!linux_daemon_startup_widen_schema2(&migrated, &boot) ||
        !linux_daemon_startup_valid(&migrated) ||
        migrated.targetLocationRole != GPU_LOCATION_ROLE_UNRECORDED ||
        migrated.desired.hasCurvePoint[74]) return 7429;
    return 0;
}
} // namespace

int run_linux_restore_role_tests() {
    if (int result = current_records()) return result;
    return legacy_records();
}
