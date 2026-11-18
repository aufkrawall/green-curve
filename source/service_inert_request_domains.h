// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Mutation-domain availability for a request judged against a published
// ServiceSnapshot, with the inert-request rules applied.  One implementation
// for every gate that compares "requested" against "available": the Linux
// daemon's precondition gate and the TUI's draft binding, edit staging, Apply
// and profile-load checks.
//
// A saved profile always carries the mandatory power_limit_pct and fan_mode
// keys.  On a notebook whose driver exposes no power target, or no fan, those
// keys at their default (board-default power, Auto fan) ask for nothing the
// board could do anyway.  The backend already treated them as inert
// (linux_power_request_is_inert(), fan_request_policy.h); these gates did not,
// so such a profile stayed detached and unappliable from the TUI.  Any other
// value in those domains is a real request and stays missing.
#ifndef GREEN_CURVE_SERVICE_INERT_REQUEST_DOMAINS_H
#define GREEN_CURVE_SERVICE_INERT_REQUEST_DOMAINS_H

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

// Power is missing from the published domains only when the board exposes no
// power control surface (unreadable target or no setter), which is exactly
// when the backend treats a board-default request as inert.
static inline bool service_snapshot_power_request_is_inert(
    const DesiredSettings* desired, const ServiceSnapshot* snapshot) {
    return desired && snapshot &&
        (snapshot->health.availableMutationDomains &
            SERVICE_MUTATION_DOMAIN_POWER) == 0 &&
        desired_power_request_is_board_default(*desired);
}

// Domains `desired` asks for that the GPU behind `snapshot` does not offer.
// An inert request owns its domain without needing the surface.
static inline gc_u32 service_snapshot_missing_mutation_domains(
    const DesiredSettings* desired, const ServiceSnapshot* snapshot) {
    if (!desired || !snapshot) return 0;
    gc_u32 missing = service_desired_mutation_domains(desired) &
        ~snapshot->health.availableMutationDomains;
    if ((missing & SERVICE_MUTATION_DOMAIN_POWER) &&
        service_snapshot_power_request_is_inert(desired, snapshot))
        missing &= ~(gc_u32)SERVICE_MUTATION_DOMAIN_POWER;
    // Only the inert shape is exempt: Fixed/Curve on a fanless GPU stays
    // missing, and its callers name the refusal.
    if ((missing & SERVICE_MUTATION_DOMAIN_FAN) &&
        service_snapshot_fan_request_disposition(desired, snapshot) ==
            FAN_REQUEST_INERT_NO_FAN)
        missing &= ~(gc_u32)SERVICE_MUTATION_DOMAIN_FAN;
    return missing;
}

#endif  // GREEN_CURVE_SERVICE_INERT_REQUEST_DOMAINS_H
