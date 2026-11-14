// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_check_commands.h"
#include "profile_editor_report.h"

template<class Capture>
struct ProfileEditorCheckIO {
    const ProfileDocument& source;
    Capture& readCurrent;
    std::vector<std::string>& lines;
    bool truncated = false;
    bool read(const std::string&, std::string& output, size_t maximum, bool, ProfileCodecError& error) {
        auto bank = std::make_unique<ProfileBank>(); bank->count = 1; bank->documents[0] = source;
        if (!profile_bank_encode(*bank, output, &error)) return false;
        return output.size() <= maximum || profile_codec::fail(&error, 0, "source document exceeds preview limit");
    }
    bool capture(gc_u32 domains, HardwareCaptureObservation& out, ProfileCodecError& error) {
        return readCurrent(domains, out, error);
    }
    void message(const std::string& text) {
        profile_editor_report_append(lines, truncated, text, "Additional destination details omitted", true);
    }
};
// Reuse the CLI's mapping, exact-control and curve planner on an in-memory
// document. The adapter offers Capture and report output, never file writes.
template<class Capture>
static inline int profile_editor_check(const ProfileDocument& source, gc_u32 graphics, gc_u32 memory,
    Capture capture, std::vector<std::string>& lines, ProfileDestinationReview* review = nullptr, gc_u64 sourceRevision = 1) {
    lines.clear();
    std::vector<std::string> args{"--check-profile", "editor-source"};
    const auto name = [](gc_u32 choice) { return choice == CLOCK_OFFSET_SURFACE_P0 ? "p0" :
        choice == CLOCK_OFFSET_SURFACE_GLOBAL ? "global" : choice == DESIRED_GRAPHICS_VF_COMPONENT ? "vf" : "invalid"; };
    if (graphics) { args.emplace_back("--graphics-control"); args.emplace_back(name(graphics)); }
    if (memory) { args.emplace_back("--memory-control"); args.emplace_back(name(memory)); }
    ProfileEditorCheckIO<Capture> io{source, capture, lines};
    int code = PROFILE_COMMAND_INVALID;
    (void)profile_check_commands_run(args, io, code, review, sourceRevision);
    return code;
}
