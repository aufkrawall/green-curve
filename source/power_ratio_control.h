// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_POWER_RATIO_CONTROL_H
#define GREEN_CURVE_POWER_RATIO_CONTROL_H

#include "gpu_core.h"
#include "tuning_units.h"
#include <limits.h>

struct PowerRatioSurface {
    bool currentValid, defaultValid, writable, rangeKnown, rangeMalformed;
    unsigned int currentMw, defaultMw, minimumMw, maximumMw;
    unsigned int gridMw; // API units; zero means no established destination grid
    int currentStatus, defaultStatus, rangeStatus;
};

enum PowerRatioControlStatus {
    POWER_RATIO_READY, POWER_RATIO_INERT, POWER_RATIO_INVALID,
    POWER_RATIO_UNAVAILABLE, POWER_RATIO_CONVERSION_REQUIRED,
    POWER_RATIO_OUT_OF_RANGE, POWER_RATIO_STATE_CHANGED,
    POWER_RATIO_WRITE_FAILED, POWER_RATIO_VERIFY_FAILED,
};

static inline const char* power_ratio_control_status_text(PowerRatioControlStatus s) {
    switch (s) {
        case POWER_RATIO_READY: return "ready";
        case POWER_RATIO_INERT: return "default surface unavailable";
        case POWER_RATIO_INVALID: return "invalid ratio or arithmetic overflow";
        case POWER_RATIO_UNAVAILABLE: return "control surface unavailable";
        case POWER_RATIO_CONVERSION_REQUIRED: return "explicit unit conversion required";
        case POWER_RATIO_OUT_OF_RANGE: return "outside destination bounds";
        case POWER_RATIO_STATE_CHANGED: return "controls changed before write";
        case POWER_RATIO_WRITE_FAILED: return "setter failed";
        case POWER_RATIO_VERIFY_FAILED: return "exact readback failed";
    }
    return "unknown result";
}

struct PowerRatioControlPlan {
    PowerRatioControlStatus status;
    unsigned int targetMw, defaultMw;
    long long lowerRepresentableMw;
    bool writeRequired, fallbackBounds;
};

// This plans against actual controls, never a rounded percentage preview.
// Fractional mW or off-grid targets need explicit downward conversion.
static inline PowerRatioControlPlan power_ratio_control_plan(TuningPowerRatio ratio,
    const PowerRatioSurface& s, bool forceWrite = false) {
    PowerRatioControlPlan p{};
    p.status = POWER_RATIO_INVALID;
    if (!tuning_power_ratio_valid(ratio)) return p;
    if (!s.currentValid || !s.defaultValid || !s.currentMw || !s.defaultMw || !s.writable) {
        p.status = !forceWrite && ratio.numerator == ratio.denominator
            ? POWER_RATIO_INERT : POWER_RATIO_UNAVAILABLE;
        return p;
    }
    p.defaultMw = s.defaultMw;
    const TuningPowerRatioResolution resolved = tuning_power_ratio_resolve(ratio, s.defaultMw);
    if (resolved.overflowed) return p;
    if (resolved.requestedMw < 1 || resolved.requestedMw > UINT_MAX) {
        p.status = POWER_RATIO_OUT_OF_RANGE;
        return p;
    }
    if (!s.gridMw) { p.status = POWER_RATIO_UNAVAILABLE; return p; }
    p.lowerRepresentableMw = (resolved.requestedMw / s.gridMw) * s.gridMw;
    if (!resolved.exact || resolved.resolvedMw % s.gridMw) {
        p.status = POWER_RATIO_CONVERSION_REQUIRED;
        return p;
    }
    p.targetMw = (unsigned int)resolved.resolvedMw;
    if (!forceWrite && p.targetMw == s.currentMw) { p.status = POWER_RATIO_READY; return p; }
    if (s.rangeMalformed) { p.status = POWER_RATIO_UNAVAILABLE; return p; }
    const long long minimum = s.rangeKnown ? (long long)s.minimumMw
        : ((long long)s.defaultMw * POWER_LIMIT_MIN_PCT + 99) / 100;
    const long long maximum = s.rangeKnown ? (long long)s.maximumMw
        : (long long)s.defaultMw * POWER_LIMIT_MAX_PCT / 100;
    p.fallbackBounds = !s.rangeKnown;
    if (p.targetMw < minimum || p.targetMw > maximum) { p.status = POWER_RATIO_OUT_OF_RANGE; return p; }
    p.status = POWER_RATIO_READY;
    p.writeRequired = true;
    return p;
}

