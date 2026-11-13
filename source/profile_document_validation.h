// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_DOCUMENT_VALIDATION_H
#define GREEN_CURVE_PROFILE_DOCUMENT_VALIDATION_H
// Included after the document types. Structural validity never tests hardware
// feasibility, changes a target, repairs a legacy index or chooses a GPU.
#include "profile_text_validation.h"

static inline int profile_topology_index(const ProfileDocument& d, int id) {
    if (d.topologyCount < 0 || d.topologyCount > VF_NUM_POINTS) return -1;
    for (int i = 0; i < d.topologyCount; ++i) if (d.topology[i].topologyId == id) return i;
    return -1;
}

static inline bool profile_document_structurally_valid(const ProfileDocument& d,
    char* why, size_t whySize) {
    if (why && whySize) why[0] = 0;
    const auto fail = [&](const char* reason) {
        if (why && whySize) snprintf(why, whySize, "%s", reason);
        return false;
    };
    constexpr int allDomains = (1 << 9) - 1;
    if (d.documentVersion != 1 || !d.documentId) return fail("unsupported document version or missing identity");
    if (d.domains < 0 || (d.domains & ~allDomains) ||
        d.applicationScope < PROFILE_SCOPE_UPDATE_INCLUDED || d.applicationScope > PROFILE_SCOPE_EXPLICIT_RELEASE ||
        d.pointSetPolicy < PROFILE_POINTS_MERGE || d.pointSetPolicy > PROFILE_POINTS_REPLACE)
        return fail("invalid domains, application scope or point-set policy");
    if (!profile_bounded_text_valid(d.displayName, sizeof(d.displayName), PROFILE_DOCUMENT_MAX_DISPLAY_NAME) ||
        !profile_bounded_text_valid(d.source.driverVersion, sizeof(d.source.driverVersion), 64))
        return fail("invalid UTF-8, control character or overlong text");
    if (d.source.osFamily < PROFILE_SOURCE_OS_UNKNOWN || d.source.osFamily > PROFILE_SOURCE_OS_LINUX ||
        d.source.backendFamily < 0 || d.source.pinKnowledge < PROFILE_PIN_UNKNOWN ||
        d.source.pinKnowledge > PROFILE_PIN_PRESENT ||
        ((d.source.capturedDomains | d.source.omittedDomains | d.source.unavailableDomains) & ~(gc_u32)allDomains))
        return fail("invalid source metadata");
    if (d.topologyCount < 0 || d.topologyCount > VF_NUM_POINTS ||
        (d.sourceTopologyComplete && d.legacyIndicesUnresolved)) return fail("invalid topology declaration");
    for (int i = 0; i < d.topologyCount; ++i) {
        const ProfileTopologyPoint& p = d.topology[i];
        if (p.topologyId < 0 || p.sourceIndex < -1 || p.sourceIndex >= VF_NUM_POINTS ||
            p.sourceWritable < 0 || p.sourceWritable > 2 ||
            (d.legacyIndicesUnresolved ? (p.voltageUv != 0 || p.sourceIndex < 0) : p.voltageUv <= 0))
            return fail("invalid topology identity, voltage or legacy evidence");
        for (int j = 0; j < i; ++j)
            if (p.topologyId == d.topology[j].topologyId ||
                (d.legacyIndicesUnresolved && p.sourceIndex == d.topology[j].sourceIndex))
                return fail("duplicate topology identity or legacy index");
    }
    bool pointIntent = false;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        const ProfilePointIntent& p = d.points[i];
        if (!profile_point_intent_structurally_valid(p) ||
            (p.hasObservedFrequency && p.observedFrequencyHz <= 0)) return fail("invalid point intent or observation");
        if (i >= d.topologyCount && (p.kind != PROFILE_POINT_ABSENT || p.hasObservedFrequency || p.hasObservedOffset))
            return fail("point does not reference document topology");
        if (p.kind == PROFILE_POINT_PROJECTION && !d.graphicsOffset.present)
            return fail("projection has no included graphics component");
        pointIntent |= p.kind != PROFILE_POINT_ABSENT;
    }
    int present = pointIntent ? PROFILE_DOMAIN_VF_CURVE : 0;
    if (d.graphicsOffset.present) present |= PROFILE_DOMAIN_GRAPHICS_OFFSET;
    if (d.hasMemoryOffset) present |= PROFILE_DOMAIN_MEMORY;
    if (d.power.present) present |= PROFILE_DOMAIN_POWER;
    if (d.lock.present) present |= PROFILE_DOMAIN_LOCK;
    if (d.fan.present) present |= PROFILE_DOMAIN_FAN;
    if (d.hasXbarOffsetHz || d.hasXbarMsvddOffsetUv) present |= PROFILE_DOMAIN_XBAR;
    if (d.hasSysClkOffsetHz) present |= PROFILE_DOMAIN_SYS_CLK;
    if (d.hasVideoClkOffsetHz) present |= PROFILE_DOMAIN_VIDEO_CLK;
    if ((present & ~d.domains) || (d.applicationScope != PROFILE_SCOPE_EXPLICIT_RELEASE &&
        ((present ^ d.domains) & ~PROFILE_DOMAIN_VF_CURVE))) return fail("domain declaration contradicts included controls");
    if (d.hasMemoryOffset && d.memorySurface > CLOCK_OFFSET_SURFACE_GLOBAL)
        return fail("unknown memory control surface");
    if (d.graphicsOffset.present) {
        const ProfileGraphicsOffset& g = d.graphicsOffset;
        if (g.exclusionPolicy < PROFILE_EXCLUDE_NONE || g.exclusionPolicy > PROFILE_EXCLUDE_LEGACY_COUNT ||
            g.excludedCount < 0 || g.excludedCount > VF_NUM_POINTS || g.excludeLowCount < 0 ||
            g.excludeLowCount > VF_NUM_POINTS ||
            (g.exclusionPolicy != PROFILE_EXCLUDE_IDENTITIES && g.excludedCount) ||
            (g.exclusionPolicy != PROFILE_EXCLUDE_LEGACY_COUNT && g.excludeLowCount))
            return fail("invalid graphics exclusion policy");
        for (int i = 0; i < g.excludedCount; ++i) {
            if (profile_topology_index(d, g.excludedTopologyIds[i]) < 0) return fail("dangling graphics exclusion identity");
            for (int j = 0; j < i; ++j) if (g.excludedTopologyIds[i] == g.excludedTopologyIds[j])
                return fail("duplicate graphics exclusion identity");
        }
    }
    if (d.power.present && !tuning_power_ratio_valid(d.power.ratio)) return fail("invalid power ratio");
    if ((d.power.hasObservedDefaultMw && d.power.observedDefaultMw <= 0) ||
        (d.power.hasObservedCurrentMw && d.power.observedCurrentMw <= 0)) return fail("invalid observed power");
    if (d.lock.present && (profile_topology_index(d, d.lock.anchorTopologyId) < 0 ||
        d.lock.mode < LOCK_MODE_FLATTEN || d.lock.mode > LOCK_MODE_HARD || d.lock.targetFrequencyHz <= 0))
        return fail("invalid lock mode, target or topology reference");
    if (d.lock.provenance < PROFILE_LOCK_PROVENANCE_USER ||
        d.lock.provenance > PROFILE_LOCK_PROVENANCE_USER_CONFIRMED_OBSERVATION) return fail("unknown lock provenance");
    if (d.lock.hasRecognitionEvidence && (d.lock.recognizedAnchorVoltageUv <= 0 ||
        d.lock.recognizedTailCount < 1 || d.lock.recognizedTailCount > VF_NUM_POINTS ||
        d.lock.recognitionToleranceHz < 0)) return fail("invalid lock recognition evidence");
    if (d.lock.hasObservedPinRange && (d.lock.observedPinMinimumHz <= 0 ||
        d.lock.observedPinMaximumHz < d.lock.observedPinMinimumHz)) return fail("invalid observed pin range");
    if (d.fan.present) {
        if (d.fan.fanMode < FAN_MODE_AUTO || d.fan.fanMode > FAN_MODE_CURVE ||
            d.fan.fanAuto != (d.fan.fanMode == FAN_MODE_AUTO) || d.fan.fanPercent < 0 || d.fan.fanPercent > 100 ||
            (d.fan.fanMode == FAN_MODE_CURVE && !d.fan.hasCurveConfig)) return fail("invalid fan intent");
        if (d.fan.hasCurveConfig) {
            if (!fan_curve_wire_flags_valid(&d.fan.fanCurve)) return fail("invalid fan curve flags");
            if (!fan_curve_validate(&d.fan.fanCurve, why, whySize)) return false;
        }
    }
    return true;
}
#endif
