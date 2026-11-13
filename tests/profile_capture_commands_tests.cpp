// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "profile_capture_commands.h"
#include "profile_check_commands.h"
#include <map>

namespace {
struct CaptureIO {
    std::map<std::string, std::string> files;
    std::string messages;
    int captures = 0, writes = 0;
    bool failCapture = false, failWrite = false, malformed = false, noControls = false;
    bool readOnly = false;
    bool read(const std::string& path, std::string& data, size_t limit, bool, ProfileCodecError& error) {
        auto found = files.find(path);
        if (found == files.end() || found->second.size() > limit) return profile_codec::fail(&error, 0, "fixture read refused");
        data = found->second; return true;
    }
    bool distinct(const std::string& a, const std::string& b, ProfileCodecError& error) {
        return a != b || profile_codec::fail(&error, 0, "same destination");
    }
    void message(const std::string& text) { messages += text; }
    bool write(const std::string& path, const std::string& data, ProfileCodecError& error) {
        ++writes;
        if (failWrite) return profile_codec::fail(&error, 0, "fixture atomic write refused");
        files[path] = data; return true;
    }
    bool capture(gc_u32 domains, HardwareCaptureObservation& out, ProfileCodecError& error) {
        ++captures;
        if (failCapture) return profile_codec::fail(&error, 0, "fixture session unavailable");
        out = {}; out.valid = 1; out.schemaVersion = 1; out.gpuGeneration = 3;
        out.binding.valid = 1; out.requestedDomains = domains; out.revision = out.fingerprint = 7;
        for (int i = 0; i < 9; ++i) if (domains & (1u << i))
            hardware_capture_domain(out, 1 << i, CAPTURE_UNSUPPORTED, -100001);
        if (noControls) return true;
        if (domains & PROFILE_DOMAIN_VF_CURVE) {
            out.topologyComplete = 1; out.pointCount = 3;
            for (gc_u32 i = 0; i < 3; ++i) {
                auto& p = out.points[i];
                p.index = 22 + i; p.voltageUv = 662000 + (gc_i64)i * 6250; p.writableState = readOnly && !i ? 2 : 1;
                p.offsetValid = p.frequencyValid = 1; p.offsetHz = i == 0 ? 0 : i == 1 ? 15000500 : -15000500;
                p.frequencyHz = 1000000500 + (gc_i64)i * 1000000;
            }
            hardware_capture_domain(out, PROFILE_DOMAIN_VF_CURVE, CAPTURE_COMPLETE);
        }
        if (domains & PROFILE_DOMAIN_GRAPHICS_OFFSET) {
            out.graphics.valid = 1; out.graphics.surface = CLOCK_OFFSET_SURFACE_P0;
            hardware_capture_domain(out, PROFILE_DOMAIN_GRAPHICS_OFFSET, CAPTURE_COMPOSITION_UNRESOLVED);
        }
        if (domains & PROFILE_DOMAIN_MEMORY) {
            out.memory.valid = 1; out.memory.surface = CLOCK_OFFSET_SURFACE_P0; out.memory.value = -500000;
            hardware_capture_domain(out, PROFILE_DOMAIN_MEMORY, CAPTURE_COMPLETE);
        }
        if (domains & PROFILE_DOMAIN_POWER) {
            out.power.currentValid = out.power.defaultValid = 1;
            out.power.currentMw = 249000; out.power.defaultMw = 250000;
            hardware_capture_domain(out, PROFILE_DOMAIN_POWER, CAPTURE_COMPLETE);
        }
        if (malformed) out.pointCount = 129;
        return true;
    }
};
}

