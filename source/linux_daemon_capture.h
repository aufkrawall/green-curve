// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "service_control_review.h"
static bool daemon_collect_capture(const GpuAdapterInfo& target, gc_u32 domains,
    HardwareCaptureObservation& out, char* why, size_t size) {
    if (!g_gpuReady || g_stateUncertain || g_fanOwnershipHandbackDeferred ||
        !hardware_capture_target_matches(target, g_gpu.selectedGpu)) {
        gc_strlcpy(why, size, "capture: bound GPU session is unavailable or needs recovery; Refresh explicitly first");
        return false;
    }
    if (!linux_backend_capture_current(&g_gpu, g_daemonGpuGeneration, domains,
        g_hasActiveDesired ? &g_activeDesired : nullptr, &out, why, size)) return false;
    service_control_review_stamp(out, g_hasActiveDesired ? &g_activeDesired : nullptr);
    return true;
}
static bool daemon_control_preconditions_match(const ServiceRequest* req, ServiceResponse* resp) {
    const auto read = [&](HardwareCaptureObservation& out, char* why, size_t size) {
        return daemon_collect_capture(req->targetGpu, req->expectedControlDomains, out, why, size);
    };
    if (service_control_review_matches(*req, g_hasActiveDesired ? &g_activeDesired : nullptr,
            read, resp->message, sizeof(resp->message))) return true;
    resp->status = SERVICE_STATUS_STALE_STATE;
    dlog("daemon APPLY control review refused: %s\n", resp->message);
    return false;
}
static void daemon_handle_capture(const ServiceRequest* req, ServiceResponse* resp) {
    resp->status = SERVICE_STATUS_ERROR;
    if (daemon_collect_capture(req->targetGpu, req->captureDomains,
            resp->capture, resp->message, sizeof(resp->message))) {
        resp->status = SERVICE_STATUS_OK;
        hardware_capture_summary(resp->capture, resp->message, sizeof(resp->message));
    }
    dlog("%s\n", resp->message);
}
