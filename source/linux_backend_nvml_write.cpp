// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_backend_nvml_write.cpp — the NVML write helpers of the Linux backend
// (clock-domain offsets, power limit, fan policy/duty) plus the range queries
// they depend on.  Split out of linux_backend.cpp to keep that file under the
// project size guideline; it is #included there, not compiled separately, and
// shares its statics (lb_log, the LinuxGpuState/NvmlApi types).
//
// Everything here follows one rule the hardware forced on us twice: a driver
// readback is not required to equal what was written.  Clock offsets snap to a
// per-domain grid, and a manual fan duty is echoed as *intent* while the
// measured tachometer value lags or reads zero.  Verify against the right
// readback, tolerate a documented snap, and log it.

// ports nvml_set_clock_offset_domain(): set a clock-domain offset in MHz.
// Shared readback gate: accepts the driver's grid-snapped value (see
// nvml_clock_offset_readback_matches) and logs it, so a snap is diagnosable
// instead of surfacing as an unexplained rolled-back apply.
static bool nvml_clock_offset_verified(unsigned int domain, int requested,
                                       int readback, const char* via) {
    int step = nvml_clock_offset_grid_step(domain);
    if (!nvml_clock_offset_readback_matches(requested, readback, step)) {
        lb_log("offset: %s domain=%u requested=%d readback=%d rejected "
               "(grid step %d)\n", via, domain, requested, readback, step);
        return false;
    }
    if (readback != requested)
        lb_log("offset: %s domain=%u requested=%d snapped by driver to %d "
               "(grid step %d); accepted\n", via, domain, requested, readback,
               step);
    return true;
}

struct NvmlClockOffsetReadback {
    bool offsetValid;
    bool rangeValid;
    bool modernApi;
    int offsetMHz;
    int minMHz;
    int maxMHz;
};

// Prefer the current NVML P-state-scoped API at the configured P0 slot. The
// deprecated domain-global getters remain a compatibility fallback for older
// drivers, but are no longer required for capture/rollback availability.
static NvmlClockOffsetReadback nvml_read_clock_offset(
    LinuxGpuState* g, unsigned int domain) {
    NvmlClockOffsetReadback readback = {};
    if (!g) return readback;
    NvmlApi* a = &g->nvml;
    if (a->getClockOffsets) {
        nvmlClockOffset_t info = {};
        info.version = nvmlClockOffset_v1;
        info.type = domain;
        info.pstate = nvml_configured_clock_offset_pstate();
        if (a->getClockOffsets(g->nvmlDevice, &info) == NVML_SUCCESS) {
            readback.offsetValid = true;
            readback.rangeValid = true;
            readback.modernApi = true;
            readback.offsetMHz = info.clockOffsetMHz;
            readback.minMHz = info.minClockOffsetMHz;
            readback.maxMHz = info.maxClockOffsetMHz;
            g->offsetReadPstate = (int)info.pstate;
            return readback;
        }
    }

    nvmlReturn_t rangeResult = NVML_ERROR_NOT_SUPPORTED;
    nvmlReturn_t offsetResult = NVML_ERROR_NOT_SUPPORTED;
    if (domain == NVML_CLOCK_GRAPHICS) {
        if (a->getGpcClkMinMaxVfOffset) {
            rangeResult = a->getGpcClkMinMaxVfOffset(
                g->nvmlDevice, &readback.minMHz, &readback.maxMHz);
        }
        if (a->getGpcClkVfOffset) {
            offsetResult = a->getGpcClkVfOffset(
                g->nvmlDevice, &readback.offsetMHz);
        }
    } else if (domain == NVML_CLOCK_MEM) {
        if (a->getMemClkMinMaxVfOffset) {
            rangeResult = a->getMemClkMinMaxVfOffset(
                g->nvmlDevice, &readback.minMHz, &readback.maxMHz);
        }
        if (a->getMemClkVfOffset) {
            offsetResult = a->getMemClkVfOffset(
                g->nvmlDevice, &readback.offsetMHz);
        }
    }
    readback.rangeValid = rangeResult == NVML_SUCCESS;
    readback.offsetValid = offsetResult == NVML_SUCCESS;
    return readback;
}

