// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "linux_tui_internal.h"
#include "linux_tui_source_edit.h"
#include "profile_document_slot.h"
#include <cstring>
#include <memory>

namespace {
static std::string encode(const ProfileDocument& d) {
    auto bank = std::make_unique<ProfileBank>(); bank->count = 1; bank->documents[0] = d;
    std::string bytes; (void)profile_bank_encode(*bank, bytes); return bytes;
}
}
int run_tui_source_edit_tests() {
    long long hz = 17;
    for (const long long value : {0LL, 1LL, -1LL, 123500000LL, -100000500LL, LLONG_MAX, LLONG_MIN}) {
        if (!profile_editor_parse_mhz(profile_editor_format_mhz(value).c_str(), hz) || hz != value) return 9241;
    }
    for (const char* invalid : {"", "-", "1.", ".5", "1.0000001", "1e3", " 1", "1 MHz", "--1",
        "9223372036854.775808", "-9223372036854.775809", "9223372036855", "999999999999999999999999999999999"}) {
        hz = 17;
        if (profile_editor_parse_mhz(invalid, hz) || hz != 17) return 9242;
    }
    if (!profile_editor_parse_mhz("+0.000500", hz) || hz != 500) return 9243;
    char edit[32] = "100"; bool selected = true;
    if (tui_edit_insert_character(edit, sizeof(edit), &selected, '.') || !selected || std::strcmp(edit, "100")) return 9244;
    if (!tui_edit_insert_character(edit, sizeof(edit), &selected, '1', true) ||
        !tui_edit_insert_character(edit, sizeof(edit), &selected, '.', true) ||
        tui_edit_insert_character(edit, sizeof(edit), &selected, '.', true) || std::strcmp(edit, "1.")) return 9245;

    auto state = std::make_unique<TuiState>();
    auto& d = state->document.value;
    d = profile_document_make_empty(); state->document.valid = true;
    d.domains = PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_GRAPHICS_OFFSET | PROFILE_DOMAIN_MEMORY | PROFILE_DOMAIN_POWER | PROFILE_DOMAIN_LOCK;
    d.topologyCount = 3; d.sourceTopologyComplete = true;
    for (int i = 0; i < 3; ++i) d.topology[i] = {42 + i, 77 + i, 650000 + i * 50000, i == 1 ? 2 : 1};
    d.points[0].kind = d.points[1].kind = PROFILE_POINT_CAPTURED_OFFSET;
    d.points[0].hasObservedFrequency = true; d.points[0].observedFrequencyHz = 1200123456;
    d.points[1].capturedOffsetHz = -100000500;
    d.points[2].kind = PROFILE_POINT_PROJECTION;
    d.graphicsOffset.present = true; d.graphicsOffset.offsetHz = 500;
    d.graphicsOffset.exclusionPolicy = PROFILE_EXCLUDE_IDENTITIES;
    d.graphicsOffset.excludedCount = 1; d.graphicsOffset.excludedTopologyIds[0] = 43;
    d.hasMemoryOffset = true; d.memoryOffsetHz = -123500000; d.memorySurface = CLOCK_OFFSET_SURFACE_GLOBAL;
    d.power.present = true; d.power.ratio = {1, 3};
    d.lock.present = true; d.lock.mode = LOCK_MODE_FLATTEN; d.lock.anchorTopologyId = 44;
    d.lock.targetFrequencyHz = 1500000000; d.lock.trackAnchor = true;
    state->document.saved = d;
    const auto original = encode(d);
    if (original.empty() || !tui_begin_source_edit(*state, TUI_FIELD_SOURCE_POINT, 0) ||
        std::strcmp(state->edit.text, "1200.123456") || !state->edit.selectAll || state->serviceOnline ||
        encode(d) != original || profile_editor_document_dirty(state->document)) return 9246;
    state->sourceCheckLines = {"old report"}; state->draftAttached = true;
    // Even typing the observed number is an explicit kind conversion, without
    // a baseline, destination index, clamp or edit to any other document field.
    auto expected = std::make_unique<ProfileDocument>(d);
    expected->points[0].kind = PROFILE_POINT_ABSOLUTE_FREQUENCY;
    expected->points[0].absoluteFrequencyHz = 1200123456;
    if (!tui_commit_source_edit(*state) || state->edit.active || state->draftAttached ||
        !state->sourceCheckLines.empty() || !state->dirty || !profile_editor_document_dirty(state->document) ||
        encode(d) != encode(*expected)) return 9247;
    if (tui_begin_source_edit(*state, TUI_FIELD_SOURCE_POINT, 1) ||
        tui_begin_source_edit(*state, TUI_FIELD_SOURCE_POINT, -1) ||
        tui_begin_source_edit(*state, TUI_FIELD_SOURCE_POINT, 3)) return 9248;
    if (!tui_begin_source_edit(*state, TUI_FIELD_SOURCE_POINT, 2) || state->edit.text[0]) return 9249;
    const auto beforeInvalid = encode(d);
    for (const char* invalid : {"0", "-1", "1.0000001", "9223372036855"}) {
        std::snprintf(state->edit.text, sizeof(state->edit.text), "%s", invalid);
        if (tui_commit_source_edit(*state) || !state->edit.active || encode(d) != beforeInvalid) return 9250;
    }
    std::snprintf(state->edit.text, sizeof(state->edit.text), "9999.000001");
    if (!tui_commit_source_edit(*state) || d.points[2].kind != PROFILE_POINT_ABSOLUTE_FREQUENCY ||
        d.points[2].absoluteFrequencyHz != 9999000001LL || d.lock.targetFrequencyHz != 1500000000) return 9251;
    if (!tui_begin_source_edit(*state, TUI_FIELD_SOURCE_MEMORY, 0) || std::strcmp(state->edit.text, "-123.5")) return 9252;
    std::snprintf(state->edit.text, sizeof(state->edit.text), "-0.000500");
    // The document keeps sub-grid intent even when the runtime's half-MHz
    // memory representation cannot carry it. No precise legacy value survives.
    if (!tui_commit_source_edit(*state) || d.memoryOffsetHz != -500 || d.memorySurface != CLOCK_OFFSET_SURFACE_GLOBAL ||
        state->desired.memOffsetPrecise || state->desired.memOffsetHz ||
        d.power.ratio.numerator != 1 || d.power.ratio.denominator != 3) return 9253;
    if (!tui_begin_source_edit(*state, TUI_FIELD_SOURCE_GRAPHICS, 0) || std::strcmp(state->edit.text, "0.0005")) return 9254;
    std::snprintf(state->edit.text, sizeof(state->edit.text), "0");
    if (!tui_commit_source_edit(*state) || d.graphicsOffset.offsetHz || !d.graphicsOffset.present ||
        d.graphicsOffset.excludedCount != 1 || d.graphicsOffset.excludedTopologyIds[0] != 43) return 9255;
    state->profileLoadPending = true;
    if (tui_begin_source_edit(*state, TUI_FIELD_SOURCE_MEMORY, 0)) return 9256;
    state->profileLoadPending = false; state->sourceCheckVisible = true;
    if (tui_begin_source_edit(*state, TUI_FIELD_SOURCE_MEMORY, 0)) return 9257;
    state->sourceCheckVisible = false;
    d.applicationScope = PROFILE_SCOPE_EXPLICIT_RELEASE;
    if (tui_begin_source_edit(*state, TUI_FIELD_SOURCE_MEMORY, 0)) return 9258;
    d.applicationScope = PROFILE_SCOPE_UPDATE_INCLUDED;

    DesiredSettings display{}; TuiViewModel vm{};
    vm.desired = &display; vm.sourceDocument = &d; vm.tab = TUI_TAB_PROFILES; vm.currentSlot = 1;
    for (const auto size : {TuiRect{0,0,72,24}, TuiRect{0,0,100,32}, TuiRect{0,0,160,48}}) {
        bool point = false, memory = false, graphics = false;
        TuiLayout layout{}; build_tui_layout(vm, size.width, size.height, &layout);
        const int count = layout.sourceLineCount;
        for (int scroll = 0; scroll < count; ++scroll) {
            vm.sourceScroll = scroll; build_tui_layout(vm, size.width, size.height, &layout);
            if (!tui_layout_actions_valid(layout)) return 9259;
            for (const auto& action : layout.actions) if (action.type == ACTION_FIELD_EDIT) {
                if (action.index == TUI_FIELD_SOURCE_POINT) {
                    if (action.value == 1 || action.value >= 3) return 9260;
                    point = true;
                }
                memory |= action.index == TUI_FIELD_SOURCE_MEMORY;
                graphics |= action.index == TUI_FIELD_SOURCE_GRAPHICS;
            }
        }
        if (!point || !memory || !graphics) return 9261;
        vm.sourceScroll = 0;
    }
    std::vector<std::string> report{"Report only"}; vm.sourceCheckLines = &report;
    TuiLayout layout{}; build_tui_layout(vm, 100, 32, &layout);
    for (const auto& action : layout.actions) if (action.type == ACTION_FIELD_EDIT) return 9262;
    std::string slot; ProfileCodecError error;
    if (!profile_slot_encode(d, 3, slot, &error)) return 9263;
    auto loaded = std::make_unique<ProfileDocument>(); bool found = false;
    if (!profile_document_slot_decode(slot, 3, *loaded, found, error) || !found || encode(*loaded) != encode(d) ||
        loaded->points[1].capturedOffsetHz != -100000500) return 9264;
    return 0;
}
