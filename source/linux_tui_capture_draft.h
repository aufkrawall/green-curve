// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
// TUI "Import current GPU settings", the same model as the Windows dialog:
// read the shown GPU once on open, show each setting group in plain words,
// let the user leave groups out and pick how the curve is saved, then save an
// ordinary slot record (profile_capture_save.h). The editor draft, the GPU
// and Green Curve's ownership are never changed.
#include "profile_editor_capture.h"
#include "profile_capture_save.h"
#include "linux_tui_capture_binding.h"
#include "linux_tui_slot_state.h"

static inline void tui_capture_clear(TuiState& state) {
    state.captureObservation.reset(); state.captureProposals.reset(); state.captureProposalContext = {};
    state.captureReviewContext = {}; state.capturePinObservation.reset(); state.captureLines.clear();
    state.capture.hasObservation = state.capture.pinObservable = false;
    state.capture.included = state.capture.available = state.capture.shown = state.capture.representations = 0;
    state.capture.choice.clear();
}
static inline void tui_capture_cancel(TuiState& state) {
    tui_capture_clear(state);
    state.capture = {}; state.capturePresented = false; state.focusIndex = -1;
    std::snprintf(state.status, sizeof(state.status), "Import of current GPU settings closed; nothing was saved");
    linux_debug_logf("profile-capture: stage=cancelled");
}
static inline void tui_capture_describe_choice(TuiState& state) {
    state.capture.choice = state.captureProposals
        ? profile_capture_choice_label(*state.captureProposals, state.capture.representation) : std::string();
}
template<class IO> static inline void tui_capture_describe_slot(TuiState& state, IO& storage) {
    bool saved[6];
    tui_profile_slots_saved(storage, state.configPath, saved);
    state.capture.destination = tui_slot_choice_label(state.capture.slot, saved);
}
static inline bool tui_capture_pin_observable(const TuiState& state) {
    if (!state.captureObservation || !state.captureProposals) return false;
    const auto& o = *state.captureObservation;
    return !profile_capture_pin_has_stronger_tier(o, state.captureProposalContext) &&
        (o.capturedDomains & PROFILE_DOMAIN_VF_CURVE) && (o.requestedDomains & PROFILE_DOMAIN_LOCK) &&
        !(state.capturePinObservation && state.captureProposals->pin.selectable());
}
template<class Read>
static inline void tui_capture_read(TuiState& state, Read read) {
    if (!state.capture.active) return;
    tui_capture_clear(state);
    state.capture.failed = false; state.capture.scroll = 0;
    state.captureReviewContext = state.serviceOnline ? linux_tui_review_context(state.service) : LinuxTuiReviewContext{};
    auto observation = std::make_unique<HardwareCaptureObservation>(); ProfileCodecError error;
    if (!read(PROFILE_CAPTURE_DEFAULT_DOMAINS, *observation, error) || !observation->valid ||
        !hardware_capture_observation_valid(*observation) || observation->requestedDomains != PROFILE_CAPTURE_DEFAULT_DOMAINS) {
        state.capture.failed = true;
        state.captureLines.emplace_back(std::string("Could not read the GPU: ") + (error.message[0] ? error.message : "no snapshot") + ".");
        std::snprintf(state.status, sizeof(state.status), "Reading the GPU failed; nothing was changed");
        linux_debug_logf("profile-capture: stage=read-failed");
        return;
    }
    state.captureObservation = std::move(observation);
    if (!tui_capture_binding_current(state)) {
        tui_capture_clear(state); state.capture.failed = true;
        state.captureLines.emplace_back("The GPU session changed while reading. Read again.");
        return;
    }
    auto ignored = std::make_unique<ProfileDocument>(); auto proposals = std::make_unique<ProfileCaptureProposals>();
    std::vector<std::string> report;
    const auto& captured = *state.captureObservation;
    const int code = profile_editor_capture_review(captured, true, PROFILE_CAPTURE_UNDECIDED, *ignored, report,
        state.captureProposalContext, proposals.get());
    if (code != PROFILE_COMMAND_DECISION || !captured.capturedDomains) {
        tui_capture_clear(state); state.capture.failed = true;
        state.captureLines.emplace_back("No GPU settings could be read. Check the daemon and read again.");
        return;
    }
    state.captureProposals = std::move(proposals);
    const auto summary = profile_capture_summary(captured, state.captureProposals.get());
    for (int g = 0; g < PROFILE_CAPTURE_GROUP_COUNT; ++g) {
        const auto& row = profile_capture_group_row(summary, g);
        state.captureLines.push_back(row.text);
        if (profile_capture_group_shown(summary, g)) state.capture.shown |= 1u << g;
        if (row.available) state.capture.available |= 1u << g;
    }
    state.capture.included = state.capture.available & state.capture.shown;
    state.capture.hasObservation = true;
    state.capture.representations = profile_capture_available_representations(*state.captureProposals);
    state.capture.representation = state.captureProposals->suggested;
    state.capture.pinObservable = tui_capture_pin_observable(state);
    tui_capture_describe_choice(state);
    std::snprintf(state.status, sizeof(state.status), "Choose what to include and the slot to save it to");
    linux_debug_logf("profile-capture: stage=review captured=0x%03x omitted=0x%03x choices=0x%x",
        (unsigned)captured.capturedDomains, (unsigned)captured.omittedDomains, state.capture.representations);
}
template<class Read, class IO>
static inline void tui_capture_begin(TuiState& state, Read read, IO& storage) {
    if (state.profileLoadPending || state.capture.active || state.transfer.active) return;
    state.capture = {}; state.capture.active = true; state.capturePresented = false; state.focusIndex = -1;
    bool saved[6];
    tui_profile_slots_saved(storage, state.configPath, saved);
    state.capture.slot = profile_capture_default_slot(state.currentSlot, [&](int s) { return saved[s]; });
    tui_capture_describe_slot(state, storage);
    linux_debug_logf("profile-capture: stage=opened slot=%d", state.capture.slot);
    tui_capture_read(state, read);
}
static inline bool tui_capture_toggle_group(TuiState& state, int group) {
    tui_reconcile_capture(state, "the GPU session or ownership changed");
    if (!state.capture.hasObservation || group < 0 || group >= PROFILE_CAPTURE_GROUP_COUNT ||
        !(state.capture.available & state.capture.shown & (1u << group))) return false;
    state.capture.included ^= 1u << group;
    return true;
}
static inline bool tui_capture_cycle_representation(TuiState& state) {
    tui_reconcile_capture(state, "the GPU session or ownership changed");
    if (!state.capture.hasObservation || !state.captureProposals) return false;
    for (int step = 0; step < 3; ++step) {
        const int current = (int)state.capture.representation;
        state.capture.representation = (ProfileCaptureRepresentation)(current >= 1 && current <= 3 ? current % 3 + 1 : 1);
        if (state.capture.representations & profile_capture_representation_bit(state.capture.representation)) break;
    }
    tui_capture_describe_choice(state);
    return true;
}
template<class IO> static inline bool tui_capture_cycle_slot(TuiState& state, IO& storage) {
    if (!state.capture.active) return false;
    state.capture.slot = state.capture.slot % 5 + 1;
    tui_capture_describe_slot(state, storage);
    return true;
}
template<class SampleFn>
static inline bool tui_capture_observe_pin(TuiState& state, SampleFn sampleFn) {
    tui_reconcile_capture(state, "the GPU session or ownership changed");
    if (!state.capture.hasObservation || !tui_capture_pin_observable(state)) return false;
    std::vector<PinWorkloadSample> samples;
    char err[192]{};
    if (!sampleFn(state.captureObservation->binding, samples, err, sizeof(err))) samples.clear();
    auto obs = std::make_unique<PinObservationResult>();
    profile_pin_observation_analyze(*state.captureObservation, false, samples.data(), (int)samples.size(), *obs);
    linux_debug_logf("%s", obs->summary);
    if (obs->status != PIN_OBS_SUSPECTED_PIN ||
        !profile_capture_apply_pin_observation(*state.captureObservation, state.captureProposalContext, *obs,
            *state.captureProposals, obs->proposedPinHz) || !state.captureProposals->pin.selectable()) {
        snprintf(state.status, sizeof(state.status), "No clock lock detected: %s", obs->explanation);
        return false;
    }
    state.capturePinObservation = std::move(obs);
    state.capture.representations = profile_capture_available_representations(*state.captureProposals);
    state.capture.representation = PROFILE_CAPTURE_PIN;
    state.capture.pinObservable = false;
    tui_capture_describe_choice(state);
    snprintf(state.status, sizeof(state.status), "Detected a clock lock under load; check the frequency before saving");
    return true;
}
// `save(slot, record, error, size)` writes the ordinary slot record.
template<class Save>
static inline bool tui_capture_accept(TuiState& state, Save save) {
    tui_reconcile_capture(state, "the GPU session or ownership changed");
    if (!state.capture.active || !state.capture.hasObservation || !state.captureObservation || !state.captureProposals) return false;
    auto record = std::make_unique<DesiredSettings>();
    char why[192]{};
    if (!profile_capture_record(*state.captureObservation, *state.captureProposals, state.capture.representation,
            state.capture.included, *record, why, sizeof(why))) {
        snprintf(state.status, sizeof(state.status), "Cannot save: %s", why);
        return false;
    }
    const int slot = state.capture.slot;
    char error[256]{};
    if (!save(slot, *record, error, sizeof(error))) {
        snprintf(state.status, sizeof(state.status), "Saving failed: %s", error);
        return false;
    }
    linux_debug_logf("profile-capture: stage=saved slot=%d representation=%s included=0x%x",
        slot, profile_capture_representation_name(state.capture.representation), state.capture.included);
    tui_capture_clear(state);
    state.capture = {}; state.capturePresented = false; state.focusIndex = -1;
    snprintf(state.status, sizeof(state.status), "Saved the GPU's current settings to slot %d; apply it to let Green Curve manage them", slot);
    return true;
}
