// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "linux_port.h"
#include "intent_readback_status.h"
// For linux_auto_restore_lockout_reason_name(): a latched automatic-restore
// lockout is the one daemon state that makes settings silently stop applying at
// boot, so it belongs in the status a bug report is built from.
#include "linux_auto_restore_policy.h"

#include <stdio.h>

namespace {

const char* phase_name(gc_u32 phase) {
    switch (phase) {
        case SERVICE_GPU_PHASE_STARTING: return "starting";
        case SERVICE_GPU_PHASE_DEVICE_MISSING: return "device-missing";
        case SERVICE_GPU_PHASE_RECOVERING: return "recovering";
        case SERVICE_GPU_PHASE_REAPPLYING: return "reapplying";
        case SERVICE_GPU_PHASE_READY: return "ready";
        case SERVICE_GPU_PHASE_DEGRADED: return "degraded";
        default: return "unknown";
    }
}

const char* architecture_source_name(gc_u32 source) {
    switch (source) {
        case SERVICE_GPU_ARCH_SOURCE_NVAPI: return "NvAPI";
        case SERVICE_GPU_ARCH_SOURCE_NVML: return "NVML";
        case SERVICE_GPU_ARCH_SOURCE_CACHED: return "cached";
        case SERVICE_GPU_ARCH_SOURCE_FUTURE_GUESS: return "future-guess";
        default: return "none";
    }
}

int point_base_mhz(const ServiceSnapshot& snapshot, int index) {
    return intent_point_base_mhz(&snapshot, index);
}

int point_live_mhz(const ServiceSnapshot& snapshot, int index) {
    return intent_point_live_mhz(&snapshot, index);
}

int point_target_mhz(const ServiceResponse& response, int index,
                     const char** rule) {
    const ServiceSnapshot& snapshot = response.snapshot;
    const DesiredSettings& desired = response.desired;
    int live = point_live_mhz(snapshot, index);
    if (!response.state.activeDesiredValid) {
        if (rule) *rule = "live";
        return live;
    }
    if (desired.hasLock && desired.lockCi >= 0 && index >= desired.lockCi &&
        desired.lockMHz > 0) {
        if (rule) *rule = desired.lockMode == LOCK_MODE_HARD
            ? "hard-pin" : "flatten-tail";
        return (int)desired.lockMHz;
    }
    if (desired.hasCurvePoint[index] && desired.curvePointMHz[index] > 0) {
        if (rule) *rule = "absolute";
        return (int)desired.curvePointMHz[index];
    }
    int populatedOrdinal = 0;
    for (int i = 0; i <= index; ++i) {
        if (snapshot.curve[i].freq_kHz == 0) continue;
        if (i == index) break;
        ++populatedOrdinal;
    }
    if (desired.hasGpuOffset &&
        populatedOrdinal >= desired.gpuOffsetExcludeLowCount) {
        if (rule) *rule = "gpu-offset";
        return point_base_mhz(snapshot, index) + desired.gpuOffsetMHz;
    }
    if (rule) *rule = desired.hasGpuOffset ? "excluded" : "live";
    return live;
}

void json_string(FILE* out, const char* text) {
    fputc('"', out);
    for (const unsigned char* p = (const unsigned char*)(text ? text : ""); *p; ++p) {
        if (*p == '"' || *p == '\\') fprintf(out, "\\%c", *p);
        else if (*p == '\n') fputs("\\n", out);
        else if (*p < 32) fprintf(out, "\\u%04x", *p);
        else fputc(*p, out);
    }
    fputc('"', out);
}

const char* intent_readback_state(
    const ServiceResponse& response, const IntentReadbackStatus& status) {
    if (!response.state.activeDesiredValid) return "no-active-intent";
    if (status.divergedDomains) return "overridden";
    if (status.unavailableDomains) return "partial";
    return "matches";
}

void print_optional_value(FILE* out, bool valid, int value) {
    if (valid) fprintf(out, "%d", value);
    else fputs("null", out);
}

}  // namespace

