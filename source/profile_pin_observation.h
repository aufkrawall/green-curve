// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Tier 3 capture pin evidence (§8.5): user-started bounded read-only
// workload observation. When neither a validated pin getter (Tier 1) nor
// Green Curve applied pin intent (Tier 2) is available, a bounded burst of
// live samples collected under the user's own active workload can propose a
// suspected hard-pin frequency (sustained clock below the captured VF curve
// ceiling at the operating voltage, with any reported driver limit reason).
//
// Pure, read-only, GPU-free; never runs automatically, never treats idle
// clocks as a pin, and never writes or adopts lock intent without explicit
// user confirmation/edit.
#pragma once
#include "hardware_capture.h"
#include <algorithm>
#include <stdio.h>
#include <string.h>

static constexpr int PROFILE_PIN_OBS_MIN_SAMPLES = 3;
static constexpr int PROFILE_PIN_OBS_DEFAULT_SAMPLES = 5;
static constexpr int PROFILE_PIN_OBS_MAX_SAMPLES = 16;
static constexpr gc_i64 PROFILE_PIN_OBS_SPREAD_TOLERANCE_HZ = 15000000LL;
static constexpr gc_i64 PROFILE_PIN_OBS_CURVE_MARGIN_HZ = 30000000LL;
static constexpr unsigned int PROFILE_PIN_OBS_MIN_UTIL_PCT = 30u;

enum : gc_u64 {
    PIN_OBS_REASON_GPU_IDLE = 0x0000000000000001ULL,
    PIN_OBS_REASON_APPLICATIONS_CLOCKS = 0x0000000000000002ULL,
    PIN_OBS_REASON_SW_POWER_CAP = 0x0000000000000004ULL,
    PIN_OBS_REASON_HW_SLOWDOWN = 0x0000000000000008ULL,
    PIN_OBS_REASON_SYNC_BOOST = 0x0000000000000010ULL,
    PIN_OBS_REASON_SW_THERMAL = 0x0000000000000020ULL,
    PIN_OBS_REASON_HW_THERMAL = 0x0000000000000040ULL,
    PIN_OBS_REASON_HW_POWER_BRAKE = 0x0000000000000080ULL,
    PIN_OBS_REASON_DISPLAY_CLOCK = 0x0000000000000100ULL,
};

struct PinWorkloadSample {
    bool clockValid = false;
    gc_i64 graphicsClockHz = 0;
    bool utilValid = false;
    unsigned int utilGpuPct = 0;
    unsigned int utilMemPct = 0;
    bool voltageValid = false;
    gc_i64 operatingVoltageUv = 0;
    bool perfStateValid = false;
    unsigned int perfState = 0;
    bool throttleValid = false;
    gc_u64 throttleReasons = 0;
    bool powerValid = false;
    unsigned int powerMw = 0;
};

enum PinObservationStatus : int {
    PIN_OBS_UNKNOWN = 0,
    PIN_OBS_SUPERSEDED = 1,
    PIN_OBS_IDLE = 2,
    PIN_OBS_NOT_PINNED_VARYING = 3,
    PIN_OBS_AT_CURVE = 4,
    PIN_OBS_SUSPECTED_PIN = 5,
    PIN_OBS_UNAVAILABLE = 6,
};

struct PinObservationResult {
    PinObservationStatus status = PIN_OBS_UNKNOWN;
    int totalSamples = 0;
    int activeSamples = 0;
    gc_i64 minClockHz = 0;
    gc_i64 maxClockHz = 0;
    gc_i64 medianClockHz = 0;
    gc_i64 operatingVoltageUv = 0;
    gc_i64 curveAllowedHz = 0;
    gc_i64 proposedPinHz = 0;
    unsigned int avgUtilGpuPct = 0;
    bool throttleKnown = false;
    gc_u64 limitReasonsMask = 0;
    bool hasThermalOrPowerThrottle = false;
    bool hasAppClockLimit = false;
    char limitReasonsText[192]{};
    char summary[256]{};
    char explanation[512]{};
};

