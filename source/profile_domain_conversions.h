// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Shared per-domain batch conversion discovery/execution and exact captured-tail
// restore over ProfileDocument + HardwareCaptureObservation (UNIT-07, CAP-10).
// Pure: no globals, no driver calls, never mutates the caller's source draft on
// failure or preview.
#pragma once
#include "profile_conversion_batch.h"
#include "profile_runtime_resolution.h"
#include "profile_control_feasibility.h"
#include <memory>
#include <string>
#include <string_view>

enum { PROFILE_CONVERSION_MAX_CANDIDATES = 10 };

struct ProfileConversionCandidate {
    ProfileConversionKind kind = PROFILE_CONVERT_VF_OFFSETS;
    bool convertible = false;
    bool unresolved = false;
    ProfileConversionSurface surface{};
    ProfileConversionBatch batch{};
    char why[192]{};
};

struct ProfileConversionPlan {
    int count = 0;
    ProfileConversionCandidate items[PROFILE_CONVERSION_MAX_CANDIDATES]{};
    bool requiresDecision() const {
        for (int i = 0; i < count; ++i)
            if (items[i].convertible || items[i].unresolved) return true;
        return false;
    }
    bool anyConvertible() const {
        for (int i = 0; i < count; ++i) if (items[i].convertible) return true;
        return false;
    }
    const ProfileConversionCandidate* find(ProfileConversionKind kind) const {
        for (int i = 0; i < count; ++i) if (items[i].kind == kind) return &items[i];
        return nullptr;
    }
};

static inline const char* profile_conversion_kind_key(ProfileConversionKind kind) {
    switch (kind) {
        case PROFILE_CONVERT_VF_OFFSETS: return "vf-offsets";
        case PROFILE_CONVERT_VF_FREQUENCIES: return "vf-frequencies";
        case PROFILE_CONVERT_GRAPHICS: return "graphics";
        case PROFILE_CONVERT_MEMORY: return "memory";
        case PROFILE_CONVERT_XBAR_CLOCK: return "xbar-clock";
        case PROFILE_CONVERT_SYSTEM_CLOCK: return "system-clock";
        case PROFILE_CONVERT_VIDEO_CLOCK: return "video-clock";
        case PROFILE_CONVERT_POWER: return "power";
        case PROFILE_CONVERT_XBAR_VOLTAGE: return "xbar-voltage";
        case PROFILE_CONVERT_LOCK_FREQUENCY: return "lock-frequency";
    }
    return "unknown";
}

static inline const char* profile_conversion_kind_label(ProfileConversionKind kind) {
    switch (kind) {
        case PROFILE_CONVERT_VF_OFFSETS: return "VF offsets";
        case PROFILE_CONVERT_VF_FREQUENCIES: return "VF targets";
        case PROFILE_CONVERT_GRAPHICS: return "Graphics";
        case PROFILE_CONVERT_MEMORY: return "Memory";
        case PROFILE_CONVERT_XBAR_CLOCK: return "XBAR clock";
        case PROFILE_CONVERT_SYSTEM_CLOCK: return "System clock";
        case PROFILE_CONVERT_VIDEO_CLOCK: return "Video clock";
        case PROFILE_CONVERT_POWER: return "Power limit";
        case PROFILE_CONVERT_XBAR_VOLTAGE: return "XBAR voltage";
        case PROFILE_CONVERT_LOCK_FREQUENCY: return "Lock target";
    }
    return "Unknown";
}

static inline bool profile_conversion_kind_from_key(std::string_view key, ProfileConversionKind& out) {
    for (int k = PROFILE_CONVERT_VF_OFFSETS; k <= PROFILE_CONVERT_LOCK_FREQUENCY; ++k) {
        const auto kind = (ProfileConversionKind)k;
        if (key == profile_conversion_kind_key(kind)) { out = kind; return true; }
    }
    return false;
}

