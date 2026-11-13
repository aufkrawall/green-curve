// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "linux_daemon_state.h"
#include "desired_settings_ui_policy.h"
#include "service_scoped_apply_policy.h"
#include "service_lifecycle_policy.h"
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include "service_restart_snapshot_schema.h"
#endif

static DesiredSettings precise_fixture() {
    DesiredSettings d{};
    d.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED;
    desired_memory_set_precise(&d, -123500000, CLOCK_OFFSET_SURFACE_P0);
    desired_power_set_ratio(&d, {249, 250});
    return d;
}

static int scalar_intent_cases() {
    DesiredSettings d = precise_fixture(), copy = d;
    if (!desired_scalar_intent_valid(&d) || d.memOffsetMHz != -123 || d.powerLimitPct != 99) return 8220;
    validate_desired_settings_for_ipc(&copy);
    if (!desired_settings_equal(&d, &copy)) return 8221;
    copy.memOffsetHz += 500000;
    if (desired_settings_equal(&d, &copy)) return 8222;
    copy = d; ++copy.powerRatioNumerator;
    if (desired_settings_equal(&d, &copy)) return 8223;
    copy = d; copy.memOffsetMHz = -124;
    if (service_desired_bool_fields_valid(&copy)) return 8224;
    copy = d; copy.powerRatioNumerator *= 2; copy.powerRatioDenominator *= 2;
    if (service_desired_bool_fields_valid(&copy)) return 8225;
    copy = d; copy.powerLimitRatio = 2;
    if (service_desired_bool_fields_valid(&copy)) return 8226;
    if (desired_memory_set_precise(&copy, 500001, 0) ||
        desired_memory_set_precise(&copy, LLONG_MAX, 0) ||
        desired_memory_set_precise(&copy, 3000500000LL, 0) ||
        desired_memory_set_precise(&copy, 500000, 99)) return 8227;
    copy = {}; copy.memOffsetHz = 1; copy.powerRatioNumerator = 1;
    if (service_desired_settings_is_zero(copy)) return 8228;
    validate_desired_settings_for_ipc(&copy);
    if (!service_desired_settings_is_zero(copy)) return 8229;
    copy = d;
    service_project_desired_to_available_domains(&copy, 0);
    if (copy.memOffsetPrecise || copy.powerLimitRatio || copy.memOffsetHz || copy.powerRatioNumerator ||
        !desired_scalar_intent_valid(&copy)) return 8230;
    DesiredSettings fan{}; fan.hasFan = 1;
    copy = service_merge_desired_after_mutation(&d, &fan);
    if (copy.memOffsetHz != d.memOffsetHz || copy.powerRatioNumerator != 249 || !copy.powerLimitRatio) return 8231;
    DesiredSettings next{}; next.hasMemOffset = 1; next.memOffsetMHz = 30;
    copy = service_merge_desired_after_mutation(&d, &next);
    if (copy.memOffsetPrecise || copy.memOffsetHz || copy.memOffsetMHz != 30 || !copy.powerLimitRatio) return 8232;
    ServiceScopedApplyPlan scoped{};
    char why[192]{};
    fan.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED;
    if (!service_build_scoped_apply_plan(&d, fan, nullptr, {}, false, &scoped, why, sizeof(why)) ||
        scoped.owned.memOffsetHz != -123500000 || scoped.owned.powerRatioNumerator != 249 ||
        scoped.hardware.hasMemOffset || scoped.hardware.hasPowerLimit) return 8233;
    fan.applicationScope = DESIRED_SCOPE_REPLACE_PROFILE;
    if (!service_build_scoped_apply_plan(&d, fan, nullptr, {}, false, &scoped, why, sizeof(why)) ||
        !scoped.hardware.memOffsetPrecise || scoped.hardware.memOffsetSurface != CLOCK_OFFSET_SURFACE_P0 ||
        !scoped.hardware.powerLimitRatio || scoped.hardware.powerRatioNumerator != 1 ||
        scoped.hardware.powerRatioDenominator != 1 ||
        scoped.hardware.memOffsetMHz || scoped.hardware.powerLimitPct != 100 ||
        scoped.owned.hasMemOffset || scoped.owned.hasPowerLimit) return 8234;
    next = d; next.applicationScope = DESIRED_SCOPE_EXPLICIT_RELEASE;
    if (!service_build_scoped_apply_plan(&d, next, nullptr, {}, false, &scoped, why, sizeof(why)) ||
        !scoped.hardware.memOffsetPrecise || !scoped.hardware.powerLimitRatio ||
        service_scoped_has_owned_intent(scoped.owned)) return 8235;
    ServiceRequest request{};
    request.magic = SERVICE_PROTOCOL_MAGIC; request.version = SERVICE_PROTOCOL_VERSION;
    request.command = SERVICE_CMD_APPLY; request.callerPid = 1; request.operationId = 1;
    request.applyOrigin = SERVICE_APPLY_ORIGIN_CLI;
    request.expectedServiceInstanceId = 1; request.expectedGpuGeneration = 1;
    request.targetGpu.valid = 1; request.desired = d;
    if (service_request_reject_reason(&request)) return 8236;
    request.desired.applicationScope = DESIRED_SCOPE_LEGACY;
    if (!service_request_reject_reason(&request)) return 8237;
    copy = {}; copy.hasMemOffset = 1; copy.memOffsetMHz = -123;
    if (desired_memory_intent_matches(copy, d)) return 8253;
    copy.memOffsetMHz = -124;
    desired_memory_set_precise(&d, -124000000, CLOCK_OFFSET_SURFACE_P0);
    if (!desired_memory_intent_matches(copy, d) || desired_memory_intent_matches(d, copy)) return 8254;
    copy = {}; copy.hasPowerLimit = 1; copy.powerLimitPct = 100;
    desired_power_set_ratio(&d, {1001, 1000});
    if (desired_power_intent_matches(copy, d)) return 8255;
    desired_power_set_ratio(&d, {1, 1});
    if (!desired_power_intent_matches(copy, d)) return 8256;
    next = {};
    if (!service_build_profile_transition_request(&d, &next, &copy) ||
        copy.applicationScope != DESIRED_SCOPE_REPLACE_PROFILE || copy.resetOcBeforeApply) return 8257;
    next = precise_fixture(); next.memOffsetSurface = CLOCK_OFFSET_SURFACE_GLOBAL;
    next.applicationScope = DESIRED_SCOPE_EXPLICIT_RELEASE;
    if (!service_build_scoped_apply_plan(nullptr, next, nullptr, {}, false, &scoped, why, sizeof(why)) ||
        scoped.hardware.memOffsetSurface != CLOCK_OFFSET_SURFACE_GLOBAL || scoped.hardware.memOffsetHz) return 8258;
    next.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED;
    if (service_build_scoped_apply_plan(&d, next, nullptr, {}, false, &scoped, why, sizeof(why))) return 8259;
    next = {}; next.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED;
    next.hasMemOffset = true; next.memOffsetMHz = 5;
    if (!service_build_scoped_apply_plan(nullptr, next, nullptr, {}, false, &scoped, why, sizeof(why)) ||
        !scoped.hardware.memOffsetPrecise || scoped.hardware.memOffsetSurface || scoped.owned.memOffsetSurface ||
        !service_bind_scoped_memory_surface(&scoped, CLOCK_OFFSET_SURFACE_P0, why, sizeof(why)) ||
        scoped.hardware.memOffsetSurface != CLOCK_OFFSET_SURFACE_P0 ||
        scoped.owned.memOffsetSurface != CLOCK_OFFSET_SURFACE_P0) return 8260;
    if (service_bind_scoped_memory_surface(&scoped, CLOCK_OFFSET_SURFACE_GLOBAL, why, sizeof(why)) ||
        scoped.owned.memOffsetSurface != CLOCK_OFFSET_SURFACE_P0) return 8261;
    if (service_bind_scoped_memory_surface(&scoped, 0, why, sizeof(why))) return 8262;
    next = {}; next.hasPowerLimit = true; next.powerLimitPct = 100;
    service_upgrade_legacy_scalar_scope(&d, &next);
    if (next.applicationScope != DESIRED_SCOPE_UPDATE_INCLUDED || next.resetOcBeforeApply ||
        !service_build_scoped_apply_plan(&d, next, nullptr, {}, false, &scoped, why, sizeof(why)) ||
        !scoped.hardware.powerLimitRatio || scoped.hardware.powerRatioNumerator != 1 ||
        scoped.hardware.powerRatioDenominator != 1) return 8263;
    return 0;
}

