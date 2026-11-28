// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_SUMMARY_H
#define GREEN_CURVE_PROFILE_SUMMARY_H
// Plain-language, one-line descriptions of profiles and captured GPU settings
// for the import, export and capture dialogs. Presentation only: nothing here
// decides what is saved or applied.
#include "profile_capture_proposals.h"
#include "profile_document_domains.h"
#include "profile_legacy_flat_tail_offset_policy.h"
#include <string>

// "+75", "-12.5", "0" -- MHz with at most three decimals, sign for nonzero.
static inline std::string profile_summary_mhz(long long hz, bool sign = true) {
    const bool negative = hz < 0;
    const unsigned long long magnitude = negative ? 0ULL - (unsigned long long)hz : (unsigned long long)hz;
    unsigned long long whole = magnitude / 1000000ULL, fraction = (magnitude % 1000000ULL + 500ULL) / 1000ULL;
    if (fraction >= 1000ULL) { ++whole; fraction = 0; }
    char text[48]{};
    if (fraction) {
        char digits[8]{};
        snprintf(digits, sizeof(digits), "%03llu", fraction);
        for (int i = 2; i > 0 && digits[i] == '0'; --i) digits[i] = 0;
        snprintf(text, sizeof(text), "%s%llu.%s", negative ? "-" : (sign && magnitude ? "+" : ""), whole, digits);
    } else {
        snprintf(text, sizeof(text), "%s%llu", negative ? "-" : (sign && magnitude ? "+" : ""), whole);
    }
    return text;
}
static inline std::string profile_summary_percent(TuningPowerRatio ratio) {
    long long tenths = 0; bool exact = false;
    if (!tuning_power_ratio_valid(ratio) ||
        !tuning_mul_div_nonnegative(ratio.numerator, 1000, ratio.denominator, &tenths, &exact)) return "?";
    char text[32]{};
    if (tenths % 10) snprintf(text, sizeof(text), "%lld.%lld %%", tenths / 10, tenths % 10);
    else snprintf(text, sizeof(text), "%lld %%", tenths / 10);
    return text;
}
static inline void profile_summary_append(std::string& out, const std::string& part) {
    if (part.empty()) return;
    if (!out.empty()) out += ", ";
    out += part;
}
static inline std::string profile_summary_capitalized(std::string text) {
    if (!text.empty() && text[0] >= 'a' && text[0] <= 'z') text[0] = (char)(text[0] - 'a' + 'A');
    return text;
}

// "Flat at 1905 MHz, 12 curve points, core +75 MHz, memory +1000 MHz, power 90 %"
static inline std::string profile_document_summary(const ProfileDocument& d) {
    std::string out;
    if (d.lock.present && d.lock.mode != LOCK_MODE_NONE) {
        profile_summary_append(out, std::string(d.lock.mode == LOCK_MODE_HARD ? "locked at " : "flat at ") +
            profile_summary_mhz(d.lock.targetFrequencyHz, false) + " MHz");
    }
    int points = 0;
    for (int i = 0; i < d.topologyCount && i < VF_NUM_POINTS; ++i)
        if (d.points[i].kind != PROFILE_POINT_ABSENT) ++points;
    if (points) profile_summary_append(out, std::to_string(points) + (points == 1 ? " curve point" : " curve points"));
    if (d.graphicsOffset.present) {
        const long long offsetHz = profile_indexed_document_has_legacy_flat_tail_offset_artifact(d)
            ? 0LL : d.graphicsOffset.offsetHz;
        profile_summary_append(out, "core " + profile_summary_mhz(offsetHz) + " MHz");
    }
    if (d.hasMemoryOffset) profile_summary_append(out, "memory " + profile_summary_mhz(d.memoryOffsetHz) + " MHz");
    if (d.power.present) profile_summary_append(out, "power " + profile_summary_percent(d.power.ratio));
    if (d.fan.present) {
        char fan[32]{};
        if (d.fan.fanMode == FAN_MODE_FIXED) snprintf(fan, sizeof(fan), "fan %d %%", d.fan.fanPercent);
        else snprintf(fan, sizeof(fan), "%s", d.fan.fanMode == FAN_MODE_CURVE ? "fan curve" : "fan auto");
        profile_summary_append(out, fan);
    }
    if (d.hasXbarOffsetHz || d.hasXbarMsvddOffsetUv || d.hasSysClkOffsetHz || d.hasVideoClkOffsetHz)
        profile_summary_append(out, "advanced clocks");
    return out.empty() ? std::string("No settings") : profile_summary_capitalized(out);
}

