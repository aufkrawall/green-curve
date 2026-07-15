// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "linux_tui_layout_internal.h"

#include <stdio.h>
#include <string.h>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

namespace {

int utf8_character_bytes(unsigned char lead) {
    if ((lead & 0x80) == 0) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

bool action_rects_overlap(const ClickAction& a, const ClickAction& b) {
    return a.x1 <= b.x2 && a.x2 >= b.x1 &&
           a.y1 <= b.y2 && a.y2 >= b.y1;
}

TuiRect action_bar_rect(int width, int height) {
    return TuiRect{1, height - 2, width, 2};
}

void draw_header(TuiCanvas* c) {
    TuiLayout* out = c->layout();
    const TuiViewModel& vm = c->view();
    char title[160] = {};
    snprintf(title, sizeof(title), " Green Curve Linux v%s", APP_VERSION);
    c->fill(1, 1, out->width, 1, TUI_STYLE_PANEL);
    c->text(1, 1, out->width / 2, title, TUI_STYLE_TITLE);
    const char* connection = vm.serviceOnline ? "● DAEMON CONNECTED " : "● DAEMON OFFLINE ";
    c->text(out->width / 2 + 1, 1, out->width - out->width / 2,
            connection, vm.serviceOnline ? TUI_STYLE_GREEN : TUI_STYLE_RED, true);

    c->fill(1, 2, out->width, 1, TUI_STYLE_PANEL);
    c->text(2, 2, 4, "GPU", TUI_STYLE_MUTED);
    int gpuWidth = out->width >= 140 ? 62 : out->width >= 100 ? 48 : out->width - 30;
    if (gpuWidth < 28) gpuWidth = 28;
    c->button(TuiRect{7, 2, gpuWidth, 1}, vm.selectedGpu && vm.selectedGpu[0]
        ? vm.selectedGpu : "unselected GPU", ACTION_GPU_SELECT_DELTA, 0, 1);
    char live[160] = {};
    if (vm.serviceOnline && vm.service) {
        snprintf(live, sizeof(live), "%d VF pts  •  generation %llu",
                 vm.service->snapshot.numPopulated,
                 (unsigned long long)vm.service->state.gpuGeneration);
    } else {
        snprintf(live, sizeof(live), "%u GPU(s) known", vm.gpuCount);
    }
    int liveX = 9 + gpuWidth;
    if (liveX < out->width - 4)
        c->text(liveX, 2, out->width - liveX, live, TUI_STYLE_MUTED, true);

    c->fill(1, 3, out->width, 1, TUI_STYLE_DEFAULT);
    int tabWidth = out->breakpoint == TUI_BREAKPOINT_COMPACT
        ? (out->width - 3) / 3 : 22;
    const char* vf = out->breakpoint == TUI_BREAKPOINT_COMPACT ? "VF" : "VF CURVE";
    const char* fan = out->breakpoint == TUI_BREAKPOINT_COMPACT ? "FAN" : "FAN CURVE";
    const char* profiles = out->breakpoint == TUI_BREAKPOINT_COMPACT
        ? "TOOLS" : "PROFILES & TOOLS";
    c->button(TuiRect{2, 3, tabWidth, 1}, vf, ACTION_TAB_SET, 0,
              TUI_TAB_VF, vm.tab == TUI_TAB_VF);
    c->button(TuiRect{3 + tabWidth, 3, tabWidth, 1}, fan, ACTION_TAB_SET, 0,
              TUI_TAB_FAN, vm.tab == TUI_TAB_FAN);
    c->button(TuiRect{4 + tabWidth * 2, 3, tabWidth, 1}, profiles,
              ACTION_TAB_SET, 0, TUI_TAB_PROFILES,
              vm.tab == TUI_TAB_PROFILES);
    if (out->breakpoint != TUI_BREAKPOINT_COMPACT) {
        c->text(out->width - 27, 3, 25,
                vm.dirty ? "● staged changes" : "● live state",
                vm.dirty ? TUI_STYLE_ORANGE : TUI_STYLE_GREEN, true);
    }
}

void draw_footer(TuiCanvas* c) {
    TuiLayout* out = c->layout();
    const TuiViewModel& vm = c->view();
    TuiRect bar = action_bar_rect(out->width, out->height);
    c->fill(bar.x, bar.y, bar.width, bar.height, TUI_STYLE_PANEL);
    c->hline(1, bar.y, out->width, TUI_STYLE_BORDER);

    int y = bar.y;
    if (out->breakpoint == TUI_BREAKPOINT_COMPACT) {
        c->button(TuiRect{2, y, 12, 1}, "REFRESH", ACTION_REFRESH);
        c->button(TuiRect{15, y, 17, 1}, "RESET GPU", ACTION_APPLY_RESET);
        c->button(TuiRect{out->width - 22, y, 16, 1}, "APPLY",
                  ACTION_APPLY, 0, 0, vm.dirty);
        c->button(TuiRect{out->width - 5, y, 4, 1}, "Q", ACTION_QUIT);
    } else {
        char profile[64] = {};
        snprintf(profile, sizeof(profile), "SLOT %d", vm.currentSlot);
        c->button(TuiRect{2, y, 10, 1}, profile, ACTION_TAB_SET, 0,
                  TUI_TAB_PROFILES);
        c->button(TuiRect{13, y, 10, 1}, "LOAD", ACTION_LOAD);
        c->button(TuiRect{24, y, 10, 1}, "SAVE", ACTION_SAVE);
        c->button(TuiRect{35, y, 10, 1}, "CLEAR", ACTION_CLEAR_PROFILE);
        c->button(TuiRect{out->width - 48, y, 12, 1}, "RESET GPU",
                  ACTION_APPLY_RESET);
        c->button(TuiRect{out->width - 35, y, 11, 1}, "REFRESH",
                  ACTION_REFRESH);
        c->button(TuiRect{out->width - 23, y, 18, 1}, "APPLY CHANGES",
                  ACTION_APPLY, 0, 0, vm.dirty);
        c->button(TuiRect{out->width - 4, y, 3, 1}, "Q", ACTION_QUIT);
    }

    const char* status = vm.status && vm.status[0] ? vm.status : "Ready";
    c->text(2, out->height - 1, out->width - 3, status,
            vm.serviceOnline ? TUI_STYLE_MUTED : TUI_STYLE_RED);
    const char* help = "Tab/Shift+Tab focus • Enter edit • wheel/PgUp/PgDn scroll • Ctrl+PgUp/PgDn tabs • F1 help";
    if (out->width >= 140)
        c->text(out->width / 2, out->height - 1, out->width / 2 - 2,
                help, TUI_STYLE_DIM, true);
}

}  // namespace

int tui_display_columns(const std::string& text) {
    int columns = 0;
    for (size_t i = 0; i < text.size();) {
        int bytes = utf8_character_bytes((unsigned char)text[i]);
        if (i + (size_t)bytes > text.size()) bytes = 1;
        i += (size_t)bytes;
        ++columns;
    }
    return columns;
}

int tui_column_to_byte_offset(const std::string& text, int column) {
    if (column <= 1) return 0;
    int current = 1;
    for (size_t i = 0; i < text.size();) {
        if (current >= column) return (int)i;
        int bytes = utf8_character_bytes((unsigned char)text[i]);
        if (i + (size_t)bytes > text.size()) bytes = 1;
        i += (size_t)bytes;
        ++current;
    }
    return (int)text.size();
}

bool tui_rect_contains(const TuiRect& rect, int x, int y) {
    return rect.width > 0 && rect.height > 0 && x >= rect.x && y >= rect.y &&
           x < rect.x + rect.width && y < rect.y + rect.height;
}

bool tui_layout_actions_valid(const TuiLayout& layout) {
    for (size_t i = 0; i < layout.actions.size(); ++i) {
        const ClickAction& a = layout.actions[i];
        if (a.x1 < 1 || a.y1 < 1 || a.x2 < a.x1 || a.y2 < a.y1 ||
            a.x2 > layout.width || a.y2 > layout.height) return false;
        for (size_t j = i + 1; j < layout.actions.size(); ++j) {
            if (action_rects_overlap(a, layout.actions[j])) return false;
        }
    }
    return true;
}

const char* tui_point_rule_label(TuiPointRule rule) {
    switch (rule) {
        case TUI_POINT_EXCLUDED: return "excluded";
        case TUI_POINT_GPU_OFFSET: return "GPU offset";
        case TUI_POINT_ABSOLUTE: return "absolute";
        case TUI_POINT_FLATTEN_KNEE: return "flatten knee";
        case TUI_POINT_FLATTEN_TAIL: return "flat tail";
        case TUI_POINT_HARD_PIN: return "hard pin";
        default: return "live";
    }
}

TuiPointValues tui_point_values(const TuiViewModel& vm, int pointIndex) {
    TuiPointValues out = {};
    if (!vm.service || !vm.desired || pointIndex < 0 ||
        pointIndex >= VF_NUM_POINTS) return out;
    const ServiceSnapshot& snapshot = vm.service->snapshot;
    const DesiredSettings& desired = *vm.desired;
    if (snapshot.curve[pointIndex].freq_kHz == 0) return out;
    out.populated = true;
    for (int i = 0; i < pointIndex; ++i)
        if (snapshot.curve[i].freq_kHz != 0) ++out.ordinal;
    out.voltageMv = snapshot.curve[pointIndex].volt_uV / 1000u;
    long long baseKHz = (long long)snapshot.curve[pointIndex].freq_kHz -
                        (long long)snapshot.freqOffsets[pointIndex];
    out.baseMHz = (int)((baseKHz >= 0 ? baseKHz + 500 : baseKHz - 500) / 1000);
    out.liveMHz = (int)((snapshot.curve[pointIndex].freq_kHz + 500u) / 1000u);
    out.targetMHz = out.liveMHz;
    out.rule = TUI_POINT_LIVE;

    if (desired.hasLock && desired.lockCi >= 0 &&
        pointIndex >= desired.lockCi && desired.lockMHz > 0) {
        out.targetMHz = (int)desired.lockMHz;
        if (desired.lockMode == LOCK_MODE_HARD)
            out.rule = TUI_POINT_HARD_PIN;
        else if (pointIndex == desired.lockCi)
            out.rule = TUI_POINT_FLATTEN_KNEE;
        else
            out.rule = TUI_POINT_FLATTEN_TAIL;
    } else if (desired.hasCurvePoint[pointIndex] &&
               desired.curvePointMHz[pointIndex] > 0) {
        out.targetMHz = (int)desired.curvePointMHz[pointIndex];
        out.rule = TUI_POINT_ABSOLUTE;
    } else if (desired.hasGpuOffset) {
        if (out.ordinal >= desired.gpuOffsetExcludeLowCount) {
            out.targetMHz = out.baseMHz + desired.gpuOffsetMHz;
            out.rule = TUI_POINT_GPU_OFFSET;
        } else {
            out.rule = TUI_POINT_EXCLUDED;
        }
    }
    out.deltaMHz = out.targetMHz - out.baseMHz;
    return out;
}

int tui_nearest_graph_point(const TuiViewModel& vm, const TuiRect& graph,
                            int mouseX) {
    if (!vm.service || graph.width <= 1) return -1;
    const ServiceSnapshot& snapshot = vm.service->snapshot;
    unsigned int minMv = 0, maxMv = 0;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        if (snapshot.curve[i].freq_kHz == 0) continue;
        unsigned int mv = snapshot.curve[i].volt_uV / 1000u;
        if (!minMv || mv < minMv) minMv = mv;
        if (mv > maxMv) maxMv = mv;
    }
    if (!minMv || maxMv <= minMv) return -1;
    int clamped = mouseX < graph.x ? graph.x : mouseX;
    if (clamped >= graph.x + graph.width) clamped = graph.x + graph.width - 1;
    unsigned int wanted = minMv + (unsigned int)(((long long)(clamped - graph.x) *
        (maxMv - minMv)) / (graph.width - 1));
    int best = -1;
    unsigned int bestDistance = 0;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        if (snapshot.curve[i].freq_kHz == 0) continue;
        unsigned int mv = snapshot.curve[i].volt_uV / 1000u;
        unsigned int distance = mv > wanted ? mv - wanted : wanted - mv;
        if (best < 0 || distance < bestDistance) {
            best = i;
            bestDistance = distance;
        }
    }
    return best;
}

