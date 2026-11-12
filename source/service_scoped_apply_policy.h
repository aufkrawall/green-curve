// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_SERVICE_SCOPED_APPLY_POLICY_H
#define GREEN_CURVE_SERVICE_SCOPED_APPLY_POLICY_H

#include "vf_writable_point_policy.h"
#include <stdio.h>

// The hardware transition includes cleanup and retained graphics dependencies.
// Only `owned` becomes durable ownership after a verified successful Apply.
struct ServiceScopedApplyPlan {
    DesiredSettings hardware;
    DesiredSettings owned;
    int cleanupPoints;
    bool retainedLock;
    bool releasesCurveCeiling;
    bool graphicsOffsetViaVf;
};

static inline bool service_scoped_has_owned_intent(const DesiredSettings& d) {
    if (d.hasGpuOffset || d.hasMemOffset || d.hasPowerLimit || d.hasFan || d.hasLock ||
        d.hasXbarOffsetKhz || d.hasXbarMsvddOffsetUv || d.hasSysClkOffsetKhz || d.hasVideoClkOffsetKhz)
        return true;
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci)
        if (desired_curve_point_present(&d, ci)) return true;
    return false;
}

static inline void service_scoped_copy_lock(DesiredSettings* out, const DesiredSettings& in) {
    out->hasLock = in.hasLock;
    out->lockCi = in.lockCi;
    out->lockMHz = in.lockMHz;
    out->lockMode = in.lockMode;
    out->lockTracksAnchor = in.lockTracksAnchor;
}

static inline bool service_scoped_point_owned(const DesiredSettings& d, int ci,
    int ordinal, bool gpuOffsetInVf) {
    return desired_curve_point_present(&d, ci) ||
        (d.hasLock && d.lockMode != LOCK_MODE_NONE && d.lockCi >= 0 && ci >= d.lockCi) ||
        (gpuOffsetInVf && d.hasGpuOffset && ordinal >= d.gpuOffsetExcludeLowCount);
}

static inline bool service_scoped_graphics_offset_in_vf(const DesiredSettings& d,
    bool uniformViaVf, bool composeWithCurve) {
    bool points = false;
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) points |= desired_curve_point_present(&d, ci);
    return d.hasGpuOffset && (uniformViaVf || d.gpuOffsetExcludeLowCount > 0 ||
        (composeWithCurve && (points || (d.hasLock && d.lockMode != LOCK_MODE_NONE))));
}

static inline bool service_scoped_graphics_work(const DesiredSettings* old,
    const DesiredSettings& request, bool uniformViaVf, bool composeWithCurve = false) {
    bool oldPoints = false, requestPoints = false;
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        oldPoints |= desired_curve_point_present(old, ci);
        requestPoints |= desired_curve_point_present(&request, ci);
    }
    const bool oldGraph = old && (oldPoints || old->hasLock ||
        service_scoped_graphics_offset_in_vf(*old, uniformViaVf, composeWithCurve));
    return requestPoints || (request.applicationScope & DESIRED_SCOPE_REPLACE_VF_POINTS) ||
        (request.hasLock && (request.lockMode != LOCK_MODE_NONE || (old && old->hasLock))) ||
        (request.hasGpuOffset && (service_scoped_graphics_offset_in_vf(request, uniformViaVf, composeWithCurve) || oldGraph)) ||
        (desired_application_scope_kind(request.applicationScope) == DESIRED_SCOPE_REPLACE_PROFILE && oldGraph);
}

