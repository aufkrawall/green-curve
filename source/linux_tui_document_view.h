// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_document.h"
#include <algorithm>

static void tui_draw_document_source(TuiCanvas* canvas, const TuiRect& panel) {
    const auto& vm = canvas->view();
    const auto& d = *vm.sourceDocument;
    std::vector<std::string> lines;
    lines.emplace_back(std::string("Name: ") + d.displayName);
    const char* scopes[] = {"update included settings", "replace owned profile", "release owned settings"};
    lines.emplace_back(std::string("Scope: ") + scopes[d.applicationScope]);
    lines.emplace_back("Destination review required before editing or Apply");
    if (d.graphicsOffset.present) lines.emplace_back("Graphics offset: " + std::to_string(d.graphicsOffset.offsetHz) + " Hz");
    if (d.hasMemoryOffset) lines.emplace_back("Memory offset: " + std::to_string(d.memoryOffsetHz) + " physical Hz");
    if (d.power.present) lines.emplace_back("Power: " + std::to_string(d.power.ratio.numerator) + "/" +
        std::to_string(d.power.ratio.denominator) + " of default");
    if (d.lock.present) lines.emplace_back(std::string(d.lock.mode == LOCK_MODE_HARD ? "Clock pin: " :
        d.lock.mode == LOCK_MODE_FLATTEN ? "Flatten: " : "Lock release: ") +
        std::to_string(d.lock.targetFrequencyHz) + " Hz");
    if (d.fan.present) lines.emplace_back(std::string("Fan intent: ") +
        (d.fan.fanMode == FAN_MODE_CURVE ? "temperature curve" : d.fan.fanAuto ? "automatic" : "fixed duty"));
    const auto add = [&](bool present, const char* label, long long value, const char* unit) {
        if (present) lines.emplace_back(std::string(label) + std::to_string(value) + unit);
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
        lines.emplace_back(line);
    }
    const int rows = std::max(1, panel.height - 4);
    const int first = std::clamp(vm.sourceScroll, 0, std::max(0, (int)lines.size() - rows));
    char meta[96]{};
    snprintf(meta, sizeof(meta), "source intent | rows %d-%d/%d | PgUp/PgDn scroll", first + 1,
        std::min((int)lines.size(), first + rows), (int)lines.size());
    canvas->box(panel, "PORTABLE SOURCE", meta);
    for (int row = 0; row < rows && first + row < (int)lines.size(); ++row)
        canvas->text(panel.x + 2, panel.y + 2 + row, panel.width - 4, lines[first + row].c_str(), TUI_STYLE_TEXT);
}