static inline const char* profile_pin_observation_status_name(PinObservationStatus s) {
    switch (s) {
        case PIN_OBS_UNKNOWN: return "unknown";
        case PIN_OBS_SUPERSEDED: return "superseded";
        case PIN_OBS_IDLE: return "idle";
        case PIN_OBS_NOT_PINNED_VARYING: return "varying";
        case PIN_OBS_AT_CURVE: return "at-curve";
        case PIN_OBS_SUSPECTED_PIN: return "suspected-pin";
        case PIN_OBS_UNAVAILABLE: return "unavailable";
    }
    return "unknown";
}

static inline void profile_pin_observation_format_reasons(gc_u64 mask, char* out, size_t size) {
    if (!out || !size) return;
    out[0] = 0;
    struct { gc_u64 bit; const char* name; } table[] = {
        {PIN_OBS_REASON_APPLICATIONS_CLOCKS, "app-clocks"},
        {PIN_OBS_REASON_SW_POWER_CAP, "sw-power-cap"},
        {PIN_OBS_REASON_HW_SLOWDOWN, "hw-slowdown"},
        {PIN_OBS_REASON_SW_THERMAL, "sw-thermal"},
        {PIN_OBS_REASON_HW_THERMAL, "hw-thermal"},
        {PIN_OBS_REASON_HW_POWER_BRAKE, "hw-power-brake"},
        {PIN_OBS_REASON_SYNC_BOOST, "sync-boost"},
        {PIN_OBS_REASON_DISPLAY_CLOCK, "display-clock"},
        {PIN_OBS_REASON_GPU_IDLE, "gpu-idle"},
    };
    size_t used = 0;
    for (const auto& entry : table) {
        if (!(mask & entry.bit)) continue;
        const int n = snprintf(out + used, size - used, "%s%s", used ? "," : "", entry.name);
        if (n < 0 || (size_t)n >= size - used) { used = size - 1; break; }
        used += (size_t)n;
    }
    const gc_u64 knownBits = PIN_OBS_REASON_APPLICATIONS_CLOCKS | PIN_OBS_REASON_SW_POWER_CAP |
        PIN_OBS_REASON_HW_SLOWDOWN | PIN_OBS_REASON_SW_THERMAL | PIN_OBS_REASON_HW_THERMAL |
        PIN_OBS_REASON_HW_POWER_BRAKE | PIN_OBS_REASON_SYNC_BOOST | PIN_OBS_REASON_DISPLAY_CLOCK |
        PIN_OBS_REASON_GPU_IDLE;
    if (mask & ~knownBits) {
        snprintf(out + used, size - used, "%s0x%llx", used ? "," : "", (unsigned long long)(mask & ~knownBits));
    }
    if (!out[0]) snprintf(out, size, "none");
}

static inline gc_i64 profile_pin_observation_curve_ceiling(const HardwareCaptureObservation& obs,
    gc_i64 operatingVoltageUv) {
    if (!obs.valid || !obs.pointCount) return 0;
    gc_i64 bestFreq = 0;
    gc_i64 atOrBelowVolt = -1;
    gc_i64 lowestVolt = -1;
    gc_i64 lowestFreq = 0;
    gc_i64 maxFreq = 0;
    for (gc_u32 i = 0; i < obs.pointCount && i < 128u; ++i) {
        const auto& pt = obs.points[i];
        if (!pt.frequencyValid || pt.frequencyHz <= 0 || pt.voltageUv <= 0) continue;
        if (pt.frequencyHz > maxFreq) maxFreq = pt.frequencyHz;
        if (lowestVolt < 0 || pt.voltageUv < lowestVolt) {
            lowestVolt = pt.voltageUv;
            lowestFreq = pt.frequencyHz;
        }
        if (operatingVoltageUv > 0 && pt.voltageUv <= operatingVoltageUv && pt.voltageUv >= atOrBelowVolt) {
            atOrBelowVolt = pt.voltageUv;
            bestFreq = pt.frequencyHz;
        }
    }
    if (operatingVoltageUv <= 0) return maxFreq;
    return atOrBelowVolt >= 0 ? bestFreq : lowestFreq;
}

