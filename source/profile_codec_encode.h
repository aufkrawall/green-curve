// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_CODEC_ENCODE_H
#define GREEN_CURVE_PROFILE_CODEC_ENCODE_H
#include "profile_codec_types.h"

namespace profile_codec {
inline bool bank_valid(const ProfileBank& bank, ProfileCodecError* error) {
    if (bank.count < 0 || bank.count > PROFILE_BANK_CAPACITY) return fail(error, 0, "invalid profile count");
    for (int i = 0; i < bank.count; ++i) {
        char why[160]{};
        if (!profile_document_structurally_valid(bank.documents[i], why, sizeof(why))) return fail(error, 0, why);
        for (int j = 0; j < i; ++j)
            if (bank.documents[i].documentId == bank.documents[j].documentId)
                return fail(error, 0, "duplicate document identity");
    }
    return true;
}
struct Writer {
    std::string result;
    bool valid = true;
    void append(View v) {
        if (!valid) return;
        if (v.size() > PROFILE_FILE_MAX_BYTES - result.size()) { valid = false; return; }
        result.append(v.data(), v.size());
    }
    void section(const std::string& name) { append("\n["); append(name); append("]\n"); }
    void value(View key, View value) { append(key); append("="); append(value); append("\n"); }
    void integer(View key, long long n) { value(key, std::to_string(n)); }
    void boolean(View key, bool b) { value(key, b ? "1" : "0"); }
    void text(View key, const char* text) {
        append(key); append("=\"");
        for (const char* p = text; *p; ++p) {
            if (*p == '\\' || *p == '"') append("\\");
            append(View(p, 1));
        }
        append("\"\n");
    }
    void domains(View key, int mask) {
        append(key); append("=");
        if (!mask) append("none");
        bool first = true;
        for (int i = 0; i < 9; ++i) if (mask & (1 << i)) {
            if (!first) append(",");
            append(domainNames[i]); first = false;
        }
        append("\n");
    }
};
inline bool point_record(const ProfilePointIntent& p) {
    return p.kind != PROFILE_POINT_ABSENT || p.hasObservedFrequency || p.hasObservedOffset;
}
inline void write_document(Writer& w, const ProfileDocument& d, int ordinal) {
    const std::string root = "profile." + std::to_string(ordinal);
    int pointCount = 0;
    for (int i = 0; i < d.topologyCount; ++i) if (point_record(d.points[i])) ++pointCount;
    w.section(root);
    w.integer("document_id", d.documentId);
    w.domains("domains", d.domains);
    const View scopes[] = {"update_included", "replace_profile", "explicit_release"};
    w.value("application_scope", scopes[d.applicationScope]);
    w.value("point_set_policy", d.pointSetPolicy == PROFILE_POINTS_MERGE ? "merge_points" : "replace_points");
    w.boolean("topology_complete", d.sourceTopologyComplete);
    w.boolean("legacy_indices_unresolved", d.legacyIndicesUnresolved);
    w.integer("topology_count", d.topologyCount);
    w.integer("point_count", pointCount);
    w.text("display_name", d.displayName);
    w.section(root + ".source");
    const View operatingSystems[] = {"unknown", "windows", "linux"};
    const View pinKnowledge[] = {"unknown", "absent", "present"};
    w.value("os", operatingSystems[d.source.osFamily]);
    w.integer("backend_family", d.source.backendFamily);
    w.integer("device_id", d.source.deviceId);
    w.integer("subsystem_id", d.source.subSystemId);
    w.text("driver_version", d.source.driverVersion);
    w.boolean("capture_complete", d.source.captureComplete);
    w.domains("captured_domains", (int)d.source.capturedDomains);
    w.domains("omitted_domains", (int)d.source.omittedDomains);
    w.domains("unavailable_domains", (int)d.source.unavailableDomains);
    w.value("pin_knowledge", pinKnowledge[d.source.pinKnowledge]);
    const View writable[] = {"unknown", "writable", "read_only"};
    for (int i = 0; i < d.topologyCount; ++i) {
        const auto& t = d.topology[i];
        w.section(root + ".topology." + std::to_string(i));
        w.integer("topology_id", t.topologyId);
        w.integer("source_index", t.sourceIndex);
        w.integer("voltage_uv", t.voltageUv);
        w.value("source_writable", writable[t.sourceWritable]);
    }
    const View pointKinds[] = {"absent", "absolute", "projection", "driver_offset"};
    int pointOrdinal = 0;
    for (int i = 0; i < d.topologyCount; ++i) {
        const auto& p = d.points[i];
        if (!point_record(p)) continue;
        w.section(root + ".point." + std::to_string(pointOrdinal++));
        w.integer("topology_id", d.topology[i].topologyId);
        w.value("kind", pointKinds[p.kind]);
        if (p.kind == PROFILE_POINT_ABSOLUTE_FREQUENCY) w.integer("target_hz", p.absoluteFrequencyHz);
        if (p.kind == PROFILE_POINT_CAPTURED_OFFSET) w.integer("offset_hz", p.capturedOffsetHz);
        if (p.hasObservedFrequency) w.integer("observed_frequency_hz", p.observedFrequencyHz);
        if (p.hasObservedOffset) w.integer("observed_offset_hz", p.observedOffsetHz);
    }
    if (d.graphicsOffset.present) {
        const auto& g = d.graphicsOffset;
        w.section(root + ".graphics");
        w.integer("offset_hz", g.offsetHz);
        const View policies[] = {"none", "identities", "legacy_count_unresolved"};
        w.value("exclusion_policy", policies[g.exclusionPolicy]);
        if (g.exclusionPolicy == PROFILE_EXCLUDE_LEGACY_COUNT) w.integer("exclude_low_count", g.excludeLowCount);
        if (g.exclusionPolicy == PROFILE_EXCLUDE_IDENTITIES) {
            w.integer("excluded_count", g.excludedCount);
            int sorted[VF_NUM_POINTS]{};
            for (int i = 0; i < g.excludedCount; ++i) {
                int j = i;
                while (j > 0 && sorted[j - 1] > g.excludedTopologyIds[i]) { sorted[j] = sorted[j - 1]; --j; }
                sorted[j] = g.excludedTopologyIds[i];
            }
            for (int i = 0; i < g.excludedCount; ++i) {
                w.section(root + ".exclusion." + std::to_string(i));
                w.integer("topology_id", sorted[i]);
            }
        }
    }
    if (d.hasMemoryOffset) {
        w.section(root + ".memory");
        w.integer("offset_hz", d.memoryOffsetHz);
        const View surfaces[] = {"destination", "p0", "global"};
        w.value("surface", surfaces[d.memorySurface]);
    }
    if (d.power.present) {
        w.section(root + ".power");
        w.value("mode", "board_default_ratio");
        const TuningPowerRatio ratio = tuning_power_ratio_reduce(d.power.ratio.numerator, d.power.ratio.denominator);
        w.integer("numerator", ratio.numerator);
        w.integer("denominator", ratio.denominator);
    }
    if (d.power.hasObservedDefaultMw || d.power.hasObservedCurrentMw) {
        w.section(root + ".power_observation");
        if (d.power.hasObservedDefaultMw) w.integer("observed_default_mw", d.power.observedDefaultMw);
        if (d.power.hasObservedCurrentMw) w.integer("observed_limit_mw", d.power.observedCurrentMw);
    }
    if (d.lock.present) {
        w.section(root + ".lock");
        w.value("mode", d.lock.mode == LOCK_MODE_HARD ? "hard" : "flatten");
        w.integer("anchor_topology_id", d.lock.anchorTopologyId);
        w.integer("target_hz", d.lock.targetFrequencyHz);
        w.boolean("track_anchor", d.lock.trackAnchor);
    }
    if (d.lock.present || d.lock.provenance != PROFILE_LOCK_PROVENANCE_USER ||
        d.lock.hasRecognitionEvidence || d.lock.hasObservedPinRange) {
        w.section(root + ".lock_observation");
        const View provenance[] = {"user", "recognized_flatten", "driver_getter", "applied_intent", "user_confirmed"};
        w.value("provenance", provenance[d.lock.provenance]);
        w.boolean("recognition_present", d.lock.hasRecognitionEvidence);
        if (d.lock.hasRecognitionEvidence) {
            w.integer("anchor_voltage_uv", d.lock.recognizedAnchorVoltageUv);
            w.integer("tail_count", d.lock.recognizedTailCount);
            w.integer("tolerance_hz", d.lock.recognitionToleranceHz);
        }
        w.boolean("pin_range_present", d.lock.hasObservedPinRange);
        if (d.lock.hasObservedPinRange) {
            w.integer("pin_min_hz", d.lock.observedPinMinimumHz);
            w.integer("pin_max_hz", d.lock.observedPinMaximumHz);
        }
    }
    if (d.fan.present) {
        w.section(root + ".fan");
        const View modes[] = {"auto", "fixed", "curve"};
        w.value("mode", modes[d.fan.fanMode]);
        w.integer("duty_pct", d.fan.fanPercent);
        w.boolean("curve_present", d.fan.hasCurveConfig);
        if (d.fan.hasCurveConfig) {
            const auto& c = d.fan.fanCurve;
            w.section(root + ".fan.curve");
            w.integer("poll_interval_ms", c.pollIntervalMs);
            w.integer("hysteresis_c", c.hysteresisC);
            w.boolean("zero_rpm_enabled", c.zeroRpmEnabled != 0);
            w.integer("zero_rpm_hysteresis_c", c.zeroRpmHysteresisC);
            w.integer("point_count", FAN_CURVE_MAX_POINTS);
            for (int i = 0; i < FAN_CURVE_MAX_POINTS; ++i) {
                w.section(root + ".fan.point." + std::to_string(i));
                w.boolean("enabled", c.points[i].enabled != 0);
                w.integer("temperature_c", c.points[i].temperatureC);
                w.integer("duty_pct", c.points[i].fanPercent);
            }
        }
    }
    if (d.hasXbarOffsetHz || d.hasXbarMsvddOffsetUv) {
        w.section(root + ".aux.xbar");
        if (d.hasXbarOffsetHz) w.integer("clock_offset_hz", d.xbarOffsetHz);
        if (d.hasXbarMsvddOffsetUv) w.integer("voltage_offset_uv", d.xbarMsvddOffsetUv);
    }
    if (d.hasSysClkOffsetHz) { w.section(root + ".aux.sys"); w.integer("offset_hz", d.sysClkOffsetHz); }
    if (d.hasVideoClkOffsetHz) { w.section(root + ".aux.video"); w.integer("offset_hz", d.videoClkOffsetHz); }
}
} // namespace profile_codec

static inline bool profile_bank_encode(const ProfileBank& bank, std::string& output,
    ProfileCodecError* error = nullptr) {
    using namespace profile_codec;
    if (error) *error = {};
    if (!bank_valid(bank, error)) return false;
    Writer writer;
    writer.append("[green_curve_profile]\nformat=green_curve_profiles\nmajor_version=1\n");
    writer.integer("profile_count", bank.count);
    for (int n = 0; n < bank.count; ++n) write_document(writer, bank.documents[n], n + 1);
    if (!writer.valid) return fail(error, 0, "profile file exceeds size limit");
    output.swap(writer.result);
    return true;
}
#endif
