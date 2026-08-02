// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_GUI_GRAPH_AXIS_POLICY_H
#define GREEN_CURVE_GUI_GRAPH_AXIS_POLICY_H

// The VF graph's frequency axis is derived from the plotted data instead of a
// fixed 500..3400 MHz band.  A fixed ceiling made high-clock curves (a large
// GPU offset, a high flatten target, an unsupported GPU's boosted curve) run
// flat against the top edge, while low-clock curves floated in a band of empty
// space above them.
//
// The rule keeps the axis on the existing 500 MHz grid so the labels stay
// round:
//   - ceiling = the next grid boundary at or above dataMax + headroom;
//   - floor   = the previous grid boundary at or below dataMin - headroom,
//     clamped to minAllowedMHz;
//   - the span never drops below minSpanMHz, so a tight curve cannot fill the
//     whole plot height.
// With no plottable data the caller gets a conservative fallback band of
// minAllowed..minAllowed+minSpan.

// True floor/ceil rounding on the grid.  C++ division truncates toward zero,
// which would make a negative pre-clamp floor step UP instead of DOWN; the
// axis clamps hide that today, but the helpers must not depend on the clamp.
static inline int gui_grid_floor(int value, int gridStep) {
    int quotient = value / gridStep;
    if (value % gridStep < 0) --quotient;
    return quotient * gridStep;
}

static inline int gui_grid_ceil(int value, int gridStep) {
    int quotient = value / gridStep;
    if (value % gridStep > 0) ++quotient;
    return quotient * gridStep;
}

struct GuiGraphAxisRange {
    int minMHz;
    int maxMHz;
};

static inline GuiGraphAxisRange gui_graph_frequency_axis(
    int dataMinMHz, int dataMaxMHz, int minAllowedMHz, int gridStepMHz,
    int minSpanMHz, int headroomMHz) {
    GuiGraphAxisRange out = {};
    if (dataMaxMHz <= 0) {
        out.minMHz = minAllowedMHz;
        out.maxMHz = minAllowedMHz + minSpanMHz;
        return out;
    }
    int ceiling = dataMaxMHz + headroomMHz;
    int maxMHz = gui_grid_ceil(ceiling, gridStepMHz);

    int floor = dataMinMHz - headroomMHz;
    int minMHz = gui_grid_floor(floor, gridStepMHz);
    if (minMHz < minAllowedMHz) minMHz = minAllowedMHz;

    if (maxMHz - minMHz < minSpanMHz) maxMHz = minMHz + minSpanMHz;
    out.minMHz = minMHz;
    out.maxMHz = maxMHz;
    return out;
}

// The voltage axis follows the same idea on the 50 mV grid: it starts at the
// first visible VF point's voltage (rounded down, clamped to minAllowedMv)
// instead of a fixed 700 mV, so a GPU whose curve begins at 750 mV does not
// show an empty 700-750 mV cell on the left, and ends at the last visible
// point rounded up (clamped to maxAllowedMv).  A minimum span keeps a tight
// curve from filling the whole width; when the span is too small it expands
// downward first so the right edge stays stable.
struct GuiGraphVoltageRange {
    int minMv;
    int maxMv;
};

static inline GuiGraphVoltageRange gui_graph_voltage_axis(
    int dataMinMv, int dataMaxMv, int minAllowedMv, int maxAllowedMv,
    int gridStepMv, int minSpanMv, int headroomMv) {
    GuiGraphVoltageRange out = {};
    if (dataMaxMv <= 0) {
        out.minMv = minAllowedMv;
        out.maxMv = maxAllowedMv;
        return out;
    }
    int ceiling = dataMaxMv + headroomMv;
    int maxMv = gui_grid_ceil(ceiling, gridStepMv);
    if (maxMv > maxAllowedMv) maxMv = maxAllowedMv;

    int floor = dataMinMv - headroomMv;
    int minMv = gui_grid_floor(floor, gridStepMv);
    if (minMv < minAllowedMv) minMv = minAllowedMv;

    if (maxMv - minMv < minSpanMv) {
        minMv = maxMv - minSpanMv;
        if (minMv < minAllowedMv) {
            minMv = minAllowedMv;
            maxMv = minAllowedMv + minSpanMv;
            if (maxMv > maxAllowedMv) maxMv = maxAllowedMv;
        }
    }
    out.minMv = minMv;
    out.maxMv = maxMv;
    return out;
}

#endif // GREEN_CURVE_GUI_GRAPH_AXIS_POLICY_H
