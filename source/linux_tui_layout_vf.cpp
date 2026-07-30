// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "linux_tui_layout_internal.h"
#include "linux_tui_authority.h"

#include <stdio.h>
#include <string.h>
#include <vector>

namespace {

struct PlotPoint {
    int x;
    int y;
};

void utf8_codepoint(unsigned int codepoint, char out[5]) {
    memset(out, 0, 5);
    if (codepoint <= 0x7F) {
        out[0] = (char)codepoint;
    } else if (codepoint <= 0x7FF) {
        out[0] = (char)(0xC0 | (codepoint >> 6));
        out[1] = (char)(0x80 | (codepoint & 0x3F));
    } else {
        out[0] = (char)(0xE0 | (codepoint >> 12));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char)(0x80 | (codepoint & 0x3F));
    }
}

int braille_bit(int x, int y) {
    static const int left[4] = {1, 2, 4, 64};
    static const int right[4] = {8, 16, 32, 128};
    return (x & 1) ? right[y & 3] : left[y & 3];
}

void raster_line(std::vector<unsigned char>* mask, int width, int height,
                 PlotPoint a, PlotPoint b) {
    int virtualWidth = width * 2;
    int virtualHeight = height * 4;
    int dx = a.x > b.x ? a.x - b.x : b.x - a.x;
    int sx = a.x < b.x ? 1 : -1;
    int dy = -(a.y > b.y ? a.y - b.y : b.y - a.y);
    int sy = a.y < b.y ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        if (a.x >= 0 && a.y >= 0 && a.x < virtualWidth &&
            a.y < virtualHeight) {
            size_t cellIndex = (size_t)(a.y / 4) * width + (a.x / 2);
            (*mask)[cellIndex] |= (unsigned char)braille_bit(a.x, a.y);
        }
        if (a.x == b.x && a.y == b.y) break;
        int twice = error * 2;
        if (twice >= dy) { error += dy; a.x += sx; }
        if (twice <= dx) { error += dx; a.y += sy; }
    }
}

void draw_curve_trace(TuiCanvas* c, const TuiRect& plot,
                      const std::vector<PlotPoint>& points,
                      TuiStyle style, bool sparse) {
    if (plot.width <= 0 || plot.height <= 0 || points.empty()) return;
    std::vector<unsigned char> mask((size_t)plot.width * plot.height, 0);
    if (sparse) {
        for (size_t i = 0; i < points.size(); i += 3) {
            PlotPoint p = points[i];
            if (p.x >= 0 && p.y >= 0 && p.x < plot.width * 2 &&
                p.y < plot.height * 4) {
                mask[(size_t)(p.y / 4) * plot.width + (p.x / 2)] |=
                    (unsigned char)braille_bit(p.x, p.y);
            }
        }
    } else {
        for (size_t i = 1; i < points.size(); ++i)
            raster_line(&mask, plot.width, plot.height,
                        points[i - 1], points[i]);
    }
    for (int y = 0; y < plot.height; ++y) {
        for (int x = 0; x < plot.width; ++x) {
            unsigned char bits = mask[(size_t)y * plot.width + x];
            if (!bits) continue;
            char glyph[5] = {};
            utf8_codepoint(0x2800u + bits, glyph);
            c->cell(plot.x + x, plot.y + y, glyph, style);
        }
    }
}

// Widest Y-axis label the plot will reserve room for, plus one separating
// column.  "3187" is four digits on the RTX 5070; five keeps headroom without
// eating into a plot that has to survive a 100-column terminal.
const int TUI_GRAPH_Y_LABEL_WIDTH = 5;
// Minimum plot width, unchanged from before the axes existed.  The Y labels are
// only reserved when the plot still clears it afterwards, so a narrow terminal
// silently loses the labels rather than the graph.
const int TUI_GRAPH_MIN_PLOT_WIDTH = 12;

// Value shown against plot row `row`.  Row 0 is the top of the range and the
// last row the bottom, which is how a reader interprets a text-mode chart --
// the braille cell resolves four sub-rows, but the label is a row-level guide.
int graph_row_value(int row, int rows, int lowValue, int highValue) {
    if (rows <= 1) return highValue;
    long long span = (long long)(highValue - lowValue);
    return highValue - (int)((span * row) / (rows - 1));
}

