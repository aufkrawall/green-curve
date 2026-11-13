// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_LINUX_DAEMON_AUTHORING_SCHEMA_H
#define GREEN_CURVE_LINUX_DAEMON_AUTHORING_SCHEMA_H

// FROZEN exact-scalar records preceding bound authoring intent.
struct LinuxDaemonStateRecordSchema6 {
    gc_u32 magic;
    gc_u32 version;
    gc_u32 size;
    gc_u32 state;
    GpuAdapterInfo targetGpu;
    DesiredSettingsSchema4 desired;
    gc_u64 operationId;
    gc_u32 operationState;
    gc_u32 targetLocationRole; // GpuLocationRole, part of write authority
    LinuxGpuUuid targetUuid; // optional physical-device identity
    gc_u32 checksum;
};
static_assert(sizeof(LinuxDaemonStateRecordSchema6) == 2472, "the pre-authoring layout is frozen");

struct LinuxDaemonStartupRecordSchema6 {
    gc_u32 magic;
    gc_u32 version;
    gc_u32 size;
    gc_u32 mode;        // ServiceStartupPolicyMode
    gc_u32 profileSlot; // 1..CONFIG_NUM_SLOTS when mode == PROFILE, else 0
    gc_u32 reserved;
    char profileName[LINUX_DAEMON_STARTUP_NAME_MAX];
    GpuAdapterInfo targetGpu;
    DesiredSettingsSchema4 desired;
    gc_u32 targetLocationRole; // GpuLocationRole, preserved by content refresh
    LinuxGpuUuid targetUuid; // also preserved by content refresh
    gc_u32 checksum;
};
static_assert(sizeof(LinuxDaemonStartupRecordSchema6) == 2528, "the pre-authoring layout is frozen");

static inline bool linux_daemon_state_record_widen_schema6(
    LinuxDaemonStateRecord* out, const LinuxDaemonStateRecordSchema6* in) {
    if (!out || !in || in->magic != LINUX_DAEMON_RECORD_MAGIC ||
        in->size != sizeof(*in) ||
        in->version != LINUX_DAEMON_RECORD_PRE_AUTHORING_VERSION ||
        in->state < LINUX_DAEMON_RECORD_PREPARED ||
        in->state > LINUX_DAEMON_RECORD_UNCERTAIN ||
        in->operationState > SERVICE_OPERATION_OUTCOME_UNKNOWN ||
        in->targetLocationRole > GPU_LOCATION_ROLE_REQUIRED ||
        in->checksum != linux_daemon_record_hash_bytes(in,
            offsetof(LinuxDaemonStateRecordSchema6, checksum))) return false;
    DesiredSettings desired = {};
    desired_settings_widen_schema4(&desired, &in->desired);
    GpuLocationRole role = (GpuLocationRole)in->targetLocationRole;
    linux_daemon_record_initialize(out, (LinuxDaemonRecordState)in->state,
        &in->targetGpu, &desired, in->operationId, in->operationState, role, &in->targetUuid);
    return linux_daemon_record_valid(out);
}

static inline bool linux_daemon_startup_widen_schema6(
    LinuxDaemonStartupRecord* out, const LinuxDaemonStartupRecordSchema6* in) {
    if (!out || !in || in->magic != LINUX_DAEMON_STARTUP_MAGIC ||
        in->size != sizeof(*in) ||
        in->version != LINUX_DAEMON_STARTUP_PRE_AUTHORING_VERSION ||
        in->mode >= SERVICE_STARTUP_POLICY_MODE_COUNT ||
        in->targetLocationRole > GPU_LOCATION_ROLE_REQUIRED ||
        in->checksum != linux_daemon_record_hash_bytes(in,
            offsetof(LinuxDaemonStartupRecordSchema6, checksum)) ||
        !service_wire_string_is_terminated(in->profileName, sizeof(in->profileName)))
        return false;
    if (in->mode == SERVICE_STARTUP_POLICY_PROFILE) {
        if (in->profileSlot < 1 || in->profileSlot > (gc_u32)CONFIG_NUM_SLOTS ||
            !in->targetGpu.valid || !in->targetGpu.pciInfoValid) return false;
    } else if (in->profileSlot || in->targetGpu.valid ||
               in->targetLocationRole != GPU_LOCATION_ROLE_UNRECORDED) return false;
    DesiredSettings desired = {};
    desired_settings_widen_schema4(&desired, &in->desired);
    GpuLocationRole role = (GpuLocationRole)in->targetLocationRole;
    linux_daemon_startup_initialize(out, in->mode, in->profileSlot,
        in->profileName, &in->targetGpu, &desired, role, &in->targetUuid);
    return linux_daemon_startup_valid(out);
}
#endif
