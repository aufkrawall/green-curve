// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "profile_codec_fixture.h"
#include "profile_commands.h"
#include "profile_legacy_document.h"
#include <map>

struct ProfileTestIO {
    std::map<std::string, std::string> files;
    std::string messages;
    bool confirmation = false, changeWhileReviewing = false, failWrite = false;
    int writes = 0, confirmations = 0;
    bool distinct(const std::string& a, const std::string& b, ProfileCodecError&) { return a != b; }
    bool read(const std::string& path, std::string& out, size_t limit, bool missing, ProfileCodecError& error) {
        const auto it = files.find(path);
        if (it == files.end()) { if (missing) { out.clear(); return true; } return profile_codec::fail(&error, 0, "missing test input"); }
        if (it->second.size() > limit) return profile_codec::fail(&error, 0, "oversized test input");
        out = it->second; return true;
    }
    bool write(const std::string& path, const std::string& bytes, ProfileCodecError&) {
        ++writes;
        if (failWrite) return false;
        files[path] = bytes; return true;
    }
    bool legacy(const std::string&, int, ProfileDocument&, bool& found, ProfileCodecError&) { found = false; return true; }
    void message(const std::string& text) { messages += text; }
    bool confirm() {
        ++confirmations;
        if (changeWhileReviewing) files["config.ini"] += "[concurrent]\nchanged=1\n";
        return confirmation;
    }
    bool commit(const std::string& path, const ProfileBankEdit& edit, ProfileCodecError& error) {
        return profile_bank_commit_edit(edit,
            [&](std::string& out) { return read(path, out, profile_slot::MAX_CONFIG_BYTES, true, error); },
            [&](const std::string& bytes) { return write(path, bytes, error); }, &error);
    }
};

int run_profile_commands_tests() {
    auto bank = std::make_unique<ProfileBank>();
    bank->count = 1; bank->documents[0] = profile_codec_fixture();
    std::string input;
    if (!profile_bank_encode(*bank, input)) return 8400;
    ProfileTestIO io;
    const std::string original = "[profiles]\nlogon_slot=3\n[hotkeys]\nslot3=123\n[auto_rule1]\nslot=3\npattern=opaque\n";
    io.files["config.ini"] = original; io.files["input.gcprofile"] = input;
    const std::vector<std::string> import = {"--import-profiles", "input.gcprofile", "--slots", "3"};
    int code = -1;
    if (!profile_commands_run(import, "config.ini", io, code) || code != PROFILE_COMMAND_DECISION ||
        io.writes || io.confirmations || io.files["config.ini"] != original ||
        io.messages.find("logon, hotkey, automatic profile rule") == std::string::npos ||
        io.messages.find("pattern=opaque") != std::string::npos) return 8401;
    std::vector<std::string> confirmed = import; confirmed.push_back("--confirm-import");
    if (!profile_commands_run(confirmed, "config.ini", io, code) || code != PROFILE_COMMAND_DECISION || io.writes) return 8402;
    io.confirmation = true; io.changeWhileReviewing = true;
    if (!profile_commands_run(confirmed, "config.ini", io, code) || code != PROFILE_COMMAND_CONFLICT ||
        io.writes || io.files["config.ini"].find("[concurrent]") == std::string::npos) return 8403;
    io.changeWhileReviewing = false; io.files["config.ini"] = original; io.failWrite = true;
    if (!profile_commands_run(confirmed, "config.ini", io, code) || code != PROFILE_COMMAND_CONFLICT ||
        io.writes != 1 || io.files["config.ini"] != original) return 8404;
    io.failWrite = false; io.writes = 0;
    if (!profile_commands_run(confirmed, "config.ini", io, code) || code || io.writes != 1) return 8405;
    if (!profile_commands_run({"--export-profiles", "output.gcprofile", "--slots", "3"}, "config.ini", io, code) ||
        code || io.files["output.gcprofile"] != input) return 8406;
    if (!profile_commands_run({"--export-profiles", "bank.gcprofile"}, "config.ini", io, code) || code ||
        io.files["bank.gcprofile"] != input) return 8407;
    const int before = io.writes;
    const std::vector<std::vector<std::string>> invalid = {
        {"--import-profiles", "input.gcprofile"}, {"--import-profiles", "input.gcprofile", "--slots", "all"},
        {"--import-profiles", "input.gcprofile", "--slots", "1,1"},
        {"--import-profiles", "input.gcprofile", "--slots", "6"},
        {"--import-profiles", "input.gcprofile", "--slots", "01"},
        {"--export-profiles", "config.ini"}, {"--export-profiles", "x", "--apply-config"},
        {"--export-profiles", "x", "--confirm-import"},
        {"--export-profiles", "x", "--import-profiles", "input.gcprofile"},
        {"--export-profiles", "x", "--config", "a", "--config", "b"},
        {"--import-profiles", "input.gcprofile", "--slots", "1,2"},
    };
    for (const auto& args : invalid)
        if (!profile_commands_run(args, "config.ini", io, code) || code != PROFILE_COMMAND_INVALID || io.writes != before) return 8408;
    if (profile_commands_run({"--json"}, "config.ini", io, code)) return 8409;
    DesiredSettings desired{};
    initialize_desired_settings_defaults(&desired);
    desired.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED;
    desired.hasGpuOffset = true; desired.gpuOffsetMHz = -25; desired.gpuOffsetExcludeLowCount = 7;
    desired.hasLock = true; desired.lockCi = 22; desired.lockMHz = 1000; desired.lockMode = LOCK_MODE_FLATTEN;
    desired.curvePointCaptured[3] = 1; desired.curvePointCapturedOffsetHz[3] = 0;
    if (!desired_memory_set_precise(&desired, -500000, CLOCK_OFFSET_SURFACE_P0) ||
        !desired_power_set_ratio(&desired, {249, 250})) return 8410;
    ProfileDocument legacy{}; char why[160]{};
    if (!profile_document_from_legacy_desired(desired, legacy, why, sizeof(why)) ||
        !legacy.legacyIndicesUnresolved || legacy.sourceTopologyComplete || legacy.topologyCount != 2 ||
        legacy.topology[0].voltageUv || legacy.topology[1].sourceIndex != 22 ||
        legacy.points[0].kind != PROFILE_POINT_CAPTURED_OFFSET || legacy.points[0].capturedOffsetHz ||
        legacy.memoryOffsetHz != -500000 || legacy.power.ratio.numerator != 249 ||
        legacy.graphicsOffset.exclusionPolicy != PROFILE_EXCLUDE_LEGACY_COUNT) return 8411;
    fprintf(stdout, "profile commands: explicit review, assignments, atomic import/export and legacy evidence checks passed\n");
    return 0;
}
