// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_LINUX_DAEMON_LOCATION_SCHEMA_H
#define GREEN_CURVE_LINUX_DAEMON_LOCATION_SCHEMA_H

// FROZEN pre-location-role records. They widen with
// LINUX_DAEMON_LEGACY_LOCATION_ROLE (UNRECORDED): an exact location still
// wins, and a moved model is followed only while it is the one adapter
// present. A successful restore or an explicit Apply/startup binding then
// records the real role from current enumeration.

struct LinuxDaemonStateRecordSchema2 {
    gc_u32 magic;
    gc_u32 version;
    gc_u32 size;
    gc_u32 state;
    GpuAdapterInfo targetGpu;
    DesiredSettingsSchema2 desired;
    gc_u64 operationId;
    gc_u32 operationState;
    gc_u32 checksum;
};
static_assert(sizeof(LinuxDaemonStateRecordSchema2) == 1176,
              "LinuxDaemonStateRecordSchema2 is a FROZEN on-disk layout");
static_assert(sizeof(LinuxDaemonStateRecordSchema2) != sizeof(LinuxDaemonStateRecord),
              "location-role record layouts must be distinguishable by size");

static inline bool linux_daemon_state_record_widen_schema2(
    LinuxDaemonStateRecord* out, const LinuxDaemonStateRecordSchema2* in) {
    if (!out || !in || in->magic != LINUX_DAEMON_RECORD_MAGIC ||
        in->version != LINUX_DAEMON_RECORD_PRE_LOCATION_ROLE_VERSION || in->size != sizeof(*in) ||
        in->checksum != linux_daemon_record_hash_bytes(
            in, offsetof(LinuxDaemonStateRecordSchema2, checksum))) return false;
    DesiredSettings desired = {};
    desired_settings_widen_schema2(&desired, &in->desired);
    if (in->state < LINUX_DAEMON_RECORD_PREPARED ||
        in->state > LINUX_DAEMON_RECORD_UNCERTAIN ||
        in->operationState > SERVICE_OPERATION_OUTCOME_UNKNOWN) return false;
    linux_daemon_record_initialize(out, (LinuxDaemonRecordState)in->state,
        &in->targetGpu, &desired, in->operationId,
        in->operationState, LINUX_DAEMON_LEGACY_LOCATION_ROLE);
    return linux_daemon_record_valid(out);
}

struct LinuxDaemonStartupRecordSchema2 {
    gc_u32 magic;
    gc_u32 version;
    gc_u32 size;
    gc_u32 mode;        // ServiceStartupPolicyMode
    gc_u32 profileSlot; // 1..CONFIG_NUM_SLOTS when mode == PROFILE, else 0
    gc_u32 reserved;
    char profileName[LINUX_DAEMON_STARTUP_NAME_MAX];
    GpuAdapterInfo targetGpu;
    DesiredSettingsSchema2 desired;
    gc_u32 checksum;
};
static_assert(sizeof(LinuxDaemonStartupRecordSchema2) == 1236,
              "LinuxDaemonStartupRecordSchema2 is a FROZEN on-disk layout");
static_assert(sizeof(LinuxDaemonStartupRecordSchema2) != sizeof(LinuxDaemonStartupRecord),
              "location-role record layouts must be distinguishable by size");

static inline bool linux_daemon_startup_widen_schema2(
    LinuxDaemonStartupRecord* out, const LinuxDaemonStartupRecordSchema2* in) {
    if (!out || !in || in->magic != LINUX_DAEMON_STARTUP_MAGIC ||
        in->version != LINUX_DAEMON_STARTUP_PRE_LOCATION_ROLE_VERSION || in->size != sizeof(*in) ||
        in->checksum != linux_daemon_record_hash_bytes(
            in, offsetof(LinuxDaemonStartupRecordSchema2, checksum))) return false;
    if (in->mode >= SERVICE_STARTUP_POLICY_MODE_COUNT) return false;
    if (in->mode == SERVICE_STARTUP_POLICY_PROFILE) {
        if (in->profileSlot < 1 || in->profileSlot > (gc_u32)CONFIG_NUM_SLOTS ||
            !in->targetGpu.valid || !in->targetGpu.pciInfoValid) return false;
    } else if (in->profileSlot != 0 || in->targetGpu.valid) return false;
    DesiredSettings desired = {};
    desired_settings_widen_schema2(&desired, &in->desired);
    if (!service_wire_string_is_terminated(in->profileName,
            (unsigned int)sizeof(in->profileName))) return false;
    linux_daemon_startup_initialize(out, in->mode, in->profileSlot,
        in->profileName, &in->targetGpu, &desired, LINUX_DAEMON_LEGACY_LOCATION_ROLE);
    return linux_daemon_startup_valid(out);
}

#endif