// CAP-10: switch a reviewed flatten/lock draft back to exact captured VF tail
// offsets using stored per-point observations, without triggering recapture.
static inline bool profile_document_can_restore_captured_tail(const ProfileDocument& d,
    char* why = nullptr, size_t whySize = 0) {
    const auto fail = [&](const char* reason) {
        if (why && whySize) snprintf(why, whySize, "%s", reason);
        return false;
    };
    if (!profile_document_structurally_valid(d, why, whySize)) return false;
    if (d.applicationScope == PROFILE_SCOPE_EXPLICIT_RELEASE)
        return fail("explicit release drafts cannot restore a captured tail");
    if (!d.lock.present)
        return fail("source draft has no flattened or lock tail to restore");
    if (d.legacyIndicesUnresolved)
        return fail("resolve legacy point indices before restoring captured tail");
    const int anchorAt = profile_topology_index(d, d.lock.anchorTopologyId);
    if (anchorAt < 0)
        return fail("lock anchor topology point was not found in this document");
    const gc_i64 anchorUv = d.topology[anchorAt].voltageUv;
    int tailCount = 0;
    for (int i = 0; i < d.topologyCount; ++i) {
        if (d.topology[i].sourceWritable == 2 || d.topology[i].voltageUv < anchorUv) continue;
        ++tailCount;
        const auto& p = d.points[i];
        if (!p.hasObservedOffset)
            return fail("captured offset observations are missing on a tail point; exact tail cannot be restored without recapture");
        if (p.kind != PROFILE_POINT_ABSENT && p.kind != PROFILE_POINT_CAPTURED_OFFSET)
            return fail("tail point has an explicit override; clear override before restoring exact captured tail");
        if (p.kind == PROFILE_POINT_CAPTURED_OFFSET && p.capturedOffsetHz != p.observedOffsetHz)
            return fail("tail point already holds a modified captured offset");
    }
    return tailCount > 0 ? true : fail("no writable tail points are covered by this lock");
}

static inline bool profile_document_restore_captured_tail(const ProfileDocument& source,
    ProfileDocument& restored, char* why = nullptr, size_t whySize = 0) {
    if (!profile_document_can_restore_captured_tail(source, why, whySize)) return false;
    const int anchorAt = profile_topology_index(source, source.lock.anchorTopologyId);
    const gc_i64 anchorUv = source.topology[anchorAt].voltageUv;
    auto next = std::make_unique<ProfileDocument>(source);
    for (int i = 0; i < next->topologyCount; ++i) {
        if (next->topology[i].sourceWritable == 2 || next->topology[i].voltageUv < anchorUv) continue;
        next->points[i].kind = PROFILE_POINT_CAPTURED_OFFSET;
        next->points[i].capturedOffsetHz = next->points[i].observedOffsetHz;
        next->points[i].absoluteFrequencyHz = 0;
    }
    next->lock = {};
    next->domains &= ~PROFILE_DOMAIN_LOCK;
    next->domains |= PROFILE_DOMAIN_VF_CURVE;
    if (!profile_document_structurally_valid(*next, why, whySize)) return false;
    restored = *next;
    return true;
}

