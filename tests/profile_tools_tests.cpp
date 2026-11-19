// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// The profile tool dialogs (import current GPU settings, import/export files)
// sit on these shared engines; the Windows dialogs and the TUI only present
// them. Cases 9970-9998.
#include "profile_editor_capture.h"
#include "profile_capture_save.h"
#include "profile_capture_proposal_fixture.h"
#include "gui_profile_transfer_work.h"
#include "profile_legacy_document.h"
#include "profile_ini_slot.h"
#include <map>
#include <cstring>

namespace {
struct ToolsIO {
    std::map<std::string, std::string> files;
    int writes = 0;
    bool read(const std::string& path, std::string& out, size_t maximum, bool missing, ProfileCodecError& error) {
        const auto it = files.find(path);
        if (it == files.end()) {
            if (missing) { out.clear(); return true; }
            return profile_codec::fail(&error, 0, "fixture input missing");
        }
        if (it->second.size() > maximum) return profile_codec::fail(&error, 0, "fixture input too large");
        out = it->second; return true;
    }
    bool write(const std::string& path, const std::string& bytes, ProfileCodecError&) { ++writes; files[path] = bytes; return true; }
    bool distinct(const std::string& config, const std::string& file, ProfileCodecError& error) {
        return config != file || profile_codec::fail(&error, 0, "fixture paths must differ");
    }
    bool legacy(const std::string& text, int slot, ProfileDocument& document, bool& found, ProfileCodecError& error) {
        return profile_legacy_native_slot_decode(text, PROFILE_LEGACY_WINDOWS_INI, slot, document, found, &error);
    }
    bool commit(const std::string& path, const ProfileBankEdit& edit, ProfileCodecError& error) {
        return profile_bank_commit_edit(edit, [&](std::string& current) { current = files[path]; return true; },
            [&](const std::string& replacement) { return write(path, replacement, error); }, &error);
    }
};
ProfileDocument memory_profile(long long hz, const char* name) {
    ProfileDocument d = profile_document_make_empty();
    snprintf(d.displayName, sizeof(d.displayName), "%s", name);
    d.applicationScope = PROFILE_SCOPE_REPLACE_PROFILE;
    d.domains = PROFILE_DOMAIN_MEMORY; d.hasMemoryOffset = true; d.memoryOffsetHz = hz;
    return d;
}
}  // namespace