TuiCanvas::TuiCanvas(const TuiViewModel& vm, TuiLayout* layout)
    : vm_(vm), layout_(layout) {}

void TuiCanvas::clear(TuiStyle style) {
    for (TuiCell& cellValue : layout_->cells) {
        strcpy(cellValue.glyph, " ");
        cellValue.style = (uint8_t)style;
    }
}

void TuiCanvas::cell(int x, int y, const char* glyph, TuiStyle style) {
    if (x < 1 || y < 1 || x > layout_->width || y > layout_->height) return;
    TuiCell& target = layout_->cells[(size_t)(y - 1) * layout_->width + (x - 1)];
    snprintf(target.glyph, sizeof(target.glyph), "%s", glyph && glyph[0] ? glyph : " ");
    target.style = (uint8_t)style;
}

void TuiCanvas::text(int x, int y, int width, const char* value, TuiStyle style,
                     bool right) {
    if (width <= 0 || !value) return;
    int display = tui_display_columns(value);
    int cursor = right && display < width ? x + width - display : x;
    const unsigned char* p = (const unsigned char*)value;
    int written = 0;
    while (*p && written < width) {
        int bytes = utf8_character_bytes(*p);
        size_t remaining = strlen((const char*)p);
        if ((size_t)bytes > remaining) bytes = 1;
        char glyph[5] = {};
        for (int i = 0; i < bytes && p[i]; ++i) glyph[i] = (char)p[i];
        cell(cursor + written, y, glyph, style);
        p += bytes;
        ++written;
    }
}