// Rows of the capture dialog. `available` is false when the GPU did not let
// the setting be read; `text` then says so.
struct ProfileCaptureSummaryRow { bool available = false, changed = false; std::string text; };
struct ProfileCaptureSummary {
    ProfileCaptureSummaryRow curve, core, memory, power, advanced;
    bool advancedPresent = false;
};
static inline bool profile_capture_domain_read(const HardwareCaptureObservation& o, gc_u32 domain) {
    return (o.capturedDomains & domain) != 0;
}
static inline gc_u32 profile_capture_domain_status(const HardwareCaptureObservation& o, gc_u32 domain) {
    for (int index = 0; index < 9; ++index) if ((1u << index) == domain) return o.domainStatus[index];
    return CAPTURE_UNSUPPORTED;
}
static inline ProfileCaptureSummaryRow profile_capture_scalar_row(const HardwareCaptureObservation& o, gc_u32 domain,
    const HardwareCaptureScalar& value) {
    ProfileCaptureSummaryRow row;
    row.available = profile_capture_domain_read(o, domain) && value.valid;
    if (!row.available) { row.text = "Not readable on this GPU"; return row; }
    row.changed = value.value != 0;
    row.text = row.changed ? profile_summary_mhz(value.value) + " MHz" : "No offset (stock)";
    return row;
}
// The driver-reported lock, in words, or empty when none is held or known.
static inline std::string profile_capture_lock_text(const HardwareCaptureObservation& o,
    const ProfileCaptureProposals* proposals) {
    if (!o.pinGetterValid) return std::string();
    const std::string voltage = profile_summary_mhz((gc_i64)o.pinVoltageUv * 1000, false) + " mV";
    if (o.pinVoltageLocked && proposals && proposals->pin.recognized && proposals->pin.voltageLockUv)
        return "locked at " + voltage + " (" + profile_summary_mhz(proposals->pin.frequencyHz, false) + " MHz)";
    if (o.pinVoltageLocked && !o.pinPresent) return "voltage locked at " + voltage;
    if (!o.pinPresent) return std::string();
    const std::string ceiling = profile_summary_mhz(o.pinMaximumHz, false) + " MHz";
    std::string text = !o.pinMinimumHz ? "clock capped at " + ceiling :
        o.pinMinimumHz == o.pinMaximumHz ? "clock locked at " + ceiling :
        "clock held at " + profile_summary_mhz(o.pinMinimumHz, false) + "-" + ceiling;
    if (o.pinVoltageLocked) text += ", voltage locked at " + voltage;
    return text;
}
static inline ProfileCaptureSummary profile_capture_summary(const HardwareCaptureObservation& o,
    const ProfileCaptureProposals* proposals) {
    ProfileCaptureSummary s;
    bool uniformNonzeroCurve = false;
    if (profile_capture_domain_read(o, PROFILE_DOMAIN_VF_CURVE)) {
        s.curve.available = true;
        int changed = 0; int validOffsets = 0; long long first = 0; bool uniform = true;
        for (gc_u32 i = 0; i < o.pointCount && i < 128; ++i) {
            const auto& p = o.points[i];
            if (!p.offsetValid) continue;
            ++validOffsets;
            if (!p.offsetHz) continue;
            if (!changed) first = p.offsetHz; else if (p.offsetHz != first) uniform = false;
            ++changed;
        }
        s.curve.changed = changed > 0;
        if (proposals && proposals->flatten.selectable()) {
            s.curve.text = "Flat at " + profile_summary_mhz(proposals->flatten.frequencyHz, false) + " MHz from " +
                profile_summary_mhz(proposals->flatten.anchorVoltageUv * 1000, false) + " mV";
            int preTailChanged = 0; long long preTailFirst = 0; bool preTailUniform = true;
            for (gc_u32 i = 0; i < o.pointCount && i < 128; ++i) {
                const auto& p = o.points[i];
                if (!p.offsetValid || !p.offsetHz || p.voltageUv >= proposals->flatten.anchorVoltageUv) continue;
                if (!preTailChanged) preTailFirst = p.offsetHz;
                else if (p.offsetHz != preTailFirst) preTailUniform = false;
                ++preTailChanged;
            }
            if (preTailChanged > 0) {
                s.curve.text += preTailUniform
                    ? " (" + profile_summary_mhz(preTailFirst) + " MHz on " + std::to_string(preTailChanged) +
                        (preTailChanged == 1 ? " lower point)" : " lower points)")
                    : " (" + std::to_string(preTailChanged) + " lower points changed)";
            }
        } else if (!changed) {
            s.curve.text = "Stock curve (no changes)";
        } else if (uniform) {
            uniformNonzeroCurve = true;
            const bool allPoints = o.pointCount > 1 && (gc_u32)changed == o.pointCount && changed == validOffsets;
            s.curve.text = profile_summary_mhz(first) +
                (allPoints ? " MHz on all " : " MHz on ") + std::to_string(changed) +
                (changed == 1 ? " point" : " points");
        } else {
            s.curve.text = "Custom curve, " + std::to_string(changed) + " points changed";
        }
        // A lock another tool holds is part of what the curve does; the driver
        // reports it without any GPU load.
        const std::string lock = profile_capture_lock_text(o, proposals);
        if (!lock.empty()) { s.curve.text += ", " + lock; s.curve.changed = true; }
    } else {
        s.curve.text = "Not readable on this GPU";
    }
    s.core = profile_capture_scalar_row(o, PROFILE_DOMAIN_GRAPHICS_OFFSET, o.graphics);
    // Read, but whether it acts on top of the curve or is already part of it
    // is unknown, so it cannot be saved on its own. Not "unreadable".
    if (!s.core.available && o.graphics.valid &&
        profile_capture_domain_status(o, PROFILE_DOMAIN_GRAPHICS_OFFSET) == CAPTURE_COMPOSITION_UNRESOLVED)
        s.core.text = o.graphics.value
            ? profile_summary_mhz(o.graphics.value) + " MHz, cannot be saved separately on this GPU"
            : (uniformNonzeroCurve ? "No separate offset (included in Curve)" : "No separate offset (stock)");
    s.memory = profile_capture_scalar_row(o, PROFILE_DOMAIN_MEMORY, o.memory);
    s.power.available = profile_capture_domain_read(o, PROFILE_DOMAIN_POWER) && o.power.currentValid && o.power.defaultValid &&
        o.power.defaultMw > 0;
    if (s.power.available) {
        const TuningPowerRatio ratio = tuning_power_ratio_from_mw(o.power.currentMw, o.power.defaultMw);
        char watts[64]{};
        snprintf(watts, sizeof(watts), " (%lld W of %lld W)", o.power.currentMw / 1000, o.power.defaultMw / 1000);
        s.power.changed = o.power.currentMw != o.power.defaultMw;
        s.power.text = profile_summary_percent(ratio) + watts;
    } else {
        s.power.text = "Not readable on this GPU";
    }
    const gc_u32 advancedDomains = PROFILE_DOMAIN_XBAR | PROFILE_DOMAIN_SYS_CLK | PROFILE_DOMAIN_VIDEO_CLK;
    std::string advanced;
    const auto clock = [&](const char* name, gc_u32 domain, const HardwareCaptureScalar& v, const char* unit, long long scale) {
        if (!profile_capture_domain_read(o, domain) || !v.valid) return;
        s.advancedPresent = true;
        if (!v.value) return;
        s.advanced.changed = true;
        profile_summary_append(advanced, std::string(name) + " " + profile_summary_mhz(v.value * scale) + unit);
    };
    clock("XBAR", PROFILE_DOMAIN_XBAR, o.xbarClock, " MHz", 1);
    clock("XBAR voltage", PROFILE_DOMAIN_XBAR, o.xbarVoltage, " mV", 1000);
    clock("system", PROFILE_DOMAIN_SYS_CLK, o.systemClock, " MHz", 1);
    clock("video", PROFILE_DOMAIN_VIDEO_CLK, o.videoClock, " MHz", 1);
    s.advanced.available = s.advancedPresent && (o.capturedDomains & advancedDomains);
    s.advanced.text = !s.advanced.available ? "Not readable on this GPU" : advanced.empty() ? "No offsets (stock)" : advanced;
    return s;
}

