// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "linux_tui_internal.h"
#include "linux_debug_log.h"

static inline bool tui_capture_binding_current(const TuiState &state) {
    const auto &context = state.captureReviewContext;
    // Injected offline readers can provide already validated source snapshots.
    // The production transport requires a live binding before it sends Capture.
    if (!context.serviceInstanceId)
        return !state.serviceOnline;
    if (!state.serviceOnline || !state.captureObservation ||
        !linux_tui_review_context_matches(context, state.service) ||
        linux_tui_shown_gpu_write(&state.configuredGpu, linux_tui_authoritative_gpu(&state.service)) !=
            LINUX_TUI_SHOWN_GPU_WRITE_ALLOWED)
        return false;
    const auto &observation = *state.captureObservation;
    const auto binding = hardware_capture_binding(context.gpu);
    return observation.gpuGeneration == context.gpuGeneration && observation.fingerprint &&
           hardware_capture_observation_valid(observation) &&
           observation.fingerprint == hardware_capture_fingerprint(observation) &&
           observation.revision == service_control_review_revision(
                                       observation.fingerprint,
                                       state.service.state.activeDesiredValid ? &state.service.desired : nullptr) &&
           !memcmp(&binding, &observation.binding, sizeof(binding));
}
static inline void tui_reconcile_capture(TuiState &state, const char *reason) {
    if (!state.capture.active || !state.capture.hasObservation || tui_capture_binding_current(state))
        return;
    state.capture.hasObservation = state.capture.allowPartial = state.capture.discardConfirmed = false;
    state.capture.representations = 0;
    state.capture.scroll = 0;
    state.captureObservation.reset();
    state.captureProposals.reset();
    state.captureProposalContext = {};
    state.captureReviewContext = {};
    state.capturePresented = false;
    state.focusIndex = -1;
    state.captureLines = {std::string("Snapshot expired: ") + reason +
                          ". Read again; the previous draft is preserved."};
    snprintf(state.status, sizeof(state.status), "Capture expired: %s; Read again", reason);
    linux_debug_logf("profile-capture: expired=%s", reason);
}
