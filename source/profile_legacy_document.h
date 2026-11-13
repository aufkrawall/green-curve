// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_LEGACY_DOCUMENT_H
#define GREEN_CURVE_PROFILE_LEGACY_DOCUMENT_H
#include "profile_document.h"
#include "desired_settings_ui_policy.h"

// Adapt an already decoded historical profile without consulting live hardware.
// Indices stay explicitly unresolved: this adapter never supplies voltages from
// a different/current GPU or promotes the record into capture evidence.
static inline bool profile_document_from_legacy_desired(const DesiredSettings& in,
    ProfileDocument& output, char* why, size_t whySize) {
    ProfileDocument d = profile_document_make_empty();
    const auto fail = [&](const char* message) { if (why && whySize) snprintf(why, whySize, "%s", message); return false; };
    const gc_u32 scope = desired_application_scope_kind(in.applicationScope);
    d.applicationScope = scope == DESIRED_SCOPE_UPDATE_INCLUDED ? PROFILE_SCOPE_UPDATE_INCLUDED :
        scope == DESIRED_SCOPE_EXPLICIT_RELEASE ? PROFILE_SCOPE_EXPLICIT_RELEASE : PROFILE_SCOPE_REPLACE_PROFILE;
    d.pointSetPolicy = in.applicationScope & DESIRED_SCOPE_REPLACE_VF_POINTS ? PROFILE_POINTS_REPLACE : PROFILE_POINTS_MERGE;
    if (in.hasGpuOffset) {
        d.domains |= PROFILE_DOMAIN_GRAPHICS_OFFSET; d.graphicsOffset.present = true;
        d.graphicsOffset.offsetHz = (long long)in.gpuOffsetMHz * 1000000;
        if (in.gpuOffsetExcludeLowCount) {
            d.graphicsOffset.exclusionPolicy = PROFILE_EXCLUDE_LEGACY_COUNT;
            d.graphicsOffset.excludeLowCount = in.gpuOffsetExcludeLowCount;
        }
    }
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        const auto kind = desired_curve_point_kind(&in, ci);
        if (kind == DESIRED_CURVE_INVALID) return fail("invalid legacy point tags");
        if (kind == DESIRED_CURVE_ABSENT && !(in.hasLock && in.lockCi == ci)) continue;
        const int index = d.topologyCount++;
        d.topology[index] = {ci, ci, 0, 0};
        if (kind == DESIRED_CURVE_ABSENT) continue;
        d.domains |= PROFILE_DOMAIN_VF_CURVE;
        auto& p = d.points[index];
        if (kind == DESIRED_CURVE_CAPTURED) {
            p.kind = PROFILE_POINT_CAPTURED_OFFSET; p.capturedOffsetHz = in.curvePointCapturedOffsetHz[ci];
        } else if (kind == DESIRED_CURVE_PROJECTED) {
            if (!in.hasGpuOffset) return fail("legacy projected point requires an explicit baseline conversion");
            p.kind = PROFILE_POINT_PROJECTION;
        } else {
            p.kind = PROFILE_POINT_ABSOLUTE_FREQUENCY;
            p.absoluteFrequencyHz = (long long)in.curvePointMHz[ci] * 1000000;
        }
    }
    d.legacyIndicesUnresolved = d.topologyCount != 0;
    if (in.hasLock) {
        d.domains |= PROFILE_DOMAIN_LOCK; d.lock.present = true;
        d.lock.mode = in.lockMode; d.lock.anchorTopologyId = in.lockCi;
        d.lock.targetFrequencyHz = (long long)in.lockMHz * 1000000;
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
