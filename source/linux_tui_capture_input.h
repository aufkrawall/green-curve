// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "linux_tui_internal.h"
#include <algorithm>

static inline void tui_capture_present_layout(TuiState& state) {
    int cancel = -1;
    if (state.capture.active && state.layout.captureVisible && !state.layout.tooSmall)
        for (size_t i = 0; i < state.layout.actions.size(); ++i)
            if (state.layout.actions[i].type == ACTION_CAPTURE_CANCEL) cancel = (int)i;
    if (cancel >= 0 && !state.capturePresented) state.focusIndex = cancel;
    state.capturePresented = cancel >= 0;
}
template<class Action, class Focus, class Activate, class Hit>
static inline bool tui_capture_modal_event(TuiState& state, const TuiInputEvent& event,
    Action action, Focus focus, Activate activate, Hit hit) {
    if (!state.capture.active) return false;
    if (event.type == TUI_INPUT_ESCAPE) action(ClickAction{0,0,0,0,ACTION_CAPTURE_CANCEL,0,0,0});
    else if (!state.capturePresented) return true;
    else if (event.type == TUI_INPUT_TAB || event.type == TUI_INPUT_SHIFT_TAB) focus(event.type == TUI_INPUT_TAB ? 1 : -1);
    else if (event.type == TUI_INPUT_ENTER || (event.type == TUI_INPUT_CHARACTER && event.character == ' ')) activate();
    else if (event.type == TUI_INPUT_PAGE_UP || event.type == TUI_INPUT_PAGE_DOWN ||
        (event.type == TUI_INPUT_MOUSE && event.mousePress && (event.mouseButton & 64))) {
        const bool up = event.type == TUI_INPUT_PAGE_UP || (event.type == TUI_INPUT_MOUSE && !(event.mouseButton & 1));
        const int amount = event.type == TUI_INPUT_MOUSE ? 1 : std::max(1, state.layout.captureVisibleRows);
        state.capture.scroll = std::clamp(state.capture.scroll + (up ? -amount : amount), 0,
            std::max(0, state.layout.captureLineCount - state.layout.captureVisibleRows));
    } else if (event.type == TUI_INPUT_MOUSE && event.mousePress && event.mouseButton == 0) {
        const int index = hit(event.mouseX, event.mouseY);
        if (index >= 0 && index < (int)state.layout.actions.size()) {
            state.focusIndex = index; action(state.layout.actions[index]);
        }
    }
    return true;
}
