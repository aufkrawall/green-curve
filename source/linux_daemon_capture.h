// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "service_control_review.h"
static bool daemon_collect_capture(LinuxGpuController& ctl, const GpuAdapterInfo& target, gc_u32 domains,
    HardwareCaptureObservation& out, char* why, size_t size) {
    if (!ctl.gpuReady || ctl.stateUncertain || ctl.fanOwnershipHandbackDeferred ||
        !hardware_capture_target_matches(target, ctl.gpu.selectedGpu)) {
        gc_strlcpy(why, size, "capture: bound GPU session is unavailable or needs recovery; Refresh explicitly first");
        return false;
    }
    if (!linux_backend_capture_current(&ctl.gpu, ctl.gpuGeneration, domains,
        ctl.hasActiveDesired ? &ctl.activeDesired : nullptr, &out, why, size)) return false;
    service_control_review_stamp(out, ctl.hasActiveDesired ? &ctl.activeDesired : nullptr);
    return true;
}
static bool daemon_control_preconditions_match(LinuxGpuController& ctl, const ServiceRequest* req, ServiceResponse* resp) {
    const auto read = [&](HardwareCaptureObservation& out, char* why, size_t size) {
        return daemon_collect_capture(ctl, req->targetGpu, req->expectedControlDomains, out, why, size);
    };
    if (service_control_review_matches(*req, ctl.hasActiveDesired ? &ctl.activeDesired : nullptr,
            read, resp->message, sizeof(resp->message))) return true;
    resp->status = SERVICE_STATUS_STALE_STATE;
    dlog("daemon APPLY control review refused: %s\n", resp->message);
    return false;
}
static void daemon_handle_capture(LinuxGpuController& ctl, const ServiceRequest* req, ServiceResponse* resp) {
    resp->status = SERVICE_STATUS_ERROR;
    if (daemon_collect_capture(ctl, req->targetGpu, req->captureDomains,
            resp->capture, resp->message, sizeof(resp->message))) {
        resp->status = SERVICE_STATUS_OK;
        hardware_capture_summary(resp->capture, resp->message, sizeof(resp->message));
    }
    dlog("%s\n", resp->message);
}