// Radio-button captions for the curve choices the capture offers.
static inline std::string profile_capture_choice_label(const ProfileCaptureProposals& p, ProfileCaptureRepresentation r) {
    if (r == PROFILE_CAPTURE_FLATTEN)
        return "Flat curve at " + profile_summary_mhz(p.flatten.frequencyHz, false) + " MHz from " +
            profile_summary_mhz(p.flatten.anchorVoltageUv * 1000, false) + " mV";
    if (r == PROFILE_CAPTURE_PIN) {
        std::string evidence = p.pin.provenance == PROFILE_LOCK_PROVENANCE_USER_CONFIRMED_OBSERVATION ? " (detected under load)" :
            p.pin.provenance == PROFILE_LOCK_PROVENANCE_APPLIED_INTENT ? " (set by Green Curve)" : "";
        if (p.pin.provenance == PROFILE_LOCK_PROVENANCE_DRIVER_GETTER)
            evidence = p.pin.voltageLockUv
                ? " (replaces the voltage lock at " + profile_summary_mhz(p.pin.voltageLockUv * 1000, false) + " mV)"
                : std::string(" (read from the driver)");
        return "Hard clock lock at " + profile_summary_mhz(p.pin.frequencyHz, false) + " MHz" + evidence;
    }
    // An exact copy Green Curve would refuse at Apply (for example offsets past
    // this GPU's range that another tool used to flatten its curve) stays
    // available for the file, but says so.
    if (p.exactControls.disposition == PROFILE_CONTROL_REFUSED)
        return "Copy every curve point exactly (cannot be applied on this GPU)";
    return "Copy every curve point exactly";
}

#endif
