// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_editor_capture.h"
#include "linux_tui_capture_binding.h"

static inline void tui_capture_cancel(TuiState& state) {
    state.capture = {}; state.capturePresented = false; state.captureObservation.reset(); state.captureLines.clear();
    state.captureProposals.reset(); state.captureProposalContext = {}; state.captureReviewContext = {};
    state.capturePinObservation.reset();
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
    state.capturePinObservation.reset();
    std::snprintf(state.status, sizeof(state.status), "Choose capture domains; Read performs a passive snapshot only");
    linux_debug_logf("profile-capture: stage=select domains=0x%03x", (unsigned)state.capture.domains);
}
template<class Read>
static inline void tui_capture_read(TuiState& state, Read read) {
    if (!state.capture.active || !state.capturePresented || !state.capture.domains) return;
    state.captureObservation.reset(); state.captureLines.clear();
    state.captureProposals.reset(); state.captureProposalContext = {}; state.capturePinObservation.reset();
    state.captureReviewContext = state.serviceOnline ? linux_tui_review_context(state.service) : LinuxTuiReviewContext{};
    state.capture.hasObservation = state.capture.allowPartial = state.capture.discardConfirmed = false;
    state.capture.pinObservable = state.capture.pinSuspected = false; state.capture.pinConfirmedHz = 0;
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
        state.capture.pinObservable = !profile_capture_pin_has_stronger_tier(captured, state.captureProposalContext) &&
            (captured.capturedDomains & PROFILE_DOMAIN_VF_CURVE) != 0 &&
            (captured.requestedDomains & PROFILE_DOMAIN_LOCK) != 0;
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
    state.capturePinObservation.reset();
    linux_debug_logf("profile-capture: stage=accepted source-draft=unsaved representation=%s", representation);
    return true;
}
template<class SampleFn>
static inline bool tui_capture_observe_pin(TuiState& state, SampleFn sampleFn) {
    tui_reconcile_capture(state, "GPU session or ownership changed");
    if (!state.capture.active || !state.capturePresented || !state.capture.hasObservation || !state.captureObservation ||
        !state.captureProposals || !state.capture.pinObservable) return false;
    if (profile_capture_pin_has_stronger_tier(*state.captureObservation, state.captureProposalContext)) {
        state.capture.pinObservable = state.capture.pinSuspected = false;
        snprintf(state.status, sizeof(state.status), "Tier 1 getter or Tier 2 applied intent already provides pin evidence");
        return false;
    }
    std::vector<PinWorkloadSample> samples;
    char err[192]{};
    if (!sampleFn(state.captureObservation->binding, samples, err, sizeof(err))) samples.clear();
    auto obs = std::make_unique<PinObservationResult>();
    profile_pin_observation_analyze(*state.captureObservation, false, samples.data(), (int)samples.size(), *obs);
    linux_debug_logf("%s", obs->summary);
    auto ignored = std::make_unique<ProfileDocument>();
    state.captureLines.clear();
    (void)profile_editor_capture_review(*state.captureObservation, false, PROFILE_CAPTURE_UNDECIDED, *ignored,
        state.captureLines, state.captureProposalContext);
    if (err[0] && samples.empty()) state.captureLines.emplace_back(std::string("Sampler note: ") + err);
    state.captureLines.emplace_back(obs->summary);
    state.captureLines.emplace_back(std::string("Workload pin observation: ") + obs->explanation);
    state.capture.scroll = 0;
    if (obs->status == PIN_OBS_SUSPECTED_PIN) {
        state.capture.pinSuspected = true;
        state.capture.pinConfirmedHz = obs->proposedPinHz;
        state.capturePinObservation = std::move(obs);
        snprintf(state.status, sizeof(state.status),
            "Suspected pin proposed (%lld MHz); adjust or use CONFIRM PIN to review PIN choice",
            (long long)(state.capture.pinConfirmedHz / 1000000LL));
        return true;
    }
    state.capture.pinSuspected = false;
    state.capture.pinConfirmedHz = 0;
    state.capturePinObservation.reset();
    snprintf(state.status, sizeof(state.status), "%s", obs->explanation);
    return false;
}
static inline bool tui_capture_step_pin(TuiState& state, gc_i64 deltaHz) {
    tui_reconcile_capture(state, "GPU session or ownership changed");
    if (!state.capture.active || !state.capture.pinSuspected || state.capture.pinConfirmedHz <= 0) return false;
    const gc_i64 nextHz = std::clamp(state.capture.pinConfirmedHz + deltaHz, 1000000LL, 100000000000LL);
    state.capture.pinConfirmedHz = (nextHz / 1000000LL) * 1000000LL;
    snprintf(state.status, sizeof(state.status), "Edited pin target: %lld MHz; CONFIRM PIN to apply proposal",
        (long long)(state.capture.pinConfirmedHz / 1000000LL));
    return true;
}
static inline bool tui_capture_confirm_pin(TuiState& state) {
    tui_reconcile_capture(state, "GPU session or ownership changed");
    if (!state.capture.active || !state.capturePresented || !state.capture.hasObservation || !state.captureObservation ||
        !state.captureProposals || !state.capturePinObservation ||
        state.capturePinObservation->status != PIN_OBS_SUSPECTED_PIN || state.capture.pinConfirmedHz <= 0) return false;
    if (!profile_capture_apply_pin_observation(*state.captureObservation, state.captureProposalContext,
            *state.capturePinObservation, *state.captureProposals, state.capture.pinConfirmedHz)) {
        snprintf(state.status, sizeof(state.status), "%s",
            state.captureProposals->pin.reason[0] ? state.captureProposals->pin.reason : "Pin target unavailable");
        return false;
    }
    state.capture.representations = profile_capture_available_representations(*state.captureProposals);
    auto ignored = std::make_unique<ProfileDocument>();
    state.captureLines.clear();
    (void)profile_editor_capture_review(*state.captureObservation, false, PROFILE_CAPTURE_UNDECIDED, *ignored,
        state.captureLines, state.captureProposalContext);
    state.captureLines.emplace_back(state.capturePinObservation->summary);
    struct {
        std::vector<std::string>& lines;
        void message(const std::string& text) { lines.emplace_back(text); }
    } sink{state.captureLines};
    profile_capture_report_proposals(sink, *state.captureProposals);
    linux_debug_logf("profile-capture: confirmed observed pin hz=%lld provenance=%d selectable=%d",
        (long long)state.captureProposals->pin.frequencyHz, (int)state.captureProposals->pin.provenance,
        state.captureProposals->pin.selectable() ? 1 : 0);
    snprintf(state.status, sizeof(state.status), "%s",
        state.captureProposals->pin.selectable()
            ? "Pin confirmed in capture options; select Representation: pin and USE PIN to stage draft"
            : state.captureProposals->pin.reason);
    return state.captureProposals->pin.selectable();
}
