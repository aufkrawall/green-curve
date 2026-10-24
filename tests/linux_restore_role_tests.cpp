// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Durable restore authority and pre-role layout migration (7420-7429,
// 7440-7459).
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
        widened.targetLocationRole != GPU_LOCATION_ROLE_UNRECORDED ||
        widened.operationId != 42 || widened.operationState != SERVICE_OPERATION_SUCCEEDED ||
        memcmp(&widened.desired, &desired, sizeof(desired)) != 0) return 7423;
    // A legacy record never said whether its location told twins apart, so
    // it keeps the role-less rule: the exact location wins, a lone adapter is
    // followed to its new bus, and a move among several stays unresolved.
    const GpuAdapterInfo moved[] = {adapter(6)};
    const GpuAdapterInfo exact[] = {adapter(7)};
    const GpuAdapterInfo twinsElsewhere[] = {adapter(6), adapter(8)};
    const GpuAdapterInfo twinsExact[] = {adapter(6), adapter(7)};
    const GpuLocationRole widenedRole = (GpuLocationRole)widened.targetLocationRole;
    const LinuxGpuInitSelection followed =
        linux_gpu_init_selection(&widened.targetGpu, moved, 1, widenedRole);
    const LinuxGpuInitSelection exactPick =
        linux_gpu_init_selection(&widened.targetGpu, twinsExact, 2, widenedRole);
    if (!followed.writeResolved || !followed.moved || followed.index != 0 ||
        !linux_gpu_init_selection(&widened.targetGpu, exact, 1, widenedRole).writeResolved ||
        linux_gpu_init_selection(&widened.targetGpu, twinsElsewhere, 2,
            widenedRole).writeResolved ||
        !exactPick.writeResolved || exactPick.index != 1) return 7424;
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
        migrated.targetLocationRole != GPU_LOCATION_ROLE_UNRECORDED ||
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
// The reported upgrade regression: a boot profile bound before the role was
// persisted, on a single GPU that firmware has since moved from bus 7 to bus
// 6. It must boot on the moved card and re-bind with the role a selection on
// the live adapter list records.
int legacy_startup_on_moved_gpu() {
    const DesiredSettings desired = intent();
    LinuxDaemonStartupRecordSchema2 boot = {};
    boot.magic = LINUX_DAEMON_STARTUP_MAGIC;
    boot.version = LINUX_DAEMON_STARTUP_PRE_LOCATION_ROLE_VERSION;
    boot.size = sizeof(boot);
    boot.mode = SERVICE_STARTUP_POLICY_PROFILE;
    boot.profileSlot = 1;
    boot.targetGpu = adapter(7);
    memcpy(boot.profileName, "profile 1", 10);
    desired_settings_narrow_schema2(&boot.desired, &desired);
    boot.checksum = linux_daemon_record_hash_bytes(&boot, offsetof(LinuxDaemonStartupRecordSchema2, checksum));
    LinuxDaemonStartupRecord migrated = {};
    if (!linux_daemon_startup_widen_schema2(&migrated, &boot)) return 7440;
    const GpuAdapterInfo live[] = {adapter(6)};
    const LinuxGpuInitSelection selection = linux_gpu_init_selection(&migrated.targetGpu,
        live, 1, (GpuLocationRole)migrated.targetLocationRole);
    if (!selection.writeResolved || selection.index != 0 || !selection.moved) return 7441;
    if (linux_gpu_location_role_for_selection(&live[0], live, 1) != GPU_LOCATION_ROLE_ADVISORY)
        return 7442;
    return 0;
}

