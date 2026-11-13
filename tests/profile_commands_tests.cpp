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
    bool legacy(const std::string& bytes, int slot, ProfileDocument& document, bool& found, ProfileCodecError& error) {
        return profile_legacy_native_slot_decode(bytes, PROFILE_LEGACY_WINDOWS_INI, slot, document, found, &error);
    }
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
        {"--export-profiles", "x", "--source-profiles", "1"},
        {"--export-profiles", "x", "--legacy-memory-unit", "effective-mhz"},
        {"--export-profiles", "x", "--legacy-format", "automatic"},
        {"--import-profiles", "x", "--slots", "1", "--source-profiles", "1,1"},
        {"--import-profiles", "x", "--slots", "1", "--legacy-format", "linux-ini", "--legacy-format", "windows-ini"},
        {"--import-profiles", "x", "--slots", "1", "--legacy-curve-semantics", "auto"},
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
    ProfileTestIO transfer;
    transfer.files["config.ini"] = original;
    transfer.files["legacy.ini"] = "[profile2]\nmem_offset_mhz=-1001\n[profile4]\nmem_offset_mhz=3\n";
    std::vector<std::string> legacyArgs = {"--import-profiles", "legacy.ini", "--legacy-format", "linux-ini",
        "--source-profiles", "4,2", "--slots", "1,5", "--confirm-import"};
    transfer.confirmation = true;
    if (!profile_commands_run(legacyArgs, "config.ini", transfer, code) || code != PROFILE_COMMAND_INVALID ||
        transfer.writes || transfer.confirmations || transfer.files["config.ini"] != original) return 9010;
    legacyArgs.insert(legacyArgs.end(), {"--legacy-memory-unit", "effective-mhz"});
    if (!profile_commands_run(legacyArgs, "config.ini", transfer, code) || code || transfer.writes != 1 ||
        transfer.files["config.ini"].find("document.memory.offset_hz=-500500000") == std::string::npos) return 9011;
    if (!profile_commands_run({"--export-profiles", "selected.gcprofile", "--slots", "5,1"}, "config.ini", transfer, code) || code ||
        !profile_bank_decode(transfer.files["selected.gcprofile"], *bank) || bank->count != 2 ||
        bank->documents[0].memoryOffsetHz != -500500000 || bank->documents[1].memoryOffsetHz != 1500000) return 9012;
    if (!profile_commands_run({"--import-profiles", "selected.gcprofile", "--source-profiles", "2", "--slots", "3",
        "--confirm-import"}, "config.ini", transfer, code) || code) return 9013;
    if (!profile_commands_run({"--export-profiles", "one.gcprofile", "--slots", "3"}, "config.ini", transfer, code) || code ||
        !profile_bank_decode(transfer.files["one.gcprofile"], *bank) ||
        bank->count != 1 || bank->documents[0].memoryOffsetHz != 1500000) return 9014;
    const auto beforeFailedSelection = transfer.files["config.ini"];
    if (!profile_commands_run({"--import-profiles", "one.gcprofile", "--source-profiles", "2", "--slots", "4",
        "--confirm-import"}, "config.ini", transfer, code) || code != PROFILE_COMMAND_INVALID ||
        transfer.files["config.ini"] != beforeFailedSelection) return 9015;
    transfer.files["local.ini"] = "[profile1]\nmem_offset_mhz=-1001\n";
    if (!profile_commands_run({"--export-profiles", "native.gcprofile", "--config", "local.ini"}, "config.ini", transfer, code) ||
        code || !profile_bank_decode(transfer.files["native.gcprofile"], *bank) ||
        bank->documents[0].memoryOffsetHz != -1001000000) return 9016;
    if (!profile_commands_run({"--export-profiles", "explicit.gcprofile", "--config", "legacy.ini",
        "--legacy-format", "linux-ini", "--legacy-memory-unit", "effective-mhz", "--slots", "2"},
        "config.ini", transfer, code) || code || !profile_bank_decode(transfer.files["explicit.gcprofile"], *bank) ||
        bank->documents[0].memoryOffsetHz != -500500000) return 9017;
    fprintf(stdout, "profile commands: explicit review, assignments, atomic import/export, source selection and legacy conversion passed\n");
    return 0;
}
#ifdef GC_PROFILE_COMMAND_FIXTURE_MAIN
int main() {
    const int code = run_profile_commands_tests();
    if (code) fprintf(stderr, "profile command regression: %d\n", code);
    return code;
}
#endif
