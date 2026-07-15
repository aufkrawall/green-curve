// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_LINUX_TUI_LAYOUT_INTERNAL_H
#define GREEN_CURVE_LINUX_TUI_LAYOUT_INTERNAL_H

#include "linux_tui_layout.h"

class TuiCanvas {
public:
    TuiCanvas(const TuiViewModel& vm, TuiLayout* layout);
    void clear(TuiStyle style = TUI_STYLE_DEFAULT);
    void cell(int x, int y, const char* glyph, TuiStyle style);
    void text(int x, int y, int width, const char* value, TuiStyle style,
              bool right = false);
    void fill(int x, int y, int width, int height, TuiStyle style);
    void hline(int x, int y, int width, TuiStyle style);
    void vline(int x, int y, int height, TuiStyle style);
    void box(const TuiRect& rect, const char* title, const char* meta = nullptr);
    int button(const TuiRect& rect, const char* label, ActionType type,
               int index = 0, int value = 0, bool selected = false);
    int field(const TuiRect& rect, const char* value, TuiField field,
              int index = 0);
    void checkbox(int x, int y, int state, TuiStyle style);
    void register_action(const TuiRect& rect, ActionType type,
                         int index = 0, int value = 0, int context = 0);
    bool editing(TuiField field, int index) const;
    const TuiViewModel& view() const { return vm_; }
    TuiLayout* layout() const { return layout_; }

private:
    const TuiViewModel& vm_;
    TuiLayout* layout_;
};

void tui_draw_vf_tab(TuiCanvas* canvas, const TuiRect& content);
void tui_draw_fan_tab(TuiCanvas* canvas, const TuiRect& content);
void tui_draw_profiles_tab(TuiCanvas* canvas, const TuiRect& content);

void tui_format_int(char* buffer, size_t size, int value, bool sign = false);
void tui_format_gpu_summary(const TuiViewModel& vm, char* buffer, size_t size);

#endif