#include "profile_check_commands_cases.h"
int run_profile_capture_commands_tests() {
    if (int failure = profile_check_commands_cases()) return failure;
    CaptureIO io; int code = -1;
    io.files["capture.gcprofile"] = "previous file";
    const std::vector<std::string> base{"--capture-current", "capture.gcprofile"};
    if (!profile_capture_commands_run(base, "config.ini", io, code) || code != PROFILE_COMMAND_DECISION ||
        io.captures != 1 || io.writes || io.files["capture.gcprofile"] != "previous file" ||
        io.messages.find("composition unresolved") == std::string::npos ||
        io.messages.find("Pin knowledge: unknown") == std::string::npos) return 8540;
    auto partial = base; partial.push_back("--allow-partial");
    if (!profile_capture_commands_run(partial, "config.ini", io, code) || code != PROFILE_COMMAND_DECISION || io.writes ||
        io.messages.find("--representation exact") == std::string::npos) return 8541;
    auto confirmed = partial; confirmed.insert(confirmed.end(), {"--representation", "exact"});
    if (!profile_capture_commands_run(confirmed, "config.ini", io, code) || code || io.writes != 1) return 8542;
    auto decoded = std::make_unique<ProfileBank>();
    if (!profile_bank_decode(io.files["capture.gcprofile"], *decoded) || decoded->count != 1 ||
        decoded->documents[0].points[0].kind != PROFILE_POINT_CAPTURED_OFFSET || decoded->documents[0].points[0].capturedOffsetHz ||
        decoded->documents[0].points[1].capturedOffsetHz != 15000500 || decoded->documents[0].points[2].capturedOffsetHz != -15000500 ||
        decoded->documents[0].topology[0].voltageUv != 662000 || decoded->documents[0].memoryOffsetHz != -500000 ||
        decoded->documents[0].power.ratio.numerator != 249 || decoded->documents[0].power.ratio.denominator != 250 ||
        decoded->documents[0].source.captureComplete || decoded->documents[0].lock.present ||
        !decoded->documents[0].graphicsOffset.hasObservedOffset || decoded->documents[0].graphicsOffset.present) return 8543;
    // Local save/restart decoding retains the same exact source document.
    std::string section; ProfileDocument restored{}; bool found = false;
    if (!profile_slot_encode(decoded->documents[0], 1, section) ||
        !profile_slot_decode(std::string_view(section).substr(section.find('\n') + 1), restored, found) || !found ||
        !profile_document_semantically_equal(decoded->documents[0], restored) ||
        restored.points[1].observedFrequencyHz != decoded->documents[0].points[1].observedFrequencyHz ||
        restored.graphicsOffset.observedSurface != CLOCK_OFFSET_SURFACE_P0) return 8544;
    const auto saved = io.files["capture.gcprofile"];
    io.failCapture = true;
    if (!profile_capture_commands_run(confirmed, "config.ini", io, code) || code != PROFILE_COMMAND_IO ||
        io.writes != 1 || io.files["capture.gcprofile"] != saved) return 8545;
    io.failCapture = false; io.malformed = true;
    if (!profile_capture_commands_run(confirmed, "config.ini", io, code) || code != PROFILE_COMMAND_IO || io.writes != 1) return 8546;
    io.malformed = false; io.noControls = true;
    if (!profile_capture_commands_run(confirmed, "config.ini", io, code) || code != PROFILE_COMMAND_IO || io.writes != 1) return 8547;
    io.noControls = false; io.failWrite = true;
    if (!profile_capture_commands_run(confirmed, "config.ini", io, code) || code != PROFILE_COMMAND_IO ||
        io.files["capture.gcprofile"] != saved) return 8548;
    io.failWrite = false;
    const std::vector<std::string> memory{"--capture-current", "memory.gcprofile", "--domains", "memory", "--representation", "exact"};
    if (!profile_capture_commands_run(memory, "config.ini", io, code) || code ||
        !profile_bank_decode(io.files["memory.gcprofile"], *decoded) || decoded->documents[0].domains != PROFILE_DOMAIN_MEMORY ||
        !decoded->documents[0].source.captureComplete || decoded->documents[0].source.pinKnowledge != PROFILE_PIN_UNKNOWN) return 8549;
    const int prior = io.captures;
    const std::vector<std::vector<std::string>> invalid{
        {"--capture-current", "x", "--apply-config"}, {"--capture-current", "config.ini"},
        {"--capture-current", "x", "--domains", "vf,vf"}, {"--capture-current", "x", "--domains", "vf,"},
        {"--capture-current", "x", "--domains", "unknown"}, {"--capture-current", "x", "--representation", "sampled"},
        {"--capture-current", "x", "--allow-partial", "--allow-partial"},
        {"--capture-current", "x", "--import-profiles", "y"}, {"--capture-current"},
    };
    for (const auto& args : invalid)
        if (!profile_capture_commands_run(args, "config.ini", io, code) || code != PROFILE_COMMAND_INVALID || io.captures != prior) return 8550;
    if (profile_capture_commands_run({"--help"}, "config.ini", io, code)) return 8551;
    return 0;
}
