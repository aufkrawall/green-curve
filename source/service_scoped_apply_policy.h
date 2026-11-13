// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_SERVICE_SCOPED_APPLY_POLICY_H
#define GREEN_CURVE_SERVICE_SCOPED_APPLY_POLICY_H

#include "vf_writable_point_policy.h"
#include "desired_topology_binding.h"
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
    int remappedPoints;
};

static inline void service_upgrade_legacy_scalar_scope(const DesiredSettings* previous,
    DesiredSettings* requested, bool replacement = false) {
    if (!previous || !requested || requested->applicationScope != DESIRED_SCOPE_LEGACY ||
        (!previous->memOffsetPrecise && !previous->powerLimitRatio && desired_authoring_is_zero(previous->authoring))) return;
    requested->applicationScope = replacement || requested->resetOcBeforeApply
        ? DESIRED_SCOPE_REPLACE_PROFILE : DESIRED_SCOPE_UPDATE_INCLUDED;
    requested->resetOcBeforeApply = false;
}

static inline bool service_bind_scoped_memory_surface(ServiceScopedApplyPlan* p,
    gc_u32 surface, char* why, size_t whySize) {
    if (!p || !p->hardware.memOffsetPrecise) return p != nullptr;
    if (surface < CLOCK_OFFSET_SURFACE_P0 || surface > CLOCK_OFFSET_SURFACE_GLOBAL ||
        (p->hardware.memOffsetSurface && p->hardware.memOffsetSurface != surface)) {
        if (why && whySize) snprintf(why, whySize, "Exact memory control surface changed or is unavailable");
        return false;
    }
    p->hardware.memOffsetSurface = surface;
    if (p->owned.hasMemOffset && p->owned.memOffsetPrecise && !p->owned.memOffsetSurface)
        p->owned.memOffsetSurface = surface;
    return true;
}

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
    desired_lock_precision_copy(out, in);
}

static inline bool service_scoped_point_owned(const DesiredSettings& d, int ci,
    int ordinal, bool gpuOffsetInVf) {
    return desired_curve_point_present(&d, ci) ||
        (d.hasLock && d.lockMode != LOCK_MODE_NONE && d.lockCi >= 0 && ci >= d.lockCi) ||
        (gpuOffsetInVf && d.hasGpuOffset && (d.authoring.identityExclusions ?
            !d.authoring.graphicsExcluded[ci] : ordinal >= d.gpuOffsetExcludeLowCount));
}

