// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_runtime_resolution.h"
#include "hardware_capture_runtime.h"
#include "profile_capture_representation.h"
#include "profile_control_feasibility.h"
#include "profile_document_domains.h"
#include "profile_pin_observation.h"
#include "vf_flat_tail_recognition.h"
#include "service_protocol.h"
#include "service_control_review_revision.h"
#include <algorithm>
#include <limits>
#include <string>

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
    gc_i64 voltageLockUv = 0;  // pin derived from the driver's VF voltage lock
    char reason[192]{};
    bool selectable() const { return recognized && controls.disposition <= PROFILE_CONTROL_CONDITIONAL; }
};
struct ProfileCaptureProposals {
    ProfileDocument exact{};
    // Exact replay judged like the lock proposals. An exact copy of a curve
    // another tool flattened with offsets beyond this GPU's range is refused
    // at Apply, so it must not be the recommendation.
    ProfileControlPlan exactControls{};
    char exactReason[192]{};
    ProfileCaptureLockProposal flatten{}, pin{};
    bool sameTarget = false, conflictingTargets = false;
    ProfileCaptureRepresentation suggested = PROFILE_CAPTURE_EXACT;
};
// The captured curve point at exactly `voltageUv`, or null. A voltage lock
// names a curve point's voltage; any other voltage is not turned into a clock.
static inline const HardwareCapturePoint *profile_capture_point_at_voltage(const HardwareCaptureObservation &o,
                                                                           gc_i64 voltageUv) {
    for (gc_u32 i = 0; i < o.pointCount && i < 128u; ++i)
        if (o.points[i].voltageUv == voltageUv && o.points[i].frequencyValid) return &o.points[i];
    return nullptr;
}
static inline void profile_capture_proposal_reason(ProfileCaptureLockProposal &proposal, const char *reason,
                                                   ProfileControlFeasibility status = PROFILE_CONTROL_UNAVAILABLE) {
    proposal.controls.disposition = status;
    snprintf(proposal.reason, sizeof(proposal.reason), "%s", reason);
}
// Judges one curve representation. Exact, flatten and pin carry memory,
// power, fan and the advanced clocks identically, and the user includes or
// leaves out each of those as its own row, so their writability must not
// decide which curve representation is offered. Only the curve, the core
// offset it may follow and the lock are judged here; the saved document
// itself keeps every captured domain.
static inline void profile_capture_judge_curve(const HardwareCaptureObservation &observation,
                                               const ProfileDocument &document, ProfileControlPlan &controls,
                                               char *reason, size_t reasonSize) {
    auto runtime = std::make_unique<DesiredSettings>();
    auto mapping = std::make_unique<ProfileMappingReport>();
    auto destination = std::make_unique<ProfileDestinationTopology>();
    auto judged = std::make_unique<ProfileDocument>(document);
    ProfileRuntimeRoutes routes;
    routes.graphics = judged->graphicsOffset.present ? observation.graphics.surface : 0;
    if (!profile_document_drop_domains(*judged, PROFILE_DOCUMENT_NON_CURVE_DOMAINS, reason, reasonSize) ||
        !profile_destination_from_capture(observation, *destination, reason, reasonSize) ||
        !profile_resolve_runtime(*judged, *destination, 1, routes, *runtime, *mapping, reason, reasonSize)) {
        controls.disposition = PROFILE_CONTROL_UNAVAILABLE;
        return;
    }
    controls = profile_control_plan(*runtime, observation);
    snprintf(reason, reasonSize, "%s; fresh complete operation checks remain required",
             profile_control_disposition_name(controls.disposition));
    if (const auto *issue = curve_plan_lead_issue(controls.curve))
        snprintf(reason, reasonSize, "%s: %s ci=%d worstCi=%d boundHz=%lld affected=%d",
                 profile_control_disposition_name(controls.disposition), curve_plan_reason_name(issue->reason),
                 issue->ci, issue->worstCi, issue->limitHz, issue->affectedCount);
}
static inline void profile_capture_check_proposal(const HardwareCaptureObservation &observation,
                                                  ProfileCaptureLockProposal &proposal) {
    profile_capture_judge_curve(observation, proposal.document, proposal.controls, proposal.reason,
                                sizeof(proposal.reason));
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

// The representation a frontend preselects. A recognized lock is what the
// other tool actually set, so it is recommended over copying its raw offsets
// even when its replay still needs the Apply-time checks (conditional): a
// flatten is always recognized on a shaped curve, whose live baseline can
// never prove the plan, so requiring "representable" recommended it almost
// never. A pin is recommended only on driver or Green Curve evidence (never a
// workload observation) and only when it agrees with any recognized flatten;
// conflicting targets, or no usable lock, leave the exact copy preselected.
static inline ProfileCaptureRepresentation profile_capture_suggest(const ProfileCaptureProposals &p) {
    const bool pinProven = p.pin.selectable() && (p.pin.provenance == PROFILE_LOCK_PROVENANCE_DRIVER_GETTER ||
                                                  p.pin.provenance == PROFILE_LOCK_PROVENANCE_APPLIED_INTENT);
    if (pinProven && (!p.flatten.recognized || p.sameTarget)) return PROFILE_CAPTURE_PIN;
    if (p.flatten.selectable() && !p.pin.recognized) return PROFILE_CAPTURE_FLATTEN;
    return PROFILE_CAPTURE_EXACT;
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
    auto order = std::make_unique<int[]>(VF_NUM_POINTS);
    auto normalized = std::make_unique<VFCurvePoint[]>(VF_NUM_POINTS);
    const int count = proposals->exact.topologyCount;
    for (int i = 0; i < count; ++i)
        order[i] = i;
    std::sort(order.get(), order.get() + count,
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
        const auto recognized = vf_recognize_flat_tail(normalized.get(), floor, 1, 3);
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
    gc_i64 pinHz = 0, pinVoltageUv = 0;
    ProfileLockProvenance pinProvenance = PROFILE_LOCK_PROVENANCE_USER;
    if (observation.requestedDomains & PROFILE_DOMAIN_LOCK) {
        if (observation.pinGetterValid) {
            if (observation.pinVoltageLocked && observation.pinPresent)
                profile_capture_proposal_reason(proposals->pin,
                    "another tool holds both a clock limit and a voltage lock; a fixed pin needs a separate decision");
            else if (observation.pinVoltageLocked) {
                // A VF voltage lock holds the GPU at that curve point, i.e. at
                // its frequency (observed idle and under load). Replay pins
                // that frequency, rounded down to the MHz the clock lock
                // takes so it never needs a higher voltage than the point.
                const auto point = profile_capture_point_at_voltage(observation, observation.pinVoltageUv);
                if (!point || point->frequencyHz < 1000000)
                    profile_capture_proposal_reason(proposals->pin,
                        "the driver's voltage lock names no readable curve point");
                else {
                    pinHz = point->frequencyHz / 1000000 * 1000000;
                    pinVoltageUv = observation.pinVoltageUv;
                    pinProvenance = PROFILE_LOCK_PROVENANCE_DRIVER_GETTER;
                }
            } else if (!observation.pinPresent)
                profile_capture_proposal_reason(proposals->pin, "validated getter reports no pin");
            else if (!observation.pinMinimumHz) {
                char reason[192]{};
                snprintf(reason, sizeof(reason),
                    "another tool caps the clock at %lld MHz without a minimum; only fixed clock locks can be saved",
                    (long long)(observation.pinMaximumHz / 1000000));
                profile_capture_proposal_reason(proposals->pin, reason);
            } else if (observation.pinMinimumHz != observation.pinMaximumHz)
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
            pin.voltageLockUv = pinVoltageUv;
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
    profile_capture_judge_curve(observation, proposals->exact, proposals->exactControls, proposals->exactReason,
                                sizeof(proposals->exactReason));
    proposals->suggested = profile_capture_suggest(*proposals);
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
static inline bool profile_capture_pin_has_stronger_tier(const HardwareCaptureObservation& observation,
    ProfileCaptureProposalContext context) {
    return observation.pinGetterValid != 0 || context.ownedPinHz > 0;
}
static inline bool profile_capture_apply_pin_observation(const HardwareCaptureObservation& observation,
    ProfileCaptureProposalContext context, const PinObservationResult& obs,
    ProfileCaptureProposals& proposals, gc_i64 confirmedOrEditedHz = 0) {
    if (profile_capture_pin_has_stronger_tier(observation, context)) return false;
    auto& pin = proposals.pin;
    pin.recognized = false;
    if (obs.status != PIN_OBS_SUSPECTED_PIN) {
        profile_capture_proposal_reason(pin,
            obs.explanation[0] ? obs.explanation : "workload observation yielded unknown pin state");
        proposals.sameTarget = proposals.conflictingTargets = false;
        return false;
    }
    const gc_i64 targetHz = confirmedOrEditedHz > 0 ? confirmedOrEditedHz : obs.proposedPinHz;
    if (targetHz <= 0 || targetHz > 100000000000LL || (targetHz % 1000000LL) != 0) {
        profile_capture_proposal_reason(pin,
            "pin frequency must be a positive 1 MHz multiple", PROFILE_CONTROL_UNAVAILABLE);
        proposals.sameTarget = proposals.conflictingTargets = false;
        return false;
    }
    const int count = proposals.exact.topologyCount;
    int order[VF_NUM_POINTS]{};
    for (int i = 0; i < count; ++i) order[i] = i;
    std::sort(order, order + count,
        [&](int a, int b) { return observation.points[a].voltageUv < observation.points[b].voltageUv; });
    int anchor = -1;
    for (int i = 0; i < count; ++i) {
        const int at = order[i];
        if (observation.points[at].writableState == 1 && observation.points[at].frequencyHz >= targetHz) {
            anchor = at;
            break;
        }
    }
    if (anchor < 0) {
        profile_capture_proposal_reason(pin, "confirmed pin frequency has no complete writable curve anchor");
        proposals.sameTarget = proposals.conflictingTargets = false;
        return false;
    }
    const ProfileLockProvenance provenance = (confirmedOrEditedHz > 0 && confirmedOrEditedHz != obs.proposedPinHz)
        ? PROFILE_LOCK_PROVENANCE_USER
        : PROFILE_LOCK_PROVENANCE_USER_CONFIRMED_OBSERVATION;
    profile_capture_make_lock(proposals.exact, pin, anchor, LOCK_MODE_HARD, targetHz, provenance);
    if (context.pinAvailabilityKnown && !context.pinAvailable) {
        profile_capture_proposal_reason(
            pin, "hard-pin control is unavailable; exact replay or an explicit flatten remains separate");
    } else {
        profile_capture_check_proposal(observation, pin);
        if (pin.controls.disposition == PROFILE_CONTROL_READY) {
            pin.controls.disposition = PROFILE_CONTROL_CONDITIONAL;
            snprintf(pin.reason, sizeof(pin.reason), "%s",
                provenance == PROFILE_LOCK_PROVENANCE_USER
                    ? "user-edited pin write and transition behavior require fresh validation"
                    : "user-confirmed observation pin write and transition behavior require fresh validation");
        }
    }
    proposals.sameTarget = proposals.flatten.recognized && pin.recognized &&
        proposals.flatten.frequencyHz == pin.frequencyHz;
    proposals.conflictingTargets = proposals.flatten.recognized && pin.recognized && !proposals.sameTarget;
    return true;
}
// One bounded debug line per lock representation: whether it was recognized,
// why it is or is not offered, and its anchor/target. Frontends log both
// after every review so a missing choice can be explained from the log.
static inline void profile_capture_proposal_log_line(const ProfileCaptureLockProposal& p, const char* name,
    char* out, size_t size) {
    if (!out || !size) return;
    snprintf(out, size,
        "profile-capture: proposal=%s recognized=%d offered=%d disposition=%s anchorUv=%lld hz=%lld tail=%d "
        "maxDeviationHz=%lld provenance=%d reason=%s",
        name ? name : "?", p.recognized ? 1 : 0, p.selectable() ? 1 : 0,
        profile_control_disposition_name(p.controls.disposition), (long long)p.anchorVoltageUv,
        (long long)p.frequencyHz, p.tailCount, (long long)p.maximumDeviationHz, (int)p.provenance,
        p.reason[0] ? p.reason : "-");
}
static inline void profile_capture_exact_log_line(const ProfileCaptureProposals& p, char* out, size_t size) {
    if (!out || !size) return;
    snprintf(out, size, "profile-capture: proposal=exact disposition=%s suggested=%s reason=%s",
        profile_control_disposition_name(p.exactControls.disposition),
        profile_capture_representation_name(p.suggested), p.exactReason[0] ? p.exactReason : "-");
}
template <class IO>
static inline void profile_capture_report_observation_result(IO& io, const PinObservationResult& obs) {
    io.message(std::string(obs.summary) + "\n");
    io.message(std::string("Workload pin observation: ") + obs.explanation + "\n");
}
template <class IO>
static inline void profile_capture_report_proposals(IO &io, const ProfileCaptureProposals &proposals) {
    io.message(std::string("capture: proposal=exact; disposition=") +
               profile_control_disposition_name(proposals.exactControls.disposition) + "; " + proposals.exactReason +
               "; recommended=" + profile_capture_representation_name(proposals.suggested) + "\n");
    const ProfileCaptureLockProposal *choices[] = {&proposals.flatten, &proposals.pin};
    for (int i = 0; i < 2; ++i) {
        const auto &choice = *choices[i];
        const char *name = i ? "pin" : "flatten";
        const char *evidence = !choice.recognized                                           ? "unavailable"
                               : choice.provenance == PROFILE_LOCK_PROVENANCE_DRIVER_GETTER ? "validated getter"
                               : choice.provenance == PROFILE_LOCK_PROVENANCE_APPLIED_INTENT
                                   ? "applied intent (not read from driver)"
                               : choice.provenance == PROFILE_LOCK_PROVENANCE_USER_CONFIRMED_OBSERVATION
                                   ? "user-confirmed workload observation"
                               : choice.provenance == PROFILE_LOCK_PROVENANCE_USER
                                   ? "user-edited observation"
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
