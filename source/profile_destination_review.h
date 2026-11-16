// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_runtime_resolution.h"
#include "profile_control_feasibility.h"
#include "profile_domain_conversions.h"
#include "profile_codec.h"

enum ProfileDestinationReviewStage {
    PROFILE_REVIEW_EMPTY, PROFILE_REVIEW_SOURCE, PROFILE_REVIEW_CAPTURE,
    PROFILE_REVIEW_MAPPED, PROFILE_REVIEW_PLANNED
};
// A read-only result, including the original source and the observation used
// for mapping. Even PLANNED/representable grants no permission to Apply: this
// contains no outgoing ownership, recovery or transition-protection proof.
// Keep on the heap; documents, topology and complete capture are intentionally
// retained rather than narrowed to a summary or reconstructed from report text.
struct ProfileDestinationReview {
    bool accepted = false; // local draft choice; never serialized or applied proof
    ProfileDestinationReviewStage stage = PROFILE_REVIEW_EMPTY;
    gc_u64 sourceRevision = 0;
    ProfileDocument source{};
    HardwareCaptureObservation capture{};
    ProfileRuntimeRoutes routes{};
    ProfileMappingReport mapping{};
    DesiredSettings runtime{};
    ProfileControlPlan controls{};
    ProfileConversionPlan conversions{};
    ProfileCodecError error{};
    bool mapped() const { return stage >= PROFILE_REVIEW_MAPPED; }
    bool planned() const { return stage == PROFILE_REVIEW_PLANNED; }
};
static inline void profile_destination_review_reset(ProfileDestinationReview& r) {
    r.accepted = false;
    r.stage = PROFILE_REVIEW_EMPTY;
    r.sourceRevision = 0;
    profile_document_reset(r.source);
    memset(&r.capture, 0, sizeof(r.capture));
    r.routes = {};
    memset(&r.mapping, 0, sizeof(r.mapping));
    for (int i = 0; i < VF_NUM_POINTS + 16; ++i) {
        r.mapping.issues[i].topologyId = -1;
        r.mapping.issues[i].destinationIndex = -1;
    }
    memset(&r.runtime, 0, sizeof(r.runtime));
    profile_control_plan_reset(r.controls);
    profile_conversion_plan_reset(r.conversions);
    memset(&r.error, 0, sizeof(r.error));
}
