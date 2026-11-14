// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_LINUX_DAEMON_GRAPHICS_RECOVERY_SCHEMA_H
#define GREEN_CURVE_LINUX_DAEMON_GRAPHICS_RECOVERY_SCHEMA_H
// FROZEN: version 10 carried bound authoring intent without recovery coverage.
struct LinuxDaemonStateRecordSchema7 {
    gc_u32 magic, version, size, state;
    GpuAdapterInfo targetGpu;
    DesiredSettingsSchema5 desired;
    gc_u64 operationId;
    gc_u32 operationState, targetLocationRole;
    LinuxGpuUuid targetUuid;
    gc_u32 checksum;
};
static_assert(sizeof(LinuxDaemonStateRecordSchema7) == 4808, "daemon record version 10 is frozen");
static_assert(offsetof(LinuxDaemonStateRecordSchema7, checksum) == 4800, "version-10 checksum coverage moved");

static inline bool linux_daemon_state_record_widen_schema7(
    LinuxDaemonStateRecord* out, const LinuxDaemonStateRecordSchema7* in) {
    if (!out || !in || in->magic != LINUX_DAEMON_RECORD_MAGIC || in->size != sizeof(*in) ||
        in->version != LINUX_DAEMON_RECORD_PRE_GRAPHICS_RECOVERY_VERSION ||
        in->state < LINUX_DAEMON_RECORD_PREPARED || in->state > LINUX_DAEMON_RECORD_UNCERTAIN ||
        in->operationState > SERVICE_OPERATION_OUTCOME_UNKNOWN || in->targetLocationRole > GPU_LOCATION_ROLE_REQUIRED ||
        in->checksum != linux_daemon_record_hash_bytes(in, offsetof(LinuxDaemonStateRecordSchema7, checksum))) return false;
    DesiredSettings desired{};
    desired_settings_widen_schema5(&desired, &in->desired);
    LinuxDaemonStateRecord next{};
    linux_daemon_record_initialize(&next, (LinuxDaemonRecordState)in->state, &in->targetGpu, &desired,
        in->operationId, in->operationState, (GpuLocationRole)in->targetLocationRole, &in->targetUuid);
    if (!linux_daemon_record_valid(&next)) return false;
    *out = next; return true;
}
#endif
