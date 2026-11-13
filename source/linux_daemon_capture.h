// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "hardware_capture_runtime.h"
static void daemon_handle_capture(const ServiceRequest* req, ServiceResponse* resp) {
    resp->status = SERVICE_STATUS_ERROR;
    if (!g_gpuReady || g_stateUncertain || g_fanOwnershipHandbackDeferred ||
        !hardware_capture_target_matches(req->targetGpu, g_gpu.selectedGpu)) {
        gc_strlcpy(resp->message, sizeof(resp->message), "capture: bound GPU session is unavailable or needs recovery; Refresh explicitly first");
    } else if (linux_backend_capture_current(&g_gpu, g_daemonGpuGeneration, req->captureDomains,
        g_hasActiveDesired ? &g_activeDesired : nullptr, &resp->capture, resp->message, sizeof(resp->message))) {
        resp->status = SERVICE_STATUS_OK;
        hardware_capture_summary(resp->capture, resp->message, sizeof(resp->message));
    }
    dlog("%s\n", resp->message);
}