static int frozen_scalar_cases() {
    DesiredSettings d = precise_fixture();
    d.curvePointCaptured[20] = 1; d.curvePointCapturedOffsetHz[20] = 15000500;
    LinuxDaemonStateRecordSchema5 old{};
    old.magic = LINUX_DAEMON_RECORD_MAGIC; old.version = LINUX_DAEMON_RECORD_PRE_PRECISE_SCALAR_VERSION;
    old.size = sizeof(old); old.state = LINUX_DAEMON_RECORD_ACTIVE;
    old.operationId = 42; old.operationState = SERVICE_OPERATION_SUCCEEDED;
    old.targetLocationRole = GPU_LOCATION_ROLE_ADVISORY;
    desired_settings_narrow_schema3(&old.desired, &d);
    old.checksum = linux_daemon_record_hash_bytes(&old, offsetof(LinuxDaemonStateRecordSchema5, checksum));
    LinuxDaemonStateRecord current{};
    if (!linux_daemon_state_record_widen_schema5(&current, &old) || current.operationId != 42 ||
        current.desired.memOffsetPrecise || current.desired.powerLimitRatio ||
        current.desired.memOffsetMHz != -123 || current.desired.powerLimitPct != 99 ||
        current.desired.curvePointCapturedOffsetHz[20] != 15000500) return 8240;
    ++old.desired.memOffsetMHz;
    if (linux_daemon_state_record_widen_schema5(&current, &old)) return 8241;
    LinuxDaemonStartupRecordSchema5 startup{};
    startup.magic = LINUX_DAEMON_STARTUP_MAGIC; startup.version = LINUX_DAEMON_STARTUP_PRE_PRECISE_SCALAR_VERSION;
    startup.size = sizeof(startup); startup.mode = SERVICE_STARTUP_POLICY_PROFILE; startup.profileSlot = 2;
    startup.targetGpu.valid = startup.targetGpu.pciInfoValid = 1;
    startup.targetLocationRole = GPU_LOCATION_ROLE_REQUIRED;
    desired_settings_narrow_schema3(&startup.desired, &d);
    startup.checksum = linux_daemon_record_hash_bytes(&startup, offsetof(LinuxDaemonStartupRecordSchema5, checksum));
    LinuxDaemonStartupRecord widened{};
    if (!linux_daemon_startup_widen_schema5(&widened, &startup) || widened.profileSlot != 2 ||
        widened.desired.memOffsetPrecise || widened.desired.powerLimitRatio ||
        widened.desired.curvePointCapturedOffsetHz[20] != 15000500) return 8242;
    ++startup.desired.memOffsetMHz;
    if (linux_daemon_startup_widen_schema5(&widened, &startup)) return 8243;
    linux_daemon_record_initialize(&current, LINUX_DAEMON_RECORD_ACTIVE, nullptr, &d, 1,
        SERVICE_OPERATION_SUCCEEDED, GPU_LOCATION_ROLE_UNRECORDED);
    if (!linux_daemon_record_valid(&current) || current.desired.memOffsetHz != -123500000 ||
        current.desired.powerRatioNumerator != 249) return 8244;
    current.desired.powerRatioDenominator = 0;
    current.checksum = linux_daemon_record_checksum(&current);
    if (linux_daemon_record_valid(&current)) return 8245;
#ifdef _WIN32
    ServiceRestartReapplySnapshotSchema3 prior{};
    desired_settings_narrow_schema3(&prior.desired, &d); prior.activeProfileSlot = 3;
    ServiceRestartReapplySnapshot result{};
    auto readOld = [&](void* out, size_t size) {
        if (size != sizeof(prior)) return false;
        memcpy(out, &prior, size); return true;
    };
    if (!service_restart_snapshot_read_payload(SERVICE_ACTIVE_DESIRED_SCHEMA3_VERSION,
        sizeof(prior), readOld, &result) || result.activeProfileSlot != 3 ||
        result.desired.memOffsetPrecise || result.desired.powerLimitRatio ||
        result.desired.curvePointCapturedOffsetHz[20] != 15000500) return 8246;
    if (service_restart_snapshot_read_payload(SERVICE_ACTIVE_DESIRED_VERSION,
        sizeof(prior), readOld, &result)) return 8247;
    result.desired = d;
    ServiceRestartReapplySnapshot reread{};
    auto readNew = [&](void* out, size_t size) {
        if (size != sizeof(result)) return false;
        memcpy(out, &result, size); return true;
    };
    if (!service_restart_snapshot_read_payload(SERVICE_ACTIVE_DESIRED_VERSION,
        sizeof(result), readNew, &reread) || !desired_settings_equal(&d, &reread.desired)) return 8248;
#endif
    return 0;
}

int run_precise_scalar_schema_tests() {
    int result = scalar_intent_cases();
    return result ? result : frozen_scalar_cases();
}