// The 0.25.x schema-1 layouts reach the current record through their own
// widening paths; none of them may pick up a stricter role than the record had.
int schema1_records_stay_unrecorded() {
    const DesiredSettings desired = intent();
    LinuxDaemonStateRecordSchema1 state = {};
    state.magic = LINUX_DAEMON_RECORD_MAGIC;
    state.version = LINUX_DAEMON_RECORD_PRE_PROVENANCE_VERSION;
    state.size = sizeof(state);
    state.state = LINUX_DAEMON_RECORD_ACTIVE;
    state.targetGpu = adapter(7);
    state.operationId = 9;
    state.operationState = SERVICE_OPERATION_SUCCEEDED;
    desired_settings_narrow_to_schema1(&state.desired, &desired);
    state.checksum = linux_daemon_record_hash_bytes(&state, offsetof(LinuxDaemonStateRecordSchema1, checksum));
    LinuxDaemonStateRecord wide = {};
    if (!linux_daemon_state_record_widen_schema1(&wide, &state, nullptr, nullptr) ||
        !linux_daemon_record_valid(&wide) ||
        wide.targetLocationRole != GPU_LOCATION_ROLE_UNRECORDED || wide.operationId != 9)
        return 7443;

    LinuxDaemonStateRecordSchema1V1 first = {};
    first.magic = LINUX_DAEMON_RECORD_MAGIC;
    first.version = LINUX_DAEMON_RECORD_PRE_OPERATION_ID_VERSION;
    first.size = sizeof(first);
    first.state = LINUX_DAEMON_RECORD_ACTIVE;
    first.targetGpu = adapter(7);
    desired_settings_narrow_to_schema1(&first.desired, &desired);
    first.checksum = linux_daemon_record_hash_bytes(&first, offsetof(LinuxDaemonStateRecordSchema1V1, checksum));
    if (!linux_daemon_state_record_widen_schema1_v1(&wide, &first, nullptr) ||
        !linux_daemon_record_valid(&wide) ||
        wide.targetLocationRole != GPU_LOCATION_ROLE_UNRECORDED ||
        wide.operationId != 0 || wide.operationState != SERVICE_OPERATION_NONE) return 7444;

    LinuxDaemonStartupRecordSchema1 boot = {};
    boot.magic = LINUX_DAEMON_STARTUP_MAGIC;
    boot.version = LINUX_DAEMON_STARTUP_PRE_PROVENANCE_VERSION;
    boot.size = sizeof(boot);
    boot.mode = SERVICE_STARTUP_POLICY_PROFILE;
    boot.profileSlot = 3;
    boot.targetGpu = adapter(7);
    memcpy(boot.profileName, "profile 3", 10);
    desired_settings_narrow_to_schema1(&boot.desired, &desired);
    boot.checksum = linux_daemon_record_hash_bytes(&boot, offsetof(LinuxDaemonStartupRecordSchema1, checksum));
    LinuxDaemonStartupRecord startup = {};
    if (!linux_daemon_startup_widen_schema1(&startup, &boot, nullptr, nullptr) ||
        !linux_daemon_startup_valid(&startup) ||
        startup.targetLocationRole != GPU_LOCATION_ROLE_UNRECORDED) return 7445;
    return 0;
}

// The aborted-restore log names what makes the next automatic write succeed.
int rebind_hint() {
    if (!strstr(linux_auto_restore_rebind_hint(LINUX_AUTO_RESTORE_TRIGGER_BOOT_PROFILE, true),
                "--startup-profile") ||
        !strstr(linux_auto_restore_rebind_hint(LINUX_AUTO_RESTORE_TRIGGER_BOOT_RESTORE_LAST, true),
                "apply") ||
        !strstr(linux_auto_restore_rebind_hint(LINUX_AUTO_RESTORE_TRIGGER_RESUME, true), "apply"))
        return 7446;
    // A backend that was not ready is not a binding problem.
    if (strncmp(linux_auto_restore_rebind_hint(LINUX_AUTO_RESTORE_TRIGGER_BOOT_PROFILE, false),
                "none", 4) != 0) return 7447;
    return 0;
}
// The first role generation (state v5, startup v4) as an unreleased build
// left it on disk after widening a pre-role record as `required`: the same
// layout, re-stamped with the old version and its checksum.
LinuxDaemonStateRecord first_generation_state(GpuLocationRole role) {
    const GpuAdapterInfo selected = adapter(7);
    const DesiredSettings desired = intent();
    LinuxDaemonStateRecord record = {};
    linux_daemon_record_initialize(&record, LINUX_DAEMON_RECORD_ACTIVE,
        &selected, &desired, 77, SERVICE_OPERATION_SUCCEEDED, role);
    record.version = LINUX_DAEMON_RECORD_PRE_TRUSTED_ROLE_VERSION;
    record.checksum = linux_daemon_record_checksum(&record);
    return record;
}
LinuxDaemonStartupRecord first_generation_startup(GpuLocationRole role) {
    const GpuAdapterInfo selected = adapter(7);
    const DesiredSettings desired = intent();
    LinuxDaemonStartupRecord record = {};
    linux_daemon_startup_initialize(&record, SERVICE_STARTUP_POLICY_PROFILE,
        1, "profile 1", &selected, &desired, role);
    record.version = LINUX_DAEMON_STARTUP_PRE_TRUSTED_ROLE_VERSION;
    record.checksum = linux_daemon_startup_checksum(&record);
    return record;
}