namespace profile_conversion_detail {

static inline gc_i64 safe_lcm(gc_i64 a, gc_i64 b) {
    if (a <= 0 || b <= 0) return 0;
    gc_i64 x = a, y = b;
    while (y) { const gc_i64 t = x % y; x = y; y = t; }
    const gc_i64 q = a / x;
    return q > LLONG_MAX / b ? 0 : q * b;
}

static inline bool capture_point_base_hz(const HardwareCaptureObservation& capture, int ci, gc_i64& baseHz) {
    for (gc_u32 i = 0; i < capture.pointCount && i < VF_NUM_POINTS; ++i) {
        if ((int)capture.points[i].index != ci) continue;
        if (!capture.points[i].frequencyValid || !capture.points[i].offsetValid) return false;
        baseHz = capture.points[i].frequencyHz - capture.points[i].offsetHz;
        return true;
    }
    return false;
}

struct SurfaceQuery {
    bool needed = false;
    bool gridKnown = false;
    ProfileConversionSurface surface{};
    char why[192]{};
};

static inline SurfaceQuery query_vf_offsets(const ProfileDocument& source, const HardwareCaptureObservation& capture) {
    SurfaceQuery q;
    bool hasCaptured = false, anyNonZero = false, anyOffGrid = false;
    const bool known = capture.vfLimits.valid && capture.vfLimits.gridValid && capture.vfLimits.gridHz > 0;
    for (int i = 0; i < source.topologyCount; ++i) {
        if (source.points[i].kind != PROFILE_POINT_CAPTURED_OFFSET) continue;
        hasCaptured = true;
        const gc_i64 v = source.points[i].capturedOffsetHz;
        anyNonZero |= v != 0;
        if (known && (v % capture.vfLimits.gridHz) != 0) anyOffGrid = true;
    }
    if (!hasCaptured) return q;
    if (!known) {
        if (capture.vfLimits.valid && anyNonZero) {
            q.needed = true;
            snprintf(q.why, sizeof(q.why), "VF offset grid on destination is unknown; automatic rounding is refused");
        }
        return q;
    }
    if (!anyOffGrid) return q;
    q.needed = q.gridKnown = true;
    q.surface = {PROFILE_CONVERT_VF_OFFSETS, (gc_u64)PROFILE_DOMAIN_VF_CURVE,
        capture.gpuGeneration, capture.revision, {true, capture.vfLimits.gridHz, 0}, 0};
    return q;
}

static inline SurfaceQuery query_vf_frequencies(const ProfileDocument& source,
    const HardwareCaptureObservation& capture, const ProfileMappingReport& mapping) {
    SurfaceQuery q;
    bool hasAbs = false, baseKnownAll = mapping.ready(), sameOrigin = true, anyOffGrid = false;
    gc_i64 commonOrigin = -1;
    const bool known = capture.vfLimits.valid && capture.vfLimits.gridValid && capture.vfLimits.gridHz > 0;
    for (int i = 0; i < source.topologyCount; ++i) {
        if (source.points[i].kind != PROFILE_POINT_ABSOLUTE_FREQUENCY) continue;
        hasAbs = true;
        const gc_i64 v = source.points[i].absoluteFrequencyHz;
        if (!known) continue;
        gc_i64 baseHz = 0;
        const int ci = mapping.ready() ? mapping.destinationByTopology[i] : -1;
        if (ci < 0 || !capture_point_base_hz(capture, ci, baseHz)) {
            baseKnownAll = false;
            anyOffGrid |= (v % capture.vfLimits.gridHz) != 0;
            continue;
        }
        gc_i64 origin = baseHz % capture.vfLimits.gridHz;
        if (origin < 0) origin += capture.vfLimits.gridHz;
        if (commonOrigin < 0) commonOrigin = origin;
        else if (origin != commonOrigin) sameOrigin = false;
        if ((v - baseHz) % capture.vfLimits.gridHz != 0) anyOffGrid = true;
    }
    if (!hasAbs) return q;
    if (!known) {
        if (capture.vfLimits.valid) {
            q.needed = true;
            snprintf(q.why, sizeof(q.why), "VF frequency grid on destination is unknown; automatic rounding is refused");
        }
        return q;
    }
    if (!anyOffGrid) return q;
    q.needed = true;
    if (baseKnownAll && !sameOrigin) {
        snprintf(q.why, sizeof(q.why), "absolute VF points have differing destination baseline grids; edit them manually");
        return q;
    }
    q.gridKnown = true;
    q.surface = {PROFILE_CONVERT_VF_FREQUENCIES, (gc_u64)PROFILE_DOMAIN_VF_CURVE | (1ull << 16),
        capture.gpuGeneration, capture.revision,
        {true, capture.vfLimits.gridHz, baseKnownAll && commonOrigin >= 0 ? commonOrigin : 0}, 0};
    return q;
}

static inline SurfaceQuery query_lock_frequency(const ProfileDocument& source,
    const HardwareCaptureObservation& capture, const ProfileMappingReport& mapping) {
    SurfaceQuery q;
    if (!source.lock.present) return q;
    const bool vfKnown = capture.vfLimits.valid && capture.vfLimits.gridValid && capture.vfLimits.gridHz > 0;
    gc_i64 anchorBase = 0; bool baseKnown = false;
    const int at = profile_topology_index(source, source.lock.anchorTopologyId);
    if (at >= 0 && mapping.ready() && mapping.destinationByTopology[at] >= 0)
        baseKnown = capture_point_base_hz(capture, mapping.destinationByTopology[at], anchorBase);
    const gc_i64 v = source.lock.targetFrequencyHz;
    if (source.lock.mode == LOCK_MODE_HARD) {
        const bool off1M = (v % 1000000LL) != 0;
        const bool offVf = vfKnown && baseKnown && ((v - anchorBase) % capture.vfLimits.gridHz) != 0;
        if (!off1M && !offVf) return q;
        q.needed = true;
        if (capture.vfLimits.valid && !vfKnown) {
            snprintf(q.why, sizeof(q.why), "lock frequency grid on destination is unknown");
            return q;
        }
        if (vfKnown && baseKnown && (anchorBase % capture.vfLimits.gridHz) != 0) {
            snprintf(q.why, sizeof(q.why), "hard pin requires MHz alignment incompatible with anchor baseline grid");
            return q;
        }
        const gc_i64 step = vfKnown ? safe_lcm(1000000LL, capture.vfLimits.gridHz) : 1000000LL;
        if (step <= 0) { snprintf(q.why, sizeof(q.why), "lock grid arithmetic overflow"); return q; }
        q.gridKnown = true;
        q.surface = {PROFILE_CONVERT_LOCK_FREQUENCY, (gc_u64)PROFILE_DOMAIN_LOCK,
            capture.gpuGeneration, capture.revision, {true, step, 0}, 0};
        return q;
    }
    if (!vfKnown) {
        if (capture.vfLimits.valid && (v % 1000LL != 0)) {
            q.needed = true;
            snprintf(q.why, sizeof(q.why), "lock frequency grid on destination is unknown");
        }
        return q;
    }
    if (baseKnown && (v - anchorBase) % capture.vfLimits.gridHz == 0) return q;
    if (!baseKnown && (v % capture.vfLimits.gridHz) == 0) return q;
    q.needed = q.gridKnown = true;
    gc_i64 origin = baseKnown ? (anchorBase % capture.vfLimits.gridHz) : 0;
    if (origin < 0) origin += capture.vfLimits.gridHz;
    q.surface = {PROFILE_CONVERT_LOCK_FREQUENCY, (gc_u64)PROFILE_DOMAIN_LOCK,
        capture.gpuGeneration, capture.revision, {true, capture.vfLimits.gridHz, origin}, 0};
    return q;
}

static inline SurfaceQuery query_graphics(const ProfileDocument& source,
    const HardwareCaptureObservation& capture, ProfileRuntimeRoutes routes) {
    SurfaceQuery q;
    if (!source.graphicsOffset.present) return q;
    const gc_i64 v = source.graphicsOffset.offsetHz;
    const gc_u32 route = routes.graphics ? routes.graphics
        : (capture.composition == CAPTURE_COMPOSITION_INDEPENDENT && capture.graphics.valid ? capture.graphics.surface : 0u);
    if (route == DESIRED_GRAPHICS_VF_COMPONENT) {
        const bool known = capture.vfLimits.valid && capture.vfLimits.gridValid && capture.vfLimits.gridHz > 0;
        if (!known) {
            if (capture.vfLimits.valid && v != 0) {
                q.needed = true; snprintf(q.why, sizeof(q.why), "graphics VF component grid on destination is unknown");
            }
            return q;
        }
        if (v % capture.vfLimits.gridHz == 0) return q;
        q.needed = q.gridKnown = true;
        q.surface = {PROFILE_CONVERT_GRAPHICS, (gc_u64)PROFILE_DOMAIN_GRAPHICS_OFFSET | ((gc_u64)route << 16),
            capture.gpuGeneration, capture.revision, {true, capture.vfLimits.gridHz, 0}, 0};
        return q;
    }
    if (route == CLOCK_OFFSET_SURFACE_P0 || route == CLOCK_OFFSET_SURFACE_GLOBAL) {
        const auto* s = hardware_capture_named_clock(capture.graphics, capture.graphicsAlternate, route);
        if (s && s->valid && s->writable && s->grid > 0) {
            if (v % s->grid == 0) return q;
            q.needed = q.gridKnown = true;
            q.surface = {PROFILE_CONVERT_GRAPHICS, (gc_u64)PROFILE_DOMAIN_GRAPHICS_OFFSET | ((gc_u64)route << 16),
                capture.gpuGeneration, capture.revision, {true, s->grid, 0}, 0};
            return q;
        }
        if ((s && s->valid && s->writable && s->grid <= 0 && v != 0) || (v % 1000) != 0) {
            q.needed = true; snprintf(q.why, sizeof(q.why), "graphics clock grid on destination is unknown");
        }
        return q;
    }
    if (v % 1000 != 0) {
        q.needed = true; snprintf(q.why, sizeof(q.why), "select a graphics control or valid grid before converting");
    }
    return q;
}

static inline SurfaceQuery query_memory(const ProfileDocument& source,
    const HardwareCaptureObservation& capture, ProfileRuntimeRoutes routes) {
    SurfaceQuery q;
    if (!source.hasMemoryOffset) return q;
    const gc_u32 surf = routes.memory ? routes.memory : (source.memorySurface ? source.memorySurface : capture.memory.surface);
    const auto* ms = surf ? hardware_capture_named_clock(capture.memory, capture.memoryAlternate, surf)
        : (capture.memory.valid ? &capture.memory : nullptr);
    if (ms && ms->valid && ms->writable && ms->grid > 0) {
        const gc_i64 step = safe_lcm(500000LL, ms->grid);
        if (step <= 0) { q.needed = true; snprintf(q.why, sizeof(q.why), "memory grid arithmetic overflow"); return q; }
        if (source.memoryOffsetHz % step == 0) return q;
        q.needed = q.gridKnown = true;
        q.surface = {PROFILE_CONVERT_MEMORY,
            (gc_u64)PROFILE_DOMAIN_MEMORY | ((gc_u64)(surf ? surf : ms->surface) << 16),
            capture.gpuGeneration, capture.revision, {true, step, 0}, 0};
        return q;
    }
    if ((ms && ms->valid && ms->writable && ms->grid <= 0 && source.memoryOffsetHz != 0) ||
        (source.memoryOffsetHz % 500000LL != 0)) {
        q.needed = true;
        snprintf(q.why, sizeof(q.why), "memory offset grid on destination is unknown; explicit batch conversion refused");
    }
    return q;
}

static inline SurfaceQuery query_power(const ProfileDocument& source, const HardwareCaptureObservation& capture) {
    SurfaceQuery q;
    if (!source.power.present) return q;
    if (!capture.power.defaultValid || !capture.power.writable || capture.power.defaultMw <= 0) return q;
    const auto ideal = tuning_power_ratio_resolve(source.power.ratio, capture.power.defaultMw);
    if (capture.power.gridMw <= 0) {
        if (!ideal.overflowed && (!ideal.exact || source.power.ratio.numerator != source.power.ratio.denominator)) {
            q.needed = true;
            snprintf(q.why, sizeof(q.why), "power limit grid on destination is unknown; automatic rounding is refused");
        }
        return q;
    }
    if (!ideal.overflowed && ideal.exact && (ideal.resolvedMw % capture.power.gridMw) == 0) return q;
    q.needed = true;
    if (ideal.overflowed) { snprintf(q.why, sizeof(q.why), "power ratio resolution overflowed"); return q; }
    q.gridKnown = true;
    q.surface = {PROFILE_CONVERT_POWER, (gc_u64)PROFILE_DOMAIN_POWER,
        capture.gpuGeneration, capture.revision, {true, capture.power.gridMw, 0}, capture.power.defaultMw};
    return q;
}

static inline SurfaceQuery query_aux_clock(ProfileConversionKind kind, gc_u32 domainBit,
    bool present, gc_i64 value, const HardwareCaptureScalar& s, const HardwareCaptureObservation& capture,
    const char* name) {
    SurfaceQuery q;
    if (!present) return q;
    if (s.valid && s.writable && s.grid > 0) {
        const gc_i64 step = safe_lcm(1000LL, s.grid);
        if (step <= 0) { q.needed = true; snprintf(q.why, sizeof(q.why), "%s grid arithmetic overflow", name); return q; }
        if (value % step == 0) return q;
        q.needed = q.gridKnown = true;
        q.surface = {kind, (gc_u64)domainBit, capture.gpuGeneration, capture.revision, {true, step, 0}, 0};
        return q;
    }
    if ((s.valid && s.writable && s.grid <= 0 && value != 0) || (value % 1000LL != 0)) {
        q.needed = true;
        snprintf(q.why, sizeof(q.why), "%s grid on destination is unknown; explicit batch conversion refused", name);
    }
    return q;
}

static inline SurfaceQuery query_xbar_voltage(const ProfileDocument& source, const HardwareCaptureObservation& capture) {
    SurfaceQuery q;
    if (!source.hasXbarMsvddOffsetUv) return q;
    const auto& s = capture.xbarVoltage;
    if ((s.valid && s.grid > 0 && source.xbarMsvddOffsetUv % s.grid != 0) ||
        (s.valid && s.writable && s.grid <= 0 && source.xbarMsvddOffsetUv != 0)) {
        q.needed = true;
        q.gridKnown = s.grid > 0;
        q.surface = {PROFILE_CONVERT_XBAR_VOLTAGE, (gc_u64)PROFILE_DOMAIN_XBAR | (1ull << 16),
            capture.gpuGeneration, capture.revision, {s.grid > 0, s.grid, 0}, 0};
        if (!q.gridKnown)
            snprintf(q.why, sizeof(q.why), "voltage conversion requires separate review; destination grid is unknown");
    }
    return q;
}

static inline bool validate_domain(ProfileConversionKind kind, const ProfileDocument& derived,
    const HardwareCaptureObservation& capture, const ProfileDestinationTopology& destination,
    gc_u64 sourceRevision, ProfileRuntimeRoutes routes, char* why, size_t whySize) {
    const auto fail = [&](const char* msg) {
        if (why && whySize) snprintf(why, whySize, "%s", msg);
        return false;
    };
    auto scoped = std::make_unique<ProfileDocument>(profile_document_make_empty());
    ProfileRuntimeRoutes domainRoutes{};
    const bool curveDomain = kind == PROFILE_CONVERT_VF_OFFSETS || kind == PROFILE_CONVERT_VF_FREQUENCIES ||
        kind == PROFILE_CONVERT_LOCK_FREQUENCY || kind == PROFILE_CONVERT_GRAPHICS;
    if (curveDomain) {
        *scoped = derived;
        scoped->domains &= (PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_GRAPHICS_OFFSET | PROFILE_DOMAIN_LOCK);
        scoped->hasMemoryOffset = false; scoped->memoryOffsetHz = 0; scoped->memorySurface = 0;
        scoped->power = {}; scoped->fan = {};
        scoped->hasXbarOffsetHz = scoped->hasXbarMsvddOffsetUv = scoped->hasSysClkOffsetHz = scoped->hasVideoClkOffsetHz = false;
        scoped->xbarOffsetHz = scoped->xbarMsvddOffsetUv = scoped->sysClkOffsetHz = scoped->videoClkOffsetHz = 0;
        domainRoutes.graphics = routes.graphics;
    } else {
        scoped->applicationScope = derived.applicationScope;
        if (kind == PROFILE_CONVERT_MEMORY) {
            scoped->domains = PROFILE_DOMAIN_MEMORY; scoped->hasMemoryOffset = derived.hasMemoryOffset;
            scoped->memoryOffsetHz = derived.memoryOffsetHz; scoped->memorySurface = derived.memorySurface;
            domainRoutes.memory = routes.memory;
        } else if (kind == PROFILE_CONVERT_POWER) {
            scoped->domains = PROFILE_DOMAIN_POWER; scoped->power = derived.power;
        } else if (kind == PROFILE_CONVERT_XBAR_CLOCK) {
            scoped->domains = PROFILE_DOMAIN_XBAR; scoped->hasXbarOffsetHz = derived.hasXbarOffsetHz;
            scoped->xbarOffsetHz = derived.xbarOffsetHz;
        } else if (kind == PROFILE_CONVERT_SYSTEM_CLOCK) {
            scoped->domains = PROFILE_DOMAIN_SYS_CLK; scoped->hasSysClkOffsetHz = derived.hasSysClkOffsetHz;
            scoped->sysClkOffsetHz = derived.sysClkOffsetHz;
        } else if (kind == PROFILE_CONVERT_VIDEO_CLOCK) {
            scoped->domains = PROFILE_DOMAIN_VIDEO_CLK; scoped->hasVideoClkOffsetHz = derived.hasVideoClkOffsetHz;
            scoped->videoClkOffsetHz = derived.videoClkOffsetHz;
        } else return fail("voltage or unknown domain cannot be batch converted");
    }
    auto runtime = std::make_unique<DesiredSettings>();
    ProfileMappingReport mapping;
    if (!profile_resolve_runtime(*scoped, destination, sourceRevision, domainRoutes, *runtime, mapping, why, whySize))
        return false;
    const auto plan = profile_control_plan(*runtime, capture);
    if (curveDomain) {
        if (plan.curveChecked && (plan.curve.disposition == CURVE_PLAN_REFUSED || plan.curve.disposition == CURVE_PLAN_UNAVAILABLE)) {
            if (const auto* lead = curve_plan_lead_issue(plan.curve))
                snprintf(why, whySize, "converted curve violates destination constraint: %s at point=%d",
                    curve_plan_reason_name(lead->reason), lead->ci);
            else snprintf(why, whySize, "converted curve is refused or unavailable on destination");
            return false;
        }
        if (kind == PROFILE_CONVERT_GRAPHICS) {
            for (int i = 0; i < plan.issueCount; ++i) {
                const auto& iss = plan.issues[i];
                if (iss.domain == PROFILE_DOMAIN_GRAPHICS_OFFSET && iss.kind != PROFILE_CONTROL_RELATIONSHIP) {
                    snprintf(why, whySize, "converted graphics control failed: %s", profile_control_issue_name(iss.kind));
                    return false;
                }
            }
        }
        return true;
    }
    if (plan.disposition == PROFILE_CONTROL_REFUSED || plan.disposition == PROFILE_CONTROL_UNAVAILABLE) {
        if (plan.issueCount > 0)
            snprintf(why, whySize, "converted control failed: %s", profile_control_issue_name(plan.issues[0].kind));
        else snprintf(why, whySize, "converted control is refused or unavailable on destination");
        return false;
    }
    return true;
}

} // namespace profile_conversion_detail

