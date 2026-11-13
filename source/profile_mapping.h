// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_MAPPING_H
#define GREEN_CURVE_PROFILE_MAPPING_H
#include "profile_document.h"

// Destination evidence belongs to the authorized selected-GPU observation,
// never to imported source metadata. This pure mapper grants no write authority.
struct ProfileDestinationPoint {
    int index;
    long long voltageUv;
    bool graphics, writable;
};
struct ProfileDestinationTopology {
    bool available = false, complete = false;
    gc_u64 gpuGeneration = 0, revision = 0;
    int count = 0;
    ProfileDestinationPoint points[VF_NUM_POINTS]{};
    int knownUnavailableDomains = 0;
};
struct ProfileReviewBinding { gc_u64 sourceRevision = 0, gpuGeneration = 0, destinationRevision = 0; };
enum ProfileMappingIssueKind {
    PROFILE_MAP_OFFLINE, PROFILE_MAP_INVALID_TOPOLOGY, PROFILE_MAP_INCOMPLETE_TOPOLOGY, PROFILE_MAP_LEGACY_INDEX,
    PROFILE_MAP_LEGACY_EXCLUSIONS, PROFILE_MAP_MISSING_VOLTAGE, PROFILE_MAP_AMBIGUOUS_VOLTAGE,
    PROFILE_MAP_READ_ONLY_POINT, PROFILE_MAP_DUPLICATE_DESTINATION, PROFILE_MAP_UNAVAILABLE_DOMAIN
};
struct ProfileMappingIssue {
    ProfileMappingIssueKind kind;
    int topologyId = -1, destinationIndex = -1, domains = 0;
    long long voltageUv = 0;
};
struct ProfileMappingReport {
    bool structurallyValid = false, completeTopologyMatch = false, needsCompleteCurvePlan = false;
    bool defaultPowerWithoutSurface = false; // final Apply still checks required cleanup
    gc_u64 gpuGeneration = 0, destinationRevision = 0;
    gc_u64 sourceRevision = 0;
    int destinationByTopology[VF_NUM_POINTS]{};
    int issueCount = 0, omittedIssues = 0;
    ProfileMappingIssue issues[VF_NUM_POINTS + 16]{};
    bool ready() const { return structurallyValid && !issueCount && !omittedIssues; }
};

static inline bool profile_destination_topology_valid(const ProfileDestinationTopology& destination) {
    if (destination.count < 0 || destination.count > VF_NUM_POINTS ||
        destination.knownUnavailableDomains < 0 || (destination.knownUnavailableDomains & ~511)) return false;
    for (int i = 0; i < destination.count; ++i) {
        const auto& p = destination.points[i];
        if (p.index < 0 || p.index >= VF_NUM_POINTS || (p.graphics && p.voltageUv <= 0)) return false;
        for (int j = 0; j < i; ++j) if (p.index == destination.points[j].index) return false;
    }
    return true;
}

static inline int profile_destination_find_index(const ProfileDestinationTopology& destination, int index) {
    if (destination.count < 0 || destination.count > VF_NUM_POINTS) return -1;
    for (int i = 0; i < destination.count; ++i) if (destination.points[i].index == index) return i;
    return -1;
}

static inline bool profile_complete_topology_matches(const ProfileDocument& source,
    const ProfileDestinationTopology& destination) {
    if (!source.sourceTopologyComplete || source.legacyIndicesUnresolved || !destination.complete ||
        source.topologyCount < 0 || source.topologyCount > VF_NUM_POINTS ||
        destination.count < 0 || destination.count > VF_NUM_POINTS) return false;
    int graphics = 0;
    for (int i = 0; i < destination.count; ++i) if (destination.points[i].graphics) ++graphics;
    if (graphics != source.topologyCount) return false;
    for (int i = 0; i < source.topologyCount; ++i) {
        const auto& p = source.topology[i];
        if (p.sourceIndex < 0) return false;
        for (int j = 0; j < i; ++j) if (p.sourceIndex == source.topology[j].sourceIndex) return false;
        const int at = profile_destination_find_index(destination, p.sourceIndex);
        if (at < 0 || !destination.points[at].graphics || destination.points[at].voltageUv != p.voltageUv) return false;
    }
    return true;
}

