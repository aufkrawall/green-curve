// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_runtime_resolution.h"
#include "hardware_capture_runtime.h"
#include "profile_capture_representation.h"
#include "profile_control_feasibility.h"
#include "vf_flat_tail_recognition.h"
#include "service_protocol.h"
#include "service_control_review_revision.h"
#include <algorithm>
#include <limits>

struct ProfileCaptureProposalContext {
    gc_i64 ownedPinHz = 0; // Service-owned intent, never a hardware getter.
    bool pinAvailabilityKnown = false, pinAvailable = false;
};
static inline ProfileCaptureProposalContext profile_capture_proposal_context(const ServiceResponse &response) {
    ProfileCaptureProposalContext context;
    const auto &o = response.capture;
    const auto &d = response.desired;
    if (response.status != SERVICE_STATUS_OK || !o.valid || !hardware_capture_observation_valid(o) || !o.fingerprint ||
        o.fingerprint != hardware_capture_fingerprint(o) || !response.state.serviceInstanceId ||
        response.state.gpuGeneration != o.gpuGeneration ||
        (response.state.gpuPhase != SERVICE_GPU_PHASE_READY && response.state.gpuPhase != SERVICE_GPU_PHASE_DEGRADED) ||
        o.revision != service_control_review_revision(o.fingerprint, response.state.activeDesiredValid ? &d : nullptr))
        return context;
    if (response.state.activeDesiredValid && desired_authoring_valid(&d) && desired_curve_intent_valid(&d) &&
        d.hasLock && d.lockMode == LOCK_MODE_HARD && desired_lock_hz(d) > 0)
        context.ownedPinHz = desired_lock_hz(d);
    // A successful passive collector can publish DEGRADED when it deliberately
    // omits the ordinary snapshot. Applied intent still is not driver readback.
    // Passive replies need not contain an ordinary snapshot. Its absence is
    // unknown write capability, not proof that a clock control is absent.
    if ((response.state.validSections & SERVICE_STATE_SECTION_READY_REQUIRED) == SERVICE_STATE_SECTION_READY_REQUIRED &&
        response.snapshot.adapterCount && response.snapshot.selectedAdapterIndex < response.snapshot.adapterCount &&
        response.snapshot.selectedAdapterIndex < MAX_GPU_ADAPTERS) {
        const auto binding =
            hardware_capture_binding(response.snapshot.adapters[response.snapshot.selectedAdapterIndex]);
        if (!memcmp(&binding, &o.binding, sizeof(binding))) {
            context.pinAvailabilityKnown = true;
            context.pinAvailable =
                (response.snapshot.health.availableMutationDomains & SERVICE_MUTATION_DOMAIN_LOCK) != 0;
        }
    }
    return context;
}
// Existing capture adapters without retained service intent supply no pin
// evidence. In particular, imported document provenance cannot supply it.
template <class IO>
static inline auto profile_capture_context_from_io(IO &io, int) -> decltype(io.capture_proposal_context()) {
    return io.capture_proposal_context();
}
template <class IO> static inline ProfileCaptureProposalContext profile_capture_context_from_io(IO &, long) {
    return {};
}