static bool nvml_set_clock_offset(LinuxGpuState* g, unsigned int domain, int offsetMHz) {
    NvmlApi* a = &g->nvml;
    if (a->setClockOffsets) {
        nvmlClockOffset_t info = {};
        info.version = nvmlClockOffset_v1;
        info.type = domain;
        info.pstate = nvml_configured_clock_offset_pstate();
        info.clockOffsetMHz = offsetMHz;
        nvmlReturn_t result = a->setClockOffsets(g->nvmlDevice, &info);
        lb_log("offset: write domain=%u pstate=P%u requested=%d "
               "via=setClockOffsets nvml=%d\n",
               domain, info.pstate, offsetMHz, (int)result);
        if (result == NVML_SUCCESS) {
            NvmlClockOffsetReadback verify =
                nvml_read_clock_offset(g, domain);
            if (!verify.offsetValid) {
                lb_log("offset: verify domain=%u pstate=P%u requested=%d "
                       "readback=unavailable\n",
                       domain, info.pstate, offsetMHz);
                return false;
            }
            lb_log("offset: verify domain=%u pstate=P%u requested=%d "
                   "readback=%d via=%s\n",
                   domain, info.pstate, offsetMHz, verify.offsetMHz,
                   verify.modernApi ? "getClockOffsets" : "legacy");
            return nvml_clock_offset_verified(
                domain, offsetMHz, verify.offsetMHz, "setClockOffsets");
        }
    }
    if (domain == NVML_CLOCK_GRAPHICS && a->setGpcClkVfOffset) {
        if (a->setGpcClkVfOffset(g->nvmlDevice, offsetMHz) != NVML_SUCCESS) return false;
        NvmlClockOffsetReadback verify = nvml_read_clock_offset(g, domain);
        lb_log("offset: write domain=%u pstate=global requested=%d "
               "readback=%s%d via=setGpcClkVfOffset\n",
               domain, offsetMHz, verify.offsetValid ? "" : "unavailable:",
               verify.offsetMHz);
        return !verify.offsetValid ||
               nvml_clock_offset_verified(domain, offsetMHz, verify.offsetMHz,
                                          "setGpcClkVfOffset");
    }
    if (domain == NVML_CLOCK_MEM && a->setMemClkVfOffset) {
        if (a->setMemClkVfOffset(g->nvmlDevice, offsetMHz) != NVML_SUCCESS) return false;
        NvmlClockOffsetReadback verify = nvml_read_clock_offset(g, domain);
        lb_log("offset: write domain=%u pstate=global requested=%d "
               "readback=%s%d via=setMemClkVfOffset\n",
               domain, offsetMHz, verify.offsetValid ? "" : "unavailable:",
               verify.offsetMHz);
        return !verify.offsetValid ||
               nvml_clock_offset_verified(domain, offsetMHz, verify.offsetMHz,
                                          "setMemClkVfOffset");
    }
    return false;
}

static bool nvml_set_power_limit_pct(LinuxGpuState* g, int pct) {
    NvmlApi* a = &g->nvml;
    if (!a->setPowerLimit) return false;
    int defmW = g->powerLimitDefaultmW;
    if (defmW <= 0 && a->getPowerDefaultLimit) {
        unsigned int d = 0;
        if (a->getPowerDefaultLimit(g->nvmlDevice, &d) == NVML_SUCCESS) defmW = (int)d;
    }
    if (defmW <= 0) return false;
    long target = (long)defmW * pct / 100;
    if (g->powerLimitMinmW > 0 && target < g->powerLimitMinmW) target = g->powerLimitMinmW;
    if (g->powerLimitMaxmW > 0 && target > g->powerLimitMaxmW) target = g->powerLimitMaxmW;
    nvmlReturn_t r = a->setPowerLimit(g->nvmlDevice, (unsigned int)target);
    lb_log("power: set %d%% -> %ld mW ret=%d\n", pct, target, (int)r);
    if (r != NVML_SUCCESS) return false;
    unsigned int verify = 0;
    return !a->getPowerLimit ||
           (a->getPowerLimit(g->nvmlDevice, &verify) == NVML_SUCCESS && verify == (unsigned int)target);
}

