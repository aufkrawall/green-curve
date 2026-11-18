// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Linux backend fan setters and maintenance probes used by the daemon fan
// worker.  Included at the end of linux_backend_mutation.cpp, after nvml_set_fan
// and the fan maintenance policy are visible.
#ifndef GREEN_CURVE_LINUX_BACKEND_FAN_CONTROL_H
#define GREEN_CURVE_LINUX_BACKEND_FAN_CONTROL_H

bool linux_backend_set_curve_fan_percent(LinuxGpuState* g, unsigned int percent) {
    if (!g || percent > 100) return false;
    return nvml_set_fan(g, FAN_MODE_FIXED, false, (int)percent);
}

bool linux_backend_set_fan_auto(LinuxGpuState* g) {
    return g && nvml_set_fan(g, FAN_MODE_AUTO, true, 0);
}

FanFixedMaintenanceDecision linux_backend_fixed_fan_check(LinuxGpuState* g,
                                                          int requestedPercent) {
    FanFixedMaintenanceDecision result = {false, false, "holding"};
    if (!g || !g->nvml.getNumFans) {
        result.failure = true;
        result.reason = "fan count unavailable";
        return result;
    }
    unsigned int fanCount = 0;
    if (g->nvml.getNumFans(g->nvmlDevice, &fanCount) != NVML_SUCCESS || fanCount == 0) {
        result.failure = true;
        result.reason = "fan count unreadable";
        return result;
    }
    if (fanCount > MAX_GPU_FANS) fanCount = MAX_GPU_FANS;
    // Compare against the duty the write actually sets, after the driver-range
    // clamp, so a fan whose floor is above the request does not look drifted.
    int effective = fan_manual_effective_percent(requestedPercent, g->fanMinPct,
        g->fanMaxPct, g->fanRangeKnown);
    for (unsigned int fan = 0; fan < fanCount; ++fan) {
        FanFixedMaintenanceInputs in = {};
        in.fixedModeActive = true;
        in.targetPercent = effective;
        unsigned int policy = 0;
        in.policyKnown = g->nvml.getFanControlPolicy &&
            g->nvml.getFanControlPolicy(g->nvmlDevice, fan, &policy) == NVML_SUCCESS;
        in.policyManual = in.policyKnown && policy == NVML_FAN_POLICY_MANUAL;
        unsigned int intent = 0;
        in.intentKnown = g->nvml.getTargetFanSpeed &&
            g->nvml.getTargetFanSpeed(g->nvmlDevice, fan, &intent) == NVML_SUCCESS;
        in.intentPercent = (int)intent;
        FanFixedMaintenanceDecision d = fan_fixed_maintenance_decide(in);
        if (d.failure) return d;
        if (d.write && !result.write) result = d;
    }
    return result;
}

bool linux_backend_fans_are_auto(LinuxGpuState* g) {
    if (!g || !g->nvml.getNumFans || !g->nvml.getFanControlPolicy) return false;
    unsigned int fanCount = 0;
    if (g->nvml.getNumFans(g->nvmlDevice, &fanCount) != NVML_SUCCESS ||
        fanCount == 0) return false;
    if (fanCount > MAX_GPU_FANS) fanCount = MAX_GPU_FANS;
    for (unsigned int fan = 0; fan < fanCount; ++fan) {
        unsigned int policy = 0;
        if (g->nvml.getFanControlPolicy(g->nvmlDevice, fan, &policy) !=
                NVML_SUCCESS ||
            policy != NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW) {
            return false;
        }
    }
    return true;
}

#endif  // GREEN_CURVE_LINUX_BACKEND_FAN_CONTROL_H