static inline void profile_conversion_plan_build(const ProfileDocument& source,
    const HardwareCaptureObservation& capture, const ProfileDestinationTopology& destination,
    gc_u64 sourceRevision, ProfileRuntimeRoutes routes, ProfileConversionPlan& output) {
    output = {};
    const auto mapping = profile_map_topology(source, destination, sourceRevision);
    const ProfileConversionKind order[] = {
        PROFILE_CONVERT_VF_OFFSETS, PROFILE_CONVERT_VF_FREQUENCIES, PROFILE_CONVERT_LOCK_FREQUENCY,
        PROFILE_CONVERT_GRAPHICS, PROFILE_CONVERT_MEMORY, PROFILE_CONVERT_POWER,
        PROFILE_CONVERT_XBAR_CLOCK, PROFILE_CONVERT_SYSTEM_CLOCK, PROFILE_CONVERT_VIDEO_CLOCK,
        PROFILE_CONVERT_XBAR_VOLTAGE
    };
    for (const auto kind : order) {
        if (output.count >= PROFILE_CONVERSION_MAX_CANDIDATES) break;
        profile_conversion_detail::SurfaceQuery q{};
        switch (kind) {
            case PROFILE_CONVERT_VF_OFFSETS: q = profile_conversion_detail::query_vf_offsets(source, capture); break;
            case PROFILE_CONVERT_VF_FREQUENCIES: q = profile_conversion_detail::query_vf_frequencies(source, capture, mapping); break;
            case PROFILE_CONVERT_LOCK_FREQUENCY: q = profile_conversion_detail::query_lock_frequency(source, capture, mapping); break;
            case PROFILE_CONVERT_GRAPHICS: q = profile_conversion_detail::query_graphics(source, capture, routes); break;
            case PROFILE_CONVERT_MEMORY: q = profile_conversion_detail::query_memory(source, capture, routes); break;
            case PROFILE_CONVERT_POWER: q = profile_conversion_detail::query_power(source, capture); break;
            case PROFILE_CONVERT_XBAR_CLOCK:
                q = profile_conversion_detail::query_aux_clock(kind, PROFILE_DOMAIN_XBAR,
                    source.hasXbarOffsetHz, source.xbarOffsetHz, capture.xbarClock, capture, "crossbar clock");
                break;
            case PROFILE_CONVERT_SYSTEM_CLOCK:
                q = profile_conversion_detail::query_aux_clock(kind, PROFILE_DOMAIN_SYS_CLK,
                    source.hasSysClkOffsetHz, source.sysClkOffsetHz, capture.systemClock, capture, "system clock");
                break;
            case PROFILE_CONVERT_VIDEO_CLOCK:
                q = profile_conversion_detail::query_aux_clock(kind, PROFILE_DOMAIN_VIDEO_CLK,
                    source.hasVideoClkOffsetHz, source.videoClkOffsetHz, capture.videoClock, capture, "video clock");
                break;
            case PROFILE_CONVERT_XBAR_VOLTAGE: q = profile_conversion_detail::query_xbar_voltage(source, capture); break;
        }
        if (!q.needed) continue;
        ProfileConversionCandidate item{};
        item.kind = kind;
        item.surface = q.surface;
        item.batch.source = source;
        item.batch.surface = q.surface;
        item.batch.binding = {sourceRevision, capture.gpuGeneration, capture.revision};
        if (!q.gridKnown) {
            item.unresolved = true;
            snprintf(item.why, sizeof(item.why), "%s", q.why);
            output.items[output.count++] = item;
            continue;
        }
        const auto validator = [&](const ProfileDocument& derived, char* why, size_t whySize) {
            return profile_conversion_detail::validate_domain(kind, derived, capture, destination,
                sourceRevision, routes, why, whySize);
        };
        if (profile_conversion_prepare(source, sourceRevision, q.surface, validator, item.batch, item.why, sizeof(item.why))) {
            if (item.batch.affectedCount > 0) {
                item.convertible = true;
                output.items[output.count++] = item;
            }
        } else {
            item.unresolved = true;
            output.items[output.count++] = item;
        }
    }
}

