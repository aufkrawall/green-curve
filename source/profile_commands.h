// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_COMMANDS_H
#define GREEN_CURVE_PROFILE_COMMANDS_H
#include "profile_bank_edit.h"
#include "profile_legacy_ini.h"

enum ProfileCommandExit { PROFILE_COMMAND_OK = 0, PROFILE_COMMAND_IO = 1,
    PROFILE_COMMAND_INVALID = 2, PROFILE_COMMAND_DECISION = 3, PROFILE_COMMAND_CONFLICT = 5 };
struct ProfileCommand {
    bool importing = false, exporting = false, confirm = false, all = true;
    std::string config, file;
    int slots[PROFILE_BANK_CAPACITY]{1, 2, 3, 4, 5}, count = PROFILE_BANK_CAPACITY;
    ProfileLegacyOptions legacy;
    ProfileSourceSelection source;
};

template<class Selection>
static inline bool profile_command_slots(std::string_view value, Selection& out) {
    if (value == "all") { out.all = true; out.count = 5; for (int i = 0; i < 5; ++i) out.slots[i] = i + 1; return true; }
    bool seen[5]{};
    out.all = false; out.count = 0;
    for (;;) {
        const size_t comma = value.find(',');
        int slot = 0;
        if (out.count == 5 || !profile_codec::ordinal(value.substr(0, comma), slot, 1, 5) || seen[slot - 1]) return false;
        seen[slot - 1] = true; out.slots[out.count++] = slot;
        if (comma == std::string_view::npos) break;
        value.remove_prefix(comma + 1);
    }
    return true;
}

static inline bool profile_command_parse(const std::vector<std::string>& args, const char* defaultConfig,
    ProfileCommand& output, bool& handled, ProfileCodecError* error) {
    handled = false;
    for (const auto& arg : args) if (arg == "--export-profiles" || arg == "--import-profiles") handled = true;
    if (!handled) return true;
    ProfileCommand command;
    command.config = defaultConfig ? defaultConfig : "config.ini";
    bool haveConfig = false, haveSlots = false, haveSource = false;
    bool haveFormat = false, haveMemory = false, haveCurve = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--confirm-import") {
            if (command.confirm) return profile_codec::fail(error, 0, "duplicate import confirmation option");
            command.confirm = true; continue;
        }
        if (arg != "--export-profiles" && arg != "--import-profiles" && arg != "--config" && arg != "--slots" &&
            arg != "--source-profiles" && arg != "--legacy-format" && arg != "--legacy-memory-unit" &&
            arg != "--legacy-curve-semantics")
            return profile_codec::fail(error, 0, "profile transfer cannot be combined with other commands");
        if (++i == args.size() || args[i].empty() || args[i].size() >= 4096)
            return profile_codec::fail(error, 0, "missing or overlong profile command value");
        const auto& value = args[i];
        if (arg == "--config") {
            if (haveConfig) return profile_codec::fail(error, 0, "duplicate configuration path");
            haveConfig = true; command.config = value;
        } else if (arg == "--slots") {
            if (haveSlots || !profile_command_slots(value, command)) return profile_codec::fail(error, 0, "invalid or duplicate destination slots");
            haveSlots = true;
        } else if (arg == "--source-profiles") {
            if (haveSource || !profile_command_slots(value, command.source))
                return profile_codec::fail(error, 0, "invalid or duplicate source profiles");
            haveSource = true;
        } else if (arg == "--legacy-format") {
            if (haveFormat || (value != "windows-ini" && value != "linux-ini"))
                return profile_codec::fail(error, 0, "select one legacy source format: windows-ini or linux-ini");
            haveFormat = true;
            command.legacy.format = value == "windows-ini" ? PROFILE_LEGACY_WINDOWS_INI : PROFILE_LEGACY_LINUX_INI;
        } else if (arg == "--legacy-memory-unit") {
            if (haveMemory || (value != "display-mhz" && value != "effective-mhz"))
                return profile_codec::fail(error, 0, "select one legacy memory unit: display-mhz or effective-mhz");
            haveMemory = true;
            command.legacy.memory = value == "display-mhz" ? PROFILE_LEGACY_MEMORY_DISPLAY : PROFILE_LEGACY_MEMORY_EFFECTIVE;
        } else if (arg == "--legacy-curve-semantics") {
            if (haveCurve || (value != "absolute" && value != "base-plus-offset" && value != "absolute-with-origin"))
                return profile_codec::fail(error, 0, "select one supported legacy curve interpretation");
            haveCurve = true;
            command.legacy.curve = value == "absolute" ? PROFILE_LEGACY_CURVE_ABSOLUTE :
                value == "base-plus-offset" ? PROFILE_LEGACY_CURVE_BASE : PROFILE_LEGACY_CURVE_ORIGIN;
        } else {
            if (command.importing || command.exporting) return profile_codec::fail(error, 0, "select one profile transfer command");
            command.importing = arg == "--import-profiles"; command.exporting = !command.importing; command.file = value;
        }
    }
    if (command.importing && (!haveSlots || command.all)) return profile_codec::fail(error, 0, "import requires explicit destination slots");
    if (command.exporting && command.confirm) return profile_codec::fail(error, 0, "import confirmation cannot accompany export");
    if (haveSource && command.exporting) return profile_codec::fail(error, 0, "export source selection uses --slots");
    if ((haveMemory || haveCurve) && !haveFormat)
        return profile_codec::fail(error, 0, "legacy interpretation requires --legacy-format");
    if (command.file == command.config) return profile_codec::fail(error, 0, "profile transfer and configuration paths must differ");
    output = std::move(command);
    return true;
}

