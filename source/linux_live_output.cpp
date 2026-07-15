// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "linux_port.h"

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

int point_base_mhz(const ServiceSnapshot& snapshot, int index) {
    long long base = (long long)snapshot.curve[index].freq_kHz -
                     (long long)snapshot.freqOffsets[index];
    return (int)((base >= 0 ? base + 500 : base - 500) / 1000);
}

int point_live_mhz(const ServiceSnapshot& snapshot, int index) {
    return (int)((snapshot.curve[index].freq_kHz + 500u) / 1000u);
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

}  // namespace

void print_linux_live_state_text(FILE* out, const ServiceResponse* response) {
    if (!out || !response) return;
    const ServiceSnapshot& snapshot = response->snapshot;
    fprintf(out, "Green Curve live GPU state\n");
    fprintf(out, "Service: %s build %u | phase=%s | instance=%llu | generation=%llu\n",
            response->serviceVersion, response->serviceBuildNumber,
            phase_name(response->state.gpuPhase),
            (unsigned long long)response->state.serviceInstanceId,
            (unsigned long long)response->state.gpuGeneration);
    fprintf(out, "GPU: %s | populated VF points: %d\n\n",
            snapshot.gpuName, snapshot.numPopulated);
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
    fprintf(out, ",\n  \"service_build\": %u,\n  \"phase\": ",
            response->serviceBuildNumber);
    json_string(out, phase_name(response->state.gpuPhase));
    fprintf(out, ",\n  \"service_instance\": %llu,\n  \"gpu_generation\": %llu,\n",
            (unsigned long long)response->state.serviceInstanceId,
            (unsigned long long)response->state.gpuGeneration);
    fputs("  \"gpu\": ", out); json_string(out, snapshot.gpuName);
    fprintf(out, ",\n  \"active_intent\": %s,\n  \"vf_points\": [\n",
            response->state.activeDesiredValid ? "true" : "false");
    bool first = true;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        if (snapshot.curve[i].freq_kHz == 0) continue;
        const char* rule = "live";
        int target = point_target_mhz(*response, i, &rule);
        if (!first) fputs(",\n", out);
        first = false;
        fprintf(out,
            "    {\"index\": %d, \"mv\": %u, \"base_mhz\": %d, "
            "\"live_mhz\": %d, \"offset_mhz\": %d, \"target_mhz\": %d, \"rule\": ",
            i, snapshot.curve[i].volt_uV / 1000u,
            point_base_mhz(snapshot, i), point_live_mhz(snapshot, i),
            snapshot.freqOffsets[i] / 1000, target);
        json_string(out, rule);
        fputc('}', out);
    }
    fputs("\n  ]\n}\n", out);
}