static inline bool profile_conversion_accept_item(const ProfileConversionCandidate& item,
    const ProfileDocument& current, gc_u64 sourceRevision, const HardwareCaptureObservation& freshCapture,
    const ProfileDestinationTopology& freshDestination, ProfileRuntimeRoutes routes,
    ProfileDocument& derived, char* summary, size_t summarySize, char* why, size_t whySize) {
    if (!item.convertible) {
        profile_conversion_summary(item.batch, PROFILE_CONVERSION_REJECTED, summary, summarySize);
        if (why && whySize) snprintf(why, whySize, "%s", item.why[0] ? item.why : "conversion candidate is unresolved");
        return false;
    }
    const auto mapping = profile_map_topology(current, freshDestination, sourceRevision);
    profile_conversion_detail::SurfaceQuery freshQuery{};
    switch (item.kind) {
        case PROFILE_CONVERT_VF_OFFSETS: freshQuery = profile_conversion_detail::query_vf_offsets(current, freshCapture); break;
        case PROFILE_CONVERT_VF_FREQUENCIES: freshQuery = profile_conversion_detail::query_vf_frequencies(current, freshCapture, mapping); break;
        case PROFILE_CONVERT_LOCK_FREQUENCY: freshQuery = profile_conversion_detail::query_lock_frequency(current, freshCapture, mapping); break;
        case PROFILE_CONVERT_GRAPHICS: freshQuery = profile_conversion_detail::query_graphics(current, freshCapture, routes); break;
        case PROFILE_CONVERT_MEMORY: freshQuery = profile_conversion_detail::query_memory(current, freshCapture, routes); break;
        case PROFILE_CONVERT_POWER: freshQuery = profile_conversion_detail::query_power(current, freshCapture); break;
        case PROFILE_CONVERT_XBAR_CLOCK:
            freshQuery = profile_conversion_detail::query_aux_clock(item.kind, PROFILE_DOMAIN_XBAR,
                current.hasXbarOffsetHz, current.xbarOffsetHz, freshCapture.xbarClock, freshCapture, "crossbar clock");
            break;
        case PROFILE_CONVERT_SYSTEM_CLOCK:
            freshQuery = profile_conversion_detail::query_aux_clock(item.kind, PROFILE_DOMAIN_SYS_CLK,
                current.hasSysClkOffsetHz, current.sysClkOffsetHz, freshCapture.systemClock, freshCapture, "system clock");
            break;
        case PROFILE_CONVERT_VIDEO_CLOCK:
            freshQuery = profile_conversion_detail::query_aux_clock(item.kind, PROFILE_DOMAIN_VIDEO_CLK,
                current.hasVideoClkOffsetHz, current.videoClkOffsetHz, freshCapture.videoClock, freshCapture, "video clock");
            break;
        case PROFILE_CONVERT_XBAR_VOLTAGE:
            freshQuery = profile_conversion_detail::query_xbar_voltage(current, freshCapture);
            break;
    }
    const ProfileConversionSurface freshSurface = freshQuery.surface;
    const auto validator = [&](const ProfileDocument& next, char* msg, size_t cap) {
        return profile_conversion_detail::validate_domain(item.kind, next, freshCapture, freshDestination,
            sourceRevision, routes, msg, cap);
    };
    const bool accepted = profile_conversion_accept(item.batch, current, sourceRevision, freshSurface, validator,
        derived, why, whySize);
    profile_conversion_summary(item.batch, accepted ? PROFILE_CONVERSION_ACCEPTED : PROFILE_CONVERSION_REJECTED,
        summary, summarySize);
    return accepted;
}
