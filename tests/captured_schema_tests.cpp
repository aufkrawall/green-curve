// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "linux_daemon_state.h"
#include "desired_settings_ui_policy.h"
#ifdef _WIN32
#include <windows.h>
#include "service_restart_snapshot_schema.h"
#endif

int run_captured_schema_tests() {
    // Populate the historical layout itself; never relabel the live struct.
    DesiredSettingsSchema2 legacy{};
    legacy.hasCurvePoint[22] = 1;
    legacy.curvePointMHz[22] = 1000;
    legacy.curvePointFromGpuOffset[22] = 1;
    legacy.hasMemOffset = 1;
    legacy.memOffsetMHz = -123;
    DesiredSettings current{};
    desired_settings_widen_schema2(&current, &legacy);
    if (current.curvePointCaptured[22] || current.curvePointCapturedOffsetHz[22] ||
        !current.curvePointFromGpuOffset[22] || current.memOffsetMHz != -123) return 8010;
    DesiredSettingsSchema2 narrowed{};
    desired_settings_narrow_schema2(&narrowed, &current);
    if (memcmp(&legacy, &narrowed, sizeof(legacy))) return 8011;

    current.curvePointCaptured[21] = 1;
    current.curvePointCapturedOffsetHz[21] = 15000500;
    current.curvePointCaptured[20] = 1; // zero is present
    current.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED;
    DesiredSettingsSchema3 exact{};
    desired_settings_narrow_schema3(&exact, &current);
    DesiredSettings restored{};
    desired_settings_widen_schema3(&restored, &exact);
    if (!desired_settings_equal(&current, &restored) ||
        !restored.curvePointCaptured[20] ||
        restored.curvePointCapturedOffsetHz[21] != 15000500) return 8012;
    restored.curvePointCaptured[20] = 0;
    if (desired_settings_equal(&current, &restored)) return 8013;
    if (!service_desired_bool_fields_valid(&current)) return 8014;
    restored = current;
    restored.hasCurvePoint[21] = 1;
    if (service_desired_bool_fields_valid(&restored)) return 8015;
    restored = current;
    restored.curvePointCaptured[21] = 2;
    if (service_desired_bool_fields_valid(&restored)) return 8016;
    restored = {};
    restored.curvePointCapturedOffsetHz[21] = 999;
    if (service_desired_settings_is_zero(restored)) return 8017;
    validate_desired_settings_for_ipc(&restored);
    if (restored.curvePointCapturedOffsetHz[21]) return 8018;

    LinuxDaemonStateRecordSchema4 oldState{};
    oldState.magic = LINUX_DAEMON_RECORD_MAGIC;
    oldState.version = LINUX_DAEMON_RECORD_PRE_CAPTURED_OFFSET_VERSION;
    oldState.size = sizeof(oldState);
    oldState.state = LINUX_DAEMON_RECORD_ACTIVE;
    oldState.desired = legacy;
    oldState.operationId = 42;
    oldState.operationState = SERVICE_OPERATION_SUCCEEDED;
    oldState.targetLocationRole = GPU_LOCATION_ROLE_ADVISORY;
    oldState.targetUuid = linux_gpu_uuid_parse("GPU-12345678-1234-1234-1234-123456789abc");
    oldState.checksum = linux_daemon_record_hash_bytes(&oldState,
        offsetof(LinuxDaemonStateRecordSchema4, checksum));
    LinuxDaemonStateRecord state{};
    if (!linux_daemon_state_record_widen_schema4(&state, &oldState) ||
        state.operationId != 42 || state.desired.memOffsetMHz != -123 ||
        state.targetLocationRole != GPU_LOCATION_ROLE_ADVISORY ||
        memcmp(&state.targetUuid, &oldState.targetUuid, sizeof(state.targetUuid))) return 8019;
    oldState.desired.memOffsetMHz++;
    if (linux_daemon_state_record_widen_schema4(&state, &oldState)) return 8020;

    LinuxDaemonStartupRecordSchema4 oldStartup{};
    oldStartup.magic = LINUX_DAEMON_STARTUP_MAGIC;
    oldStartup.version = LINUX_DAEMON_STARTUP_PRE_CAPTURED_OFFSET_VERSION;
    oldStartup.size = sizeof(oldStartup);
    oldStartup.mode = SERVICE_STARTUP_POLICY_PROFILE;
    oldStartup.profileSlot = 2;
    strcpy(oldStartup.profileName, "fixture");
    oldStartup.targetGpu.valid = oldStartup.targetGpu.pciInfoValid = 1;
    oldStartup.desired = legacy;
    oldStartup.targetLocationRole = GPU_LOCATION_ROLE_REQUIRED;
    oldStartup.targetUuid = oldState.targetUuid;
    oldStartup.checksum = linux_daemon_record_hash_bytes(&oldStartup,
        offsetof(LinuxDaemonStartupRecordSchema4, checksum));
    LinuxDaemonStartupRecord startup{};
    if (!linux_daemon_startup_widen_schema4(&startup, &oldStartup) ||
        startup.profileSlot != 2 || startup.desired.memOffsetMHz != -123 ||
        startup.targetLocationRole != GPU_LOCATION_ROLE_REQUIRED ||
        memcmp(&startup.targetUuid, &oldStartup.targetUuid, sizeof(startup.targetUuid))) return 8021;
    oldStartup.desired.curvePointMHz[22]++;
    if (linux_daemon_startup_widen_schema4(&startup, &oldStartup)) return 8022;
#ifdef _WIN32
    ServiceRestartReapplySnapshotSchema2 oldRestart{};
    oldRestart.desired = legacy;
    oldRestart.activeProfileSlot = 3;
    ServiceRestartReapplySnapshot restart{};
    unsigned char bytes[sizeof(oldRestart)]{};
    memcpy(bytes, &oldRestart, sizeof(bytes));
    int reads = 0;
    auto reader = [&](void* dst, size_t count) {
        ++reads;
        if (count != sizeof(bytes)) return false;
        memcpy(dst, bytes, count);
        return true;
    };
    if (!service_restart_snapshot_read_payload(SERVICE_ACTIVE_DESIRED_SCHEMA2_VERSION,
            sizeof(bytes), reader, &restart) || reads != 1) return 8026;
    if (restart.activeProfileSlot != 3 || restart.desired.curvePointMHz[22] != 1000 ||
        restart.desired.curvePointCaptured[22]) return 8023;
    if (service_restart_snapshot_read_payload(SERVICE_ACTIVE_DESIRED_SCHEMA2_VERSION,
            sizeof(bytes) - 1, reader, &restart) || reads != 1) return 8027;
    if (service_restart_snapshot_read_payload(SERVICE_ACTIVE_DESIRED_VERSION,
            sizeof(bytes), reader, &restart) || reads != 1) return 8028;
#endif
    ServiceRequest request{};
    request.magic = SERVICE_PROTOCOL_MAGIC;
    request.version = SERVICE_PROTOCOL_VERSION - 1;
    if (strcmp(service_request_reject_reason(&request), "wrong protocol version")) return 8024;
    request.version = SERVICE_PROTOCOL_VERSION + 1;
    if (strcmp(service_request_reject_reason(&request), "wrong protocol version")) return 8025;
    request.version = SERVICE_PROTOCOL_VERSION;
    request.callerPid = 1;
    request.command = SERVICE_CMD_PING;
    request.desired.curvePointCaptured[0] = 1;
    if (strcmp(service_request_reject_reason(&request), "captured offset replay is not available")) return 8029;
    request.desired = {};
    request.desired.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED;
    if (strcmp(service_request_reject_reason(&request), "explicit application scope is not available")) return 8030;
    return 0;
}
