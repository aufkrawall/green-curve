// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_document.h"
#include "profile_editor_source_edit.h"
#include <algorithm>
#include <string>
#include <vector>
#include <utility>

struct TuiSourceLine { std::string text; TuiField field = TUI_FIELD_NONE; int index = 0; };
static std::vector<TuiSourceLine> tui_document_source_lines(const ProfileDocument& d) {
    std::vector<TuiSourceLine> lines;
    const auto addLine = [&](std::string text, TuiField field = TUI_FIELD_NONE, int index = 0) {
        lines.push_back({std::move(text), field, index});
    };
    addLine(std::string("Name: ") + d.displayName);
    const char* scopes[] = {"update included settings", "replace owned profile", "release owned settings"};
    addLine(std::string("Scope: ") + scopes[d.applicationScope]);
    addLine("Select a field to type MHz; destination review required before Apply");
    if (d.graphicsOffset.present) addLine("Graphics offset: " + std::to_string(d.graphicsOffset.offsetHz) + " Hz",
        profile_editor_source_editable(d, PROFILE_EDIT_GRAPHICS, 0) ? TUI_FIELD_SOURCE_GRAPHICS : TUI_FIELD_NONE);
    if (d.hasMemoryOffset) addLine("Memory offset: " + std::to_string(d.memoryOffsetHz) + " physical Hz",
        profile_editor_source_editable(d, PROFILE_EDIT_MEMORY, 0) ? TUI_FIELD_SOURCE_MEMORY : TUI_FIELD_NONE);
    if (d.power.present) addLine("Power: " + std::to_string(d.power.ratio.numerator) + "/" +
        std::to_string(d.power.ratio.denominator) + " of default");
    if (d.lock.present) addLine(std::string(d.lock.mode == LOCK_MODE_HARD ? "Clock pin: " :
        d.lock.mode == LOCK_MODE_FLATTEN ? "Flatten: " : "Lock release: ") +
        std::to_string(d.lock.targetFrequencyHz) + " Hz");
    if (d.fan.present) addLine(std::string("Fan intent: ") +
        (d.fan.fanMode == FAN_MODE_CURVE ? "temperature curve" : d.fan.fanAuto ? "automatic" : "fixed duty"));
    const auto add = [&](bool present, const char* label, long long value, const char* unit) {
        if (present) addLine(std::string(label) + std::to_string(value) + unit);
    };
    add(d.hasXbarOffsetHz, "XBAR clock: ", d.xbarOffsetHz, " Hz");
    add(d.hasXbarMsvddOffsetUv, "XBAR voltage: ", d.xbarMsvddOffsetUv, " uV");
    add(d.hasSysClkOffsetHz, "System clock: ", d.sysClkOffsetHz, " Hz");
    add(d.hasVideoClkOffsetHz, "Video clock: ", d.videoClkOffsetHz, " Hz");
    for (int i = 0; i < d.topologyCount; ++i) {
        const auto& p = d.points[i];
        std::string line = d.topology[i].voltageUv > 0 ? std::to_string(d.topology[i].voltageUv) + " uV: " :
            "Source index " + std::to_string(d.topology[i].sourceIndex) + " (voltage unresolved): ";
        if (p.kind == PROFILE_POINT_ABSOLUTE_FREQUENCY) line += std::to_string(p.absoluteFrequencyHz) + " Hz absolute";
        else if (p.kind == PROFILE_POINT_CAPTURED_OFFSET) line += std::to_string(p.capturedOffsetHz) + " Hz captured offset";
        else if (p.kind == PROFILE_POINT_PROJECTION) line += "projected from graphics intent";
        else line += "no point intent";
        if (d.topology[i].sourceWritable == 2) line += " (source read-only)";
        addLine(line, profile_editor_source_editable(d, PROFILE_EDIT_POINT, i) ? TUI_FIELD_SOURCE_POINT : TUI_FIELD_NONE, i);
    }
    return lines;
}
static void tui_wrap_source_lines(std::vector<TuiSourceLine>& lines, size_t width) {
    std::vector<TuiSourceLine> wrapped;
    for (auto line : lines) {
        while (line.text.size() > width) {
            size_t split = line.text.rfind(' ', width);
            if (split == std::string::npos || !split) split = width;
            while (split && ((unsigned char)line.text[split] & 0xc0) == 0x80) --split;
            if (!split) split = width;
            wrapped.push_back({line.text.substr(0, split), line.field, line.index});
            line.text.erase(0, split); if (!line.text.empty() && line.text[0] == ' ') line.text.erase(0, 1);
            line.field = TUI_FIELD_NONE;
        }
        wrapped.push_back(line);
    }
    lines.swap(wrapped);
}
static void tui_draw_document_source(TuiCanvas* canvas, const TuiRect& panel) {
    const auto& vm = canvas->view();
    auto lines = tui_document_source_lines(*vm.sourceDocument);
    if (vm.sourceCheckLines) {
        lines.clear();
        for (const auto& line : *vm.sourceCheckLines) lines.push_back({line});
    }
    tui_wrap_source_lines(lines, (size_t)std::max(8, panel.width - 6));
    const int rows = std::max(1, panel.height - 4);
    canvas->layout()->sourceVisibleRows = rows;
    canvas->layout()->sourceLineCount = (int)lines.size();
    const int first = std::clamp(vm.sourceScroll, 0, std::max(0, (int)lines.size() - rows));
    char meta[96]{};
    snprintf(meta, sizeof(meta), "%s | rows %d-%d/%d | PgUp/PgDn scroll", vm.sourceCheckLines ? "read-only preview" : "source intent", first + 1,
        std::min((int)lines.size(), first + rows), (int)lines.size());
    canvas->box(panel, vm.sourceCheckLines ? "DESTINATION CHECK (read-only)" : "PORTABLE SOURCE", meta);
    for (int row = 0; row < rows && first + row < (int)lines.size(); ++row) {
        const auto& line = lines[first + row];
        if (line.field != TUI_FIELD_NONE)
            canvas->field({panel.x + 2, panel.y + 2 + row, panel.width - 4, 1}, line.text.c_str(), line.field, line.index);
        else canvas->text(panel.x + 2, panel.y + 2 + row, panel.width - 4, line.text.c_str(), TUI_STYLE_TEXT);
    }
}
