// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The platform-neutral authoring document (§5.2 of the authoring plan):
// versioned tuning intent that survives capture, editing, slot storage,
// process restart and export WITHOUT a connected GPU. Pure and GPU-free, by
// design (§7 acceptance: "no live-GPU dependence in serialization") -- it
// has no field that can only be filled from `g_app` or `LinuxGpuState`, and
// no function here calls a driver or reads global state.
//
// This is separate from the live wire `DesiredSettings` (gpu_core.h).
// Offline file commands and local storage retain the authoring document;
// destination resolution compiles its intent into the live runtime model.
// Keeping the two separate is deliberate: this document must
// stay valid (and comparable, and exportable) for a destination GPU that is
// absent, unavailable, or does not match it at all, which the live wire
// type -- always scoped to "the selected GPU, right now" -- cannot express.
#ifndef GREEN_CURVE_PROFILE_DOCUMENT_H
#define GREEN_CURVE_PROFILE_DOCUMENT_H

#include "gpu_core.h"            // VF_NUM_POINTS, LockMode, FanCurveConfig
#include "fan_curve_api.h"       // pure fan contracts; no UI state dependency
#include "profile_point_intent.h"
#include "tuning_units.h"

#include <stdio.h>
#include <string.h>

// Which domains this document includes. A domain's absence means "no
// opinion" for application scope UPDATE_INCLUDED (§5.5): the destination's
// current state and ownership for that domain are left alone. Bit flags so
// one document can name an arbitrary subset.
enum ProfileDomainMask : int {
    PROFILE_DOMAIN_NONE = 0,
    PROFILE_DOMAIN_VF_CURVE = 1 << 0,
    PROFILE_DOMAIN_GRAPHICS_OFFSET = 1 << 1,
    PROFILE_DOMAIN_MEMORY = 1 << 2,
    PROFILE_DOMAIN_POWER = 1 << 3,
    PROFILE_DOMAIN_LOCK = 1 << 4,
    PROFILE_DOMAIN_FAN = 1 << 5,
    PROFILE_DOMAIN_XBAR = 1 << 6,
    PROFILE_DOMAIN_SYS_CLK = 1 << 7,
    PROFILE_DOMAIN_VIDEO_CLK = 1 << 8,
};

// §5.5: explicit application scope, distinct from domain membership.
enum ProfileApplicationScope : int {
    // Omitted domains retain their current hardware state and existing
    // Green Curve ownership. Default for a freshly captured or imported
    // partial document.
    PROFILE_SCOPE_UPDATE_INCLUDED = 0,
    // Release previously Green Curve-owned controls the replacement omits,
    // while preserving unrelated external controls (established named-
    // profile behavior).
    PROFILE_SCOPE_REPLACE_PROFILE = 1,
    // An explicit Reset or intentional field-clear. Never inferred from a
    // failed getter or an absent serialization key.
    PROFILE_SCOPE_EXPLICIT_RELEASE = 2,
};

enum ProfilePointSetPolicy : int { PROFILE_POINTS_MERGE = 0, PROFILE_POINTS_REPLACE = 1 };
enum ProfileExclusionPolicy : int {
    PROFILE_EXCLUDE_NONE = 0, PROFILE_EXCLUDE_IDENTITIES = 1,
    PROFILE_EXCLUDE_LEGACY_COUNT = 2,
};
enum ProfilePinKnowledge : int {
    PROFILE_PIN_UNKNOWN = 0, PROFILE_PIN_ABSENT = 1, PROFILE_PIN_PRESENT = 2,
};

// §9.1 [profile.N.lock] provenance: how this lock intent was established.
enum ProfileLockProvenance : int {
    PROFILE_LOCK_PROVENANCE_USER = 0,
    PROFILE_LOCK_PROVENANCE_RECOGNIZED_FLATTEN = 1,
    PROFILE_LOCK_PROVENANCE_DRIVER_GETTER = 2,
    PROFILE_LOCK_PROVENANCE_APPLIED_INTENT = 3,
    PROFILE_LOCK_PROVENANCE_USER_CONFIRMED_OBSERVATION = 4,
};

// §9.1 [profile.N.topology.I]: complete source graphics topology membership.
// `topologyId` is this document's own stable within-document identity
// (independent of `sourceIndex`, which is diagnostic only unless an exact
// topology match proves its meaning -- §5.2).
struct ProfileTopologyPoint {
    int topologyId;
    int sourceIndex;
    long long voltageUv;
    int sourceWritable = 0; // 0 unknown, 1 writable, 2 read-only; provenance only
};

// §9.1 [profile.N.lock]: a coherent unit, never partially applied.
struct ProfileLockIntent {
    bool present;
    LockMode mode;
    int anchorTopologyId;      // references ProfileDocument::topology[].topologyId
    long long targetFrequencyHz;
    bool trackAnchor;
    ProfileLockProvenance provenance;
    bool hasRecognitionEvidence;
    long long recognizedAnchorVoltageUv;
    int recognizedTailCount;
    long long recognitionToleranceHz;
    bool hasObservedPinRange;
    long long observedPinMinimumHz, observedPinMaximumHz;
    // A converted flatten's former tail controls live in each point's labeled
    // observed offset, with the former point authority removed after review.
};

