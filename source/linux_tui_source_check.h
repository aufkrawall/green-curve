// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_editor_check.h"
#include "linux_tui_capture_transport.h"

static void tui_check_source(TuiState* state) {
    if (!state->document.valid) return;
    if (state->sourceCheckVisible) {
        state->sourceCheckVisible = false; state->sourceScroll = 0;
        snprintf(state->status, sizeof(state->status), "Showing stored source intent"); return;
    }
    state->sourceReview.reset(); state->sourceAssistPending.reset();
    const auto* shown = linux_tui_authoritative_gpu(&state->service);
    state->sourceReviewContext = linux_tui_review_context(state->service);
    if (!state->serviceOnline || !shown || !state->sourceReviewContext.serviceInstanceId ||
        linux_tui_shown_gpu_write(&state->configuredGpu, shown) != LINUX_TUI_SHOWN_GPU_WRITE_ALLOWED) {
        snprintf(state->status, sizeof(state->status), "Destination check unavailable: the selected GPU must be shown by the daemon");
        return;
    }
    state->sourceReview = std::make_unique<ProfileDestinationReview>();
    const int code = profile_editor_check(state->document.value, state->sourceGraphicsChoice, state->sourceMemoryChoice,
        [&](gc_u32 domains, HardwareCaptureObservation& out, ProfileCodecError& error) {
            return tui_read_capture(*state, domains, out, error, state->sourceReviewContext);
        }, state->sourceCheckLines, state->sourceReview.get(), state->document.revision);
    state->sourceCheckVisible = true; state->sourceScroll = 0; state->sourceConvertIndex = 0;
    snprintf(state->status, sizeof(state->status), "Destination Check: %s; read-only; full operation review is required before Apply",
        state->sourceReview->planned() ? profile_control_disposition_name(state->sourceReview->controls.disposition) :
        state->sourceReview->stage == PROFILE_REVIEW_CAPTURE ? "mapping or intent needs review" : "required reads unavailable");
    linux_debug_logf("profile-check: source=tui code=%d stage=%d revision=%llu lines=%u graphics=%u memory=%u", code,
        (int)state->sourceReview->stage, (unsigned long long)state->document.revision,
        (unsigned)state->sourceCheckLines.size(), (unsigned)state->sourceGraphicsChoice, (unsigned)state->sourceMemoryChoice);
}
