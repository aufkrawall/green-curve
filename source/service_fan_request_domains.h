// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Mutation-domain availability for a request judged against a published
// ServiceSnapshot, with the fanless-GPU rule of fan_request_policy.h applied.
// One implementation for every gate that compares "requested" against
// "available": the Linux daemon's precondition gate and the TUI's draft
// binding, edit staging, Apply and profile-load checks.  Without it a saved
// profile (which always carries fan_mode) stayed detached and unappliable on a
// notebook, because its inert Auto fan request counted as a missing domain.
#ifndef GREEN_CURVE_SERVICE_FAN_REQUEST_DOMAINS_H
#define GREEN_CURVE_SERVICE_FAN_REQUEST_DOMAINS_H

#include "gpu_core.h"
#include "gpu_capability_policy.h"
#include "fan_request_policy.h"

// The snapshot's own evidence: the fan count the driver reported and the fan
// capability carried in the packed probe.
static inline bool service_snapshot_fan_domain_missing(
    const ServiceSnapshot* snapshot) {
    if (!snapshot) return false;
    GpuCapabilityProbe capability = {};
    gpu_capability_unpack_domains(&capability,
        snapshot->health.capabilityDomainsPacked,
        snapshot->health.capabilityMemoryTopology);
    return gpu_capability_domain_is_missing(gpu_capability_get(
        &capability, SERVICE_MUTATION_DOMAIN_FAN));
}

static inline bool service_snapshot_fan_control_known_absent(
    const ServiceSnapshot* snapshot) {
    return snapshot && fan_control_known_absent(snapshot->fanCount > 0,
        service_snapshot_fan_domain_missing(snapshot));
}

static inline FanRequestDisposition service_snapshot_fan_request_disposition(
    const DesiredSettings* desired, const ServiceSnapshot* snapshot) {
    if (!desired) return FAN_REQUEST_NOT_OWNED;
    if (!snapshot) return desired->hasFan ? FAN_REQUEST_HARDWARE : FAN_REQUEST_NOT_OWNED;
    return fan_request_disposition(desired->hasFan != 0, desired->fanMode,
        desired->fanAuto != 0, snapshot->fanCount > 0,
        service_snapshot_fan_domain_missing(snapshot));
}

// Domains `desired` asks for that the GPU behind `snapshot` does not offer.
// An inert fan request owns the fan domain without needing its surface.
static inline gc_u32 service_snapshot_missing_mutation_domains(
    const DesiredSettings* desired, const ServiceSnapshot* snapshot) {
    if (!desired || !snapshot) return 0;
    gc_u32 missing = service_desired_mutation_domains(desired) &
        ~snapshot->health.availableMutationDomains;
    // Only the inert shape is exempt: Fixed/Curve on a fanless GPU stays
    // missing, and its callers name the refusal.
    if ((missing & SERVICE_MUTATION_DOMAIN_FAN) &&
        service_snapshot_fan_request_disposition(desired, snapshot) ==
            FAN_REQUEST_INERT_NO_FAN)
        missing &= ~(gc_u32)SERVICE_MUTATION_DOMAIN_FAN;
    return missing;
}

#endif  // GREEN_CURVE_SERVICE_FAN_REQUEST_DOMAINS_H
