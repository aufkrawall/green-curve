// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "linux_tui_profile_load.h"
#include "linux_tui_slot_state.h"
#include "profile_summary.h"
#include <cstdlib>
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
// Export: tick saved slots, type a file path, EXPORT. Import: type a path,
// READ FILE, choose a slot for each record, IMPORT. Both use the shared engine
// (profile_editor_transfer.h); importing never changes the editor draft.
template<class IO> static inline void tui_transfer_refresh(TuiState& state, IO& storage) {
    auto& v = state.transfer;
    bool saved[6];
    tui_profile_slots_saved(storage, state.configPath, saved);
    state.transferLines.clear();
    std::string note;
    if (!v.importing) {
        v.rows = 5;
        auto document = std::make_unique<ProfileDocument>();
        std::string bytes, text; ProfileCodecError error;
        const bool readable = storage.read(state.configPath, bytes, profile_slot::MAX_CONFIG_BYTES, true, error) &&
            profile_config::decode(bytes, text, &error);
        for (int i = 0; i < 5; ++i) {
            bool found = false;
            v.rowAvailable[i] = saved[i + 1];
            if (!v.rowAvailable[i]) { v.rowEnabled[i] = false; state.transferLines.push_back("Empty"); continue; }
            const bool decoded = readable && ((profile_document_slot_decode(text, i + 1, *document, found, error) && found) ||
                (storage.legacy(text, i + 1, *document, found, error) && found));
            state.transferLines.push_back(decoded ? profile_document_summary(*document) : std::string("Saved profile"));
        }
        if (v.confirmOverwrite) note = "The file exists. Press EXPORT again to replace it.";
    } else if (state.transferProbe && state.transferProbe->ok) {
        const auto& probe = *state.transferProbe;
        v.rows = (int)std::min<size_t>(probe.documents.size(), 5);
        for (int i = 0; i < v.rows; ++i) {
            const auto& d = probe.documents[(size_t)i];
            v.rowAvailable[i] = true;
            state.transferLines.push_back((d.displayName[0] ? std::string(d.displayName) + ": " : std::string()) +
                profile_document_summary(d));
        }
    } else {
        v.rows = 0;
    }
    bool used[6]{}, duplicate = false, any = false;
    std::string replaced;
    for (int i = 0; i < v.rows; ++i) {
        if (!v.rowEnabled[i] || !v.rowAvailable[i]) continue;
        any = true;
        if (!v.importing) continue;
        const int slot = v.rowSlot[i];
        if (used[slot]) duplicate = true;
        used[slot] = true;
        if (saved[slot]) replaced += (replaced.empty() ? "" : ", ") + std::to_string(slot);
    }
    if (duplicate) note = "Two profiles go to the same slot; choose another slot for one of them.";
    else if (!replaced.empty()) note = "Replaces the profile saved in slot " + replaced + ".";
    if (v.importing && state.transferProbe && !state.transferProbe->ok) {
        if (state.transferProbe->needsMemoryUnit || state.transferProbe->needsCurveMeaning)
            note = "This older file does not say how some values were saved; answer the question above.";
        else if (!state.transferProbe->error.empty()) note = "Cannot import this file: " + state.transferProbe->error + ".";
    }
    v.canRun = any && !duplicate && !v.pathEditing && !state.transferPath.empty() && (!v.importing || v.probed);
    state.transferLines.push_back(note);
}
template<class IO> static inline void tui_transfer_probe(TuiState& state, IO& storage) {
    auto& v = state.transfer;
    ProfileLegacyOptions legacy{};
    if (v.format) {
        legacy.format = (ProfileLegacyFormat)v.format;
        legacy.memory = (ProfileLegacyMemoryUnit)v.memory;
        legacy.curve = (ProfileLegacyCurveKind)v.curve;
    }
    state.transferProbe = std::make_unique<ProfileEditorTransferProbe>();
    (void)profile_editor_transfer_probe(state.transferPath, legacy, storage, *state.transferProbe);
    const auto& probe = *state.transferProbe;
    v.probed = probe.ok;
    v.legacyFile = probe.legacyFile;
    if (probe.legacyFile && !v.format) v.format = probe.legacy.format;
    v.askMemory = probe.needsMemoryUnit || (probe.legacyFile && v.memory);
    v.askCurve = probe.needsCurveMeaning || (probe.legacyFile && v.curve);
    bool saved[6];
    tui_profile_slots_saved(storage, state.configPath, saved);
    bool taken[6]{};
    for (int i = 0; i < 5; ++i) {
        v.rowEnabled[i] = probe.ok && i < (int)probe.documents.size();
        int slot = 0;
        for (int s = 1; s <= 5 && !slot; ++s) if (!taken[s] && !saved[s]) slot = s;
        for (int s = i + 1; s <= 5 && !slot; ++s) if (!taken[s]) slot = s;
        for (int s = 1; s <= 5 && !slot; ++s) if (!taken[s]) slot = s;
        if (slot) taken[slot] = true;
        v.rowSlot[i] = slot ? slot : i + 1;
    }
    std::snprintf(state.status, sizeof(state.status), "%s", probe.ok ? "Choose where each profile goes, then IMPORT" :
        "The file could not be imported as it is; see the note");
    linux_debug_logf("profile-transfer: probe ok=%d legacy=%d records=%u askMemory=%d askCurve=%d", probe.ok ? 1 : 0,
        probe.legacyFile ? 1 : 0, (unsigned)probe.documents.size(), probe.needsMemoryUnit ? 1 : 0, probe.needsCurveMeaning ? 1 : 0);
    tui_transfer_refresh(state, storage);
}
static inline void tui_transfer_cancel(TuiState& state) {
    state.transfer = {}; state.transferProbe.reset(); state.transferLines.clear();
    state.transferPresented = false; state.focusIndex = -1;
    std::snprintf(state.status, sizeof(state.status), "Profile file dialog closed; nothing was changed");
    linux_debug_logf("profile-transfer: stage=closed");
}
template<class IO> static inline bool tui_transfer_run(TuiState& state, IO& storage) {
    auto& v = state.transfer;
    tui_transfer_refresh(state, storage);
    if (!v.canRun) return false;
    ProfileEditorTransferOptions options;
    options.importing = v.importing;
    if (v.importing) {
        if (!state.transferProbe || !state.transferProbe->ok) return false;
        options.legacy = state.transferProbe->legacy;
        for (int i = 0; i < v.rows; ++i) if (v.rowEnabled[i]) {
            options.sources.push_back(state.transferProbe->sources[(size_t)i]); options.slots.push_back(v.rowSlot[i]);
        }
    } else {
        std::string existing; ProfileCodecError probeError;
        if (!v.confirmOverwrite && storage.read(state.transferPath, existing, PROFILE_FILE_MAX_BYTES, true, probeError) && !existing.empty()) {
            v.confirmOverwrite = true; tui_transfer_refresh(state, storage); return false;
        }
        for (int i = 0; i < v.rows; ++i) if (v.rowEnabled[i] && v.rowAvailable[i]) options.slots.push_back(i + 1);
    }
    auto preview = std::make_unique<ProfileEditorTransferPreview>();
    const bool success = profile_editor_transfer_prepare(options, state.transferPath, state.configPath, storage, *preview) &&
        profile_editor_transfer_commit(storage, *preview);
    linux_debug_logf("profile-transfer: run import=%d records=%u success=%d code=%d", v.importing ? 1 : 0,
        (unsigned)options.slots.size(), success ? 1 : 0, preview->result);
    if (!success) {
        std::snprintf(state.status, sizeof(state.status), "%s failed: %s; nothing was changed", v.importing ? "Import" : "Export",
            preview->error.message[0] ? preview->error.message : "unknown error");
        return false;
    }
    // Import does not refresh the daemon's startup snapshot; its comparison is unknown.
    if (v.importing && state.service.state.startupPolicyMode == SERVICE_STARTUP_POLICY_PROFILE)
        state.startupSnapshotState = STARTUP_SNAPSHOT_STATE_UNKNOWN;
    const bool imported = v.importing;
    std::string slots;
    for (int slot : options.slots) slots += (slots.empty() ? "" : ", ") + std::to_string(slot);
    tui_transfer_cancel(state);
    std::snprintf(state.status, sizeof(state.status), imported ? "Imported into slot %s; Load a slot to edit it" :
        "Exported slot %s to the file", slots.c_str());
    return true;
}
template<class IO, class Bind> static inline bool tui_transfer_action(TuiState& state, const ClickAction& action, IO& storage, Bind) {
    const bool transferAction = action.type >= ACTION_TRANSFER_BEGIN && action.type <= ACTION_TRANSFER_CANCEL;
    if (!transferAction) return state.transfer.active;
    if (action.type == ACTION_TRANSFER_BEGIN) {
        if (state.capture.active || state.profileLoadPending || state.transfer.active) return true;
        state.transfer = {}; state.transfer.active = true; state.transferProbe.reset();
        for (bool& enabled : state.transfer.rowEnabled) enabled = true;
        if (state.transferPath.empty()) {
            const char* home = getenv("HOME");
            state.transferPath = home && *home ? std::string(home) + "/green-curve-profiles.gcprofile" : "green-curve-profiles.gcprofile";
        }
        state.transferPresented = false; state.focusIndex = -1;
        tui_transfer_refresh(state, storage);
        std::snprintf(state.status, sizeof(state.status), "Choose the profiles to export, or switch to import");
        return true;
    }
    if (!state.transfer.active) return true;
    if (action.type == ACTION_TRANSFER_CANCEL) { tui_transfer_cancel(state); return true; }
    if (!state.transferPresented) return true;
    auto& v = state.transfer;
    const int row = action.index;
    switch (action.type) {
        case ACTION_TRANSFER_PATH: v.pathEditing = v.selectAll = true; v.probed = v.confirmOverwrite = false; state.transferProbe.reset(); break;
        case ACTION_TRANSFER_MODE:
            v.importing = !v.importing; v.probed = v.confirmOverwrite = v.legacyFile = v.askMemory = v.askCurve = false;
            v.format = v.memory = v.curve = 0; state.transferProbe.reset();
            for (bool& enabled : v.rowEnabled) enabled = true;
            break;
        case ACTION_TRANSFER_RECORD: if (row >= 0 && row < v.rows && v.rowAvailable[row]) v.rowEnabled[row] = !v.rowEnabled[row]; break;
        case ACTION_TRANSFER_SLOT: if (row >= 0 && row < v.rows) v.rowSlot[row] = v.rowSlot[row] % 5 + 1; break;
        case ACTION_TRANSFER_FORMAT: v.format = v.format == PROFILE_LEGACY_WINDOWS_INI ? PROFILE_LEGACY_LINUX_INI : PROFILE_LEGACY_WINDOWS_INI;
            v.memory = v.curve = 0; tui_transfer_probe(state, storage); return true;
        case ACTION_TRANSFER_MEMORY: v.memory = v.memory % 2 + 1; tui_transfer_probe(state, storage); return true;
        case ACTION_TRANSFER_CURVE: v.curve = v.curve % 3 + 1; tui_transfer_probe(state, storage); return true;
        case ACTION_TRANSFER_PREVIEW: if (v.importing && !v.pathEditing) { v.format = v.memory = v.curve = 0; tui_transfer_probe(state, storage); } return true;
        case ACTION_TRANSFER_COMMIT: (void)tui_transfer_run(state, storage); if (state.transfer.active) tui_transfer_refresh(state, storage); return true;
        default: break;
    }
    tui_transfer_refresh(state, storage);
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
            // A finished path reads an import file at once; export re-checks its choices.
            action(ClickAction{0, 0, 0, 0, v.importing ? ACTION_TRANSFER_PREVIEW : ACTION_TRANSFER_RECORD, -1, 0, 0});
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
