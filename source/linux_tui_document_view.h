// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_document.h"
#include "profile_editor_source_rows.h"
#include <algorithm>
#include <string>
#include <vector>
#include <utility>

struct TuiSourceLine { std::string text; TuiField field = TUI_FIELD_NONE; int index = 0; };
static std::vector<TuiSourceLine> tui_document_source_lines(const ProfileDocument& d) {
    std::vector<TuiSourceLine> lines;
    for (const auto& row : profile_editor_source_rows(d)) {
        const TuiField field = !row.editable ? TUI_FIELD_NONE : row.field == PROFILE_EDIT_GRAPHICS ? TUI_FIELD_SOURCE_GRAPHICS :
            row.field == PROFILE_EDIT_MEMORY ? TUI_FIELD_SOURCE_MEMORY : TUI_FIELD_SOURCE_POINT;
        lines.push_back({row.text, field, row.index});
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