// Inverse of the X mapping the trace and tui_nearest_graph_point() both use, so
// a tick label names the same voltage a click on that column selects.
unsigned int graph_column_voltage(int column, int columns, unsigned int minMv,
                                  unsigned int maxMv) {
    if (columns <= 1) return minMv;
    return minMv + (unsigned int)(((long long)column * (maxMv - minMv)) /
                                  (columns - 1));
}

// Right-aligned MHz labels down the left margin, on the same rows as the
// gridlines so the number and the line it names are the same row.
void draw_vf_graph_y_axis(TuiCanvas* c, const TuiRect& plot, int labelWidth,
                          int minMHz, int maxMHz) {
    if (labelWidth <= 1) return;
    for (int row = 0; row < plot.height; ++row) {
        // Gridlines every third row, and always the bottom row, so the floor of
        // the range is stated even when it is not on a gridline.
        bool gridRow = row % 3 == 0;
        bool lastRow = row == plot.height - 1;
        if (!gridRow && !lastRow) continue;
        char label[16] = {};
        snprintf(label, sizeof(label), "%d",
                 graph_row_value(row, plot.height, minMHz, maxMHz));
        c->text(plot.x - labelWidth, plot.y + row, labelWidth - 1, label,
                TUI_STYLE_MUTED, true);
    }
}

// Voltage ticks under the plot, composed into one row buffer rather than
// written tick by tick: overlapping c->text() calls would let a wide label
// silently overwrite its neighbour's digits and read as a wrong number.
void draw_vf_graph_x_axis(TuiCanvas* c, const TuiRect& plot, int row,
                          unsigned int minMv, unsigned int maxMv) {
    if (plot.width < TUI_GRAPH_MIN_PLOT_WIDTH) return;
    std::string axis((size_t)plot.width, ' ');

    // One tick per ~14 columns: dense enough to read a voltage off the graph,
    // sparse enough that labels never collide on a 100-column terminal.
    int tickCount = plot.width / 14;
    if (tickCount < 2) tickCount = 2;
    if (tickCount > 8) tickCount = 8;
    // Right to left, so the highest voltage -- the end of the curve, and the
    // one number a reader actually looks for -- is placed first and can never
    // be the tick that gets dropped for want of room.
    for (int tick = tickCount - 1; tick >= 0; --tick) {
        int column = (plot.width - 1) * tick / (tickCount - 1);
        char label[16] = {};
        // The unit rides on the last tick rather than owning the right edge on
        // its own: a separate "mV" there collided with the maximum voltage and
        // silently suppressed it.
        snprintf(label, sizeof(label), tick == tickCount - 1 ? "%u mV" : "%u",
                 graph_column_voltage(column, plot.width, minMv, maxMv));
        size_t length = strlen(label);
        // Centred on its column, then pulled inside the plot at both ends.
        int start = column - (int)(length / 2);
        if (start < 0) start = 0;
        if (start + (int)length > plot.width) start = plot.width - (int)length;
        if (start < 0) continue;
        // One blank column of separation on each side, or the tick is dropped:
        // a half-printed number is worse than a missing one.
        bool free_ = true;
        for (int i = start - 1; i <= start + (int)length && free_; ++i) {
            if (i < 0 || i >= plot.width) continue;
            if (axis[(size_t)i] != ' ') free_ = false;
        }
        if (!free_) continue;
        axis.replace((size_t)start, length, label);
    }
    c->text(plot.x, row, plot.width, axis.c_str(), TUI_STYLE_MUTED);
}