// §9.1 [profile.N.graphics]: precise global/selective graphics offset.
// New exclusions name document topology identities. A legacy count remains
// explicitly unresolved until destination mapping can prove equivalence.
struct ProfileGraphicsOffset {
    bool present;
    long long offsetHz;
    int excludeLowCount;
    ProfileExclusionPolicy exclusionPolicy;
    int excludedCount;
    int excludedTopologyIds[VF_NUM_POINTS];
};

struct ProfilePowerIntent {
    bool present;
    TuningPowerRatio ratio;
    // Provenance only; never resolved against without re-reading a FRESH
    // destination default (§5.4).
    bool hasObservedDefaultMw;
    long long observedDefaultMw;
    bool hasObservedCurrentMw;
    long long observedCurrentMw;
};

struct ProfileFanIntent {
    bool present;
    bool fanAuto;
    int fanMode;
    int fanPercent;
    bool hasCurveConfig;
    FanCurveConfig fanCurve;
};

// Non-sensitive compatibility metadata (§5.2). Source indices elsewhere are
// diagnostic only; nothing here selects a privileged target by itself.
struct ProfileSourceMetadata {
    int osFamily;        // ProfileSourceOs
    int backendFamily;    // e.g. GpuFamily from gpu_core.h, 0 = unknown
    unsigned int deviceId;
    unsigned int subSystemId;
    // Capture completeness/provenance (§5.2): true only when every included
    // domain's read was coherent (§8.2). A document built by Save/typing
    // rather than Capture leaves this false; it is not a defect marker.
    bool captureComplete;
    char driverVersion[65];
    gc_u32 capturedDomains, omittedDomains, unavailableDomains;
    ProfilePinKnowledge pinKnowledge;
};

enum ProfileSourceOs : int {
    PROFILE_SOURCE_OS_UNKNOWN = 0,
    PROFILE_SOURCE_OS_WINDOWS = 1,
    PROFILE_SOURCE_OS_LINUX = 2,
};

static constexpr int PROFILE_DOCUMENT_MAX_DISPLAY_NAME = 128;

struct ProfileDocument {
    int documentVersion; // semantic format version; never persist raw struct bytes
    int domains;          // ProfileDomainMask bits
    ProfileApplicationScope applicationScope;
    char displayName[PROFILE_DOCUMENT_MAX_DISPLAY_NAME + 1];
    gc_u32 documentId; // file-local identity; never a destination slot
    ProfilePointSetPolicy pointSetPolicy;
    bool sourceTopologyComplete;
    bool legacyIndicesUnresolved;

    ProfileSourceMetadata source;

    int topologyCount;
    ProfileTopologyPoint topology[VF_NUM_POINTS];

    // Index-parallel to `topology`: point I's intent is for
    // `topology[I]`. Keeping them index-parallel (rather than a separate
    // topology reference per point) matches the live runtime's per-index
    // curve arrays and keeps per-point lookup O(1); topology identity
    // still travels with each entry for export/mapping.
    ProfilePointIntent points[VF_NUM_POINTS];

    ProfileGraphicsOffset graphicsOffset;
    bool hasMemoryOffset;
    long long memoryOffsetHz;
    gc_u32 memorySurface; // zero is user intent without a named source API
    ProfilePowerIntent power;
    ProfileLockIntent lock;
    ProfileFanIntent fan;

    bool hasXbarOffsetHz;
    long long xbarOffsetHz;
    bool hasXbarMsvddOffsetUv;
    long long xbarMsvddOffsetUv;
    bool hasSysClkOffsetHz;
    long long sysClkOffsetHz;
    bool hasVideoClkOffsetHz;
    long long videoClkOffsetHz;
};

static inline ProfileDocument profile_document_make_empty() {
    ProfileDocument d{};
    d.documentVersion = 1;
    d.applicationScope = PROFILE_SCOPE_UPDATE_INCLUDED;
    d.documentId = 1;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        d.topology[i].topologyId = -1;
        d.topology[i].sourceIndex = -1;
    }
    d.lock.anchorTopologyId = -1;
    return d;
}

#include "profile_document_validation.h"

static inline bool profile_topology_point_equal(const ProfileTopologyPoint& a,
    const ProfileTopologyPoint& b) {
    return a.topologyId == b.topologyId && a.sourceIndex == b.sourceIndex &&
        a.voltageUv == b.voltageUv;
}

