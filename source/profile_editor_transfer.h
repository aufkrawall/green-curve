// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_commands.h"
#include "profile_editor_report.h"

struct ProfileEditorTransferOptions {
    bool importing = false, all = false;
    int slot = 1, sourceRecord = 1;
    ProfileLegacyOptions legacy{};
};
struct ProfileEditorTransferPreview {
    ProfileCommand command{};
    ProfileBankEdit edit{};
    std::string exportBytes;
    std::vector<ProfileDocument> documents;
    std::vector<std::string> lines;
    ProfileCodecError error{};
    int result = PROFILE_COMMAND_INVALID;
    bool ready = false, committed = false;
};
template<class IO> struct ProfileEditorTransferIO {
    IO& storage;
    ProfileEditorTransferPreview& preview;
    const std::string& sourceBytes;
    bool truncated = false, recorded = false;
    bool read(const std::string& path, std::string& bytes, size_t maximum, bool missing, ProfileCodecError& error) {
        if (preview.command.importing && path == preview.command.file) {
            if (sourceBytes.size() > maximum) return profile_codec::fail(&error, 0, "source file exceeds preview limit");
            bytes = sourceBytes; return true;
        }
        return storage.read(path, bytes, maximum, missing, error);
    }
    bool distinct(const std::string& config, const std::string& file, ProfileCodecError& error) { return storage.distinct(config, file, error); }
    bool legacy(const std::string& bytes, int slot, ProfileDocument& document, bool& found, ProfileCodecError& error) {
        return storage.legacy(bytes, slot, document, found, error);
    }
    // These are virtual sinks, not authorization or external mutation. The
    // underlying storage has no write/commit call anywhere in preparation.
    bool confirm() { return true; }
    bool write(const std::string& path, const std::string& bytes, ProfileCodecError& error) {
        if (!preview.command.exporting || path != preview.command.file)
            return profile_codec::fail(&error, 0, "unexpected export preview destination");
        auto bank = std::make_unique<ProfileBank>();
        if (!profile_bank_decode(bytes, *bank, &error) || !bank->count) return profile_codec::fail(&error, 0, "no saved profiles selected");
        preview.documents.assign(bank->documents, bank->documents + bank->count);
        for (int i = 0; i < bank->count; ++i) message("Export record " + std::to_string(i + 1) + ": name=\"" +
            bank->documents[i].displayName + "\", controls=" + profile_command_domains(bank->documents[i].domains) + "\n");
        preview.exportBytes = bytes; recorded = true; return true;
    }
    bool commit(const std::string& path, const ProfileBankEdit& edit, ProfileCodecError& error) {
        if (!preview.command.importing || path != preview.command.config)
            return profile_codec::fail(&error, 0, "unexpected import preview destination");
        std::string text;
        if (!profile_config::decode(edit.replacement, text, &error)) return false;
        std::vector<ProfileDocument> documents;
        auto document = std::make_unique<ProfileDocument>();
        for (int i = 0; i < edit.count; ++i) {
            bool found = false;
            if (!profile_document_slot_decode(text, edit.destinations[i], *document, found, error) || !found)
                return profile_codec::fail(&error, 0, "prepared import is missing its canonical document");
            documents.push_back(*document);
        }
        preview.documents = std::move(documents); preview.edit = edit; recorded = true; return true;
    }
    void message(const std::string& text) {
        const char* presentation = text.rfind("Imported ", 0) == 0 ? "Import preview ready; the bank is unchanged.\n" :
            text.rfind("Exported ", 0) == 0 ? "Export preview ready; no file has been written.\n" : nullptr;
        profile_editor_report_append(preview.lines, truncated, presentation ? presentation : text, "Additional transfer details omitted");
    }
};
// Bank imports explicitly map records to slots 1..N. Single-record imports
// select a file ordinal and one destination slot. All inputs are frozen before
// a frontend offers Commit; changing UI choices must discard this preview.
template<class IO> static inline bool profile_editor_transfer_prepare(const ProfileEditorTransferOptions& options,
    const std::string& file, const std::string& config, IO& storage, ProfileEditorTransferPreview& preview) {
    preview = {};
    const auto fail = [&](std::string message, int result = PROFILE_COMMAND_INVALID) {
        if (message.empty()) message = "could not prepare the profile transfer";
        profile_codec::fail(&preview.error, preview.error.line, message.c_str()); preview.result = result;
        preview.lines.emplace_back(std::string("Profile transfer: ") + message); return false;
    };
    if (options.slot < 1 || options.slot > 5 || options.sourceRecord < 1 || options.sourceRecord > 5 ||
        file.empty() || file.size() >= 4096 || !profile_utf8_valid(file.data(), file.size())) return fail("invalid profile path or record/slot selection");
    std::vector<std::string> args{options.importing ? "--import-profiles" : "--export-profiles", file, "--slots",
        options.all ? (options.importing ? "1,2,3,4,5" : "all") : std::to_string(options.slot)};
    if (options.importing) {
        args.emplace_back("--confirm-import");
        if (!options.all) { args.emplace_back("--source-profiles"); args.emplace_back(std::to_string(options.sourceRecord)); }
    }
    const auto& legacy = options.legacy;
    if (legacy.format != PROFILE_LEGACY_NONE) {
        args.emplace_back("--legacy-format"); args.emplace_back(legacy.format == PROFILE_LEGACY_WINDOWS_INI ? "windows-ini" :
            legacy.format == PROFILE_LEGACY_LINUX_INI ? "linux-ini" : "invalid");
    }
    if (legacy.memory != PROFILE_LEGACY_MEMORY_UNKNOWN) {
        args.emplace_back("--legacy-memory-unit"); args.emplace_back(legacy.memory == PROFILE_LEGACY_MEMORY_DISPLAY ? "display-mhz" :
            legacy.memory == PROFILE_LEGACY_MEMORY_EFFECTIVE ? "effective-mhz" : "invalid");
    }
    if (legacy.curve != PROFILE_LEGACY_CURVE_UNKNOWN) {
        args.emplace_back("--legacy-curve-semantics"); args.emplace_back(legacy.curve == PROFILE_LEGACY_CURVE_ABSOLUTE ? "absolute" :
            legacy.curve == PROFILE_LEGACY_CURVE_ORIGIN ? "absolute-with-origin" : legacy.curve == PROFILE_LEGACY_CURVE_BASE ? "base-plus-offset" : "invalid");
    }
    bool handled = false;
    if (!profile_command_parse(args, config.c_str(), preview.command, handled, &preview.error)) return fail(preview.error.message);
    std::string sourceBytes;
    if (options.importing) {
        if (!storage.distinct(config, file, preview.error)) return fail(preview.error.message);
        if (!storage.read(file, sourceBytes, legacy.format == PROFILE_LEGACY_NONE ? PROFILE_FILE_MAX_BYTES :
            profile_slot::MAX_CONFIG_BYTES, false, preview.error)) return fail(preview.error.message, PROFILE_COMMAND_IO);
        auto bank = std::make_unique<ProfileBank>();
        if (!profile_command_decode_import(sourceBytes, legacy, preview.command.source, *bank, preview.error)) return fail(preview.error.message);
        if (!bank->count) return fail("no source profiles selected");
        if (options.all) {
            std::string slots;
            for (int i = 0; i < bank->count; ++i) { if (i) slots += ','; slots += std::to_string(i + 1); }
            args[3] = slots;
            if (!profile_command_parse(args, config.c_str(), preview.command, handled, &preview.error)) return fail(preview.error.message);
        }
    }
    ProfileEditorTransferIO<IO> io{storage, preview, sourceBytes};
    (void)profile_commands_run(args, config.c_str(), io, preview.result, &preview.error);
    preview.ready = preview.result == PROFILE_COMMAND_OK && io.recorded && !preview.documents.empty();
    return preview.ready;
}
template<class IO> static inline bool profile_editor_transfer_commit(IO& storage, ProfileEditorTransferPreview& preview) {
    if (!preview.ready || preview.committed) {
        preview.result = PROFILE_COMMAND_DECISION;
        return profile_codec::fail(&preview.error, 0, "review a current transfer before committing");
    }
    preview.ready = false;
    preview.error = {};
    preview.committed = preview.command.importing ? storage.commit(preview.command.config, preview.edit, preview.error) :
        storage.write(preview.command.file, preview.exportBytes, preview.error);
    preview.result = preview.committed ? PROFILE_COMMAND_OK : preview.command.importing ? PROFILE_COMMAND_CONFLICT : PROFILE_COMMAND_IO;
    return preview.committed;
}
