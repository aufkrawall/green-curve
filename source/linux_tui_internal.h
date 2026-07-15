// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_LINUX_TUI_INTERNAL_H
#define GREEN_CURVE_LINUX_TUI_INTERNAL_H

#include "linux_daemon.h"
#include "linux_port.h"
#include "linux_tui_layout.h"

#include <string>
#include <vector>

struct TuiEditState {
    bool active;
    TuiField field;
    int index;
    char text[32];
};

struct TuiState {
    DesiredSettings desired;
    DesiredSettings acceptedDesired;
    ServiceResponse service;
    bool serviceOnline;
    bool draftAttached;
    bool dirty;
    int currentSlot;
    TuiTab tab;
    int selectedPoint;
    int vfScroll;
    int fanScroll;
    int focusIndex;
    bool running;
    bool forceFullRender;
    int renderedWidth;
    int renderedHeight;
    unsigned long long nextTelemetryMs;
    unsigned long long escapePendingSince;
    char configPath[LINUX_PATH_MAX];
    char status[512];
    ProbeSummary probe;
    GpuAdapterInfo targetGpu;
    TuiEditState edit;
    TuiLayout layout;
    std::vector<std::string> renderedRows;
    std::string inputBuffer;
};

enum TuiInputType {
    TUI_INPUT_NONE = 0,
    TUI_INPUT_CHARACTER,
    TUI_INPUT_ESCAPE,
    TUI_INPUT_ENTER,
    TUI_INPUT_BACKSPACE,
    TUI_INPUT_TAB,
    TUI_INPUT_SHIFT_TAB,
    TUI_INPUT_UP,
    TUI_INPUT_DOWN,
    TUI_INPUT_LEFT,
    TUI_INPUT_RIGHT,
    TUI_INPUT_PAGE_UP,
    TUI_INPUT_PAGE_DOWN,
    TUI_INPUT_CTRL_PAGE_UP,
    TUI_INPUT_CTRL_PAGE_DOWN,
    TUI_INPUT_HOME,
    TUI_INPUT_END,
    TUI_INPUT_F1,
    TUI_INPUT_MOUSE,
};

struct TuiInputEvent {
    TuiInputType type;
    char character;
    int mouseX;
    int mouseY;
    int mouseButton;
    bool mousePress;
};

unsigned long long tui_monotonic_ms();
bool tui_refresh_service(TuiState* state, bool userRequested,
                         const GpuAdapterInfo* requestedTarget = nullptr);
void tui_recompute_dirty(TuiState* state);
void tui_apply_action(TuiState* state, const ClickAction& action);
void tui_begin_edit(TuiState* state, TuiField field, int index);
void tui_commit_edit(TuiState* state);
void tui_cancel_edit(TuiState* state);
void tui_handle_character(TuiState* state, char character);
void tui_handle_event(TuiState* state, const TuiInputEvent& event);
void tui_render(TuiState* state);
bool tui_read_input(TuiState* state);
bool tui_parse_next_event(std::string* buffer, TuiInputEvent* event);

#endif
