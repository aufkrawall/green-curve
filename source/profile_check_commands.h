// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_CHECK_COMMANDS_H
#define GREEN_CURVE_PROFILE_CHECK_COMMANDS_H
#include "profile_commands.h"
#include "profile_destination_review.h"

namespace profile_check_detail {
template <class IO>
static inline auto distinct_impl(IO& io, const std::string& a, const std::string& b, ProfileCodecError& error, int)
    -> decltype(io.distinct(a, b, error)) {
    return io.distinct(a, b, error);
}
template <class IO>
static inline bool distinct_impl(IO&, const std::string&, const std::string&, ProfileCodecError& error, long) {
    return profile_codec::fail(&error, 0, "output path is unsupported in read-only mode");
}
template <class IO>
static inline auto write_impl(IO& io, const std::string& path, const std::string& bytes, ProfileCodecError& error, int)
    -> decltype(io.write(path, bytes, error)) {
    return io.write(path, bytes, error);
}
template <class IO>
static inline bool write_impl(IO&, const std::string&, const std::string&, ProfileCodecError& error, long) {
    return profile_codec::fail(&error, 0, "output writing is unsupported in read-only mode");
}
static inline bool parse_convert_domains(std::string_view value, bool& all,
    std::vector<ProfileConversionKind>& out, gc_u32& mask) {
    if (value == "all") { all = true; return true; }
    for (;;) {
        const size_t comma = value.find(',');
        const auto token = value.substr(0, comma);
        ProfileConversionKind kind = PROFILE_CONVERT_VF_OFFSETS;
        if (token.empty() || !profile_conversion_kind_from_key(token, kind)) return false;
        const gc_u32 bit = 1u << (unsigned)kind;
        if (mask & bit) return false;
        mask |= bit;
        out.push_back(kind);
        if (comma == std::string_view::npos) break;
        value.remove_prefix(comma + 1);
    }
    return true;
}
} // namespace profile_check_detail