int run_profile_tools_tests() {
    ToolsIO io;
    ProfileCodecError error{};
    io.files["bank.ini"] = "[profiles]\nlogon_slot=2\n";
    auto bank = std::make_unique<ProfileBank>();
    bank->count = 3;
    bank->documents[0] = memory_profile(1000000000LL, "Gaming");
    bank->documents[1] = memory_profile(500000000LL, "Quiet");
    bank->documents[2] = memory_profile(-250000000LL, "Cool");
    for (int i = 0; i < bank->count; ++i) bank->documents[i].documentId = (gc_u32)i + 1;
    if (!profile_bank_encode(*bank, io.files["three.gcprofile"], &error)) {
        fprintf(stderr, "9970: %s\n", error.message); return 9970;
    }

    // 9971: a portable file lists every record in file order before any choice.
    auto probe = std::make_unique<ProfileEditorTransferProbe>();
    if (!profile_editor_transfer_probe("three.gcprofile", {}, io, *probe) || probe->legacyFile ||
        probe->documents.size() != 3 || probe->sources != std::vector<int>({1, 2, 3}) ||
        strcmp(probe->documents[1].displayName, "Quiet"))
        return 9971;
    // 9972: records go exactly where the user put them; nothing else changes.
    ProfileEditorTransferOptions options;
    options.importing = true; options.sources = {3, 1}; options.slots = {4, 2};
    auto work = std::make_unique<GuiProfileTransferWork>();
    work->kind = GUI_PROFILE_TRANSFER_RUN; work->options = options; work->file = "three.gcprofile"; work->config = "bank.ini";
    gui_profile_transfer_execute(*work, io);
    auto slot = std::make_unique<ProfileDocument>();
    bool found = false;
    std::string text;
    if (!work->success || !work->committing || !profile_config::decode(io.files["bank.ini"], text, &error) ||
        !profile_document_slot_decode(text, 4, *slot, found, error) || !found || slot->memoryOffsetHz != -250000000LL ||
        !profile_document_slot_decode(text, 2, *slot, found, error) || !found || slot->memoryOffsetHz != 1000000000LL ||
        !profile_document_slot_decode(text, 1, *slot, found, error) || found ||
        text.find("logon_slot=2") == std::string::npos)
        return 9972;
    // 9973: imported slots load as ordinary records (no GPU needed for scalars).
    auto record = std::make_unique<DesiredSettings>();
    char why[192]{};
    if (!profile_document_slot_decode(text, 4, *slot, found, error) ||
        !profile_slot_document_runtime(*slot, nullptr, *record, why, sizeof(why)) || !record->hasMemOffset ||
        record->memOffsetMHz != -250)
        return 9973;
    // 9974: mismatched record/slot lists are refused before anything is read.
    work = std::make_unique<GuiProfileTransferWork>();
    work->options.importing = true; work->options.sources = {1, 2}; work->options.slots = {5};
    work->file = "three.gcprofile"; work->config = "bank.ini";
    const auto before = io.files["bank.ini"];
    gui_profile_transfer_execute(*work, io);
    if (work->success || io.files["bank.ini"] != before) return 9974;
    // 9975: export writes only the chosen slots, in slot order.
    work = std::make_unique<GuiProfileTransferWork>();
    work->options.slots = {2, 4}; work->file = "out.gcprofile"; work->config = "bank.ini";
    gui_profile_transfer_execute(*work, io);
    auto exported = std::make_unique<ProfileBank>();
    if (!work->success || !profile_bank_decode(io.files["out.gcprofile"], *exported) || exported->count != 2 ||
        exported->documents[0].memoryOffsetHz != 1000000000LL || exported->documents[1].memoryOffsetHz != -250000000LL)
        return 9975;
    // 9976: a probe reports an unreadable file instead of guessing.
    io.files["junk.gcprofile"] = "document.version=99\n";
    if (profile_editor_transfer_probe("junk.gcprofile", {}, io, *probe) || probe->ok || probe->error.empty()) return 9976;
    // 9977: an old Windows settings file is recognized and read by slot number.
    io.files["old.ini"] = "[profile2]\nmem_offset_mhz=700\npower_limit_pct=90\n[profile5]\nmem_offset_mhz=100\n";
    if (!profile_editor_transfer_probe("old.ini", {}, io, *probe) || !probe->legacyFile ||
        probe->legacy.format != PROFILE_LEGACY_WINDOWS_INI || probe->sources != std::vector<int>({2, 5}) ||
        probe->documents[0].memoryOffsetHz != 700000000LL)
        return 9977;
    // 9978: an unmarked Linux file asks how memory was written, never guesses.
    ProfileLegacyOptions linux{};
    linux.format = PROFILE_LEGACY_LINUX_INI;
    if (profile_editor_transfer_probe("old.ini", linux, io, *probe) || !probe->needsMemoryUnit || !probe->documents.empty())
        return 9978;
    linux.memory = PROFILE_LEGACY_MEMORY_EFFECTIVE;
    if (!profile_editor_transfer_probe("old.ini", linux, io, *probe) || probe->documents[0].memoryOffsetHz != 350000000LL)
        return 9979;
    // 9980: probe jobs run on the worker like any transfer and own their result.
    work = std::make_unique<GuiProfileTransferWork>();
    work->kind = GUI_PROFILE_TRANSFER_PROBE; work->file = "old.ini";
    gui_profile_transfer_execute(*work, io);
    if (!work->success || !work->probe || work->probe->documents.size() != 2 || work->preview) return 9980;
    // 9981: only a job that has not started can be cancelled.
    std::unique_ptr<GuiProfileTransferWork> pending = std::make_unique<GuiProfileTransferWork>();
    pending->token = 7;
    if (gui_profile_transfer_cancel_pending(pending, 8) || !gui_profile_transfer_cancel_pending(pending, 7) || pending) return 9981;

    // 9982: a capture becomes an ordinary record bound to the captured GPU.
    const auto observation = profile_capture_proposal_fixture::curve();
    auto ignored = std::make_unique<ProfileDocument>();
    auto proposals = std::make_unique<ProfileCaptureProposals>();
    std::vector<std::string> lines;
    if (profile_editor_capture_review(observation, true, PROFILE_CAPTURE_UNDECIDED, *ignored, lines, {}, proposals.get()) !=
            PROFILE_COMMAND_DECISION || !proposals->flatten.selectable())
        return 9982;
    const unsigned int all = (1u << PROFILE_CAPTURE_GROUP_COUNT) - 1;
    if (!profile_capture_record(observation, *proposals, PROFILE_CAPTURE_EXACT, all, *record, why, sizeof(why)) ||
        record->applicationScope == DESIRED_SCOPE_LEGACY || !record->authoring.topologyBound ||
        !record->curvePointCaptured[22] || record->curvePointCapturedOffsetHz[22] != -100000500LL ||
        record->authoring.voltageUv[22] != 662000 || !desired_authoring_valid(record.get()))
        return 9983;
    // 9984: the flat-curve choice saves a lock at the recognized frequency.
    if (!profile_capture_record(observation, *proposals, PROFILE_CAPTURE_FLATTEN, all, *record, why, sizeof(why)) ||
        !record->hasLock || record->lockMode != LOCK_MODE_FLATTEN || record->lockMHz != 1000 || record->lockCi < 21)
        return 9984;
    // 9985: leaving the curve out keeps only the core offset, explicitly scoped
    // (included controls only) so saving cannot complete it with live values.
    const unsigned int coreOnly = 1u << PROFILE_CAPTURE_GROUP_CORE;
    if (!profile_capture_record(observation, *proposals, PROFILE_CAPTURE_EXACT, coreOnly, *record, why, sizeof(why)) ||
        record->hasLock || record->curvePointCaptured[22] || !record->hasGpuOffset ||
        record->applicationScope == DESIRED_SCOPE_LEGACY)
        return 9985;
    // 9986: nothing included is refused with a reason.
    why[0] = 0;
    if (profile_capture_record(observation, *proposals, PROFILE_CAPTURE_EXACT, 0, *record, why, sizeof(why)) || !why[0])
        return 9986;
    // 9987: the summary describes what the GPU has in plain words.
    const auto summary = profile_capture_summary(observation, proposals.get());
    if (!summary.curve.available || summary.curve.text != "Flat at 1000 MHz from 662 mV" || !summary.core.available ||
        summary.core.text != "No offset (stock)" || summary.memory.available || summary.advancedPresent ||
        profile_capture_group_shown(summary, PROFILE_CAPTURE_GROUP_ADVANCED))
        return 9987;
    if (profile_capture_choice_label(*proposals, PROFILE_CAPTURE_FLATTEN) != "Flat curve at 1000 MHz from 662 mV" ||
        profile_capture_choice_label(*proposals, PROFILE_CAPTURE_EXACT) != "Copy every curve point exactly")
        return 9988;
    // 9989: one-line profile summaries and exact MHz formatting.
    auto summaryDoc = std::make_unique<ProfileDocument>(memory_profile(1000500000LL, "x"));
    summaryDoc->domains |= PROFILE_DOMAIN_POWER; summaryDoc->power.present = true; summaryDoc->power.ratio = {9, 10};
    if (profile_document_summary(*summaryDoc) != "Memory +1000.5 MHz, power 90 %" ||
        profile_summary_mhz(-12500000) != "-12.5" || profile_summary_mhz(0) != "0" ||
        profile_document_summary(profile_document_make_empty()) != "No settings")
        return 9989;
    // 9990: capture saves into the first empty slot, else the preferred one.
    if (profile_capture_default_slot(4, [](int s) { return s != 3; }) != 3 ||
        profile_capture_default_slot(4, [](int) { return true; }) != 4)
        return 9990;
    // 9991: a core offset that was read but whose relation to the curve is
    // unknown shows its value and why it cannot be saved, not "Not readable".
    auto unresolved = std::make_unique<HardwareCaptureObservation>(observation);
    unresolved->graphics.valid = 1; unresolved->graphics.value = 475000000LL;
    hardware_capture_domain(*unresolved, PROFILE_DOMAIN_GRAPHICS_OFFSET, CAPTURE_COMPOSITION_UNRESOLVED);
    const auto unresolvedSummary = profile_capture_summary(*unresolved, nullptr);
    if (unresolvedSummary.core.available ||
        unresolvedSummary.core.text != "+475 MHz, cannot be saved separately on this GPU") {
        fprintf(stderr, "9991: %s\n", unresolvedSummary.core.text.c_str());
        return 9991;
    }
    return 0;
}