// Report assignment references without emitting unrelated preferences, rule
// patterns, machine identities or application paths into terminal output.
static inline std::string profile_command_assignments(std::string_view file, int slot) {
    using namespace profile_codec;
    std::string section, result;
    View line;
    while (profile_slot::next_line(file, line)) {
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        if (line.front() == '[' && line.back() == ']') { section = std::string(trim(line.substr(1, line.size() - 2))); continue; }
        const size_t equals = line.find('=');
        if (equals == View::npos) continue;
        const View key = trim(line.substr(0, equals)), value = trim(line.substr(equals + 1));
        int n = 0;
        const auto add = [&](const char* label) { if (!result.empty()) result += ", "; result += label; };
        if (profile_slot::ascii_equal(section, "profiles") && number(value, n, 0, 5) && n == slot) {
            if (profile_slot::ascii_equal(key, "app_launch_slot")) add("application startup");
            if (profile_slot::ascii_equal(key, "logon_slot")) add("logon");
        }
        if (profile_slot::ascii_equal(section, "hotkeys") &&
            profile_slot::ascii_equal(key, "slot" + std::to_string(slot)) && value != "0" && !value.empty()) add("hotkey");
        if ((profile_slot::ascii_equal(section, "auto_profiles") && profile_slot::ascii_equal(key, "default_slot")) ||
            (section.substr(0, 9) == "auto_rule" && profile_slot::ascii_equal(key, "slot"))) {
            if (number(value, n, 1, 5) && n == slot) add("automatic profile rule");
        }
        if (result.size() > 160) { result += ", additional references"; break; }
    }
    return result.empty() ? "none found" : result;
}

static inline std::string profile_command_domains(int mask) {
    const char* names[] = {"VF points", "graphics offset", "memory offset", "power limit", "clock lock",
        "fan", "XBAR", "system clock", "video clock"};
    std::string text;
    for (int i = 0; i < 9; ++i) if (mask & (1 << i)) { if (!text.empty()) text += ", "; text += names[i]; }
    return text.empty() ? "none" : text;
}