void draw_vf_graph(TuiCanvas* c, const TuiRect& panel) {
    const TuiViewModel& vm = c->view();
    TuiLayout* out = c->layout();
    c->box(panel, "VOLTAGE / FREQUENCY",
           "click selects • preview only");
    if (!vm.serviceOnline || !vm.service || vm.service->snapshot.numPopulated <= 0) {
        int messageY = panel.y + panel.height / 2;
        if (!vm.serviceOnline || !vm.service) {
            c->text(panel.x + 3, messageY, panel.width - 6,
                    "Daemon offline — no authoritative VF snapshot",
                    TUI_STYLE_RED);
        } else {
            char reason[256] = {};
            const ServiceGpuHealth& health = vm.service->snapshot.health;
            snprintf(reason, sizeof(reason), "%s%s%s",
                service_gpu_health_reason_name(health.reason),
                health.detail[0] ? " — " : "", health.detail);
            c->text(panel.x + 3, messageY - 1, panel.width - 6,
                    reason, TUI_STYLE_ORANGE);
            c->text(panel.x + 3, messageY + 1, panel.width - 6,
                    linux_tui_health_remediation(health.reason),
                    TUI_STYLE_MUTED);
        }
        return;
    }

    // Reserve the left margin for the MHz scale, but only while the plot still
    // clears its own minimum width afterwards: on a narrow terminal the graph
    // itself matters more than the numbers beside it.
    TuiRect plot{panel.x + 3, panel.y + 2,
                 panel.width - 6, panel.height - 4};
    int yLabelWidth = 0;
    if (plot.width - TUI_GRAPH_Y_LABEL_WIDTH >= TUI_GRAPH_MIN_PLOT_WIDTH) {
        yLabelWidth = TUI_GRAPH_Y_LABEL_WIDTH;
        plot.x += yLabelWidth;
        plot.width -= yLabelWidth;
    }
    if (plot.width < TUI_GRAPH_MIN_PLOT_WIDTH || plot.height < 4) return;
    out->graphRect = plot;
    c->fill(plot.x, plot.y, plot.width, plot.height, TUI_STYLE_DEFAULT);
    for (int row = 0; row < plot.height; row += 3)
        c->hline(plot.x, plot.y + row, plot.width, TUI_STYLE_DIM);

    unsigned int minMv = 0, maxMv = 0;
    int minMHz = 0, maxMHz = 0;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        TuiPointValues values = tui_point_values(vm, i);
        if (!values.populated) continue;
        if (!minMv || values.voltageMv < minMv) minMv = values.voltageMv;
        if (values.voltageMv > maxMv) maxMv = values.voltageMv;
        int low = values.liveMHz < values.targetMHz
            ? values.liveMHz : values.targetMHz;
        int high = values.liveMHz > values.targetMHz
            ? values.liveMHz : values.targetMHz;
        if (!minMHz || low < minMHz) minMHz = low;
        if (high > maxMHz) maxMHz = high;
    }
    if (!minMv || maxMv <= minMv || maxMHz <= minMHz) return;
    int range = maxMHz - minMHz;
    minMHz = (minMHz / 250) * 250;
    maxMHz = ((maxMHz + 249) / 250) * 250;
    if (maxMHz <= minMHz) maxMHz = minMHz + (range > 0 ? range : 500);

    std::vector<PlotPoint> live;
    std::vector<PlotPoint> target;
    int virtualWidth = plot.width * 2 - 1;
    int virtualHeight = plot.height * 4 - 1;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        TuiPointValues values = tui_point_values(vm, i);
        if (!values.populated) continue;
        int x = (int)(((long long)(values.voltageMv - minMv) * virtualWidth) /
                      (maxMv - minMv));
        int liveY = virtualHeight - (int)(((long long)(values.liveMHz - minMHz) *
                    virtualHeight) / (maxMHz - minMHz));
        int targetY = virtualHeight - (int)(((long long)(values.targetMHz - minMHz) *
                      virtualHeight) / (maxMHz - minMHz));
        live.push_back(PlotPoint{x, liveY});
        target.push_back(PlotPoint{x, targetY});
    }
    draw_curve_trace(c, plot, live, TUI_STYLE_CYAN, true);
    draw_curve_trace(c, plot, target, TUI_STYLE_GREEN, false);

    TuiPointValues selected = tui_point_values(vm, vm.selectedPoint);
    if (selected.populated) {
        int selectedX = plot.x + (int)(((long long)(selected.voltageMv - minMv) *
            (plot.width - 1)) / (maxMv - minMv));
        c->vline(selectedX, plot.y, plot.height, TUI_STYLE_CYAN);
        c->cell(selectedX, plot.y + plot.height / 2, "●", TUI_STYLE_CYAN);
        char tooltip[96] = {};
        snprintf(tooltip, sizeof(tooltip), "#%d  %u mV  %d MHz",
                 vm.selectedPoint, selected.voltageMv, selected.targetMHz);
        c->text(plot.x + 1, plot.y, plot.width - 2, tooltip, TUI_STYLE_CYAN);
    }
    // Axes replace the single centred "450-1240 mV - 180-3187 MHz" summary the
    // panel used to end with: the endpoints were the only two numbers on it, so
    // nothing between them could be read off the trace at all.
    draw_vf_graph_y_axis(c, plot, yLabelWidth, minMHz, maxMHz);
    draw_vf_graph_x_axis(c, plot, panel.y + panel.height - 2, minMv, maxMv);
    // Unit for the Y scale, on the free row between the panel title and the
    // plot so it cannot cost the graph a row.
    if (yLabelWidth > 1) {
        c->text(plot.x - yLabelWidth, panel.y + 1, yLabelWidth - 1, "MHz",
                TUI_STYLE_MUTED, true);
    }
    c->register_action(plot, ACTION_VF_SELECT, -1, 0);
}

