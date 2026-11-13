// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_CONVERSION_BATCH_H
#define GREEN_CURVE_PROFILE_CONVERSION_BATCH_H
#include "profile_mapping.h"
#include "profile_conversion_grid.h"

enum ProfileConversionKind {
    PROFILE_CONVERT_VF_OFFSETS, PROFILE_CONVERT_VF_FREQUENCIES,
    PROFILE_CONVERT_GRAPHICS, PROFILE_CONVERT_MEMORY, PROFILE_CONVERT_XBAR_CLOCK,
    PROFILE_CONVERT_SYSTEM_CLOCK, PROFILE_CONVERT_VIDEO_CLOCK, PROFILE_CONVERT_POWER,
    PROFILE_CONVERT_XBAR_VOLTAGE, PROFILE_CONVERT_LOCK_FREQUENCY
};
struct ProfileConversionSurface {
    ProfileConversionKind kind = PROFILE_CONVERT_VF_OFFSETS;
    gc_u64 identity = 0;
    gc_u64 gpuGeneration = 0, destinationRevision = 0;
    ProfileConversionGrid grid;
    long long powerDefaultMw = 0;
};
struct ProfileConversionChange {
    int topologyId = -1;
    long long voltageUv = 0, original = 0, lower = 0;
};
struct ProfileConversionBatch {
    ProfileDocument source{};
    ProfileReviewBinding binding;
    ProfileConversionSurface surface;
    int affectedCount = 0, firstTopologyId = -1, worstTopologyId = -1;
    long long firstVoltageUv = 0, worstVoltageUv = 0;
    unsigned long long maximumError = 0;
    ProfileConversionChange changes[VF_NUM_POINTS]{};
    TuningPowerRatio derivedPowerRatio{};
};

static inline bool profile_conversion_surface_equal(const ProfileConversionSurface& a,
    const ProfileConversionSurface& b) {
    return a.kind == b.kind && a.identity == b.identity && a.gpuGeneration == b.gpuGeneration &&
        a.destinationRevision == b.destinationRevision && a.grid.known == b.grid.known &&
        a.grid.step == b.grid.step && a.grid.origin == b.grid.origin && a.powerDefaultMw == b.powerDefaultMw;
}

static inline bool profile_conversion_apply_changes(const ProfileConversionBatch& batch,
    ProfileDocument& document) {
    if (batch.affectedCount < 0 || batch.affectedCount > VF_NUM_POINTS) return false;
    for (int i = 0; i < batch.affectedCount; ++i) {
        const auto& change = batch.changes[i];
        switch (batch.surface.kind) {
            case PROFILE_CONVERT_VF_OFFSETS:
            case PROFILE_CONVERT_VF_FREQUENCIES: {
                const int at = profile_topology_index(document, change.topologyId);
                if (at < 0) return false;
                if (batch.surface.kind == PROFILE_CONVERT_VF_OFFSETS) document.points[at].capturedOffsetHz = change.lower;
                else document.points[at].absoluteFrequencyHz = change.lower;
                break;
            }
            case PROFILE_CONVERT_GRAPHICS: document.graphicsOffset.offsetHz = change.lower; break;
            case PROFILE_CONVERT_MEMORY: document.memoryOffsetHz = change.lower; break;
            case PROFILE_CONVERT_XBAR_CLOCK: document.xbarOffsetHz = change.lower; break;
            case PROFILE_CONVERT_SYSTEM_CLOCK: document.sysClkOffsetHz = change.lower; break;
            case PROFILE_CONVERT_VIDEO_CLOCK: document.videoClkOffsetHz = change.lower; break;
            case PROFILE_CONVERT_LOCK_FREQUENCY: document.lock.targetFrequencyHz = change.lower; break;
            case PROFILE_CONVERT_POWER: document.power.ratio = batch.derivedPowerRatio; break;
            default: return false;
        }
    }
    if (batch.affectedCount) document.source.captureComplete = false;
    return true;
}

