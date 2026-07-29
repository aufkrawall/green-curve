// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Pure client-side rule: a mutation names the published state it was computed
// against.
//
// Every APPLY and RESET carries the service instance id, GPU generation, and
// curve topology signature of the READY envelope the client last read.  The
// service refuses a mutation that carries none (see
// service_request_reject_reason) and, when it carries them, re-checks the
// values against what it publishes right now (service_mutation_preconditions_
// match), so a request built against a service that has since restarted,
// re-enumerated the GPU, or changed the curve topology fails instead of writing
// to hardware the client never actually looked at.
//
// The window path keeps this identity in GuiServiceModel.  The synchronous
// path -- every CLI verb, the installer's settings restore, the reset that
// --service-remove performs before deleting the service -- has no such model,
// and carried nothing at all: it sent three zeros, and the service rejected
// every one of those mutations as malformed.  Two live upgrades restored
// nothing because of it.  This header is the single rule both paths stamp
// through, kept pure and out of the Windows shards so the whole chain
// (envelope -> identity -> request -> validator) is asserted on either host.

#ifndef GREEN_CURVE_SERVICE_CLIENT_PRECONDITION_POLICY_H
#define GREEN_CURVE_SERVICE_CLIENT_PRECONDITION_POLICY_H

#include "gpu_core.h"

struct ServiceClientStateIdentity {
    gc_u64 serviceInstanceId;
    gc_u64 gpuGeneration;
    gc_u64 topologySignature;
};

static inline void service_client_identity_clear(
    ServiceClientStateIdentity* identity) {
    if (!identity) return;
    identity->serviceInstanceId = 0;
    identity->gpuGeneration = 0;
    identity->topologySignature = 0;
}

static inline bool service_client_identity_complete(
    const ServiceClientStateIdentity* identity) {
    return identity && identity->serviceInstanceId != 0 &&
        identity->gpuGeneration != 0 && identity->topologySignature != 0;
}

// Adopt an envelope the client has just read.
//
// Only a coherent READY envelope may be adopted.  A SYNCING, RECOVERING, or
// DEGRADED envelope describes a state the service does not consider current, so
// a mutation stamped from one would be rejected as stale -- and a READY
// envelope without a topology signature cannot authorize a VF write at all.
//
// A rejected envelope CLEARS the identity instead of leaving the previous one
// in place: a client that watched the service go away must not keep stamping
// the id of a service generation that no longer exists.
static inline bool service_client_identity_adopt(
    ServiceClientStateIdentity* identity, const ServiceStateEnvelope* state) {
    if (!identity) return false;
    service_client_identity_clear(identity);
    if (!state || state->gpuPhase != SERVICE_GPU_PHASE_READY ||
        (state->validSections & SERVICE_STATE_SECTION_READY_REQUIRED) !=
            SERVICE_STATE_SECTION_READY_REQUIRED ||
        !state->serviceInstanceId || !state->gpuGeneration ||
        !state->topologySignature) return false;
    identity->serviceInstanceId = state->serviceInstanceId;
    identity->gpuGeneration = state->gpuGeneration;
    identity->topologySignature = state->topologySignature;
    return true;
}

// True once a built mutation request may go on the wire.
static inline bool service_client_mutation_is_stamped(
    const ServiceRequest* request) {
    return request && request->expectedServiceInstanceId != 0 &&
        request->expectedGpuGeneration != 0 &&
        request->expectedTopologySignature != 0;
}

// Stamp a built APPLY/RESET.  Fails closed rather than sending an unstamped
// request: the service reads zeroed preconditions as "this client is not
// participating in reconnect safety" and refuses the whole request, which the
// caller then sees as a transport-shaped failure with no usable reason.
static inline bool service_client_stamp_mutation_preconditions(
    ServiceRequest* request, const ServiceClientStateIdentity* identity) {
    if (!request || !service_client_identity_complete(identity)) return false;
    request->expectedServiceInstanceId = identity->serviceInstanceId;
    request->expectedGpuGeneration = identity->gpuGeneration;
    request->expectedTopologySignature = identity->topologySignature;
    return service_client_mutation_is_stamped(request);
}

#endif // GREEN_CURVE_SERVICE_CLIENT_PRECONDITION_POLICY_H