void add_stepper(TuiCanvas* c, int x, int y, int width, const char* label,
                 TuiField field, int context, int value, int delta,
                 const char* unit, bool enabled = true) {
    int labelWidth = width >= 46 ? 24 : 16;
    int unitWidth = unit && unit[0] ? (int)strlen(unit) + 1 : 0;
    int fieldWidth = 10;
    int minusX = x + labelWidth;
    int fieldX = minusX + 4;
    int plusX = fieldX + fieldWidth + 1;
    c->text(x, y, labelWidth - 1, label,
            enabled ? TUI_STYLE_TEXT : TUI_STYLE_DIM);
    char formatted[32] = {};
    bool sign = field == TUI_FIELD_GPU_OFFSET || field == TUI_FIELD_MEMORY_OFFSET;
    tui_format_int(formatted, sizeof(formatted), value, sign);
    if (!enabled) {
        c->text(fieldX, y, fieldWidth, formatted, TUI_STYLE_DIM, true);
        if (unitWidth > 0)
            c->text(plusX + 4, y, unitWidth, unit, TUI_STYLE_DIM);
        return;
    }
    int minusAction = c->button(TuiRect{minusX, y, 3, 1}, "−",
                                 ACTION_FIELD_STEP, (int)field, -delta);
    c->layout()->actions[minusAction].context = context;
    c->field(TuiRect{fieldX, y, fieldWidth, 1}, formatted, field, context);
    int plusAction = c->button(TuiRect{plusX, y, 3, 1}, "+",
                               ACTION_FIELD_STEP, (int)field, delta);
    c->layout()->actions[plusAction].context = context;
    if (unitWidth > 0)
        c->text(plusX + 4, y, unitWidth, unit, TUI_STYLE_MUTED);
}