static inline bool service_scoped_graphics_offset_in_vf(const DesiredSettings& d,
    bool uniformViaVf, bool composeWithCurve) {
    if (d.authoring.graphicsPrecise && d.authoring.graphicsSurface != DESIRED_GRAPHICS_AUTO)
        return d.hasGpuOffset && d.authoring.graphicsSurface == DESIRED_GRAPHICS_VF_COMPONENT;
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

static inline bool service_build_scoped_apply_plan_bound(const DesiredSettings* previous,
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
    if (!desired_curve_intent_valid(&requested) || !desired_scalar_intent_valid(&requested) ||
        !desired_authoring_valid(&requested)) return fail("Contradictory curve intent");
    const gc_u32 scope = desired_application_scope_kind(requested.applicationScope);
    const bool release = scope == DESIRED_SCOPE_EXPLICIT_RELEASE;
    const bool replace = scope == DESIRED_SCOPE_REPLACE_PROFILE;
    ServiceScopedApplyPlan result{};
    DesiredSettings old{};
    old.lockCi = -1;
    if (previous) old = *previous;
    result.owned = replace ? requested : old;
    DesiredSettings& owned = result.owned;
    if (!replace && requested.authoring.topologyBound) {
        if (old.authoring.topologyBound && memcmp(old.authoring.voltageUv, requested.authoring.voltageUv,
                sizeof(old.authoring.voltageUv))) return fail("Retained intent needs topology remapping before merge");
        desired_topology_copy(&owned, requested);
    }
    // A release is an operation, never an instruction to release remaining
    // ownership again during automatic replay.
    owned.applicationScope = release ? (gc_u32)DESIRED_SCOPE_UPDATE_INCLUDED : requested.applicationScope;
    owned.resetOcBeforeApply = false;
    if (!replace) {
        if (requested.hasGpuOffset) {
            owned.hasGpuOffset = !release;
            owned.gpuOffsetMHz = release ? 0 : requested.gpuOffsetMHz;
            owned.gpuOffsetExcludeLowCount = release ? 0 : requested.gpuOffsetExcludeLowCount;
            desired_graphics_precision_copy(&owned, requested);
            if (release) desired_graphics_precision_clear(&owned);
        }
        if (requested.hasMemOffset) {
            desired_memory_intent_copy(&owned, &requested);
            owned.hasMemOffset = !release;
            if (release) { owned.memOffsetMHz = 0; desired_memory_precision_clear(&owned); }
        }
        if (requested.hasPowerLimit) {
            desired_power_intent_copy(&owned, &requested);
            owned.hasPowerLimit = !release;
            if (release) { owned.powerLimitPct = 100; desired_power_precision_clear(&owned); }
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
                desired_lock_precision_clear(&owned);
            } else service_scoped_copy_lock(&owned, requested);
        }
    }
    if (owned.hasLock && owned.lockMode == LOCK_MODE_NONE) {
        owned.hasLock = false; owned.lockCi = -1; owned.lockMHz = 0; owned.lockTracksAnchor = false;
        desired_lock_precision_clear(&owned);
    }
    if (!desired_curve_intent_valid(&owned))
        return fail("Requested points conflict with the retained lock; release or edit the lock explicitly");
    if (!desired_authoring_valid(&owned)) return fail("Retained intent requires a complete destination topology binding");
    result.hardware = requested;
    DesiredSettings& hardware = result.hardware;
    hardware.resetOcBeforeApply = false;
    // Scalar cleanup is intentionally independent of the ownership declaration.
    if ((old.hasGpuOffset && !owned.hasGpuOffset) || (release && requested.hasGpuOffset)) {
        hardware.hasGpuOffset = true; hardware.gpuOffsetMHz = 0; hardware.gpuOffsetExcludeLowCount = 0;
        desired_graphics_precision_copy(&hardware, old);
        hardware.authoring.graphicsOffsetHz = 0;
        if (old.authoring.topologyBound) desired_topology_copy(&hardware, old);
    }
    if ((old.hasMemOffset && !owned.hasMemOffset) || (release && requested.hasMemOffset)) {
        hardware.hasMemOffset = true; hardware.memOffsetMHz = 0; desired_memory_precision_clear(&hardware);
    }
    if ((old.hasPowerLimit && !owned.hasPowerLimit) || (release && requested.hasPowerLimit)) {
        hardware.hasPowerLimit = true; hardware.powerLimitPct = 100; desired_power_precision_clear(&hardware);
    }
    // Explicit scopes always resolve scalar intent in exact units. Cleanup
    // uses the outgoing memory surface, including owned zero; previews cannot
    // select another API or hide a difference below one displayed percent.
    if (hardware.hasMemOffset) {
        const gc_u32 oldSurface = old.hasMemOffset && old.memOffsetPrecise ? old.memOffsetSurface : 0;
        const gc_u32 cleanupSurface = oldSurface ? oldSurface :
            (requested.memOffsetPrecise ? requested.memOffsetSurface : 0);
        if (!release && requested.memOffsetPrecise && oldSurface && requested.memOffsetSurface &&
            requested.memOffsetSurface != oldSurface)
            return fail("Release the previous memory control surface before changing it");
        if (!hardware.memOffsetPrecise && !desired_memory_set_precise(&hardware,
                (long long)hardware.memOffsetMHz * 1000000, cleanupSurface))
            return fail("Memory intent requires explicit unit conversion or correction");
        if (!hardware.memOffsetSurface && oldSurface) hardware.memOffsetSurface = oldSurface;
        if (owned.hasMemOffset && requested.hasMemOffset) desired_memory_intent_copy(&owned, &hardware);
    }
    if (hardware.hasPowerLimit) {
        if (!hardware.powerLimitRatio && !desired_power_set_ratio(&hardware,
                tuning_power_ratio_from_percent(hardware.powerLimitPct)))
            return fail("Power intent requires a valid ratio");
        if (owned.hasPowerLimit && requested.hasPowerLimit) desired_power_intent_copy(&owned, &hardware);
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
        if (owned.authoring.topologyBound) desired_topology_copy(&hardware, owned);
        else if (old.authoring.topologyBound) desired_topology_copy(&hardware, old);
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
            desired_graphics_precision_copy(&hardware, owned);
        }
        if ((old.hasLock && !owned.hasLock) || (release && requested.hasLock)) {
            hardware.hasLock = true; hardware.lockCi = -1; hardware.lockMHz = 0;
            hardware.lockMode = LOCK_MODE_NONE; hardware.lockTracksAnchor = false;
            desired_lock_precision_clear(&hardware);
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
    if (!desired_curve_intent_valid(&hardware) || !desired_authoring_valid(&hardware))
        return fail("Scoped hardware transition has contradictory intent");
    *out = result;
    return true;
}

static inline bool service_build_scoped_apply_plan(const DesiredSettings* previous,
    const DesiredSettings& requested, const VFCurvePoint* curve,
    const VfFloorTopology& floor, bool uniformGpuOffsetInVf,
    ServiceScopedApplyPlan* out, char* why, size_t whySize, bool composeGpuWithCurve = false) {
    const bool graphWork = service_scoped_graphics_work(previous, requested,
        uniformGpuOffsetInVf, composeGpuWithCurve);
    if (!graphWork || (!requested.authoring.topologyBound && (!previous || !previous->authoring.topologyBound)))
        return service_build_scoped_apply_plan_bound(previous, requested, curve, floor,
            uniformGpuOffsetInVf, out, why, whySize, composeGpuWithCurve);
    DesiredSettings reboundRequest{}, reboundPrevious{};
    int movedRequest = 0, movedPrevious = 0;
    if (!desired_rebind_topology(requested, curve, floor, &reboundRequest, why, whySize, &movedRequest)) return false;
    if (previous && !desired_rebind_topology(*previous, curve, floor, &reboundPrevious, why, whySize, &movedPrevious)) return false;
    ServiceScopedApplyPlan result{};
    if (!service_build_scoped_apply_plan_bound(previous ? &reboundPrevious : nullptr,
            reboundRequest, curve, floor, uniformGpuOffsetInVf, &result, why, whySize, composeGpuWithCurve)) return false;
    result.remappedPoints = movedRequest + movedPrevious;
    if (!out) return false;
    *out = result;
    return true;
}
#endif
