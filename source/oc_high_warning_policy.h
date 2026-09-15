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
    // F-05-001. The XBAR MSVDD rail offset is written through a private,
    // undocumented NvAPI interface at a reverse-engineered field offset, and
    // voltage is the one domain here that can damage silicon rather than merely
    // destabilise it -- yet it was the only OC domain with no confirmation at
    // all, while a +200 MHz core offset had one. 25 mV sits above the +10 mV the
    // XBAR dialog's own hint recommends starting at and below a vendor-typical
    // safe step, so a cautious first try stays silent and a large hand-typed
    // jump does not.
    OC_HIGH_WARN_DEFAULT_MSVDD_OFFSET_MV = 25,
};

// A threshold <= 0 disables warnings for that domain.
struct OcHighWarnThresholds {
    int gpuOffsetMHz;
    int memOffsetMHz;
    // Millivolts, matching the units the XBAR dialog edits in.
    int msvddOffsetMv;
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
    bool hasMsvddOffset;
    bool msvddHandTyped;
    int msvddOffsetMv;
    int currentMsvddOffsetMv;
};

struct OcHighWarnDecision {
    bool warn;
    bool gpu;
    bool mem;
    bool msvdd;
};

static inline OcHighWarnThresholds oc_high_warn_default_thresholds(void) {
    OcHighWarnThresholds thresholds = {};
    thresholds.gpuOffsetMHz = OC_HIGH_WARN_DEFAULT_GPU_OFFSET_MHZ;
    thresholds.memOffsetMHz = OC_HIGH_WARN_DEFAULT_MEM_OFFSET_MHZ;
    thresholds.msvddOffsetMv = OC_HIGH_WARN_DEFAULT_MSVDD_OFFSET_MV;
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
    decision.msvdd = oc_high_warn_domain_triggers(
        in->hasMsvddOffset, in->msvddHandTyped, in->msvddOffsetMv,
        in->currentMsvddOffsetMv, thresholds->msvddOffsetMv);
    decision.warn = decision.gpu || decision.mem || decision.msvdd;
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
    if (decision->msvdd) {
        used = gc_appendf(out, outSize, used,
                          "XBAR voltage offset %+d mV is a high overvolt "
                          "(warning threshold %d mV, currently applied %+d mV).\n",
                          in->msvddOffsetMv, thresholds->msvddOffsetMv,
                          in->currentMsvddOffsetMv);
    }
    used = gc_appendf(out, outSize, used,
               "\nUnstable clocks can hang or crash the GPU driver and the "
               "system, and memory errors can corrupt data silently.\n");
    if (decision->msvdd) {
        // Stated separately because it is a different KIND of risk: the other
        // two domains threaten stability, this one threatens the hardware, and
        // it is written through an interface the vendor does not document.
        used = gc_appendf(out, outSize, used,
               "Raising a rail voltage stresses the GPU beyond its validated "
               "operating point and can damage it permanently.\n");
    }
    gc_appendf(out, outSize, used, "\nApply anyway?");
}

#endif // GREEN_CURVE_OC_HIGH_WARNING_POLICY_H
