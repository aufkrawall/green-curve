// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "linux_tui_internal.h"
#include "linux_debug_log.h"
#include <map>
#include <cstring>
namespace tui_transfer_fixture {
static void linux_debug_logf(const char*, ...) {}
#include "linux_tui_transfer.h"
struct IO {
    std::map<std::string, std::string> files;
    int reads = 0, writes = 0, commits = 0;
    bool read(const std::string& path, std::string& bytes, size_t maximum, bool missing, ProfileCodecError& error) {
        ++reads; const auto it = files.find(path);
        if (it == files.end()) { bytes.clear(); return missing || profile_codec::fail(&error, 0, "missing fixture file"); }
        bytes = it->second; return bytes.size() <= maximum || profile_codec::fail(&error, 0, "fixture too large");
    }
    bool distinct(const std::string& a, const std::string& b, ProfileCodecError& error) {
        return a != b || profile_codec::fail(&error, 0, "same fixture path");
    }
    bool legacy(const std::string& bytes, int slot, ProfileDocument& d, bool& found, ProfileCodecError& error) {
        return profile_legacy_native_slot_decode(bytes, PROFILE_LEGACY_LINUX_INI, slot, d, found, &error);
    }
    bool write(const std::string& path, const std::string& bytes, ProfileCodecError&) { ++writes; files[path] = bytes; return true; }
    bool commit(const std::string& path, const ProfileBankEdit& edit, ProfileCodecError& error) {
        ++commits; return profile_bank_commit_edit(edit, [&](std::string& bytes) { bytes = files[path]; return true; },
            [&](const std::string& bytes) { return write(path, bytes, error); }, &error);
    }
};
static int run() {
    auto state = std::make_unique<TuiState>(); auto& s = *state; s.currentSlot = 4; s.dirty = true;
    s.edit.active = true; std::strcpy(s.edit.text, "invalid-"); std::strcpy(s.configPath, "bank.ini");
    s.desired.hasMemOffset = 1; s.desired.memOffsetMHz = 17;
    const auto oldEdit = s.edit; const auto oldDesired = s.desired;
    IO io; int binds = 0; const auto bind = [&](TuiState&) { ++binds; };
    const auto act = [&](ActionType type) { (void)tui_transfer_action(s, ClickAction{0,0,0,0,type,0,0,0}, io, bind); };
    auto bank = std::make_unique<ProfileBank>(); bank->count = 5;
    for (int i = 0; i < 5; ++i) {
        auto& d = bank->documents[i]; d = profile_document_make_empty(); d.documentId = i + 1;
        std::snprintf(d.displayName, sizeof(d.displayName), "transfer fixture %d", i + 1); d.domains = PROFILE_DOMAIN_MEMORY;
        d.hasMemoryOffset = true; d.memoryOffsetHz = -123500000 + i; d.memorySurface = CLOCK_OFFSET_SURFACE_GLOBAL;
    }
    ProfileCodecError error; std::string bytes;
    if (!profile_bank_encode(*bank, bytes, &error)) return 9460;
    io.files["input.gcprofile"] = bytes; io.files["bank.ini"] = "[preferences]\nkept=yes\n";
    act(ACTION_TRANSFER_BEGIN);
    if (!s.transfer.active || s.transfer.loadAfterImport || !s.transfer.needsDiscard || s.transferPresented) return 9461;
    s.transferPath = "input.gcprofile"; s.transfer.importing = true;
    act(ACTION_TRANSFER_PREVIEW); if (io.reads) return 9462;
    s.transferPresented = true; act(ACTION_TRANSFER_PREVIEW);
    if (!s.transfer.ready || io.writes || io.commits || binds || std::memcmp(&s.edit, &oldEdit, sizeof(oldEdit))) return 9463;
    act(ACTION_TRANSFER_COMMIT); if (io.commits) return 9464;
    s.transferPresented = true; act(ACTION_TRANSFER_LOAD); act(ACTION_TRANSFER_COMMIT);
    if (io.commits || !s.transfer.active) return 9465;
    act(ACTION_TRANSFER_DISCARD); act(ACTION_TRANSFER_LOAD); // import only, retaining invalid input
    act(ACTION_TRANSFER_COMMIT);
    if (s.transfer.active || io.commits != 1 || io.writes != 1 || binds || s.currentSlot != 4 ||
        std::memcmp(&s.edit, &oldEdit, sizeof(oldEdit)) || std::memcmp(&s.desired, &oldDesired, sizeof(oldDesired))) return 9466;
    ProfileDocument loaded; bool found = false;
    if (!profile_document_slot_decode(io.files["bank.ini"], 4, loaded, found, error) || !found || loaded.memoryOffsetHz != -123500000) return 9467;
    act(ACTION_TRANSFER_BEGIN); s.transfer.importing = true; s.transfer.all = true;
    s.transferPresented = true; act(ACTION_TRANSFER_PREVIEW); s.transferPresented = true; act(ACTION_TRANSFER_LOAD);
    act(ACTION_TRANSFER_DISCARD); act(ACTION_TRANSFER_COMMIT);
    if (s.transfer.active || binds != 1 || s.currentSlot != 1 || s.edit.active || !s.document.valid || s.document.unsaved ||
        s.document.value.memoryOffsetHz != -123500000 || s.draftAttached) return 9468;
    for (int i = 1; i <= 5; ++i) if (!profile_document_slot_decode(io.files["bank.ini"], i, loaded, found, error) || !found ||
        loaded.memoryOffsetHz != -123500001 + i) return 9469;
    act(ACTION_TRANSFER_BEGIN); s.transfer.importing = true; s.transferPresented = true; act(ACTION_TRANSFER_PREVIEW);
    io.files["bank.ini"] += "\n[concurrent]\nkept=yes\n"; const auto concurrent = io.files["bank.ini"];
    s.transferPresented = true; act(ACTION_TRANSFER_COMMIT);
    if (!s.transfer.active || s.transfer.ready || io.files["bank.ini"] != concurrent) return 9470;
    act(ACTION_TRANSFER_CANCEL);
    if (s.transfer.active || s.document.value.memoryOffsetHz != -123500000) return 9471;
    act(ACTION_TRANSFER_BEGIN); s.transferPresented = true; act(ACTION_TRANSFER_PREVIEW);
    if (!s.transfer.ready) return 9472;
    s.transferPresented = true; act(ACTION_TRANSFER_SLOT);
    if (s.transfer.ready || s.transferPreview || s.transferPresented) return 9473;
    act(ACTION_TRANSFER_CANCEL); act(ACTION_TRANSFER_BEGIN);
    TuiInputEvent event{};
    const auto focus = [&](int) {}; const auto activate = [&] { act(s.layout.actions[s.focusIndex].type); };
    const auto hit = [&](int, int) { return 0; };
    const auto action = [&](const ClickAction& a) { act(a.type); };
    const auto input = [&](TuiInputType type, char c = 0) {
        event.type = type; event.character = c; return tui_transfer_modal_event(s, event, action, focus, activate, hit);
    };
    input(TUI_INPUT_ENTER); if (!s.transfer.active) return 9474;
    s.transferPresented = true; act(ACTION_TRANSFER_PATH); s.transferPresented = true;
    const std::string path = "\xe6\x9b\xb2\xe7\xba\xbf file.gcprofile";
    for (char c : path) input(TUI_INPUT_CHARACTER, c);
    if (s.transferPath != path || tui_transfer_path_display(path) != path || !s.transfer.pathEditing) return 9475;
    input(TUI_INPUT_BACKSPACE); input(TUI_INPUT_CHARACTER, 'e'); input(TUI_INPUT_ENTER);
    if (s.transferPath != path || s.transfer.pathEditing) return 9476;
    if (tui_transfer_path_display(std::string("\xe6\x9b", 2)) != "\\xE6\\x9B" ||
        tui_transfer_path_display(std::string("\x1b\xc2\x9b", 3)) != "\\x1B\\xC2\\x9B") return 9477;
    s.transferPath = "\xe6\x9b\xb2"; s.transfer.pathEditing = true; s.transfer.selectAll = false;
    input(TUI_INPUT_BACKSPACE); if (!s.transferPath.empty()) return 9478;
    input(TUI_INPUT_ESCAPE); if (s.transfer.active) return 9479;
    DesiredSettings display{}; TuiViewModel vm{}; vm.desired = &display; vm.tab = TUI_TAB_PROFILES;
    vm.transfer.active = true; vm.transfer.path = path; std::vector<std::string> lines(30, "Profile intent and replacement scope");
    lines.emplace_back("Trailing transfer summary marker");
    for (const auto size : {TuiRect{0,0,72,24}, TuiRect{0,0,100,32}, TuiRect{0,0,160,48}}) {
        vm.transfer.active = false;
        for (int source = 0; source < 2; ++source) {
            vm.sourceDocument = source ? &bank->documents[0] : nullptr;
            build_tui_layout(vm, size.width, size.height, &s.layout);
            bool entry = false; for (const auto& a : s.layout.actions) entry |= a.type == ACTION_TRANSFER_BEGIN;
            if (!entry || !tui_layout_actions_valid(s.layout)) return 9487;
        }
        vm.sourceDocument = nullptr; vm.transfer.active = true;
        for (int phase = 0; phase < 3; ++phase) {
            vm.transfer.reviewing = phase != 0; vm.transfer.ready = true; vm.transfer.importing = true;
            vm.transfer.format = 1; vm.transfer.report = &lines; vm.transfer.needsDiscard = vm.transfer.loadAfterImport = true;
            vm.transfer.discardConfirmed = phase == 2;
            build_tui_layout(vm, size.width, size.height, &s.layout);
            bool commit = false;
            for (const auto& a : s.layout.actions) { if (a.type < ACTION_TRANSFER_PATH || a.type > ACTION_TRANSFER_CANCEL) return 9480; commit |= a.type == ACTION_TRANSFER_COMMIT; }
            if (!tui_layout_actions_valid(s.layout) || !s.layout.transferVisible || commit != (phase == 2)) return 9481;
            s.transfer.active = true; s.transferPresented = false; tui_transfer_present_layout(s);
            if (!s.transferPresented || s.layout.actions[s.focusIndex].type != ACTION_TRANSFER_CANCEL) return 9482;
        }
        vm.transfer.scroll = 1000; build_tui_layout(vm, size.width, size.height, &s.layout);
        std::string cells; for (const auto& cell : s.layout.cells) cells += cell.glyph;
        if (cells.find("Trailing transfer summary marker") == std::string::npos) return 9483;
    }
    build_tui_layout(vm, 71, 23, &s.layout); tui_transfer_present_layout(s);
    if (s.transferPresented || !s.layout.actions.empty()) return 9484;
    input(TUI_INPUT_ENTER); if (!s.transfer.active) return 9485;
    input(TUI_INPUT_ESCAPE); if (s.transfer.active) return 9486;
    act(ACTION_TRANSFER_BEGIN); s.transferPresented = true; act(ACTION_TRANSFER_MODE);
    if (!s.transfer.importing || s.transfer.slot != 0) return 9488;
    s.transferPresented = true; act(ACTION_TRANSFER_SLOT);
    if (s.transfer.slot != 1) return 9489;
    act(ACTION_TRANSFER_CANCEL); io.files["bank.ini"] = "[profile1]\nkept=yes\n";
    act(ACTION_TRANSFER_BEGIN); s.transferPresented = true; act(ACTION_TRANSFER_MODE);
    if (s.transfer.slot != 2) return 9490;
    s.transferPath = "input.gcprofile"; s.transferPresented = true; act(ACTION_TRANSFER_PREVIEW);
    s.transfer.needsDiscard = false; s.transfer.loadAfterImport = true; s.transfer.discardConfirmed = true;
    s.edit.active = true; s.transferPresented = true; const int commitsBefore = io.commits;
    act(ACTION_TRANSFER_COMMIT);
    if (io.commits != commitsBefore || !s.transfer.needsDiscard || s.transfer.discardConfirmed || s.transferPresented) return 9491;
    return 0;
}
}
int run_tui_transfer_tests() { return tui_transfer_fixture::run(); }