struct ProfileCaptureLockProposal {
    bool recognized = false;
    ProfileDocument document{};
    ProfileControlPlan controls{};
    gc_i64 anchorVoltageUv = 0, frequencyHz = 0, maximumDeviationHz = 0;
    int tailCount = 0;
    ProfileLockProvenance provenance = PROFILE_LOCK_PROVENANCE_USER;
    char reason[192]{};
    bool selectable() const { return recognized && controls.disposition <= PROFILE_CONTROL_CONDITIONAL; }
};
struct ProfileCaptureProposals {
    ProfileDocument exact{};
    ProfileCaptureLockProposal flatten{}, pin{};
    bool sameTarget = false, conflictingTargets = false;
    ProfileCaptureRepresentation suggested = PROFILE_CAPTURE_EXACT;
};
static inline void profile_capture_proposal_reason(ProfileCaptureLockProposal &proposal, const char *reason,
                                                   ProfileControlFeasibility status = PROFILE_CONTROL_UNAVAILABLE) {
    proposal.controls.disposition = status;
    snprintf(proposal.reason, sizeof(proposal.reason), "%s", reason);
}
static inline void profile_capture_check_proposal(const HardwareCaptureObservation &observation,
                                                  ProfileCaptureLockProposal &proposal) {
    auto runtime = std::make_unique<DesiredSettings>();
    ProfileMappingReport mapping;
    ProfileDestinationTopology destination;
    ProfileRuntimeRoutes routes;
    routes.graphics = proposal.document.graphicsOffset.present ? observation.graphics.surface : 0;
    routes.memory = observation.memory.surface;
    if (!profile_destination_from_capture(observation, destination, proposal.reason, sizeof(proposal.reason)) ||
        !profile_resolve_runtime(proposal.document, destination, 1, routes, *runtime, mapping, proposal.reason,
                                 sizeof(proposal.reason))) {
        proposal.controls.disposition = PROFILE_CONTROL_UNAVAILABLE;
        return;
    }
    proposal.controls = profile_control_plan(*runtime, observation);
    snprintf(proposal.reason, sizeof(proposal.reason), "%s; fresh complete operation checks remain required",
             profile_control_disposition_name(proposal.controls.disposition));
    if (const auto *issue = curve_plan_lead_issue(proposal.controls.curve))
        snprintf(proposal.reason, sizeof(proposal.reason), "%s: %s ci=%d worstCi=%d boundHz=%lld affected=%d",
                 profile_control_disposition_name(proposal.controls.disposition), curve_plan_reason_name(issue->reason),
                 issue->ci, issue->worstCi, issue->limitHz, issue->affectedCount);
}
static inline void profile_capture_make_lock(const ProfileDocument &exact, ProfileCaptureLockProposal &proposal,
                                             int anchor, LockMode mode, gc_i64 frequency,
                                             ProfileLockProvenance provenance) {
    proposal.recognized = true;
    proposal.document = exact;
    proposal.anchorVoltageUv = exact.topology[anchor].voltageUv;
    proposal.frequencyHz = frequency;
    proposal.provenance = provenance;
    auto &d = proposal.document;
    d.domains |= PROFILE_DOMAIN_LOCK;
    d.source.captureComplete = false;
    d.lock.present = true;
    d.lock.mode = mode;
    d.lock.anchorTopologyId = d.topology[anchor].topologyId;
    d.lock.targetFrequencyHz = frequency;
    d.lock.trackAnchor = false;
    d.lock.provenance = provenance;
    for (int i = 0; i < d.topologyCount; ++i)
        if (d.topology[i].voltageUv >= proposal.anchorVoltageUv) {
            // Keep exact source controls as observations while the lock owns its
            // tail. Switching back to exact replay never needs a new driver read.
            d.points[i].kind = PROFILE_POINT_ABSENT;
            d.points[i].absoluteFrequencyHz = d.points[i].capturedOffsetHz = 0;
        }
}

