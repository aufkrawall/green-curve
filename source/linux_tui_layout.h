// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Pure, terminal-I/O-free layout model for the dependency-free Linux TUI.
// Rendering and hit testing consume the same cell grid and action rectangles;
// resizing can therefore never leave stale or offset mouse targets.

#ifndef GREEN_CURVE_LINUX_TUI_LAYOUT_H
#define GREEN_CURVE_LINUX_TUI_LAYOUT_H

#include <stdint.h>
#include <string>
#include <vector>

#include "gpu_core.h"

enum TuiTab {
    TUI_TAB_VF = 0,
    TUI_TAB_FAN = 1,
    TUI_TAB_PROFILES = 2,
};

enum TuiBreakpoint {
    TUI_BREAKPOINT_TOO_SMALL = 0,
    TUI_BREAKPOINT_COMPACT = 1,
    TUI_BREAKPOINT_MEDIUM = 2,
    TUI_BREAKPOINT_WIDE = 3,
};

enum TuiStyle : uint8_t {
    TUI_STYLE_DEFAULT = 0,
    TUI_STYLE_PANEL,
    TUI_STYLE_BORDER,
    TUI_STYLE_TITLE,
    TUI_STYLE_TEXT,
    TUI_STYLE_MUTED,
    TUI_STYLE_DIM,
    TUI_STYLE_GREEN,
    TUI_STYLE_CYAN,
    TUI_STYLE_ORANGE,
    TUI_STYLE_RED,
    TUI_STYLE_VIOLET,
    TUI_STYLE_BUTTON,
    TUI_STYLE_BUTTON_SELECTED,
    TUI_STYLE_FIELD,
    TUI_STYLE_FIELD_ACTIVE,
    TUI_STYLE_ROW_ALT,
    TUI_STYLE_ROW_SELECTED,
    TUI_STYLE_ROW_LOCKED,
    TUI_STYLE_COUNT,
};

enum TuiField {
    TUI_FIELD_NONE = 0,
    TUI_FIELD_GPU_OFFSET,
    TUI_FIELD_EXCLUDED_POINTS,
    TUI_FIELD_MEMORY_OFFSET,
    TUI_FIELD_POWER_LIMIT,
    TUI_FIELD_VF_TARGET,
    TUI_FIELD_FAN_FIXED,
    TUI_FIELD_FAN_POLL,
    TUI_FIELD_FAN_HYSTERESIS,
    TUI_FIELD_FAN_TEMPERATURE,
    TUI_FIELD_FAN_PERCENT,
};

enum ActionType {
    ACTION_NONE = 0,
    ACTION_QUIT,
    ACTION_TAB_SET,
    ACTION_GPU_SELECT_DELTA,
    ACTION_REFRESH,
    ACTION_APPLY,
    ACTION_APPLY_RESET,
    ACTION_FIELD_EDIT,
    ACTION_FIELD_STEP,
    ACTION_LOCK_CYCLE,
    ACTION_VF_SELECT,
    ACTION_VF_SCROLL,
    ACTION_FAN_MODE_SET,
    ACTION_FAN_POINT_ENABLE,
    ACTION_SLOT_DELTA,
    ACTION_LOAD,
    ACTION_SAVE,
    ACTION_CLEAR_PROFILE,
    ACTION_RESET_DRAFT,
    ACTION_PROBE,
    ACTION_WRITE_ASSETS,
    ACTION_EXPORT_LIVE_TEXT,
    ACTION_EXPORT_LIVE_JSON,
};

struct TuiRect {
    int x;
    int y;
    int width;
    int height;
};

struct TuiCell {
    char glyph[5];
    uint8_t style;
};

struct ClickAction {
    int x1;
    int y1;
    int x2;
    int y2;
    ActionType type;
    int index;
    int value;
    int context;
};

enum TuiPointRule {
    TUI_POINT_LIVE = 0,
    TUI_POINT_EXCLUDED,
    TUI_POINT_GPU_OFFSET,
    TUI_POINT_ABSOLUTE,
    TUI_POINT_FLATTEN_KNEE,
    TUI_POINT_FLATTEN_TAIL,
    TUI_POINT_HARD_PIN,
};

struct TuiPointValues {
    bool populated;
    int ordinal;
    unsigned int voltageMv;
    int baseMHz;
    int liveMHz;
    int targetMHz;
    int deltaMHz;
    TuiPointRule rule;
};

struct TuiViewModel {
    const DesiredSettings* desired;
    const ServiceResponse* service;
    int currentSlot;
    TuiTab tab;
    int selectedPoint;
    int vfScroll;
    int fanScroll;
    int focusIndex;
    bool dirty;
    bool serviceOnline;
    bool editing;
    TuiField editField;
    int editIndex;
    const char* editText;
    const char* configPath;
    const char* status;
    const char* selectedGpu;
    unsigned int gpuCount;
    bool probeCompleted;
    const char* probeSummary;
    const char* probeReportPath;
};

struct TuiLayout {
    int width;
    int height;
    int requiredCols;
    int requiredRows;
    TuiBreakpoint breakpoint;
    bool tooSmall;
    std::vector<TuiCell> cells;
    std::vector<ClickAction> actions;
    TuiRect graphRect;
    int vfFirstVisible;
    int vfVisibleRows;
};

int tui_display_columns(const std::string& text);
int tui_column_to_byte_offset(const std::string& text, int column);
bool tui_rect_contains(const TuiRect& rect, int x, int y);
bool tui_layout_actions_valid(const TuiLayout& layout);
const char* tui_point_rule_label(TuiPointRule rule);
TuiPointValues tui_point_values(const TuiViewModel& vm, int pointIndex);
int tui_nearest_graph_point(const TuiViewModel& vm, const TuiRect& graph,
                            int mouseX);
void build_tui_layout(const TuiViewModel& vm, int width, int height,
                      TuiLayout* out);

#endif  // GREEN_CURVE_LINUX_TUI_LAYOUT_H