static inline ProfileMappingReport profile_map_topology(const ProfileDocument& source,
    const ProfileDestinationTopology& destination, gc_u64 sourceRevision = 0) {
    ProfileMappingReport report;
    for (int& index : report.destinationByTopology) index = -1;
    const auto issue = [&](ProfileMappingIssueKind kind, int sourceIndex, int destinationIndex = -1, int domains = 0) {
        ProfileMappingIssue value{kind, -1, destinationIndex, domains, 0};
        if (sourceIndex >= 0 && sourceIndex < source.topologyCount) {
            value.topologyId = source.topology[sourceIndex].topologyId;
            value.voltageUv = source.topology[sourceIndex].voltageUv;
        }
        if (report.issueCount < VF_NUM_POINTS + 16) report.issues[report.issueCount++] = value;
        else ++report.omittedIssues;
    };
    char why[160]{};
    if (!profile_document_structurally_valid(source, why, sizeof(why)) || !profile_destination_topology_valid(destination)) {
        issue(PROFILE_MAP_INVALID_TOPOLOGY, -1); return report;
    }
    report.structurallyValid = true;
    report.sourceRevision = sourceRevision;
    report.gpuGeneration = destination.gpuGeneration;
    report.destinationRevision = destination.revision;
    report.needsCompleteCurvePlan = source.lock.present || source.graphicsOffset.present ||
        (source.domains & PROFILE_DOMAIN_VF_CURVE) || source.pointSetPolicy == PROFILE_POINTS_REPLACE;
    int unavailable = source.domains & destination.knownUnavailableDomains;
    if ((unavailable & PROFILE_DOMAIN_POWER) && source.power.present &&
        tuning_power_ratio_equal(source.power.ratio, {1, 1}) && source.applicationScope != PROFILE_SCOPE_EXPLICIT_RELEASE) {
        report.defaultPowerWithoutSurface = true;
        unavailable &= ~PROFILE_DOMAIN_POWER;
    }
    if (unavailable) issue(PROFILE_MAP_UNAVAILABLE_DOMAIN, -1, -1, unavailable);
    if (!destination.available) { issue(PROFILE_MAP_OFFLINE, -1); return report; }
    bool requiresTopology = source.lock.present || (source.domains & PROFILE_DOMAIN_VF_CURVE) ||
        (source.graphicsOffset.present && source.graphicsOffset.exclusionPolicy != PROFILE_EXCLUDE_NONE);
    if (requiresTopology && !destination.complete) { issue(PROFILE_MAP_INCOMPLETE_TOPOLOGY, -1); return report; }
    report.completeTopologyMatch = profile_complete_topology_matches(source, destination);
    if (source.graphicsOffset.present && source.graphicsOffset.exclusionPolicy == PROFILE_EXCLUDE_LEGACY_COUNT)
        issue(PROFILE_MAP_LEGACY_EXCLUSIONS, -1);
    bool writesDestination[VF_NUM_POINTS]{};
    for (int i = 0; i < source.topologyCount; ++i) {
        const auto& p = source.topology[i];
        const bool write = source.points[i].kind != PROFILE_POINT_ABSENT ||
            (source.lock.present && source.lock.anchorTopologyId == p.topologyId);
        bool identity = write;
        if (source.graphicsOffset.present && source.graphicsOffset.exclusionPolicy == PROFILE_EXCLUDE_IDENTITIES)
            for (int j = 0; j < source.graphicsOffset.excludedCount; ++j)
                identity |= source.graphicsOffset.excludedTopologyIds[j] == p.topologyId;
        if (source.legacyIndicesUnresolved) {
            if (identity) issue(PROFILE_MAP_LEGACY_INDEX, i);
            continue;
        }
        int selected = -1, matches = 0;
        if (report.completeTopologyMatch) {
            selected = profile_destination_find_index(destination, p.sourceIndex); matches = 1;
        } else {
            for (int j = 0; j < destination.count; ++j) if (destination.points[j].graphics &&
                destination.points[j].voltageUv == p.voltageUv) { selected = j; ++matches; }
        }
        if (matches != 1) {
            if (identity) issue(matches ? PROFILE_MAP_AMBIGUOUS_VOLTAGE : PROFILE_MAP_MISSING_VOLTAGE, i);
            continue;
        }
        const auto& mapped = destination.points[selected];
        report.destinationByTopology[i] = mapped.index;
        if (write && !mapped.writable) issue(PROFILE_MAP_READ_ONLY_POINT, i, mapped.index);
        if (write && writesDestination[mapped.index]) issue(PROFILE_MAP_DUPLICATE_DESTINATION, i, mapped.index);
        if (write) writesDestination[mapped.index] = true;
    }
    return report;
}

// Deliberate legacy-index conversion creates a DERIVED document. The caller
// retains the original, shows the exact binding and invalidates acceptance if
// the source revision or destination observation changes before Apply.
static inline bool profile_mapping_accept_legacy_indices(const ProfileDocument& source,
    const ProfileDestinationTopology& destination, gc_u64 currentSourceRevision, ProfileReviewBinding review,
    ProfileDocument& derived, char* why, size_t whySize) {
    const auto fail = [&](const char* text) { if (why && whySize) snprintf(why, whySize, "%s", text); return false; };
    if (!source.legacyIndicesUnresolved || !profile_document_structurally_valid(source, why, whySize) ||
        !destination.available || !profile_destination_topology_valid(destination) ||
        !currentSourceRevision || currentSourceRevision != review.sourceRevision || !destination.gpuGeneration ||
        !destination.revision || destination.gpuGeneration != review.gpuGeneration || destination.revision != review.destinationRevision)
        return fail("legacy binding is unavailable or changed; review it again");
    ProfileDocument next = source;
    for (int i = 0; i < source.topologyCount; ++i) {
        const int at = profile_destination_find_index(destination, source.topology[i].sourceIndex);
        if (at < 0 || !destination.points[at].graphics) return fail("a reviewed legacy index has no graphics destination");
        next.topology[i].voltageUv = destination.points[at].voltageUv;
        next.topology[i].sourceWritable = destination.points[at].writable ? 1 : 2;
    }
    next.legacyIndicesUnresolved = false;
    next.sourceTopologyComplete = false;
    next.source.captureComplete = false;
    if (!profile_document_structurally_valid(next, why, whySize)) return false;
    derived = next;
    return true;
}
#endif