static inline bool profile_pin_observation_sample_active(const HardwareCaptureObservation& cap,
    const PinWorkloadSample& s) {
    if (!s.clockValid || s.graphicsClockHz <= 0) return false;
    if (s.throttleValid && (s.throttleReasons & PIN_OBS_REASON_GPU_IDLE)) return false;
    if (s.perfStateValid && s.perfState >= 5u) return false;
    if (s.utilValid) return s.utilGpuPct >= PROFILE_PIN_OBS_MIN_UTIL_PCT;
    // Without utilization telemetry, require active P0/P1 plus meaningful board power load.
    return s.perfStateValid && s.perfState <= 1u && s.powerValid && cap.power.defaultValid &&
        cap.power.defaultMw > 0 && (gc_i64)s.powerMw * 2 >= cap.power.defaultMw;
}

static inline void profile_pin_observation_analyze(
    const HardwareCaptureObservation& cap,
    bool getterOrAppliedPinPresent,
    const PinWorkloadSample* samples,
    int sampleCount,
    PinObservationResult& out) {
    out = {};
    const auto finish = [&](PinObservationStatus status, const char* explanation) {
        out.status = status;
        if (!out.limitReasonsText[0]) {
            if (out.throttleKnown) profile_pin_observation_format_reasons(out.limitReasonsMask, out.limitReasonsText, sizeof(out.limitReasonsText));
            else snprintf(out.limitReasonsText, sizeof(out.limitReasonsText), "unknown");
        }
        snprintf(out.explanation, sizeof(out.explanation), "%s", explanation);
        snprintf(out.summary, sizeof(out.summary),
            "pin-observe: status=%s samples=%d/%d clock=%lld curve=%lld proposed=%lld reasons=%s",
            profile_pin_observation_status_name(status), out.activeSamples, out.totalSamples,
            out.medianClockHz, out.curveAllowedHz, out.proposedPinHz, out.limitReasonsText);
    };
    if (!cap.valid || !(cap.requestedDomains & PROFILE_DOMAIN_LOCK) || !(cap.capturedDomains & PROFILE_DOMAIN_VF_CURVE)) {
        finish(PIN_OBS_UNAVAILABLE, "Capture VF curve and lock domains cleanly before starting workload pin observation.");
        return;
    }
    if (getterOrAppliedPinPresent || cap.pinGetterValid) {
        finish(PIN_OBS_SUPERSEDED, "Tier 1 getter or Tier 2 applied pin evidence already applies; observation will not override it.");
        return;
    }
    if (!samples || sampleCount <= 0) {
        finish(PIN_OBS_UNKNOWN, "No workload samples were read; pin state remains unknown.");
        return;
    }
    const int capped = std::min(sampleCount, PROFILE_PIN_OBS_MAX_SAMPLES);
    gc_i64 activeClocks[PROFILE_PIN_OBS_MAX_SAMPLES]{};
    gc_i64 activeVoltages[PROFILE_PIN_OBS_MAX_SAMPLES]{};
    int voltageCount = 0;
    int clockSamples = 0;
    unsigned long long utilSum = 0;
    int utilCount = 0;
    for (int i = 0; i < capped; ++i) {
        const auto& s = samples[i];
        ++out.totalSamples;
        if (s.clockValid && s.graphicsClockHz > 0) ++clockSamples;
        if (s.throttleValid) {
            out.throttleKnown = true;
            out.limitReasonsMask |= s.throttleReasons;
        }
        if (!profile_pin_observation_sample_active(cap, s)) continue;
        activeClocks[out.activeSamples++] = s.graphicsClockHz;
        if (s.voltageValid && s.operatingVoltageUv > 0) activeVoltages[voltageCount++] = s.operatingVoltageUv;
        if (s.utilValid) {
            utilSum += s.utilGpuPct;
            ++utilCount;
        }
    }
    if (out.throttleKnown) {
        profile_pin_observation_format_reasons(out.limitReasonsMask, out.limitReasonsText, sizeof(out.limitReasonsText));
        const gc_u64 capMask = PIN_OBS_REASON_SW_POWER_CAP | PIN_OBS_REASON_HW_SLOWDOWN |
            PIN_OBS_REASON_SW_THERMAL | PIN_OBS_REASON_HW_THERMAL | PIN_OBS_REASON_HW_POWER_BRAKE;
        out.hasThermalOrPowerThrottle = (out.limitReasonsMask & capMask) != 0;
        out.hasAppClockLimit = (out.limitReasonsMask & PIN_OBS_REASON_APPLICATIONS_CLOCKS) != 0;
    }
    if (out.activeSamples < PROFILE_PIN_OBS_MIN_SAMPLES) {
        finish(clockSamples > 0 ? PIN_OBS_IDLE : PIN_OBS_UNKNOWN,
            clockSamples > 0
                ? "GPU was idle or lacked sustained load (>=30% GPU load across >=3 samples); pin state remains unknown."
                : "No valid GPU clock samples were returned; pin state remains unknown.");
        return;
    }
    std::sort(activeClocks, activeClocks + out.activeSamples);
    out.minClockHz = activeClocks[0];
    out.maxClockHz = activeClocks[out.activeSamples - 1];
    out.medianClockHz = activeClocks[out.activeSamples / 2];
    if (utilCount > 0) out.avgUtilGpuPct = (unsigned int)(utilSum / (unsigned long long)utilCount);
    if (voltageCount > 0) {
        std::sort(activeVoltages, activeVoltages + voltageCount);
        out.operatingVoltageUv = activeVoltages[voltageCount / 2];
    }
    out.curveAllowedHz = profile_pin_observation_curve_ceiling(cap, out.operatingVoltageUv);
    if (out.curveAllowedHz <= 0) {
        finish(PIN_OBS_UNAVAILABLE, "Captured VF curve has no valid ceiling frequency; pin state remains unknown.");
        return;
    }
    if (out.maxClockHz - out.minClockHz > PROFILE_PIN_OBS_SPREAD_TOLERANCE_HZ) {
        char msg[320]{};
        snprintf(msg, sizeof(msg),
            "Observed clock varied %lld..%lld Hz (spread %lld Hz > %lld Hz) under load; clock does not appear pinned (unknown).",
            out.minClockHz, out.maxClockHz, out.maxClockHz - out.minClockHz, PROFILE_PIN_OBS_SPREAD_TOLERANCE_HZ);
        finish(PIN_OBS_NOT_PINNED_VARYING, msg);
        return;
    }
    if (out.medianClockHz > out.curveAllowedHz - PROFILE_PIN_OBS_CURVE_MARGIN_HZ) {
        char msg[320]{};
        snprintf(msg, sizeof(msg),
            "Sustained load clock %lld Hz is within %lld Hz of captured curve ceiling %lld Hz; no pin suspected (unknown).",
            out.medianClockHz, PROFILE_PIN_OBS_CURVE_MARGIN_HZ, out.curveAllowedHz);
        finish(PIN_OBS_AT_CURVE, msg);
        return;
    }
    const gc_i64 snapped = std::max(1000000LL, ((out.medianClockHz + 500000LL) / 1000000LL) * 1000000LL);
    out.proposedPinHz = snapped;
    char msg[512]{};
    snprintf(msg, sizeof(msg),
        "Suspected clock pin: sustained %lld Hz (proposed %lld MHz) at %u%% GPU load vs curve allowance %lld Hz"
        " (reasons: %s).%s Confirm or edit MHz before selecting PIN.",
        out.medianClockHz, snapped / 1000000LL, out.avgUtilGpuPct, out.curveAllowedHz,
        out.limitReasonsText[0] ? out.limitReasonsText : "unknown",
        out.hasThermalOrPowerThrottle ? " WARNING: driver reported power/thermal limit reason; verify this is an external clock pin." : "");
    finish(PIN_OBS_SUSPECTED_PIN, msg);
}
