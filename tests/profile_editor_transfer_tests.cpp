// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "profile_editor_transfer.h"
#include <map>

namespace {
struct IO {
    std::map<std::string, std::string> files;
    int sourceReads = 0, configReads = 0, writes = 0, commits = 0;
    std::string replaceSourceDuringConfigRead;
    bool failWrite = false;
    bool read(const std::string& path, std::string& out, size_t maximum, bool missing, ProfileCodecError& error) {
        if (path == "bank.ini") {
            ++configReads;
            if (!replaceSourceDuringConfigRead.empty()) { files["input.gcprofile"] = replaceSourceDuringConfigRead; replaceSourceDuringConfigRead.clear(); }
        } else ++sourceReads;
        const auto it = files.find(path);
        if (it == files.end()) {
            if (missing) { out.clear(); return true; }
            return profile_codec::fail(&error, 0, "fixture input missing");
        }
        if (it->second.size() > maximum) return profile_codec::fail(&error, 0, "fixture input too large");
        out = it->second; return true;
    }
    bool write(const std::string& path, const std::string& bytes, ProfileCodecError& error) {
        if (failWrite) return profile_codec::fail(&error, 0, "fixture write refused");
        ++writes; files[path] = bytes; return true;
    }
    bool distinct(const std::string& config, const std::string& file, ProfileCodecError& error) {
        return config != file || profile_codec::fail(&error, 0, "fixture paths must differ");
    }
    bool legacy(const std::string& text, int slot, ProfileDocument& document, bool& found, ProfileCodecError& error) {
        return profile_legacy_native_slot_decode(text, PROFILE_LEGACY_WINDOWS_INI, slot, document, found, &error);
    }
    bool commit(const std::string& path, const ProfileBankEdit& edit, ProfileCodecError& error) {
        ++commits;
        return profile_bank_commit_edit(edit, [&](std::string& current) { current = files[path]; return true; },
            [&](const std::string& replacement) { return write(path, replacement, error); }, &error);
    }
};
}
int run_profile_editor_transfer_tests() {
    IO io; ProfileCodecError error;
    io.files["bank.ini"] = "[profiles]\napp_launch_slot=5\nlogon_slot=2\n[preferences]\nkept=yes\n";
    auto bank = std::make_unique<ProfileBank>(); bank->count = 3;
    for (int i = 0; i < 3; ++i) {
        auto& d = bank->documents[i]; d = profile_document_make_empty(); d.documentId = i + 1;
        snprintf(d.displayName, sizeof(d.displayName), "Source %d", i + 1);
        d.domains = PROFILE_DOMAIN_MEMORY; d.hasMemoryOffset = true;
        d.memoryOffsetHz = i == 1 ? -123500000 : 0; d.memorySurface = CLOCK_OFFSET_SURFACE_GLOBAL;
    }
    bank->documents[2].domains = PROFILE_DOMAIN_POWER; bank->documents[2].hasMemoryOffset = false;
    bank->documents[2].memoryOffsetHz = 0; bank->documents[2].memorySurface = 0;
    bank->documents[2].power.present = true; bank->documents[2].power.ratio = {2, 3};
    if (!profile_bank_encode(*bank, io.files["input.gcprofile"], &error) ||
        !profile_document_slot_save(io, "bank.ini", 1, bank->documents[0], error) ||
        !profile_document_slot_save(io, "bank.ini", 3, bank->documents[2], error)) return 9440;
    io.sourceReads = io.configReads = io.writes = io.commits = 0;
    const auto originalConfig = io.files["bank.ini"];
    ProfileEditorTransferOptions options; options.importing = true; options.slot = 5; options.sourceRecord = 2;
    auto preview = std::make_unique<ProfileEditorTransferPreview>();
    if (!profile_editor_transfer_prepare(options, "input.gcprofile", "bank.ini", io, *preview) || !preview->ready ||
        preview->documents.size() != 1 || preview->documents[0].memoryOffsetHz != -123500000 || preview->edit.destinations[0] != 5 ||
        io.sourceReads != 1 || io.configReads != 1 || io.writes || io.commits || io.files["bank.ini"] != originalConfig) return 9441;
    std::string report; for (const auto& line : preview->lines) report += line + '\n';
    if (report.find("application startup") == std::string::npos || report.find("Import preview ready") == std::string::npos ||
        report.find("Imported ") != std::string::npos) return 9442;
    io.files["input.gcprofile"] = "changed after review";
    if (!profile_editor_transfer_commit(io, *preview) || preview->ready || !preview->committed || io.sourceReads != 1 ||
        io.writes != 1 || io.commits != 1 || profile_editor_transfer_commit(io, *preview) || io.writes != 1) return 9443;
    auto saved = std::make_unique<ProfileDocument>(); bool found = false;
    if (!profile_document_slot_load(io, "bank.ini", 5, *saved, found, error) || !found ||
        !profile_document_semantically_equal(*saved, preview->documents[0]) || saved->memoryOffsetHz != -123500000 ||
        io.files["bank.ini"].find("kept=yes") == std::string::npos) return 9444;
    if (!profile_bank_encode(*bank, io.files["input.gcprofile"], &error) ||
        !profile_editor_transfer_prepare(options, "input.gcprofile", "bank.ini", io, *preview)) return 9445;
    const auto commits = io.commits, writes = io.writes;
    io.files["bank.ini"] += "\n[concurrent]\nkept=1\n";
    const auto concurrent = io.files["bank.ini"];
    if (profile_editor_transfer_commit(io, *preview) || preview->ready || preview->committed ||
        preview->result != PROFILE_COMMAND_CONFLICT || io.writes != writes || io.commits != commits + 1 ||
        io.files["bank.ini"] != concurrent || !strstr(preview->error.message, "bank changed")) return 9446;
    options.importing = false; options.slot = 5;
    if (!profile_editor_transfer_prepare(options, "output.gcprofile", "bank.ini", io, *preview) ||
        preview->documents.size() != 1 || preview->documents[0].memoryOffsetHz != -123500000 || io.writes != writes) return 9447;
    const auto frozenExport = preview->exportBytes;
    io.files["bank.ini"] += "\n[after_preview]\nkept=1\n";
    if (!profile_editor_transfer_commit(io, *preview) || io.files["output.gcprofile"] != frozenExport || io.writes != writes + 1) return 9448;
    options.importing = options.all = true;
    auto changed = std::make_unique<ProfileBank>(); changed->count = 1; changed->documents[0] = bank->documents[0];
    if (!profile_bank_encode(*changed, io.replaceSourceDuringConfigRead, &error)) return 9449;
    const int sourceReads = io.sourceReads;
    if (!profile_editor_transfer_prepare(options, "input.gcprofile", "bank.ini", io, *preview) ||
        preview->documents.size() != 3 || preview->command.count != 3 || preview->edit.destinations[2] != 3 ||
        preview->documents[1].memoryOffsetHz != -123500000 || io.sourceReads != sourceReads + 1) return 9450;
    // Malformed choices clear the previous ready state before any storage I/O.
    const int reads = io.sourceReads + io.configReads;
    options.sourceRecord = 0;
    if (profile_editor_transfer_prepare(options, "input.gcprofile", "bank.ini", io, *preview) || preview->ready ||
        !preview->documents.empty() || io.sourceReads + io.configReads != reads) return 9451;
    options.sourceRecord = 1;
    for (const auto& path : {std::string{}, std::string("bad\0path", 8), std::string(4096, 'x'), std::string("\x9b", 1)})
        if (profile_editor_transfer_prepare(options, path, "bank.ini", io, *preview) || preview->ready ||
            io.sourceReads + io.configReads != reads) return 9452;
    options.all = false; options.legacy = {PROFILE_LEGACY_WINDOWS_INI}; options.slot = 2;
    io.files["legacy.ini"] = "[meta]\nformat_version=2\n[profile1]\nmem_offset_mhz=-1001\n";
    if (!profile_editor_transfer_prepare(options, "legacy.ini", "bank.ini", io, *preview) ||
        preview->documents[0].memoryOffsetHz != -1001000000 || preview->edit.destinations[0] != 2) return 9453;
    options.legacy = {}; options.legacy.memory = PROFILE_LEGACY_MEMORY_EFFECTIVE;
    if (profile_editor_transfer_prepare(options, "legacy.ini", "bank.ini", io, *preview) || !preview->error.message[0]) return 9454;
    options.legacy = {}; io.files["broken.gcprofile"] = "not a profile";
    if (profile_editor_transfer_prepare(options, "broken.gcprofile", "bank.ini", io, *preview) || !preview->error.message[0] || preview->ready) return 9455;
    options.importing = false; options.slot = 5;
    if (!profile_editor_transfer_prepare(options, "refused.gcprofile", "bank.ini", io, *preview)) return 9456;
    io.failWrite = true;
    if (profile_editor_transfer_commit(io, *preview) || preview->ready || preview->committed || preview->result != PROFILE_COMMAND_IO ||
        strcmp(preview->error.message, "fixture write refused")) return 9457;
    auto preserved = std::make_unique<ProfileBank>(*bank); ProfileSourceSelection selection;
    selection.all = false; selection.count = 1; selection.slots[0] = 0;
    std::string canonical; if (!profile_bank_encode(*bank, canonical, &error)) return 9458;
    if (profile_command_decode_import(canonical, {}, selection, *preserved, error) || preserved->count != 3 ||
        preserved->documents[1].memoryOffsetHz != -123500000) return 9459;
    return 0;
}
