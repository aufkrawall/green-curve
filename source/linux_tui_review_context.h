// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "service_protocol.h"
#include "linux_tui_authority.h"
#include "service_control_review_revision.h"

// A cached preview's lifetime, independent of mutation/draft authority.
struct LinuxTuiReviewContext {
    gc_u64 serviceInstanceId = 0, gpuGeneration = 0, topologySignature = 0, ownedRevision = 0;
    gc_u32 topologySection = 0, mutationDomains = 0, gpuPhase = 0;
    GpuAdapterInfo gpu{};
};
static inline gc_u64 linux_tui_review_owned_revision(const ServiceResponse& response) {
    return service_control_review_revision(0, response.state.activeDesiredValid ? &response.desired : nullptr);
}
static inline LinuxTuiReviewContext linux_tui_review_context(const ServiceResponse& response) {
    const auto* gpu = linux_tui_authoritative_gpu(&response);
    if (!gpu || !response.state.serviceInstanceId || !response.state.gpuGeneration) return {};
    return {response.state.serviceInstanceId, response.state.gpuGeneration, response.state.topologySignature,
        linux_tui_review_owned_revision(response), response.state.validSections & SERVICE_STATE_SECTION_CURVE_TOPOLOGY,
        response.snapshot.health.availableMutationDomains, response.state.gpuPhase, *gpu};
}
static inline bool linux_tui_review_context_matches(const LinuxTuiReviewContext& context, const ServiceResponse& response) {
    const auto current = linux_tui_review_context(response);
    return context.serviceInstanceId && current.serviceInstanceId == context.serviceInstanceId &&
        current.gpuGeneration == context.gpuGeneration && current.topologySignature == context.topologySignature &&
        current.topologySection == context.topologySection && current.mutationDomains == context.mutationDomains &&
        current.gpuPhase == context.gpuPhase && current.ownedRevision == context.ownedRevision &&
        linux_gpu_identity_matches(&current.gpu, &context.gpu);
}
// Passive Capture has its own observation, not an ordinary refreshed snapshot.
// Compare the envelope/ownership fields it actually publishes, never adopt its
// empty telemetry as live state. The named target is checked by the service.
static inline bool linux_tui_review_capture_matches(const LinuxTuiReviewContext& context, const ServiceResponse& response) {
    return context.serviceInstanceId && response.state.serviceInstanceId == context.serviceInstanceId &&
        response.state.gpuGeneration == context.gpuGeneration && response.capture.gpuGeneration == context.gpuGeneration &&
        linux_tui_review_owned_revision(response) == context.ownedRevision;
}
