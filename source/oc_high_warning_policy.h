// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_OC_HIGH_WARNING_POLICY_H
#define GREEN_CURVE_OC_HIGH_WARNING_POLICY_H

#include "platform.h"

// Pure decision for the "this is a high overclock, continue?" confirmation the
// GDI main window raises on a manual Apply.  Kept free of Win32 so the
// regression harness can pin every branch without a message loop.
//
// The whole point of this policy is to warn ONCE about a genuinely new risk and
// never nag, so three independent conditions must all hold per domain:
//
//   1. hand-typed  - a value that came from a profile load is the user's own
//                    saved intent, already reviewed when it was saved.  Only
//                    freshly typed values are surprising.
//   2. >= threshold - the configured danger line (0 disables the domain).
//   3. > current    - the value must raise the clock beyond what is already
//                    running.  Re-applying or lowering an existing high clock
//                    reduces risk and must stay silent.
//
// Automation (auto-profile rules, hotkeys, tray picks, logon/app-launch) never
// consults this policy at all: those paths queue their mutations directly and
// are required to stay presentation-silent.

enum {
    OC_HIGH_WARN_DEFAULT_GPU_OFFSET_MHZ = 200,
    OC_HIGH_WARN_DEFAULT_MEM_OFFSET_MHZ = 2000,
};

// A threshold <= 0 disables warnings for that domain.
struct OcHighWarnThresholds {
    int gpuOffsetMHz;
    int memOffsetMHz;
};

struct OcHighWarnInputs {
    bool hasGpuOffset;
    bool gpuHandTyped;
    int gpuOffsetMHz;
    int currentGpuOffsetMHz;
    bool hasMemOffset;
    bool memHandTyped;
    int memOffsetMHz;
    int currentMemOffsetMHz;
};

struct OcHighWarnDecision {
    bool warn;
    bool gpu;
    bool mem;
};

static inline OcHighWarnThresholds oc_high_warn_default_thresholds(void) {
    OcHighWarnThresholds thresholds = {};
    thresholds.gpuOffsetMHz = OC_HIGH_WARN_DEFAULT_GPU_OFFSET_MHZ;
    thresholds.memOffsetMHz = OC_HIGH_WARN_DEFAULT_MEM_OFFSET_MHZ;
    return thresholds;
}

static inline bool oc_high_warn_domain_triggers(bool present, bool handTyped,
                                                int value, int current,
                                                int threshold) {
    if (!present || !handTyped) return false;
    if (threshold <= 0) return false;
    if (value < threshold) return false;
    return value > current;
}

static inline OcHighWarnDecision oc_high_warn_decide(
    const OcHighWarnInputs* in, const OcHighWarnThresholds* thresholds) {
    OcHighWarnDecision decision = {};
    if (!in || !thresholds) return decision;
    decision.gpu = oc_high_warn_domain_triggers(
        in->hasGpuOffset, in->gpuHandTyped, in->gpuOffsetMHz,
        in->currentGpuOffsetMHz, thresholds->gpuOffsetMHz);
    decision.mem = oc_high_warn_domain_triggers(
        in->hasMemOffset, in->memHandTyped, in->memOffsetMHz,
        in->currentMemOffsetMHz, thresholds->memOffsetMHz);
    decision.warn = decision.gpu || decision.mem;
    return decision;
}

// One dialog names every domain that triggered, so a run that raises both the
// core and the memory clock still asks exactly once.
static inline void oc_high_warn_format_message(
    char* out, size_t outSize, const OcHighWarnDecision* decision,
    const OcHighWarnInputs* in, const OcHighWarnThresholds* thresholds) {
    if (!out || outSize == 0) return;
    out[0] = '\0';
    if (!decision || !in || !thresholds || !decision->warn) return;
    size_t used = 0;
    if (decision->gpu) {
        used = gc_appendf(out, outSize, used,
                          "GPU offset %+d MHz is a high overclock (warning "
                          "threshold %d MHz, currently applied %+d MHz).\n",
                          in->gpuOffsetMHz, thresholds->gpuOffsetMHz,
                          in->currentGpuOffsetMHz);
    }
    if (decision->mem) {
        used = gc_appendf(out, outSize, used,
                          "Memory offset %+d MHz is a high overclock (warning "
                          "threshold %d MHz, currently applied %+d MHz).\n",
                          in->memOffsetMHz, thresholds->memOffsetMHz,
                          in->currentMemOffsetMHz);
    }
    gc_appendf(out, outSize, used,
               "\nUnstable clocks can hang or crash the GPU driver and the "
               "system, and memory errors can corrupt data silently.\n\n"
               "Apply anyway?");
}

#endif // GREEN_CURVE_OC_HIGH_WARNING_POLICY_H
