// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included inside linux_tui_gpu_target_tests.cpp's scripted-daemon fixture.
static int review_lifetime_tests() {
    const auto stage = [](TuiState& state) {
        state.document.valid = true; state.document.value = profile_document_make_empty();
        state.document.value.domains = PROFILE_DOMAIN_MEMORY;
        state.document.value.hasMemoryOffset = true; state.document.value.memoryOffsetHz = -123500000;
        state.document.saved = state.document.value; state.dirty = true;
        state.sourceReview = std::make_unique<ProfileDestinationReview>();
        state.sourceReview->sourceRevision = state.document.revision;
        state.sourceReview->stage = PROFILE_REVIEW_PLANNED;
        state.sourceReviewContext = linux_tui_review_context(state.service);
        state.sourceCheckVisible = true; state.sourceCheckLines = {"old review"};
        state.capture.active = state.capture.hasObservation = state.capturePresented = true;
        state.captureObservation = std::make_unique<HardwareCaptureObservation>(profile_capture_proposal_fixture::curve());
        auto& observation = *state.captureObservation;
        observation.osFamily = PROFILE_SOURCE_OS_LINUX; observation.gpuGeneration = state.service.state.gpuGeneration;
        observation.binding = hardware_capture_binding(*linux_tui_authoritative_gpu(&state.service));
        observation.fingerprint = hardware_capture_fingerprint(observation);
        observation.revision = service_control_review_revision(observation.fingerprint,
            state.service.state.activeDesiredValid ? &state.service.desired : nullptr);
        state.captureReviewContext = linux_tui_review_context(state.service);
        state.captureProposals = std::make_unique<ProfileCaptureProposals>(); char why[192]{};
        (void)profile_capture_prepare_proposals(observation, {}, *state.captureProposals, why, sizeof(why));
        state.capture.representations = profile_capture_available_representations(*state.captureProposals);
        state.captureLines = {"old capture"};
    };
    for (int changed = 0; changed < 8; ++changed) {
        daemon_reset(false); TuiState* state = fresh_state(LINUX_CONFIGURED_GPU_NONE, 0);
        state->configuredGpu.target = {}; if (!tui_refresh_service(state, false)) return 9279;
        stage(*state);
        auto& next = g_daemon.published;
        if (changed == 0) ++next.state.serviceInstanceId;
        else if (changed == 1) ++next.state.gpuGeneration;
        else if (changed == 2) ++next.state.topologySignature;
        else if (changed == 3) next.state.validSections &= ~SERVICE_STATE_SECTION_CURVE_TOPOLOGY;
        else if (changed == 4) next.snapshot.health.availableMutationDomains &= ~SERVICE_MUTATION_DOMAIN_MEM_OFFSET;
        else if (changed == 5) next.state.gpuPhase = SERVICE_GPU_PHASE_RECOVERING;
        else if (changed == 6) { next.state.activeDesiredValid = 1; next.desired.hasGpuOffset = 1; }
        else ++state->document.revision;
        if (!tui_refresh_service(state, false) || state->sourceReview || state->sourceCheckVisible ||
            !state->sourceCheckLines.empty() || !state->document.valid || state->document.value.memoryOffsetHz != -123500000)
            return 9280;
        if (changed < 7 && (state->capture.hasObservation || state->captureProposals || state->captureObservation ||
            state->capturePresented || state->capture.representations || state->captureLines.empty())) return 9752;
        if (changed == 7 && (!state->capture.hasObservation || !state->captureProposals)) return 9753;
    }
    daemon_reset(false); TuiState* state = fresh_state(LINUX_CONFIGURED_GPU_NONE, 0);
    state->configuredGpu.target = {}; if (!tui_refresh_service(state, false)) return 9281;
    stage(*state); ++g_daemon.published.state.stateRevision;
    g_daemon.published.snapshot.gpuTemperatureC = 70;
    if (!tui_refresh_service(state, false) || !state->sourceReview || !state->sourceCheckVisible) return 9282;
    if (!state->capture.hasObservation || !state->captureProposals) return 9754;
    g_daemon.offline = true;
    if (tui_refresh_service(state, false) || state->sourceReview || state->sourceCheckVisible || !state->document.valid) return 9283;
    if (state->capture.hasObservation || state->captureProposals || state->captureObservation || state->capturePresented) return 9755;
    daemon_reset(false); state = fresh_state(LINUX_CONFIGURED_GPU_NONE, 0);
    state->configuredGpu.target = {}; if (!tui_refresh_service(state, false)) return 9284;
    stage(*state);
    if (!tui_select_gpu_step(state, 1) || shown_bus(state) != 7 || state->sourceReview ||
        state->sourceCheckVisible || state->document.value.memoryOffsetHz != -123500000) return 9285;
    if (state->capture.hasObservation || state->captureProposals || state->captureObservation) return 9756;
    return 0;
}