// The validator must run the complete destination planner. A failed proposal
// never publishes a partial batch. Each surface gets one count/error summary.
template <class Validate>
static inline bool profile_conversion_prepare(const ProfileDocument& source, gc_u64 sourceRevision,
    ProfileConversionSurface surface, Validate validate, ProfileConversionBatch& output,
    char* why, size_t whySize) {
    const auto fail = [&](const char* reason) { if (why && whySize) snprintf(why, whySize, "%s", reason); return false; };
    if (!profile_document_structurally_valid(source, why, whySize)) return false;
    if (!sourceRevision || !surface.identity || !surface.gpuGeneration || !surface.destinationRevision ||
        !surface.grid.known || surface.grid.step <= 0) return fail("conversion grid or destination is unresolved");
    if (surface.kind < PROFILE_CONVERT_VF_OFFSETS || surface.kind > PROFILE_CONVERT_LOCK_FREQUENCY)
        return fail("unknown conversion surface");
    if (surface.kind == PROFILE_CONVERT_XBAR_VOLTAGE) return fail("voltage conversion requires separate review; no safe rounding direction is established");
    ProfileConversionBatch batch;
    batch.source = source; batch.surface = surface;
    batch.binding = {sourceRevision, surface.gpuGeneration, surface.destinationRevision};
    const auto add = [&](int at, long long original, long long lower, unsigned long long error) {
        if (!error) return;
        const int id = at >= 0 ? source.topology[at].topologyId : -1;
        const long long uv = at >= 0 ? source.topology[at].voltageUv : 0;
        batch.changes[batch.affectedCount++] = {id, uv, original, lower};
        if (batch.firstTopologyId < 0 || uv < batch.firstVoltageUv) { batch.firstTopologyId = id; batch.firstVoltageUv = uv; }
        if (error > batch.maximumError || (error == batch.maximumError && uv < batch.worstVoltageUv)) {
            batch.maximumError = error; batch.worstTopologyId = id; batch.worstVoltageUv = uv;
        }
    };
    const auto clock = [&](int at, long long value, bool positive) {
        const auto lower = profile_grid_lower(value, surface.grid);
        if (!lower.valid || (positive && lower.lower <= 0)) return false;
        if (!lower.exact && at >= 0 && source.legacyIndicesUnresolved) return false;
        add(at, value, lower.lower, lower.error); return true;
    };
    bool valid = true;
    switch (surface.kind) {
        case PROFILE_CONVERT_VF_OFFSETS:
        case PROFILE_CONVERT_VF_FREQUENCIES:
            for (int i = 0; i < source.topologyCount; ++i) {
                const auto& p = source.points[i];
                if (surface.kind == PROFILE_CONVERT_VF_OFFSETS && p.kind == PROFILE_POINT_CAPTURED_OFFSET)
                    valid &= clock(i, p.capturedOffsetHz, false);
                if (surface.kind == PROFILE_CONVERT_VF_FREQUENCIES && p.kind == PROFILE_POINT_ABSOLUTE_FREQUENCY)
                    valid &= clock(i, p.absoluteFrequencyHz, true);
            }
            break;
        case PROFILE_CONVERT_GRAPHICS: if (source.graphicsOffset.present) valid = clock(-1, source.graphicsOffset.offsetHz, false); break;
        case PROFILE_CONVERT_MEMORY: if (source.hasMemoryOffset) valid = clock(-1, source.memoryOffsetHz, false); break;
        case PROFILE_CONVERT_XBAR_CLOCK: if (source.hasXbarOffsetHz) valid = clock(-1, source.xbarOffsetHz, false); break;
        case PROFILE_CONVERT_SYSTEM_CLOCK: if (source.hasSysClkOffsetHz) valid = clock(-1, source.sysClkOffsetHz, false); break;
        case PROFILE_CONVERT_VIDEO_CLOCK: if (source.hasVideoClkOffsetHz) valid = clock(-1, source.videoClkOffsetHz, false); break;
        case PROFILE_CONVERT_LOCK_FREQUENCY: if (source.lock.present) valid = clock(-1, source.lock.targetFrequencyHz, true); break;
        case PROFILE_CONVERT_POWER:
            if (source.power.present) {
                const auto lower = profile_power_lower(source.power.ratio, surface.powerDefaultMw, surface.grid);
                valid = lower.valid;
                if (valid) {
                    batch.derivedPowerRatio = lower.derivedRatio;
                    add(-1, tuning_power_ratio_resolve(source.power.ratio, surface.powerDefaultMw).requestedMw,
                        lower.lowerMw, lower.maximumErrorMw);
                }
            }
            break;
        default: valid = false; break;
    }
    if (!valid) return fail("conversion is unrepresentable or needs reviewed topology identities");
    if (batch.affectedCount) {
        ProfileDocument derived = source;
        if (!profile_conversion_apply_changes(batch, derived) || !profile_document_structurally_valid(derived, why, whySize) ||
            !validate(derived, why, whySize)) return false;
    }
    output = batch;
    return true;
}

template <class Validate>
static inline bool profile_conversion_accept(const ProfileConversionBatch& batch, const ProfileDocument& current,
    gc_u64 sourceRevision, const ProfileConversionSurface& freshSurface, Validate validate,
    ProfileDocument& derived, char* why, size_t whySize) {
    if (!profile_document_structurally_valid(batch.source, why, whySize) ||
        !profile_document_structurally_valid(current, why, whySize)) return false;
    if (!sourceRevision || !batch.binding.sourceRevision || !batch.surface.identity || !batch.surface.grid.known ||
        !batch.surface.gpuGeneration || !batch.surface.destinationRevision ||
        sourceRevision != batch.binding.sourceRevision || !profile_conversion_surface_equal(batch.surface, freshSurface) ||
        !profile_document_semantically_equal(batch.source, current)) {
        if (why && whySize) snprintf(why, whySize, "%s", "source values or destination conversion changed; review it again");
        return false;
    }
    ProfileDocument next = current;
    if (!profile_conversion_apply_changes(batch, next) || !profile_document_structurally_valid(next, why, whySize) ||
        !validate(next, why, whySize)) return false;
    derived = next;
    return true;
}

enum ProfileConversionDecision { PROFILE_CONVERSION_PREVIEW, PROFILE_CONVERSION_ACCEPTED, PROFILE_CONVERSION_REJECTED };
static inline void profile_conversion_summary(const ProfileConversionBatch& batch, ProfileConversionDecision decision,
    char* output, size_t size) {
    if (!output || !size) return;
    const char* domains[] = {"vf-offsets", "vf-frequencies", "graphics", "memory", "xbar-clock", "system-clock",
        "video-clock", "power", "xbar-voltage", "lock-frequency"};
    const int kind = (int)batch.surface.kind;
    const char* domain = kind >= 0 && kind < 10 ? domains[kind] : "unknown";
    const char* verdict = decision == PROFILE_CONVERSION_ACCEPTED ? "accepted" :
        decision == PROFILE_CONVERSION_REJECTED ? "rejected" : "preview";
    snprintf(output, size, "profile-map: domain=%s surface=%llu count=%d max_error=%llu unit=%s decision=%s",
        domain, batch.surface.identity, batch.affectedCount, batch.maximumError,
        batch.surface.kind == PROFILE_CONVERT_POWER ? "mW" : "Hz", verdict);
}
#endif
