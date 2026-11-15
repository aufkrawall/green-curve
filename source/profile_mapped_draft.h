// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_destination_review.h"
#include "hardware_capture_runtime.h"

// A local draft decision, never an applied proof or a portable acknowledgment.
// The service must compare the opaque control revision under its runtime lane
// and independently plan the complete operation before any setter.
static inline bool profile_mapped_draft_ready(const ProfileDestinationReview &review, const ProfileDocument &source,
                                              gc_u64 revision) {
    if (!revision || revision != review.sourceRevision || !review.planned() || !review.mapping.ready() ||
        review.controls.disposition > PROFILE_CONTROL_CONDITIONAL || !review.capture.valid ||
        review.capture.requestedDomains != 511u || !review.capture.revision || !review.capture.fingerprint ||
        !hardware_capture_observation_valid(review.capture) ||
        hardware_capture_fingerprint(review.capture) != review.capture.fingerprint ||
        review.mapping.gpuGeneration != review.capture.gpuGeneration ||
        review.mapping.destinationRevision != review.capture.revision ||
        !desired_authoring_runtime_ready(&review.runtime) || !desired_authoring_valid(&review.runtime) ||
        !desired_scalar_intent_valid(&review.runtime) || !desired_curve_intent_valid(&review.runtime))
        return false;
    auto bank = std::make_unique<ProfileBank>();
    bank->count = 1;
    std::string current, frozen;
    bank->documents[0] = source;
    if (!profile_bank_encode(*bank, current))
        return false;
    bank->documents[0] = review.source;
    return profile_bank_encode(*bank, frozen) && current == frozen;
}
static inline bool profile_mapped_draft_accept(ProfileDestinationReview &review, const ProfileDocument &source,
                                               gc_u64 revision) {
    review.accepted = profile_mapped_draft_ready(review, source, revision);
    return review.accepted;
}
static inline bool profile_mapped_draft_prepare(const ProfileDestinationReview &review, const ProfileDocument &source,
                                                gc_u64 revision, DesiredSettings &desired, gc_u64 &controlRevision) {
    if (!review.accepted || !profile_mapped_draft_ready(review, source, revision))
        return false;
    desired = review.runtime;
    controlRevision = review.capture.revision;
    return true;
}
