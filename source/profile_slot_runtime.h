// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_SLOT_RUNTIME_H
#define GREEN_CURVE_PROFILE_SLOT_RUNTIME_H
// A saved slot may hold a portable document (written by import, by older
// capture builds, or by the TUI). Every apply path -- the editor's Load, the
// tray, hotkeys, auto-switch, app start, logon and sharing -- consumes the one
// runtime record, so a document slot is converted here instead of being
// refused. Two kinds exist:
//
// * Position-based records (every export of an ordinary profile and every
//   historical INI) name curve points by index on the GPU they were saved on.
//   That is exactly what an ordinary slot means, so they convert without a GPU
//   by reversing profile_document_from_legacy_desired().
// * Voltage-bound records (captures) name curve points by exact voltage and
//   need the live curve of the GPU they will run on. Mapping is exact: a point
//   without exactly one matching voltage refuses the whole record.
//
// Nothing here grants write authority or proves feasibility; the service
// validates and plans every apply as it does for any other slot.
#include "profile_runtime_resolution.h"
#include "desired_settings_ui_policy.h"
#include "vf_writable_point_policy.h"

// The selected GPU's live curve as mapping evidence: graphics points by exact
// voltage, with fixed floor points marked read-only so a record that would
// change them refuses instead of failing later at Apply.
static inline bool profile_destination_from_vf_curve(const VFCurvePoint* curve, const VfFloorTopology& floor,
    ProfileDestinationTopology& output) {
    output = ProfileDestinationTopology{};
    if (!curve || !floor.valid) return false;
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        if (!vf_point_in_graphics_domain(floor, curve, ci) || !curve[ci].volt_uV) continue;
        output.points[output.count++] = {ci, (long long)curve[ci].volt_uV, true,
            vf_point_target_status(floor, curve, ci) == VF_POINT_TARGET_OK};
    }
    output.available = output.count > 0;
    output.complete = output.available;
    return output.available && profile_destination_topology_valid(output);
}

static inline bool profile_document_requires_destination(const ProfileDocument& d) {
    if (d.legacyIndicesUnresolved) return false;
    if (d.lock.present) return true;
    for (int i = 0; i < d.topologyCount && i < VF_NUM_POINTS; ++i)
        if (d.points[i].kind != PROFILE_POINT_ABSENT) return true;
    return d.graphicsOffset.present && d.graphicsOffset.exclusionPolicy == PROFILE_EXCLUDE_IDENTITIES;
}

static inline bool profile_slot_runtime_fail(char* why, size_t whySize, const char* text) {
    if (why && whySize) snprintf(why, whySize, "%s", text);
    return false;
}

static inline bool profile_power_ratio_is_whole_percent(TuningPowerRatio ratio, int* percent) {
    int preview = 0;
    if (!desired_power_ratio_preview(ratio, &preview)) return false;
    if (percent) *percent = preview;
    return tuning_power_ratio_equal(tuning_power_ratio_reduce(ratio.numerator, ratio.denominator),
        tuning_power_ratio_from_percent(preview));
}

// Anything an ordinary (legacy-scoped) record cannot carry: the service refuses
// precise or captured intent without an explicit application scope.
static inline bool profile_indexed_document_needs_explicit_scope(const ProfileDocument& d) {
    for (int i = 0; i < d.topologyCount && i < VF_NUM_POINTS; ++i) {
        const auto& p = d.points[i];
        if (p.kind == PROFILE_POINT_CAPTURED_OFFSET) return true;
        if (p.kind == PROFILE_POINT_ABSOLUTE_FREQUENCY && p.absoluteFrequencyHz % 1000000) return true;
    }
    if (d.graphicsOffset.present && d.graphicsOffset.offsetHz % 1000000) return true;
    if (d.lock.present && d.lock.targetFrequencyHz % 1000000) return true;
    if (d.hasMemoryOffset && (d.memorySurface || d.memoryOffsetHz % 1000000)) return true;
    if (d.power.present && !profile_power_ratio_is_whole_percent(d.power.ratio, nullptr)) return true;
    return false;
}