// Read the duty the driver says it intends to hold for `fan`.  Returns false
// when the driver exposes no intent getter, which the caller must distinguish
// from an intent of 0 — see fan_manual_write_confirmed().
static bool nvml_read_fan_intent(LinuxGpuState* g, unsigned int fan, int* intendedPercent) {
    NvmlApi* a = &g->nvml;
    if (!a->getTargetFanSpeed) return false;
    unsigned int target = 0;
    nvmlReturn_t r = a->getTargetFanSpeed(g->nvmlDevice, fan, &target);
    if (r != NVML_SUCCESS) {
        lb_log("fan: intent read failed for fan %u (nvml=%d)\n", fan, (int)r);
        return false;
    }
    if (intendedPercent) *intendedPercent = (int)target;
    return true;
}

static int nvml_read_fan_measured(LinuxGpuState* g, unsigned int fan) {
    NvmlApi* a = &g->nvml;
    unsigned int measured = 0;
    if (!a->getFanSpeed || a->getFanSpeed(g->nvmlDevice, fan, &measured) != NVML_SUCCESS)
        return -1;
    return (int)measured;
}

static bool nvml_set_fan(LinuxGpuState* g, int fanMode, bool fanAuto, int fanPercent) {
    NvmlApi* a = &g->nvml;
    unsigned int numFans = 0;
    if (!a->getNumFans || a->getNumFans(g->nvmlDevice, &numFans) != NVML_SUCCESS ||
        numFans == 0)
        return false;
    if (numFans > MAX_GPU_FANS) numFans = MAX_GPU_FANS;
    bool ok = true;
    if (fanAuto || fanMode == FAN_MODE_AUTO) {
        for (unsigned int f = 0; f < numFans; f++) {
            bool fanOk = false;
            if (a->setDefaultFanSpeed)
                fanOk = a->setDefaultFanSpeed(g->nvmlDevice, f) == NVML_SUCCESS;
            else if (a->setFanControlPolicy)
                fanOk = a->setFanControlPolicy(g->nvmlDevice, f,
                    NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW) == NVML_SUCCESS;
            unsigned int policy = 0;
            fanOk = fanOk && a->getFanControlPolicy &&
                    a->getFanControlPolicy(g->nvmlDevice, f, &policy) == NVML_SUCCESS &&
                    policy == NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW;
            if (!fanOk) lb_log("fan: auto restore failed for fan %u\n", f);
            ok &= fanOk;
        }
        // Handing the fan back ends the manual episode: the next manual duty is
        // a transition again and must be logged.
        g->fanWriteLogged = false;
        return ok;
    }
    // Fixed (and the initial set for curve mode; the daemon reasserts curve).
    if (!a->setFanSpeed) return false;
    int requestedPercent = fanPercent;
    int effectivePercent = fan_manual_effective_percent(
        requestedPercent, g->fanMinPct, g->fanMaxPct, g->fanRangeKnown);
    for (unsigned int f = 0; f < numFans; f++) {
        // Mirror the Windows order: take manual policy first, then set the
        // duty.  nvmlDeviceSetFanSpeed_v2 flips the policy by itself on current
        // drivers, but an explicit, verified policy write keeps the failure
        // attributable when it does not.
        if (a->setFanControlPolicy) {
            nvmlReturn_t pr = a->setFanControlPolicy(g->nvmlDevice, f, NVML_FAN_POLICY_MANUAL);
            if (pr != NVML_SUCCESS)
                lb_log("fan: manual policy write failed for fan %u (nvml=%d); "
                       "relying on the speed write to switch policy\n", f, (int)pr);
        }
        nvmlReturn_t sr = a->setFanSpeed(g->nvmlDevice, f, (unsigned int)effectivePercent);
        bool fanOk = sr == NVML_SUCCESS;
        int intended = 0;
        bool intendedKnown = false;
        int measured = -1;
        if (fanOk) {
            intendedKnown = nvml_read_fan_intent(g, f, &intended);
            measured = nvml_read_fan_measured(g, f);
            // The measured duty is telemetry: it reads 0 for as long as the
            // zero-RPM fan stop holds and overshoots while the fan spins up.
            // Verifying against it rejected every accepted write on an idle
            // GPU, which is what broke manual fan control on Linux outright.
            fanOk = fan_manual_write_confirmed(effectivePercent,
                measured < 0 ? 0 : measured, intended, intendedKnown);
        }
        if (!fanOk) {
            lb_log("fan: fixed write/readback failed for fan %u "
                   "(set=%d nvml=%d intent=%s%d measured=%d)\n",
                   f, effectivePercent, (int)sr,
                   intendedKnown ? "" : "unknown:", intended, measured);
        }
        ok &= fanOk;
    }
    // Transition-only: a failure, a recovery, or a new duty is logged; the
    // per-poll curve re-assertion of an unchanged duty is not.  The gate is on
    // the *effective* duty, so a curve wandering across 27/28/29% while the
    // driver floor pins all three at 30% does not produce a line per poll —
    // nothing about the hardware state changed.
    if (!g->fanWriteLogged || g->lastLoggedFanPercent != effectivePercent ||
        g->lastLoggedFanOk != ok) {
        if (effectivePercent != requestedPercent) {
            lb_log("fan: set fixed %d%% across %u fan(s) ok=%d "
                   "(requested %d%%, clamped by the driver range %d..%d%%)\n",
                   effectivePercent, numFans, ok ? 1 : 0, requestedPercent,
                   g->fanMinPct, g->fanMaxPct);
        } else {
            lb_log("fan: set fixed %d%% across %u fan(s) ok=%d\n", effectivePercent, numFans, ok ? 1 : 0);
        }
        g->lastLoggedFanPercent = effectivePercent;
        g->lastLoggedFanOk = ok;
        g->fanWriteLogged = true;
    }
    return ok;
}