static inline bool power_ratio_surface_equal(const PowerRatioSurface& a, const PowerRatioSurface& b,
    bool compareCurrent = true) {
    return a.currentValid == b.currentValid && a.defaultValid == b.defaultValid &&
        (!compareCurrent || !a.currentValid || a.currentMw == b.currentMw) &&
        (!a.defaultValid || a.defaultMw == b.defaultMw) && a.writable == b.writable &&
        a.rangeKnown == b.rangeKnown && a.rangeMalformed == b.rangeMalformed &&
        (!a.rangeKnown || (a.minimumMw == b.minimumMw && a.maximumMw == b.maximumMw)) &&
        a.gridMw == b.gridMw;
}

// Read-only; missing values remain missing and constraints stay independent.
static inline PowerRatioSurface power_ratio_read_nvml(const NvmlApi& a, nvmlDevice_t device) {
    PowerRatioSurface s{};
    s.currentStatus = s.defaultStatus = s.rangeStatus = NVML_ERROR_NOT_SUPPORTED;
    s.gridMw = 1;
    s.writable = a.setPowerLimit != nullptr;
    if (a.getPowerLimit) s.currentStatus = a.getPowerLimit(device, &s.currentMw);
    if (a.getPowerDefaultLimit) s.defaultStatus = a.getPowerDefaultLimit(device, &s.defaultMw);
    s.currentValid = s.currentStatus == NVML_SUCCESS && s.currentMw > 0;
    s.defaultValid = s.defaultStatus == NVML_SUCCESS && s.defaultMw > 0;
    if (a.getPowerConstraints) {
        s.rangeStatus = a.getPowerConstraints(device, &s.minimumMw, &s.maximumMw);
        if (s.rangeStatus == NVML_SUCCESS) {
            s.rangeKnown = s.minimumMw <= s.maximumMw && s.maximumMw > 0;
            s.rangeMalformed = !s.rangeKnown;
        }
    }
    return s;
}

struct PowerRatioControlWrite {
    bool attempted, accepted;
};
struct PowerRatioControlResult {
    PowerRatioControlPlan plan;
    PowerRatioSurface after;
    bool success, writeAttempted;
};

// Two bounded pre-reads detect changing controls/defaults before mutation.
// Read and write adapters own platform I/O; parsing/planning never calls write.
template<class Read, class Write>
static inline PowerRatioControlResult power_ratio_control_apply(TuningPowerRatio ratio,
    Read read, Write write, bool forceWrite = false, unsigned int expectedDefaultMw = 0) {
    PowerRatioControlResult r{};
    r.plan.status = POWER_RATIO_INVALID;
    if (!tuning_power_ratio_valid(ratio)) return r;
    const PowerRatioSurface first = read();
    const PowerRatioSurface before = read();
    r.after = before;
    r.plan = power_ratio_control_plan(ratio, before, forceWrite);
    if (r.plan.status == POWER_RATIO_INERT) { r.success = true; return r; }
    if (!power_ratio_surface_equal(first, before) ||
        (expectedDefaultMw && before.defaultValid && before.defaultMw != expectedDefaultMw)) {
        r.plan.status = POWER_RATIO_STATE_CHANGED;
        return r;
    }
    if (r.plan.status != POWER_RATIO_READY) return r;
    if (!r.plan.writeRequired) { r.success = true; return r; }
    const PowerRatioControlWrite written = write(r.plan.targetMw);
    r.writeAttempted = written.attempted;
    if (!written.accepted) {
        r.plan.status = POWER_RATIO_WRITE_FAILED;
        if (written.attempted) r.after.currentValid = false;
        return r;
    }
    r.after = read();
    if (!r.after.currentValid || !r.after.defaultValid || r.after.currentMw != r.plan.targetMw ||
        !power_ratio_surface_equal(before, r.after, false)) {
        r.plan.status = POWER_RATIO_VERIFY_FAILED;
        return r;
    }
    r.success = true;
    return r;
}
#endif
