// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "linux_tui_layout_internal.h"
#include "linux_tui_document_view.h"

static inline void tui_draw_transfer_dialog(TuiCanvas* canvas) {
    auto* layout = canvas->layout(); const auto& v = canvas->view().transfer;
    layout->actions.clear(); layout->transferVisible = true;
    const TuiRect panel{3, 2, layout->width - 6, layout->height - 4};
    canvas->fill(panel.x, panel.y, panel.width, panel.height, TUI_STYLE_PANEL);
    canvas->box(panel, v.reviewing ? "REVIEW PROFILE TRANSFER" : "TRANSFER SAVED PROFILES");
    const int x = panel.x + 2, y = panel.y + 2, width = panel.width - 4;
    const int buttons = panel.y + panel.height - 3;
    if (!v.reviewing) {
        const std::string& path = v.path;
        const int offset = tui_column_to_byte_offset(path, std::max(0, tui_display_columns(path) - width + 8));
        canvas->text(x, y, width, (std::string(v.pathEditing ? "> " : "File: ") + path.substr((size_t)offset)).c_str(),
            v.pathEditing ? (v.selectAll ? TUI_STYLE_FIELD_SELECTED : TUI_STYLE_FIELD_ACTIVE) : TUI_STYLE_FIELD);
        canvas->register_action({x, y, width, 1}, ACTION_TRANSFER_PATH);
        canvas->button({x, y + 2, 19, 1}, v.importing ? "IMPORT FROM FILE" : "EXPORT TO FILE", ACTION_TRANSFER_MODE);
        canvas->button({x + 21, y + 2, 25, 1}, v.all ? "BANK (UP TO 5)" : "ONE PROFILE", ACTION_TRANSFER_KIND);
        char label[48]{};
        if (v.importing && !v.all) {
            std::snprintf(label, sizeof(label), "Source record: %d", v.sourceRecord);
            canvas->button({x, y + 4, 25, 1}, label, ACTION_TRANSFER_RECORD);
        }
        if (!v.all) {
            if (!v.slot) std::snprintf(label, sizeof(label), "Choose destination slot");
            else std::snprintf(label, sizeof(label), "%s slot: %d", v.importing ? "Destination" : "Saved", v.slot);
            canvas->button({x + (v.importing ? 27 : 0), y + 4, 27, 1}, label, ACTION_TRANSFER_SLOT);
        } else canvas->text(x, y + 4, width, v.importing ? "Records 1..N replace slots 1..N; other slots stay." : "Exports every occupied saved slot, in slot order.", TUI_STYLE_MUTED);
        const char* formats[] = {"Format: portable / markers", "Format: Windows legacy INI", "Format: Linux legacy INI"};
        canvas->button({x, y + 6, 36, 1}, formats[std::clamp(v.format, 0, 2)], ACTION_TRANSFER_FORMAT);
        if (v.format) {
            const char* memory[] = {"Memory: use markers", "Memory: physical MHz", "Memory: effective MHz"};
            const char* curve[] = {"Curve: use markers", "Curve: absolute", "Curve: absolute + origin", "Curve: base + offset"};
            canvas->button({x, y + 8, 32, 1}, memory[std::clamp(v.memory, 0, 2)], ACTION_TRANSFER_MEMORY);
            canvas->button({x, y + 10, 36, 1}, curve[std::clamp(v.curve, 0, 3)], ACTION_TRANSFER_CURVE);
        } else canvas->text(x, y + 8, width, "Transfers exact saved intent without reading the GPU.", TUI_STYLE_MUTED);
        if (!v.pathEditing) canvas->button({x, buttons, 19, 1}, "PREVIEW", ACTION_TRANSFER_PREVIEW);
        else canvas->text(x, buttons, width - 16, "Type path; Enter finishes; Esc cancels", TUI_STYLE_MUTED);
    } else {
        std::vector<TuiSourceLine> lines;
        if (v.report) for (const auto& line : *v.report) lines.push_back({line});
        tui_wrap_source_lines(lines, (size_t)std::max(8, width));
        const int rows = std::max(1, panel.height - 10);
        layout->transferVisibleRows = rows; layout->transferLineCount = (int)lines.size();
        const int first = std::clamp(v.scroll, 0, std::max(0, (int)lines.size() - rows));
        char meta[96]{}; std::snprintf(meta, sizeof(meta), "Rows %d-%d/%d | PgUp/PgDn scroll", first + 1,
            std::min((int)lines.size(), first + rows), (int)lines.size());
        canvas->text(x, y, width, meta, TUI_STYLE_MUTED);
        for (int i = 0; i < rows && first + i < (int)lines.size(); ++i)
            canvas->text(x, y + 1 + i, width, lines[first + i].text.c_str(), TUI_STYLE_TEXT);
        if (v.importing) {
            canvas->button({x, buttons - 4, 37, 1}, v.loadAfterImport ? "[x] Load first imported profile" : "[ ] Load first imported profile", ACTION_TRANSFER_LOAD);
            if (v.loadAfterImport && v.needsDiscard)
                canvas->button({x, buttons - 2, 37, 1}, v.discardConfirmed ? "[x] Discard unsaved draft" : "[ ] Discard unsaved draft", ACTION_TRANSFER_DISCARD);
        }
        if (v.ready && (!v.importing || !v.loadAfterImport || !v.needsDiscard || v.discardConfirmed))
            canvas->button({x, buttons, 20, 1}, v.importing ? "CONFIRM IMPORT" : "WRITE EXPORT", ACTION_TRANSFER_COMMIT);
        canvas->button({x + 22, buttons, 14, 1}, "BACK", ACTION_TRANSFER_BACK);
    }
    canvas->button({panel.x + panel.width - 16, buttons, 14, 1}, "CANCEL", ACTION_TRANSFER_CANCEL);
}
