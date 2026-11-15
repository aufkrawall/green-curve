// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "linux_tui_profile_load.h"
#include <algorithm>

// Input arrives byte by byte. Never emit an incomplete UTF-8 sequence or a
// terminal control while editing; the raw path remains exact for file I/O.
static inline std::string tui_transfer_path_display(const std::string& path) {
    std::string text;
    for (size_t i = 0; i < path.size();) {
        const unsigned char c = (unsigned char)path[i];
        const size_t n = c < 128 ? 1 : c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        if (n && n <= path.size() - i && profile_utf8_valid(path.data() + i, n) && !(c == 0xc2 && (unsigned char)path[i + 1] < 0xa0)) {
            text.append(path, i, n); i += n;
        } else {
            char escaped[5]{}; std::snprintf(escaped, sizeof(escaped), "\\x%02X", c); text += escaped; ++i;
        }
    }
    return text;
}
static inline bool tui_transfer_draft_dirty(const TuiState& state) {
    return state.edit.active || (state.document.valid ? profile_editor_document_dirty(state.document) : state.dirty);
}
template<class IO> static inline int tui_transfer_unused_slot(IO& storage, const char* config) {
    std::string bytes, text; ProfileCodecError error;
    if (!storage.read(config, bytes, profile_slot::MAX_CONFIG_BYTES, true, error) || !profile_config::decode(bytes, text, &error)) return 0;
    for (int slot = 1; slot <= 5; ++slot) {
        std::string body; bool found = false;
        if (!profile_slot::extract(text, slot, body, found, &error)) return 0;
        if (!found || profile_codec::trim(body).empty()) return slot;
    }
    return 0; // A full or unreadable bank requires an explicit destination.
}
static inline void tui_transfer_invalidate(TuiState& state) {
    state.transferPreview.reset(); state.transfer.ready = state.transfer.reviewing = false;
    state.transfer.discardConfirmed = false; state.transfer.scroll = 0;
    state.transferPresented = false; state.focusIndex = -1;
}
static inline void tui_transfer_cancel(TuiState& state) {
    tui_transfer_invalidate(state); state.transfer = {};
    std::snprintf(state.status, sizeof(state.status), "Profile transfer closed; editor draft kept");
    linux_debug_logf("profile-import: stage=closed");
}
template<class IO> static inline void tui_transfer_preview(TuiState& state, IO& storage) {
    if (!state.transfer.active || !state.transferPresented || state.transfer.pathEditing) return;
    tui_transfer_invalidate(state);
    const auto& v = state.transfer;
    ProfileEditorTransferOptions options; options.importing = v.importing; options.all = v.all;
    options.slot = v.all ? 1 : v.slot; options.sourceRecord = v.sourceRecord;
    options.legacy.format = (ProfileLegacyFormat)v.format;
    options.legacy.memory = (ProfileLegacyMemoryUnit)v.memory;
    options.legacy.curve = (ProfileLegacyCurveKind)v.curve;
    state.transferPreview = std::make_unique<ProfileEditorTransferPreview>();
    state.transfer.ready = profile_editor_transfer_prepare(options, state.transferPath, state.configPath, storage, *state.transferPreview);
    state.transferPreview->lines.insert(state.transferPreview->lines.begin(),
        std::string(v.importing ? "Read from: " : "Write to (replaces existing file): ") + tui_transfer_path_display(state.transferPath));
    state.transfer.reviewing = true;
    state.transfer.needsDiscard = tui_transfer_draft_dirty(state);
    std::snprintf(state.status, sizeof(state.status), "Transfer preview: no file or bank changes yet");
    linux_debug_logf("profile-import: stage=preview direction=%s records=%u code=%d", v.importing ? "import" : "export",
        (unsigned)state.transferPreview->documents.size(), state.transferPreview->result);
}
template<class IO, class Bind> static inline bool tui_transfer_commit(TuiState& state, IO& storage, Bind bind) {
    auto& v = state.transfer;
    if (!v.active || !state.transferPresented || !v.reviewing || !v.ready || !state.transferPreview || v.pathEditing) return false;
    if (v.importing && v.loadAfterImport && tui_transfer_draft_dirty(state)) {
        if (!v.needsDiscard) {
            v.needsDiscard = true; v.discardConfirmed = false; state.transferPresented = false; state.focusIndex = -1;
        }
        if (!v.discardConfirmed) return false;
    }
    auto& preview = *state.transferPreview;
    const bool success = profile_editor_transfer_commit(storage, preview); v.ready = false;
    linux_debug_logf("profile-import: stage=commit direction=%s records=%u code=%d", v.importing ? "import" : "export",
        (unsigned)preview.documents.size(), preview.result);
    if (!success) {
        preview.lines.emplace_back(std::string("Transfer failed: ") + preview.error.message);
        std::snprintf(state.status, sizeof(state.status), "Transfer failed; review again before retrying"); return false;
    }
    if (v.importing) {
        // Import does not refresh or change the daemon's existing startup snapshot.
        if (state.service.state.startupPolicyMode == SERVICE_STARTUP_POLICY_PROFILE)
            state.startupSnapshotState = STARTUP_SNAPSHOT_STATE_UNKNOWN;
        if (v.loadAfterImport) {
            auto candidate = std::make_unique<ProfileEditorCandidate>();
            profile_editor_source_candidate(preview.documents.front(), *candidate);
            tui_install_profile_load(state, *candidate, preview.edit.destinations[0], bind);
        }
    }
    const bool imported = v.importing, loaded = imported && v.loadAfterImport;
    tui_transfer_invalidate(state); state.transfer = {};
    std::snprintf(state.status, sizeof(state.status), "%s; %s", imported ? "Imported reviewed profiles atomically" : "Exported reviewed saved profiles",
        loaded ? "first imported source loaded; Apply requires destination review" : "editor draft kept");
    return true;
}
template<class IO, class Bind> static inline bool tui_transfer_action(TuiState& state, const ClickAction& action, IO& storage, Bind bind) {
    const bool transferAction = action.type >= ACTION_TRANSFER_BEGIN && action.type <= ACTION_TRANSFER_CANCEL;
    if (!transferAction) return state.transfer.active;
    if (action.type == ACTION_TRANSFER_BEGIN) {
        if (state.capture.active || state.profileLoadPending || state.transfer.active) return true;
        state.transfer = {}; state.transfer.active = true; state.transfer.slot = state.currentSlot;
        state.transfer.loadAfterImport = !tui_transfer_draft_dirty(state); state.transfer.needsDiscard = tui_transfer_draft_dirty(state);
        if (state.transferPath.empty()) state.transferPath = "profiles.gcprofile";
        tui_transfer_invalidate(state);
        std::snprintf(state.status, sizeof(state.status), "Choose file and records; Preview performs no writes"); return true;
    }
    if (!state.transfer.active) return true;
    if (action.type == ACTION_TRANSFER_CANCEL) { tui_transfer_cancel(state); return true; }
    if (!state.transferPresented) return true;
    auto& v = state.transfer;
    switch (action.type) {
        case ACTION_TRANSFER_PATH: v.pathEditing = true; v.selectAll = true; tui_transfer_invalidate(state); break;
        case ACTION_TRANSFER_MODE:
            v.importing = !v.importing; v.slot = v.importing ? tui_transfer_unused_slot(storage, state.configPath) : state.currentSlot;
            tui_transfer_invalidate(state); break;
        case ACTION_TRANSFER_KIND: v.all = !v.all; tui_transfer_invalidate(state); break;
        case ACTION_TRANSFER_RECORD: v.sourceRecord = v.sourceRecord % 5 + 1; tui_transfer_invalidate(state); break;
        case ACTION_TRANSFER_SLOT: v.slot = v.slot % 5 + 1; tui_transfer_invalidate(state); break;
        case ACTION_TRANSFER_FORMAT: v.format = (v.format + 1) % 3; v.memory = v.curve = 0; tui_transfer_invalidate(state); break;
        case ACTION_TRANSFER_MEMORY: v.memory = (v.memory + 1) % 3; tui_transfer_invalidate(state); break;
        case ACTION_TRANSFER_CURVE: v.curve = (v.curve + 1) % 4; tui_transfer_invalidate(state); break;
        case ACTION_TRANSFER_PREVIEW: tui_transfer_preview(state, storage); break;
        case ACTION_TRANSFER_BACK: tui_transfer_invalidate(state); break;
        case ACTION_TRANSFER_LOAD: if (v.importing) { v.loadAfterImport = !v.loadAfterImport; v.discardConfirmed = false; } break;
        case ACTION_TRANSFER_DISCARD: if (v.importing && v.loadAfterImport && v.needsDiscard) v.discardConfirmed = !v.discardConfirmed; break;
        case ACTION_TRANSFER_COMMIT: (void)tui_transfer_commit(state, storage, bind); break;
        default: break;
    }
    return true;
}
static inline void tui_transfer_present_layout(TuiState& state) {
    int cancel = -1;
    if (state.transfer.active && state.layout.transferVisible && !state.layout.tooSmall)
        for (size_t i = 0; i < state.layout.actions.size(); ++i)
            if (state.layout.actions[i].type == ACTION_TRANSFER_CANCEL) cancel = (int)i;
    if (cancel >= 0 && !state.transferPresented) state.focusIndex = cancel;
    state.transferPresented = cancel >= 0;
}
template<class Action, class Focus, class Activate, class Hit>
static inline bool tui_transfer_modal_event(TuiState& state, const TuiInputEvent& event, Action action, Focus focus, Activate activate, Hit hit) {
    auto& v = state.transfer;
    if (!v.active) return false;
    if (event.type == TUI_INPUT_ESCAPE) { action(ClickAction{0,0,0,0,ACTION_TRANSFER_CANCEL,0,0,0}); return true; }
    if (!state.transferPresented) return true;
    if (v.pathEditing) {
        if (event.type == TUI_INPUT_CHARACTER || event.type == TUI_INPUT_BACKSPACE) {
            if (v.selectAll) state.transferPath.clear();
            v.selectAll = false;
            if (event.type == TUI_INPUT_CHARACTER && (unsigned char)event.character >= 32 && event.character != 127 && state.transferPath.size() < 4095)
                state.transferPath += event.character;
            else if (event.type == TUI_INPUT_BACKSPACE && !state.transferPath.empty()) {
                size_t end = state.transferPath.size() - 1;
                while (end && ((unsigned char)state.transferPath[end] & 0xc0) == 0x80) --end;
                state.transferPath.resize(end);
            }
        } else if (event.type == TUI_INPUT_ENTER || event.type == TUI_INPUT_TAB || event.type == TUI_INPUT_SHIFT_TAB) {
            v.pathEditing = v.selectAll = false;
            if (event.type != TUI_INPUT_ENTER) focus(event.type == TUI_INPUT_TAB ? 1 : -1);
        }
        return true;
    }
    if (event.type == TUI_INPUT_TAB || event.type == TUI_INPUT_SHIFT_TAB) focus(event.type == TUI_INPUT_TAB ? 1 : -1);
    else if (event.type == TUI_INPUT_ENTER || (event.type == TUI_INPUT_CHARACTER && event.character == ' ')) activate();
    else if (event.type == TUI_INPUT_PAGE_UP || event.type == TUI_INPUT_PAGE_DOWN ||
        (event.type == TUI_INPUT_MOUSE && event.mousePress && (event.mouseButton & 64))) {
        const bool up = event.type == TUI_INPUT_PAGE_UP || (event.type == TUI_INPUT_MOUSE && !(event.mouseButton & 1));
        const int amount = event.type == TUI_INPUT_MOUSE ? 1 : std::max(1, state.layout.transferVisibleRows);
        v.scroll = std::clamp(v.scroll + (up ? -amount : amount), 0, std::max(0, state.layout.transferLineCount - state.layout.transferVisibleRows));
    } else if (event.type == TUI_INPUT_MOUSE && event.mousePress && event.mouseButton == 0) {
        const int index = hit(event.mouseX, event.mouseY);
        if (index >= 0 && index < (int)state.layout.actions.size()) { state.focusIndex = index; action(state.layout.actions[index]); }
    }
    return true;
}
