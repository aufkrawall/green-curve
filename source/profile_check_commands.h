// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_CHECK_COMMANDS_H
#define GREEN_CURVE_PROFILE_CHECK_COMMANDS_H
#include "profile_commands.h"
#include "profile_runtime_resolution.h"
#include "profile_control_feasibility.h"

// The command deliberately has no write/commit/Apply callback. It parses before
// contacting the existing bound service session, through passive Capture only.
template <class IO>
static inline bool profile_check_commands_run(const std::vector<std::string>& args, IO& io, int& exitCode) {
    bool handled = false;
    for (const auto& arg : args) handled |= arg == "--check-profile";
    if (!handled) return false;
    ProfileCodecError error; std::string file; int selected = 0;
    const auto reject = [&](int code) {
        io.message(std::string("Profile check: ") + error.message + "\n"); exitCode = code; return true;
    };
    for (size_t i = 0; i < args.size(); ++i) {
        const auto& key = args[i];
        if ((key != "--check-profile" && key != "--profile") || ++i == args.size() ||
            args[i].empty() || args[i].size() >= 4096) {
            profile_codec::fail(&error, 0, "use --check-profile FILE [--profile N]"); return reject(PROFILE_COMMAND_INVALID);
        }
        if (key == "--check-profile") {
            if (!file.empty()) { profile_codec::fail(&error, 0, "duplicate profile file"); return reject(PROFILE_COMMAND_INVALID); }
            file = args[i];
        } else if (selected || !profile_codec::ordinal(args[i], selected, 1, PROFILE_BANK_CAPACITY)) {
            profile_codec::fail(&error, 0, "invalid or duplicate profile selection"); return reject(PROFILE_COMMAND_INVALID);
        }
    }
    std::string bytes; auto bank = std::make_unique<ProfileBank>();
    if (!io.read(file, bytes, PROFILE_FILE_MAX_BYTES, false, error)) return reject(PROFILE_COMMAND_IO);
    if (!profile_bank_decode(bytes, *bank, &error)) return reject(PROFILE_COMMAND_INVALID);
    if ((!selected && bank->count != 1) || selected > bank->count) {
        profile_codec::fail(&error, 0, "select one existing bank entry with --profile N"); return reject(PROFILE_COMMAND_INVALID);
    }
    if (!selected) selected = 1;
    const auto& source = bank->documents[selected - 1];
    const char* scopeNames[] = {"update included settings", "replace owned profile", "release owned settings"};
    io.message("Profile " + std::to_string(selected) + ": " + source.displayName + "\nIncluded: " +
        profile_command_domains(source.domains) + "; scope=" + scopeNames[source.applicationScope] + "\n");
    const gc_u32 domains = (gc_u32)source.domains | PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_GRAPHICS_OFFSET | PROFILE_DOMAIN_LOCK;
    auto capture = std::make_unique<HardwareCaptureObservation>();
    if (!io.capture(domains, *capture, error)) return reject(PROFILE_COMMAND_IO);
    ProfileDestinationTopology destination;
    if (capture->requestedDomains != domains ||
        !profile_destination_from_capture(*capture, destination, error.message, sizeof(error.message))) {
        profile_codec::fail(&error, 0, "destination capture is invalid or differs from the request"); return reject(PROFILE_COMMAND_IO);
    }
    ProfileRuntimeRoutes routes;
    routes.memory = capture->memory.valid ? capture->memory.surface : 0;
    if (source.graphicsOffset.present) {
        bool projection = false;
        for (int i = 0; i < source.topologyCount; ++i) projection |= source.points[i].kind == PROFILE_POINT_PROJECTION;
        if (projection || source.graphicsOffset.exclusionPolicy != PROFILE_EXCLUDE_NONE)
            routes.graphics = DESIRED_GRAPHICS_VF_COMPONENT;
        else if (capture->composition == CAPTURE_COMPOSITION_INDEPENDENT && capture->graphics.valid)
            routes.graphics = capture->graphics.surface;
    }
    auto runtime = std::make_unique<DesiredSettings>(); ProfileMappingReport mapping;
    const bool resolved = profile_resolve_runtime(source, destination, 1, routes, *runtime, mapping,
        error.message, sizeof(error.message));
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
    if (!resolved) return reject(PROFILE_COMMAND_DECISION);
    int captured = 0, absolute = 0, projected = 0;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        captured += runtime->curvePointCaptured[i] != 0;
        projected += runtime->hasCurvePoint[i] && runtime->curvePointFromGpuOffset[i];
        absolute += runtime->hasCurvePoint[i] && !runtime->curvePointFromGpuOffset[i];
    }
    io.message("Mapped points: captured=" + std::to_string(captured) + "; absolute=" + std::to_string(absolute) +
        "; projected=" + std::to_string(projected) + "; complete table match=" + (mapping.completeTopologyMatch ? "yes\n" : "no\n"));
    if (runtime->hasGpuOffset) io.message("Graphics: " + std::to_string(desired_graphics_hz(*runtime)) +
        " Hz; control selection=" + (routes.graphics == DESIRED_GRAPHICS_AUTO ? "unresolved\n" : "identified\n"));
    if (runtime->hasMemOffset) io.message("Memory: " + std::to_string(desired_memory_hz(*runtime)) + " physical Hz\n");
    if (runtime->hasPowerLimit) { const auto ratio = desired_power_ratio(*runtime);
        io.message("Power: " + std::to_string(ratio.numerator) + "/" + std::to_string(ratio.denominator) + " of the destination default\n"); }
    if (runtime->hasLock) io.message("Lock: " + std::to_string(desired_lock_hz(*runtime)) + " Hz\n");
    const auto plan = profile_control_plan(*runtime, *capture);
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
    exitCode = PROFILE_COMMAND_DECISION;
    return true;
}
#endif
