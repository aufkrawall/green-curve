// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "linux_tui_internal.h"
#include "profile_review_fixture.h"
#include "profile_capture_proposal_fixture.h"
#include <unistd.h>

// Passive Capture transport and review-context lifetime. Cases 9757-9760.
namespace tui_capture_context_fixture {
static ServiceRequest sent{};
static ServiceResponse reply{};
static int calls = 0;
static void linux_debug_logf(const char*, ...) {}
static bool scripted_capture_send(const ServiceRequest* request, ServiceResponse* response, char*, size_t) {
    ++calls; sent = *request; *response = reply; return true;
}
#define linux_daemon_send scripted_capture_send
#include "linux_tui_capture_transport.h"
#undef linux_daemon_send

static int capture_context_tests() {
    auto state = std::make_unique<TuiState>(); auto live = std::make_unique<ServiceResponse>();
    profile_review_fixture::live(*live); state->serviceOnline = true;
    live->state.activeDesiredValid = 1; live->desired.hasLock = 1; live->desired.lockMode = LOCK_MODE_HARD;
    live->desired.lockCi = 22; live->desired.lockMHz = 1000; state->service = *live;
    reply = *live; reply.capture = profile_capture_proposal_fixture::curve(); reply.capture.osFamily = PROFILE_SOURCE_OS_LINUX;
    reply.capture.binding = hardware_capture_binding(live->snapshot.adapters[0]);
    reply.capture.fingerprint = hardware_capture_fingerprint(reply.capture);
    reply.capture.revision = service_control_review_revision(reply.capture.fingerprint, &reply.desired);
    reply.snapshot = {}; reply.state.validSections = SERVICE_STATE_SECTION_ACTIVE_INTENT; reply.state.gpuPhase = SERVICE_GPU_PHASE_DEGRADED;
    ProfileCaptureProposalContext context; HardwareCaptureObservation observation{}; ProfileCodecError error;
    const auto shown = linux_tui_review_context(state->service);
    if (!tui_read_capture(*state, reply.capture.requestedDomains, observation, error, shown, &context) ||
        context.ownedPinHz != 1000000000 || !context.pinAvailabilityKnown || !context.pinAvailable ||
        sent.command != SERVICE_CMD_CAPTURE_CURRENT || sent.operationId || state->service.snapshot.adapterCount != 1) return 9757;
    const auto before = observation;
    ++reply.desired.lockMHz;
    if (tui_read_capture(*state, reply.capture.requestedDomains, observation, error, shown, &context) ||
        context.ownedPinHz || memcmp(&observation, &before, sizeof(before))) return 9758;
    --reply.desired.lockMHz;
    auto degraded = shown; degraded.gpuPhase = SERVICE_GPU_PHASE_DEGRADED; degraded.mutationDomains = 0;
    if (!tui_read_capture(*state, reply.capture.requestedDomains, observation, error, degraded, &context) ||
        context.ownedPinHz != 1000000000 || context.pinAvailabilityKnown) return 9759;
    state->service.snapshot.health.availableMutationDomains &= ~SERVICE_MUTATION_DOMAIN_LOCK;
    const auto absent = linux_tui_review_context(state->service);
    if (!tui_read_capture(*state, reply.capture.requestedDomains, observation, error, absent, &context) ||
        context.ownedPinHz != 1000000000 || !context.pinAvailabilityKnown || context.pinAvailable) return 9760;
    return 0;
}

static int run() { return capture_context_tests(); }
}
int run_tui_capture_context_tests() { return tui_capture_context_fixture::run(); }
