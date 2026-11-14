// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "linux_tui_internal.h"
#include "profile_editor_check.h"
#include <unistd.h>

namespace tui_review_session_fixture {
static ServiceRequest sent{};
static ServiceResponse reply{};
static int calls = 0;
static void linux_debug_logf(const char*, ...) {}
static bool scripted_capture_send(const ServiceRequest* request, ServiceResponse* response, char*, size_t) {
    ++calls; sent = *request; *response = reply; return true;
}
#define linux_daemon_send scripted_capture_send
#include "linux_tui_source_check.h"
#undef linux_daemon_send

static int run() {
    auto state = std::make_unique<TuiState>();
    state->serviceOnline = true; state->document.valid = true;
    state->document.value = profile_document_make_empty();
    auto& source = state->document.value;
    source.domains = PROFILE_DOMAIN_MEMORY; source.hasMemoryOffset = true; source.memoryOffsetHz = -123500000;
    state->service.state.serviceInstanceId = 4; state->service.state.gpuGeneration = 7;
    state->service.state.validSections = SERVICE_STATE_SECTION_ADAPTER_IDENTITY;
    state->service.snapshot.adapterCount = 1;
    auto& gpu = state->service.snapshot.adapters[0]; gpu.valid = gpu.pciInfoValid = 1;
    gpu.pciBus = 6; gpu.deviceId = 42;
    reply = {}; reply.status = SERVICE_STATUS_OK; reply.state = state->service.state;
    auto& capture = reply.capture; capture.valid = capture.binding.valid = 1;
    capture.schemaVersion = HARDWARE_CAPTURE_SCHEMA_CURRENT; capture.gpuGeneration = 7;
    capture.revision = capture.fingerprint = 11;
    capture.requestedDomains = PROFILE_DOMAIN_MEMORY | PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_GRAPHICS_OFFSET | PROFILE_DOMAIN_LOCK;
    for (int i = 0; i < 9; ++i) if (capture.requestedDomains & (1u << i))
        hardware_capture_domain(capture, 1 << i, CAPTURE_UNSUPPORTED, -100001);
    capture.memory.valid = capture.memory.writable = 1; capture.memory.grid = 500000; capture.memory.surface = CLOCK_OFFSET_SURFACE_P0;
    hardware_capture_domain(capture, PROFILE_DOMAIN_MEMORY, CAPTURE_COMPLETE);
    calls = 0; tui_check_source(state.get());
    if (calls != 1 || sent.command != SERVICE_CMD_CAPTURE_CURRENT || sent.captureDomains != capture.requestedDomains ||
        !linux_gpu_identity_matches(&sent.targetGpu, &gpu) || sent.expectedServiceInstanceId ||
        sent.expectedGpuGeneration || sent.expectedTopologySignature || sent.operationId ||
        service_request_reject_reason(&sent) || !state->sourceReview || !state->sourceReview->planned()) return 9275;
    // The passive reply intentionally contains no ordinary adapter snapshot.
    if (reply.snapshot.adapterCount || state->service.snapshot.adapterCount != 1 ||
        state->sourceReviewContext.serviceInstanceId != 4) return 9276;
    for (int changed = 0; changed < 3; ++changed) {
        state->sourceCheckVisible = false;
        reply.state = state->service.state; reply.capture.gpuGeneration = 7;
        if (changed == 0) ++reply.state.serviceInstanceId;
        else if (changed == 1) ++reply.capture.gpuGeneration;
        else { reply.state.activeDesiredValid = 1; reply.desired.hasGpuOffset = 1; }
        tui_check_source(state.get());
        if (!state->sourceReview || state->sourceReview->stage != PROFILE_REVIEW_SOURCE ||
            state->sourceReview->capture.valid || !state->sourceCheckVisible ||
            std::string(state->sourceReview->error.message).find("changed during the read") == std::string::npos) return 9277;
    }
    state->sourceCheckVisible = false; state->service.state.serviceInstanceId = 0;
    const int beforeUnavailable = calls;
    tui_check_source(state.get());
    if (calls != beforeUnavailable || state->sourceReview || state->document.value.memoryOffsetHz != -123500000) return 9278;
    return 0;
}
}
int run_tui_review_session_tests() { return tui_review_session_fixture::run(); }