void TuiCanvas::fill(int x, int y, int width, int height, TuiStyle style) {
    for (int row = 0; row < height; ++row)
        for (int col = 0; col < width; ++col)
            cell(x + col, y + row, " ", style);
}

void TuiCanvas::hline(int x, int y, int width, TuiStyle style) {
    for (int i = 0; i < width; ++i) cell(x + i, y, "─", style);
}

void TuiCanvas::vline(int x, int y, int height, TuiStyle style) {
    for (int i = 0; i < height; ++i) cell(x, y + i, "│", style);
}

void TuiCanvas::box(const TuiRect& rect, const char* title, const char* meta) {
    if (rect.width < 2 || rect.height < 2) return;
    fill(rect.x, rect.y, rect.width, rect.height, TUI_STYLE_PANEL);
    hline(rect.x + 1, rect.y, rect.width - 2, TUI_STYLE_BORDER);
    hline(rect.x + 1, rect.y + rect.height - 1, rect.width - 2, TUI_STYLE_BORDER);
    vline(rect.x, rect.y + 1, rect.height - 2, TUI_STYLE_BORDER);
    vline(rect.x + rect.width - 1, rect.y + 1, rect.height - 2, TUI_STYLE_BORDER);
    cell(rect.x, rect.y, "┌", TUI_STYLE_BORDER);
    cell(rect.x + rect.width - 1, rect.y, "┐", TUI_STYLE_BORDER);
    cell(rect.x, rect.y + rect.height - 1, "└", TUI_STYLE_BORDER);
    cell(rect.x + rect.width - 1, rect.y + rect.height - 1, "┘", TUI_STYLE_BORDER);
    if (title && rect.width > 6)
        text(rect.x + 2, rect.y, rect.width - 4, title, TUI_STYLE_TITLE);
    if (meta && rect.width > 24)
        text(rect.x + rect.width / 2, rect.y, rect.width / 2 - 2,
             meta, TUI_STYLE_MUTED, true);
}

