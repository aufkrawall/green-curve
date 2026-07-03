// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_tui_layout.cpp — see linux_tui_layout.h.  Pure layout builder shared by
// the renderer and the regression test; must not include POSIX headers.

#include "linux_tui_layout.h"

#include <stdio.h>

int tui_display_columns(const std::string& text) {
    int cols = 0;
    for (size_t i = 0; i < text.size(); i++) {
        unsigned char c = (unsigned char)text[i];
        if ((c & 0xC0) == 0x80) continue;  // UTF-8 continuation byte
        cols++;
    }
    return cols;
}

int tui_column_to_byte_offset(const std::string& text, int column) {
    if (column <= 1) return 0;
    int cols = 0;
    for (size_t i = 0; i < text.size(); i++) {
        unsigned char c = (unsigned char)text[i];
        if ((c & 0xC0) == 0x80) continue;  // do not advance on continuation bytes
        cols++;
        if (cols >= column) return (int)i;
    }
    return (int)text.size();
}

namespace {

// Display columns of the leading `byteCount` bytes of `s`.
int columns_of_prefix(const std::string& s, size_t byteCount) {
    if (byteCount > s.size()) byteCount = s.size();
    int cols = 0;
    for (size_t i = 0; i < byteCount; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c & 0xC0) == 0x80) continue;
        cols++;
    }
    return cols;
}

void append_text(std::string* line, const char* text) {
    line->append(text ? text : "");
}

// Append "[label]" to the line under construction and register a hitbox whose
// row is the row this line *will* occupy once emitted, and whose columns are
// the real display columns of the brackets.  Because the row is taken from the
// already-emitted line count, it is impossible for the hitbox to land on the
// wrong row.
void push_button(TuiLayout* out, std::string* line, ActionType type, int index,
                 int value, const char* label) {
    int row = (int)out->lines.size() + 1;             // 1-based row of this line
    int byteStart = (int)line->size();
    int colStart = columns_of_prefix(*line, line->size()) + 1;  // '[' display column

    line->push_back('[');
    append_text(line, label);
    line->push_back(']');

    int byteLen = (int)line->size() - byteStart;
    int colEnd = columns_of_prefix(*line, line->size());        // ']' display column

    ClickAction action = {};
    action.x1 = colStart;
    action.y1 = row;
    action.x2 = colEnd;
    action.y2 = row;
    action.byteStart = byteStart;
    action.byteLen = byteLen;
    action.type = type;
    action.index = index;
    action.value = value;
    out->actions.push_back(action);
}

// Commit the line under construction as a screen row and reset it.
void emit(TuiLayout* out, std::string* line) {
    out->lines.push_back(*line);
    line->clear();
}

void emit_text(TuiLayout* out, const char* text) {
    out->lines.push_back(text ? text : "");
}

}  // namespace

