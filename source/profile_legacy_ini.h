// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_slot_codec.h"
#include "profile_config_encoding.h"
#include "fan_zero_rpm_profile_policy.h"
#include <map>

enum ProfileLegacyFormat : int { PROFILE_LEGACY_NONE, PROFILE_LEGACY_WINDOWS_INI, PROFILE_LEGACY_LINUX_INI };
enum ProfileLegacyMemoryUnit : int { PROFILE_LEGACY_MEMORY_UNKNOWN, PROFILE_LEGACY_MEMORY_DISPLAY, PROFILE_LEGACY_MEMORY_EFFECTIVE };
enum ProfileLegacyCurveKind : int { PROFILE_LEGACY_CURVE_UNKNOWN, PROFILE_LEGACY_CURVE_ABSOLUTE,
    PROFILE_LEGACY_CURVE_ORIGIN, PROFILE_LEGACY_CURVE_BASE };
struct ProfileLegacyOptions {
    ProfileLegacyFormat format = PROFILE_LEGACY_NONE;
    ProfileLegacyMemoryUnit memory = PROFILE_LEGACY_MEMORY_UNKNOWN;
    ProfileLegacyCurveKind curve = PROFILE_LEGACY_CURVE_UNKNOWN;
    // This machine's own slot (profile_ini_slot.h): a curve without its
    // marker is read with the slot loader's rule instead of asking.
    bool nativeSlot = false;
};
struct ProfileSourceSelection {
    bool all = true;
    int count = 5, slots[5]{1, 2, 3, 4, 5};
};
namespace profile_legacy {
using profile_codec::View;
using Fields = std::map<std::string, std::string>;
using Sections = std::map<std::string, Fields>;
inline std::string lower(View text) {
    std::string result(text);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c = (char)(c + 'a' - 'A');
    return result;
}
inline bool fail(ProfileCodecError* e, const char* text) { return profile_codec::fail(e, 0, text); }
inline bool sections(View raw, Sections& out, ProfileCodecError* e) {
    std::string decoded;
    if (!profile_config::decode(raw, decoded, e)) return false;
    View input(decoded), line;
    if (input.substr(0, 3) == "\xef\xbb\xbf") input.remove_prefix(3);
    Fields* current = nullptr; int fields = 0;
    while (profile_slot::next_line(input, line)) {
        if (line.size() > PROFILE_FILE_MAX_LINE) return fail(e, "legacy line exceeds size limit");
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[') {
            if (line.back() != ']') return fail(e, "invalid legacy section");
            const auto name = lower(profile_codec::trim(line.substr(1, line.size() - 2)));
            bool owned = name == "meta" || name == "controls" || name == "curve" || name == "fan_curve";
            if (name.size() > 7 && name.substr(0, 7) == "profile" && name[7] >= '0' && name[7] <= '9') {
                if (name[7] < '1' || name[7] > '5' ||
                    (name.substr(8) != "" && name.substr(8) != "_curve" && name.substr(8) != "_fan_curve"))
                    return fail(e, "unsupported legacy profile section");
                owned = true;
            }
            current = nullptr;
            if (owned) {
                auto inserted = out.emplace(name, Fields{});
                if (!inserted.second) return fail(e, "duplicate legacy tuning section");
                current = &inserted.first->second;
            }
            continue;
        }
        if (!current) continue; // Application preferences are not tuning intent.
        const size_t equals = line.find('=');
        if (equals == View::npos || ++fields > 32768) return fail(e, "invalid or excessive legacy tuning fields");
        const auto key = lower(profile_codec::trim(line.substr(0, equals)));
        if (key.empty()) return fail(e, "empty legacy tuning key");
        if (!current->emplace(key, std::string(profile_codec::trim(line.substr(equals + 1)))).second)
            return fail(e, "duplicate legacy tuning key");
    }
    return true;
}
inline bool take(Fields& fields, const std::string& key, std::string& value) {
    auto at = fields.find(key); if (at == fields.end()) return false;
    value = at->second; fields.erase(at); return true;
}
inline bool number(Fields& fields, const std::string& key, gc_i64& value, bool& present,
    ProfileCodecError* e, gc_i64 low = INT_MIN, gc_i64 high = INT_MAX) {
    std::string text; present = take(fields, key, text);
    if (!present) return true;
    if (!profile_codec::number(View(text), value, low, high)) return fail(e, "invalid legacy numeric tuning value");
    return true;
}
inline bool fan(Fields& controls, Fields& fields, ProfileDocument& d, ProfileCodecError* e) {
    std::string mode, speed;
    const bool hasMode = take(controls, "fan_mode", mode), hasSpeed = take(controls, "fan", speed);
    gc_i64 fixed = 0; bool hasFixed = false;
    if (!number(controls, "fan_fixed_pct", fixed, hasFixed, e, 0, 100)) return false;
    if (!hasMode && !hasSpeed && !hasFixed) return fields.empty() || fail(e, "legacy fan configuration has no fan intent");
    mode = lower(mode); speed = lower(speed);
    int selected = FAN_MODE_AUTO;
    if (hasMode) {
        if (mode == "fixed") selected = FAN_MODE_FIXED;
        else if (mode == "curve") selected = FAN_MODE_CURVE;
        else if (mode != "auto") return fail(e, "invalid legacy fan mode");
    } else if (hasFixed || (hasSpeed && speed != "auto")) selected = FAN_MODE_FIXED;
    gc_i64 duty = hasFixed ? fixed : 0;
    if (hasSpeed && speed != "auto") {
        if (!profile_codec::number(View(speed), duty, 0, 100) || (hasFixed && duty != fixed))
            return fail(e, "contradictory legacy fan duty");
    } else if (selected == FAN_MODE_FIXED && !hasFixed) return fail(e, "legacy fixed fan duty is missing");
    d.fan.present = true; d.domains |= PROFILE_DOMAIN_FAN;
    d.fan.fanMode = selected; d.fan.fanAuto = selected == FAN_MODE_AUTO; d.fan.fanPercent = (int)duty;
    if (fields.empty()) return selected != FAN_MODE_CURVE || fail(e, "legacy fan curve is missing");
    d.fan.hasCurveConfig = true;
    fan_curve_set_default(&d.fan.fanCurve);
    auto& c = d.fan.fanCurve; bool present = false, zeroHysteresis = false; gc_i64 value = 0;
    const auto integer = [&](const std::string& key, int& destination, int low, int high) {
        if (!number(fields, key, value, present, e, low, high)) return false;
        if (present) destination = (int)value;
        return true;
    };
    if (!integer("poll_interval_ms", c.pollIntervalMs, 1, 5000) || !integer("hysteresis_c", c.hysteresisC, 0, 100) ||
        !number(fields, "zero_rpm_enabled", value, present, e, 0, 1)) return false;
    if (present) c.zeroRpmEnabled = (gc_bool8)value;
    if (!number(fields, "zero_rpm_hysteresis_c", value, zeroHysteresis, e, 1, 100)) return false;
    if (zeroHysteresis) c.zeroRpmHysteresisC = (gc_u8)value;
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; ++i) {
        auto& p = c.points[i]; const auto index = std::to_string(i);
        if (!number(fields, "enabled" + index, value, present, e, 0, 1)) return false;
        if (present) p.enabled = (gc_bool8)value;
        if (!integer("temp" + index, p.temperatureC, 0, 100) || !integer("pct" + index, p.fanPercent, 0, 100)) return false;
    }
    fan_curve_migrate_legacy_zero_rpm_hysteresis(&c, zeroHysteresis);
    if (!fields.empty()) return fail(e, "unsupported legacy fan curve key");
    return fan_curve_validate(&c, e ? e->message : nullptr, e ? sizeof(e->message) : 0);
}
// Builds before the curve marker saved either final clocks or base clocks plus
// the core offset without saying which. For this machine's own slots, answer
// with the rule the slot loader applies (config_profile_repair.cpp on Windows,
// linux_port_profiles.cpp on Linux), so an export describes what the slot
// actually applies. A file being imported still asks.
inline ProfileLegacyCurveKind native_unmarked(const Fields& fields, ProfileLegacyOptions options,
    const ProfileDocument& d, bool& dropPoints) {
    dropPoints = false;
    const gc_i64 offsetMHz = d.graphicsOffset.present ? d.graphicsOffset.offsetHz / 1000000 : 0;
    if (!offsetMHz) return PROFILE_LEGACY_CURVE_ABSOLUTE;
    // The Linux loader cannot tell either, so it keeps the offset and ignores the points.
    if (options.format == PROFILE_LEGACY_LINUX_INI) { dropPoints = true; return PROFILE_LEGACY_CURVE_ABSOLUTE; }
    // Windows: base clocks only when the lock point plus the offset lands
    // exactly on the lock frequency and the stored value alone does not.
    const int ci = d.lock.present ? d.lock.anchorTopologyId : -1;
    if (offsetMHz < 0 || ci < 0 || ci >= VF_NUM_POINTS || d.lock.targetFrequencyHz % 1000000) return PROFILE_LEGACY_CURVE_ABSOLUTE;
    if (d.graphicsOffset.exclusionPolicy == PROFILE_EXCLUDE_LEGACY_COUNT && ci < d.graphicsOffset.excludeLowCount)
        return PROFILE_LEGACY_CURVE_ABSOLUTE;
    const auto at = fields.find("point" + std::to_string(ci) + "_mhz");
    gc_i64 stored = 0;
    if (at == fields.end() || !profile_codec::number(View(at->second), stored, 1, UINT_MAX)) return PROFILE_LEGACY_CURVE_ABSOLUTE;
    const gc_i64 lockMHz = d.lock.targetFrequencyHz / 1000000;
    return stored != lockMHz && stored + offsetMHz == lockMHz ? PROFILE_LEGACY_CURVE_BASE : PROFILE_LEGACY_CURVE_ABSOLUTE;
}
inline bool points(Fields& fields, ProfileLegacyOptions options, ProfileDocument& d, ProfileCodecError* e) {
    std::string marker, format;
    take(fields, "format", format); take(fields, "curve_semantics", marker);
    if (!format.empty() && format != "explicit_vf_points_v1") return fail(e, "unsupported legacy curve format");
    ProfileLegacyCurveKind kind = marker == "absolute_with_origin" ? PROFILE_LEGACY_CURVE_ORIGIN :
        marker == "base_plus_gpu_offset" ? PROFILE_LEGACY_CURVE_BASE :
        marker == "absolute" ? PROFILE_LEGACY_CURVE_ABSOLUTE : PROFILE_LEGACY_CURVE_UNKNOWN;
    if (!marker.empty() && kind == PROFILE_LEGACY_CURVE_UNKNOWN) return fail(e, "unsupported legacy curve semantics");
    if (options.curve != PROFILE_LEGACY_CURVE_UNKNOWN) {
        if (kind != PROFILE_LEGACY_CURVE_UNKNOWN && kind != options.curve) return fail(e, "legacy curve choice conflicts with its marker");
        kind = options.curve;
    }
    gc_i64 component = 0; bool hasComponent = false;
    if (!number(fields, "gpu_offset_mhz", component, hasComponent, e)) return false;
    if (hasComponent) {
        if (d.graphicsOffset.present && d.graphicsOffset.offsetHz != component * 1000000)
            return fail(e, "legacy curve and graphics controls disagree");
        d.graphicsOffset.present = true; d.graphicsOffset.offsetHz = component * 1000000; d.domains |= PROFILE_DOMAIN_GRAPHICS_OFFSET;
    }
    // Windows writers copy the exclusion count next to the offset. The loaders
    // read only the control section's value, so the copy must agree with it.
    gc_i64 excludeCopy = 0; bool hasExcludeCopy = false;
    if (!number(fields, "gpu_offset_exclude_low_count", excludeCopy, hasExcludeCopy, e, 0, VF_NUM_POINTS)) return false;
    const gc_i64 exclude = d.graphicsOffset.exclusionPolicy == PROFILE_EXCLUDE_LEGACY_COUNT ? d.graphicsOffset.excludeLowCount : 0;
    if (hasExcludeCopy && excludeCopy != exclude) return fail(e, "legacy curve and graphics exclusions disagree");
    bool dropPoints = false;
    if (kind == PROFILE_LEGACY_CURVE_UNKNOWN && marker.empty() && options.nativeSlot)
        kind = native_unmarked(fields, options, d, dropPoints);
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        const std::string root = "point" + std::to_string(ci);
        gc_i64 mhz = 0, voltage = 0, offset = 0, origin = 0, visible = 0;
        bool has = false, hasVoltage = false, hasOffset = false, hasOrigin = false, hasVisible = false;
        if (!number(fields, root + (options.format == PROFILE_LEGACY_WINDOWS_INI ? "_mhz" : ""), mhz, has, e, 0, UINT_MAX) ||
            !number(fields, root + "_from_gpu_offset", origin, hasOrigin, e, 0, 1)) return false;
        if (options.format == PROFILE_LEGACY_WINDOWS_INI &&
            (!number(fields, root + "_mv", voltage, hasVoltage, e, 1, UINT_MAX) ||
             !number(fields, root + "_offset_khz", offset, hasOffset, e) ||
             !number(fields, root + "_visible", visible, hasVisible, e, 0, 1))) return false;
        if (!has) {
            if (hasVoltage || hasOffset || hasOrigin || hasVisible) return fail(e, "legacy point metadata has no point value");
            continue;
        }
        if (dropPoints) continue;
        if (kind == PROFILE_LEGACY_CURVE_UNKNOWN) return fail(e, "legacy points require an explicit source curve-semantics choice");
        if (hasOrigin && kind != PROFILE_LEGACY_CURVE_ORIGIN) return fail(e, "legacy point origin conflicts with curve semantics");
        const bool projected = kind == PROFILE_LEGACY_CURVE_BASE || (kind == PROFILE_LEGACY_CURVE_ORIGIN && origin);
        if (mhz == 0 && !(kind == PROFILE_LEGACY_CURVE_ORIGIN && origin))
            return fail(e, "invalid legacy numeric tuning value");
        if (projected && !d.graphicsOffset.present) return fail(e, "legacy projected points have no graphics offset intent");
        const int at = d.topologyCount++;
        // Rounded historical mV labels cannot supply exact voltage identity.
        // Retain unresolved source indices; never borrow destination voltages.
        d.topology[at] = {ci, ci, 0, 0};
        auto& point = d.points[at];
        point.kind = projected ? PROFILE_POINT_PROJECTION : PROFILE_POINT_ABSOLUTE_FREQUENCY;
        if (!projected) point.absoluteFrequencyHz = mhz * 1000000;
        else if (kind != PROFILE_LEGACY_CURVE_BASE && mhz > 0) {
            point.hasObservedFrequency = true;
            point.observedFrequencyHz = mhz * 1000000;
        }
        if (hasOffset) { point.hasObservedOffset = true; point.observedOffsetHz = offset * 1000; }
        d.domains |= PROFILE_DOMAIN_VF_CURVE;
    }
    return fields.empty() || fail(e, "unsupported legacy curve key");
}
inline bool document(Fields controls, Fields curve, Fields fanFields, const Fields& meta,
    ProfileLegacyOptions options, int slot, ProfileDocument& output, ProfileCodecError* e) {
    for (const auto& field : controls) if (field.first.substr(0, 9) == "document.")
        return fail(e, "new profile documents cannot use legacy decoding");
    auto d = profile_document_make_empty();
    d.documentId = (gc_u32)slot; d.applicationScope = PROFILE_SCOPE_REPLACE_PROFILE; d.pointSetPolicy = PROFILE_POINTS_REPLACE;
    d.source.osFamily = options.format == PROFILE_LEGACY_WINDOWS_INI ? PROFILE_SOURCE_OS_WINDOWS : PROFILE_SOURCE_OS_LINUX;
    snprintf(d.displayName, sizeof(d.displayName), "Legacy profile %d", slot);
    gc_i64 value = 0; bool present = false;
    const auto scalar = [&](const char* key, gc_i64 scale, bool& has, gc_i64& target, int domain) {
        if (!number(controls, key, value, present, e)) return false;
        has = present; if (present) { target = value * scale; d.domains |= domain; } return true;
    };
    if (!scalar("gpu_offset_mhz", 1000000, d.graphicsOffset.present, d.graphicsOffset.offsetHz, PROFILE_DOMAIN_GRAPHICS_OFFSET)) return false;
    gc_i64 exclude = 0; bool hasExclude = false;
    if (!number(controls, "gpu_offset_exclude_low_count", exclude, hasExclude, e, 0, VF_NUM_POINTS)) return false;
    if (hasExclude && exclude) {
        if (!d.graphicsOffset.present) return fail(e, "legacy exclusions have no graphics offset");
        d.graphicsOffset.exclusionPolicy = PROFILE_EXCLUDE_LEGACY_COUNT; d.graphicsOffset.excludeLowCount = (int)exclude;
    }
    gc_i64 memory = 0; bool hasMemory = false;
    if (!number(controls, "mem_offset_mhz", memory, hasMemory, e)) return false;
    if (hasMemory) {
        auto unit = options.memory;
        auto migrated = meta.find("linux_mem_migrated");
        if (options.format == PROFILE_LEGACY_WINDOWS_INI) {
            if (unit == PROFILE_LEGACY_MEMORY_EFFECTIVE) return fail(e, "Windows legacy memory uses display MHz");
            unit = PROFILE_LEGACY_MEMORY_DISPLAY;
        } else if (migrated != meta.end()) {
            if (migrated->second != "1" || unit == PROFILE_LEGACY_MEMORY_EFFECTIVE)
                return fail(e, "legacy memory choice conflicts with its unit marker");
            unit = PROFILE_LEGACY_MEMORY_DISPLAY;
        }
        if (unit == PROFILE_LEGACY_MEMORY_UNKNOWN) return fail(e, "legacy Linux memory requires an explicit source-unit choice");
        d.hasMemoryOffset = true; d.memoryOffsetHz = memory * (unit == PROFILE_LEGACY_MEMORY_DISPLAY ? 1000000LL : 500000LL);
        d.domains |= PROFILE_DOMAIN_MEMORY;
    }
    if (!number(controls, "power_limit_pct", value, present, e, 1, INT_MAX)) return false;
    if (present) { d.power.present = true; d.power.ratio = tuning_power_ratio_from_percent(value); d.domains |= PROFILE_DOMAIN_POWER; }
    if (!scalar("xbar_offset_khz", 1000, d.hasXbarOffsetHz, d.xbarOffsetHz, PROFILE_DOMAIN_XBAR) ||
        !scalar("xbar_msvdd_offset_uv", 1, d.hasXbarMsvddOffsetUv, d.xbarMsvddOffsetUv, PROFILE_DOMAIN_XBAR) ||
        !scalar("sys_clk_offset_khz", 1000, d.hasSysClkOffsetHz, d.sysClkOffsetHz, PROFILE_DOMAIN_SYS_CLK) ||
        !scalar("video_clk_offset_khz", 1000, d.hasVideoClkOffsetHz, d.videoClkOffsetHz, PROFILE_DOMAIN_VIDEO_CLK)) return false;
    gc_i64 lockCi = -1, lockMHz = 0, lockMode = LOCK_MODE_FLATTEN, tracks = 0;
    bool hasCi = false, hasMHz = false, hasMode = false, hasTracks = false;
    if (!number(controls, "lock_ci", lockCi, hasCi, e, -1, 127) || !number(controls, "lock_mhz", lockMHz, hasMHz, e, 0, UINT_MAX) ||
        !number(controls, "lock_mode", lockMode, hasMode, e, LOCK_MODE_NONE, LOCK_MODE_HARD) ||
        !number(controls, "lock_tracks_anchor", tracks, hasTracks, e, 0, 1)) return false;
    // Earlier Linux/Windows writers emitted lock_tracks_anchor=1 (the default
    // UI flag) alongside the explicit no-lock triple (lock_ci=-1, lock_mhz=0,
    // lock_mode=0). Accept that explicit unlocked record while still rejecting
    // bare or partial lock declarations.
    const bool explicitNoLock = hasCi && lockCi < 0 &&
        (!hasMHz || lockMHz == 0) && (!hasMode || lockMode == LOCK_MODE_NONE);
    if ((hasCi && lockCi >= 0) || (hasMHz && lockMHz > 0) ||
        (hasMode && lockMode != LOCK_MODE_NONE) || (tracks && !explicitNoLock)) {
        if (!hasCi || lockCi < 0 || !hasMHz || lockMHz <= 0 || (hasMode && lockMode == LOCK_MODE_NONE))
            return fail(e, "legacy lock needs an explicit anchor, frequency and mode");
        d.lock.present = true; d.lock.mode = (LockMode)lockMode; d.lock.anchorTopologyId = (int)lockCi;
        d.lock.targetFrequencyHz = lockMHz * 1000000; d.lock.trackAnchor = tracks != 0; d.domains |= PROFILE_DOMAIN_LOCK;
    }
    if (!fan(controls, fanFields, d, e)) return false;
    if (!controls.empty()) return fail(e, "unsupported legacy control key");
    if (!points(curve, options, d, e)) return false;
    if (d.lock.present && profile_topology_index(d, d.lock.anchorTopologyId) < 0) {
        const int at = d.topologyCount++; d.topology[at] = {(int)lockCi, (int)lockCi, 0, 0};
    }
    d.legacyIndicesUnresolved = d.topologyCount != 0 || d.graphicsOffset.exclusionPolicy == PROFILE_EXCLUDE_LEGACY_COUNT;
    if (!profile_document_structurally_valid(d, e ? e->message : nullptr, e ? sizeof(e->message) : 0)) return false;
    output = d; return true;
}
} // namespace profile_legacy
