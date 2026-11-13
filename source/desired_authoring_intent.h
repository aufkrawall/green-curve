// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_DESIRED_AUTHORING_INTENT_H
#define GREEN_CURVE_DESIRED_AUTHORING_INTENT_H
#include <limits.h>
#include <string.h>

template <class D> static inline gc_i64 desired_absolute_hz(const D& d, int i) {
    return d.authoring.pointFrequencyPrecise[i] ? d.authoring.pointFrequencyHz[i] : (gc_i64)d.curvePointMHz[i] * 1000000;
}
template <class D> static inline gc_i64 desired_graphics_hz(const D& d) {
    return d.authoring.graphicsPrecise ? d.authoring.graphicsOffsetHz : (gc_i64)d.gpuOffsetMHz * 1000000;
}
template <class D> static inline gc_i64 desired_lock_hz(const D& d) {
    return d.authoring.lockPrecise ? d.authoring.lockFrequencyHz : (gc_i64)d.lockMHz * 1000000;
}
template <class D> static inline void desired_graphics_precision_clear(D* d) {
    d->authoring.graphicsPrecise = d->authoring.identityExclusions = 0;
    d->authoring.graphicsOffsetHz = 0; d->authoring.graphicsSurface = 0;
    memset(d->authoring.graphicsExcluded, 0, sizeof(d->authoring.graphicsExcluded));
}
template <class D> static inline void desired_lock_precision_clear(D* d) {
    d->authoring.lockPrecise = 0; d->authoring.lockFrequencyHz = 0;
}
template <class D> static inline void desired_graphics_precision_copy(D* out, const D& in) {
    out->authoring.graphicsPrecise = in.authoring.graphicsPrecise;
    out->authoring.graphicsOffsetHz = in.authoring.graphicsOffsetHz;
    out->authoring.graphicsSurface = in.authoring.graphicsSurface;
    out->authoring.identityExclusions = in.authoring.identityExclusions;
    memcpy(out->authoring.graphicsExcluded, in.authoring.graphicsExcluded, sizeof(out->authoring.graphicsExcluded));
}
template <class D> static inline void desired_lock_precision_copy(D* out, const D& in) {
    out->authoring.lockPrecise = in.authoring.lockPrecise;
    out->authoring.lockFrequencyHz = in.authoring.lockFrequencyHz;
}
template <class D> static inline void desired_topology_copy(D* out, const D& in) {
    out->authoring.topologyBound = in.authoring.topologyBound;
    out->authoring.topologyCount = in.authoring.topologyCount;
    memcpy(out->authoring.voltageUv, in.authoring.voltageUv, sizeof(out->authoring.voltageUv));
}
static inline bool desired_authoring_equal(const DesiredAuthoringFields& a, const DesiredAuthoringFields& b) {
    if (a.graphicsPrecise != b.graphicsPrecise || a.lockPrecise != b.lockPrecise ||
        a.identityExclusions != b.identityExclusions || a.topologyBound != b.topologyBound ||
        a.graphicsSurface != b.graphicsSurface || a.graphicsOffsetHz != b.graphicsOffsetHz ||
        a.lockFrequencyHz != b.lockFrequencyHz || a.topologyCount != b.topologyCount || a.reserved != b.reserved) return false;
    for (int i = 0; i < 128; ++i) if (a.pointFrequencyPrecise[i] != b.pointFrequencyPrecise[i] ||
        a.pointFrequencyHz[i] != b.pointFrequencyHz[i] || a.voltageUv[i] != b.voltageUv[i] ||
        a.graphicsExcluded[i] != b.graphicsExcluded[i]) return false;
    return true;
}
static inline bool desired_authoring_is_zero(const DesiredAuthoringFields& a) {
    const DesiredAuthoringFields empty{}; return desired_authoring_equal(a, empty);
}
template <class D> static inline bool desired_authoring_valid(const D* d) {
    if (!d) return false;
    const auto& a = d->authoring;
    if (a.graphicsPrecise > 1 || a.lockPrecise > 1 || a.identityExclusions > 1 || a.topologyBound > 1 ||
        a.graphicsSurface > DESIRED_GRAPHICS_VF_COMPONENT || a.topologyCount > 128 || a.reserved) return false;
    if ((!a.graphicsPrecise && (a.graphicsOffsetHz || a.graphicsSurface || a.identityExclusions)) ||
        (!a.lockPrecise && a.lockFrequencyHz)) return false;
    if (a.graphicsPrecise && (!d->hasGpuOffset || a.graphicsOffsetHz < -1000000000LL ||
        a.graphicsOffsetHz > 1000000000LL || a.graphicsOffsetHz / 1000000 != d->gpuOffsetMHz)) return false;
    if (a.lockPrecise && (!d->hasLock || d->lockMode == LOCK_MODE_NONE || a.lockFrequencyHz <= 0 ||
        a.lockFrequencyHz > 5000000000LL || (unsigned long long)(a.lockFrequencyHz / 1000000) != d->lockMHz)) return false;
    if (a.identityExclusions && (!d->hasGpuOffset || !a.graphicsPrecise || !a.topologyBound ||
        d->gpuOffsetExcludeLowCount || a.graphicsSurface == DESIRED_GRAPHICS_P0 || a.graphicsSurface == DESIRED_GRAPHICS_GLOBAL)) return false;
    gc_u32 populated = 0;
    for (int i = 0; i < 128; ++i) {
        if (a.pointFrequencyPrecise[i] > 1 || a.graphicsExcluded[i] > 1 || a.voltageUv[i] < 0 || a.voltageUv[i] > UINT_MAX) return false;
        if ((!a.pointFrequencyPrecise[i] && a.pointFrequencyHz[i]) ||
            (!a.identityExclusions && a.graphicsExcluded[i])) return false;
        if (a.pointFrequencyPrecise[i] && (!d->hasCurvePoint[i] || d->curvePointFromGpuOffset[i] ||
            d->curvePointCaptured[i] || a.pointFrequencyHz[i] <= 0 || a.pointFrequencyHz[i] > 5000000000LL ||
            (unsigned long long)(a.pointFrequencyHz[i] / 1000000) != d->curvePointMHz[i])) return false;
        if (a.voltageUv[i]) ++populated;
        if (a.topologyBound && ((d->hasCurvePoint[i] || d->curvePointCaptured[i] ||
            (a.identityExclusions && a.graphicsExcluded[i])) && !a.voltageUv[i])) return false;
    }
    if (a.topologyBound) {
        if (populated != a.topologyCount) return false;
        if (d->hasLock && d->lockMode != LOCK_MODE_NONE &&
            (d->lockCi < 0 || d->lockCi >= 128 || !a.voltageUv[d->lockCi])) return false;
    } else if (populated || a.topologyCount) return false;
    return true;
}
template <class D> static inline void desired_authoring_canonicalize(D* d) {
    auto& a = d->authoring;
    canonicalize_gc_bool8(&a.graphicsPrecise); canonicalize_gc_bool8(&a.lockPrecise);
    canonicalize_gc_bool8(&a.identityExclusions); canonicalize_gc_bool8(&a.topologyBound);
    if (!d->hasGpuOffset || !a.graphicsPrecise) desired_graphics_precision_clear(d);
    if (!d->hasLock || d->lockMode == LOCK_MODE_NONE || !a.lockPrecise) desired_lock_precision_clear(d);
    for (int i = 0; i < 128; ++i) {
        canonicalize_gc_bool8(&a.pointFrequencyPrecise[i]); canonicalize_gc_bool8(&a.graphicsExcluded[i]);
        if (!d->hasCurvePoint[i] || d->curvePointFromGpuOffset[i] || d->curvePointCaptured[i]) a.pointFrequencyPrecise[i] = 0;
        if (!a.pointFrequencyPrecise[i]) a.pointFrequencyHz[i] = 0;
        if (!a.identityExclusions) a.graphicsExcluded[i] = 0;
        if (!a.topologyBound) a.voltageUv[i] = 0;
    }
    if (!a.topologyBound) a.topologyCount = 0;
    a.reserved = 0;
}
// Capacity may be retained on disk before a producer is activated. Backends and
// IPC refuse its use until exact adapters and binding review are connected.
template <class D> static inline bool desired_authoring_runtime_ready(const D* d) {
    return d && desired_authoring_is_zero(d->authoring);
}
#endif
