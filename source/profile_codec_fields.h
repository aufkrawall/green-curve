// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_CODEC_FIELDS_H
#define GREEN_CURVE_PROFILE_CODEC_FIELDS_H
#include "profile_codec_types.h"

namespace profile_codec {
// Key positions below are also the presence-mask contract used by finalization.
// Inactive payload keys are rejected by their presence, even when their value is zero.
struct FieldReader {
    View key;
    Seen& seen;
    int index = 0;
    bool found = false, valid = false;
    template <class Parse> void read(View name, Parse parse) {
        const unsigned long long bit = 1ull << index++;
        if (key != name) return;
        found = true;
        if (seen.keys & bit) return;
        seen.keys |= bit;
        valid = parse();
    }
};

inline bool field(DecodeState& state, const Location& at, View key, View v) {
    FieldReader f{key, seen(state, at)};
    // Parse callbacks are evaluated only for the matching key.
#define GC_PROFILE_FIELD(name, expression) f.read(name, [&] { return (expression); })
#define GC_PROFILE_I64(value) number(v, value, LLONG_MIN, LLONG_MAX)
#define GC_PROFILE_INT(value, low, high) number(v, value, low, high)
    if (at.section == HEADER) {
        int version = 0;
        GC_PROFILE_FIELD("format", v == "green_curve_profiles");
        GC_PROFILE_FIELD("major_version", number(v, version, 1, 1));
        GC_PROFILE_FIELD("profile_count", GC_PROFILE_INT(state.bank.count, 0, PROFILE_BANK_CAPACITY));
        return f.found && f.valid;
    }
    auto& d = state.bank.documents[at.document];
    auto& r = state.records[at.document];
    switch (at.section) {
        case DOCUMENT:
            GC_PROFILE_FIELD("document_id", GC_PROFILE_INT(d.documentId, 1, 4294967295LL));
            GC_PROFILE_FIELD("domains", domains(v, d.domains));
            GC_PROFILE_FIELD("application_scope", choice(v, d.applicationScope, {"update_included", "replace_profile", "explicit_release"}));
            GC_PROFILE_FIELD("point_set_policy", choice(v, d.pointSetPolicy, {"merge_points", "replace_points"}));
            GC_PROFILE_FIELD("topology_complete", boolean(v, d.sourceTopologyComplete));
            GC_PROFILE_FIELD("legacy_indices_unresolved", boolean(v, d.legacyIndicesUnresolved));
            GC_PROFILE_FIELD("topology_count", GC_PROFILE_INT(d.topologyCount, 0, VF_NUM_POINTS));
            GC_PROFILE_FIELD("point_count", GC_PROFILE_INT(r.pointCount, 0, VF_NUM_POINTS));
            GC_PROFILE_FIELD("display_name", text(v, d.displayName, sizeof(d.displayName)));
            break;
        case SOURCE:
            GC_PROFILE_FIELD("os", choice(v, d.source.osFamily, {"unknown", "windows", "linux"}));
            GC_PROFILE_FIELD("backend_family", GC_PROFILE_INT(d.source.backendFamily, 0, INT_MAX));
            GC_PROFILE_FIELD("device_id", GC_PROFILE_INT(d.source.deviceId, 0, 4294967295LL));
            GC_PROFILE_FIELD("subsystem_id", GC_PROFILE_INT(d.source.subSystemId, 0, 4294967295LL));
            GC_PROFILE_FIELD("driver_version", text(v, d.source.driverVersion, sizeof(d.source.driverVersion)));
            GC_PROFILE_FIELD("capture_complete", boolean(v, d.source.captureComplete));
            GC_PROFILE_FIELD("captured_domains", domains(v, d.source.capturedDomains));
            GC_PROFILE_FIELD("omitted_domains", domains(v, d.source.omittedDomains));
            GC_PROFILE_FIELD("unavailable_domains", domains(v, d.source.unavailableDomains));
            GC_PROFILE_FIELD("pin_knowledge", choice(v, d.source.pinKnowledge, {"unknown", "absent", "present"}));
            break;
        case TOPOLOGY: {
            auto& t = d.topology[at.index];
            GC_PROFILE_FIELD("topology_id", GC_PROFILE_INT(t.topologyId, 0, INT_MAX));
            GC_PROFILE_FIELD("source_index", GC_PROFILE_INT(t.sourceIndex, -1, VF_NUM_POINTS - 1));
            GC_PROFILE_FIELD("voltage_uv", GC_PROFILE_INT(t.voltageUv, 0, LLONG_MAX));
            GC_PROFILE_FIELD("source_writable", choice(v, t.sourceWritable, {"unknown", "writable", "read_only"}));
            break;
        }
        case POINT: {
            auto& p = r.points[at.index];
            GC_PROFILE_FIELD("topology_id", GC_PROFILE_INT(p.reference, 0, INT_MAX));
            GC_PROFILE_FIELD("kind", choice(v, p.value.kind, {"absent", "absolute", "projection", "driver_offset"}));
            GC_PROFILE_FIELD("target_hz", GC_PROFILE_INT(p.value.absoluteFrequencyHz, 1, LLONG_MAX));
            GC_PROFILE_FIELD("offset_hz", GC_PROFILE_I64(p.value.capturedOffsetHz));
            GC_PROFILE_FIELD("observed_frequency_hz", GC_PROFILE_INT(p.value.observedFrequencyHz, 1, LLONG_MAX));
            GC_PROFILE_FIELD("observed_offset_hz", GC_PROFILE_I64(p.value.observedOffsetHz));
            break;
        }
        case GRAPHICS:
            GC_PROFILE_FIELD("offset_hz", GC_PROFILE_I64(d.graphicsOffset.offsetHz));
            GC_PROFILE_FIELD("exclusion_policy", choice(v, d.graphicsOffset.exclusionPolicy, {"none", "identities", "legacy_count_unresolved"}));
            GC_PROFILE_FIELD("excluded_count", GC_PROFILE_INT(d.graphicsOffset.excludedCount, 0, VF_NUM_POINTS));
            GC_PROFILE_FIELD("exclude_low_count", GC_PROFILE_INT(d.graphicsOffset.excludeLowCount, 0, VF_NUM_POINTS));
            break;
        case EXCLUSION:
            GC_PROFILE_FIELD("topology_id", GC_PROFILE_INT(d.graphicsOffset.excludedTopologyIds[at.index], 0, INT_MAX));
            break;
        case MEMORY:
            GC_PROFILE_FIELD("offset_hz", GC_PROFILE_I64(d.memoryOffsetHz));
            GC_PROFILE_FIELD("surface", choice(v, d.memorySurface, {"destination", "p0", "global"}));
            break;
        case POWER:
            GC_PROFILE_FIELD("mode", v == "board_default_ratio");
            GC_PROFILE_FIELD("numerator", GC_PROFILE_INT(d.power.ratio.numerator, 1, LLONG_MAX));
            GC_PROFILE_FIELD("denominator", GC_PROFILE_INT(d.power.ratio.denominator, 1, LLONG_MAX));
            break;
        case POWER_OBSERVATION:
            GC_PROFILE_FIELD("observed_default_mw", GC_PROFILE_INT(d.power.observedDefaultMw, 1, LLONG_MAX));
            GC_PROFILE_FIELD("observed_limit_mw", GC_PROFILE_INT(d.power.observedCurrentMw, 1, LLONG_MAX));
            break;
        case LOCK:
            GC_PROFILE_FIELD("mode", v == "flatten" ? (d.lock.mode = LOCK_MODE_FLATTEN, true) :
                v == "hard" ? (d.lock.mode = LOCK_MODE_HARD, true) : false);
            GC_PROFILE_FIELD("anchor_topology_id", GC_PROFILE_INT(d.lock.anchorTopologyId, 0, INT_MAX));
            GC_PROFILE_FIELD("target_hz", GC_PROFILE_INT(d.lock.targetFrequencyHz, 1, LLONG_MAX));
            GC_PROFILE_FIELD("track_anchor", boolean(v, d.lock.trackAnchor));
            break;
        case LOCK_OBSERVATION:
            GC_PROFILE_FIELD("provenance", choice(v, d.lock.provenance, {"user", "recognized_flatten", "driver_getter", "applied_intent", "user_confirmed"}));
            GC_PROFILE_FIELD("recognition_present", boolean(v, d.lock.hasRecognitionEvidence));
            GC_PROFILE_FIELD("anchor_voltage_uv", GC_PROFILE_INT(d.lock.recognizedAnchorVoltageUv, 1, LLONG_MAX));
            GC_PROFILE_FIELD("tail_count", GC_PROFILE_INT(d.lock.recognizedTailCount, 1, VF_NUM_POINTS));
            GC_PROFILE_FIELD("tolerance_hz", GC_PROFILE_INT(d.lock.recognitionToleranceHz, 0, LLONG_MAX));
            GC_PROFILE_FIELD("pin_range_present", boolean(v, d.lock.hasObservedPinRange));
            GC_PROFILE_FIELD("pin_min_hz", GC_PROFILE_INT(d.lock.observedPinMinimumHz, 1, LLONG_MAX));
            GC_PROFILE_FIELD("pin_max_hz", GC_PROFILE_INT(d.lock.observedPinMaximumHz, 1, LLONG_MAX));
            break;
        case FAN:
            GC_PROFILE_FIELD("mode", choice(v, d.fan.fanMode, {"auto", "fixed", "curve"}));
            GC_PROFILE_FIELD("duty_pct", GC_PROFILE_INT(d.fan.fanPercent, 0, 100));
            GC_PROFILE_FIELD("curve_present", boolean(v, d.fan.hasCurveConfig));
            break;
        case FAN_CURVE:
            GC_PROFILE_FIELD("poll_interval_ms", GC_PROFILE_INT(d.fan.fanCurve.pollIntervalMs, 250, 5000));
            GC_PROFILE_FIELD("hysteresis_c", GC_PROFILE_INT(d.fan.fanCurve.hysteresisC, 0, 10));
            GC_PROFILE_FIELD("zero_rpm_enabled", boolean(v, d.fan.fanCurve.zeroRpmEnabled));
            GC_PROFILE_FIELD("zero_rpm_hysteresis_c", GC_PROFILE_INT(d.fan.fanCurve.zeroRpmHysteresisC, 2, 30));
            GC_PROFILE_FIELD("point_count", GC_PROFILE_INT(r.fanPointCount, FAN_CURVE_MAX_POINTS, FAN_CURVE_MAX_POINTS));
            break;
        case FAN_POINT: {
            auto& p = d.fan.fanCurve.points[at.index];
            GC_PROFILE_FIELD("enabled", boolean(v, p.enabled));
            GC_PROFILE_FIELD("temperature_c", GC_PROFILE_INT(p.temperatureC, 0, 100));
            GC_PROFILE_FIELD("duty_pct", GC_PROFILE_INT(p.fanPercent, 0, 100));
            break;
        }
        case AUX_XBAR:
            GC_PROFILE_FIELD("clock_offset_hz", GC_PROFILE_I64(d.xbarOffsetHz));
            GC_PROFILE_FIELD("voltage_offset_uv", GC_PROFILE_I64(d.xbarMsvddOffsetUv));
            break;
        case AUX_SYS:
            GC_PROFILE_FIELD("offset_hz", GC_PROFILE_I64(d.sysClkOffsetHz));
            break;
        case AUX_VIDEO:
            GC_PROFILE_FIELD("offset_hz", GC_PROFILE_I64(d.videoClkOffsetHz));
            break;
        default: return false;
    }
#undef GC_PROFILE_FIELD
#undef GC_PROFILE_I64
#undef GC_PROFILE_INT
    return f.found && f.valid;
}
} // namespace profile_codec
#endif
