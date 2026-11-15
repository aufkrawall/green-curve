// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_editor_capture.h"
#include "linux_tui_capture_binding.h"

static inline void tui_capture_cancel(TuiState& state) {
    state.capture = {}; state.capturePresented = false; state.captureObservation.reset(); state.captureLines.clear();
    state.captureProposals.reset(); state.captureProposalContext = {}; state.captureReviewContext = {};
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
    state.captureProposals.reset(); state.captureProposalContext = {}; state.captureReviewContext = {};
    std::snprintf(state.status, sizeof(state.status), "Choose capture domains; Read performs a passive snapshot only");
    linux_debug_logf("profile-capture: stage=select domains=0x%03x", (unsigned)state.capture.domains);
}
template<class Read>
static inline void tui_capture_read(TuiState& state, Read read) {
    if (!state.capture.active || !state.capturePresented || !state.capture.domains) return;
    state.captureObservation.reset(); state.captureLines.clear();
    state.captureProposals.reset(); state.captureProposalContext = {};
    state.captureReviewContext = state.serviceOnline ? linux_tui_review_context(state.service) : LinuxTuiReviewContext{};
    state.capture.hasObservation = state.capture.allowPartial = state.capture.discardConfirmed = false;
    state.capture.omissions = 0; state.capture.scroll = 0; state.capturePresented = false; state.focusIndex = -1;
    state.capture.representations = 0; state.capture.representation = PROFILE_CAPTURE_EXACT;
    auto observation = std::make_unique<HardwareCaptureObservation>(); ProfileCodecError error;
    if (!read(state.capture.domains, *observation, error) || !observation->valid ||
        !hardware_capture_observation_valid(*observation) || observation->requestedDomains != state.capture.domains) {
        state.captureLines.emplace_back(std::string("Capture unavailable: ") + (error.message[0] ? error.message : "invalid or different snapshot"));
        std::snprintf(state.status, sizeof(state.status), "Capture failed; previous draft kept");
        linux_debug_logf("profile-capture: stage=read-failed domains=0x%03x", (unsigned)state.capture.domains);
        return;
    }
    state.captureObservation = std::move(observation);
    if (!tui_capture_binding_current(state)) {
        state.captureObservation.reset(); state.captureLines.emplace_back("Capture binding changed; Read the current GPU again.");
        std::snprintf(state.status, sizeof(state.status), "Capture changed during Read; previous draft kept"); return;
    }
    auto ignored = std::make_unique<ProfileDocument>(); auto proposals = std::make_unique<ProfileCaptureProposals>();
    const auto& captured = *state.captureObservation;
    const int code = profile_editor_capture_review(captured, false, PROFILE_CAPTURE_UNDECIDED, *ignored, state.captureLines,
        state.captureProposalContext, proposals.get());
    state.capture.omissions = captured.omittedDomains;
    state.capture.hasObservation = code == PROFILE_COMMAND_DECISION && captured.capturedDomains != 0;
    if (state.capture.hasObservation) {
        state.capture.representations = profile_capture_available_representations(*proposals);
        state.capture.representation = proposals->suggested; state.captureProposals = std::move(proposals);
    }
    std::snprintf(state.status, sizeof(state.status), "Review settings, evidence and omissions; choose a representation for the draft");
    linux_debug_logf("profile-capture: stage=review domains=0x%03x omitted=0x%03x code=%d", (unsigned)state.capture.domains,
        (unsigned)state.capture.omissions, code);
}
static inline bool tui_capture_cycle_representation(TuiState& state) {
    tui_reconcile_capture(state, "GPU session or ownership changed");
    if (!state.capture.active || !state.capturePresented || !state.capture.hasObservation || !state.captureProposals) return false;
    const int current = (int)state.capture.representation;
    state.capture.representation = (ProfileCaptureRepresentation)(current >= 1 && current <= 3 ? current % 3 + 1 : 1);
    const bool available = (state.capture.representations & profile_capture_representation_bit(state.capture.representation)) != 0;
    snprintf(state.status, sizeof(state.status), "Representation: %s; %s",
        profile_capture_representation_name(state.capture.representation), available ? "review evidence before accepting" : "unavailable in this snapshot; choose another");
    linux_debug_logf("profile-capture: representation=%s selectable=%d", profile_capture_representation_name(state.capture.representation), available ? 1 : 0);
    return true;
}
template<class Bind>
static inline bool tui_capture_accept(TuiState& state, Bind bind) {
    tui_reconcile_capture(state, "GPU session or ownership changed");
    const bool dirty = state.edit.active || (state.document.valid ? profile_editor_document_dirty(state.document) : state.dirty);
    if (state.capture.active && dirty && !state.capture.needsDiscard) {
        state.capture.needsDiscard = true; state.capture.discardConfirmed = false;
        state.capturePresented = false; state.focusIndex = -1;
        snprintf(state.status, sizeof(state.status), "Confirm replacing the current unsaved or unfinished draft before accepting capture");
        linux_debug_logf("profile-capture: discard review became required");
        return false;
    }
    if (!state.capture.active || !state.capturePresented || !state.capture.hasObservation || !state.captureObservation ||
        !state.captureProposals || !(state.capture.representations & profile_capture_representation_bit(state.capture.representation)) ||
        (state.capture.omissions && !state.capture.allowPartial) || (state.capture.needsDiscard && !state.capture.discardConfirmed)) return false;
    auto source = std::make_unique<ProfileDocument>();
    char why[192]{};
    if (!profile_capture_select_proposal(*state.captureProposals, state.capture.representation, *source, why, sizeof(why))) {
        std::snprintf(state.status, sizeof(state.status), "Capture representation unavailable: %s", why);
        return false;
    }
    auto candidate = std::make_unique<ProfileEditorCandidate>(); profile_editor_source_candidate(*source, *candidate);
    const char* representation = profile_capture_representation_name(state.capture.representation);
    candidate->source.unsaved = true;
    tui_install_profile_load(state, *candidate, state.currentSlot, bind, true);
    state.capture = {}; state.capturePresented = false; state.captureObservation.reset(); state.captureLines.clear(); state.focusIndex = -1;
    state.captureProposals.reset(); state.captureProposalContext = {}; state.captureReviewContext = {};
    linux_debug_logf("profile-capture: stage=accepted source-draft=unsaved representation=%s", representation);
    return true;
}
