// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_editor_source_edit.h"

static inline bool tui_source_field(TuiField field) {
    return field == TUI_FIELD_SOURCE_POINT || field == TUI_FIELD_SOURCE_GRAPHICS || field == TUI_FIELD_SOURCE_MEMORY;
}
static inline ProfileEditorSourceField tui_source_field_kind(TuiField field) {
    return field == TUI_FIELD_SOURCE_GRAPHICS ? PROFILE_EDIT_GRAPHICS :
        field == TUI_FIELD_SOURCE_MEMORY ? PROFILE_EDIT_MEMORY : PROFILE_EDIT_POINT;
}
static inline bool tui_begin_source_edit(TuiState& state, TuiField field, int index) {
    if (!tui_source_field(field) || !state.document.valid || state.sourceCheckVisible || state.profileLoadPending ||
        !profile_editor_source_editable(state.document.value, tui_source_field_kind(field), index)) return false;
    state.edit = {}; state.edit.active = state.edit.selectAll = true;
    state.edit.field = field; state.edit.index = index;
    const auto seed = profile_editor_source_seed(state.document.value, tui_source_field_kind(field), index);
    std::snprintf(state.edit.text, sizeof(state.edit.text), "%s", seed.c_str());
    std::snprintf(state.status, sizeof(state.status), "%s",
        field == TUI_FIELD_SOURCE_POINT ? "Type absolute MHz (up to 6 decimals); Enter converts only this source point; Esc cancels" :
        "Type physical offset MHz (up to 6 decimals); Enter changes only this source intent; Esc cancels");
    return true;
}
static inline bool tui_commit_source_edit(TuiState& state) {
    if (!state.edit.active || !tui_source_field(state.edit.field)) return false;
    if (!state.document.valid || state.profileLoadPending ||
        !profile_editor_document_type(state.document, tui_source_field_kind(state.edit.field), state.edit.index, state.edit.text)) {
        std::snprintf(state.status, sizeof(state.status), "Invalid or unavailable source value; use exact MHz with up to 6 decimals");
        return false;
    }
    state.edit = {}; state.dirty = true; state.draftAttached = false; state.draftBinding = {};
    profile_editor_source_display(state.document.value, state.desired);
    state.sourceCheckVisible = false; state.sourceCheckLines.clear(); state.sourceReview.reset();
    std::snprintf(state.status, sizeof(state.status), "Source intent updated; Save preserves it; Check reviews the destination again");
    return true;
}
