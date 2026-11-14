// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "linux_tui_internal.h"
#include "profile_editor_check.h"
#include <memory>

namespace {
struct Capture {
    int calls = 0;
    bool fail = false, malformed = false, alternateNonzero = false, narrowAlternate = false;
    bool operator()(gc_u32 domains, HardwareCaptureObservation& out, ProfileCodecError& error) {
        ++calls;
        if (fail) return profile_codec::fail(&error, 0, "session unavailable");
        out = {}; out.valid = 1; out.schemaVersion = HARDWARE_CAPTURE_SCHEMA_CURRENT; out.gpuGeneration = 3;
        out.binding.valid = 1; out.requestedDomains = domains; out.revision = out.fingerprint = 7;
        for (int i = 0; i < 9; ++i) if (domains & (1u << i)) hardware_capture_domain(out, 1 << i, CAPTURE_UNSUPPORTED, -100001);
        out.topologyComplete = 1; out.pointCount = malformed ? 129 : 1;
        out.points[0].index = 22; out.points[0].voltageUv = 662000; out.points[0].writableState = 1;
        out.points[0].frequencyValid = out.points[0].offsetValid = 1; out.points[0].frequencyHz = 1500000000;
        hardware_capture_domain(out, PROFILE_DOMAIN_VF_CURVE, CAPTURE_COMPLETE);
        out.composition = CAPTURE_COMPOSITION_INDEPENDENT;
        out.graphics.valid = out.graphics.writable = 1; out.graphics.surface = CLOCK_OFFSET_SURFACE_P0; out.graphics.grid = 1000000;
        hardware_capture_domain(out, PROFILE_DOMAIN_GRAPHICS_OFFSET, CAPTURE_COMPLETE);
        if (domains & PROFILE_DOMAIN_MEMORY) {
            out.memory.valid = out.memory.writable = out.memory.rangeValid = 1;
            out.memory.surface = CLOCK_OFFSET_SURFACE_P0; out.memory.grid = 500000;
            out.memory.minimum = -3000000000LL; out.memory.maximum = 3000000000LL;
            out.memoryAlternate = out.memory; out.memoryAlternate.surface = CLOCK_OFFSET_SURFACE_GLOBAL;
            out.memoryAlternate.value = alternateNonzero ? 500000 : 0;
            if (narrowAlternate) out.memoryAlternate.minimum = 0;
            hardware_capture_domain(out, PROFILE_DOMAIN_MEMORY, alternateNonzero ? CAPTURE_PARTIAL : CAPTURE_COMPLETE);
        }
        return true;
    }
};
}
int run_tui_profile_check_tests() {
    auto source = std::make_unique<ProfileDocument>(profile_document_make_empty());
    source->domains = PROFILE_DOMAIN_MEMORY; source->hasMemoryOffset = true;
    source->memoryOffsetHz = -123500000; source->memorySurface = CLOCK_OFFSET_SURFACE_P0;
    auto bank = std::make_unique<ProfileBank>(); bank->count = 1; bank->documents[0] = *source;
    std::string original; ProfileCodecError error;
    if (!profile_bank_encode(*bank, original, &error)) return 9230;
    Capture state; std::vector<std::string> lines;
    const auto capture = [&](gc_u32 domains, auto& out, auto& issue) { return state(domains, out, issue); };
    const auto text = [&] { std::string out; for (const auto& line : lines) out += line + "\n"; return out; };
    if (profile_editor_check(*source, 0, CLOCK_OFFSET_SURFACE_GLOBAL, capture, lines) != PROFILE_COMMAND_DECISION ||
        state.calls != 1 || text().find("-123500000 physical Hz; surface=global") == std::string::npos ||
        text().find("Control feasibility: disposition=representable") == std::string::npos ||
        text().find("Draft: ") == std::string::npos) return 9231;
    std::string after; bank->documents[0] = *source;
    if (!profile_bank_encode(*bank, after, &error) || after != original) return 9232;
    if (profile_editor_check(*source, CLOCK_OFFSET_SURFACE_P0, 0, capture, lines) != PROFILE_COMMAND_INVALID || state.calls != 1 ||
        profile_editor_check(*source, 0, 3, capture, lines) != PROFILE_COMMAND_INVALID || state.calls != 1) return 9233;
    state.narrowAlternate = true;
    if (profile_editor_check(*source, 0, CLOCK_OFFSET_SURFACE_GLOBAL, capture, lines) != PROFILE_COMMAND_DECISION ||
        text().find("outside-control-range") == std::string::npos || text().find("Operation feasibility: refused") == std::string::npos) return 9234;
    state.narrowAlternate = false; state.alternateNonzero = true;
    if (profile_editor_check(*source, 0, 0, capture, lines) != PROFILE_COMMAND_DECISION ||
        text().find("other-clock-control-relationship-unresolved") == std::string::npos) return 9235;
    state.alternateNonzero = false; state.malformed = true;
    if (profile_editor_check(*source, 0, 0, capture, lines) != PROFILE_COMMAND_IO ||
        text().find("destination capture is invalid") == std::string::npos) return 9236;
    state.malformed = false; state.fail = true;
    if (profile_editor_check(*source, 0, 0, capture, lines) != PROFILE_COMMAND_IO ||
        text().find("session unavailable") == std::string::npos) return 9237;
    // Reports wrap long structured reasons rather than clipping their trailing bounds.
    lines = {"Mapping and feasibility report with a long explanatory constraint followed by requested=123456789 and maximum=987654321"};
    TuiViewModel vm{}; DesiredSettings display{}; vm.desired = &display; vm.sourceDocument = source.get();
    vm.sourceCheckLines = &lines; vm.tab = TUI_TAB_PROFILES; vm.currentSlot = 1;
    for (const auto size : {TuiRect{0,0,72,24}, TuiRect{0,0,100,32}, TuiRect{0,0,160,48}}) {
        TuiLayout layout{}; build_tui_layout(vm, size.width, size.height, &layout);
        if (!tui_layout_actions_valid(layout) || layout.sourceVisibleRows <= 0 || layout.sourceLineCount < 1) return 9238;
        bool check = false, memory = false;
        for (const auto& a : layout.actions) { check |= a.type == ACTION_CHECK_SOURCE; memory |= a.type == ACTION_SOURCE_MEMORY_CHOICE; }
        if (!check || !memory) return 9239;
        std::string cells; for (const auto& cell : layout.cells) cells += cell.glyph;
        if (cells.find("maximum=987654321") == std::string::npos) return 9240;
    }
    return 0;
}
