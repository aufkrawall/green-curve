// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Included into main_probe_config.cpp's translation unit. Keeping profile
// serialization beside its parser avoids growing that already dense shard.
static void append_fan_curve_section_text(char* cfg, size_t cfgSize,
    size_t* used, const char* sectionName, const FanCurveConfig* curve) {
    if (!cfg || !used || !sectionName || !curve) return;

    auto appendf = [&](const char* fmt, ...) {
        if (*used >= cfgSize - 1) return;
        va_list ap;
        va_start(ap, fmt);
        int n = _vsnprintf_s(cfg + *used, cfgSize - *used, _TRUNCATE, fmt, ap);
        va_end(ap);
        if (n > 0) *used += (size_t)n;
    };

    appendf("[%s]\r\n", sectionName);
    appendf("poll_interval_ms=%d\r\n", curve->pollIntervalMs);
    appendf("hysteresis_c=%d\r\n", curve->hysteresisC);
    appendf("zero_rpm_hysteresis_c=%u\r\n",
        (unsigned)curve->zeroRpmHysteresisC);
    appendf("zero_rpm_enabled=%d\r\n", curve->zeroRpmEnabled ? 1 : 0);
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        appendf("enabled%d=%d\r\n", i, curve->points[i].enabled ? 1 : 0);
        appendf("temp%d=%d\r\n", i, curve->points[i].temperatureC);
        appendf("pct%d=%d\r\n", i, curve->points[i].fanPercent);
    }
    appendf("\r\n");
}
