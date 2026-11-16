// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Read-only bounded NVML workload sampler for Tier 3 capture pin observation
// (§8.5). Runs only on explicit user demand when Tier 1 (validated getter) and
// Tier 2 (Green Curve applied intent) did not resolve pin state. Uses only
// read-only NVML getters matched to `binding` and performs no GPU writes,
// configuration edits, sleeps, or global state mutations.
#pragma once
#include "profile_pin_observation.h"
#include <vector>

template <class OpenLib, class ResolveSym, class CloseLib>
static inline bool nvml_pin_observation_sample_burst(
    const HardwareCaptureBinding& binding,
    int requestedSamples,
    std::vector<PinWorkloadSample>& outSamples,
    char* error, size_t errorSize,
    OpenLib openLib, ResolveSym resolveSym, CloseLib closeLib) {
    outSamples.clear();
    if (error && errorSize) error[0] = 0;
    if (!binding.valid) {
        if (error && errorSize) snprintf(error, errorSize, "no valid captured GPU identity for workload observation");
        return false;
    }
    auto lib = openLib();
    if (!lib) {
        if (error && errorSize) snprintf(error, errorSize, "NVML library unavailable for read-only pin observation");
        return false;
    }
    using PciInfoFn = nvmlReturn_t (*)(nvmlDevice_t, nvmlPciInfo_t*);
    using ThrottleReasonsFn = nvmlReturn_t (*)(nvmlDevice_t, unsigned long long*);
    const auto resolve = [&](auto& fn, const char* name) -> bool {
        fn = reinterpret_cast<std::decay_t<decltype(fn)>>(resolveSym(lib, name));
        return fn != nullptr;
    };
    nvmlInit_v2_t init = nullptr;
    nvmlShutdown_t shutdown = nullptr;
    nvmlDeviceGetCount_v2_t getCount = nullptr;
    nvmlDeviceGetHandleByIndex_v2_t getByIndex = nullptr;
    PciInfoFn getPci = nullptr;
    nvmlDeviceGetClock_t getClock = nullptr;
    nvmlDeviceGetUtilizationRates_t getUtil = nullptr;
    nvmlDeviceGetPerformanceState_t getPstate = nullptr;
    nvmlDeviceGetPowerUsage_t getPower = nullptr;
    ThrottleReasonsFn getReasons = nullptr;

    resolve(init, "nvmlInit_v2");
    resolve(shutdown, "nvmlShutdown");
    resolve(getCount, "nvmlDeviceGetCount_v2");
    resolve(getByIndex, "nvmlDeviceGetHandleByIndex_v2");
    if (!resolve(getPci, "nvmlDeviceGetPciInfo_v3") && !resolve(getPci, "nvmlDeviceGetPciInfo_v2"))
        resolve(getPci, "nvmlDeviceGetPciInfo");
    resolve(getClock, "nvmlDeviceGetClock");
    resolve(getUtil, "nvmlDeviceGetUtilizationRates");
    resolve(getPstate, "nvmlDeviceGetPerformanceState");
    resolve(getPower, "nvmlDeviceGetPowerUsage");
    if (!resolve(getReasons, "nvmlDeviceGetCurrentClocksEventReasons"))
        resolve(getReasons, "nvmlDeviceGetCurrentClocksThrottleReasons");

    if (!init || !getCount || !getByIndex || !getClock) {
        closeLib(lib);
        if (error && errorSize) snprintf(error, errorSize, "NVML read-only clock telemetry entry points unavailable");
        return false;
    }
    if (init() != NVML_SUCCESS) {
        closeLib(lib);
        if (error && errorSize) snprintf(error, errorSize, "NVML initialization failed for workload observation");
        return false;
    }
    nvmlDevice_t target = nullptr;
    unsigned int count = 0;
    if (getCount(&count) == NVML_SUCCESS && count > 0) {
        if (binding.pciInfoValid && getPci) {
            for (unsigned int idx = 0; idx < count; ++idx) {
                nvmlDevice_t dev = nullptr;
                nvmlPciInfo_t pci{};
                if (getByIndex(idx, &dev) == NVML_SUCCESS && dev && getPci(dev, &pci) == NVML_SUCCESS) {
                    if (pci.domain == binding.pciDomain && pci.bus == binding.pciBus && pci.device == binding.pciDevice) {
                        target = dev;
                        break;
                    }
                }
            }
        } else if (!binding.pciInfoValid && binding.nvmlIndex < count) {
            nvmlDevice_t dev = nullptr;
            if (getByIndex(binding.nvmlIndex, &dev) == NVML_SUCCESS) target = dev;
        }
    }
    if (!target) {
        if (shutdown) (void)shutdown();
        closeLib(lib);
        if (error && errorSize) snprintf(error, errorSize, "captured GPU did not match an NVML device");
        return false;
    }

    const int n = std::clamp(requestedSamples, PROFILE_PIN_OBS_MIN_SAMPLES, PROFILE_PIN_OBS_MAX_SAMPLES);
    outSamples.reserve((size_t)n);
    for (int i = 0; i < n; ++i) {
        PinWorkloadSample sample{};
        unsigned int mhz = 0;
        if (getClock(target, NVML_CLOCK_GRAPHICS, NVML_CLOCK_ID_CURRENT, &mhz) == NVML_SUCCESS && mhz > 0) {
            sample.clockValid = true;
            sample.graphicsClockHz = (gc_i64)mhz * 1000000LL;
        }
        nvmlUtilization_t util{};
        if (getUtil && getUtil(target, &util) == NVML_SUCCESS) {
            sample.utilValid = true;
            sample.utilGpuPct = util.gpu;
            sample.utilMemPct = util.memory;
        }
        unsigned int pstate = 0;
        if (getPstate && getPstate(target, &pstate) == NVML_SUCCESS) {
            sample.perfStateValid = true;
            sample.perfState = pstate;
        }
        unsigned int mw = 0;
        if (getPower && getPower(target, &mw) == NVML_SUCCESS && mw > 0) {
            sample.powerValid = true;
            sample.powerMw = mw;
        }
        unsigned long long reasons = 0;
        if (getReasons && getReasons(target, &reasons) == NVML_SUCCESS) {
            sample.throttleValid = true;
            sample.throttleReasons = (gc_u64)reasons;
        }
        outSamples.push_back(sample);
    }

    if (shutdown) (void)shutdown();
    closeLib(lib);
    return !outSamples.empty();
}
