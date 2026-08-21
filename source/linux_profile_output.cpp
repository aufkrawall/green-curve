// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "linux_port_internal.h"

#include <string>

static std::string json_escape(const char* text) {
    std::string out;
    if (!text) return out;
    for (const unsigned char* p = (const unsigned char*)text; *p; ++p) {
        switch (*p) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (*p < 32) appendf(&out, "\\u%04x", *p);
                else out.push_back((char)*p);
                break;
        }
    }
    return out;
}

void print_desired_settings_text(FILE* out, int slot, const DesiredSettings* desired) {
    if (!out || !desired) return;
    fprintf(out, "Green Curve Linux config snapshot\n");
    fprintf(out, "Profile slot: %d\n", slot);
    fprintf(out, "GPU offset: %d MHz\n", desired->gpuOffsetMHz);
    fprintf(out, "GPU offset exclude first N: %d\n", desired->gpuOffsetExcludeLowCount);
    if (desired->hasLock) fprintf(out, "Lock: point %d @ %u MHz\n", desired->lockCi, desired->lockMHz);
    fprintf(out, "Memory offset: %d MHz\n", desired->memOffsetMHz);
    fprintf(out, "Power limit: %d%%\n", desired->powerLimitPct);
    if (desired->hasXbarOffsetKhz)
        fprintf(out, "XBAR clock offset: %d kHz\n", desired->xbarOffsetKhz);
    if (desired->hasXbarMsvddOffsetUv)
        fprintf(out, "XBAR MSVDD offset: %d uV\n", desired->xbarMsvddOffsetUv);
    fprintf(out, "Fan mode: %s\n", fan_mode_label(desired->fanMode));
    fprintf(out, "Fan fixed: %d%%\n", desired->fanPercent);
    char fanSummary[96] = {};
    fan_curve_format_summary(&desired->fanCurve, fanSummary, sizeof(fanSummary));
    fprintf(out, "Fan curve: %s\n", fanSummary);
    fprintf(out, "\nFan curve points\n");
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        fprintf(out, "  %d: %s temp=%dC pct=%d\n", i, desired->fanCurve.points[i].enabled ? "on " : "off", desired->fanCurve.points[i].temperatureC, desired->fanCurve.points[i].fanPercent);
    }
    fprintf(out, "\nVF points\n");
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!desired->hasCurvePoint[i]) continue;
        fprintf(out, "  point%d=%u\n", i, desired->curvePointMHz[i]);
    }
}

void print_desired_settings_json(FILE* out, int slot, const DesiredSettings* desired) {
    if (!out || !desired) return;
    fprintf(out, "{\n");
    fprintf(out, "  \"profile_slot\": %d,\n", slot);
    fprintf(out, "  \"gpu_offset_mhz\": %d,\n", desired->gpuOffsetMHz);
    fprintf(out, "  \"gpu_offset_exclude_low_count\": %d,\n", desired->gpuOffsetExcludeLowCount);
    fprintf(out, "  \"lock_ci\": %d,\n", desired->hasLock ? desired->lockCi : -1);
    fprintf(out, "  \"lock_mhz\": %u,\n", desired->hasLock ? desired->lockMHz : 0u);
    fprintf(out, "  \"mem_offset_mhz\": %d,\n", desired->memOffsetMHz);
    fprintf(out, "  \"power_limit_pct\": %d,\n", desired->powerLimitPct);
    fprintf(out, "  \"xbar_clock_owned\": %s,\n", desired->hasXbarOffsetKhz ? "true" : "false");
    fprintf(out, "  \"xbar_offset_khz\": %d,\n", desired->xbarOffsetKhz);
    fprintf(out, "  \"xbar_msvdd_owned\": %s,\n", desired->hasXbarMsvddOffsetUv ? "true" : "false");
    fprintf(out, "  \"xbar_msvdd_offset_uv\": %d,\n", desired->xbarMsvddOffsetUv);
    fprintf(out, "  \"fan_mode\": \"%s\",\n", json_escape(fan_mode_to_config_value(desired->fanMode)).c_str());
    fprintf(out, "  \"fan_fixed_pct\": %d,\n", desired->fanPercent);
    fprintf(out, "  \"fan_curve\": {\n");
    fprintf(out, "    \"poll_interval_ms\": %d,\n", desired->fanCurve.pollIntervalMs);
    fprintf(out, "    \"hysteresis_c\": %d,\n", desired->fanCurve.hysteresisC);
    fprintf(out, "    \"points\": [\n");
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        fprintf(out,
            "      {\"index\": %d, \"enabled\": %s, \"temp_c\": %d, \"pct\": %d}%s\n",
            i,
            desired->fanCurve.points[i].enabled ? "true" : "false",
            desired->fanCurve.points[i].temperatureC,
            desired->fanCurve.points[i].fanPercent,
            (i + 1 < FAN_CURVE_MAX_POINTS) ? "," : "");
    }
    fprintf(out, "    ]\n  },\n  \"vf_curve\": [\n");
    bool first = true;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!desired->hasCurvePoint[i]) continue;
        fprintf(out, "%s    {\"index\": %d, \"mhz\": %u}", first ? "" : ",\n", i, desired->curvePointMHz[i]);
        first = false;
    }
    if (!first) fprintf(out, "\n");
    fprintf(out, "  ]\n}\n");
}