void print_linux_live_state_text(FILE* out, const ServiceResponse* response) {
    if (!out || !response) return;
    const ServiceSnapshot& snapshot = response->snapshot;
    fprintf(out, "Green Curve live GPU state\n");
    fprintf(out, "Service: %s build %u | protocol=%u | pid=%u | phase=%s | instance=%llu | generation=%llu\n",
            response->serviceVersion, response->serviceBuildNumber,
            response->version, response->servicePid,
            phase_name(response->state.gpuPhase),
            (unsigned long long)response->state.serviceInstanceId,
            (unsigned long long)response->state.gpuGeneration);
    fprintf(out, "GPU: %s | family=%d | populated VF points=%d | topology=%llu\n",
            snapshot.gpuName, (int)snapshot.gpuFamily,
            snapshot.numPopulated,
            (unsigned long long)response->state.topologySignature);
    fprintf(out, "Health: %s | driver_status=%d | architecture_source=%s | vf_fresh=%s | "
                 "recovery=%s/%s | mutation_domains=0x%02x\n",
            service_gpu_health_reason_name(snapshot.health.reason),
            snapshot.health.driverStatus,
            architecture_source_name(snapshot.health.architectureSource),
            snapshot.health.vfSnapshotFresh ? "yes" : "no",
            snapshot.health.recoveryAttempted ? "attempted" : "not-attempted",
            snapshot.health.recoverySucceeded ? "succeeded" : "not-succeeded",
            snapshot.health.availableMutationDomains);
    fprintf(out, "Health detail: %s\n",
            snapshot.health.detail[0] ? snapshot.health.detail : "none");
    fprintf(out, "Automatic restore: %s\n\n",
            snapshot.autoRestoreLockoutReason ==
                SERVICE_AUTO_RESTORE_LOCKOUT_NONE
                ? "armed"
                : linux_auto_restore_lockout_reason_name(
                      snapshot.autoRestoreLockoutReason));
    IntentReadbackStatus readback = compare_intent_to_readback(response);
    fprintf(out,
            "Intent/readback: %s | requested=0x%02x | checked=0x%02x | "
            "overridden=0x%02x | unavailable=0x%02x | max_vf_delta=%d MHz\n",
            intent_readback_state(*response, readback),
            readback.requestedDomains, readback.checkedDomains,
            readback.divergedDomains, readback.unavailableDomains,
            readback.maxVfDeltaMHz);
    if (response->state.activeDesiredValid) {
        fprintf(out,
                "Configured intent: GPU=%s%+d MHz | memory=%s%+d MHz | "
                "power=%s%d%% | fan=%s mode=%d target=%d%%\n",
                response->desired.hasGpuOffset ? "" : "not-set/",
                response->desired.gpuOffsetMHz,
                response->desired.hasMemOffset ? "" : "not-set/",
                response->desired.memOffsetMHz,
                response->desired.hasPowerLimit ? "" : "not-set/",
                response->desired.powerLimitPct,
                response->desired.hasFan ? "" : "not-set",
                response->desired.fanMode, response->desired.fanPercent);
    } else {
        fprintf(out, "Configured intent: none\n");
    }
    fprintf(out, "Hardware readback: GPU=");
    if (response->controlState.gpuOffsetReadbackValid)
        fprintf(out, "%+d MHz", response->controlState.gpuOffsetMHz);
    else
        fputs("unavailable", out);
    fputs(" | memory=", out);
    if (response->controlState.memOffsetReadbackValid)
        fprintf(out, "%+d MHz", response->controlState.memOffsetMHz);
    else
        fputs("unavailable", out);
    fputs(" | power=", out);
    if (response->controlState.powerLimitReadbackValid)
        fprintf(out, "%d%% (%d mW)", response->controlState.powerLimitPct,
                snapshot.powerLimitCurrentmW);
    else
        fputs("unavailable", out);
    if (snapshot.fanSupported && snapshot.fanCount > 0 &&
        response->controlState.fanPolicyReadbackValid) {
        fprintf(out, " | fan0=%s",
                snapshot.fanPolicy[0] ==
                    NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW
                    ? "auto" : "manual");
        if (response->controlState.fanTargetReadbackValid)
            fprintf(out, " target=%u%%", snapshot.fanTargetPercent[0]);
        else
            fputs(" target=unavailable", out);
        fprintf(out, " measured=%u%%", snapshot.fanPercent[0]);
    } else {
        fputs(" | fan=unavailable", out);
    }
    fputs("\n\n", out);
    fprintf(out, " idx  mode     mV   baseMHz  liveMHz  offsetMHz  targetMHz  rule\n");
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        if (snapshot.curve[i].freq_kHz == 0) continue;
        const char* rule = "live";
        int target = point_target_mhz(*response, i, &rule);
        const char* mode = "off";
        if (response->state.activeDesiredValid && response->desired.hasLock &&
            response->desired.lockCi == i) {
            mode = response->desired.lockMode == LOCK_MODE_HARD ? "pin" : "flat";
        }
        fprintf(out, " %3d  %-7s %4u  %7d  %7d  %+9d  %9d  %s\n",
                i, mode, snapshot.curve[i].volt_uV / 1000u,
                point_base_mhz(snapshot, i), point_live_mhz(snapshot, i),
                snapshot.freqOffsets[i] / 1000, target, rule);
    }
}

