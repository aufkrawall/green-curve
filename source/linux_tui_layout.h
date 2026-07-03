// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_tui_layout.h — pure, terminal-I/O-free layout builder for the Linux TUI.
//
// The whole screen is produced here as a list of plain-text lines plus a list
// of button hitboxes.  Every hitbox is derived from the same line list that the
// renderer prints, so a button's registered click/focus rectangle can never
// drift away from where the button is actually drawn.  This is the root-cause
// fix for the reported "mouse offset grows as you go down" bug, which came from
// a hand-maintained row counter that fell out of sync with the printed rows.
//
// This translation unit must stay free of POSIX headers so the regression test
// harness (compiled on the host) can include it directly.

#ifndef GREEN_CURVE_LINUX_TUI_LAYOUT_H
#define GREEN_CURVE_LINUX_TUI_LAYOUT_H

#include <string>
#include <vector>

#include "gpu_core.h"  // DesiredSettings, FAN_MODE_*, VF_NUM_POINTS, etc.

enum ActionType {
    ACTION_NONE = 0,
    ACTION_QUIT,
    ACTION_SAVE,
    ACTION_LOAD,
    ACTION_RESET,
    ACTION_PROBE,
    ACTION_WRITE_ASSETS,
    ACTION_APPLY,          // push current in-memory settings to the root daemon
    ACTION_APPLY_RESET,    // reset the GPU to driver defaults via the daemon
    ACTION_SLOT_DELTA,
    ACTION_VF_PAGE_DELTA,
    ACTION_GPU_DELTA,
    ACTION_MEM_DELTA,
    ACTION_POWER_DELTA,
    ACTION_FAN_FIXED_DELTA,
    ACTION_FAN_MODE_SET,
    ACTION_POLL_DELTA,
    ACTION_HYST_DELTA,
    ACTION_FAN_POINT_ENABLE,
    ACTION_FAN_POINT_TEMP_DELTA,
    ACTION_FAN_POINT_PCT_DELTA,
    ACTION_VF_POINT_DELTA,
};

// A clickable / keyboard-focusable button.
//   x1..x2 / y1..y2 are 1-based *display* columns/rows (what the terminal
//   reports for a mouse click, and where the renderer positions the cursor).
//   byteStart/byteLen locate the "[label]" substring inside its line so the
//   renderer can splice in a focus highlight without recomputing widths.
struct ClickAction {
    int x1;
    int y1;
    int x2;
    int y2;
    int byteStart;
    int byteLen;
    ActionType type;
    int index;
    int value;
};

// Read-only view of the editable state the layout needs.  Kept deliberately
// small (plain fields only) so it is trivial to build in tests.
struct TuiViewModel {
    const DesiredSettings* desired;
    int currentSlot;
    int vfPage;
    const char* configPath;
    const char* status;
    bool probeCompleted;
    const char* probeSummary;
    const char* probeReportPath;
};

struct TuiLayout {
    std::vector<std::string> lines;    // plain-text rows, no trailing newline, no ANSI
    std::vector<ClickAction> actions;  // hitboxes, in natural (top-to-bottom) focus order
    int requiredRows;                  // rows the content needs (== lines.size())
    int requiredCols;                  // widest *interactive* line, in display columns
};

// Number of display columns a UTF-8 string occupies (continuation bytes
// 0x80-0xBF do not advance a column; every other byte counts as one).  ANSI is
// not expected in layout lines, so it is intentionally not special-cased.
int tui_display_columns(const std::string& text);

// Byte offset of the start of the given 1-based display column within `text`
// (clamped to text.size()).  Used by the renderer to clip over-wide info lines.
int tui_column_to_byte_offset(const std::string& text, int column);

// Build the full screen.  Pure: no terminal I/O, no globals.
void build_tui_layout(const TuiViewModel& vm, TuiLayout* out);

#endif  // GREEN_CURVE_LINUX_TUI_LAYOUT_H