// IO supplies bounded file reads, atomic writes, legacy decoding, synchronized
// commit, terminal output and explicit confirmation. There is no GPU interface.
template <class IO>
static inline bool profile_commands_run(const std::vector<std::string>& args,
    const char* defaultConfig, IO& io, int& exitCode) {
    ProfileCommand command;
    ProfileCodecError error;
    bool handled = false;
    const auto reject = [&](int code) {
        io.message(std::string("Profile transfer: ") + error.message + "\n"); exitCode = code; return true;
    };
    if (!profile_command_parse(args, defaultConfig, command, handled, &error)) return reject(PROFILE_COMMAND_INVALID);
    if (!handled) return false;
    if (!io.distinct(command.config, command.file, error)) return reject(PROFILE_COMMAND_INVALID);
    std::string config;
    if (!io.read(command.config, config, profile_slot::MAX_CONFIG_BYTES, true, error)) return reject(PROFILE_COMMAND_IO);
    if (command.exporting) {
        std::string decoded;
        if (!profile_legacy::text(config, decoded, &error)) return reject(PROFILE_COMMAND_INVALID);
        config.swap(decoded);
    }
    auto bank = std::make_unique<ProfileBank>();
    if (command.exporting && command.legacy.format != PROFILE_LEGACY_NONE) {
        ProfileSourceSelection selection;
        selection.all = command.all; selection.count = command.count;
        for (int n = 0; n < selection.count; ++n) selection.slots[n] = command.slots[n];
        if (!profile_legacy_ini_decode(config, command.legacy, selection, *bank, &error)) return reject(PROFILE_COMMAND_INVALID);
    } else if (command.exporting) {
        for (int n = 0; n < command.count; ++n) {
            const int slot = command.slots[n];
            std::string body;
            bool section = false, found = false;
            ProfileDocument& document = bank->documents[bank->count];
            if (!profile_slot::extract(config, slot, body, section, &error) ||
                !profile_slot_decode(body, document, found, &error)) return reject(PROFILE_COMMAND_INVALID);
            if (!found && !io.legacy(config, slot, document, found, error)) return reject(PROFILE_COMMAND_INVALID);
            if (!found) {
                if (command.all) continue;
                profile_codec::fail(&error, 0, "selected export slot is empty"); return reject(PROFILE_COMMAND_INVALID);
            }
            document.documentId = (gc_u32)++bank->count;
        }
    }
    if (command.exporting) {
        for (int n = 0; n < bank->count; ++n) bank->documents[n].documentId = (gc_u32)n + 1;
        std::string encoded;
        if (!profile_bank_encode(*bank, encoded, &error)) return reject(PROFILE_COMMAND_INVALID);
        if (!io.write(command.file, encoded, error)) return reject(PROFILE_COMMAND_IO);
        io.message("Exported " + std::to_string(bank->count) + " profile(s).\n"); exitCode = PROFILE_COMMAND_OK; return true;
    }
    std::string input;
    if (!io.read(command.file, input, command.legacy.format == PROFILE_LEGACY_NONE ?
        PROFILE_FILE_MAX_BYTES : profile_slot::MAX_CONFIG_BYTES, false, error)) return reject(PROFILE_COMMAND_IO);
    if (command.legacy.format != PROFILE_LEGACY_NONE) {
        if (!profile_legacy_ini_decode(input, command.legacy, command.source, *bank, &error)) return reject(PROFILE_COMMAND_INVALID);
    } else {
        if (!profile_bank_decode(input, *bank, &error)) return reject(PROFILE_COMMAND_INVALID);
        if (!command.source.all) {
            auto selected = std::make_unique<ProfileBank>();
            for (int n = 0; n < command.source.count; ++n) {
                const int record = command.source.slots[n] - 1;
                if (record >= bank->count) {
                    profile_codec::fail(&error, 0, "selected source profile is absent"); return reject(PROFILE_COMMAND_INVALID);
                }
                selected->documents[selected->count++] = bank->documents[record];
            }
            bank.swap(selected);
        }
    }
    if (bank->count != command.count) {
        profile_codec::fail(&error, 0, "destination slot count must match imported profile count"); return reject(PROFILE_COMMAND_INVALID);
    }
    ProfileBankEdit edit;
    if (!profile_bank_prepare_edit(config, *bank, command.slots, edit, &error)) return reject(PROFILE_COMMAND_INVALID);
    for (int n = 0; n < bank->count; ++n) {
        const auto& d = bank->documents[n];
        const int slot = command.slots[n];
        io.message("Import record " + std::to_string(n + 1) + " -> slot " + std::to_string(slot) +
            ": name=\"" + d.displayName + "\", controls=" + profile_command_domains(d.domains) +
            ", topology points=" + std::to_string(d.topologyCount) +
            ", assignments=" + profile_command_assignments(config, slot) + "\n");
        const char* scope = d.applicationScope == PROFILE_SCOPE_UPDATE_INCLUDED ? "update included controls" :
            d.applicationScope == PROFILE_SCOPE_REPLACE_PROFILE ? "replace previously owned profile controls" : "release selected owned controls";
        io.message(std::string("  Apply scope: ") + scope + "; source values and precision are retained for destination review.\n");
        if (d.legacyIndicesUnresolved)
            io.message("  Legacy point indices/exclusions need explicit mapping review; rounded voltage labels are not exact identities.\n");
    }
    io.message("The saved records will replace these slots; existing assignments will use their new contents. Hardware remains unchanged.\n");
    if (!command.confirm || !io.confirm()) {
        io.message("Import awaits confirmation. Use --confirm-import and type IMPORT after reviewing the summary.\n");
        exitCode = PROFILE_COMMAND_DECISION; return true;
    }
    if (!io.commit(command.config, edit, error)) return reject(PROFILE_COMMAND_CONFLICT);
    io.message("Imported " + std::to_string(bank->count) + " profile(s).\n"); exitCode = PROFILE_COMMAND_OK; return true;
}
#endif