void print_linux_live_state_json(FILE* out, const ServiceResponse* response) {
    if (!out || !response) return;
    const ServiceSnapshot& snapshot = response->snapshot;
    fputs("{\n  \"service_version\": ", out); json_string(out, response->serviceVersion);
    fprintf(out, ",\n  \"service_build\": %u,\n  \"protocol\": %u,\n  \"service_pid\": %u,\n  \"phase\": ",
            response->serviceBuildNumber, response->version,
            response->servicePid);
    json_string(out, phase_name(response->state.gpuPhase));
    fprintf(out, ",\n  \"service_instance\": %llu,\n  \"gpu_generation\": %llu,\n",
            (unsigned long long)response->state.serviceInstanceId,
            (unsigned long long)response->state.gpuGeneration);
    fputs("  \"gpu\": ", out); json_string(out, snapshot.gpuName);
    fprintf(out, ",\n  \"gpu_family\": %d,\n  \"topology_signature\": %llu,\n",
            (int)snapshot.gpuFamily,
            (unsigned long long)response->state.topologySignature);
    fputs("  \"health\": {\n    \"reason\": ", out);
    json_string(out, service_gpu_health_reason_name(snapshot.health.reason));
    fprintf(out, ",\n    \"reason_code\": %u,\n    \"driver_status\": %d,\n    \"architecture_source\": ",
            snapshot.health.reason, snapshot.health.driverStatus);
    json_string(out, architecture_source_name(snapshot.health.architectureSource));
    fprintf(out,
            ",\n    \"vf_snapshot_fresh\": %s,\n    \"recovery_attempted\": %s,\n"
            "    \"recovery_succeeded\": %s,\n    \"available_mutation_domains\": %u,\n"
            "    \"detail\": ",
            snapshot.health.vfSnapshotFresh ? "true" : "false",
            snapshot.health.recoveryAttempted ? "true" : "false",
            snapshot.health.recoverySucceeded ? "true" : "false",
            snapshot.health.availableMutationDomains);
    json_string(out, snapshot.health.detail);
    IntentReadbackStatus readback = compare_intent_to_readback(response);
    fputs("\n  },\n  \"auto_restore\": {\n    \"locked_out\": ", out);
    fprintf(out, "%s,\n    \"lockout_reason_code\": %u,\n    \"lockout_reason\": ",
            snapshot.autoRestoreLockoutReason ==
                SERVICE_AUTO_RESTORE_LOCKOUT_NONE ? "false" : "true",
            snapshot.autoRestoreLockoutReason);
    json_string(out, linux_auto_restore_lockout_reason_name(
                         snapshot.autoRestoreLockoutReason));
    fprintf(out,
            "\n  },\n  \"active_intent\": %s,\n"
            "  \"intent_readback\": {\n    \"state\": ",
            response->state.activeDesiredValid ? "true" : "false");
    json_string(out, intent_readback_state(*response, readback));
    fprintf(out,
            ",\n    \"requested_domains\": %u,\n"
            "    \"checked_domains\": %u,\n"
            "    \"overridden_domains\": %u,\n"
            "    \"unavailable_domains\": %u,\n"
            "    \"max_vf_delta_mhz\": %d\n  },\n"
            "  \"configured_intent\": {\n"
            "    \"gpu_offset_set\": %s, \"gpu_offset_mhz\": %d,\n"
            "    \"memory_offset_set\": %s, \"memory_offset_mhz\": %d,\n"
            "    \"power_limit_set\": %s, \"power_limit_pct\": %d,\n"
            "    \"fan_set\": %s, \"fan_mode\": %d, \"fan_fixed_pct\": %d\n"
            "  },\n"
            "  \"hardware_readback\": {\n"
            "    \"gpu_offset_mhz\": ",
            readback.requestedDomains, readback.checkedDomains,
            readback.divergedDomains, readback.unavailableDomains,
            readback.maxVfDeltaMHz,
            response->desired.hasGpuOffset ? "true" : "false",
            response->desired.gpuOffsetMHz,
            response->desired.hasMemOffset ? "true" : "false",
            response->desired.memOffsetMHz,
            response->desired.hasPowerLimit ? "true" : "false",
            response->desired.powerLimitPct,
            response->desired.hasFan ? "true" : "false",
            response->desired.fanMode, response->desired.fanPercent);
    print_optional_value(out, response->controlState.gpuOffsetReadbackValid,
                         response->controlState.gpuOffsetMHz);
    fputs(",\n    \"memory_offset_mhz\": ", out);
    print_optional_value(out, response->controlState.memOffsetReadbackValid,
                         response->controlState.memOffsetMHz);
    fputs(",\n    \"power_limit_pct\": ", out);
    print_optional_value(out,
        response->controlState.powerLimitReadbackValid,
        response->controlState.powerLimitPct);
    fputs(",\n    \"fan_policy\": ", out);
    if (snapshot.fanSupported && snapshot.fanCount > 0 &&
        response->controlState.fanPolicyReadbackValid)
        json_string(out, snapshot.fanPolicy[0] ==
            NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW ? "auto" : "manual");
    else
        fputs("null", out);
    fputs(",\n    \"fan_target_pct\": ", out);
    print_optional_value(out, snapshot.fanSupported && snapshot.fanCount > 0 &&
                         response->controlState.fanTargetReadbackValid,
                         (int)snapshot.fanTargetPercent[0]);
    fputs(",\n    \"fan_measured_pct\": ", out);
    print_optional_value(out, snapshot.fanSupported && snapshot.fanCount > 0,
                         (int)snapshot.fanPercent[0]);
    fputs("\n  },\n  \"vf_points\": [\n", out);
    bool first = true;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        if (snapshot.curve[i].freq_kHz == 0) continue;
        const char* rule = "live";
        int target = point_target_mhz(*response, i, &rule);
        int ownedTarget = 0;
        bool owned = intent_owned_point_target_mhz(
            response, i, &ownedTarget);
        if (!first) fputs(",\n", out);
        first = false;
        fprintf(out,
            "    {\"index\": %d, \"mv\": %u, \"base_mhz\": %d, "
            "\"live_mhz\": %d, \"offset_mhz\": %d, \"target_mhz\": %d, "
            "\"owned_by_intent\": %s, \"matches_intent\": ",
            i, snapshot.curve[i].volt_uV / 1000u,
            point_base_mhz(snapshot, i), point_live_mhz(snapshot, i),
            snapshot.freqOffsets[i] / 1000, target,
            owned ? "true" : "false");
        if (owned) {
            fputs(intent_abs_int(point_live_mhz(snapshot, i) - ownedTarget) <=
                    INTENT_VF_READBACK_TOLERANCE_MHZ ? "true" : "false", out);
        } else {
            fputs("null", out);
        }
        fputs(", \"rule\": ", out);
        json_string(out, rule);
        fputc('}', out);
    }
    fputs("\n  ]\n}\n", out);
}