void draw_vf_controls(TuiCanvas* c, const TuiRect& panel, bool horizontal) {
    const TuiViewModel& vm = c->view();
    const DesiredSettings& desired = *vm.desired;
    bool vfAvailable = vm.serviceOnline && vm.service &&
        (vm.service->snapshot.health.availableMutationDomains &
            SERVICE_MUTATION_DOMAIN_VF_CURVE) != 0;
    gc_u32 available = vm.serviceOnline && vm.service
        ? vm.service->snapshot.health.availableMutationDomains
        : SERVICE_MUTATION_DOMAIN_ALL;
    bool gpuUsesVf = (service_desired_mutation_domains(&desired) &
        SERVICE_MUTATION_DOMAIN_VF_CURVE) != 0;
    bool gpuAvailable = gpuUsesVf ? vfAvailable :
        (available & SERVICE_MUTATION_DOMAIN_GPU_OFFSET) != 0;
    bool memoryAvailable =
        (available & SERVICE_MUTATION_DOMAIN_MEM_OFFSET) != 0;
    bool powerAvailable =
        (available & SERVICE_MUTATION_DOMAIN_POWER) != 0;
    char hardware[128] = {};
    if (vm.serviceOnline && vm.service && vm.service->controlState.valid) {
        const ControlState& actual = vm.service->controlState;
        char gpu[16] = "?", memory[16] = "?", power[16] = "?";
        if (actual.gpuOffsetReadbackValid)
            snprintf(gpu, sizeof(gpu), "%+d", actual.gpuOffsetMHz);
        if (actual.memOffsetReadbackValid)
            snprintf(memory, sizeof(memory), "%+d", actual.memOffsetMHz);
        if (actual.powerLimitReadbackValid)
            snprintf(power, sizeof(power), "%d", actual.powerLimitPct);
        snprintf(hardware, sizeof(hardware),
                 "actual G %s • M %s • P %s%%%s",
                 gpu, memory, power,
                 vm.intentReadback.divergedDomains ? " • OVERRIDDEN" :
                 vm.intentReadback.unavailableDomains ? " • partial" : "");
    } else {
        snprintf(hardware, sizeof(hardware), "hardware readback unavailable");
    }
    c->box(panel, "VF TUNING — CONFIGURED INTENT", hardware);
    if (horizontal) {
        int half = (panel.width - 4) / 2;
        add_stepper(c, panel.x + 2, panel.y + 2, half,
                     "GPU target", TUI_FIELD_GPU_OFFSET, 0,
                     desired.gpuOffsetMHz, 15, "MHz", gpuAvailable);
        add_stepper(c, panel.x + 2 + half, panel.y + 2, half,
                     "Exclude first", TUI_FIELD_EXCLUDED_POINTS, 0,
                     desired.gpuOffsetExcludeLowCount, 1, "pts", vfAvailable);
        add_stepper(c, panel.x + 2, panel.y + 4, half,
                     "Memory target", TUI_FIELD_MEMORY_OFFSET, 0,
                     desired.memOffsetMHz, 100, "MHz", memoryAvailable);
        add_stepper(c, panel.x + 2 + half, panel.y + 4, half,
                     "Power target", TUI_FIELD_POWER_LIMIT, 0,
                     desired.powerLimitPct, 1, "%", powerAvailable);
    } else {
        add_stepper(c, panel.x + 2, panel.y + 2, panel.width - 4,
                     "GPU target offset", TUI_FIELD_GPU_OFFSET, 0,
                     desired.gpuOffsetMHz, 15, "MHz", gpuAvailable);
        add_stepper(c, panel.x + 2, panel.y + 4, panel.width - 4,
                     "Exclude first VF", TUI_FIELD_EXCLUDED_POINTS, 0,
                     desired.gpuOffsetExcludeLowCount, 1, "pts", vfAvailable);
        add_stepper(c, panel.x + 2, panel.y + 6, panel.width - 4,
                     "Memory target", TUI_FIELD_MEMORY_OFFSET, 0,
                     desired.memOffsetMHz, 100, "MHz", memoryAvailable);
        add_stepper(c, panel.x + 2, panel.y + 8, panel.width - 4,
                     "Power target", TUI_FIELD_POWER_LIMIT, 0,
                     desired.powerLimitPct, 1, "%", powerAvailable);
    }

    int modeY = horizontal ? panel.y + 6 : panel.y + 10;
    if (modeY < panel.y + panel.height - 2) {
        c->text(panel.x + 2, modeY, 16, "Curve tail", TUI_STYLE_TEXT);
        int buttonX = panel.x + 18;
        if (vfAvailable) {
            c->button(TuiRect{buttonX, modeY, 7, 1}, "OFF", ACTION_LOCK_CYCLE,
                      vm.selectedPoint, LOCK_MODE_NONE, !desired.hasLock);
            c->button(TuiRect{buttonX + 8, modeY, 12, 1}, "✓ FLATTEN",
                      ACTION_LOCK_CYCLE, vm.selectedPoint, LOCK_MODE_FLATTEN,
                      desired.hasLock && desired.lockMode == LOCK_MODE_FLATTEN);
            c->button(TuiRect{buttonX + 21, modeY, 8, 1}, "• PIN",
                      ACTION_LOCK_CYCLE, vm.selectedPoint, LOCK_MODE_HARD,
                      desired.hasLock && desired.lockMode == LOCK_MODE_HARD);
            if (panel.width >= 58)
                c->text(buttonX + 30, modeY, panel.width - 50,
                        "2nd click pins", TUI_STYLE_MUTED);
        } else {
            c->text(buttonX, modeY, panel.width - 20,
                    "unavailable until a fresh VF snapshot validates",
                    TUI_STYLE_DIM);
        }
    }

    int selectedY = modeY + 2;
    if (selectedY < panel.y + panel.height - 1) {
        TuiPointValues selected = tui_point_values(vm, vm.selectedPoint);
        char label[96] = {};
        if (selected.populated) {
            snprintf(label, sizeof(label), "#%d • %u mV  Base %d  Live %d",
                     vm.selectedPoint, selected.voltageMv,
                     selected.baseMHz, selected.liveMHz);
        } else {
            snprintf(label, sizeof(label), "Select a populated VF point");
        }
        c->text(panel.x + 2, selectedY, panel.width - 4, label,
                TUI_STYLE_CYAN);
        if (selected.populated && selectedY + 1 < panel.y + panel.height - 1) {
            char target[24] = {};
            tui_format_int(target, sizeof(target), selected.targetMHz);
            c->text(panel.x + 2, selectedY + 1, 12, "Target MHz", TUI_STYLE_TEXT);
            if (vfAvailable)
                c->field(TuiRect{panel.x + 14, selectedY + 1, 10, 1}, target,
                         TUI_FIELD_VF_TARGET, vm.selectedPoint);
            else
                c->text(panel.x + 14, selectedY + 1, 10, target,
                        TUI_STYLE_DIM, true);
            c->text(panel.x + 26, selectedY + 1, panel.width - 28,
                    tui_point_rule_label(selected.rule), TUI_STYLE_GREEN);
        }
    }
}

