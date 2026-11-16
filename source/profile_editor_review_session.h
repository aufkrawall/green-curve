// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_editor_check.h"
#include "hardware_capture_runtime.h"
#include "service_control_review_revision.h"

struct ProfileEditorReviewContext {
    gc_u64 instance = 0, generation = 0, topology = 0, ownership = 0;
    gc_u32 sections = 0, domains = 0, phase = 0;
    GpuAdapterInfo target{};
};
static inline ProfileEditorReviewContext profile_editor_review_context(const ServiceResponse& response) {
    const auto& s = response.state; const auto& snapshot = response.snapshot;
    if (!s.serviceInstanceId || !s.gpuGeneration || !s.topologySignature ||
        s.gpuPhase != SERVICE_GPU_PHASE_READY ||
        (s.validSections & SERVICE_STATE_SECTION_READY_REQUIRED) != SERVICE_STATE_SECTION_READY_REQUIRED ||
        snapshot.selectedAdapterIndex >= snapshot.adapterCount || snapshot.selectedAdapterIndex >= MAX_GPU_ADAPTERS) return {};
    const auto& target = snapshot.adapters[snapshot.selectedAdapterIndex];
    if (!target.valid) return {};
    return {s.serviceInstanceId, s.gpuGeneration, s.topologySignature,
        service_control_review_revision(0, s.activeDesiredValid ? &response.desired : nullptr),
        s.validSections & SERVICE_STATE_SECTION_CURVE_TOPOLOGY, snapshot.health.availableMutationDomains, s.gpuPhase, target};
}
static inline bool profile_editor_review_context_equal(const ProfileEditorReviewContext& a, const ProfileEditorReviewContext& b) {
    const auto left = hardware_capture_binding(a.target), right = hardware_capture_binding(b.target);
    return a.instance && a.instance == b.instance && a.generation == b.generation && a.topology == b.topology &&
        a.ownership == b.ownership && a.sections == b.sections && a.domains == b.domains && a.phase == b.phase &&
        !memcmp(&left, &right, sizeof(left));
}
static inline bool profile_editor_review_capture_matches(const ProfileEditorReviewContext& context, const ServiceResponse& response) {
    const auto binding = hardware_capture_binding(context.target);
    return context.instance && response.status == SERVICE_STATUS_OK && response.capture.valid &&
        hardware_capture_observation_valid(response.capture) && response.capture.fingerprint &&
        response.capture.fingerprint == hardware_capture_fingerprint(response.capture) &&
        response.capture.revision == service_control_review_revision(response.capture.fingerprint,
            response.state.activeDesiredValid ? &response.desired : nullptr) &&
        response.state.serviceInstanceId == context.instance && response.state.gpuGeneration == context.generation &&
        response.capture.gpuGeneration == context.generation &&
        context.ownership == service_control_review_revision(0, response.state.activeDesiredValid ? &response.desired : nullptr) &&
        !memcmp(&binding, &response.capture.binding, sizeof(binding));
}
struct ProfileEditorReviewJob {
    gc_u64 token = 0, sourceRevision = 0;
    gc_u32 graphics = 0, memory = 0;
    ProfileEditorReviewContext context{};
    ProfileDocument source{};
};
static inline gc_u32 profile_editor_review_domains(const ProfileDocument&) {
    return 511u; // Whole-operation preconditions include preserved/cleanup controls.
}
static inline bool profile_editor_review_complete(const ProfileEditorReviewJob& job, gc_u64 token, gc_u64 revision,
    const ProfileEditorReviewContext& current, const ServiceResponse& response,
    std::vector<std::string>& lines, ProfileDestinationReview& review, int& result) {
    profile_destination_review_reset(review); lines.clear(); result = PROFILE_COMMAND_IO;
    if (!token || token != job.token || revision != job.sourceRevision ||
        !profile_editor_review_context_equal(job.context, current) || !profile_editor_review_capture_matches(job.context, response)) return false;
    result = profile_editor_check(job.source, job.graphics, job.memory,
        [&](gc_u32 domains, HardwareCaptureObservation& out, ProfileCodecError& error) {
            if (response.capture.requestedDomains != domains)
                return profile_codec::fail(&error, 0, "destination read differs from the reviewed request");
            out = response.capture; return true;
        }, lines, &review, job.sourceRevision);
    return true;
}
