// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Pure sd_notify READY payload construction.

#ifndef GREEN_CURVE_LINUX_SYSTEMD_NOTIFY_POLICY_H
#define GREEN_CURVE_LINUX_SYSTEMD_NOTIFY_POLICY_H

#include "gpu_core.h"

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif
#ifndef APP_BUILD_NUMBER
#define APP_BUILD_NUMBER 0
#endif

static inline bool linux_systemd_build_ready_payload(
    const ServiceGpuHealth* health, char* payload, size_t payloadSize) {
    if (!payload || payloadSize == 0) return false;
    const char* healthName = health
        ? service_gpu_health_reason_name(health->reason) : "unknown";
    int written = snprintf(payload, payloadSize,
        "READY=1\nSTATUS=Green Curve v%s build %u protocol %u; GPU health: %s%s%s",
        APP_VERSION, (unsigned int)APP_BUILD_NUMBER,
        (unsigned int)SERVICE_PROTOCOL_VERSION, healthName,
        health && health->detail[0] ? " - " : "",
        health && health->detail[0] ? health->detail : "");
    return written >= 0 && (size_t)written < payloadSize;
}

#endif // GREEN_CURVE_LINUX_SYSTEMD_NOTIFY_POLICY_H