// Pure recognition and draft proposals over one coherent observation. There
// are no setters, implicit decisions, baseline arithmetic or ownership changes.
static inline bool profile_capture_prepare_proposals(const HardwareCaptureObservation &observation,
                                                     ProfileCaptureProposalContext context,
                                                     ProfileCaptureProposals &output, char *why, size_t whySize) {
    auto proposals = std::make_unique<ProfileCaptureProposals>();
    if (!hardware_capture_to_document(observation, proposals->exact, why, whySize))
        return false;
    profile_capture_proposal_reason(proposals->flatten, "no complete writable flattened tail was recognized");
    profile_capture_proposal_reason(proposals->pin, "pin unknown: no validated getter or retained applied intent");
    int order[VF_NUM_POINTS]{};
    VFCurvePoint normalized[VF_NUM_POINTS]{};
    const int count = proposals->exact.topologyCount;
    for (int i = 0; i < count; ++i)
        order[i] = i;
    std::sort(order, order + count,
              [&](int a, int b) { return observation.points[a].voltageUv < observation.points[b].voltageUv; });
    VfFloorTopology floor{};
    floor.valid = count > 0 && observation.topologyComplete;
    floor.graphicsEnd = count;
    floor.firstPopulated = 0;
    bool writable = false;
    gc_i64 previousVoltage = 0;
    for (int i = 0; i < count; ++i) {
        const auto &p = observation.points[order[i]];
        if (!p.frequencyValid || !p.offsetValid || !p.writableState || p.voltageUv <= previousVoltage ||
            p.voltageUv > UINT_MAX || p.frequencyHz / 1000 <= 0 || p.frequencyHz / 1000 > INT_MAX)
            floor.valid = false;
        previousVoltage = p.voltageUv;
        normalized[i] = {(unsigned int)(p.frequencyHz / 1000), (unsigned int)p.voltageUv};
        if (p.writableState == 1)
            writable = true;
        else if (writable)
            floor.valid = false;
        else {
            ++floor.floorEnd;
            ++floor.floorRun;
        }
    }
    if (!writable)
        floor.valid = false;
    if (floor.valid && (observation.capturedDomains & PROFILE_DOMAIN_VF_CURVE)) {
        const auto recognized = vf_recognize_flat_tail(normalized, floor, 1, 3);
        if (recognized.found) {
            auto &flatten = proposals->flatten;
            const int anchor = order[recognized.anchorCi];
            profile_capture_make_lock(proposals->exact, flatten, anchor, LOCK_MODE_FLATTEN,
                                      (gc_i64)recognized.plateauMHz * 1000000,
                                      PROFILE_LOCK_PROVENANCE_RECOGNIZED_FLATTEN);
            flatten.tailCount = recognized.tailCount;
            for (int i = recognized.anchorCi; i < count; ++i) {
                const gc_i64 delta = observation.points[order[i]].frequencyHz - observation.points[anchor].frequencyHz;
                flatten.maximumDeviationHz = std::max(flatten.maximumDeviationHz, delta < 0 ? -delta : delta);
            }
            auto &lock = flatten.document.lock;
            lock.hasRecognitionEvidence = true;
            lock.recognizedAnchorVoltageUv = flatten.anchorVoltageUv;
            lock.recognizedTailCount = flatten.tailCount;
            lock.recognitionToleranceHz = 1000000;
            profile_capture_check_proposal(observation, flatten);
        }
    }
    gc_i64 pinHz = 0;
    ProfileLockProvenance pinProvenance = PROFILE_LOCK_PROVENANCE_USER;
    if (observation.requestedDomains & PROFILE_DOMAIN_LOCK) {
        if (observation.pinGetterValid) {
            if (!observation.pinPresent)
                profile_capture_proposal_reason(proposals->pin, "validated getter reports no pin");
            else if (observation.pinMinimumHz != observation.pinMaximumHz)
                profile_capture_proposal_reason(proposals->pin,
                                                "the getter reports a range; a fixed pin needs a separate decision");
            else {
                pinHz = observation.pinMaximumHz;
                pinProvenance = PROFILE_LOCK_PROVENANCE_DRIVER_GETTER;
            }
        } else if (context.ownedPinHz > 0) {
            pinHz = context.ownedPinHz;
            pinProvenance = PROFILE_LOCK_PROVENANCE_APPLIED_INTENT;
        }
    }
    if (pinHz > 0) {
        auto &pin = proposals->pin;
        int anchor = -1;
        if (floor.valid)
            for (int i = 0; i < count; ++i) {
                const int at = order[i];
                if (observation.points[at].writableState == 1 && observation.points[at].frequencyHz >= pinHz) {
                    anchor = at;
                    break;
                }
            }
        if (anchor < 0)
            profile_capture_proposal_reason(pin, "pin evidence has no complete writable curve anchor");
        else {
            profile_capture_make_lock(proposals->exact, pin, anchor, LOCK_MODE_HARD, pinHz, pinProvenance);
            if (context.pinAvailabilityKnown && !context.pinAvailable)
                profile_capture_proposal_reason(
                    pin, "hard-pin control is unavailable; exact replay or an explicit flatten remains separate");
            else {
                profile_capture_check_proposal(observation, pin);
                // A readable value is not proof of the destination's complete
                // write/transition/recovery behavior.
                if (pin.controls.disposition == PROFILE_CONTROL_READY) {
                    pin.controls.disposition = PROFILE_CONTROL_CONDITIONAL;
                    snprintf(pin.reason, sizeof(pin.reason), "%s",
                             "hard-pin write and transition behavior require fresh validation");
                }
            }
        }
    }
    proposals->sameTarget = proposals->flatten.recognized && proposals->pin.recognized &&
                            proposals->flatten.frequencyHz == proposals->pin.frequencyHz;
    proposals->conflictingTargets =
        proposals->flatten.recognized && proposals->pin.recognized && !proposals->sameTarget;
    if (proposals->flatten.selectable() && proposals->flatten.controls.disposition == PROFILE_CONTROL_READY &&
        !proposals->pin.recognized)
        proposals->suggested = PROFILE_CAPTURE_FLATTEN;
    output = *proposals;
    return true;
}
static inline bool profile_capture_select_proposal(const ProfileCaptureProposals &proposals,
                                                   ProfileCaptureRepresentation representation, ProfileDocument &output,
                                                   char *why, size_t whySize) {
    const ProfileDocument* document = representation == PROFILE_CAPTURE_EXACT ? &proposals.exact : nullptr;
    const auto *proposal = representation == PROFILE_CAPTURE_FLATTEN ? &proposals.flatten
                           : representation == PROFILE_CAPTURE_PIN   ? &proposals.pin
                                                                     : nullptr;
    if (!document && (!proposal || !proposal->selectable())) {
        if (why && whySize)
            snprintf(why, whySize, "%s", proposal ? proposal->reason : "choose a capture representation explicitly");
        return false;
    }
    if (!document) document = &proposal->document;
    if (!profile_document_structurally_valid(*document, why, whySize)) return false;
    output = *document;
    snprintf(output.displayName, sizeof(output.displayName), "%s", "Captured GPU settings");
    return true;
}
static inline unsigned int profile_capture_available_representations(const ProfileCaptureProposals& proposals) {
    return profile_capture_representation_bit(PROFILE_CAPTURE_EXACT) |
        (proposals.flatten.selectable() ? profile_capture_representation_bit(PROFILE_CAPTURE_FLATTEN) : 0u) |
        (proposals.pin.selectable() ? profile_capture_representation_bit(PROFILE_CAPTURE_PIN) : 0u);
}
template <class IO>
static inline void profile_capture_report_proposals(IO &io, const ProfileCaptureProposals &proposals) {
    const ProfileCaptureLockProposal *choices[] = {&proposals.flatten, &proposals.pin};
    for (int i = 0; i < 2; ++i) {
        const auto &choice = *choices[i];
        const char *name = i ? "pin" : "flatten";
        const char *evidence = !choice.recognized                                           ? "unavailable"
                               : choice.provenance == PROFILE_LOCK_PROVENANCE_DRIVER_GETTER ? "validated getter"
                               : choice.provenance == PROFILE_LOCK_PROVENANCE_APPLIED_INTENT
                                   ? "applied intent (not read from driver)"
                                   : "flat-tail recognition";
        io.message(std::string("capture: proposal=") + name + "; evidence=" + evidence + "; anchor=" +
                   std::to_string(choice.anchorVoltageUv) + " uV; target=" + std::to_string(choice.frequencyHz) +
                   " Hz; tail=" + std::to_string(choice.tailCount) +
                   "; display-tolerance=1000000 Hz; max-deviation=" + std::to_string(choice.maximumDeviationHz) +
                   " Hz; disposition=" + profile_control_disposition_name(choice.controls.disposition) + "; " +
                   choice.reason + "\n");
    }
    if (proposals.sameTarget)
        io.message("Flatten and pin evidence agree; hard pin represents the combined state.\n");
    if (proposals.pin.recognized)
        io.message("A clock pin does not guarantee voltage.\n");
    if (proposals.conflictingTargets)
        io.message(
            "Flatten and pin targets differ. Choose their meaning explicitly; neither is adopted automatically.\n");
}
