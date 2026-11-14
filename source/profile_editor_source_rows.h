// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_editor_source_edit.h"
#include <string>
#include <vector>
#include <utility>

struct ProfileEditorSourceRow {
    std::string text;
    bool editable = false;
    ProfileEditorSourceField field = PROFILE_EDIT_POINT;
    int index = 0;
};
static inline std::vector<ProfileEditorSourceRow> profile_editor_source_rows(const ProfileDocument& d) {
    std::vector<ProfileEditorSourceRow> rows;
    const auto add = [&](std::string text, bool editable = false, ProfileEditorSourceField field = PROFILE_EDIT_POINT, int index = 0) {
        rows.push_back({std::move(text), editable, field, index});
    };
    add(std::string("Name: ") + d.displayName);
    const char* scopes[] = {"update included settings", "replace owned profile", "release owned settings"};
    add(std::string("Scope: ") + scopes[d.applicationScope]);
    add("Select a field to type MHz; destination review required before Apply");
    if (d.graphicsOffset.present) add("Graphics offset: " + std::to_string(d.graphicsOffset.offsetHz) + " Hz",
        profile_editor_source_editable(d, PROFILE_EDIT_GRAPHICS, 0), PROFILE_EDIT_GRAPHICS);
    if (d.hasMemoryOffset) add("Memory offset: " + std::to_string(d.memoryOffsetHz) + " physical Hz",
        profile_editor_source_editable(d, PROFILE_EDIT_MEMORY, 0), PROFILE_EDIT_MEMORY);
    if (d.power.present) add("Power: " + std::to_string(d.power.ratio.numerator) + "/" + std::to_string(d.power.ratio.denominator) + " of default");
    if (d.lock.present) add(std::string(d.lock.mode == LOCK_MODE_HARD ? "Clock pin: " :
        d.lock.mode == LOCK_MODE_FLATTEN ? "Flatten: " : "Lock release: ") + std::to_string(d.lock.targetFrequencyHz) + " Hz");
    if (d.fan.present) add(std::string("Fan intent: ") +
        (d.fan.fanMode == FAN_MODE_CURVE ? "temperature curve" : d.fan.fanAuto ? "automatic" : "fixed duty"));
    const auto scalar = [&](bool present, const char* label, long long value, const char* unit) {
        if (present) add(std::string(label) + std::to_string(value) + unit);
    };
    scalar(d.hasXbarOffsetHz, "XBAR clock: ", d.xbarOffsetHz, " Hz");
    scalar(d.hasXbarMsvddOffsetUv, "XBAR voltage: ", d.xbarMsvddOffsetUv, " uV");
    scalar(d.hasSysClkOffsetHz, "System clock: ", d.sysClkOffsetHz, " Hz");
    scalar(d.hasVideoClkOffsetHz, "Video clock: ", d.videoClkOffsetHz, " Hz");
    for (int i = 0; i < d.topologyCount; ++i) {
        const auto& p = d.points[i];
        std::string line = d.topology[i].voltageUv > 0 ? std::to_string(d.topology[i].voltageUv) + " uV: " :
            "Source index " + std::to_string(d.topology[i].sourceIndex) + " (voltage unresolved): ";
        if (p.kind == PROFILE_POINT_ABSOLUTE_FREQUENCY) line += std::to_string(p.absoluteFrequencyHz) + " Hz absolute";
        else if (p.kind == PROFILE_POINT_CAPTURED_OFFSET) line += std::to_string(p.capturedOffsetHz) + " Hz captured offset";
        else if (p.kind == PROFILE_POINT_PROJECTION) line += "projected from graphics intent";
        else line += "no point intent";
        if (d.topology[i].sourceWritable == 2) line += " (source read-only)";
        add(line, profile_editor_source_editable(d, PROFILE_EDIT_POINT, i), PROFILE_EDIT_POINT, i);
    }
    return rows;
}
