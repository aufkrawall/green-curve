// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_LINUX_FAN_CURVE_PROFILE_SAVE_H
#define GREEN_CURVE_LINUX_FAN_CURVE_PROFILE_SAVE_H

// Included by linux_port_profiles.cpp after the bounded INI model is visible.
static void save_fan_curve_section(IniDocument* doc,
    const char* sectionName, const FanCurveConfig* curve) {
    std::vector<IniEntry> entries;
    char key[32] = {};
    char value[32] = {};

    IniEntry pollEntry;
    pollEntry.key = "poll_interval_ms";
    snprintf(value, sizeof(value), "%d", curve->pollIntervalMs);
    pollEntry.value = value;
    entries.push_back(pollEntry);

    IniEntry hystEntry;
    hystEntry.key = "hysteresis_c";
    snprintf(value, sizeof(value), "%d", curve->hysteresisC);
    hystEntry.value = value;
    entries.push_back(hystEntry);

    IniEntry zeroRpmHystEntry;
    zeroRpmHystEntry.key = "zero_rpm_hysteresis_c";
    snprintf(value, sizeof(value), "%u", (unsigned)curve->zeroRpmHysteresisC);
    zeroRpmHystEntry.value = value;
    entries.push_back(zeroRpmHystEntry);

    IniEntry zeroRpmEntry;
    zeroRpmEntry.key = "zero_rpm_enabled";
    zeroRpmEntry.value = curve->zeroRpmEnabled ? "1" : "0";
    entries.push_back(zeroRpmEntry);

    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        IniEntry enabledEntry;
        snprintf(key, sizeof(key), "enabled%d", i);
        enabledEntry.key = key;
        snprintf(value, sizeof(value), "%d", curve->points[i].enabled ? 1 : 0);
        enabledEntry.value = value;
        entries.push_back(enabledEntry);

        IniEntry tempEntry;
        snprintf(key, sizeof(key), "temp%d", i);
        tempEntry.key = key;
        snprintf(value, sizeof(value), "%d", curve->points[i].temperatureC);
        tempEntry.value = value;
        entries.push_back(tempEntry);

        IniEntry pctEntry;
        snprintf(key, sizeof(key), "pct%d", i);
        pctEntry.key = key;
        snprintf(value, sizeof(value), "%d", curve->points[i].fanPercent);
        pctEntry.value = value;
        entries.push_back(pctEntry);
    }

    replace_section(doc, sectionName, entries);
}

#endif // GREEN_CURVE_LINUX_FAN_CURVE_PROFILE_SAVE_H
