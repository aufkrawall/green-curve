// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "profile_review_fixture.h"
#include "profile_mapped_draft.h"
#include "linux_tui_mapped_draft.h"
#include "service_control_review.h"
#include "gui_service_actionability_policy.h"

int run_profile_mapped_draft_tests() {
    auto state = std::make_unique<TuiState>();
    auto live = std::make_unique<ServiceResponse>();
    profile_review_fixture::live(*live);
    state->document.valid = true;
    state->document.value = state->document.saved = profile_review_fixture::source();
    state->document.revision = 29;
    state->serviceOnline = true;
    state->service = *live;
    state->sourceReviewContext = linux_tui_review_context(*live);
    profile_review_fixture::capture(*live, state->document.value);
    state->sourceReview = std::make_unique<ProfileDestinationReview>();
    auto &review = *state->sourceReview;
    auto &source = state->document.value;
    std::vector<std::string> lines;
    if (profile_editor_check(
            source, 0, 0,
            [&](gc_u32 domains, auto &out, auto &) {
                if (domains != 511u)
                    return false;
                out = live->capture;
                return true;
            },
            lines, &review, 29) != PROFILE_COMMAND_DECISION ||
        !profile_mapped_draft_ready(review, source, 29))
        return 9630;
    DesiredSettings desired{};
    desired.hasGpuOffset = 1;
    gc_u64 token = 123;
    if (profile_mapped_draft_prepare(review, source, 29, desired, token) || !desired.hasGpuOffset || token != 123 ||
        tui_use_mapped_draft(*state))
        return 9631; // Report must be explicitly presented first.
    state->sourceCheckVisible = true;
    if (!tui_use_mapped_draft(*state) || !review.accepted || !tui_mapped_draft_available(*state) ||
        !profile_mapped_draft_prepare(review, source, 29, desired, token) || desired.hasGpuOffset ||
        desired.memOffsetHz != -123500000 || desired.memOffsetSurface != CLOCK_OFFSET_SURFACE_P0 ||
        desired.applicationScope != DESIRED_SCOPE_UPDATE_INCLUDED || token != live->capture.revision ||
        profile_editor_document_dirty(state->document) || state->service.state.activeDesiredValid ||
        state->draftAttached)
        return 9632;
    // Acceptance is neither a rounded live projection nor a local save/ownership acknowledgment.
    if (source.memoryOffsetHz != -123500000 || state->document.saved.memoryOffsetHz != source.memoryOffsetHz)
        return 9633;
    ServiceRequest request{};
    request.magic = SERVICE_PROTOCOL_MAGIC;
    request.version = SERVICE_PROTOCOL_VERSION;
    request.command = SERVICE_CMD_APPLY;
    request.flags = SERVICE_REQUEST_FLAG_INTERACTIVE;
    request.applyOrigin = SERVICE_APPLY_ORIGIN_GUI;
    request.callerPid = 1;
    request.operationId = 5;
    request.targetGpu = live->snapshot.adapters[0];
    request.desired = desired;
    request.expectedServiceInstanceId = live->state.serviceInstanceId;
    request.expectedGpuGeneration = live->state.gpuGeneration;
    request.expectedTopologySignature = live->state.topologySignature;
    request.expectedControlFingerprint = token;
    request.expectedControlDomains = 511u;
    char why[256]{};
    int reads = 0;
    const auto read = [&](HardwareCaptureObservation &out, char *, size_t) {
        ++reads;
        out = live->capture;
        return true;
    };
    if (service_request_reject_reason(&request) ||
        !service_control_review_matches(request, nullptr, read, why, sizeof(why)) || reads != 1)
        return 9647;
    live->capture.memory.value += 500000;
    live->capture.fingerprint = hardware_capture_fingerprint(live->capture);
    if (service_control_review_matches(request, nullptr, read, why, sizeof(why)) || reads != 2)
        return 9648;
    live->capture = review.capture;
    GuiServiceActionability action{};
    action.installed = action.available = action.ready = action.loaded = true;
    action.draftDetached = true;
    if (gui_reviewed_source_apply_enabled(action, false) || !gui_reviewed_source_apply_enabled(action, true))
        return 9649;
    action.hardwareWriteInFlight = true;
    if (gui_reviewed_source_apply_enabled(action, true))
        return 9670;
    action.hardwareWriteInFlight = false;
    action.ready = false;
    if (gui_reviewed_source_apply_enabled(action, true))
        return 9671;
    TuiViewModel vm{};
    vm.desired = &desired;
    vm.sourceDocument = &source;
    vm.sourceCheckLines = &lines;
    vm.sourceMappedReady = true;
    vm.tab = TUI_TAB_PROFILES;
    vm.currentSlot = 1;
    for (const auto dimensions : {TuiRect{0, 0, 72, 24}, TuiRect{0, 0, 100, 32}, TuiRect{0, 0, 160, 48}}) {
        TuiLayout layout{};
        build_tui_layout(vm, dimensions.width, dimensions.height, &layout);
        bool found = false;
        for (const auto &item : layout.actions)
            found |= item.type == ACTION_USE_MAPPED;
        if (!found || !tui_layout_actions_valid(layout))
            return 9672;
    }
    ++state->document.revision;
    if (tui_mapped_draft_available(*state) || profile_mapped_draft_prepare(review, source, 30, desired, token))
        return 9634;
    --state->document.revision;
    source.source.driverVersion[0] = 'x';
    if (profile_mapped_draft_ready(review, source, 29))
        return 9635; // Metadata edits are source changes too.
    source = state->document.saved;
    const auto capture = review.capture;
    ++review.capture.memory.value;
    if (profile_mapped_draft_prepare(review, source, 29, desired, token))
        return 9636;
    review.capture = capture;
    review.capture.requestedDomains = PROFILE_DOMAIN_MEMORY;
    if (profile_mapped_draft_ready(review, source, 29))
        return 9637;
    review.capture = capture;
    review.controls.disposition = PROFILE_CONTROL_REFUSED;
    if (profile_mapped_draft_accept(review, source, 29) || review.accepted)
        return 9638;
    review.controls.disposition = PROFILE_CONTROL_UNAVAILABLE;
    if (profile_mapped_draft_accept(review, source, 29))
        return 9639;
    review.controls.disposition = PROFILE_CONTROL_CONDITIONAL;
    if (!profile_mapped_draft_accept(review, source, 29))
        return 9640;
    state->edit.active = true;
    if (tui_mapped_draft_available(*state))
        return 9641;
    state->edit.active = false;
    ++state->service.state.serviceInstanceId;
    if (tui_mapped_draft_available(*state))
        return 9642;
    state->service = *live;
    state->service.capture = {};
    state->service.state.gpuPhase = SERVICE_GPU_PHASE_RECOVERING;
    if (tui_mapped_draft_available(*state))
        return 9643;
    state->service.state.gpuPhase = SERVICE_GPU_PHASE_READY;
    state->service.desired.hasMemOffset = 1;
    state->service.state.activeDesiredValid = 1;
    if (tui_mapped_draft_available(*state))
        return 9644;
    state->service.state.activeDesiredValid = 0;
    state->service.desired = {};
    if (!tui_mapped_draft_available(*state))
        return 9645;
    state->service.state.validSections = SERVICE_STATE_SECTION_ADAPTER_IDENTITY;
    if (tui_mapped_draft_available(*state))
        return 9674;
    state->service = *live;
    state->capture.active = true;
    if (tui_mapped_draft_available(*state))
        return 9675;
    state->capture.active = false;
    state->transfer.active = true;
    if (tui_mapped_draft_available(*state))
        return 9676;
    state->transfer.active = false;
    state->profileLoadPending = true;
    if (tui_mapped_draft_available(*state))
        return 9677;
    state->profileLoadPending = false;
    review.mapping.structurallyValid = false;
    if (profile_mapped_draft_accept(review, source, 29))
        return 9646;
    return 0;
}
