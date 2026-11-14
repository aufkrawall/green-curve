// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "linux_tui_internal.h"
#include "linux_debug_log.h"
#include "profile_editor_capture.h"
#include "linux_tui_capture_input.h"
#include <cstring>

namespace tui_capture_fixture {
static void linux_debug_logf(const char*, ...) {}
#include "linux_tui_profile_load.h"
#include "linux_tui_capture_draft.h"

static HardwareCaptureObservation observation(gc_u32 domains = 31) {
    HardwareCaptureObservation o{}; o.valid = o.binding.valid = 1;
    o.schemaVersion = HARDWARE_CAPTURE_SCHEMA_CURRENT; o.requestedDomains = domains;
    o.gpuGeneration = 3; o.revision = o.fingerprint = 7; o.topologyComplete = 1; o.pointCount = 3;
    for (int i = 0; i < 9; ++i) if (domains & (1u << i)) hardware_capture_domain(o, 1 << i, CAPTURE_UNSUPPORTED, -100001);
    for (int i = 0; i < 3; ++i) {
        auto& p = o.points[i]; p.index = 21 + i; p.voltageUv = 650000 + i * 12500;
        p.writableState = i == 0 ? 2 : 1; p.frequencyValid = p.offsetValid = 1;
        p.frequencyHz = 1200123456; p.offsetHz = i == 2 ? -100000500 : 0;
    }
    hardware_capture_domain(o, PROFILE_DOMAIN_VF_CURVE, CAPTURE_COMPLETE);
    o.graphics.valid = o.graphics.writable = 1; o.graphics.surface = CLOCK_OFFSET_SURFACE_P0; o.graphics.grid = 1000000;
    o.composition = CAPTURE_COMPOSITION_INDEPENDENT;
    hardware_capture_domain(o, PROFILE_DOMAIN_GRAPHICS_OFFSET, CAPTURE_COMPLETE);
    o.memory.valid = o.memory.writable = 1; o.memory.surface = CLOCK_OFFSET_SURFACE_GLOBAL; o.memory.grid = 500000;
    o.memory.value = -123500000; hardware_capture_domain(o, PROFILE_DOMAIN_MEMORY, CAPTURE_COMPLETE);
    o.power.currentValid = o.power.defaultValid = o.power.writable = 1;
    o.power.defaultMw = 300000; o.power.currentMw = 200000; hardware_capture_domain(o, PROFILE_DOMAIN_POWER, CAPTURE_COMPLETE);
    return o;
}
static std::string encode(const ProfileDocument& d) {
    auto bank = std::make_unique<ProfileBank>(); bank->count = 1; bank->documents[0] = d;
    std::string bytes; (void)profile_bank_encode(*bank, bytes); return bytes;
}
static int run() {
    auto sample = std::make_unique<HardwareCaptureObservation>(observation());
    auto output = std::make_unique<ProfileDocument>(profile_document_make_empty());
    output->domains = PROFILE_DOMAIN_MEMORY; output->hasMemoryOffset = true; output->memoryOffsetHz = 17000000;
    std::vector<std::string> lines; const auto empty = encode(*output);
    if (!hardware_capture_observation_valid(*sample) || empty.empty()) return 9295;
    if (profile_editor_capture(*sample, false, false, *output, lines) != PROFILE_COMMAND_DECISION || encode(*output) != empty) return 9296;
    std::string report; for (const auto& line : lines) report += line + '\n';
    if (report.find("explicitly allow omitted settings") == std::string::npos || report.find("--allow-partial") != std::string::npos) return 9297;
    if (profile_editor_capture(*sample, true, false, *output, lines) != PROFILE_COMMAND_DECISION || encode(*output) != empty) return 9298;
    report.clear(); for (const auto& line : lines) report += line + '\n';
    if (report.find("No flatten or hard pin is inferred") == std::string::npos || report.find("--representation") != std::string::npos) return 9299;

    auto state = std::make_unique<TuiState>(); state->currentSlot = 3;
    state->document.valid = true; state->document.value = state->document.saved = *output;
    std::snprintf(state->document.value.displayName, sizeof(state->document.value.displayName), "Previous draft");
    state->document.saved = state->document.value;
    state->edit.active = true; state->edit.field = TUI_FIELD_SOURCE_MEMORY; std::strcpy(state->edit.text, "-");
    const auto before = encode(state->document.value); int reads = 0, binds = 0;
    const auto read = [&](gc_u32 domains, HardwareCaptureObservation& out, ProfileCodecError&) {
        ++reads; out = observation(domains); return true;
    };
    const auto bind = [&](TuiState& draft) { ++binds; draft.draftAttached = true; };
    tui_capture_begin(*state);
    if (!state->capture.active || !state->capture.needsDiscard || state->capturePresented) return 9300;
    tui_capture_read(*state, read);
    if (reads || tui_capture_accept(*state, bind) || binds) return 9301;
    state->capturePresented = true; tui_capture_read(*state, read);
    if (reads != 1 || !state->capture.hasObservation || state->capture.omissions != PROFILE_DOMAIN_LOCK ||
        state->capturePresented || tui_capture_accept(*state, bind) || encode(state->document.value) != before ||
        std::strcmp(state->edit.text, "-")) return 9302;
    state->capturePresented = true;
    if (tui_capture_accept(*state, bind)) return 9303;
    state->capture.allowPartial = true;
    if (tui_capture_accept(*state, bind) || binds || !state->edit.active) return 9304;
    state->capture.discardConfirmed = true;
    if (!tui_capture_accept(*state, bind) || reads != 1 || binds != 1 || state->capture.active || state->captureObservation ||
        state->edit.active || state->draftAttached || state->currentSlot != 3 || !state->document.unsaved ||
        !profile_editor_document_dirty(state->document)) return 9305;
    const auto& d = state->document.value;
    if (d.points[0].kind != PROFILE_POINT_ABSENT || !d.points[0].hasObservedOffset ||
        d.points[1].kind != PROFILE_POINT_CAPTURED_OFFSET || d.points[1].capturedOffsetHz ||
        d.points[2].capturedOffsetHz != -100000500 || d.memoryOffsetHz != -123500000 ||
        d.memorySurface != CLOCK_OFFSET_SURFACE_GLOBAL || d.power.ratio.numerator != 2 || d.power.ratio.denominator != 3 ||
        d.source.captureComplete || d.lock.present) return 9306;
    const auto captured = encode(d); if (captured.empty()) return 9307;
    state->document.unsaved = false; state->document.saved = state->document.value;
    if (profile_editor_document_dirty(state->document)) return 9308;
    state->edit.active = true; std::strcpy(state->edit.text, "bad");
    tui_capture_begin(*state); state->capturePresented = true; tui_capture_read(*state, read); tui_capture_cancel(*state);
    if (reads != 2 || state->capture.active || encode(state->document.value) != captured ||
        !state->edit.active || std::strcmp(state->edit.text, "bad") || binds != 1) return 9309;
    tui_capture_begin(*state); state->capturePresented = true;
    tui_capture_read(*state, [&](gc_u32, auto&, ProfileCodecError& error) { ++reads; return profile_codec::fail(&error, 0, "session lost"); });
    state->capturePresented = true; state->capture.allowPartial = state->capture.discardConfirmed = true;
    if (state->capture.hasObservation || tui_capture_accept(*state, bind) || encode(state->document.value) != captured) return 9310;
    tui_capture_cancel(*state);

    state->edit = {}; tui_capture_begin(*state); state->capture.domains = 15;
    state->capturePresented = true; tui_capture_read(*state, read);
    if (!state->capture.hasObservation || state->capture.omissions || state->capture.needsDiscard ||
        tui_capture_accept(*state, bind)) return 9316;
    state->capturePresented = true;
    if (!tui_capture_accept(*state, bind) || !state->document.unsaved || !state->dirty || state->draftAttached) return 9317;
    const int beforeEmpty = reads;
    tui_capture_begin(*state); state->capture.domains = 0; state->capturePresented = true; tui_capture_read(*state, read);
    if (reads != beforeEmpty || state->capture.hasObservation) return 9318;
    tui_capture_cancel(*state);
    tui_capture_begin(*state); state->capturePresented = true;
    tui_capture_read(*state, [&](gc_u32 domains, HardwareCaptureObservation& o, ProfileCodecError&) {
        ++reads; o = {}; o.valid = o.binding.valid = 1; o.schemaVersion = HARDWARE_CAPTURE_SCHEMA_CURRENT;
        o.requestedDomains = domains; o.gpuGeneration = 3; o.revision = o.fingerprint = 7;
        for (int i = 0; i < 9; ++i) if (domains & (1u << i)) hardware_capture_domain(o, 1 << i, CAPTURE_UNSUPPORTED, -100001);
        return true;
    });
    state->capturePresented = true; state->capture.allowPartial = state->capture.discardConfirmed = true;
    if (state->capture.hasObservation || tui_capture_accept(*state, bind)) return 9319;
    tui_capture_cancel(*state);

    DesiredSettings display{}; TuiViewModel vm{}; vm.desired = &display; vm.tab = TUI_TAB_PROFILES; vm.currentSlot = 1;
    vm.capture.active = true;
    for (const auto size : {TuiRect{0,0,72,24}, TuiRect{0,0,100,32}, TuiRect{0,0,160,48}}) {
        TuiLayout layout{}; build_tui_layout(vm, size.width, size.height, &layout);
        int domains = 0; bool cancel = false, readButton = false;
        for (const auto& a : layout.actions) {
            domains += a.type == ACTION_CAPTURE_DOMAIN; cancel |= a.type == ACTION_CAPTURE_CANCEL; readButton |= a.type == ACTION_CAPTURE_READ;
            if (a.type < ACTION_CAPTURE_DOMAIN || a.type > ACTION_CAPTURE_CANCEL) return 9311;
        }
        if (!tui_layout_actions_valid(layout) || !layout.captureVisible || domains != 9 || !cancel || !readButton) return 9312;
        lines.assign(20, "Snapshot detail"); lines.emplace_back("Long capture constraint with requested=123456789 and a trailing maximum=987654321");
        vm.capture.report = &lines; vm.capture.hasObservation = true; vm.capture.omissions = PROFILE_DOMAIN_LOCK;
        vm.capture.needsDiscard = true;
        for (int choices = 0; choices < 3; ++choices) {
            vm.capture.allowPartial = choices > 0; vm.capture.discardConfirmed = choices > 1;
            build_tui_layout(vm, size.width, size.height, &layout);
            bool accept = false; for (const auto& a : layout.actions) accept |= a.type == ACTION_CAPTURE_ACCEPT;
            if (!tui_layout_actions_valid(layout) || accept != (choices == 2) || layout.captureVisibleRows <= 0) return 9313;
        }
        vm.capture.scroll = layout.captureLineCount; build_tui_layout(vm, size.width, size.height, &layout);
        std::string cells; for (const auto& cell : layout.cells) cells += cell.glyph;
        if (cells.find("maximum=987654321") == std::string::npos) return 9314;
        vm.capture = {}; vm.capture.active = true;
    }
    TuiLayout small{}; build_tui_layout(vm, 71, 23, &small);
    if (!small.tooSmall || small.captureVisible || !small.actions.empty()) return 9315;
    state->capture.active = true; state->capturePresented = false; state->layout = small;
    int activated = 0, actions = 0, direction = 0, hits = 0;
    const auto action = [&](const ClickAction& a) { ++actions; if (a.type == ACTION_CAPTURE_CANCEL) tui_capture_cancel(*state); };
    const auto focus = [&](int d) { direction += d; };
    const auto activate = [&] { ++activated; action(state->layout.actions[state->focusIndex]); };
    const auto hit = [&](int, int) { ++hits; return 0; };
    TuiInputEvent event{}; event.type = TUI_INPUT_ENTER;
    if (!tui_capture_modal_event(*state, event, action, focus, activate, hit) || actions || activated) return 9320;
    tui_capture_present_layout(*state); if (state->capturePresented) return 9321;
    build_tui_layout(vm, 72, 24, &state->layout); tui_capture_present_layout(*state);
    if (!state->capturePresented || state->focusIndex < 0 ||
        state->layout.actions[state->focusIndex].type != ACTION_CAPTURE_CANCEL) return 9322;
    event.type = TUI_INPUT_CHARACTER; event.character = '7';
    const auto priorText = std::string(state->edit.text);
    if (!tui_capture_modal_event(*state, event, action, focus, activate, hit) || actions || std::string(state->edit.text) != priorText) return 9323;
    event.type = TUI_INPUT_TAB; (void)tui_capture_modal_event(*state, event, action, focus, activate, hit);
    if (direction != 1) return 9324;
    state->layout.captureVisibleRows = 4; state->layout.captureLineCount = 11;
    event.type = TUI_INPUT_PAGE_DOWN; (void)tui_capture_modal_event(*state, event, action, focus, activate, hit);
    if (state->capture.scroll != 4) return 9325;
    event.type = TUI_INPUT_MOUSE; event.mousePress = true; event.mouseButton = 64;
    (void)tui_capture_modal_event(*state, event, action, focus, activate, hit);
    if (state->capture.scroll != 3 || hits) return 9326;
    event.type = TUI_INPUT_ENTER; (void)tui_capture_modal_event(*state, event, action, focus, activate, hit);
    if (state->capture.active || activated != 1 || actions != 1) return 9327;
    if (tui_capture_modal_event(*state, event, action, focus, activate, hit)) return 9328;
    tui_capture_begin(*state); event.type = TUI_INPUT_ESCAPE;
    (void)tui_capture_modal_event(*state, event, action, focus, activate, hit);
    if (state->capture.active || actions != 2) return 9329;
    return 0;
}
}
int run_tui_capture_draft_tests() { return tui_capture_fixture::run(); }