static inline int profile_indexed_document_point(const ProfileDocument& d, int topologyId) {
    const int at = profile_topology_index(d, topologyId);
    if (at < 0 || at >= d.topologyCount) return -1;
    const int ci = d.topology[at].sourceIndex;
    return ci >= 0 && ci < VF_NUM_POINTS ? ci : -1;
}

// The exact reverse of profile_document_from_legacy_desired(). Observed
// projection frequencies keep the saved MHz an ordinary slot also stores.
static inline bool profile_desired_from_indexed_document(const ProfileDocument& s,
    DesiredSettings& output, char* why, size_t whySize) {
    const auto fail = [&](const char* text) { return profile_slot_runtime_fail(why, whySize, text); };
    if (!profile_document_structurally_valid(s, why, whySize)) return false;
    if (profile_document_requires_destination(s)) return fail("this profile needs the GPU it will run on");
    auto nextPtr = std::make_unique<DesiredSettings>();
    DesiredSettings& next = *nextPtr;
    initialize_desired_settings_defaults(&next);
    next.lockCi = -1;
    const bool explicitScope = profile_indexed_document_needs_explicit_scope(s);
    if (s.applicationScope == PROFILE_SCOPE_UPDATE_INCLUDED) {
        next.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED;
        if ((s.domains & PROFILE_DOMAIN_VF_CURVE) && s.pointSetPolicy == PROFILE_POINTS_REPLACE)
            next.applicationScope |= DESIRED_SCOPE_REPLACE_VF_POINTS;
    } else if (s.applicationScope == PROFILE_SCOPE_EXPLICIT_RELEASE) {
        next.applicationScope = DESIRED_SCOPE_EXPLICIT_RELEASE;
    } else {
        next.applicationScope = explicitScope ? DESIRED_SCOPE_REPLACE_PROFILE : DESIRED_SCOPE_LEGACY;
    }
    const bool legacyScope = next.applicationScope == DESIRED_SCOPE_LEGACY;
    if (s.graphicsOffset.present) {
        const auto& g = s.graphicsOffset;
        if (g.offsetHz < -1000000000LL || g.offsetHz > 1000000000LL) return fail("graphics offset is out of range");
        if (g.exclusionPolicy == PROFILE_EXCLUDE_IDENTITIES) return fail("this profile needs the GPU it will run on");
        next.hasGpuOffset = 1; next.gpuOffsetMHz = (int)(g.offsetHz / 1000000);
        if (g.exclusionPolicy == PROFILE_EXCLUDE_LEGACY_COUNT) next.gpuOffsetExcludeLowCount = g.excludeLowCount;
        if (g.offsetHz % 1000000) { next.authoring.graphicsPrecise = 1; next.authoring.graphicsOffsetHz = g.offsetHz; }
    }
    bool used[VF_NUM_POINTS]{};
    for (int i = 0; i < s.topologyCount; ++i) {
        const int ci = s.topology[i].sourceIndex;
        if (ci < 0 || ci >= VF_NUM_POINTS || used[ci]) return fail("profile curve point positions are invalid");
        used[ci] = true;
        const auto& p = s.points[i];
        if (p.kind == PROFILE_POINT_ABSENT) continue;
        if (p.kind == PROFILE_POINT_CAPTURED_OFFSET) {
            next.curvePointCaptured[ci] = 1; next.curvePointCapturedOffsetHz[ci] = p.capturedOffsetHz;
        } else if (p.kind == PROFILE_POINT_PROJECTION) {
            if (!next.hasGpuOffset) return fail("a curve point follows a missing graphics offset");
            next.hasCurvePoint[ci] = 1; next.curvePointFromGpuOffset[ci] = 1;
            if (p.hasObservedFrequency && p.observedFrequencyHz > 0 && p.observedFrequencyHz <= 5000000000LL)
                next.curvePointMHz[ci] = (unsigned)(p.observedFrequencyHz / 1000000);
        } else {
            if (p.absoluteFrequencyHz <= 0 || p.absoluteFrequencyHz > 5000000000LL) return fail("a curve point is out of range");
            next.hasCurvePoint[ci] = 1; next.curvePointMHz[ci] = (unsigned)(p.absoluteFrequencyHz / 1000000);
            if (p.absoluteFrequencyHz % 1000000) {
                next.authoring.pointFrequencyPrecise[ci] = 1; next.authoring.pointFrequencyHz[ci] = p.absoluteFrequencyHz;
            }
        }
    }
    if (s.lock.present) {
        const int ci = profile_indexed_document_point(s, s.lock.anchorTopologyId);
        if (ci < 0) return fail("the clock lock names no curve point");
        if (s.lock.targetFrequencyHz <= 0 || s.lock.targetFrequencyHz > 5000000000LL) return fail("the clock lock is out of range");
        next.hasLock = 1; next.lockCi = ci; next.lockMode = s.lock.mode;
        next.lockMHz = (unsigned)(s.lock.targetFrequencyHz / 1000000);
        next.lockTracksAnchor = s.lock.trackAnchor;
        if (s.lock.targetFrequencyHz % 1000000 && s.lock.mode != LOCK_MODE_NONE) {
            next.authoring.lockPrecise = 1; next.authoring.lockFrequencyHz = s.lock.targetFrequencyHz;
        }
    }
    if (s.hasMemoryOffset) {
        if (legacyScope) {
            next.hasMemOffset = 1; next.memOffsetMHz = (int)(s.memoryOffsetHz / 1000000);
        } else if (!desired_memory_set_precise(&next, s.memoryOffsetHz, s.memorySurface)) {
            return fail("memory offset cannot be represented");
        }
    }
    if (s.power.present) {
        int percent = 0;
        if (legacyScope && profile_power_ratio_is_whole_percent(s.power.ratio, &percent)) {
            next.hasPowerLimit = 1; next.powerLimitPct = percent;
        } else if (!desired_power_set_ratio(&next, s.power.ratio)) {
            return fail("power limit cannot be represented");
        }
    }
    if (s.fan.present) {
        next.hasFan = 1; next.fanAuto = s.fan.fanAuto; next.fanMode = s.fan.fanMode; next.fanPercent = s.fan.fanPercent;
        if (s.fan.hasCurveConfig) next.fanCurve = s.fan.fanCurve;
    }
    const auto clock = [](bool present, long long value, gc_bool8& has, int& khz) {
        if (!present) return true;
        if (value % 1000 || value < -1000000000LL || value > 1000000000LL) return false;
        has = 1; khz = (int)(value / 1000); return true;
    };
    if (!clock(s.hasXbarOffsetHz, s.xbarOffsetHz, next.hasXbarOffsetKhz, next.xbarOffsetKhz) ||
        !clock(s.hasSysClkOffsetHz, s.sysClkOffsetHz, next.hasSysClkOffsetKhz, next.sysClkOffsetKhz) ||
        !clock(s.hasVideoClkOffsetHz, s.videoClkOffsetHz, next.hasVideoClkOffsetKhz, next.videoClkOffsetKhz))
        return fail("an advanced clock offset cannot be represented");
    if (s.hasXbarMsvddOffsetUv) {
        if (s.xbarMsvddOffsetUv < -100000 || s.xbarMsvddOffsetUv > 100000) return fail("an advanced voltage offset is out of range");
        next.hasXbarMsvddOffsetUv = 1; next.xbarMsvddOffsetUv = (int)s.xbarMsvddOffsetUv;
    }
    if (next.applicationScope == DESIRED_SCOPE_EXPLICIT_RELEASE) {
        if ((s.domains & PROFILE_DOMAIN_VF_CURVE) || (s.domains & PROFILE_DOMAIN_LOCK))
            return fail("this profile needs the GPU it will run on");
        if ((s.domains & PROFILE_DOMAIN_GRAPHICS_OFFSET) && !next.hasGpuOffset) next.hasGpuOffset = 1;
        if ((s.domains & PROFILE_DOMAIN_MEMORY) && !next.hasMemOffset) next.hasMemOffset = 1;
        if ((s.domains & PROFILE_DOMAIN_POWER) && !next.hasPowerLimit) { next.hasPowerLimit = 1; next.powerLimitPct = POWER_LIMIT_DEFAULT_PCT; }
        if ((s.domains & PROFILE_DOMAIN_FAN) && !next.hasFan) { next.hasFan = 1; next.fanAuto = 1; next.fanMode = FAN_MODE_AUTO; }
        if ((s.domains & PROFILE_DOMAIN_XBAR) && !next.hasXbarOffsetKhz && !next.hasXbarMsvddOffsetUv) {
            next.hasXbarOffsetKhz = 1; next.hasXbarMsvddOffsetUv = 1;
        }
        if ((s.domains & PROFILE_DOMAIN_SYS_CLK) && !next.hasSysClkOffsetKhz) next.hasSysClkOffsetKhz = 1;
        if ((s.domains & PROFILE_DOMAIN_VIDEO_CLK) && !next.hasVideoClkOffsetKhz) next.hasVideoClkOffsetKhz = 1;
    }
    if (!desired_curve_intent_valid(&next) || !desired_scalar_intent_valid(&next) || !desired_authoring_valid(&next))
        return fail("the profile's settings contradict each other");
    output = next;
    return true;
}

