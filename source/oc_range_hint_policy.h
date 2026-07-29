// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_OC_RANGE_HINT_POLICY_H
#define GREEN_CURVE_OC_RANGE_HINT_POLICY_H

#include "platform.h"

// Pure derivation and formatting of the overclock ranges the Windows GDI main
// window advertises next to the GPU offset / memory offset / power limit
// fields.  Kept free of Win32 so the regression harness can exercise every
// bound and every message without creating a window.
//
// The driver-reported ranges are NOT the whole truth: every request is also
// clamped at the service IPC boundary by validate_desired_settings_for_ipc()
// (source/gpu_core.h) and the power limit is additionally gated to 50..150% in
// gpu_backend_apply.cpp.  What the user actually gets is the INTERSECTION, so
// that is what the hint must show.  Advertising the raw driver range would
// promise values the apply path then rejects.

enum {
    // Mirrors validate_desired_settings_for_ipc(); see source/gpu_core.h.
    OC_RANGE_IPC_GPU_OFFSET_ABS_MHZ = 1000,
    OC_RANGE_IPC_MEM_OFFSET_ABS_MHZ = 5000,
    // Mirrors the power gate in gpu_backend_apply.cpp and the INI loader.
    OC_RANGE_POWER_MIN_PCT = 50,
    OC_RANGE_POWER_MAX_PCT = 150,
};

// A straight copy of the g_app range fields (source/app_shared.h), which the
// service snapshot already fills for every frontend.  Memory bounds are in the
// same display MHz the edit field uses (Windows halves the NVML effective-MHz
// figure in mem_display_mhz_from_driver_mhz()).
struct OcRangeInputs {
    bool gpuKnown;
    int gpuMinMHz;
    int gpuMaxMHz;
    bool memKnown;
    int memMinMHz;
    int memMaxMHz;
    int powerMinmW;
    int powerMaxmW;
    int powerDefaultmW;
};

struct OcRangeBounds {
    bool known;
    int min;
    int max;
};

static inline int oc_range_clamp_int(int value, int lo, int hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static inline OcRangeBounds oc_range_make_bounds(bool known, int lo, int hi,
                                                 int absCap) {
    OcRangeBounds bounds = {};
    if (!known) return bounds;
    // A driver that reports an inverted or empty window tells us nothing
    // usable; treat it as unknown rather than printing a nonsense interval.
    if (lo > hi) return bounds;
    bounds.known = true;
    bounds.min = oc_range_clamp_int(lo, -absCap, absCap);
    bounds.max = oc_range_clamp_int(hi, -absCap, absCap);
    return bounds;
}

static inline OcRangeBounds oc_range_gpu_offset(const OcRangeInputs* in) {
    OcRangeBounds bounds = {};
    if (!in) return bounds;
    return oc_range_make_bounds(in->gpuKnown, in->gpuMinMHz, in->gpuMaxMHz,
                                OC_RANGE_IPC_GPU_OFFSET_ABS_MHZ);
}

static inline OcRangeBounds oc_range_mem_offset(const OcRangeInputs* in) {
    OcRangeBounds bounds = {};
    if (!in) return bounds;
    return oc_range_make_bounds(in->memKnown, in->memMinMHz, in->memMaxMHz,
                                OC_RANGE_IPC_MEM_OFFSET_ABS_MHZ);
}

// Percent is derived, never reported: the driver exposes power constraints in
// milliwatts relative to the card's default limit.  Round the minimum up and
// the maximum down so every advertised percent is actually accepted, then
// intersect with the enforced 50..150 gate.  When the driver reports no
// constraints the enforced gate IS the answer, so the range stays known.
static inline OcRangeBounds oc_range_power_pct(const OcRangeInputs* in) {
    OcRangeBounds bounds = {};
    bounds.known = true;
    bounds.min = OC_RANGE_POWER_MIN_PCT;
    bounds.max = OC_RANGE_POWER_MAX_PCT;
    if (!in) return bounds;
    if (in->powerDefaultmW <= 0 || in->powerMinmW <= 0 || in->powerMaxmW <= 0)
        return bounds;
    if (in->powerMinmW > in->powerMaxmW) return bounds;
    long long def = (long long)in->powerDefaultmW;
    long long lo = ((long long)in->powerMinmW * 100 + def - 1) / def;  // ceil
    long long hi = ((long long)in->powerMaxmW * 100) / def;           // floor
    if (lo < OC_RANGE_POWER_MIN_PCT) lo = OC_RANGE_POWER_MIN_PCT;
    if (hi > OC_RANGE_POWER_MAX_PCT) hi = OC_RANGE_POWER_MAX_PCT;
    // A card whose entire driver window falls outside the enforced gate leaves
    // an empty interval; keep the enforced gate rather than an inverted one.
    if (lo > hi) return bounds;
    bounds.min = (int)lo;
    bounds.max = (int)hi;
    return bounds;
}

static inline size_t oc_range_append_bounds(char* out, size_t outSize,
                                            size_t used,
                                            const OcRangeBounds* bounds,
                                            const char* unit) {
    if (!bounds || !bounds->known)
        return gc_appendf(out, outSize, used, "unknown (driver did not report)");
    return gc_appendf(out, outSize, used, "%+d..%+d %s", bounds->min,
                      bounds->max, unit ? unit : "");
}

// Tooltip bodies.  ASCII only: the GDI main window creates and writes its
// controls through the ANSI Win32 entry points, so non-ASCII text would render
// as mojibake on non-Latin codepages.
static inline void oc_range_format_gpu_tip(char* out, size_t outSize,
                                           const OcRangeInputs* in) {
    if (!out || outSize == 0) return;
    out[0] = '\0';
    OcRangeBounds gpu = oc_range_gpu_offset(in);
    size_t used = gc_appendf(out, outSize, 0, "GPU core clock offset.\r\nSupported: ");
    used = oc_range_append_bounds(out, outSize, used, &gpu, "MHz");
    gc_appendf(out, outSize, used,
               "\r\nValues outside this range are rejected before anything is "
               "written to the GPU.");
}

static inline void oc_range_format_mem_tip(char* out, size_t outSize,
                                           const OcRangeInputs* in) {
    if (!out || outSize == 0) return;
    out[0] = '\0';
    OcRangeBounds mem = oc_range_mem_offset(in);
    size_t used = gc_appendf(out, outSize, 0,
                             "Memory clock offset in actual MHz (the effective "
                             "data rate moves twice as far).\r\nDriver hint: ");
    used = oc_range_append_bounds(out, outSize, used, &mem, "MHz");
    gc_appendf(out, outSize, used,
               "\r\nAdvisory only: NVIDIA reports a conservative memory range, "
               "so Green Curve applies values beyond it and lets the driver "
               "clamp or refuse them.");
}

static inline void oc_range_format_power_tip(char* out, size_t outSize,
                                             const OcRangeInputs* in) {
    if (!out || outSize == 0) return;
    out[0] = '\0';
    OcRangeBounds power = oc_range_power_pct(in);
    gc_appendf(out, outSize, 0,
               "Power limit as a percentage of the card's default board power."
               "\r\nSupported: %d..%d %%\r\nDerived from the driver's "
               "milliwatt constraints and capped to the enforced %d..%d %% "
               "gate.",
               power.min, power.max, OC_RANGE_POWER_MIN_PCT,
               OC_RANGE_POWER_MAX_PCT);
}

#endif // GREEN_CURVE_OC_RANGE_HINT_POLICY_H
