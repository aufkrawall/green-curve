// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// One VF point's authoritative meaning inside the new authoring document
// (profile_document.h), per §5.3 of the authoring plan. Four mutually
// exclusive kinds; exactly one payload field is authoritative per kind, and
// the rest are cleared at trust boundaries rather than left to disagree with
// it. Pure and GPU-free, like tuning_units.h.
//
// This is the DOCUMENT's answer to "what does this point mean?", a distinct
// question from the live runtime's existing curve_point_offset_policy.h
// (`curve_point_offset_is_intent()`), which answers "what offset does this
// point want the DRIVER to write?" over the CURRENT `DesiredSettings` wire
// layout. The two converge once the schema migration (step 5) lets the
// runtime carry a point's real kind instead of inferring it from
// hasCurvePoint/curvePointFromGpuOffset; until then this type has no
// live-GPU dependence at all, by design (§7 acceptance: "no live-GPU
// dependence in serialization").
#ifndef GREEN_CURVE_PROFILE_POINT_INTENT_H
#define GREEN_CURVE_PROFILE_POINT_INTENT_H

#include "tuning_units.h"

enum ProfilePointKind : int {
    // No requested value at this point. Does not by itself prescribe
    // cleanup; the document's application scope decides that (§5.5).
    PROFILE_POINT_ABSENT = 0,
    // A physical frequency target the user typed or an import resolved to
    // one. Compiled against a baseline and verified under tolerance.
    PROFILE_POINT_ABSOLUTE_FREQUENCY = 1,
    // Governed by the document's global/selective offset and exclusion
    // policy (ProfileDocument::graphicsOffset*), not by a per-point payload.
    // Its MHz is a preview only; the component and relationship are
    // compiled/verified.
    PROFILE_POINT_PROJECTION = 2,
    // A physical signed VF offset read from the driver, replayed through
    // the correct surface. Zero is a valid owned offset (contract 6):
    // PROFILE_POINT_ABSENT and a captured zero are never interchangeable.
    PROFILE_POINT_CAPTURED_OFFSET = 3,
};

static inline const char* profile_point_kind_name(ProfilePointKind kind) {
    switch (kind) {
        case PROFILE_POINT_ABSOLUTE_FREQUENCY: return "absolute";
        case PROFILE_POINT_PROJECTION: return "projection";
        case PROFILE_POINT_CAPTURED_OFFSET: return "captured";
        default: return "absent";
    }
}

struct ProfilePointIntent {
    ProfilePointKind kind;
    long long absoluteFrequencyHz; // authoritative only for ABSOLUTE_FREQUENCY
    long long capturedOffsetHz;    // authoritative only for CAPTURED_OFFSET
    // A separately labeled, non-authoritative preview (a live sample at
    // capture/save time, or none at all). Never substituted for a missing
    // required payload; never compared or persisted as if authoritative.
    bool hasObservedFrequency;
    long long observedFrequencyHz;
    bool hasObservedOffset;
    long long observedOffsetHz; // provenance after a reviewed flatten conversion
};

// A kind's own required payload must actually be present; this rejects a
// structurally contradictory record rather than silently picking one
// interpretation (§5.3: "must reject contradictory old/new flags and
// values; it must not silently select one").
static inline bool profile_point_intent_structurally_valid(const ProfilePointIntent& p) {
    switch (p.kind) {
        case PROFILE_POINT_ABSENT:
        case PROFILE_POINT_PROJECTION:
            return p.absoluteFrequencyHz == 0 && p.capturedOffsetHz == 0;
        case PROFILE_POINT_ABSOLUTE_FREQUENCY:
            return p.absoluteFrequencyHz > 0 && p.capturedOffsetHz == 0;
        case PROFILE_POINT_CAPTURED_OFFSET:
            return p.absoluteFrequencyHz == 0; // signed offset zero is owned
        default:
            return false; // an unrecognized kind is never structurally valid
    }
}

// Clears every payload field this kind does not own, so a trust boundary
// (IPC, disk, import) cannot smuggle a stale value through an inactive
// field (§5.3: "Clear inactive payload fields at trust boundaries").
static inline ProfilePointIntent profile_point_intent_canonical(ProfilePointIntent p) {
    if (p.kind != PROFILE_POINT_ABSOLUTE_FREQUENCY) p.absoluteFrequencyHz = 0;
    if (p.kind != PROFILE_POINT_CAPTURED_OFFSET) p.capturedOffsetHz = 0;
    if (!p.hasObservedFrequency) p.observedFrequencyHz = 0;
    if (!p.hasObservedOffset) p.observedOffsetHz = 0;
    return p;
}

// Semantic equality for dirty/applied-profile comparisons (§8.4): compares
// kind and authoritative payload only. A moving observed-frequency preview
// (temperature, boost) must never mark the editor dirty or clear applied
// identity, so it is deliberately excluded here.
static inline bool profile_point_intent_semantically_equal(
    const ProfilePointIntent& a, const ProfilePointIntent& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case PROFILE_POINT_ABSOLUTE_FREQUENCY:
            return a.absoluteFrequencyHz == b.absoluteFrequencyHz;
        case PROFILE_POINT_CAPTURED_OFFSET:
            return a.capturedOffsetHz == b.capturedOffsetHz;
        case PROFILE_POINT_ABSENT:
        case PROFILE_POINT_PROJECTION:
            return true; // ABSENT / PROJECTION carry no per-point payload
        default:
            return false;
    }
}

// Typing an explicit MHz value converts the point to absolute intent and
// removes any captured/projected authority (§5.3). The observed-frequency
// preview, if any, is left untouched: it remains exactly what it always
// was, a non-authoritative sample, now simply stale until the next read.
static inline ProfilePointIntent profile_point_intent_type_absolute_mhz(
    ProfilePointIntent p, long long mhz) {
    p.kind = PROFILE_POINT_ABSOLUTE_FREQUENCY;
    p.absoluteFrequencyHz = tuning_mhz_to_hz(mhz);
    p.capturedOffsetHz = 0;
    return p;
}

#endif // GREEN_CURVE_PROFILE_POINT_INTENT_H