static inline bool profile_lock_intent_semantically_equal(const ProfileLockIntent& a,
    const ProfileLockIntent& b) {
    if (a.present != b.present) return false;
    if (!a.present) return true;
    return a.mode == b.mode && a.anchorTopologyId == b.anchorTopologyId &&
        a.targetFrequencyHz == b.targetFrequencyHz && a.trackAnchor == b.trackAnchor;
    // Provenance is informational, not part of the applied meaning: the
    // same lock is the same lock whether it was typed or recognized.
}

static inline bool profile_power_intent_semantically_equal(const ProfilePowerIntent& a,
    const ProfilePowerIntent& b) {
    if (a.present != b.present) return false;
    if (!a.present) return true;
    return tuning_power_ratio_equal(a.ratio, b.ratio);
    // Observed mW values are provenance, excluded from equality.
}

static inline bool profile_fan_intent_semantically_equal(const ProfileFanIntent& a,
    const ProfileFanIntent& b) {
    if (a.present != b.present) return false;
    if (!a.present) return true;
    if (a.fanAuto != b.fanAuto || a.fanMode != b.fanMode || a.fanPercent != b.fanPercent ||
        a.hasCurveConfig != b.hasCurveConfig)
        return false;
    return !a.hasCurveConfig || fan_curve_equals(&a.fanCurve, &b.fanCurve);
}

// Semantic equality for dirty/applied-profile comparisons (§8.4): intent,
// presence, operation scope and authoritative values. Ignores every
// observational field (point preview frequencies, power's observed mW,
// lock provenance, capture completeness) and formatting-only fields
// (display name is identity, not semantics, so it is excluded too).
static inline bool profile_document_semantically_equal(const ProfileDocument& a,
    const ProfileDocument& b) {
    if (a.domains != b.domains || a.applicationScope != b.applicationScope ||
        a.pointSetPolicy != b.pointSetPolicy || a.sourceTopologyComplete != b.sourceTopologyComplete ||
        a.legacyIndicesUnresolved != b.legacyIndicesUnresolved)
        return false;
    if (a.topologyCount != b.topologyCount) return false;
    if (a.topologyCount < 0 || a.topologyCount > VF_NUM_POINTS) return false;
    for (int i = 0; i < a.topologyCount; ++i)
        if (!profile_topology_point_equal(a.topology[i], b.topology[i])) return false;
    for (int i = 0; i < VF_NUM_POINTS; ++i)
        if (!profile_point_intent_semantically_equal(a.points[i], b.points[i])) return false;
    if (a.graphicsOffset.present != b.graphicsOffset.present) return false;
    if (a.graphicsOffset.present &&
        (a.graphicsOffset.offsetHz != b.graphicsOffset.offsetHz ||
         a.graphicsOffset.excludeLowCount != b.graphicsOffset.excludeLowCount ||
         a.graphicsOffset.exclusionPolicy != b.graphicsOffset.exclusionPolicy ||
         a.graphicsOffset.excludedCount != b.graphicsOffset.excludedCount))
        return false;
    if (a.graphicsOffset.present) {
        if (a.graphicsOffset.excludedCount < 0 || a.graphicsOffset.excludedCount > VF_NUM_POINTS) return false;
        for (int i = 0; i < a.graphicsOffset.excludedCount; ++i) {
            for (int j = 0; j < i; ++j)
                if (a.graphicsOffset.excludedTopologyIds[i] == a.graphicsOffset.excludedTopologyIds[j]) return false;
            bool found = false;
            for (int j = 0; j < b.graphicsOffset.excludedCount; ++j)
                found |= a.graphicsOffset.excludedTopologyIds[i] == b.graphicsOffset.excludedTopologyIds[j];
            if (!found) return false;
        }
    }
    if (a.hasMemoryOffset != b.hasMemoryOffset) return false;
    if (a.hasMemoryOffset && (a.memoryOffsetHz != b.memoryOffsetHz || a.memorySurface != b.memorySurface)) return false;
    if (!profile_power_intent_semantically_equal(a.power, b.power)) return false;
    if (!profile_lock_intent_semantically_equal(a.lock, b.lock)) return false;
    if (!profile_fan_intent_semantically_equal(a.fan, b.fan)) return false;
    if (a.hasXbarOffsetHz != b.hasXbarOffsetHz ||
        (a.hasXbarOffsetHz && a.xbarOffsetHz != b.xbarOffsetHz)) return false;
    if (a.hasXbarMsvddOffsetUv != b.hasXbarMsvddOffsetUv ||
        (a.hasXbarMsvddOffsetUv && a.xbarMsvddOffsetUv != b.xbarMsvddOffsetUv)) return false;
    if (a.hasSysClkOffsetHz != b.hasSysClkOffsetHz ||
        (a.hasSysClkOffsetHz && a.sysClkOffsetHz != b.sysClkOffsetHz)) return false;
    if (a.hasVideoClkOffsetHz != b.hasVideoClkOffsetHz ||
        (a.hasVideoClkOffsetHz && a.videoClkOffsetHz != b.videoClkOffsetHz)) return false;
    return true;
}

#endif // GREEN_CURVE_PROFILE_DOCUMENT_H
