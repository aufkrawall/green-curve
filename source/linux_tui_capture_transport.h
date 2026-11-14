// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include <unistd.h>

static bool tui_read_capture(TuiState& state, gc_u32 domains, HardwareCaptureObservation& out,
    ProfileCodecError& error, const LinuxTuiReviewContext& context) {
    if (!state.serviceOnline || !context.serviceInstanceId ||
        linux_tui_shown_gpu_write(&state.configuredGpu, linux_tui_authoritative_gpu(&state.service)) != LINUX_TUI_SHOWN_GPU_WRITE_ALLOWED)
        return profile_codec::fail(&error, 0, "the selected GPU must be shown by the daemon");
    ServiceRequest request{}; auto response = std::make_unique<ServiceResponse>();
    request.magic = SERVICE_PROTOCOL_MAGIC; request.version = SERVICE_PROTOCOL_VERSION;
    request.command = SERVICE_CMD_CAPTURE_CURRENT; request.captureDomains = domains; request.callerPid = (gc_u32)getpid();
    request.targetGpu = context.gpu;
    if (!linux_daemon_send(&request, response.get(), error.message, sizeof(error.message))) return false;
    if (response->status != SERVICE_STATUS_OK || !response->capture.valid)
        return profile_codec::fail(&error, 0, response->message[0] ? response->message : "capture is unavailable");
    if (!linux_tui_review_capture_matches(context, *response))
        return profile_codec::fail(&error, 0, "GPU session or ownership changed during the read; Refresh and try again");
    out = response->capture; return true;
}
