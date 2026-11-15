// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "linux_tui_internal.h"
#include "linux_debug_log.h"
#include "profile_editor_capture.h"
#include "profile_review_fixture.h"
#include "profile_capture_proposal_fixture.h"

namespace tui_capture_proposals_fixture {
static void linux_debug_logf(const char *, ...) {}
#include "linux_tui_profile_load.h"
#include "linux_tui_capture_draft.h"

static int run() {
    auto state = std::make_unique<TuiState>();
    auto live = std::make_unique<ServiceResponse>();
    profile_review_fixture::live(*live);
    state->service = *live;
    state->serviceOnline = true;
    state->currentSlot = 3;
    state->document.valid = true;
    state->document.value = state->document.saved = profile_review_fixture::source();
    state->edit.active = true;
    std::snprintf(state->edit.text, sizeof(state->edit.text), "unfinished");
    int reads = 0, binds = 0;
    const auto encoded = [&] {
        auto bank = std::make_unique<ProfileBank>();
        bank->count = 1;
        bank->documents[0] = state->document.value;
        std::string text;
        (void)profile_bank_encode(*bank, text);
        return text;
    };
    const auto original = encoded();
    const auto bind = [&](TuiState &) { ++binds; };
    const auto read = [&](gc_u32 domains, HardwareCaptureObservation &output, ProfileCodecError &) {
        ++reads;
        live->capture = profile_capture_proposal_fixture::curve();
        live->capture.osFamily = PROFILE_SOURCE_OS_LINUX;
        live->capture.binding = hardware_capture_binding(live->snapshot.adapters[0]);
        live->capture.requestedDomains = domains;
        live->capture.fingerprint = hardware_capture_fingerprint(live->capture);
        live->capture.revision = service_control_review_revision(
            live->capture.fingerprint, live->state.activeDesiredValid ? &live->desired : nullptr);
        state->captureProposalContext = profile_capture_proposal_context(*live);
        output = live->capture;
        return true;
    };
    const auto begin = [&] {
        tui_capture_begin(*state);
        state->capture.domains = PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_GRAPHICS_OFFSET | PROFILE_DOMAIN_LOCK;
        state->capturePresented = true;
        tui_capture_read(*state, read);
        state->capturePresented = true;
    };
    begin();
    if (!state->capture.hasObservation || !state->captureProposals ||
        state->capture.representation != PROFILE_CAPTURE_EXACT ||
        !(state->capture.representations & profile_capture_representation_bit(PROFILE_CAPTURE_FLATTEN)) ||
        (state->capture.representations & profile_capture_representation_bit(PROFILE_CAPTURE_PIN)) ||
        encoded() != original || reads != 1)
        return 9740;
    if (!tui_capture_cycle_representation(*state) || state->capture.representation != PROFILE_CAPTURE_FLATTEN ||
        tui_capture_accept(*state, bind) || encoded() != original || !state->edit.active)
        return 9741;
    state->capture.allowPartial = true;
    if (tui_capture_accept(*state, bind))
        return 9742;
    state->capture.discardConfirmed = true;
    if (!tui_capture_cycle_representation(*state) || state->capture.representation != PROFILE_CAPTURE_PIN ||
        tui_capture_accept(*state, bind) || encoded() != original || binds)
        return 9743;
    state->capture.representation = PROFILE_CAPTURE_FLATTEN;
    if (!tui_capture_accept(*state, bind) || binds != 1 || state->captureProposals || state->captureObservation ||
        state->captureReviewContext.serviceInstanceId || !state->document.unsaved ||
        state->document.value.lock.mode != LOCK_MODE_FLATTEN || state->document.value.lock.anchorTopologyId != 22 ||
        state->document.value.points[1].kind != PROFILE_POINT_ABSENT ||
        state->document.value.points[1].observedOffsetHz != -100000500 || state->edit.active || state->draftAttached ||
        reads != 1)
        return 9744;
    live->state.activeDesiredValid = 1;
    live->desired.hasLock = 1;
    live->desired.lockMode = LOCK_MODE_HARD;
    live->desired.lockCi = 22;
    live->desired.lockMHz = 1000;
    state->service = *live;
    const auto ownership = service_control_review_revision(0, &state->service.desired);
    begin();
    if (!state->capture.hasObservation || !state->captureProposals->pin.selectable())
        return 9745;
    state->capture.representation = PROFILE_CAPTURE_PIN;
    state->capture.allowPartial = state->capture.discardConfirmed = true;
    live->capture.points[1].offsetHz = -777;
    if (!tui_capture_accept(*state, bind) || binds != 2 || state->document.value.lock.mode != LOCK_MODE_HARD ||
        state->document.value.lock.provenance != PROFILE_LOCK_PROVENANCE_APPLIED_INTENT ||
        state->document.value.source.pinKnowledge != PROFILE_PIN_UNKNOWN ||
        state->document.value.points[1].observedOffsetHz != -100000500 ||
        service_control_review_revision(0, &state->service.desired) != ownership)
        return 9746;
    const auto accepted = encoded();
    for (int change = 0; change < 5; ++change) {
        state->service = *live;
        begin();
        if (!state->capture.hasObservation)
            return 9747;
        switch (change) {
        case 0:
            ++state->service.state.serviceInstanceId;
            break;
        case 1:
            ++state->service.state.gpuGeneration;
            break;
        case 2:
            ++state->service.desired.lockMHz;
            break;
        case 3:
            state->serviceOnline = false;
            break;
        case 4:
            ++state->service.snapshot.adapters[0].pciBus;
            break;
        }
        tui_reconcile_capture(*state, "fixture changed");
        if (state->capture.hasObservation || state->captureProposals || state->captureObservation ||
            state->capture.representations || state->capturePresented || tui_capture_accept(*state, bind) ||
            encoded() != accepted || binds != 2)
            return 9748;
        tui_capture_cancel(*state);
        state->serviceOnline = true;
    }
    state->service = *live;
    live->snapshot.health.availableMutationDomains &= ~SERVICE_MUTATION_DOMAIN_LOCK;
    state->service = *live;
    begin();
    if (!state->capture.hasObservation || state->captureProposals->pin.selectable() ||
        (state->capture.representations & profile_capture_representation_bit(PROFILE_CAPTURE_PIN)))
        return 9749;
    state->capture.representation = (ProfileCaptureRepresentation)-1;
    if (tui_capture_accept(*state, bind) || !tui_capture_cycle_representation(*state) ||
        state->capture.representation != PROFILE_CAPTURE_EXACT || encoded() != accepted)
        return 9750;
    TuiViewModel vm{};
    DesiredSettings display{};
    vm.desired = &display;
    vm.capture = state->capture;
    vm.capture.report = &state->captureLines;
    vm.capture.allowPartial = vm.capture.discardConfirmed = true;
    vm.tab = TUI_TAB_PROFILES;
    vm.currentSlot = 3;
    for (auto size : {TuiRect{0, 0, 72, 24}, TuiRect{0, 0, 100, 32}, TuiRect{0, 0, 160, 48}}) {
        for (auto representation : {PROFILE_CAPTURE_EXACT, PROFILE_CAPTURE_FLATTEN, PROFILE_CAPTURE_PIN}) {
            vm.capture.representation = representation;
            TuiLayout layout{};
            build_tui_layout(vm, size.width, size.height, &layout);
            bool choice = false, accept = false;
            for (const auto &action : layout.actions) {
                choice |= action.type == ACTION_CAPTURE_REPRESENTATION;
                accept |= action.type == ACTION_CAPTURE_ACCEPT;
            }
            const bool available =
                (vm.capture.representations & profile_capture_representation_bit(representation)) != 0;
            if (!tui_layout_actions_valid(layout) || !choice || accept != available)
                return 9751;
        }
    }
    tui_capture_cancel(*state);
    state->document.unsaved = false;
    state->document.saved = state->document.value;
    state->edit = {};
    state->dirty = false;
    begin();
    if (!state->capture.hasObservation || state->capture.needsDiscard)
        return 9761;
    state->edit.active = true;
    std::snprintf(state->edit.text, sizeof(state->edit.text), "changed after read");
    state->capture.allowPartial = true;
    if (tui_capture_accept(*state, bind) || !state->capture.needsDiscard || state->capture.discardConfirmed ||
        state->capturePresented || encoded() != accepted || std::strcmp(state->edit.text, "changed after read") ||
        binds != 2)
        return 9762;
    state->capturePresented = true;
    state->capture.discardConfirmed = true;
    if (!tui_capture_accept(*state, bind) || binds != 3 || state->edit.active || !state->document.unsaved ||
        state->document.value.lock.present || state->document.value.points[1].capturedOffsetHz != -100000500)
        return 9763;
    return 0;
}
} // namespace tui_capture_proposals_fixture
int run_tui_capture_proposals_tests() { return tui_capture_proposals_fixture::run(); }
