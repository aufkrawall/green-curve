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

// Repaint gate for owner-draw checkboxes whose tick is DERIVED at paint time
// (from config or app state) rather than stored in the control.  The only
// correct question is "does the value we would paint now differ from the value
// we last painted", and `painted` is written by the owner-draw handler itself so
// an out-of-band repaint cannot desynchronize it.
//
// Asking the control instead -- BM_GETCHECK on a BS_OWNERDRAW button -- always
// answers BST_UNCHECKED, which silently suppressed every checked -> unchecked
// repaint and left a stale tick on screen until the next full window redraw.
static inline bool ui_checkbox_state_needs_repaint(
    const UiCheckboxState* painted, bool checked) {
    // An unknown last-painted value must always repaint: nothing on screen can
    // be proven current.
    return !painted || painted->checked != checked;
}
