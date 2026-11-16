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

    // UNIT-07 & CAP-10 TUI actions: batch conversion updates draft while saved stays exact; tail restore works offline without recapture.
    auto convDoc = profile_review_fixture::source();
    convDoc.memoryOffsetHz = -500;
    state->document.valid = true; state->document.value = state->document.saved = convDoc;
    state->document.revision = 40;
    state->service = *live;
    state->sourceReviewContext = linux_tui_review_context(*live);
    profile_review_fixture::capture(*live, state->document.value);
    state->sourceReview = std::make_unique<ProfileDestinationReview>();
    if (profile_editor_check(state->document.value, 0, 0,
            [&](gc_u32 domains, auto& out, auto&) { if (domains != 511u) return false; out = live->capture; return true; },
            lines, state->sourceReview.get(), 40) != PROFILE_COMMAND_DECISION ||
        !state->sourceReview->conversions.anyConvertible()) return 9678;
    state->sourceCheckVisible = true;
    char logLine[256]{};
    if (!tui_cycle_source_conversion(*state) || !tui_accept_source_conversion(*state, logLine, sizeof(logLine)) ||
        !strstr(logLine, "profile-map: domain=memory") || !strstr(logLine, "decision=accepted") ||
        state->document.value.memoryOffsetHz != -500000 || state->document.saved.memoryOffsetHz != -500 ||
        state->document.revision != 41 || state->sourceReview != nullptr) return 9679;
    auto flat = profile_document_make_empty();
    flat.domains = PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_LOCK;
    flat.topologyCount = 2; flat.sourceTopologyComplete = true;
    flat.topology[0] = {10, 22, 662000, 1}; flat.topology[1] = {11, 23, 668250, 1};
    flat.points[0] = {PROFILE_POINT_CAPTURED_OFFSET, 0, 15000500, false, 0, true, 15000500};
    flat.points[1] = {PROFILE_POINT_ABSENT, 0, 0, false, 0, true, -25000500};
    flat.lock.present = true; flat.lock.mode = LOCK_MODE_FLATTEN;
    flat.lock.anchorTopologyId = 11; flat.lock.targetFrequencyHz = 1000000000;
    state->document.value = state->document.saved = flat;
    if (!tui_restore_captured_tail(*state, logLine, sizeof(logLine)) || state->document.value.lock.present ||
        !state->document.saved.lock.present || state->document.value.points[1].kind != PROFILE_POINT_CAPTURED_OFFSET ||
        state->document.value.points[1].capturedOffsetHz != -25000500) return 9680;

    // HELP-01..08 TUI actions: cycle mode, prepare two-step preview (draft untouched until ACCEPT ASSIST), reject stale revision.
    auto assistDoc = profile_document_make_empty();
    assistDoc.domains = PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_LOCK;
    assistDoc.topologyCount = 3; assistDoc.sourceTopologyComplete = true;
    for (int i = 0; i < 3; ++i) assistDoc.topology[i] = {10 + i, 22 + i, 662000 + (gc_i64)i * 6250, 1};
    assistDoc.points[0].kind = PROFILE_POINT_ABSOLUTE_FREQUENCY; assistDoc.points[0].absoluteFrequencyHz = 1200000000LL;
    assistDoc.points[1].kind = PROFILE_POINT_ABSOLUTE_FREQUENCY; assistDoc.points[1].absoluteFrequencyHz = 1000000000LL;
    assistDoc.lock.present = true; assistDoc.lock.mode = LOCK_MODE_FLATTEN;
    assistDoc.lock.anchorTopologyId = 11; assistDoc.lock.targetFrequencyHz = 1050000250LL;
    state->document.value = state->document.saved = assistDoc; state->document.revision = 80;
    profile_review_fixture::capture(*live, state->document.value);
    {
        auto& o = live->capture; o.pointCount = 3;
        for (gc_u32 i = 0; i < 3; ++i) {
            o.points[i].index = 22 + i; o.points[i].voltageUv = 662000 + (gc_i64)i * 6250;
            o.points[i].writableState = o.points[i].frequencyValid = o.points[i].offsetValid = 1;
            o.points[i].offsetHz = 0; o.points[i].frequencyHz = 1000000000LL + (gc_i64)i * 50000000LL;
        }
        o.vfLimits = {1, 1, 1, 1, VF_OFFSET_RANGE_SOURCE_NVML_GRAPHICS, -500000000, 500000000, 500};
        o.fingerprint = hardware_capture_fingerprint(o);
        o.revision = service_control_review_revision(o.fingerprint, nullptr);
    }
    state->service = *live;
    state->sourceReviewContext = linux_tui_review_context(*live);
    state->sourceReview = std::make_unique<ProfileDestinationReview>();
    if (profile_editor_check(state->document.value, 0, 0,
            [&](gc_u32 domains, auto& out, auto&) { if (domains != 511u) return false; out = live->capture; return true; },
            lines, state->sourceReview.get(), 80) != PROFILE_COMMAND_DECISION) return 9870;
    state->sourceCheckVisible = true;
    state->sourceAssistMode = PROFILE_ASSIST_PREFIX;
    tui_cycle_assist_mode(*state);
    tui_cycle_assist_mode(*state);
    if (state->sourceAssistMode != PROFILE_ASSIST_COMBINED) return 9871;
    if (!tui_prepare_curve_assist(*state, logLine, sizeof(logLine)) || !state->sourceAssistPending ||
        state->document.value.lock.targetFrequencyHz != 1050000250LL || !state->sourceCheckVisible) return 9872;
    ++state->document.revision;
    if (tui_accept_curve_assist(*state, logLine, sizeof(logLine)) ||
        state->document.value.lock.targetFrequencyHz != 1050000250LL) return 9873;
    --state->document.revision;
    if (!tui_accept_curve_assist(*state, logLine, sizeof(logLine)) || state->sourceAssistPending != nullptr ||
        state->document.value.lock.targetFrequencyHz != 1050000000LL ||
        state->document.value.points[0].absoluteFrequencyHz != 1050000000LL ||
        state->document.saved.lock.targetFrequencyHz != 1050000250LL ||
        state->document.revision != 81) return 9874;
    return 0;
}
