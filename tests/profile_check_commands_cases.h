// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
static int profile_check_commands_cases() {
    CaptureIO io; int code = -1;
    if (!profile_capture_commands_run({"--capture-current", "review.gcprofile", "--allow-partial", "--representation", "exact"},
            "config.ini", io, code) || code) return 8700;
    const auto files = io.files;
    io.messages.clear();
    if (!profile_check_commands_run({"--check-profile", "review.gcprofile"}, io, code) || code != PROFILE_COMMAND_DECISION ||
        io.writes != 1 || io.files != files || io.messages.find("captured=3") == std::string::npos ||
        io.messages.find("-500000 physical Hz") == std::string::npos || io.messages.find("249/250") == std::string::npos ||
        io.messages.find("still required before Apply") == std::string::npos) return 8701;
    io.readOnly = true; io.messages.clear();
    if (!profile_check_commands_run({"--check-profile", "review.gcprofile"}, io, code) || code != PROFILE_COMMAND_DECISION ||
        io.messages.find("read-only point") == std::string::npos || io.writes != 1) return 8702;
    io.readOnly = false;
    const int before = io.captures;
    for (const auto& args : std::vector<std::vector<std::string>>{
            {"--check-profile"}, {"--check-profile", "x", "--apply-config"},
            {"--check-profile", "x", "--profile", "0"}, {"--check-profile", "x", "--profile", "1", "--profile", "1"},
            {"--check-profile", "x", "--capture-current", "y"}})
        if (!profile_check_commands_run(args, io, code) || code != PROFILE_COMMAND_INVALID || io.captures != before) return 8703;
    io.files["bad"] = "invalid portable file";
    if (!profile_check_commands_run({"--check-profile", "bad"}, io, code) || code != PROFILE_COMMAND_INVALID ||
        io.captures != before || io.writes != 1) return 8704;
    auto bank = std::make_unique<ProfileBank>(); ProfileCodecError error;
    if (!profile_bank_decode(files.at("review.gcprofile"), *bank, &error)) return 8705;
    auto& d = bank->documents[0]; d.points[1] = {};
    d.points[1].kind = PROFILE_POINT_ABSOLUTE_FREQUENCY; d.points[1].absoluteFrequencyHz = 1000000500;
    d.points[2] = {}; d.domains |= PROFILE_DOMAIN_LOCK;
    d.lock.present = true; d.lock.mode = LOCK_MODE_FLATTEN; d.lock.anchorTopologyId = d.topology[2].topologyId;
    d.lock.targetFrequencyHz = 1100000500;
    HardwareCaptureObservation observation{};
    if (!io.capture((gc_u32)d.domains | PROFILE_DOMAIN_GRAPHICS_OFFSET, observation, error)) return 8706;
    ProfileDestinationTopology destination;
    if (!profile_destination_from_capture(observation, destination, error.message, sizeof(error.message))) return 8707;
    DesiredSettings runtime{}; ProfileMappingReport mapping;
    if (!profile_resolve_runtime(d, destination, 2, {}, runtime, mapping, error.message, sizeof(error.message)) ||
        runtime.curvePointCapturedOffsetHz[22] || !runtime.curvePointCaptured[22] ||
        desired_absolute_hz(runtime, 23) != 1000000500 || desired_lock_hz(runtime) != 1100000500 ||
        desired_memory_hz(runtime) != -500000 || desired_power_ratio(runtime).numerator != 249 ||
        !runtime.authoring.topologyBound || runtime.authoring.voltageUv[22] != 662000) return 8708;
    const DesiredSettings preserved = runtime;
    d.points[1].absoluteFrequencyHz = LLONG_MAX;
    if (profile_resolve_runtime(d, destination, 2, {}, runtime, mapping, error.message, sizeof(error.message)) ||
        !desired_authoring_equal(runtime.authoring, preserved.authoring) ||
        d.points[1].absoluteFrequencyHz != LLONG_MAX) return 8709;
    d.points[1].absoluteFrequencyHz = 1000000500;
    d.hasSysClkOffsetHz = true; d.sysClkOffsetHz = -500; d.domains |= PROFILE_DOMAIN_SYS_CLK;
    if (profile_resolve_runtime(d, destination, 2, {}, runtime, mapping, error.message, sizeof(error.message)) ||
        !strstr(error.message, "explicit grid conversion")) return 8710;
    if (profile_check_commands_run({"--help"}, io, code)) return 8711;
    d.hasSysClkOffsetHz = false; d.domains &= ~PROFILE_DOMAIN_SYS_CLK;
    d.graphicsOffset.present = true; d.domains |= PROFILE_DOMAIN_GRAPHICS_OFFSET;
    d.graphicsOffset.offsetHz = -15000500; d.graphicsOffset.exclusionPolicy = PROFILE_EXCLUDE_IDENTITIES;
    d.graphicsOffset.excludedCount = 1; d.graphicsOffset.excludedTopologyIds[0] = d.topology[0].topologyId;
    ProfileRuntimeRoutes routes; routes.graphics = DESIRED_GRAPHICS_VF_COMPONENT;
    if (!profile_resolve_runtime(d, destination, 2, routes, runtime, mapping, error.message, sizeof(error.message)) ||
        desired_graphics_hz(runtime) != -15000500 || !runtime.authoring.graphicsExcluded[22] ||
        runtime.authoring.graphicsExcluded[23] || runtime.authoring.graphicsSurface != DESIRED_GRAPHICS_VF_COMPONENT) return 8712;
    routes.graphics = DESIRED_GRAPHICS_P0;
    if (profile_resolve_runtime(d, destination, 2, routes, runtime, mapping, error.message, sizeof(error.message)) ||
        !strstr(error.message, "VF component")) return 8713;
    bank->count = 2; bank->documents[1] = bank->documents[0]; bank->documents[1].documentId++;
    if (!profile_bank_encode(*bank, io.files["bank.gcprofile"], &error)) return 8714;
    const int beforeBank = io.captures;
    if (!profile_check_commands_run({"--check-profile", "bank.gcprofile"}, io, code) || code != PROFILE_COMMAND_INVALID ||
        io.captures != beforeBank) return 8715;
    if (!profile_check_commands_run({"--check-profile", "bank.gcprofile", "--profile", "2"}, io, code) ||
        code != PROFILE_COMMAND_DECISION || io.captures != beforeBank + 1 || io.writes != 1) return 8716;
    const auto unchanged = io.files;
    io.failCapture = true;
    if (!profile_check_commands_run({"--check-profile", "review.gcprofile"}, io, code) || code != PROFILE_COMMAND_IO ||
        io.files != unchanged || io.writes != 1) return 8717;
    auto memory = profile_document_make_empty(); memory.domains = PROFILE_DOMAIN_MEMORY;
    memory.hasMemoryOffset = true; memory.memoryOffsetHz = -500000; memory.pointSetPolicy = PROFILE_POINTS_REPLACE;
    if (!profile_resolve_runtime(memory, destination, 2, {}, runtime, mapping, error.message, sizeof(error.message)) ||
        (runtime.applicationScope & DESIRED_SCOPE_REPLACE_VF_POINTS) || mapping.needsCompleteCurvePlan) return 8732;
    auto equivalent = memory; equivalent.pointSetPolicy = PROFILE_POINTS_MERGE;
    if (!profile_document_semantically_equal(memory, equivalent)) return 8733;
    DesiredSettings previous{}; previous.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED;
    previous.curvePointCaptured[22] = 1; previous.curvePointCapturedOffsetHz[22] = -500;
    ServiceScopedApplyPlan scoped{};
    if (!service_build_scoped_apply_plan(&previous, runtime, nullptr, {}, false, &scoped, error.message, sizeof(error.message)) ||
        !scoped.owned.curvePointCaptured[22] || scoped.hardware.curvePointCaptured[22] || scoped.cleanupPoints) return 8734;
    io.failCapture = false; io.constraints = true; io.messages.clear();
    if (!profile_check_commands_run({"--check-profile", "review.gcprofile"}, io, code) || code != PROFILE_COMMAND_DECISION ||
        io.messages.find("Control feasibility: disposition=representable") == std::string::npos ||
        io.messages.find("Operation feasibility: conditional") == std::string::npos || io.files != unchanged || io.writes != 1) return 8941;
    io.vfGrid = 1000; io.messages.clear();
    if (!profile_check_commands_run({"--check-profile", "review.gcprofile"}, io, code) || code != PROFILE_COMMAND_DECISION ||
        io.messages.find("Control feasibility: disposition=refused") == std::string::npos ||
        io.messages.find("reason=point-grid; proven=yes") == std::string::npos ||
        io.messages.find("Operation feasibility: refused") == std::string::npos ||
        io.messages.find("worst-point=") == std::string::npos || io.files != unchanged || io.writes != 1) return 8942;
    return 0;
}