// Records already rewritten by that build must boot on the moved GPU after
// the update, without the user binding anything again.
int first_generation_roles_reread() {
    const DesiredSettings desired = intent();
    const GpuAdapterInfo moved[] = {adapter(6)};
    const GpuAdapterInfo twinsExact[] = {adapter(6), adapter(7)};

    LinuxDaemonStartupRecord boot = first_generation_startup(GPU_LOCATION_ROLE_REQUIRED);
    if (linux_daemon_startup_valid(&boot)) return 7450; // not current as stored
    if (!linux_daemon_startup_reread_pre_trusted_role(&boot) ||
        !linux_daemon_startup_valid(&boot) ||
        boot.version != LINUX_DAEMON_STARTUP_VERSION ||
        boot.targetLocationRole != GPU_LOCATION_ROLE_UNRECORDED ||
        boot.profileSlot != 1 || strcmp(boot.profileName, "profile 1") != 0 ||
        boot.targetGpu.pciBus != 7 ||
        memcmp(&boot.desired, &desired, sizeof(desired)) != 0) return 7451;
    const LinuxGpuInitSelection bootSelection = linux_gpu_init_selection(&boot.targetGpu,
        moved, 1, (GpuLocationRole)boot.targetLocationRole);
    if (!bootSelection.writeResolved || !bootSelection.moved) return 7452;
    // Twins still present keep the exact location under the re-read role.
    const LinuxGpuInitSelection twinPick = linux_gpu_init_selection(&boot.targetGpu,
        twinsExact, 2, (GpuLocationRole)boot.targetLocationRole);
    if (!twinPick.writeResolved || twinPick.index != 1) return 7453;

    LinuxDaemonStateRecord active = first_generation_state(GPU_LOCATION_ROLE_REQUIRED);
    if (linux_daemon_record_valid(&active)) return 7454;
    if (!linux_daemon_state_record_reread_pre_trusted_role(&active) ||
        !linux_daemon_record_valid(&active) ||
        active.version != LINUX_DAEMON_RECORD_VERSION ||
        active.targetLocationRole != GPU_LOCATION_ROLE_UNRECORDED ||
        active.state != LINUX_DAEMON_RECORD_ACTIVE || active.operationId != 77 ||
        active.operationState != SERVICE_OPERATION_SUCCEEDED ||
        memcmp(&active.desired, &desired, sizeof(desired)) != 0 ||
        !linux_gpu_init_selection(&active.targetGpu, moved, 1,
            (GpuLocationRole)active.targetLocationRole).writeResolved) return 7455;

    // An advisory role was recorded from enumeration, never invented.
    LinuxDaemonStartupRecord advisory = first_generation_startup(GPU_LOCATION_ROLE_ADVISORY);
    LinuxDaemonStateRecord advisoryState = first_generation_state(GPU_LOCATION_ROLE_ADVISORY);
    if (!linux_daemon_startup_reread_pre_trusted_role(&advisory) ||
        advisory.targetLocationRole != GPU_LOCATION_ROLE_ADVISORY ||
        !linux_daemon_state_record_reread_pre_trusted_role(&advisoryState) ||
        advisoryState.targetLocationRole != GPU_LOCATION_ROLE_ADVISORY) return 7456;

    // A current-version `required` is a real twin selection: never re-read.
    LinuxDaemonStartupRecord current = {};
    linux_daemon_startup_initialize(&current, SERVICE_STARTUP_POLICY_PROFILE, 1,
        "profile 1", &boot.targetGpu, &desired, GPU_LOCATION_ROLE_REQUIRED);
    LinuxDaemonStateRecord currentState = {};
    linux_daemon_record_initialize(&currentState, LINUX_DAEMON_RECORD_ACTIVE,
        &boot.targetGpu, &desired, 1, SERVICE_OPERATION_SUCCEEDED, GPU_LOCATION_ROLE_REQUIRED);
    if (linux_daemon_startup_reread_pre_trusted_role(&current) ||
        current.targetLocationRole != GPU_LOCATION_ROLE_REQUIRED ||
        linux_daemon_state_record_reread_pre_trusted_role(&currentState) ||
        currentState.targetLocationRole != GPU_LOCATION_ROLE_REQUIRED ||
        linux_gpu_init_selection(&current.targetGpu, moved, 1,
            GPU_LOCATION_ROLE_REQUIRED).writeResolved) return 7457;

    // Damaged first-generation bytes are refused, not re-stamped as current.
    LinuxDaemonStartupRecord tampered = first_generation_startup(GPU_LOCATION_ROLE_REQUIRED);
    tampered.desired.gpuOffsetMHz++;
    LinuxDaemonStateRecord tamperedState = first_generation_state(GPU_LOCATION_ROLE_REQUIRED);
    tamperedState.operationId++;
    if (linux_daemon_startup_reread_pre_trusted_role(&tampered) ||
        linux_daemon_state_record_reread_pre_trusted_role(&tamperedState)) return 7458;
    // A disabled policy of that generation still loads, and stays disabled.
    LinuxDaemonStartupRecord none = {};
    linux_daemon_startup_initialize(&none, SERVICE_STARTUP_POLICY_NONE, 0, "",
        nullptr, nullptr, GPU_LOCATION_ROLE_UNRECORDED);
    none.version = LINUX_DAEMON_STARTUP_PRE_TRUSTED_ROLE_VERSION;
    none.checksum = linux_daemon_startup_checksum(&none);
    if (!linux_daemon_startup_reread_pre_trusted_role(&none) ||
        none.mode != SERVICE_STARTUP_POLICY_NONE || none.targetGpu.valid) return 7459;
    return 0;
}
} // namespace

int run_linux_restore_role_tests() {
    if (int result = current_records()) return result;
    if (int result = legacy_records()) return result;
    if (int result = legacy_startup_on_moved_gpu()) return result;
    if (int result = schema1_records_stay_unrecorded()) return result;
    if (int result = first_generation_roles_reread()) return result;
    return rebind_hint();
}