void draw_vf_table(TuiCanvas* c, const TuiRect& panel) {
    const TuiViewModel& vm = c->view();
    TuiLayout* out = c->layout();
    // The hidden count is stated rather than silently dropped: a list that
    // starts at point 45 with no explanation is worse than the rows it removes.
    char hint[64] = {};
    int hiddenLow = (vm.serviceOnline && vm.service)
        ? tui_vf_hidden_low_point_count(vm) : 0;
    if (hiddenLow > 0) {
        snprintf(hint, sizeof(hint), "%d flat pts hidden • wheel / PgUp / PgDn",
                 hiddenLow);
    } else {
        snprintf(hint, sizeof(hint), "wheel / PgUp / PgDn");
    }
    c->box(panel, "VF POINTS — TARGET AND HARDWARE READBACK", hint);
    if (!vm.serviceOnline || !vm.service) return;
    bool vfAvailable =
        (vm.service->snapshot.health.availableMutationDomains &
            SERVICE_MUTATION_DOMAIN_VF_CURVE) != 0;
    int rows = panel.height - 4;
    if (rows < 1) return;
    out->vfVisibleRows = rows;
    out->vfFirstVisible = vm.vfScroll;
    bool wide = panel.width >= 120;
    bool medium = panel.width >= 88;
    int x = panel.x + 2;
    int modeX = x + 5;
    int mvX = modeX + 6;
    int baseX = mvX + 8;
    int targetX = medium ? baseX + (wide ? 10 : 0) : mvX + 8;
    int liveX = targetX + 12;
    int deltaX = liveX + 10;
    int ruleX = deltaX + 9;
    c->text(x, panel.y + 1, 4, "#", TUI_STYLE_MUTED);
    c->text(modeX, panel.y + 1, 5, "MODE", TUI_STYLE_MUTED);
    c->text(mvX, panel.y + 1, 7, "mV", TUI_STYLE_MUTED);
    if (wide) c->text(baseX, panel.y + 1, 9, "BASE", TUI_STYLE_MUTED);
    c->text(targetX, panel.y + 1, 11, "TARGET MHz", TUI_STYLE_MUTED);
    if (medium) {
        c->text(liveX, panel.y + 1, 9, "LIVE", TUI_STYLE_MUTED);
        c->text(deltaX, panel.y + 1, 8, "DELTA", TUI_STYLE_MUTED);
    }
    if (wide) c->text(ruleX, panel.y + 1, panel.x + panel.width - ruleX - 1,
                      "RULE / ACTION", TUI_STYLE_MUTED);

    int index = vm.vfScroll;
    int firstListed = tui_vf_first_listed_point(vm);
    if (index < firstListed) index = firstListed;
    int drawn = 0;
    for (; index < VF_NUM_POINTS && drawn < rows; ++index) {
        TuiPointValues values = tui_point_values(vm, index);
        if (!values.populated) continue;
        int y = panel.y + 2 + drawn;
        TuiStyle rowStyle = (drawn & 1) ? TUI_STYLE_ROW_ALT : TUI_STYLE_PANEL;
        if (index == vm.selectedPoint) rowStyle = TUI_STYLE_ROW_SELECTED;
        if (values.rule == TUI_POINT_FLATTEN_TAIL ||
            values.rule == TUI_POINT_HARD_PIN) rowStyle = TUI_STYLE_ROW_LOCKED;
        c->fill(panel.x + 1, y, panel.width - 2, 1, rowStyle);
        char number[16] = {}, mv[16] = {}, base[16] = {}, target[16] = {};
        char live[16] = {}, delta[16] = {};
        snprintf(number, sizeof(number), "%d", index);
        snprintf(mv, sizeof(mv), "%u", values.voltageMv);
        snprintf(base, sizeof(base), "%d", values.baseMHz);
        snprintf(target, sizeof(target), "%d", values.targetMHz);
        snprintf(live, sizeof(live), "%d", values.liveMHz);
        snprintf(delta, sizeof(delta), "%+d", values.deltaMHz);
        c->text(x, y, 4, number, index == vm.selectedPoint
            ? TUI_STYLE_CYAN : TUI_STYLE_TEXT, true);
        int mode = 0;
        if (vm.desired->hasLock && vm.desired->lockCi == index)
            mode = vm.desired->lockMode == LOCK_MODE_HARD ? 2 : 1;
        c->checkbox(modeX, y, mode, !vfAvailable ? TUI_STYLE_DIM :
            (mode ? TUI_STYLE_GREEN : TUI_STYLE_BORDER));
        if (vfAvailable)
            c->register_action(TuiRect{modeX, y, 3, 1}, ACTION_LOCK_CYCLE,
                               index, 3);
        c->text(mvX, y, 6, mv, TUI_STYLE_TEXT, true);
        c->register_action(TuiRect{x, y, modeX - x, 1},
                           ACTION_VF_SELECT, index, 0);
        c->register_action(TuiRect{mvX, y, 7, 1},
                           ACTION_VF_SELECT, index, 0);
        if (wide) c->text(baseX, y, 8, base, TUI_STYLE_MUTED, true);
        if (vfAvailable)
            c->field(TuiRect{targetX, y, 10, 1}, target,
                      TUI_FIELD_VF_TARGET, index);
        else
            c->text(targetX, y, 10, target, TUI_STYLE_DIM, true);
        if (medium) {
            c->text(liveX, y, 8, live, TUI_STYLE_CYAN, true);
            c->text(deltaX, y, 7, delta,
                    values.deltaMHz ? TUI_STYLE_ORANGE : TUI_STYLE_DIM, true);
        }
        if (wide) {
            c->text(ruleX, y, panel.x + panel.width - ruleX - 2,
                    tui_point_rule_label(values.rule),
                    values.rule == TUI_POINT_EXCLUDED ? TUI_STYLE_DIM
                    : TUI_STYLE_MUTED);
        }
        ++drawn;
    }
    c->text(panel.x + 2, panel.y + panel.height - 1, panel.width - 4,
            "[✓] flatten  [•] pin  •  Enter edits absolute target  •  Space cycles mode",
            TUI_STYLE_DIM);
}

}  // namespace

