// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_LINUX_DAEMON_UUID_SCHEMA_H
#define GREEN_CURVE_LINUX_DAEMON_UUID_SCHEMA_H

// FROZEN location-role layouts before the optional UUID. The first role
// generation keeps its compatibility re-read; the next keeps its exact role.
struct LinuxDaemonStateRecordSchema3 {
    gc_u32 magic, version, size, state;
    GpuAdapterInfo targetGpu;
    DesiredSettingsSchema2 desired;
    gc_u64 operationId;
    gc_u32 operationState, targetLocationRole, checksum;
};
static_assert(sizeof(LinuxDaemonStateRecordSchema3) == 1184,
              "the pre-UUID state layout is frozen");

struct LinuxDaemonStartupRecordSchema3 {
    gc_u32 magic, version, size, mode, profileSlot, reserved;
    char profileName[LINUX_DAEMON_STARTUP_NAME_MAX];
    GpuAdapterInfo targetGpu;
    DesiredSettingsSchema2 desired;
    gc_u32 targetLocationRole, checksum;
};
static_assert(sizeof(LinuxDaemonStartupRecordSchema3) == 1240,
              "the pre-UUID startup layout is frozen");

static inline bool linux_daemon_state_record_widen_schema3(
    LinuxDaemonStateRecord* out, const LinuxDaemonStateRecordSchema3* in) {
    if (!out || !in || in->magic != LINUX_DAEMON_RECORD_MAGIC ||
        in->size != sizeof(*in) ||
        (in->version != LINUX_DAEMON_RECORD_PRE_TRUSTED_ROLE_VERSION &&
         in->version != LINUX_DAEMON_RECORD_PRE_UUID_VERSION) ||
        in->state < LINUX_DAEMON_RECORD_PREPARED ||
        in->state > LINUX_DAEMON_RECORD_UNCERTAIN ||
        in->operationState > SERVICE_OPERATION_OUTCOME_UNKNOWN ||
        in->targetLocationRole > GPU_LOCATION_ROLE_REQUIRED ||
        in->checksum != linux_daemon_record_hash_bytes(in,
            offsetof(LinuxDaemonStateRecordSchema3, checksum))) return false;
    DesiredSettings desired = {};
    desired_settings_widen_schema2(&desired, &in->desired);
    GpuLocationRole role = in->version == LINUX_DAEMON_RECORD_PRE_TRUSTED_ROLE_VERSION
        ? linux_daemon_pre_trusted_role_reread(in->targetLocationRole)
        : (GpuLocationRole)in->targetLocationRole;
    linux_daemon_record_initialize(out, (LinuxDaemonRecordState)in->state,
        &in->targetGpu, &desired, in->operationId, in->operationState, role);
    return linux_daemon_record_valid(out);
}

static inline bool linux_daemon_startup_widen_schema3(
    LinuxDaemonStartupRecord* out, const LinuxDaemonStartupRecordSchema3* in) {
    if (!out || !in || in->magic != LINUX_DAEMON_STARTUP_MAGIC ||
        in->size != sizeof(*in) ||
        (in->version != LINUX_DAEMON_STARTUP_PRE_TRUSTED_ROLE_VERSION &&
         in->version != LINUX_DAEMON_STARTUP_PRE_UUID_VERSION) ||
        in->mode >= SERVICE_STARTUP_POLICY_MODE_COUNT ||
        in->targetLocationRole > GPU_LOCATION_ROLE_REQUIRED ||
        in->checksum != linux_daemon_record_hash_bytes(in,
            offsetof(LinuxDaemonStartupRecordSchema3, checksum)) ||
        !service_wire_string_is_terminated(in->profileName, sizeof(in->profileName)))
        return false;
    if (in->mode == SERVICE_STARTUP_POLICY_PROFILE) {
        if (in->profileSlot < 1 || in->profileSlot > (gc_u32)CONFIG_NUM_SLOTS ||
            !in->targetGpu.valid || !in->targetGpu.pciInfoValid) return false;
    } else if (in->profileSlot || in->targetGpu.valid ||
               in->targetLocationRole != GPU_LOCATION_ROLE_UNRECORDED) return false;
    DesiredSettings desired = {};
    desired_settings_widen_schema2(&desired, &in->desired);
    GpuLocationRole role = in->version == LINUX_DAEMON_STARTUP_PRE_TRUSTED_ROLE_VERSION
        ? linux_daemon_pre_trusted_role_reread(in->targetLocationRole)
        : (GpuLocationRole)in->targetLocationRole;
    linux_daemon_startup_initialize(out, in->mode, in->profileSlot,
        in->profileName, &in->targetGpu, &desired, role);
    return linux_daemon_startup_valid(out);
}
#endif