void TuiCanvas::register_action(const TuiRect& rect, ActionType type,
                                int index, int value, int context) {
    if (rect.width <= 0 || rect.height <= 0 || rect.x < 1 || rect.y < 1 ||
        rect.x + rect.width - 1 > layout_->width ||
        rect.y + rect.height - 1 > layout_->height) return;
    layout_->actions.push_back(ClickAction{rect.x, rect.y,
        rect.x + rect.width - 1, rect.y + rect.height - 1,
        type, index, value, context});
}

int TuiCanvas::button(const TuiRect& rect, const char* label, ActionType type,
                      int index, int value, bool selected) {
    int actionIndex = (int)layout_->actions.size();
    fill(rect.x, rect.y, rect.width, rect.height,
         selected ? TUI_STYLE_BUTTON_SELECTED : TUI_STYLE_BUTTON);
    if (rect.width >= 2) {
        cell(rect.x, rect.y, "[", selected ? TUI_STYLE_GREEN : TUI_STYLE_BORDER);
        cell(rect.x + rect.width - 1, rect.y, "]",
             selected ? TUI_STYLE_GREEN : TUI_STYLE_BORDER);
    }
    int labelWidth = tui_display_columns(label ? label : "");
    int labelX = labelWidth > rect.width - 2
        ? rect.x + 1 : rect.x + (rect.width - labelWidth) / 2;
    text(labelX, rect.y, rect.width - 2, label ? label : "",
         selected ? TUI_STYLE_GREEN : TUI_STYLE_TEXT);
    register_action(rect, type, index, value);
    return actionIndex;
}

