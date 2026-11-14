// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "linux_tui_internal.h"
#include "linux_debug_log.h"
#include <cstdio>
#include <cstring>
#include <memory>

namespace tui_profile_load_fixture {
static void linux_debug_logf(const char*, ...) {}
#include "linux_tui_profile_load.h"
static int run() {
    auto state = std::make_unique<TuiState>(); auto& s = *state;
    s.currentSlot = 3; s.dirty = true; s.desired.hasPowerLimit = 1; s.desired.powerLimitPct = 77;
    s.edit.active = true; std::snprintf(s.edit.text, sizeof(s.edit.text), "invalid");
    DesiredSettings source{}; desired_memory_set_precise(&source, -123500000, CLOCK_OFFSET_SURFACE_GLOBAL);
    int reads = 0, binds = 0; bool fail = false;
    const auto read = [&](int slot, ProfileEditorCandidate& output, char* why, size_t size) {
        ++reads;
        if (slot != 3 || fail) { std::snprintf(why, size, "unavailable profile"); return false; }
        output.settings = source; return true;
    };
    const auto bind = [&](TuiState& draft) { ++binds; draft.draftAttached = false; draft.dirty = true; };
    const auto answer = [&](bool discard) { tui_finish_profile_load(s, discard, bind); };
    tui_request_profile_load(s, read, bind);
    if (!s.profileLoadPending || s.profileLoadPresented || reads != 1 || binds ||
        s.desired.powerLimitPct != 77 || !s.edit.active || std::strcmp(s.edit.text, "invalid")) return 9190;
    TuiInputEvent key{}; key.type = TUI_INPUT_CHARACTER; key.character = 'y';
    if (!tui_profile_load_event(s, key, ACTION_NONE, answer) || binds || !s.profileLoadPending) return 9191;
    key.character = 'r'; s.profileLoadPresented = true;
    if (!tui_profile_load_event(s, key, ACTION_NONE, answer) || reads != 1 || binds || !s.profileLoadPending) return 9192;
    key.type = TUI_INPUT_ENTER; // Default focus is Keep draft.
    if (!tui_profile_load_event(s, key, ACTION_NONE, answer) || s.profileLoadPending || binds ||
        s.desired.powerLimitPct != 77 || !s.edit.active || std::strcmp(s.edit.text, "invalid")) return 9193;
    tui_request_profile_load(s, read, bind);
    source.memOffsetHz = 9000000; // A concurrent file change cannot replace the reviewed generation.
    s.profileLoadPresented = true; s.currentSlot = 4; key.type = TUI_INPUT_CHARACTER; key.character = 'Y';
    if (!tui_profile_load_event(s, key, ACTION_NONE, answer) || s.profileLoadPending || binds != 1 || reads != 2 ||
        s.currentSlot != 3 || s.edit.active || desired_memory_hz(s.desired) != -123500000 ||
        s.desired.memOffsetSurface != CLOCK_OFFSET_SURFACE_GLOBAL || s.pendingProfile.settings.hasMemOffset) return 9194;
    if (tui_profile_load_event(s, key, ACTION_NONE, answer) || binds != 1 || reads != 2) return 9195;
    s.dirty = false; fail = true;
    tui_request_profile_load(s, read, bind);
    if (s.profileLoadPending || binds != 1 || desired_memory_hz(s.desired) != -123500000 ||
        !std::strstr(s.status, "unavailable profile")) return 9196;
    fail = false; s.edit.active = true; // An uncommitted edit needs protection even before dirty recomputation.
    tui_request_profile_load(s, read, bind);
    if (!s.profileLoadPending || binds != 1) return 9197;
    key.type = TUI_INPUT_ESCAPE;
    if (!tui_profile_load_event(s, key, ACTION_NONE, answer) || s.profileLoadPending || !s.edit.active || binds != 1) return 9198;
    s.edit.active = false; s.dirty = false;
    tui_request_profile_load(s, read, bind);
    if (s.profileLoadPending || binds != 2 || desired_memory_hz(s.desired) != 9000000) return 9199;
    s.dirty = true; tui_request_profile_load(s, read, bind); s.profileLoadPresented = true;
    key.type = TUI_INPUT_TAB;
    if (!tui_profile_load_event(s, key, ACTION_NONE, answer) || s.focusIndex != 0) return 9200;
    key.type = TUI_INPUT_ENTER;
    if (!tui_profile_load_event(s, key, ACTION_NONE, answer) || s.profileLoadPending || binds != 3) return 9201;
    tui_request_profile_load(s, read, bind); s.profileLoadPresented = true; key.type = TUI_INPUT_MOUSE;
    if (!tui_profile_load_event(s, key, ACTION_PROFILE_LOAD_KEEP, answer) || s.profileLoadPending || binds != 3) return 9202;
    for (const auto size : {TuiRect{0,0,72,24}, TuiRect{0,0,100,32}, TuiRect{0,0,160,48}}) {
        TuiViewModel vm{}; vm.desired = &s.desired; vm.currentSlot = 3; vm.focusIndex = 1;
        vm.profileLoadPending = true; vm.pendingProfileSlot = 3;
        TuiLayout layout{}; build_tui_layout(vm, size.width, size.height, &layout);
        if (layout.tooSmall || !tui_layout_actions_valid(layout) || layout.actions.size() != 2 ||
            layout.actions[0].type != ACTION_PROFILE_LOAD_DISCARD || layout.actions[1].type != ACTION_PROFILE_LOAD_KEEP) return 9203;
        std::string cells;
        for (const auto& cell : layout.cells) cells += cell.glyph;
        if (cells.find("Replace unsaved draft?") == std::string::npos ||
            cells.find("Discard and load") == std::string::npos || cells.find("Keep draft") == std::string::npos) return 9205;
    }
    TuiViewModel vm{}; vm.desired = &s.desired; vm.profileLoadPending = true;
    TuiLayout small{}; build_tui_layout(vm, 40, 12, &small);
    if (!small.tooSmall || !small.actions.empty()) return 9204;
    return 0;
}
}
int run_tui_profile_load_tests() { return tui_profile_load_fixture::run(); }
