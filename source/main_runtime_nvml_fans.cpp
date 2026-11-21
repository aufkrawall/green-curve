// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// nvml_read_fans(): one complete read of every fan (count, duty range,
// policy, intended and measured duty, tach, cooler signal) with per-fan
// validity, split out of main_runtime_nvml.cpp (size ratchet), which
// includes it in place.

static bool nvml_read_fans_for_count(char* detail, size_t detailSize, unsigned int expectedCount) {
    if (!nvml_ensure_ready()) {
        set_message(detail, detailSize, "NVML not ready");
        return false;
    }

    memset(g_app.fanPercent, 0, sizeof(g_app.fanPercent));
    memset(g_app.fanTargetPercent, 0, sizeof(g_app.fanTargetPercent));
    memset(g_app.fanRpm, 0, sizeof(g_app.fanRpm));
    memset(g_app.fanPolicy, 0, sizeof(g_app.fanPolicy));
    memset(g_app.fanControlSignal, 0, sizeof(g_app.fanControlSignal));
    memset(g_app.fanTargetMask, 0, sizeof(g_app.fanTargetMask));
    memset(&g_app.readback.fan, 0, sizeof(g_app.readback.fan));
    g_app.fanCount = 0;
    g_app.fanMinPct = 0;
    g_app.fanMaxPct = 100;
    g_app.fanRangeKnown = false;
    g_app.fanIsAuto = true;

    if (!g_nvml_api.getNumFans) {
        set_message(detail, detailSize, "nvmlDeviceGetNumFans missing");
        return false;
    }

    unsigned int count = 0;
    nvmlReturn_t r = g_nvml_api.getNumFans(g_app.nvmlDevice, &count);
    if (r != NVML_SUCCESS || count == 0) {
        set_message(detail, detailSize, "%s", nvml_err_name(r));
        return false;
    }

    if (expectedCount != 0 && count != expectedCount) {
        set_message(detail, detailSize, "Fan count changed from %u to %u", expectedCount, count);
        return false;
    }
    g_app.fanSupported = true;
    g_app.fanCount = count > MAX_GPU_FANS ? MAX_GPU_FANS : count;

    if (g_nvml_api.getMinMaxFanSpeed) {
        unsigned int mn = 0, mx = 0;
        if (g_nvml_api.getMinMaxFanSpeed(g_app.nvmlDevice, &mn, &mx) == NVML_SUCCESS) {
            g_app.fanMinPct = mn;
            g_app.fanMaxPct = mx;
            g_app.fanRangeKnown = true;
        }
    }

    bool allAuto = true;
    for (unsigned int fan = 0; fan < g_app.fanCount; fan++) {
        bool policyKnown = false;
        if (g_nvml_api.getFanControlPolicy) {
            unsigned int pol = 0;
            if (g_nvml_api.getFanControlPolicy(g_app.nvmlDevice, fan, &pol) == NVML_SUCCESS) {
                g_app.fanPolicy[fan] = pol;
                g_app.readback.fan.policy[fan] = policyKnown = true;
                if (pol != NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW) allAuto = false;
            }
        }
        bool isAutoForFan = true;
        if (policyKnown) {
            unsigned int pol = g_app.fanPolicy[fan];
            isAutoForFan = (pol == NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW);
        }
        if (g_nvml_api.getFanSpeed) {
            unsigned int pct = 0;
            if (g_nvml_api.getFanSpeed(g_app.nvmlDevice, fan, &pct) == NVML_SUCCESS) {
                g_app.fanPercent[fan] = pct;
                g_app.readback.fan.measured[fan] = true;
            }
        }
        bool shouldReadTarget = !policyKnown
            ? (g_app.fanCurveRuntimeActive || g_app.fanFixedRuntimeActive || g_app.activeFanMode != FAN_MODE_AUTO)
            : !isAutoForFan;
        if (g_nvml_api.getTargetFanSpeed && shouldReadTarget) {
            unsigned int target = 0;
            if (g_nvml_api.getTargetFanSpeed(g_app.nvmlDevice, fan, &target) == NVML_SUCCESS) {
                g_app.fanTargetPercent[fan] = target;
                g_app.readback.fan.target[fan] = true;
                if (!policyKnown && target > 0) allAuto = false;
            }
        }
        if (g_nvml_api.getFanSpeedRpm) {
            nvmlFanSpeedInfo_t info = {};
            info.version = nvmlFanSpeedInfo_v1;
            info.fan = fan;
            if (g_nvml_api.getFanSpeedRpm(g_app.nvmlDevice, &info) == NVML_SUCCESS) {
                g_app.fanRpm[fan] = info.speed;
                g_app.readback.fan.rpm[fan] = true;
            }
        }
        if (g_nvml_api.getCoolerInfo) {
            nvmlCoolerInfo_t info = {};
            info.version = nvmlCoolerInfo_v1;
            info.index = fan;
            if (g_nvml_api.getCoolerInfo(g_app.nvmlDevice, &info) == NVML_SUCCESS) {
                g_app.fanControlSignal[fan] = info.signalType;
                g_app.fanTargetMask[fan] = info.target;
            }
        }
    }
    g_app.fanIsAuto = allAuto;
    // A completed read: the stall check reuses it instead of reading again.
    g_app.fanReadSerial++;
    return true;
}

