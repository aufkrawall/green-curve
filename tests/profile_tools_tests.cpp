// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// The profile tool dialogs (import current GPU settings, import/export files)
// sit on these shared engines; the Windows dialogs and the TUI only present
// them. Cases 9970-9998.
#include "profile_editor_capture.h"
#include "profile_capture_save.h"
#include "profile_capture_proposal_fixture.h"
#include "gui_profile_transfer_work.h"
#include "profile_transfer_slot_policy.h"
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
    work->reviewedSource = probe->sourceBytes;
    io.files.erase("three.gcprofile");
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

    // 9992: importing a 5-profile export when only slot 5 is empty must keep
    // each profile in its matching slot (1..5), never swap profile 1 into slot 5
    // and profile 5 into slot 1.
    {
        const int five[5] = {1, 2, 3, 4, 5};
        const bool onlySlot5Empty[6] = {false, true, true, true, true, false};
        int assigned[5]{};
        profile_transfer_default_slots(5, five, onlySlot5Empty, assigned);
        for (int i = 0; i < 5; ++i) if (assigned[i] != i + 1) return 9992;
    }
    // 9993: for all 32 occupancy masks, a 5-profile import keeps slots 1..5.
    {
        const int five[5] = {1, 2, 3, 4, 5};
        for (int mask = 0; mask < 32; ++mask) {
            bool saved[6]{};
            for (int s = 1; s <= 5; ++s) saved[s] = ((mask >> (s - 1)) & 1) != 0;
            int assigned[5]{};
            profile_transfer_default_slots(5, five, saved, assigned);
            for (int i = 0; i < 5; ++i) if (assigned[i] != i + 1) return 9993;
        }
    }
    // 9994: partial imports avoid overwriting saved slots when empty slots
    // suffice, preserve preferred slots when possible, and stay monotonic.
    {
        int assigned[5]{};
        const bool saved14[6] = {false, true, false, false, true, false};
        const int p12[2] = {1, 2};
        profile_transfer_default_slots(2, p12, saved14, assigned);
        if (assigned[0] != 2 || assigned[1] != 3) return 9994;

        const int p25[2] = {2, 5};
        profile_transfer_default_slots(2, p25, saved14, assigned);
        if (assigned[0] != 2 || assigned[1] != 5) return 9994;

        const bool saved1234[6] = {false, true, true, true, true, false};
        const int p1[1] = {1};
        profile_transfer_default_slots(1, p1, saved1234, assigned);
        if (assigned[0] != 5) return 9994;

        const int p1234[4] = {1, 2, 3, 4};
        profile_transfer_default_slots(4, p1234, saved1234, assigned);
        if (assigned[0] != 1 || assigned[1] != 2 || assigned[2] != 3 || assigned[3] != 5) return 9994;

        const int p135[3] = {1, 3, 5};
        profile_transfer_default_slots(3, p135, saved1234, assigned);
        if (assigned[0] != 1 || assigned[1] != 3 || assigned[2] != 5) return 9994;

        const bool saved123[6] = {false, true, true, true, false, false};
        const int p123[3] = {1, 2, 3};
        profile_transfer_default_slots(3, p123, saved123, assigned);
        if (assigned[0] != 1 || assigned[1] != 4 || assigned[2] != 5) return 9994;

        const bool saved2345[6] = {false, false, true, true, true, true};
        const int p2345[4] = {2, 3, 4, 5};
        profile_transfer_default_slots(4, p2345, saved2345, assigned);
        if (assigned[0] != 1 || assigned[1] != 3 || assigned[2] != 4 || assigned[3] != 5) return 9994;

        const bool allSaved[6] = {false, true, true, true, true, true};
        profile_transfer_default_slots(2, p12, allSaved, assigned);
        if (assigned[0] != 1 || assigned[1] != 2) return 9994;

        const int dupes[3] = {2, 2, 99};
        profile_transfer_default_slots(3, dupes, saved1234, assigned);
        bool seen[6]{};
        for (int i = 0; i < 3; ++i) {
            if (assigned[i] < 1 || assigned[i] > 5 || seen[assigned[i]]) return 9994;
            seen[assigned[i]] = true;
        }
    }
    // 9995: end-to-end 5-slot round trip where slot 1 holds an out-of-range
    // legacy graphics offset (-1927 MHz) and slot 5 holds a voltage-bound
    // captured flatten: deleting slot 5 and re-importing the 5-profile export
    // using default slots restores slot 5 cleanly without swapping slot 1 in.
    {
        ToolsIO roundtrip;
        auto fiveBank = std::make_unique<ProfileBank>();
        fiveBank->count = 5;
        fiveBank->documents[0] = memory_profile(3000000000LL, "Slot 1");
        fiveBank->documents[0].domains |= PROFILE_DOMAIN_GRAPHICS_OFFSET;
        fiveBank->documents[0].graphicsOffset.present = true;
        fiveBank->documents[0].graphicsOffset.offsetHz = -1927000000LL;
        fiveBank->documents[0].graphicsOffset.exclusionPolicy = PROFILE_EXCLUDE_LEGACY_COUNT;
        fiveBank->documents[0].graphicsOffset.excludeLowCount = 76;
        fiveBank->documents[0].legacyIndicesUnresolved = true;
        for (int i = 1; i <= 3; ++i)
            fiveBank->documents[i] = memory_profile(500000000LL * i, "Middle");
        auto captured = profile_document_make_empty();
        snprintf(captured.displayName, sizeof(captured.displayName), "Captured 5");
        captured.applicationScope = PROFILE_SCOPE_UPDATE_INCLUDED;
        captured.pointSetPolicy = PROFILE_POINTS_REPLACE;
        captured.domains = PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_LOCK | PROFILE_DOMAIN_MEMORY | PROFILE_DOMAIN_POWER;
        captured.topologyCount = 2;
        captured.topology[0] = {75, 75, 920000LL, 1};
        captured.points[0].kind = PROFILE_POINT_CAPTURED_OFFSET;
        captured.points[0].capturedOffsetHz = 0;
        captured.topology[1] = {76, 76, 925000LL, 1};
        captured.lock.present = true;
        captured.lock.mode = LOCK_MODE_FLATTEN;
        captured.lock.anchorTopologyId = 76;
        captured.lock.targetFrequencyHz = 2962000000LL;
        captured.lock.trackAnchor = false;
        captured.hasMemoryOffset = true;
        captured.memoryOffsetHz = 3000000000LL;
        captured.memorySurface = CLOCK_OFFSET_SURFACE_P0;
        captured.power.present = true;
        captured.power.ratio = {17, 20};
        fiveBank->documents[4] = captured;
        for (int i = 0; i < 5; ++i) fiveBank->documents[i].documentId = (gc_u32)(i + 1);
        if (!profile_bank_encode(*fiveBank, roundtrip.files["all5.gcprofile"], &error)) return 9995;
        roundtrip.files["bank.ini"] =
            "[profile1]\nmem_offset_mhz=100\n[profile2]\nmem_offset_mhz=200\n"
            "[profile3]\nmem_offset_mhz=300\n[profile4]\nmem_offset_mhz=400\n";
        auto probe5 = std::make_unique<ProfileEditorTransferProbe>();
        if (!profile_editor_transfer_probe("all5.gcprofile", {}, roundtrip, *probe5) || probe5->documents.size() != 5)
            return 9995;
        const bool saved1234[6] = {false, true, true, true, true, false};
        int assigned[5]{};
        profile_transfer_default_slots(5, probe5->sources.data(), saved1234, assigned);
        auto runWork = std::make_unique<GuiProfileTransferWork>();
        runWork->kind = GUI_PROFILE_TRANSFER_RUN;
        runWork->options.importing = true;
        runWork->file = "all5.gcprofile";
        runWork->config = "bank.ini";
        for (int i = 0; i < 5; ++i) {
            runWork->options.sources.push_back(probe5->sources[(size_t)i]);
            runWork->options.slots.push_back(assigned[i]);
        }
        runWork->reviewedSource = probe5->sourceBytes;
        gui_profile_transfer_execute(*runWork, roundtrip);
        if (!runWork->success) return 9995;
        std::string bankText;
        auto slot5Doc = std::make_unique<ProfileDocument>();
        auto slot1Doc = std::make_unique<ProfileDocument>();
        bool found5 = false, found1 = false;
        if (!profile_config::decode(roundtrip.files["bank.ini"], bankText, &error) ||
            !profile_document_slot_decode(bankText, 5, *slot5Doc, found5, error) || !found5 ||
            !profile_document_slot_decode(bankText, 1, *slot1Doc, found1, error) || !found1)
            return 9995;
        ProfileDestinationTopology live{};
        live.available = true; live.complete = true; live.count = 2;
        live.points[0] = {75, 920000LL, true, true};
        live.points[1] = {76, 925000LL, true, true};
        auto slot5Runtime = std::make_unique<DesiredSettings>();
        auto slot1Runtime = std::make_unique<DesiredSettings>();
        why[0] = 0;
        if (!profile_slot_document_runtime(*slot5Doc, &live, *slot5Runtime, why, sizeof(why)) ||
            !slot5Runtime->hasLock || slot5Runtime->lockCi != 76 || slot5Runtime->lockMHz != 2962 ||
            slot5Runtime->memOffsetHz != 3000000000LL || slot5Runtime->powerRatioNumerator != 17 ||
            slot5Runtime->powerRatioDenominator != 20)
            return 9996;
        why[0] = 0;
        if (profile_slot_document_runtime(*slot1Doc, nullptr, *slot1Runtime, why, sizeof(why)) ||
            !strstr(why, "-1927 MHz") || !strstr(why, "-1000..1000 MHz"))
            return 9996;
    }
    // 12664-12667: Linux -> Windows profile interchange:
    // - raw Linux config.ini auto-detects as PROFILE_LEGACY_LINUX_INI in
    //   profile_editor_transfer_probe() (both migrated and pre-migration),
    // - unlocked Linux slots with leftover lock_tracks_anchor=1 decode cleanly,
    // - Linux-exported portable documents (indexed & voltage-bound, os=linux)
    //   import and resolve through profile_slot_document_runtime() cleanly.
    {
        ToolsIO linuxToWin;
        linuxToWin.files["linux-migrated.ini"] =
            "[meta]\nlinux_mem_migrated=1\nformat_version=2\n"
            "[profile1]\ngpu_offset_mhz=150\nmem_offset_mhz=500\npower_limit_pct=100\n"
            "lock_ci=-1\nlock_mhz=0\nlock_mode=0\nlock_tracks_anchor=1\n"
            "[profile1_curve]\ncurve_semantics=absolute_with_origin\n"
            "[profile2]\ngpu_offset_mhz=475\ngpu_offset_exclude_low_count=70\n"
            "lock_ci=76\nlock_mhz=2957\nlock_mode=1\nlock_tracks_anchor=0\n"
            "mem_offset_mhz=3000\npower_limit_pct=85\n"
            "[profile2_curve]\ncurve_semantics=absolute_with_origin\n"
            "point70=0\npoint70_from_gpu_offset=1\npoint76=2957\n";
        auto linuxProbe = std::make_unique<ProfileEditorTransferProbe>();
        if (!profile_editor_transfer_probe("linux-migrated.ini", {}, linuxToWin, *linuxProbe) ||
            !linuxProbe->legacyFile || linuxProbe->legacy.format != PROFILE_LEGACY_LINUX_INI ||
            linuxProbe->documents.size() != 2 || linuxProbe->documents[0].lock.present ||
            linuxProbe->documents[0].source.osFamily != PROFILE_SOURCE_OS_LINUX ||
            !linuxProbe->documents[1].lock.present ||
            linuxProbe->documents[1].lock.targetFrequencyHz != 2957000000LL)
            return 12664;

        linuxToWin.files["linux-pre-migration.ini"] =
            "[profile1]\ngpu_offset_mhz=100\nmem_offset_mhz=1000\n"
            "[profile1_curve]\ncurve_semantics=absolute_with_origin\npoint76=2900\n";
        auto preMigProbe = std::make_unique<ProfileEditorTransferProbe>();
        if (profile_editor_transfer_probe("linux-pre-migration.ini", {}, linuxToWin, *preMigProbe) ||
            !preMigProbe->legacyFile || preMigProbe->legacy.format != PROFILE_LEGACY_LINUX_INI ||
            !preMigProbe->needsMemoryUnit)
            return 12665;

        auto linuxExportBank = std::make_unique<ProfileBank>();
        linuxExportBank->count = 2;
        linuxExportBank->documents[0] = linuxProbe->documents[1];
        linuxExportBank->documents[0].documentId = 1;
        auto linuxCap = profile_document_make_empty();
        linuxCap.documentId = 2;
        linuxCap.source.osFamily = PROFILE_SOURCE_OS_LINUX;
        linuxCap.applicationScope = PROFILE_SCOPE_UPDATE_INCLUDED;
        linuxCap.pointSetPolicy = PROFILE_POINTS_REPLACE;
        linuxCap.domains = PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_LOCK | PROFILE_DOMAIN_MEMORY | PROFILE_DOMAIN_POWER;
        linuxCap.topologyCount = 2;
        linuxCap.topology[0] = {75, 75, 920000LL, 1};
        linuxCap.points[0].kind = PROFILE_POINT_CAPTURED_OFFSET;
        linuxCap.points[0].capturedOffsetHz = 0;
        linuxCap.topology[1] = {76, 76, 925000LL, 1};
        linuxCap.lock.present = true;
        linuxCap.lock.mode = LOCK_MODE_FLATTEN;
        linuxCap.lock.anchorTopologyId = 76;
        linuxCap.lock.targetFrequencyHz = 2962000000LL;
        linuxCap.hasMemoryOffset = true;
        linuxCap.memoryOffsetHz = 3000000000LL;
        linuxCap.memorySurface = CLOCK_OFFSET_SURFACE_P0;
        linuxCap.power.present = true;
        linuxCap.power.ratio = {17, 20};
        linuxExportBank->documents[1] = linuxCap;
        std::string encoded;
        if (!profile_bank_encode(*linuxExportBank, encoded, &error)) return 12666;
        auto decodedBank = std::make_unique<ProfileBank>();
        if (!profile_bank_decode(encoded, *decodedBank, &error) || decodedBank->count != 2) return 12666;
        auto indexedRuntime = std::make_unique<DesiredSettings>();
        if (!profile_slot_document_runtime(decodedBank->documents[0], nullptr, *indexedRuntime, why, sizeof(why)) ||
            indexedRuntime->gpuOffsetMHz != 475 || indexedRuntime->memOffsetMHz != 3000 ||
            !indexedRuntime->hasLock || indexedRuntime->lockCi != 76 || indexedRuntime->lockMHz != 2957)
            return 12666;
        ProfileDestinationTopology winLive{};
        winLive.available = true; winLive.complete = true; winLive.count = 2;
        winLive.points[0] = {75, 920000LL, true, true};
        winLive.points[1] = {76, 925000LL, true, true};
        auto capRuntime = std::make_unique<DesiredSettings>();
        if (!profile_slot_document_runtime(decodedBank->documents[1], &winLive, *capRuntime, why, sizeof(why)) ||
            !capRuntime->curvePointCaptured[75] || capRuntime->curvePointCapturedOffsetHz[75] != 0 ||
            !capRuntime->hasLock || capRuntime->lockCi != 76 || capRuntime->lockMHz != 2962 ||
            capRuntime->memOffsetHz != 3000000000LL || capRuntime->memOffsetSurface != CLOCK_OFFSET_SURFACE_P0)
            return 12667;
    }
    return 0;
}