void tui_draw_vf_tab(TuiCanvas* canvas, const TuiRect& content) {
    TuiLayout* out = canvas->layout();
    if (out->breakpoint == TUI_BREAKPOINT_WIDE) {
        int topHeight = content.height / 2;
        if (topHeight < 14) topHeight = 14;
        if (topHeight > 19) topHeight = 19;
        int graphWidth = (content.width * 60) / 100;
        draw_vf_graph(canvas, TuiRect{2, content.y, graphWidth - 2, topHeight});
        draw_vf_controls(canvas,
            TuiRect{graphWidth + 1, content.y,
                    content.width - graphWidth, topHeight}, false);
        draw_vf_table(canvas,
            TuiRect{2, content.y + topHeight,
                    content.width - 2, content.height - topHeight});
    } else if (out->breakpoint == TUI_BREAKPOINT_MEDIUM) {
        int graphHeight = content.height >= 30 ? 10 : 8;
        int controlHeight = 9;
        draw_vf_graph(canvas,
            TuiRect{2, content.y, content.width - 2, graphHeight});
        draw_vf_controls(canvas,
            TuiRect{2, content.y + graphHeight, content.width - 2,
                    controlHeight}, true);
        draw_vf_table(canvas,
            TuiRect{2, content.y + graphHeight + controlHeight,
                    content.width - 2,
                    content.height - graphHeight - controlHeight});
    } else {
        int controlHeight = 12;
        draw_vf_controls(canvas,
            TuiRect{2, content.y, content.width - 2, controlHeight}, false);
        draw_vf_table(canvas,
            TuiRect{2, content.y + controlHeight, content.width - 2,
                    content.height - controlHeight});
    }
}
