// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_capture_commands.h"
#include "profile_editor_report.h"

struct ProfileEditorCaptureIO {
    const HardwareCaptureObservation& observation;
    ProfileDocument& output;
    std::vector<std::string>& lines;
    bool truncated = false;
    ProfileCaptureProposalContext context{};
    ProfileCaptureProposalContext capture_proposal_context() const { return context; }
    bool distinct(const std::string&, const std::string&, ProfileCodecError&) { return true; }
    bool capture(gc_u32 domains, HardwareCaptureObservation& out, ProfileCodecError& error) {
        if (domains != observation.requestedDomains)
            return profile_codec::fail(&error, 0, "snapshot domains differ from the reviewed request");
        out = observation; return true;
    }
    // A virtual output sink. No file, slot, ownership or hardware is changed.
    bool write(const std::string&, const std::string& bytes, ProfileCodecError& error) {
        auto bank = std::make_unique<ProfileBank>();
        if (!profile_bank_decode(bytes, *bank, &error)) return false;
        if (bank->count != 1) return profile_codec::fail(&error, 0, "capture did not produce one source document");
        output = bank->documents[0]; return true;
    }
    void message(const std::string& text) {
        const char* presentation = nullptr;
        if (text.rfind("Saved captured intent.", 0) == 0)
            presentation = "Captured source draft prepared; Save stores it; capture establishes no applied ownership.\n";
        else if (text.rfind("Capture is incomplete.", 0) == 0)
            presentation = "Some selected settings were omitted. Review the report and explicitly allow omitted settings to accept this snapshot.\n";
        else if (text.rfind("Review the report, then", 0) == 0)
            presentation = "Choose a representation; lock proposals are separate from exact programmed offsets.\n";
        profile_editor_report_append(lines, truncated, presentation ? presentation : text, "Additional capture details omitted");
    }
};
// Both report and acceptance use the CLI's strict capture/conversion contract.
// Acceptance consumes the frozen observation and never rereads the driver.
static inline int profile_editor_capture_review(const HardwareCaptureObservation& observation, bool allowPartial,
    ProfileCaptureRepresentation representation, ProfileDocument& output, std::vector<std::string>& lines,
    ProfileCaptureProposalContext context = {}, ProfileCaptureProposals* reviewed = nullptr) {
    lines.clear();
    const char* names[] = {"vf", "graphics", "memory", "power", "pin", "fan", "xbar", "system", "video"};
    std::string domains;
    for (int i = 0; i < 9; ++i) if (observation.requestedDomains & (1u << i)) {
        if (!domains.empty()) domains += ',';
        domains += names[i];
    }
    std::vector<std::string> args{"--capture-current", "editor-capture", "--domains", domains};
    if (allowPartial) args.emplace_back("--allow-partial");
    if (representation != PROFILE_CAPTURE_UNDECIDED) {
        args.emplace_back("--representation"); args.emplace_back(profile_capture_representation_name(representation));
    }
    ProfileEditorCaptureIO io{observation, output, lines, false, context}; int code = PROFILE_COMMAND_INVALID;
    (void)profile_capture_commands_run(args, "editor-config", io, code, reviewed);
    return code;
}
static inline int profile_editor_capture(const HardwareCaptureObservation& observation, bool allowPartial,
    bool exactDecision, ProfileDocument& output, std::vector<std::string>& lines) {
    return profile_editor_capture_review(observation, allowPartial, exactDecision ? PROFILE_CAPTURE_EXACT : PROFILE_CAPTURE_UNDECIDED,
        output, lines);
}
