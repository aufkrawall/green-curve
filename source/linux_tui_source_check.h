// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_editor_check.h"
#include <unistd.h>

static void tui_check_source(TuiState* state) {
    if (!state->document.valid) return;
    if (state->sourceCheckVisible) {
        state->sourceCheckVisible = false; state->sourceScroll = 0;
        snprintf(state->status, sizeof(state->status), "Showing stored source intent"); return;
    }
    state->sourceReview.reset();
    if (!state->serviceOnline || linux_tui_shown_gpu_write(&state->configuredGpu,
            linux_tui_authoritative_gpu(&state->service)) != LINUX_TUI_SHOWN_GPU_WRITE_ALLOWED) {
        snprintf(state->status, sizeof(state->status), "Destination check unavailable: the selected GPU must be shown by the daemon");
        return;
    }
    state->sourceReview = std::make_unique<ProfileDestinationReview>();
    const int code = profile_editor_check(state->document.value, state->sourceGraphicsChoice, state->sourceMemoryChoice,
        [&](gc_u32 domains, HardwareCaptureObservation& out, ProfileCodecError& error) {
            ServiceRequest request{}; auto response = std::make_unique<ServiceResponse>();
            request.magic = SERVICE_PROTOCOL_MAGIC; request.version = SERVICE_PROTOCOL_VERSION;
            request.command = SERVICE_CMD_CAPTURE_CURRENT; request.captureDomains = domains; request.callerPid = (gc_u32)getpid();
            if (!linux_daemon_send(&request, response.get(), error.message, sizeof(error.message))) return false;
            if (response->status != SERVICE_STATUS_OK || !response->capture.valid)
                return profile_codec::fail(&error, 0, response->message[0] ? response->message : "capture is unavailable");
            out = response->capture; return true;
        }, state->sourceCheckLines, state->sourceReview.get(), state->document.revision);
    state->sourceCheckVisible = true; state->sourceScroll = 0;
    snprintf(state->status, sizeof(state->status), "Destination Check: %s; read-only; full operation review is required before Apply",
        state->sourceReview->planned() ? profile_control_disposition_name(state->sourceReview->controls.disposition) :
        state->sourceReview->stage == PROFILE_REVIEW_CAPTURE ? "mapping or intent needs review" : "required reads unavailable");
    linux_debug_logf("profile-check: source=tui code=%d stage=%d revision=%llu lines=%u graphics=%u memory=%u", code,
        (int)state->sourceReview->stage, (unsigned long long)state->document.revision,
        (unsigned)state->sourceCheckLines.size(), (unsigned)state->sourceGraphicsChoice, (unsigned)state->sourceMemoryChoice);
}