static bool nvml_read_fans(char* detail, size_t detailSize) {
    return nvml_read_fans_for_count(detail, detailSize, 0);
}

static bool nvml_read_controllable_fan_count(unsigned int* count, char* detail, size_t detailSize) {
    *count = 0;
    const nvmlReturn_t result = g_nvml_api.getNumFans
        ? g_nvml_api.getNumFans(g_app.nvmlDevice, count) : NVML_ERROR_NOT_SUPPORTED;
    if (result != NVML_SUCCESS) {
        set_message(detail, detailSize, "Fan count unavailable: %s", nvml_err_name(result));
        return false;
    }
    if (*count == 0 || *count > MAX_GPU_FANS) {
        set_message(detail, detailSize, "Unsupported fan count: %u", *count);
        return false;
    }
    return true;
}

static bool live_fan_policy_readback_complete() {
    return g_app.fanSupported && g_app.fanCount > 0 &&
        all_fans_known(g_app.readback.fan.policy, g_app.fanCount);
}

static bool nvml_set_fan_auto(char* detail, size_t detailSize) {
    if (!nvml_ensure_ready() || !g_app.fanSupported || (!g_nvml_api.setDefaultFanSpeed && !g_nvml_api.setFanControlPolicy)) {
        set_message(detail, detailSize, "Fan auto unsupported");
        return false;
    }
    unsigned int count = 0;
    if (!nvml_read_controllable_fan_count(&count, detail, detailSize)) return false;
    for (unsigned int fan = 0; fan < count; fan++) {
        nvmlReturn_t defaultResult = NVML_ERROR_NOT_SUPPORTED;
        nvmlReturn_t policyResult = NVML_ERROR_NOT_SUPPORTED;
        char phase[128] = {};
        if (g_nvml_api.setDefaultFanSpeed) {
            StringCchPrintfA(phase, ARRAY_COUNT(phase), "Fan auto default write: fan=%u", fan);
            set_last_apply_phase(phase);
            defaultResult = g_nvml_api.setDefaultFanSpeed(g_app.nvmlDevice, fan);
        }
        if (g_nvml_api.setFanControlPolicy) {
            StringCchPrintfA(phase, ARRAY_COUNT(phase), "Fan auto policy write: fan=%u", fan);
            set_last_apply_phase(phase);
            policyResult = g_nvml_api.setFanControlPolicy(g_app.nvmlDevice, fan,
                NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW);
        }
        if (defaultResult != NVML_SUCCESS && policyResult != NVML_SUCCESS)
            debug_log("nvml_set_fan_auto: fan=%u default=%d policy=%d\n", fan,
                      (int)defaultResult, (int)policyResult);
    }
    for (int attempt = 0; attempt < 8; attempt++) {
        if (attempt > 0) Sleep(10);
        if (nvml_read_fans_for_count(detail, detailSize, count) &&
            live_fan_policy_readback_complete() && g_app.fanIsAuto) return true;
    }
    set_message(detail, detailSize, "Fan readback did not confirm driver Auto on all %u fans", count);
    debug_log("nvml_set_fan_auto: driver Auto unverified on %u fans\n", count);
    return false;
}