// Plain-language reason for a voltage-bound record that does not fit this GPU.
static inline void profile_slot_mapping_reason(const ProfileMappingReport& report, char* why, size_t whySize) {
    if (!why || !whySize) return;
    int missing = 0, readOnly = 0, other = 0;
    for (int i = 0; i < report.issueCount; ++i) {
        const auto kind = report.issues[i].kind;
        if (kind == PROFILE_MAP_MISSING_VOLTAGE || kind == PROFILE_MAP_AMBIGUOUS_VOLTAGE) ++missing;
        else if (kind == PROFILE_MAP_READ_ONLY_POINT) ++readOnly;
        else ++other;
    }
    if (missing)
        snprintf(why, whySize, "this profile was made for a different GPU: %d curve point%s %s no matching voltage here",
            missing, missing == 1 ? "" : "s", missing == 1 ? "has" : "have");
    else if (readOnly)
        snprintf(why, whySize, "this profile changes %d fixed curve point%s this GPU does not allow",
            readOnly, readOnly == 1 ? "" : "s");
    else if (other)
        snprintf(why, whySize, "this profile does not fit this GPU's controls");
}

// Convert any slot document into the runtime record. `live` is the selected
// GPU's current curve topology, or null when none is available.
static inline bool profile_slot_document_runtime(const ProfileDocument& source,
    const ProfileDestinationTopology* live, DesiredSettings& output, char* why, size_t whySize) {
    if (!profile_document_requires_destination(source))
        return profile_desired_from_indexed_document(source, output, why, whySize);
    if (!live || !live->available || !live->count)
        return profile_slot_runtime_fail(why, whySize, "this profile needs the GPU it will run on; connect it and try again");
    ProfileRuntimeRoutes routes;
    if (source.graphicsOffset.present && source.graphicsOffset.exclusionPolicy == PROFILE_EXCLUDE_IDENTITIES)
        routes.graphics = DESIRED_GRAPHICS_VF_COMPONENT;
    auto mapping = std::make_unique<ProfileMappingReport>();
    auto nextPtr = std::make_unique<DesiredSettings>();
    char detail[160]{};
    // The binding values only satisfy the resolver's review-freshness contract;
    // this conversion is used immediately and reviews nothing.
    ProfileDestinationTopology destination = *live;
    if (!destination.gpuGeneration) destination.gpuGeneration = 1;
    if (!destination.revision) destination.revision = 1;
    if (!profile_resolve_runtime(source, destination, 1, routes, *nextPtr, *mapping, detail, sizeof(detail))) {
        if (!mapping->ready() && mapping->issueCount) profile_slot_mapping_reason(*mapping, why, whySize);
        else profile_slot_runtime_fail(why, whySize, detail[0] ? detail : "this profile does not fit this GPU");
        return false;
    }
    if (!nextPtr->hasFan || !source.fan.hasCurveConfig) fan_curve_set_default(&nextPtr->fanCurve);
    if (!nextPtr->hasFan) { nextPtr->fanAuto = 1; nextPtr->fanMode = FAN_MODE_AUTO; }
    if (!nextPtr->hasLock) nextPtr->lockTracksAnchor = true;
    output = *nextPtr;
    return true;
}
#endif
