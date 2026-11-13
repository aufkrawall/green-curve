// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included by the scalar schema fixture after its production dependencies.
static int authoring_schema_cases() {
    DesiredSettings d{};
    d.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED;
    d.hasGpuOffset = 1; d.gpuOffsetMHz = -15;
    d.authoring.graphicsPrecise = 1; d.authoring.graphicsOffsetHz = -15000500;
    d.authoring.graphicsSurface = DESIRED_GRAPHICS_VF_COMPONENT;
    d.authoring.identityExclusions = 1; d.authoring.graphicsExcluded[20] = 1;
    d.hasCurvePoint[20] = 1; d.curvePointMHz[20] = 1000;
    d.authoring.pointFrequencyPrecise[20] = 1; d.authoring.pointFrequencyHz[20] = 1000000500;
    d.hasLock = 1; d.lockMode = LOCK_MODE_FLATTEN; d.lockCi = 21; d.lockMHz = 1100;
    d.authoring.lockPrecise = 1; d.authoring.lockFrequencyHz = 1100000500;
    d.authoring.topologyBound = 1; d.authoring.topologyCount = 2;
    d.authoring.voltageUv[20] = 662000; d.authoring.voltageUv[21] = 675000;
    if (!desired_authoring_valid(&d) || !service_desired_bool_fields_valid(&d) ||
        desired_authoring_runtime_ready(&d) || service_desired_settings_is_zero(d)) return 8650;
    DesiredSettings copy = d;
    validate_desired_settings_for_ipc(&copy);
    if (!desired_settings_equal(&d, &copy) || desired_absolute_hz(d, 20) != 1000000500 ||
        desired_graphics_hz(d) != -15000500 || desired_lock_hz(d) != 1100000500) return 8651;
    copy.authoring.pointFrequencyHz[20]++;
    if (desired_settings_equal(&d, &copy) || desired_curve_point_semantically_equal(&d, &copy, 20)) return 8652;
    long long narrowed = 37;
    if (desired_curve_point_offset_khz(&d, 20, 1000000, 0, &narrowed) || narrowed != 37) return 8653;
    desired_curve_point_clear(&copy, 20);
    if (copy.authoring.pointFrequencyPrecise[20] || copy.authoring.pointFrequencyHz[20]) return 8654;
    desired_curve_point_copy(&copy, &d, 20);
    if (!desired_settings_equal(&d, &copy)) return 8655;
    for (int mutation = 0; mutation < 11; ++mutation) {
        copy = d;
        switch (mutation) {
        case 0: copy.authoring.graphicsPrecise = 2; break;
        case 1: copy.authoring.pointFrequencyPrecise[20] = 2; break;
        case 2: copy.authoring.pointFrequencyHz[20] += 1000000; break;
        case 3: copy.authoring.voltageUv[20] = 0; break;
        case 4: copy.authoring.topologyCount = 3; break;
        case 5: copy.authoring.graphicsExcluded[20] = 2; break;
        case 6: copy.authoring.graphicsSurface = DESIRED_GRAPHICS_P0; break;
        case 7: copy.authoring.lockFrequencyHz += 1000000; break;
        case 8: copy.authoring.reserved = 1; break;
        case 9: copy.authoring.topologyBound = 0; break;
        case 10: copy.authoring.pointFrequencyHz[22] = 1; break;
        }
        if (desired_authoring_valid(&copy) || service_desired_bool_fields_valid(&copy)) return 8656 + mutation;
    }
    DesiredSettings fan{}; fan.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED;
    fan.hasFan = fan.fanAuto = 1; fan.fanMode = FAN_MODE_AUTO;
    copy = service_merge_desired_after_mutation(&d, &fan);
    if (!desired_authoring_equal(d.authoring, copy.authoring)) return 8667;
    ServiceScopedApplyPlan plan{}; char why[160]{}; VfFloorTopology floor{};
    if (!service_build_scoped_apply_plan(&d, fan, nullptr, floor, false, &plan, why, sizeof(why)) ||
        !desired_authoring_equal(d.authoring, plan.owned.authoring) ||
        !desired_authoring_is_zero(plan.hardware.authoring)) return 8668;
    copy = d;
    service_project_desired_to_available_domains(&copy, SERVICE_MUTATION_DOMAIN_FAN);
    if (!desired_authoring_valid(&copy) || copy.authoring.graphicsPrecise || copy.authoring.lockPrecise ||
        copy.authoring.pointFrequencyPrecise[20] || copy.authoring.pointFrequencyHz[20]) return 8669;
    GpuAdapterInfo gpu{}; gpu.valid = gpu.pciInfoValid = 1;
    LinuxDaemonStateRecord record{};
    linux_daemon_record_initialize(&record, LINUX_DAEMON_RECORD_ACTIVE, &gpu, &d);
    if (!linux_daemon_record_valid(&record) || !desired_settings_equal(&d, &record.desired)) return 8670;
    record.desired.authoring.voltageUv[20]++;
    if (linux_daemon_record_valid(&record)) return 8671;
    LinuxDaemonStartupRecord startup{};
    linux_daemon_startup_initialize(&startup, SERVICE_STARTUP_POLICY_PROFILE, 2, "Exact", &gpu, &d);
    if (!linux_daemon_startup_valid(&startup) || !desired_settings_equal(&d, &startup.desired)) return 8672;
#ifdef _WIN32
    ServiceRestartReapplySnapshotSchema4 old{};
    desired_settings_narrow_schema4(&old.desired, &d);
    old.activeProfileSlot = 2;
    ServiceRestartReapplySnapshot current{};
    auto readOld = [&](void* out, size_t size) {
        if (size != sizeof(old)) return false;
        memcpy(out, &old, size); return true;
    };
    if (!service_restart_snapshot_read_payload(SERVICE_ACTIVE_DESIRED_SCHEMA4_VERSION, sizeof(old), readOld, &current) ||
        current.activeProfileSlot != 2 || !desired_authoring_is_zero(current.desired.authoring)) return 8673;
    current.desired = d;
    ServiceRestartReapplySnapshot reread{};
    auto readNew = [&](void* out, size_t size) {
        if (size != sizeof(current)) return false;
        memcpy(out, &current, size); return true;
    };
    if (!service_restart_snapshot_read_payload(SERVICE_ACTIVE_DESIRED_VERSION, sizeof(current), readNew, &reread) ||
        !desired_settings_equal(&d, &reread.desired)) return 8674;
#endif
    ServiceRequest request{}; request.magic = SERVICE_PROTOCOL_MAGIC;
    request.version = SERVICE_PROTOCOL_VERSION; request.command = SERVICE_CMD_APPLY; request.callerPid = 1;
    request.desired = d;
    const char* reason = service_request_reject_reason(&request);
    if (!reason || !strstr(reason, "destination review")) return 8675;
    request.desired = {}; request.expectedControlFingerprint = 1;
    reason = service_request_reject_reason(&request);
    if (!reason || !strstr(reason, "control preconditions")) return 8676;
    return 0;
}
