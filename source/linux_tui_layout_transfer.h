// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "linux_tui_layout_internal.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include "profile_legacy_ini.h"

// Export: one row per slot. Import: one row per record in the file, each with
// its destination slot. report holds one summary per row, then a note line.
static inline void tui_draw_transfer_dialog(TuiCanvas* canvas) {
    auto* layout = canvas->layout(); const auto& v = canvas->view().transfer;
    layout->actions.clear(); layout->transferVisible = true;
    const TuiRect panel{3, 2, layout->width - 6, layout->height - 4};
    canvas->fill(panel.x, panel.y, panel.width, panel.height, TUI_STYLE_PANEL);
    canvas->box(panel, v.importing ? "IMPORT PROFILES FROM FILE" : "EXPORT PROFILES TO FILE");
    const int x = panel.x + 2, width = panel.width - 4, buttons = panel.y + panel.height - 3;
    int y = panel.y + 2;
    canvas->button({x, y, 20, 1}, v.importing ? "SWITCH TO EXPORT" : "SWITCH TO IMPORT", ACTION_TRANSFER_MODE);
    canvas->text(x + 22, y, width - 22, v.importing ? "Adds profiles from a file to your slots." :
        "Saves profiles to a file; no GPU needed.", TUI_STYLE_MUTED);
    y += 2;
    const std::string& path = v.path;
    const int offset = tui_column_to_byte_offset(path, std::max(0, tui_display_columns(path) - width + 8));
    canvas->text(x, y, width, (std::string(v.pathEditing ? "> " : "File: ") + path.substr((size_t)offset)).c_str(),
        v.pathEditing ? (v.selectAll ? TUI_STYLE_FIELD_SELECTED : TUI_STYLE_FIELD_ACTIVE) : TUI_STYLE_FIELD);
    canvas->register_action({x, y, width, 1}, ACTION_TRANSFER_PATH);
    y += 2;
    if (v.importing && v.legacyFile) {
        canvas->button({x, y, 32, 1}, v.format == PROFILE_LEGACY_LINUX_INI ? "Saved on: Linux" : "Saved on: Windows", ACTION_TRANSFER_FORMAT);
        canvas->text(x + 34, y, width - 34, "Older settings file", TUI_STYLE_MUTED);
        ++y;
        if (v.askMemory) {
            const char* memory[] = {"Memory values: choose...", "Memory values: as shown in Green Curve", "Memory values: doubled (effective)"};
            canvas->button({x, y, 44, 1}, memory[std::clamp(v.memory, 0, 2)], ACTION_TRANSFER_MEMORY); ++y;
        }
        if (v.askCurve) {
            const char* curve[] = {"Curve points: choose...", "Curve points: final clocks", "Curve points: final, some follow core offset",
                "Curve points: offsets from stock"};
            canvas->button({x, y, 48, 1}, curve[std::clamp(v.curve, 0, 3)], ACTION_TRANSFER_CURVE); ++y;
        }
        ++y;
    }
    const bool lines = v.report && !v.report->empty();
    for (int i = 0; i < v.rows && i < 5 && y < buttons - 3; ++i, ++y) {
        char label[24]{};
        std::snprintf(label, sizeof(label), "%s %s %d", v.rowEnabled[i] ? "[x]" : "[ ]", v.importing ? "Profile" : "Slot", i + 1);
        if (v.rowAvailable[i]) canvas->button({x, y, 14, 1}, label, ACTION_TRANSFER_RECORD, i, 0, v.rowEnabled[i]);
        else canvas->text(x, y, 14, label, TUI_STYLE_MUTED);
        int textWidth = width - 16;
        if (v.importing) {
            char slot[24]{};
            std::snprintf(slot, sizeof(slot), "-> slot %d", v.rowSlot[i]);
            canvas->button({x + width - 12, y, 12, 1}, slot, ACTION_TRANSFER_SLOT, i);
            textWidth -= 14;
        }
        if (lines && i < (int)v.report->size())
            canvas->text(x + 16, y, textWidth, (*v.report)[(size_t)i].c_str(), v.rowAvailable[i] ? TUI_STYLE_TEXT : TUI_STYLE_MUTED);
    }
    if (lines && !v.report->back().empty()) canvas->text(x, buttons - 2, width, v.report->back().c_str(), TUI_STYLE_CYAN);
    if (v.importing && !v.probed && !v.pathEditing) canvas->button({x, buttons, 14, 1}, "READ FILE", ACTION_TRANSFER_PREVIEW);
    if (v.pathEditing) canvas->text(x, buttons, width - 16, "Type the path; Enter finishes; Esc closes", TUI_STYLE_MUTED);
    else if (v.canRun) canvas->button({x + (v.importing && !v.probed ? 16 : 0), buttons, 12, 1}, v.importing ? "IMPORT" : "EXPORT", ACTION_TRANSFER_COMMIT);
    canvas->button({panel.x + panel.width - 16, buttons, 14, 1}, "CANCEL", ACTION_TRANSFER_CANCEL);
    layout->transferVisibleRows = 1; layout->transferLineCount = 1;
}
