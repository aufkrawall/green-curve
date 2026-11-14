// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "linux_tui_internal.h"
#include "linux_debug_log.h"
#include "profile_document_slot.h"
#include <cstring>
#include <memory>

namespace tui_document_fixture {
static void linux_debug_logf(const char*, ...) {}
#include "linux_tui_profile_load.h"
struct IO {
    std::string bytes = "[preferences]\ncaption=kept\n", concurrent;
    int reads = 0, writes = 0;
    bool read(const std::string&, std::string& out, size_t limit, bool, ProfileCodecError& error) {
        ++reads;
        if (bytes.size() > limit) return profile_codec::fail(&error, 0, "size limit");
        out = bytes; return true;
    }
    bool commit(const std::string&, const ProfileBankEdit& edit, ProfileCodecError& error) {
        if (!concurrent.empty()) { bytes = concurrent; concurrent.clear(); }
        return profile_bank_commit_edit(edit, [&](std::string& out) { out = bytes; return true; },
            [&](const std::string& out) { ++writes; bytes = out; return true; }, &error);
    }
};
static int run() {
    IO io; ProfileCodecError error;
    auto source = std::make_unique<ProfileDocument>(profile_document_make_empty()); auto& d = *source;
    std::snprintf(d.displayName, sizeof(d.displayName), "Exact source");
    d.domains = PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_MEMORY | PROFILE_DOMAIN_POWER;
    d.sourceTopologyComplete = true; d.topologyCount = 3;
    for (int i = 0; i < 3; ++i) d.topology[i] = {42 + i, i, 650000 + i * 50000, 1};
    d.points[0].kind = d.points[1].kind = PROFILE_POINT_CAPTURED_OFFSET;
    d.points[1].capturedOffsetHz = -100000500;
    d.points[2].kind = PROFILE_POINT_ABSOLUTE_FREQUENCY; d.points[2].absoluteFrequencyHz = 1200123456;
    d.hasMemoryOffset = true; d.memoryOffsetHz = -123500000; d.memorySurface = CLOCK_OFFSET_SURFACE_GLOBAL;
    d.power.present = true; d.power.ratio = {1, 3};
    if (!profile_document_slot_save(io, "bank", 3, d, error) || io.writes != 1 ||
        io.bytes.find("caption=kept") == std::string::npos) return 9210;
    auto decoded = std::make_unique<ProfileDocument>(); bool found = false;
    if (!profile_document_slot_load(io, "bank", 3, *decoded, found, error) || !found ||
        !profile_document_semantically_equal(d, *decoded)) return 9211;
    auto restarted = std::make_unique<TuiState>(); int binds = 0;
    tui_request_profile_load(*restarted, [&](int slot, ProfileEditorCandidate& out, char* why, size_t size) {
        bool present = false;
        if (!profile_document_slot_load(io, "bank", slot, *decoded, present, error) || !present) {
            std::snprintf(why, size, "%s", error.message); return false;
        }
        profile_editor_source_candidate(*decoded, out); return true;
    }, [&](TuiState& draft) { ++binds; draft.draftAttached = true; draft.dirty = true; });
    // A new process selects its slot explicitly, independently of file-local IDs.
    if (binds) return 9212; // Default slot zero is invalid and cannot replace state.
    restarted->currentSlot = 3;
    tui_request_profile_load(*restarted, [&](int slot, ProfileEditorCandidate& out, char*, size_t) {
        bool present = false;
        if (!profile_document_slot_load(io, "bank", slot, *decoded, present, error) || !present) return false;
        profile_editor_source_candidate(*decoded, out); return true;
    }, [&](TuiState& draft) { ++binds; draft.draftAttached = true; draft.dirty = true; });
    if (!restarted->document.valid || restarted->draftAttached || binds != 1 || restarted->tab != TUI_TAB_PROFILES ||
        profile_editor_document_dirty(restarted->document) || restarted->desired.hasCurvePoint[0] ||
        restarted->desired.curvePointCaptured[0] || desired_memory_hz(restarted->desired) != -123500000) return 9213;
    if (!profile_document_slot_save(io, "bank", 3, restarted->document.value, error)) return 9214;
    auto bank = std::make_unique<ProfileBank>(); bank->count = 1;
    if (!profile_document_slot_load(io, "bank", 3, bank->documents[0], found, error) || !found) return 9215;
    std::string portable;
    if (!profile_bank_encode(*bank, portable, &error) || !profile_bank_decode(portable, *bank, &error) ||
        !profile_document_semantically_equal(d, bank->documents[0]) ||
        bank->documents[0].points[0].kind != PROFILE_POINT_CAPTURED_OFFSET ||
        bank->documents[0].points[1].capturedOffsetHz != -100000500 ||
        bank->documents[0].points[2].absoluteFrequencyHz != 1200123456) return 9216;
    std::string utf16;
    if (!profile_config::encode_like(std::string("\xff\xfe", 2), io.bytes, utf16, &error)) return 9222;
    io.bytes = utf16;
    if (!profile_document_slot_load(io, "bank", 3, *decoded, found, error) || !found ||
        !profile_document_slot_save(io, "bank", 3, *decoded, error) ||
        io.bytes.compare(0, 2, "\xff\xfe") || !profile_document_semantically_equal(d, *decoded)) return 9223;
    IO mirror;
    if (!profile_slot_encode(d, 1, mirror.bytes, &error)) return 9224;
    mirror.bytes.replace(0, std::strlen("[profile1]"), "[controls]");
    if (!profile_document_slot_load(mirror, "bank", 1, *decoded, found, error) || !found ||
        !profile_document_semantically_equal(d, *decoded)) return 9225;
    if (!profile_document_slot_load(mirror, "bank", 2, *decoded, found, error) || found ||
        !profile_document_semantically_equal(d, *decoded)) return 9226;
    const auto reviewed = io.bytes; const int beforeWrites = io.writes;
    io.concurrent = reviewed + "\n[other]\nvalue=1\n";
    if (profile_document_slot_save(io, "bank", 3, d, error) || io.writes != beforeWrites || io.bytes == reviewed) return 9217;
    restarted->document.value.memoryOffsetHz = -100000000;
    if (!profile_editor_document_dirty(restarted->document)) return 9218;
    restarted->document.value.memoryOffsetHz = -123500000;
    std::snprintf(restarted->document.value.displayName, sizeof(restarted->document.value.displayName), "Edited source");
    if (!profile_editor_document_dirty(restarted->document)) return 9221;
    // Preview arithmetic must not truncate a structurally valid huge request into an int.
    ProfileEditorCandidate candidate{}; d.graphicsOffset.present = true; d.domains |= PROFILE_DOMAIN_GRAPHICS_OFFSET;
    d.graphicsOffset.offsetHz = LLONG_MAX;
    profile_editor_source_candidate(d, candidate);
    if (candidate.settings.hasGpuOffset || candidate.source.value.graphicsOffset.offsetHz != LLONG_MAX) return 9219;
    TuiViewModel vm{}; vm.desired = &restarted->desired; vm.sourceDocument = &restarted->document.value;
    vm.tab = TUI_TAB_PROFILES; vm.currentSlot = 3;
    TuiLayout layout{}; build_tui_layout(vm, 100, 32, &layout);
    std::string cells; for (const auto& cell : layout.cells) cells += cell.glyph;
    if (!tui_layout_actions_valid(layout) || cells.find("-123500000 physical Hz") == std::string::npos ||
        cells.find("650000 uV: 0 Hz captured offset") == std::string::npos ||
        cells.find("1200123456 Hz absolute") == std::string::npos) return 9220;
    return 0;
}
}
int run_tui_document_tests() { return tui_document_fixture::run(); }