// The command deliberately has no write/commit/Apply callback. It parses before
// contacting the existing bound service session, through passive Capture only.
template <class IO>
static inline bool profile_check_commands_run(const std::vector<std::string>& args, IO& io, int& exitCode,
    ProfileDestinationReview* review = nullptr, gc_u64 sourceRevision = 1) {
    if (review) *review = {};
    bool handled = false;
    for (const auto& arg : args) handled |= arg == "--check-profile";
    if (!handled) return false;
    ProfileCodecError error; std::string file, outputFile; int selected = 0;
    gc_u32 graphicsChoice = 0, memoryChoice = 0, convertMask = 0;
    bool restoreTail = false, convertRequested = false, convertAll = false;
    std::vector<ProfileConversionKind> requestedDomains;
    const auto reject = [&](int code) {
        if (review) review->error = error;
        io.message(std::string("Profile check: ") + error.message + "\n"); exitCode = code; return true;
    };
    for (size_t i = 0; i < args.size(); ++i) {
        const auto& key = args[i];
        if (key == "--restore-captured-tail") {
            if (restoreTail) { profile_codec::fail(&error, 0, "duplicate --restore-captured-tail"); return reject(PROFILE_COMMAND_INVALID); }
            restoreTail = true; continue;
        }
        if ((key != "--check-profile" && key != "--profile" && key != "--graphics-control" && key != "--memory-control" &&
                key != "--convert-domain" && key != "--output") || ++i == args.size() ||
            args[i].empty() || args[i].size() >= 4096) {
            profile_codec::fail(&error, 0,
                "use --check-profile FILE [--profile N] [--graphics-control p0|global|vf] [--memory-control p0|global] "
                "[--convert-domain domains|all] [--restore-captured-tail] [--output FILE]");
            return reject(PROFILE_COMMAND_INVALID);
        }
        if (key == "--check-profile") {
            if (!file.empty()) { profile_codec::fail(&error, 0, "duplicate profile file"); return reject(PROFILE_COMMAND_INVALID); }
            file = args[i];
        } else if (key == "--output") {
            if (!outputFile.empty()) { profile_codec::fail(&error, 0, "duplicate output file"); return reject(PROFILE_COMMAND_INVALID); }
            outputFile = args[i];
        } else if (key == "--convert-domain") {
            if (convertRequested || !profile_check_detail::parse_convert_domains(args[i], convertAll, requestedDomains, convertMask)) {
                profile_codec::fail(&error, 0, "invalid or duplicate --convert-domain selection"); return reject(PROFILE_COMMAND_INVALID);
            }
            convertRequested = true;
        } else if (key == "--graphics-control" || key == "--memory-control") {
            auto& choice = key == "--graphics-control" ? graphicsChoice : memoryChoice;
            const gc_u32 parsed = args[i] == "p0" ? CLOCK_OFFSET_SURFACE_P0 :
                args[i] == "global" ? CLOCK_OFFSET_SURFACE_GLOBAL :
                args[i] == "vf" && key == "--graphics-control" ? (gc_u32)DESIRED_GRAPHICS_VF_COMPONENT : 0u;
            if (choice || !parsed) {
                profile_codec::fail(&error, 0, "invalid or duplicate clock control selection"); return reject(PROFILE_COMMAND_INVALID);
            }
            choice = parsed;
        } else if (selected || !profile_codec::ordinal(args[i], selected, 1, PROFILE_BANK_CAPACITY)) {
            profile_codec::fail(&error, 0, "invalid or duplicate profile selection"); return reject(PROFILE_COMMAND_INVALID);
        }
    }
    if (!outputFile.empty() && review) {
        profile_codec::fail(&error, 0, "--output cannot be used in editor preview mode"); return reject(PROFILE_COMMAND_INVALID);
    }
    if (!outputFile.empty() && !convertRequested && !restoreTail) {
        profile_codec::fail(&error, 0, "--output requires --convert-domain or --restore-captured-tail"); return reject(PROFILE_COMMAND_INVALID);
    }
    if (!outputFile.empty() && !profile_check_detail::distinct_impl(io, file, outputFile, error, 0))
        return reject(PROFILE_COMMAND_INVALID);
    std::string bytes; auto bank = std::make_unique<ProfileBank>();
    if (!io.read(file, bytes, PROFILE_FILE_MAX_BYTES, false, error)) return reject(PROFILE_COMMAND_IO);
    if (!profile_bank_decode(bytes, *bank, &error)) return reject(PROFILE_COMMAND_INVALID);
    if ((!selected && bank->count != 1) || selected > bank->count) {
        profile_codec::fail(&error, 0, "select one existing bank entry with --profile N"); return reject(PROFILE_COMMAND_INVALID);
    }
    if (!selected) selected = 1;
    const auto& source = bank->documents[selected - 1];
    if (review) { review->source = source; review->sourceRevision = sourceRevision; review->stage = PROFILE_REVIEW_SOURCE; }
    if ((graphicsChoice && !source.graphicsOffset.present) || (memoryChoice && !source.hasMemoryOffset)) {
        profile_codec::fail(&error, 0, "clock control selection requires that profile domain");
        return reject(PROFILE_COMMAND_INVALID);
    }
    const char* scopeNames[] = {"update included settings", "replace owned profile", "release owned settings"};
    io.message("Profile " + std::to_string(selected) + ": " + source.displayName + "\nIncluded: " +
        profile_command_domains(source.domains) + "; scope=" + scopeNames[source.applicationScope] + "\n");
    auto working = std::make_unique<ProfileDocument>(source);
    gc_u64 workingRevision = sourceRevision ? sourceRevision : 1;
    bool transformed = false;
    if (restoreTail) {
        auto restored = std::make_unique<ProfileDocument>();
        if (!profile_document_restore_captured_tail(*working, *restored, error.message, sizeof(error.message)))
            return reject(PROFILE_COMMAND_DECISION);
        *working = *restored; ++workingRevision; transformed = true;
        io.message("Restored exact captured VF tail offsets without recapture.\n");
    }
    const gc_u32 domains = 511u; // Bind later review to preserved and cleanup domains too.
    auto capture = std::make_unique<HardwareCaptureObservation>();
    if (!io.capture(domains, *capture, error)) return reject(PROFILE_COMMAND_IO);
    ProfileDestinationTopology destination;
    if (capture->requestedDomains != domains ||
        !profile_destination_from_capture(*capture, destination, error.message, sizeof(error.message))) {
        profile_codec::fail(&error, 0, "destination capture is invalid or differs from the request"); return reject(PROFILE_COMMAND_IO);
    }
    if (review) { review->capture = *capture; review->stage = PROFILE_REVIEW_CAPTURE; }
    const auto resolve_routes = [&](const ProfileDocument& doc) {
        ProfileRuntimeRoutes routes;
        routes.memory = memoryChoice ? memoryChoice : capture->memory.valid ? capture->memory.surface : 0;
        if (doc.graphicsOffset.present) {
            bool projection = false;
            for (int i = 0; i < doc.topologyCount; ++i) projection |= doc.points[i].kind == PROFILE_POINT_PROJECTION;
            if (graphicsChoice) routes.graphics = graphicsChoice;
            else if (projection || doc.graphicsOffset.exclusionPolicy != PROFILE_EXCLUDE_NONE)
                routes.graphics = DESIRED_GRAPHICS_VF_COMPONENT;
            else if (capture->composition == CAPTURE_COMPOSITION_INDEPENDENT && capture->graphics.valid)
                routes.graphics = capture->graphics.surface;
        }
        return routes;
    };
    ProfileRuntimeRoutes routes = resolve_routes(*working);
    auto conversionPlan = std::make_unique<ProfileConversionPlan>();
    if (convertRequested) {
        profile_conversion_plan_build(*working, *capture, destination, workingRevision, routes, *conversionPlan);
        if (convertAll) {
            for (int i = 0; i < conversionPlan->count; ++i)
                if (conversionPlan->items[i].convertible) requestedDomains.push_back(conversionPlan->items[i].kind);
        }
        bool conversionRejected = false;
        for (const auto kind : requestedDomains) {
            routes = resolve_routes(*working);
            profile_conversion_plan_build(*working, *capture, destination, workingRevision, routes, *conversionPlan);
            const auto* cand = conversionPlan->find(kind);
            if (!cand) {
                if (kind == PROFILE_CONVERT_XBAR_VOLTAGE) {
                    profile_codec::fail(&error, 0, "voltage conversion requires separate review; no safe rounding direction is established");
                    conversionRejected = true;
                } else {
                    io.message(std::string("profile-map: domain=") + profile_conversion_kind_key(kind) + " status=not-needed\n");
                }
                continue;
            }
            char summary[256]{};
            if (!cand->convertible) {
                profile_conversion_summary(cand->batch, PROFILE_CONVERSION_REJECTED, summary, sizeof(summary));
                io.message(std::string(summary) + " reason=" + (cand->why[0] ? cand->why : "unresolved") + "\n");
                profile_codec::fail(&error, 0, cand->why[0] ? cand->why : "conversion candidate is unresolved");
                conversionRejected = true;
                continue;
            }
            auto derived = std::make_unique<ProfileDocument>();
            if (!profile_conversion_accept_item(*cand, *working, workingRevision, *capture, destination, routes,
                    *derived, summary, sizeof(summary), error.message, sizeof(error.message))) {
                io.message(std::string(summary) + "\n");
                conversionRejected = true;
                continue;
            }
            io.message(std::string(summary) + "\n");
            *working = *derived; ++workingRevision; transformed = true;
        }
        if (convertAll && !conversionRejected) {
            routes = resolve_routes(*working);
            profile_conversion_plan_build(*working, *capture, destination, workingRevision, routes, *conversionPlan);
            for (int i = 0; i < conversionPlan->count; ++i) {
                if (!conversionPlan->items[i].unresolved) continue;
                profile_codec::fail(&error, 0, conversionPlan->items[i].why[0]
                    ? conversionPlan->items[i].why : "unresolved domain conversion");
                conversionRejected = true; break;
            }
        }
        if (conversionRejected) {
            if (review) { review->conversions = *conversionPlan; review->error = error; }
            io.message(std::string("Profile check: ") + error.message + "\n");
            exitCode = PROFILE_COMMAND_CONVERSION_REQUIRED;
            return true;
        }
    }
    routes = resolve_routes(*working);
    profile_conversion_plan_build(*working, *capture, destination, workingRevision, routes, *conversionPlan);
    auto runtime = std::make_unique<DesiredSettings>(); ProfileMappingReport mapping;
    auto reviewedSource = std::make_unique<ProfileDocument>(*working);
    if (memoryChoice) reviewedSource->memorySurface = memoryChoice;
    const bool resolved = profile_resolve_runtime(*reviewedSource, destination,
        sourceRevision ? workingRevision : 0, routes, *runtime, mapping, error.message, sizeof(error.message));
    if (review) {
        review->routes = routes; review->mapping = mapping; review->conversions = *conversionPlan;
        if (transformed) { review->source = *working; review->sourceRevision = workingRevision; }
        if (resolved) { review->runtime = *runtime; review->stage = PROFILE_REVIEW_MAPPED; }
    }
    const char* issueNames[] = {"offline", "invalid topology", "incomplete topology", "legacy index requires review",
        "legacy exclusions require review", "missing voltage", "ambiguous voltage", "read-only point",
        "duplicate destination", "unavailable domain"};
    const int shown = mapping.issueCount < 16 ? mapping.issueCount : 16;
    for (int i = 0; i < shown; ++i) {
        const auto& issue = mapping.issues[i];
        io.message(std::string("Mapping: ") + issueNames[issue.kind] + "; identity=" + std::to_string(issue.topologyId) +
            "; voltage=" + std::to_string(issue.voltageUv) + " uV\n");
    }
    if (mapping.issueCount + mapping.omittedIssues > shown)
        io.message("Additional mapping issues: " + std::to_string(mapping.issueCount + mapping.omittedIssues - shown) + "\n");
    for (int i = 0; i < conversionPlan->count; ++i) {
        const auto& item = conversionPlan->items[i];
        char summary[256]{};
        if (item.convertible) {
            profile_conversion_summary(item.batch, PROFILE_CONVERSION_PREVIEW, summary, sizeof(summary));
            io.message(std::string(summary) + "\n");
        } else {
            io.message(std::string("profile-map: domain=") + profile_conversion_kind_key(item.kind) +
                " status=unresolved reason=" + item.why + "\n");
        }
    }
    if (resolved) {
        int captured = 0, absolute = 0, projected = 0;
        for (int i = 0; i < VF_NUM_POINTS; ++i) {
            captured += runtime->curvePointCaptured[i] != 0;
            projected += runtime->hasCurvePoint[i] && runtime->curvePointFromGpuOffset[i];
            absolute += runtime->hasCurvePoint[i] && !runtime->curvePointFromGpuOffset[i];
        }
        io.message("Mapped points: captured=" + std::to_string(captured) + "; absolute=" + std::to_string(absolute) +
            "; projected=" + std::to_string(projected) + "; complete table match=" + (mapping.completeTopologyMatch ? "yes\n" : "no\n"));
        if (runtime->hasGpuOffset) io.message("Graphics: " + std::to_string(desired_graphics_hz(*runtime)) +
            " Hz; control selection=" + (routes.graphics == DESIRED_GRAPHICS_AUTO ? "unresolved" : "identified") +
            "; surface=" + (routes.graphics == DESIRED_GRAPHICS_VF_COMPONENT ? "VF table" : hardware_capture_clock_surface_name(routes.graphics)) + "\n");
        if (runtime->hasMemOffset) io.message("Memory: " + std::to_string(desired_memory_hz(*runtime)) +
            " physical Hz; surface=" + hardware_capture_clock_surface_name(runtime->memOffsetSurface) + "\n");
        if (runtime->hasPowerLimit) { const auto ratio = desired_power_ratio(*runtime);
            io.message("Power: " + std::to_string(ratio.numerator) + "/" + std::to_string(ratio.denominator) + " of the destination default\n"); }
        if (runtime->hasLock) io.message("Lock: " + std::to_string(desired_lock_hz(*runtime)) + " Hz\n");
        const auto plan = profile_control_plan(*runtime, *capture);
        if (review) { review->controls = plan; review->stage = PROFILE_REVIEW_PLANNED; }
        io.message(std::string("Control feasibility: disposition=") + profile_control_disposition_name(plan.disposition) + "\n");
        if (plan.curveChecked) {
            io.message(std::string("Curve: disposition=") + curve_plan_disposition_name(plan.curve.disposition) +
                "; evidence=" + curve_plan_evidence_name(plan.curve.evidence) + "; range=" + curve_plan_range_source_name(plan.curve.range.source) + "\n");
            for (int i = 0; i < plan.curve.issueCount; ++i) {
                const auto& issue = plan.curve.issues[i];
                io.message(std::string("Curve issue: reason=") + curve_plan_reason_name(issue.reason) +
                    "; proven=" + (issue.proven ? "yes" : "no") + "; point=" + std::to_string(issue.ci) +
                    "; voltage=" + std::to_string(issue.voltUv) + " uV; requested=" + std::to_string(issue.targetHz) +
                    " Hz; adjustment=" + std::to_string(issue.adjustmentHz) + " Hz; bound/grid=" + std::to_string(issue.limitHz) +
                    " Hz; worst-point=" + std::to_string(issue.worstCi) + "; worst-voltage=" + std::to_string(issue.worstVoltUv) +
                    " uV; affected=" + std::to_string(issue.affectedCount) + "\n");
            }
        }
        for (int i = 0; i < plan.issueCount; ++i) {
            const auto& issue = plan.issues[i];
            const char* units[] = {"Hz", "uV", "mW", "none"};
            io.message("Control issue: domain=" + profile_command_domains((int)issue.domain) + "; reason=" +
                profile_control_issue_name(issue.kind) + "; requested=" + std::to_string(issue.requested) +
                "; min=" + std::to_string(issue.minimum) + "; max=" + std::to_string(issue.maximum) + "; grid=" + std::to_string(issue.grid) +
                "; unit=" + units[issue.unit] + "\n");
        }
        if (plan.omittedIssues) io.message("Additional control issues: " + std::to_string(plan.omittedIssues) + "\n");
        if (plan.curveChecked && plan.curve.omittedIssues)
            io.message("Additional curve issues: " + std::to_string(plan.curve.omittedIssues) + "\n");
        const char* operation = plan.disposition == PROFILE_CONTROL_REFUSED ? "refused" :
            plan.disposition == PROFILE_CONTROL_UNAVAILABLE ? "unavailable" : "conditional";
        io.message(std::string("Operation feasibility: ") + operation +
            "; fresh ownership, transition and recovery checks are still required before Apply.\n");
    } else {
        if (review) review->error = error;
        io.message(std::string("Profile check: ") + error.message + "\n");
    }
    if (!outputFile.empty() && transformed) {
        bank->documents[selected - 1] = *working;
        std::string outBytes;
        if (!profile_bank_encode(*bank, outBytes, &error) ||
            !profile_check_detail::write_impl(io, outputFile, outBytes, error, 0))
            return reject(PROFILE_COMMAND_IO);
        io.message("Wrote derived profile draft to " + outputFile + " (source file unchanged).\n");
    }
    exitCode = (!review && conversionPlan->requiresDecision()) ? PROFILE_COMMAND_CONVERSION_REQUIRED : PROFILE_COMMAND_DECISION;
    return true;
}
#endif
