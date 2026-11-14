// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_CAPTURE_COMMANDS_H
#define GREEN_CURVE_PROFILE_CAPTURE_COMMANDS_H
#include "profile_commands.h"
#include "hardware_capture_runtime.h"

template<class IO>
static inline void profile_capture_report_clocks(IO& io, const char* domain,
    const HardwareCaptureScalar& primary, const HardwareCaptureScalar& alternate) {
    const HardwareCaptureScalar* controls[] = {&primary, &alternate};
    for (const auto* control : controls) {
        if (!control->surface) continue;
        std::string line = std::string("  ") + domain + " control (" +
            hardware_capture_clock_surface_name(control->surface) + "): ";
        line += control->valid ? std::to_string(control->value) + " Hz" :
            "unavailable (driver status " + std::to_string(control->status) + ")";
        line += std::string("; writable=") + (control->writable ? "yes" : "no");
        line += control->rangeValid ? "; range=" + std::to_string(control->minimum) +
            ".." + std::to_string(control->maximum) + " Hz\n" : "; range=unreported\n";
        io.message(line);
    }
    if (alternate.valid && alternate.value)
        io.message(std::string("  ") + domain + ": another nonzero control is observed; complete replay needs relationship review.\n");
}

struct ProfileCaptureCommand {
    std::string file, config;
    gc_u32 domains = PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_GRAPHICS_OFFSET | PROFILE_DOMAIN_MEMORY |
        PROFILE_DOMAIN_POWER | PROFILE_DOMAIN_LOCK;
    bool allowPartial = false, exact = false;
};
static inline bool profile_capture_domains(std::string_view value, gc_u32& output) {
    const std::string_view names[] = {"vf", "graphics", "memory", "power", "pin", "fan", "xbar", "system", "video"};
    gc_u32 mask = 0;
    for (;;) {
        const size_t comma = value.find(','); const auto item = value.substr(0, comma);
        int found = -1;
        for (int i = 0; i < 9; ++i) if (item == names[i]) found = i;
        if (found < 0 || (mask & (1u << found))) return false;
        mask |= 1u << found;
        if (comma == std::string_view::npos) break;
        value.remove_prefix(comma + 1);
    }
    output = mask; return true;
}
static inline bool profile_capture_parse(const std::vector<std::string>& args, const char* defaultConfig,
    ProfileCaptureCommand& output, ProfileCodecError& error) {
    ProfileCaptureCommand c; c.config = defaultConfig ? defaultConfig : "config.ini";
    bool config = false, domains = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const auto& key = args[i];
        if (key == "--allow-partial") {
            if (c.allowPartial) return profile_codec::fail(&error, 0, "duplicate partial-capture decision");
            c.allowPartial = true; continue;
        }
        if (key != "--capture-current" && key != "--domains" && key != "--representation" && key != "--config")
            return profile_codec::fail(&error, 0, "capture cannot be combined with another command");
        if (++i == args.size() || args[i].empty() || args[i].size() >= 4096)
            return profile_codec::fail(&error, 0, "missing or overlong capture value");
        const auto& value = args[i];
        if (key == "--capture-current") {
            if (!c.file.empty()) return profile_codec::fail(&error, 0, "duplicate capture destination");
            c.file = value;
        } else if (key == "--domains") {
            if (domains || !profile_capture_domains(value, c.domains)) return profile_codec::fail(&error, 0, "invalid or duplicate capture domains");
            domains = true;
        } else if (key == "--config") {
            if (config) return profile_codec::fail(&error, 0, "duplicate configuration path");
            config = true; c.config = value;
        } else {
            if (c.exact || value != "exact") return profile_codec::fail(&error, 0, "select exact programmed controls for capture");
            c.exact = true;
        }
    }
    if (c.file.empty() || c.file == c.config) return profile_codec::fail(&error, 0, "capture requires a separate output file");
    output = std::move(c); return true;
}

template <class IO>
static inline bool profile_capture_commands_run(const std::vector<std::string>& args,
    const char* defaultConfig, IO& io, int& exitCode) {
    bool handled = false;
    for (const auto& arg : args) handled |= arg == "--capture-current";
    if (!handled) return false;
    ProfileCaptureCommand command; ProfileCodecError error;
    const auto reject = [&](int code) {
        io.message(std::string("Capture: ") + error.message + "\n"); exitCode = code; return true;
    };
    if (!profile_capture_parse(args, defaultConfig, command, error) || !io.distinct(command.config, command.file, error))
        return reject(PROFILE_COMMAND_INVALID);
    auto capture = std::make_unique<HardwareCaptureObservation>();
    if (!io.capture(command.domains, *capture, error)) return reject(PROFILE_COMMAND_IO);
    if (!capture->valid || !hardware_capture_observation_valid(*capture) || capture->requestedDomains != command.domains) {
        profile_codec::fail(&error, 0, "service returned an invalid or different capture"); return reject(PROFILE_COMMAND_IO);
    }
    char summary[320]{}; hardware_capture_summary(*capture, summary, sizeof(summary));
    io.message(std::string(summary) + "\n");
    if (command.domains & (PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_GRAPHICS_OFFSET))
        profile_capture_report_clocks(io, "Graphics", capture->graphics, capture->graphicsAlternate);
    if (command.domains & PROFILE_DOMAIN_MEMORY)
        profile_capture_report_clocks(io, "Memory", capture->memory, capture->memoryAlternate);
    const char* statuses[] = {"not requested", "captured", "unsupported", "read failed", "state changed", "composition unresolved", "partial"};
    for (int i = 0; i < 9; ++i) if (command.domains & (1u << i))
        io.message("  " + profile_command_domains(1 << i) + ": " + statuses[capture->domainStatus[i]] +
            " (driver status " + std::to_string(capture->driverStatus[i]) + ")\n");
    io.message("Best-effort consistent programmed controls; observed clocks may vary. Pin knowledge: " +
        std::string(!capture->pinGetterValid ? "unknown" : capture->pinPresent ? "present" : "absent") + ".\n");
    if (!capture->capturedDomains) {
        profile_codec::fail(&error, 0, "no requested controls were captured; output was not written"); return reject(PROFILE_COMMAND_IO);
    }
    if (capture->omittedDomains && !command.allowPartial) {
        io.message("Capture is incomplete. Select --allow-partial or choose --domains explicitly before saving.\n");
        exitCode = PROFILE_COMMAND_DECISION; return true;
    }
    if (!command.exact) {
        io.message("Review the report, then select --representation exact to save programmed offsets; no flatten or pin is inferred.\n");
        exitCode = PROFILE_COMMAND_DECISION; return true;
    }
    auto bank = std::make_unique<ProfileBank>(); bank->count = 1;
    if (!hardware_capture_to_document(*capture, bank->documents[0], error.message, sizeof(error.message))) return reject(PROFILE_COMMAND_INVALID);
    snprintf(bank->documents[0].displayName, sizeof(bank->documents[0].displayName), "%s", "Captured GPU settings");
    std::string encoded;
    if (!profile_bank_encode(*bank, encoded, &error)) return reject(PROFILE_COMMAND_INVALID);
    if (!io.write(command.file, encoded, error)) return reject(PROFILE_COMMAND_IO);
    io.message("Saved captured intent. Import it into a local slot for review; capture establishes no applied ownership.\n");
    exitCode = PROFILE_COMMAND_OK; return true;
}
#endif