static inline bool service_build_scoped_apply_plan(const DesiredSettings* previous,
    const DesiredSettings& requested, const VFCurvePoint* curve,
    const VfFloorTopology& floor, bool uniformGpuOffsetInVf,
    ServiceScopedApplyPlan* out, char* why, size_t whySize, bool composeGpuWithCurve = false) {
    auto fail = [&](const char* message) {
        if (why && whySize) snprintf(why, whySize, "%s", message);
        return false;
    };
    if (!out || !desired_application_scope_valid(requested.applicationScope) ||
        requested.applicationScope == DESIRED_SCOPE_LEGACY)
        return fail("An explicit valid application scope is required");
    if (requested.resetOcBeforeApply)
        return fail("Explicit application scope cannot request a global baseline reset");
    if (!desired_curve_intent_valid(&requested)) return fail("Contradictory curve intent");
    const gc_u32 scope = desired_application_scope_kind(requested.applicationScope);
    const bool release = scope == DESIRED_SCOPE_EXPLICIT_RELEASE;
    const bool replace = scope == DESIRED_SCOPE_REPLACE_PROFILE;
    ServiceScopedApplyPlan result{};
    DesiredSettings old{};
    old.lockCi = -1;
    if (previous) old = *previous;
    result.owned = replace ? requested : old;
    DesiredSettings& owned = result.owned;
    // A release is an operation, never an instruction to release remaining
    // ownership again during automatic replay.
    owned.applicationScope = release ? (gc_u32)DESIRED_SCOPE_UPDATE_INCLUDED : requested.applicationScope;
    owned.resetOcBeforeApply = false;
    if (!replace) {
        if (requested.hasGpuOffset) {
            owned.hasGpuOffset = !release;
            owned.gpuOffsetMHz = release ? 0 : requested.gpuOffsetMHz;
            owned.gpuOffsetExcludeLowCount = release ? 0 : requested.gpuOffsetExcludeLowCount;
        }
        if (requested.hasMemOffset) {
            owned.hasMemOffset = !release;
            owned.memOffsetMHz = release ? 0 : requested.memOffsetMHz;
        }
        if (requested.hasPowerLimit) {
            owned.hasPowerLimit = !release;
            owned.powerLimitPct = release ? 100 : requested.powerLimitPct;
        }
        if (requested.hasFan) {
            owned.hasFan = !release;
            owned.fanAuto = release || requested.fanAuto;
            owned.fanMode = release ? FAN_MODE_AUTO : requested.fanMode;
            owned.fanPercent = release ? 0 : requested.fanPercent;
            owned.fanCurve = release ? FanCurveConfig{} : requested.fanCurve;
        }
        if (requested.hasXbarOffsetKhz) {
            owned.hasXbarOffsetKhz = !release;
            owned.xbarOffsetKhz = release ? 0 : requested.xbarOffsetKhz;
        }
        if (requested.hasXbarMsvddOffsetUv) {
            owned.hasXbarMsvddOffsetUv = !release;
            owned.xbarMsvddOffsetUv = release ? 0 : requested.xbarMsvddOffsetUv;
        }
        if (requested.hasSysClkOffsetKhz) {
            owned.hasSysClkOffsetKhz = !release;
            owned.sysClkOffsetKhz = release ? 0 : requested.sysClkOffsetKhz;
        }
        if (requested.hasVideoClkOffsetKhz) {
            owned.hasVideoClkOffsetKhz = !release;
            owned.videoClkOffsetKhz = release ? 0 : requested.videoClkOffsetKhz;
        }
        if (requested.applicationScope & DESIRED_SCOPE_REPLACE_VF_POINTS)
            for (int ci = 0; ci < VF_NUM_POINTS; ++ci) desired_curve_point_clear(&owned, ci);
        for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
            if (!desired_curve_point_present(&requested, ci)) continue;
            if (release) desired_curve_point_clear(&owned, ci);
            else desired_curve_point_copy(&owned, &requested, ci);
        }
        if (requested.hasLock) {
            if (release || requested.lockMode == LOCK_MODE_NONE) {
                owned.hasLock = false; owned.lockCi = -1; owned.lockMHz = 0;
                owned.lockMode = LOCK_MODE_NONE; owned.lockTracksAnchor = false;
            } else service_scoped_copy_lock(&owned, requested);
        }
    }
    if (owned.hasLock && owned.lockMode == LOCK_MODE_NONE) {
        owned.hasLock = false; owned.lockCi = -1; owned.lockMHz = 0; owned.lockTracksAnchor = false;
    }
    if (!desired_curve_intent_valid(&owned))
        return fail("Requested points conflict with the retained lock; release or edit the lock explicitly");
    result.hardware = requested;
    DesiredSettings& hardware = result.hardware;
    hardware.resetOcBeforeApply = false;
    // Scalar cleanup is intentionally independent of the ownership declaration.
    if ((old.hasGpuOffset && !owned.hasGpuOffset) || (release && requested.hasGpuOffset)) {
        hardware.hasGpuOffset = true; hardware.gpuOffsetMHz = 0; hardware.gpuOffsetExcludeLowCount = 0;
    }
    if ((old.hasMemOffset && !owned.hasMemOffset) || (release && requested.hasMemOffset)) {
        hardware.hasMemOffset = true; hardware.memOffsetMHz = 0;
    }
    if ((old.hasPowerLimit && !owned.hasPowerLimit) || (release && requested.hasPowerLimit)) {
        hardware.hasPowerLimit = true; hardware.powerLimitPct = 100;
    }
    if ((old.hasFan && !owned.hasFan) || (release && requested.hasFan)) {
        hardware.hasFan = true; hardware.fanAuto = true; hardware.fanMode = FAN_MODE_AUTO;
        hardware.fanPercent = 0; hardware.fanCurve = {};
    }
    if ((old.hasXbarOffsetKhz && !owned.hasXbarOffsetKhz) || (release && requested.hasXbarOffsetKhz)) {
        hardware.hasXbarOffsetKhz = true; hardware.xbarOffsetKhz = 0;
    }
    if ((old.hasXbarMsvddOffsetUv && !owned.hasXbarMsvddOffsetUv) || (release && requested.hasXbarMsvddOffsetUv)) {
        hardware.hasXbarMsvddOffsetUv = true; hardware.xbarMsvddOffsetUv = 0;
    }
    if ((old.hasSysClkOffsetKhz && !owned.hasSysClkOffsetKhz) || (release && requested.hasSysClkOffsetKhz)) {
        hardware.hasSysClkOffsetKhz = true; hardware.sysClkOffsetKhz = 0;
    }
    if ((old.hasVideoClkOffsetKhz && !owned.hasVideoClkOffsetKhz) || (release && requested.hasVideoClkOffsetKhz)) {
        hardware.hasVideoClkOffsetKhz = true; hardware.videoClkOffsetKhz = 0;
    }
    const bool oldGpuInVf = service_scoped_graphics_offset_in_vf(old, uniformGpuOffsetInVf, composeGpuWithCurve);
    result.graphicsOffsetViaVf = service_scoped_graphics_offset_in_vf(owned,
        uniformGpuOffsetInVf, composeGpuWithCurve);
    if (!replace && !requested.hasGpuOffset && old.hasGpuOffset)
        result.graphicsOffsetViaVf = oldGpuInVf; // preserve an omitted independent scalar surface
    const bool graphWork = service_scoped_graphics_work(&old, requested,
        uniformGpuOffsetInVf, composeGpuWithCurve);
    result.releasesCurveCeiling = graphWork && old.hasLock && old.lockMode == LOCK_MODE_FLATTEN &&
        (!owned.hasLock || owned.lockMode == LOCK_MODE_NONE);
    if (graphWork) {
        // Reapply retained point/lock intent so a dependent graphics write
        // cannot overwrite it. Scalar domains outside that dependency stay omitted.
        for (int ci = 0; ci < VF_NUM_POINTS; ++ci)
            desired_curve_point_copy(&hardware, &owned, ci);
        service_scoped_copy_lock(&hardware, owned);
        // Compiled lock frequency is authoritative. A drifted live anchor
        // must not silently retarget a retained pin or flatten.
        hardware.lockTracksAnchor = false;
        result.retainedLock = !requested.hasLock && owned.hasLock;
        if (result.graphicsOffsetViaVf && owned.hasGpuOffset) {
            hardware.hasGpuOffset = true; hardware.gpuOffsetMHz = owned.gpuOffsetMHz;
            hardware.gpuOffsetExcludeLowCount = owned.gpuOffsetExcludeLowCount;
        }
        if ((old.hasLock && !owned.hasLock) || (release && requested.hasLock)) {
            hardware.hasLock = true; hardware.lockCi = -1; hardware.lockMHz = 0;
            hardware.lockMode = LOCK_MODE_NONE; hardware.lockTracksAnchor = false;
        }
        if (!curve || !floor.valid) return fail("Fresh graphics topology is required for scoped curve work");
        int ordinal = 0;
        for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
            const int pointOrdinal = ordinal;
            if (ci < floor.graphicsEnd && curve[ci].freq_kHz) ++ordinal;
            const bool oldOwned = service_scoped_point_owned(old, ci, pointOrdinal, oldGpuInVf);
            const bool newOwned = service_scoped_point_owned(owned, ci, pointOrdinal, result.graphicsOffsetViaVf);
            const bool explicitPointRelease = release && desired_curve_point_present(&requested, ci);
            if ((!oldOwned || newOwned) && !explicitPointRelease) continue;
            const bool directOldPoint = desired_curve_point_present(&old, ci);
            if (vf_point_target_status(floor, curve, ci) != VF_POINT_TARGET_OK) {
                if (directOldPoint || explicitPointRelease)
                    return fail("A released VF point is no longer writable; refresh and review the topology");
                continue; // a global component or tail never owns foreign/floor entries
            }
            // The new lock owns its tail; do not create contradictory captured tags.
            if (owned.hasLock && owned.lockMode != LOCK_MODE_NONE && ci >= owned.lockCi) continue;
            desired_curve_point_clear(&hardware, ci);
            hardware.curvePointCaptured[ci] = 1;
            ++result.cleanupPoints;
        }
    }
    if (!desired_curve_intent_valid(&hardware)) return fail("Scoped hardware transition has contradictory intent");
    *out = result;
    return true;
}
#endif