static void nvml_query_ranges(LinuxGpuState* g) {
    NvmlApi* a = &g->nvml;
    NvmlClockOffsetReadback gpu =
        nvml_read_clock_offset(g, NVML_CLOCK_GRAPHICS);
    if (gpu.rangeValid) {
        g->gpuOffsetMinMHz = gpu.minMHz;
        g->gpuOffsetMaxMHz = gpu.maxMHz;
        if (!g->curveOffsetRangeKnown) {
            g->curveOffsetMinKHz = gpu.minMHz * 1000;
            g->curveOffsetMaxKHz = gpu.maxMHz * 1000;
            g->curveOffsetRangeKnown = true;
        }
    }
    NvmlClockOffsetReadback memory =
        nvml_read_clock_offset(g, NVML_CLOCK_MEM);
    if (memory.rangeValid) {
        g->memOffsetMinMHz = memory.minMHz;
        g->memOffsetMaxMHz = memory.maxMHz;
    }
    if (a->getPowerConstraints) {
        unsigned int pmin = 0, pmax = 0;
        if (a->getPowerConstraints(g->nvmlDevice, &pmin, &pmax) == NVML_SUCCESS) {
            g->powerLimitMinmW = (int)pmin; g->powerLimitMaxmW = (int)pmax;
        }
    }
    if (a->getPowerDefaultLimit) {
        unsigned int d = 0;
        if (a->getPowerDefaultLimit(g->nvmlDevice, &d) == NVML_SUCCESS) g->powerLimitDefaultmW = (int)d;
    }
    if (a->getMinMaxFanSpeed) {
        // Logged only on a transition: nvml_query_ranges() runs on every
        // backend refresh, and an unconditional line here would bury the
        // journal under one duplicate per telemetry poll (F-15-011).
        unsigned int fmin = 0, fmax = 0;
        bool known = a->getMinMaxFanSpeed(g->nvmlDevice, &fmin, &fmax) == NVML_SUCCESS &&
                     fmax > 0 && fmax <= 100 && fmin <= fmax;
        bool changed = known != g->fanRangeKnown ||
                       (known && ((int)fmin != g->fanMinPct || (int)fmax != g->fanMaxPct));
        if (known) {
            g->fanMinPct = (int)fmin;
            g->fanMaxPct = (int)fmax;
        }
        g->fanRangeKnown = known;
        if (changed) {
            if (known) lb_log("fan: driver duty range %u..%u%%\n", fmin, fmax);
            else lb_log("fan: driver duty range unavailable; manual writes are unclamped\n");
        }
    }
}
