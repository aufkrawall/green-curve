// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_LEGACY_DOCUMENT_H
#define GREEN_CURVE_PROFILE_LEGACY_DOCUMENT_H
#include "profile_document.h"
#include "desired_settings_ui_policy.h"

// Adapt an already decoded runtime record without consulting live hardware.
// Indices stay explicitly unresolved: this adapter never supplies voltages from
// a different/current GPU or promotes the record into capture evidence. A
// record bound to its own GPU's voltages (a capture, or an imported
// voltage-bound profile) exports by those saved voltages, so an import maps it
// exactly or refuses, as the record itself does at Apply.
static inline bool profile_document_from_legacy_desired(const DesiredSettings& in,
    ProfileDocument& output, char* why, size_t whySize) {
    ProfileDocument d = profile_document_make_empty();
    const auto fail = [&](const char* message) { if (why && whySize) snprintf(why, whySize, "%s", message); return false; };
    const gc_u32 scope = desired_application_scope_kind(in.applicationScope);
    d.applicationScope = scope == DESIRED_SCOPE_UPDATE_INCLUDED ? PROFILE_SCOPE_UPDATE_INCLUDED :
        scope == DESIRED_SCOPE_EXPLICIT_RELEASE ? PROFILE_SCOPE_EXPLICIT_RELEASE : PROFILE_SCOPE_REPLACE_PROFILE;
    d.explicitReplacementScope = scope == DESIRED_SCOPE_REPLACE_PROFILE;
    d.pointSetPolicy = in.applicationScope & DESIRED_SCOPE_REPLACE_VF_POINTS ? PROFILE_POINTS_REPLACE : PROFILE_POINTS_MERGE;
    if (scope == DESIRED_SCOPE_UPDATE_INCLUDED && d.pointSetPolicy == PROFILE_POINTS_REPLACE)
        d.domains |= PROFILE_DOMAIN_VF_CURVE;
    if (in.hasGpuOffset) {
        d.domains |= PROFILE_DOMAIN_GRAPHICS_OFFSET; d.graphicsOffset.present = true;
        d.graphicsOffset.offsetHz = in.authoring.graphicsPrecise ? in.authoring.graphicsOffsetHz :
            (long long)in.gpuOffsetMHz * 1000000;
        d.graphicsOffset.controlSurface = in.authoring.graphicsSurface;
        if (in.gpuOffsetExcludeLowCount) {
            d.graphicsOffset.exclusionPolicy = PROFILE_EXCLUDE_LEGACY_COUNT;
            d.graphicsOffset.excludeLowCount = in.gpuOffsetExcludeLowCount;
        }
    }
    const bool bound = in.authoring.topologyBound != 0;
    const bool identities = bound && in.authoring.identityExclusions && in.hasGpuOffset;
    if (identities) d.graphicsOffset.exclusionPolicy = PROFILE_EXCLUDE_IDENTITIES;
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        const auto kind = desired_curve_point_kind(&in, ci);
        if (kind == DESIRED_CURVE_INVALID) return fail("invalid legacy point tags");
        const bool excluded = identities && in.authoring.graphicsExcluded[ci];
        const bool savedVoltage = bound && in.authoring.voltageUv[ci] > 0;
        if (!savedVoltage && kind == DESIRED_CURVE_ABSENT && !(in.hasLock && in.lockCi == ci) && !excluded) continue;
        const int index = d.topologyCount++;
        d.topology[index] = {ci, ci, 0, 0};
        if (bound) {
            if (in.authoring.voltageUv[ci] <= 0) return fail("a voltage-bound curve point has no saved voltage");
            d.topology[index].voltageUv = in.authoring.voltageUv[ci];
        }
        if (excluded) d.graphicsOffset.excludedTopologyIds[d.graphicsOffset.excludedCount++] = ci;
        if (kind == DESIRED_CURVE_ABSENT) continue;
        d.domains |= PROFILE_DOMAIN_VF_CURVE;
        auto& p = d.points[index];
        if (kind == DESIRED_CURVE_CAPTURED) {
            p.kind = PROFILE_POINT_CAPTURED_OFFSET; p.capturedOffsetHz = in.curvePointCapturedOffsetHz[ci];
        } else if (kind == DESIRED_CURVE_PROJECTED) {
            if (!in.hasGpuOffset) return fail("legacy projected point requires an explicit baseline conversion");
            p.kind = PROFILE_POINT_PROJECTION;
            // The saved MHz is what an ordinary slot shows for a projected
            // point; it stays a labeled observation, never authority.
            if (in.curvePointMHz[ci]) {
                p.hasObservedFrequency = true;
                p.observedFrequencyHz = (long long)in.curvePointMHz[ci] * 1000000;
            }
        } else {
            p.kind = PROFILE_POINT_ABSOLUTE_FREQUENCY;
            p.absoluteFrequencyHz = in.authoring.pointFrequencyPrecise[ci] ? in.authoring.pointFrequencyHz[ci] :
                (long long)in.curvePointMHz[ci] * 1000000;
        }
    }
    if (bound && (!d.topologyCount || (gc_u32)d.topologyCount != in.authoring.topologyCount))
        return fail("the saved voltage topology is incomplete");
    d.sourceTopologyComplete = bound;
    d.legacyIndicesUnresolved = !bound && d.topologyCount != 0;
    if (in.hasLock && scope == DESIRED_SCOPE_EXPLICIT_RELEASE && in.lockMode == LOCK_MODE_NONE) {
        d.domains |= PROFILE_DOMAIN_LOCK;
    } else if (in.hasLock) {
        d.domains |= PROFILE_DOMAIN_LOCK; d.lock.present = true;
        d.lock.mode = in.lockMode; d.lock.anchorTopologyId = in.lockCi;
        d.lock.targetFrequencyHz = in.authoring.lockPrecise ? in.authoring.lockFrequencyHz :
            (long long)in.lockMHz * 1000000;
        d.lock.trackAnchor = in.lockTracksAnchor != 0;
    }
    if (in.hasMemOffset) {
        d.domains |= PROFILE_DOMAIN_MEMORY; d.hasMemoryOffset = true;
        d.memoryOffsetHz = desired_memory_hz(in);
        d.memorySurface = in.memOffsetPrecise ? in.memOffsetSurface : 0;
    }
    if (in.hasPowerLimit) {
        d.domains |= PROFILE_DOMAIN_POWER; d.power.present = true;
        d.power.ratio = desired_power_ratio(in);
    }
    if (in.hasFan) {
        d.domains |= PROFILE_DOMAIN_FAN; d.fan.present = true;
        d.fan.fanMode = in.fanMode; d.fan.fanAuto = in.fanMode == FAN_MODE_AUTO;
        d.fan.fanPercent = in.fanPercent;
        // Historical records always persisted the dormant configuration too.
        d.fan.hasCurveConfig = true; d.fan.fanCurve = in.fanCurve;
    }
    if (in.hasXbarOffsetKhz || in.hasXbarMsvddOffsetUv) d.domains |= PROFILE_DOMAIN_XBAR;
    d.hasXbarOffsetHz = in.hasXbarOffsetKhz != 0; d.xbarOffsetHz = (long long)in.xbarOffsetKhz * 1000;
    d.hasXbarMsvddOffsetUv = in.hasXbarMsvddOffsetUv != 0; d.xbarMsvddOffsetUv = in.xbarMsvddOffsetUv;
    if (in.hasSysClkOffsetKhz) d.domains |= PROFILE_DOMAIN_SYS_CLK;
    d.hasSysClkOffsetHz = in.hasSysClkOffsetKhz != 0; d.sysClkOffsetHz = (long long)in.sysClkOffsetKhz * 1000;
    if (in.hasVideoClkOffsetKhz) d.domains |= PROFILE_DOMAIN_VIDEO_CLK;
    d.hasVideoClkOffsetHz = in.hasVideoClkOffsetKhz != 0; d.videoClkOffsetHz = (long long)in.videoClkOffsetKhz * 1000;
    if (!profile_document_structurally_valid(d, why, whySize)) return false;
    output = d;
    return true;
}
#endif
