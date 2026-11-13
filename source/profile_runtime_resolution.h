// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_RUNTIME_RESOLUTION_H
#define GREEN_CURVE_PROFILE_RUNTIME_RESOLUTION_H
#include "profile_mapping.h"
#include "hardware_capture.h"

// Read-side destination evidence. A missing capture getter does not prove that
// a write surface is unavailable (in particular for pins and fan ownership).
static inline bool profile_destination_from_capture(const HardwareCaptureObservation& o,
    ProfileDestinationTopology& output, char* why, size_t whySize) {
    if (!o.valid || !hardware_capture_observation_valid(o)) {
        if (why && whySize) snprintf(why, whySize, "invalid destination capture");
        return false;
    }
    ProfileDestinationTopology next;
    next.available = true; next.complete = o.topologyComplete != 0;
    next.gpuGeneration = o.gpuGeneration; next.revision = o.revision;
    next.count = (int)o.pointCount;
    for (int i = 0; i < next.count; ++i) next.points[i] = {
        (int)o.points[i].index, o.points[i].voltageUv, true, o.points[i].writableState == 1};
    if (!profile_destination_topology_valid(next)) return false;
    output = next;
    return true;
}

struct ProfileRuntimeRoutes {
    gc_u32 graphics = DESIRED_GRAPHICS_AUTO;
    gc_u32 memory = CLOCK_OFFSET_SURFACE_UNSPECIFIED;
};

