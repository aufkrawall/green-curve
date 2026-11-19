// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// TUI "Import current GPU settings" and profile file dialogs: same model and
// engines as the Windows dialogs. Cases 9770-9799.
#include "linux_tui_internal.h"
#include "linux_debug_log.h"
#include "profile_capture_proposal_fixture.h"
#include "profile_ini_slot.h"
#include <map>
#include <cstring>

namespace tui_profile_tools_fixture {
static void linux_debug_logf(const char*, ...) {}
#include "linux_tui_profile_load.h"
#include "linux_tui_capture_draft.h"
#include "linux_tui_transfer.h"
struct IO {
    std::map<std::string, std::string> files;
    int writes = 0;
    bool read(const std::string& path, std::string& bytes, size_t maximum, bool missing, ProfileCodecError& error) {
        const auto it = files.find(path);
        if (it == files.end()) { bytes.clear(); return missing || profile_codec::fail(&error, 0, "missing fixture file"); }
        bytes = it->second; return bytes.size() <= maximum || profile_codec::fail(&error, 0, "fixture too large");
    }
    bool distinct(const std::string& a, const std::string& b, ProfileCodecError& error) {
        return a != b || profile_codec::fail(&error, 0, "same fixture path");
    }
    bool legacy(const std::string& bytes, int slot, ProfileDocument& d, bool& found, ProfileCodecError& error) {
        return profile_legacy_native_slot_decode(bytes, PROFILE_LEGACY_LINUX_INI, slot, d, found, &error);
    }
    bool write(const std::string& path, const std::string& bytes, ProfileCodecError&) { ++writes; files[path] = bytes; return true; }
    bool commit(const std::string& path, const ProfileBankEdit& edit, ProfileCodecError& error) {
        return profile_bank_commit_edit(edit, [&](std::string& bytes) { bytes = files[path]; return true; },
            [&](const std::string& bytes) { return write(path, bytes, error); }, &error);
    }
};
static HardwareCaptureObservation snapshot() {
    auto o = profile_capture_proposal_fixture::curve();
    o.requestedDomains = PROFILE_CAPTURE_DEFAULT_DOMAINS;
    for (gc_u32 domain : {(gc_u32)PROFILE_DOMAIN_MEMORY, (gc_u32)PROFILE_DOMAIN_POWER, (gc_u32)PROFILE_DOMAIN_XBAR,
             (gc_u32)PROFILE_DOMAIN_SYS_CLK, (gc_u32)PROFILE_DOMAIN_VIDEO_CLK})
        hardware_capture_domain(o, domain, CAPTURE_UNSUPPORTED, -3);
    o.fingerprint = hardware_capture_fingerprint(o);
    o.revision = service_control_review_revision(o.fingerprint, nullptr);
    return o;
}
static int run() {
    auto state = std::make_unique<TuiState>(); auto& s = *state;
    std::strcpy(s.configPath, "bank.ini"); s.currentSlot = 2; s.dirty = true;
    s.desired.hasMemOffset = 1; s.desired.memOffsetMHz = 17;
    const auto draft = s.desired;
    IO io;
    io.files["bank.ini"] = "[meta]\nlinux_mem_migrated=1\n[profile1]\nmem_offset_mhz=5\n";
    int reads = 0;
    const auto read = [&](gc_u32 domains, HardwareCaptureObservation& out, ProfileCodecError&) {
        ++reads; out = snapshot(); return domains == PROFILE_CAPTURE_DEFAULT_DOMAINS;
    };
    // 9770: opening reads the GPU once and offers the first empty slot.
    tui_capture_begin(s, read, io);
    if (!s.capture.active || reads != 1 || !s.capture.hasObservation || s.capture.slot != 2 ||
        s.capture.destination != "Slot 2 (empty)" || s.captureLines.size() != (size_t)PROFILE_CAPTURE_GROUP_COUNT)
        return 9770;
    // 9771: unreadable groups are shown but cannot be included; advanced clocks
    // are hidden when the GPU has none; the flat curve is preselected.
    const unsigned curve = 1u << PROFILE_CAPTURE_GROUP_CURVE, core = 1u << PROFILE_CAPTURE_GROUP_CORE,
        memory = 1u << PROFILE_CAPTURE_GROUP_MEMORY;
    if (s.capture.included != (curve | core) || (s.capture.shown & (1u << PROFILE_CAPTURE_GROUP_ADVANCED)) ||
        s.captureLines[PROFILE_CAPTURE_GROUP_MEMORY] != "Not readable on this GPU" ||
        s.capture.representation != s.captureProposals->suggested ||
        s.capture.choice != profile_capture_choice_label(*s.captureProposals, s.capture.representation))
    {
        fprintf(stderr, "9771: included=%x shown=%x memory=%s representation=%d choice=%s\n", s.capture.included,
            s.capture.shown, s.captureLines[2].c_str(), (int)s.capture.representation, s.capture.choice.c_str());
        return 9771;
    }
    if (tui_capture_toggle_group(s, PROFILE_CAPTURE_GROUP_MEMORY) || s.capture.included & memory) return 9772;
    // 9773: cycling the curve choice offers the flat curve and skips the
    // unavailable pin choice.
    while (s.capture.representation != PROFILE_CAPTURE_FLATTEN)
        if (!tui_capture_cycle_representation(s) || s.capture.representation == PROFILE_CAPTURE_PIN) return 9773;
    if (s.capture.choice.find("Flat curve at 1000 MHz") != 0) return 9773;
    if (!tui_capture_cycle_representation(s) || s.capture.representation != PROFILE_CAPTURE_EXACT ||
        s.capture.choice != "Copy every curve point exactly")
        return 9773;
    // 9774: saving writes an ordinary record to the chosen slot and leaves the
    // editor draft untouched.
    if (!tui_capture_cycle_slot(s, io) || s.capture.slot != 3) return 9774;
    int savedSlot = 0; DesiredSettings saved{};
    const auto save = [&](int slot, const DesiredSettings& record, char*, size_t) { savedSlot = slot; saved = record; return true; };
    if (!tui_capture_toggle_group(s, PROFILE_CAPTURE_GROUP_CORE) || !tui_capture_accept(s, save) || savedSlot != 3 ||
        !saved.curvePointCaptured[22] || saved.hasGpuOffset || saved.applicationScope == DESIRED_SCOPE_LEGACY ||
        s.capture.active || memcmp(&s.desired, &draft, sizeof(draft)) || !s.dirty)
        return 9775;
    // 9776: nothing included refuses with a reason and keeps the dialog.
    tui_capture_begin(s, read, io);
    s.capture.included = 0;
    if (tui_capture_accept(s, save) || !s.capture.active || !std::strstr(s.status, "Cannot save")) return 9776;
    tui_capture_cancel(s);
    if (s.capture.active || s.captureObservation) return 9777;
    // 9778: a failed read explains itself and offers to read again.
    const auto failing = [&](gc_u32, HardwareCaptureObservation&, ProfileCodecError& error) {
        return profile_codec::fail(&error, 0, "daemon offline");
    };
    tui_capture_begin(s, failing, io);
    if (!s.capture.active || !s.capture.failed || s.capture.hasObservation || s.captureLines.empty() ||
        s.captureLines[0].find("daemon offline") == std::string::npos)
        return 9778;
    tui_capture_cancel(s);

    // 9780: export lists saved slots and writes only the ticked ones.
    const auto act = [&](ActionType type, int index = 0) {
        (void)tui_transfer_action(s, ClickAction{0, 0, 0, 0, type, index, 0, 0}, io, [](TuiState&) {});
    };
    act(ACTION_TRANSFER_BEGIN);
    s.transferPresented = true;
    s.transferPath = "out.gcprofile";
    act(ACTION_TRANSFER_RECORD, -1);
    if (!s.transfer.active || s.transfer.importing || s.transfer.rows != 5 || !s.transfer.rowAvailable[0] ||
        s.transfer.rowAvailable[1] || !s.transfer.canRun || s.transferLines[0].find("Memory +5 MHz") != 0)
        return 9780;
    act(ACTION_TRANSFER_COMMIT);
    auto bank = std::make_unique<ProfileBank>();
    if (s.transfer.active || !profile_bank_decode(io.files["out.gcprofile"], *bank) || bank->count != 1 ||
        bank->documents[0].memoryOffsetHz != 5000000LL)
        return 9781;
    // 9782: an existing file is replaced only after a second EXPORT.
    io.files["bank.ini"] += "[profile4]\nmem_offset_mhz=9\n";
    act(ACTION_TRANSFER_BEGIN); s.transferPresented = true; s.transferPath = "out.gcprofile";
    act(ACTION_TRANSFER_RECORD, -1);
    const int writes = io.writes;
    act(ACTION_TRANSFER_COMMIT);
    if (!s.transfer.active || !s.transfer.confirmOverwrite || io.writes != writes) return 9782;
    act(ACTION_TRANSFER_COMMIT);
    if (s.transfer.active || !profile_bank_decode(io.files["out.gcprofile"], *bank) || bank->count != 2) return 9783;
    // 9784: import reads the file, maps records to empty slots first, and
    // writes exactly those slots.
    act(ACTION_TRANSFER_BEGIN); s.transferPresented = true;
    act(ACTION_TRANSFER_MODE);
    s.transferPath = "out.gcprofile";
    act(ACTION_TRANSFER_PREVIEW);
    if (!s.transfer.importing || !s.transfer.probed || s.transfer.rows != 2 || s.transfer.rowSlot[0] != 2 ||
        s.transfer.rowSlot[1] != 3 || !s.transfer.canRun)
        return 9784;
    act(ACTION_TRANSFER_SLOT, 1);
    if (s.transfer.rowSlot[1] != 4 || s.transferLines.back().find("slot 4") == std::string::npos) return 9785;
    act(ACTION_TRANSFER_SLOT, 1); act(ACTION_TRANSFER_SLOT, 1); act(ACTION_TRANSFER_SLOT, 1);
    if (s.transfer.rowSlot[1] != 2 || s.transfer.canRun || s.transferLines.back().find("same slot") == std::string::npos) return 9786;
    act(ACTION_TRANSFER_SLOT, 1);
    act(ACTION_TRANSFER_COMMIT);
    std::string text; ProfileCodecError error; bool found = false; ProfileDocument document{};
    if (s.transfer.active || !profile_config::decode(io.files["bank.ini"], text, &error) ||
        !profile_document_slot_decode(text, 3, document, found, error) || !found || document.memoryOffsetHz != 9000000LL ||
        memcmp(&s.desired, &draft, sizeof(draft)))
        return 9787;
    // 9788: an unmarked old Linux file asks how memory was saved.
    io.files["old.ini"] = "[profile1]\nmem_offset_mhz=700\n";
    act(ACTION_TRANSFER_BEGIN); s.transferPresented = true; act(ACTION_TRANSFER_MODE);
    s.transferPath = "old.ini"; act(ACTION_TRANSFER_PREVIEW);
    if (!s.transfer.legacyFile || s.transfer.format != PROFILE_LEGACY_WINDOWS_INI || !s.transfer.probed) return 9788;
    act(ACTION_TRANSFER_FORMAT);
    if (s.transfer.format != PROFILE_LEGACY_LINUX_INI || s.transfer.probed || !s.transfer.askMemory || s.transfer.canRun) return 9789;
    act(ACTION_TRANSFER_MEMORY);
    if (!s.transfer.probed || !s.transferProbe || s.transferProbe->documents[0].memoryOffsetHz != 700000000LL) return 9790;
    tui_transfer_cancel(s);
    if (s.transfer.active || s.transferProbe) return 9791;
    return 0;
}
}  // namespace tui_profile_tools_fixture
int run_tui_profile_tools_tests() { return tui_profile_tools_fixture::run(); }