void build_tui_layout(const TuiViewModel& vm, TuiLayout* out) {
    out->lines.clear();
    out->actions.clear();
    out->requiredRows = 0;
    out->requiredCols = 0;

    const DesiredSettings& d = *vm.desired;
    char buffer[128] = {};
    std::string line;

    // Header block (informational — no buttons).
    emit_text(out, "Green Curve Linux TUI  daemon-backed config editor");

    snprintf(buffer, sizeof(buffer), "Config: %s", vm.configPath ? vm.configPath : "");
    emit_text(out, buffer);

    snprintf(buffer, sizeof(buffer), "Status: %s",
             (vm.status && vm.status[0]) ? vm.status : "Ready.");
    emit_text(out, buffer);

    emit_text(out,
              "Keys: arrows/Tab move  Enter=activate  g=apply-to-GPU  s save  "
              "l load  r reset  p probe  a assets  [ ] page  1-5 slot  q quit");

    // Profile slot row.
    line = "Profile slot: ";
    push_button(out, &line, ACTION_SLOT_DELTA, 0, -1, "<");
    append_text(&line, " ");
    snprintf(buffer, sizeof(buffer), "%d", vm.currentSlot);
    append_text(&line, buffer);
    append_text(&line, " ");
    push_button(out, &line, ACTION_SLOT_DELTA, 0, 1, ">");
    append_text(&line, "   ");
    push_button(out, &line, ACTION_LOAD, 0, 0, "Load");
    append_text(&line, " ");
    push_button(out, &line, ACTION_SAVE, 0, 0, "Save");
    append_text(&line, " ");
    push_button(out, &line, ACTION_RESET, 0, 0, "Reset");
    emit(out, &line);

    emit_text(out, "");
    emit_text(out, "General controls");

    line = "  GPU offset: ";
    push_button(out, &line, ACTION_GPU_DELTA, 0, -15, "-15");
    append_text(&line, " ");
    push_button(out, &line, ACTION_GPU_DELTA, 0, 15, "+15");
    append_text(&line, "   ");
    snprintf(buffer, sizeof(buffer), "%d MHz", d.gpuOffsetMHz);
    append_text(&line, buffer);
    emit(out, &line);

    line = "  Memory offset: ";
    push_button(out, &line, ACTION_MEM_DELTA, 0, -100, "-100");
    append_text(&line, " ");
    push_button(out, &line, ACTION_MEM_DELTA, 0, 100, "+100");
    append_text(&line, "   ");
    snprintf(buffer, sizeof(buffer), "%d MHz", d.memOffsetMHz);
    append_text(&line, buffer);
    emit(out, &line);

    line = "  Power limit: ";
    push_button(out, &line, ACTION_POWER_DELTA, 0, -5, "-5");
    append_text(&line, " ");
    push_button(out, &line, ACTION_POWER_DELTA, 0, 5, "+5");
    append_text(&line, "   ");
    snprintf(buffer, sizeof(buffer), "%d%%", d.powerLimitPct);
    append_text(&line, buffer);
    emit(out, &line);

    line = "  Fan mode: ";
    push_button(out, &line, ACTION_FAN_MODE_SET, 0, FAN_MODE_AUTO,
                d.fanMode == FAN_MODE_AUTO ? "*Auto*" : "Auto");
    append_text(&line, " ");
    push_button(out, &line, ACTION_FAN_MODE_SET, 0, FAN_MODE_FIXED,
                d.fanMode == FAN_MODE_FIXED ? "*Fixed*" : "Fixed");
    append_text(&line, " ");
    push_button(out, &line, ACTION_FAN_MODE_SET, 0, FAN_MODE_CURVE,
                d.fanMode == FAN_MODE_CURVE ? "*Curve*" : "Curve");
    emit(out, &line);

    line = "  Fan fixed pct: ";
    push_button(out, &line, ACTION_FAN_FIXED_DELTA, 0, -5, "-5");
    append_text(&line, " ");
    push_button(out, &line, ACTION_FAN_FIXED_DELTA, 0, 5, "+5");
    append_text(&line, "   ");
    snprintf(buffer, sizeof(buffer), "%d%%", d.fanPercent);
    append_text(&line, buffer);
    emit(out, &line);

    emit_text(out, "");
    emit_text(out, "Fan curve");

    line = "  Poll interval: ";
    push_button(out, &line, ACTION_POLL_DELTA, 0, -250, "-250");
    append_text(&line, " ");
    push_button(out, &line, ACTION_POLL_DELTA, 0, 250, "+250");
    append_text(&line, "   ");
    snprintf(buffer, sizeof(buffer), "%d ms", d.fanCurve.pollIntervalMs);
    append_text(&line, buffer);
    append_text(&line, "    Hysteresis: ");
    push_button(out, &line, ACTION_HYST_DELTA, 0, -1, "-1");
    append_text(&line, " ");
    push_button(out, &line, ACTION_HYST_DELTA, 0, 1, "+1");
    append_text(&line, "   ");
    snprintf(buffer, sizeof(buffer), "%d\xC2\xB0""C", d.fanCurve.hysteresisC);
    append_text(&line, buffer);
    emit(out, &line);

    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        line = "  ";
        snprintf(buffer, sizeof(buffer), "P%d ", i);
        append_text(&line, buffer);
        push_button(out, &line, ACTION_FAN_POINT_ENABLE, i, 0,
                    d.fanCurve.points[i].enabled ? "On" : "Off");
        append_text(&line, "  Temp ");
        push_button(out, &line, ACTION_FAN_POINT_TEMP_DELTA, i, -1, "-");
        append_text(&line, " ");
        push_button(out, &line, ACTION_FAN_POINT_TEMP_DELTA, i, 1, "+");
        append_text(&line, " ");
        snprintf(buffer, sizeof(buffer), "%3d\xC2\xB0""C", d.fanCurve.points[i].temperatureC);
        append_text(&line, buffer);
        append_text(&line, "   Pct ");
        push_button(out, &line, ACTION_FAN_POINT_PCT_DELTA, i, -1, "-");
        append_text(&line, " ");
        push_button(out, &line, ACTION_FAN_POINT_PCT_DELTA, i, 1, "+");
        append_text(&line, " ");
        snprintf(buffer, sizeof(buffer), "%3d%%", d.fanCurve.points[i].fanPercent);
        append_text(&line, buffer);
        emit(out, &line);
    }

    emit_text(out, "");
    snprintf(buffer, sizeof(buffer), "VF curve page %d/%d", vm.vfPage + 1, VF_NUM_POINTS / 16);
    emit_text(out, buffer);

    line = "  Page: ";
    push_button(out, &line, ACTION_VF_PAGE_DELTA, 0, -1, "Prev");
    append_text(&line, " ");
    push_button(out, &line, ACTION_VF_PAGE_DELTA, 0, 1, "Next");
    emit(out, &line);

    int firstPoint = vm.vfPage * 16;
    for (int row = 0; row < 16; row++) {
        int pointIndex = firstPoint + row;
        if (pointIndex >= VF_NUM_POINTS) break;
        line = "  ";
        snprintf(buffer, sizeof(buffer), "P%03d ", pointIndex);
        append_text(&line, buffer);
        push_button(out, &line, ACTION_VF_POINT_DELTA, pointIndex, -100, "-100");
        append_text(&line, " ");
        push_button(out, &line, ACTION_VF_POINT_DELTA, pointIndex, -15, "-15");
        append_text(&line, " ");
        push_button(out, &line, ACTION_VF_POINT_DELTA, pointIndex, 15, "+15");
        append_text(&line, " ");
        push_button(out, &line, ACTION_VF_POINT_DELTA, pointIndex, 100, "+100");
        append_text(&line, "   ");
        snprintf(buffer, sizeof(buffer), "%4u MHz", d.curvePointMHz[pointIndex]);
        append_text(&line, buffer);
        emit(out, &line);
    }

    emit_text(out, "");
    push_button(out, &line, ACTION_APPLY, 0, 0, "Apply");
    append_text(&line, " ");
    push_button(out, &line, ACTION_APPLY_RESET, 0, 0, "Reset GPU");
    append_text(&line, "   ");
    push_button(out, &line, ACTION_PROBE, 0, 0, "Probe");
    append_text(&line, " ");
    push_button(out, &line, ACTION_WRITE_ASSETS, 0, 0, "Write Assets");
    append_text(&line, " ");
    push_button(out, &line, ACTION_SAVE, 0, 0, "Save");
    append_text(&line, " ");
    push_button(out, &line, ACTION_LOAD, 0, 0, "Load");
    append_text(&line, " ");
    push_button(out, &line, ACTION_QUIT, 0, 0, "Quit");
    emit(out, &line);

    if (vm.probeCompleted) {
        snprintf(buffer, sizeof(buffer), "Probe: %s", vm.probeSummary ? vm.probeSummary : "");
        emit_text(out, buffer);
        snprintf(buffer, sizeof(buffer), "Report: %s", vm.probeReportPath ? vm.probeReportPath : "");
        emit_text(out, buffer);
    } else {
        emit_text(out, "Probe: not run in this session");
        emit_text(out, "Apply pushes settings live via the root daemon (Apply button or 'g').");
    }

    // Required size: rows == line count; the width gate only considers lines
    // that carry buttons, so long informational lines (paths, status, legend)
    // never inflate the minimum width — the renderer clips those instead.
    out->requiredRows = (int)out->lines.size();
    for (const ClickAction& action : out->actions) {
        int row = action.y1 - 1;
        if (row < 0 || row >= (int)out->lines.size()) continue;
        int width = tui_display_columns(out->lines[row]);
        if (width > out->requiredCols) out->requiredCols = width;
    }
}
