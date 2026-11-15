// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
// Called on the window thread; reviewed sources have a separate draft binding.
static bool gui_mutation_stamp_request(ServiceRequest *request, char *status, size_t statusSize) {
    const GuiServiceModel &model = g_app.guiServiceModel;
    if (!gui_service_model_ready(&model) || !model.serviceInstanceId || !model.gpuGeneration ||
        !model.topologySignature ||
        (!request->expectedControlFingerprint && (!g_app.guiDraft.attached || g_app.guiDraft.detached))) {
        set_message(
            status, statusSize,
            "GPU state is reconnecting or the preserved draft no longer matches this GPU; refresh before applying");
        return false;
    }
    request->expectedServiceInstanceId = model.serviceInstanceId;
    request->expectedGpuGeneration = model.gpuGeneration;
    request->expectedTopologySignature = model.topologySignature;
    if (const char *reason = service_control_preconditions_reject_reason(*request)) {
        set_message(status, statusSize, "%s", reason);
        return false;
    }
    return true;
}
