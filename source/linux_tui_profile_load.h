// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "linux_tui_internal.h"
#include "linux_debug_log.h"
#include <memory>

// Freeze the loaded record before asking. Confirmation never rereads another
// process's newer file generation, writes a slot or invokes the daemon.
template<class Bind>
static inline void tui_install_profile_load(TuiState& state, const ProfileEditorCandidate& loaded, int slot, Bind bind) {
    state.desired = loaded.settings; state.document = loaded.source; state.currentSlot = slot; state.edit = {};
    bind(state);
    if (state.document.valid) {
        state.draftAttached = false; state.draftBinding = {}; state.sourceScroll = 0; state.tab = TUI_TAB_PROFILES;
        snprintf(state.status, sizeof(state.status), "Loaded slot %d as portable source; destination review is required before Apply", slot);
    }
    else if (state.draftAttached)
        snprintf(state.status, sizeof(state.status), "Loaded slot %d into the staged editor", slot);
    else if (state.serviceOnline) {
        const gc_u32 requested = service_desired_mutation_domains(&state.desired);
        const gc_u32 missing = requested & ~state.service.snapshot.health.availableMutationDomains;
        snprintf(state.status, sizeof(state.status),
            "Loaded slot %d, but its draft is detached (requested=0x%02x missing=0x%02x); unavailable/mixed Apply remains blocked",
            slot, (unsigned)requested, (unsigned)missing);
    }
    else
        snprintf(state.status, sizeof(state.status), "Loaded slot %d locally; daemon is offline, so Apply remains detached", slot);
    linux_debug_logf("profile-draft: loaded slot=%d attached=%d", slot, state.draftAttached ? 1 : 0);
}
template<class Read, class Bind>
static inline void tui_request_profile_load(TuiState& state, Read read, Bind bind) {
    auto loaded = std::make_unique<ProfileEditorCandidate>(); char error[256]{};
    if (!read(state.currentSlot, *loaded, error, sizeof(error))) {
        snprintf(state.status, sizeof(state.status), "Profile load failed: %.220s", error);
        linux_debug_logf("profile-draft: load failed slot=%d", state.currentSlot);
        return;
    }
    if ((state.document.valid ? profile_editor_document_dirty(state.document) : state.dirty) || state.edit.active) {
        state.pendingProfile = *loaded; state.pendingProfileSlot = state.currentSlot;
        state.profileLoadPending = true; state.profileLoadPresented = false; state.focusIndex = 1;
        snprintf(state.status, sizeof(state.status), "Unsaved draft: confirm Discard and load or Keep draft");
        linux_debug_logf("profile-draft: replacement pending slot=%d", state.currentSlot);
        return;
    }
    tui_install_profile_load(state, *loaded, state.currentSlot, bind);
}
template<class Bind>
static inline void tui_finish_profile_load(TuiState& state, bool discard, Bind bind) {
    if (!state.profileLoadPending || (discard && !state.profileLoadPresented)) return;
    const int slot = state.pendingProfileSlot;
    if (discard) tui_install_profile_load(state, state.pendingProfile, slot, bind);
    else {
        snprintf(state.status, sizeof(state.status), "Kept the unsaved draft");
        linux_debug_logf("profile-draft: replacement cancelled slot=%d", slot);
    }
    state.profileLoadPending = state.profileLoadPresented = false;
    state.pendingProfileSlot = 0; state.pendingProfile = {}; state.focusIndex = -1;
}
template<class Answer>
static inline bool tui_profile_load_event(TuiState& state, const TuiInputEvent& event, ActionType clicked, Answer answer) {
    if (!state.profileLoadPending) return false;
    const bool key = event.type == TUI_INPUT_CHARACTER;
    if (event.type == TUI_INPUT_ESCAPE || (key && (event.character == 'n' || event.character == 'N')))
        answer(false);
    else if (!state.profileLoadPresented) return true;
    else if ((key && (event.character == 'y' || event.character == 'Y')) || clicked == ACTION_PROFILE_LOAD_DISCARD ||
        (event.type == TUI_INPUT_ENTER && state.focusIndex == 0)) answer(true);
    else if (clicked == ACTION_PROFILE_LOAD_KEEP || (event.type == TUI_INPUT_ENTER && state.focusIndex == 1)) answer(false);
    else if (event.type == TUI_INPUT_TAB || event.type == TUI_INPUT_SHIFT_TAB) state.focusIndex = state.focusIndex == 0 ? 1 : 0;
    return true;
}
