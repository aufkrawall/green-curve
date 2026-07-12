// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#pragma once

// BS_OWNERDRAW is a button type, not a checkbox type. Windows therefore does
// not promise BM_GETCHECK/BM_SETCHECK storage for it. Keep checked state in the
// owning dialog model and make every read, write, and toggle explicit.
struct UiCheckboxState {
    bool checked;
};

static inline bool ui_checkbox_state_get(const UiCheckboxState* state) {
    return state && state->checked;
}

static inline void ui_checkbox_state_set(UiCheckboxState* state, bool checked) {
    if (state) state->checked = checked;
}

static inline bool ui_checkbox_state_toggle(UiCheckboxState* state) {
    if (!state) return false;
    state->checked = !state->checked;
    return state->checked;
}