bool TuiCanvas::editing(TuiField fieldValue, int index) const {
    return vm_.editing && vm_.editField == fieldValue && vm_.editIndex == index;
}

int TuiCanvas::field(const TuiRect& rect, const char* value, TuiField fieldValue,
                     int index) {
    bool active = editing(fieldValue, index);
    int actionIndex = (int)layout_->actions.size();
    fill(rect.x, rect.y, rect.width, rect.height,
         active ? TUI_STYLE_FIELD_ACTIVE : TUI_STYLE_FIELD);
    cell(rect.x, rect.y, "[", active ? TUI_STYLE_CYAN : TUI_STYLE_BORDER);
    cell(rect.x + rect.width - 1, rect.y, "]",
         active ? TUI_STYLE_CYAN : TUI_STYLE_BORDER);
    const char* shown = active && vm_.editText ? vm_.editText : value;
    int display = tui_display_columns(shown ? shown : "");
    int start = rect.x + 1 + ((rect.width - 2 - display) > 0
        ? (rect.width - 2 - display) / 2 : 0);
    text(start, rect.y, rect.width - 2, shown ? shown : "",
         active ? TUI_STYLE_CYAN : TUI_STYLE_TEXT);
    register_action(rect, ACTION_FIELD_EDIT, (int)fieldValue, index);
    return actionIndex;
}

void TuiCanvas::checkbox(int x, int y, int state, TuiStyle style) {
    cell(x, y, "[", style);
    cell(x + 1, y, state == 1 ? "✓" : state == 2 ? "•" : " ", style);
    cell(x + 2, y, "]", style);
}

void tui_format_int(char* buffer, size_t size, int value, bool sign) {
    if (!buffer || !size) return;
    snprintf(buffer, size, sign && value > 0 ? "+%d" : "%d", value);
}

void tui_format_gpu_summary(const TuiViewModel& vm, char* buffer, size_t size) {
    if (!buffer || !size) return;
    snprintf(buffer, size, "%s", vm.selectedGpu && vm.selectedGpu[0]
        ? vm.selectedGpu : "unselected GPU");
}

void build_tui_layout(const TuiViewModel& vm, int width, int height,
                      TuiLayout* out) {
    if (!out) return;
    *out = TuiLayout{};
    out->width = width > 0 ? width : 1;
    out->height = height > 0 ? height : 1;
    out->requiredCols = 72;
    out->requiredRows = 24;
    out->tooSmall = width < out->requiredCols || height < out->requiredRows;
    out->breakpoint = out->tooSmall ? TUI_BREAKPOINT_TOO_SMALL
        : width >= 140 && height >= 36 ? TUI_BREAKPOINT_WIDE
        : width >= 100 ? TUI_BREAKPOINT_MEDIUM
        : TUI_BREAKPOINT_COMPACT;
    out->cells.resize((size_t)out->width * out->height);
    TuiCanvas canvas(vm, out);
    canvas.clear();

    if (out->tooSmall) {
        canvas.text(2, 2, out->width - 2, "Green Curve Linux TUI",
                    TUI_STYLE_TITLE);
        char message[128] = {};
        snprintf(message, sizeof(message),
                 "Terminal too small: need 72x24, got %dx%d.", width, height);
        canvas.text(2, 4, out->width - 2, message, TUI_STYLE_RED);
        canvas.text(2, 6, out->width - 2,
                    "Resize the terminal; no hidden control is active.",
                    TUI_STYLE_MUTED);
        return;
    }

    draw_header(&canvas);
    TuiRect content{1, 4, width, height - 6};
    if (vm.tab == TUI_TAB_FAN) tui_draw_fan_tab(&canvas, content);
    else if (vm.tab == TUI_TAB_PROFILES) tui_draw_profiles_tab(&canvas, content);
    else tui_draw_vf_tab(&canvas, content);
    draw_footer(&canvas);
}
