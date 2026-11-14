// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "profile_review_fixture.h"
#include "gui_passive_capture.h"
#include <memory>

int run_profile_review_session_tests() {
    auto live = std::make_unique<ServiceResponse>();
    profile_review_fixture::live(*live);
    auto& state = live->state;
    auto& target = live->snapshot.adapters[0];
    const auto context = profile_editor_review_context(*live);
    if (context.instance != 4 || context.generation != 7 || context.target.pciBus != 6 ||
        !profile_editor_review_context_equal(context, context)) return 9350;
    GuiPassiveCaptureWork work{};
    if (!gui_passive_capture_prepare(target, PROFILE_DOMAIN_MEMORY, 91, work) || work.token != 91 ||
        work.request.command != SERVICE_CMD_CAPTURE_CURRENT || work.request.targetGpu.pciBus != 6 ||
        work.request.captureDomains != PROFILE_DOMAIN_MEMORY || !service_desired_settings_is_zero(work.request.desired) ||
        work.request.flags || work.request.operationId || work.request.path[0] || work.request.expectedGpuGeneration) return 9351;
    work.request.callerPid = 1;
    if (service_request_reject_reason(&work.request)) return 9352;
    if (gui_passive_capture_prepare(target, 512, 1, work) || gui_passive_capture_prepare(target, 0, 1, work) ||
        gui_passive_capture_prepare(target, 1, 0, work) || gui_passive_capture_prepare(GpuAdapterInfo{}, 1, 1, work)) return 9353;
    auto job = std::make_unique<ProfileEditorReviewJob>(); job->token = 91; job->sourceRevision = 17;
    job->context = context; job->source = profile_review_fixture::source();
    auto& source = job->source;
    auto response = std::make_unique<ServiceResponse>(*live);
    profile_review_fixture::capture(*response, source); auto& o = response->capture;
    auto review = std::make_unique<ProfileDestinationReview>(); std::vector<std::string> lines; int result = 99;
    if (!profile_editor_review_complete(*job, 91, 17, context, *response, lines, *review, result) ||
        result != PROFILE_COMMAND_DECISION || !review->planned() || review->sourceRevision != 17 ||
        review->runtime.memOffsetHz != -123500000 || lines.empty() || source.memoryOffsetHz != -123500000) return 9354;
    const auto refused = [&](gc_u64 token, gc_u64 revision, const ProfileEditorReviewContext& current) {
        review->stage = PROFILE_REVIEW_PLANNED; lines = {"obsolete"}; result = 99;
        return !profile_editor_review_complete(*job, token, revision, current, *response, lines, *review, result) &&
            review->stage == PROFILE_REVIEW_EMPTY && lines.empty() && result == PROFILE_COMMAND_IO;
    };
    if (!refused(90, 17, context) || !refused(91, 18, context) || !refused(0, 17, context)) return 9355;
    for (int field = 0; field < 9; ++field) {
        auto changed = context;
        switch (field) {
            case 0: ++changed.instance; break; case 1: ++changed.generation; break;
            case 2: ++changed.topology; break; case 3: ++changed.ownership; break;
            case 4: changed.sections = 0; break; case 5: changed.domains = 0; break;
            case 6: changed.phase = SERVICE_GPU_PHASE_RECOVERING; break;
            case 7: ++changed.target.pciBus; break; case 8: ++changed.target.nvmlIndex; break;
        }
        if (!refused(91, 17, changed)) return 9356;
    }
    response->state.serviceInstanceId = 5; if (!refused(91, 17, context)) return 9357;
    response->state = state; ++o.binding.pciBus; if (!refused(91, 17, context)) return 9358;
    o.binding = hardware_capture_binding(target); ++o.memory.value; if (!refused(91, 17, context)) return 9359;
    --o.memory.value; ++o.revision; if (!refused(91, 17, context)) return 9360;
    --o.revision; response->state.activeDesiredValid = 1; response->desired.hasMemOffset = 1;
    response->desired.memOffsetMHz = 0; if (!refused(91, 17, context)) return 9361;
    response->state = state;
    // Ordinary telemetry revisions and measurements do not change review context.
    ++live->state.stateRevision; live->snapshot.gpuTemperatureC = 70;
    if (!profile_editor_review_context_equal(context, profile_editor_review_context(*live))) return 9362;
    live->state.gpuPhase = SERVICE_GPU_PHASE_DEGRADED;
    if (profile_editor_review_context(*live).instance) return 9363;
    return 0;
}
