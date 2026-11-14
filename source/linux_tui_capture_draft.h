// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_editor_capture.h"

static inline void tui_capture_cancel(TuiState& state) {
    state.capture = {}; state.capturePresented = false; state.captureObservation.reset(); state.captureLines.clear();
    state.focusIndex = -1;
    std::snprintf(state.status, sizeof(state.status), "Capture cancelled; previous draft kept");
    linux_debug_logf("profile-capture: stage=cancelled");
}
static inline void tui_capture_begin(TuiState& state) {
    if (state.profileLoadPending || state.capture.active) return;
    state.capture = {}; state.capture.active = true;
    state.capture.needsDiscard = state.edit.active ||
        (state.document.valid ? profile_editor_document_dirty(state.document) : state.dirty);
    state.capturePresented = false; state.captureObservation.reset(); state.captureLines.clear(); state.focusIndex = -1;
    std::snprintf(state.status, sizeof(state.status), "Choose capture domains; Read performs a passive snapshot only");
    linux_debug_logf("profile-capture: stage=select domains=0x%03x", (unsigned)state.capture.domains);
}
template<class Read>
static inline void tui_capture_read(TuiState& state, Read read) {
    if (!state.capture.active || !state.capturePresented || !state.capture.domains) return;
    state.captureObservation.reset(); state.captureLines.clear();
    state.capture.hasObservation = state.capture.allowPartial = state.capture.discardConfirmed = false;
    state.capture.omissions = 0; state.capture.scroll = 0; state.capturePresented = false; state.focusIndex = -1;
    auto observation = std::make_unique<HardwareCaptureObservation>(); ProfileCodecError error;
    if (!read(state.capture.domains, *observation, error) || !observation->valid ||
        !hardware_capture_observation_valid(*observation) || observation->requestedDomains != state.capture.domains) {
        state.captureLines.emplace_back(std::string("Capture unavailable: ") + (error.message[0] ? error.message : "invalid or different snapshot"));
        std::snprintf(state.status, sizeof(state.status), "Capture failed; previous draft kept");
        linux_debug_logf("profile-capture: stage=read-failed domains=0x%03x", (unsigned)state.capture.domains);
        return;
    }
    auto ignored = std::make_unique<ProfileDocument>();
    const int code = profile_editor_capture(*observation, false, false, *ignored, state.captureLines);
    state.capture.omissions = observation->omittedDomains;
    state.capture.hasObservation = code == PROFILE_COMMAND_DECISION && observation->capturedDomains != 0;
    state.captureObservation = std::move(observation);
    std::snprintf(state.status, sizeof(state.status), "Review the snapshot; Use exact controls changes only the draft");
    linux_debug_logf("profile-capture: stage=review domains=0x%03x omitted=0x%03x code=%d", (unsigned)state.capture.domains,
        (unsigned)state.capture.omissions, code);
}
template<class Bind>
static inline bool tui_capture_accept(TuiState& state, Bind bind) {
    if (!state.capture.active || !state.capturePresented || !state.capture.hasObservation || !state.captureObservation ||
        (state.capture.omissions && !state.capture.allowPartial) || (state.capture.needsDiscard && !state.capture.discardConfirmed)) return false;
    auto source = std::make_unique<ProfileDocument>();
    if (profile_editor_capture(*state.captureObservation, state.capture.allowPartial, true, *source, state.captureLines) != PROFILE_COMMAND_OK)
        return false;
    auto candidate = std::make_unique<ProfileEditorCandidate>(); profile_editor_source_candidate(*source, *candidate);
    candidate->source.unsaved = true;
    tui_install_profile_load(state, *candidate, state.currentSlot, bind, true);
    state.capture = {}; state.capturePresented = false; state.captureObservation.reset(); state.captureLines.clear(); state.focusIndex = -1;
    linux_debug_logf("profile-capture: stage=accepted source-draft=unsaved");
    return true;
}
