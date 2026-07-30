// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// What a keystroke does to a TUI numeric field, and when the field's contents
// stop being pre-selected.
//
// Opening an editor used to seed the buffer with the current value and then
// APPEND every digit to it, so clicking a field showing `100` and typing `50`
// left `10050`.  The value had to be erased by hand first, which is not how any
// other numeric field the user meets behaves.
//
// The rule here is the one every GUI text box uses:
//
//   * entering a field selects its whole contents, so the first keystroke
//     REPLACES the value;
//   * clicking again into the field that is already open drops the selection,
//     so further keystrokes APPEND -- that is how a user asks to amend the
//     existing number rather than retype it.
//
// Pure, so both halves are asserted by the regression harness rather than only
// being reachable through a terminal.

#ifndef GREEN_CURVE_LINUX_TUI_EDIT_POLICY_H
#define GREEN_CURVE_LINUX_TUI_EDIT_POLICY_H

#include <stddef.h>
#include <string.h>

// True when a click lands on the field that is already open for editing.  The
// caller must then drop the selection and KEEP the buffer: committing and
// reopening would round-trip the value through the field's clamping and throw
// away a partially typed number the user is in the middle of entering.
static inline bool tui_edit_click_targets_active_field(
    bool editing, int activeField, int activeIndex,
    int clickedField, int clickedIndex) {
    return editing && activeField == clickedField && activeIndex == clickedIndex;
}

// Apply one typed character.  Returns whether it was accepted.
//
// A rejected keystroke (a letter, or a sign after the first position) leaves
// both the buffer and the selection untouched: it must not be the thing that
// silently discards the value the user can still see highlighted.
static inline bool tui_edit_insert_character(char* text, size_t size,
                                             bool* selectAll, char character) {
    if (!text || size == 0) return false;
    // With the contents selected the buffer is treated as empty, so the
    // accepted character lands at index 0 and the old value is gone.
    bool replacing = selectAll && *selectAll;
    size_t length = replacing ? 0 : strlen(text);
    bool digit = character >= '0' && character <= '9';
    // A sign is only meaningful in the first position -- which a replacement
    // always is, so typing "-50" over "100" works.
    bool sign = (character == '-' || character == '+') && length == 0;
    if (!digit && !sign) return false;
    if (length + 1 >= size) return false;
    text[length] = character;
    text[length + 1] = '\0';
    if (selectAll) *selectAll = false;
    return true;
}

// Backspace over a selection clears the whole value, as in any GUI field; with
// no selection it removes one character.
static inline void tui_edit_backspace(char* text, bool* selectAll) {
    if (!text) return;
    if (selectAll && *selectAll) {
        text[0] = '\0';
        *selectAll = false;
        return;
    }
    size_t length = strlen(text);
    if (length > 0) text[length - 1] = '\0';
}

#endif  // GREEN_CURVE_LINUX_TUI_EDIT_POLICY_H