// Compile mapped authoring intent without rounding. This grants no permission,
// feasibility proof, local acknowledgment or hardware write. All observations
// remain in the source document, including values outside application bounds.
static inline bool profile_resolve_runtime(const ProfileDocument& source,
    const ProfileDestinationTopology& destination, gc_u64 sourceRevision,
    const ProfileRuntimeRoutes& routes, DesiredSettings& output,
    ProfileMappingReport& mapping, char* why, size_t whySize) {
    const auto fail = [&](const char* text) {
        if (why && whySize) snprintf(why, whySize, "%s", text); return false;
    };
    const auto report = profile_map_topology(source, destination, sourceRevision);
    mapping = report;
    if (!report.ready()) return fail("destination mapping requires review");
    if (source.applicationScope == PROFILE_SCOPE_EXPLICIT_RELEASE)
        return fail("release requires current ownership resolution");
    if (!sourceRevision || !destination.gpuGeneration || !destination.revision)
        return fail("destination review binding is missing");
    if (routes.graphics > DESIRED_GRAPHICS_VF_COMPONENT || routes.memory > CLOCK_OFFSET_SURFACE_GLOBAL)
        return fail("invalid destination control route");
    DesiredSettings next{}; next.lockCi = -1;
    next.applicationScope = source.applicationScope == PROFILE_SCOPE_REPLACE_PROFILE ? DESIRED_SCOPE_REPLACE_PROFILE :
        source.applicationScope == PROFILE_SCOPE_EXPLICIT_RELEASE ? DESIRED_SCOPE_EXPLICIT_RELEASE : DESIRED_SCOPE_UPDATE_INCLUDED;
    if ((source.domains & PROFILE_DOMAIN_VF_CURVE) && source.pointSetPolicy == PROFILE_POINTS_REPLACE &&
        source.applicationScope == PROFILE_SCOPE_UPDATE_INCLUDED)
        next.applicationScope |= DESIRED_SCOPE_REPLACE_VF_POINTS;
    const bool bound = source.lock.present || (source.domains & PROFILE_DOMAIN_VF_CURVE) ||
        (source.graphicsOffset.present && (source.graphicsOffset.exclusionPolicy == PROFILE_EXCLUDE_IDENTITIES ||
            routes.graphics == DESIRED_GRAPHICS_VF_COMPONENT));
    if (bound) {
        if (!destination.complete) return fail("complete destination graphics topology is required");
        next.authoring.topologyBound = 1;
        for (int i = 0; i < destination.count; ++i) if (destination.points[i].graphics) {
            const auto& p = destination.points[i];
            if (p.voltageUv > UINT_MAX) return fail("destination voltage exceeds the runtime format");
            next.authoring.voltageUv[p.index] = p.voltageUv; ++next.authoring.topologyCount;
        }
    }
    if (source.graphicsOffset.present) {
        const auto& g = source.graphicsOffset;
        if (g.offsetHz < -1000000000LL || g.offsetHz > 1000000000LL)
            return fail("graphics intent exceeds application bounds; edit the draft explicitly");
        if (g.exclusionPolicy == PROFILE_EXCLUDE_LEGACY_COUNT) return fail("legacy exclusions require identity conversion");
        if (g.exclusionPolicy == PROFILE_EXCLUDE_IDENTITIES && routes.graphics != DESIRED_GRAPHICS_VF_COMPONENT)
            return fail("identity exclusions require a destination VF component");
        next.hasGpuOffset = 1; next.gpuOffsetMHz = (int)(g.offsetHz / 1000000);
        next.authoring.graphicsPrecise = 1; next.authoring.graphicsOffsetHz = g.offsetHz;
        next.authoring.graphicsSurface = routes.graphics;
        if (g.exclusionPolicy == PROFILE_EXCLUDE_IDENTITIES) {
            next.authoring.identityExclusions = 1;
            for (int i = 0; i < g.excludedCount; ++i) {
                const int at = profile_topology_index(source, g.excludedTopologyIds[i]);
                if (at < 0 || report.destinationByTopology[at] < 0) return fail("unmapped graphics exclusion");
                next.authoring.graphicsExcluded[report.destinationByTopology[at]] = 1;
            }
        }
    }
    for (int i = 0; i < source.topologyCount; ++i) {
        const auto& p = source.points[i];
        if (p.kind == PROFILE_POINT_ABSENT) continue;
        const int ci = report.destinationByTopology[i];
        if (ci < 0 || ci >= VF_NUM_POINTS) return fail("unmapped point authority");
        if (p.kind == PROFILE_POINT_CAPTURED_OFFSET) {
            next.curvePointCaptured[ci] = 1; next.curvePointCapturedOffsetHz[ci] = p.capturedOffsetHz;
        } else {
            next.hasCurvePoint[ci] = 1;
            if (p.kind == PROFILE_POINT_PROJECTION) {
                next.curvePointFromGpuOffset[ci] = 1;
                if (p.hasObservedFrequency && p.observedFrequencyHz > 0 && p.observedFrequencyHz <= 5000000000LL)
                    next.curvePointMHz[ci] = (unsigned)(p.observedFrequencyHz / 1000000);
            } else {
                if (p.absoluteFrequencyHz <= 0 || p.absoluteFrequencyHz > 5000000000LL)
                    return fail("point frequency exceeds application bounds; edit the draft explicitly");
                next.curvePointMHz[ci] = (unsigned)(p.absoluteFrequencyHz / 1000000);
                next.authoring.pointFrequencyPrecise[ci] = 1;
                next.authoring.pointFrequencyHz[ci] = p.absoluteFrequencyHz;
            }
        }
    }
    if (source.lock.present) {
        const int at = profile_topology_index(source, source.lock.anchorTopologyId);
        if (at < 0 || report.destinationByTopology[at] < 0) return fail("unmapped lock anchor");
        next.hasLock = 1; next.lockCi = report.destinationByTopology[at]; next.lockMode = source.lock.mode;
        if (source.lock.targetFrequencyHz <= 0 || source.lock.targetFrequencyHz > 5000000000LL)
            return fail("lock frequency exceeds application bounds; edit the draft explicitly");
        next.lockMHz = (unsigned)(source.lock.targetFrequencyHz / 1000000);
        next.lockTracksAnchor = source.lock.trackAnchor;
        next.authoring.lockPrecise = 1; next.authoring.lockFrequencyHz = source.lock.targetFrequencyHz;
    }
    if (source.hasMemoryOffset && !desired_memory_set_precise(&next, source.memoryOffsetHz,
            source.memorySurface ? source.memorySurface : routes.memory))
        return fail("memory intent exceeds application bounds; edit the draft explicitly");
    if (source.power.present && !desired_power_set_ratio(&next, source.power.ratio)) return fail("invalid power ratio");
    if (source.fan.present) {
        next.hasFan = 1; next.fanAuto = source.fan.fanAuto; next.fanMode = source.fan.fanMode;
        next.fanPercent = source.fan.fanPercent;
        if (source.fan.hasCurveConfig) next.fanCurve = source.fan.fanCurve;
    }
    const auto clock = [&](bool present, gc_i64 value, gc_bool8& has, int& khz) {
        if (!present) return true;
        if (value % 1000 || value < -1000000000LL || value > 1000000000LL) return false;
        has = 1; khz = (int)(value / 1000); return true;
    };
    if (!clock(source.hasXbarOffsetHz, source.xbarOffsetHz, next.hasXbarOffsetKhz, next.xbarOffsetKhz) ||
        !clock(source.hasSysClkOffsetHz, source.sysClkOffsetHz, next.hasSysClkOffsetKhz, next.sysClkOffsetKhz) ||
        !clock(source.hasVideoClkOffsetHz, source.videoClkOffsetHz, next.hasVideoClkOffsetKhz, next.videoClkOffsetKhz))
        return fail("auxiliary clock intent requires explicit grid conversion or correction");
    if (source.hasXbarMsvddOffsetUv) {
        if (source.xbarMsvddOffsetUv < -100000 || source.xbarMsvddOffsetUv > 100000)
            return fail("auxiliary voltage exceeds application bounds; edit the draft explicitly");
        next.hasXbarMsvddOffsetUv = 1; next.xbarMsvddOffsetUv = (int)source.xbarMsvddOffsetUv;
    }
    if (!desired_curve_intent_valid(&next) || !desired_scalar_intent_valid(&next) || !desired_authoring_valid(&next))
        return fail("mapped runtime intent is contradictory or out of bounds");
    output = next;
    return true;
}
#endif
