// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Which curve representations a capture offers. Exact, flatten and pin carry
// every non-curve setting identically, so only the curve, the core offset and
// the lock may decide whether flatten or pin is offered (12260-12264).
#include "profile_capture_proposals.h"
#include "profile_capture_proposal_fixture.h"
#include "profile_summary.h"
#include <memory>
#include <string>

namespace {
using profile_capture_proposal_fixture::curve;

// The advanced clocks as an older service reported them: read, but with no
// setter flag, which the control planner treats as an unavailable control.
void add_unflagged_advanced_clocks(HardwareCaptureObservation& o) {
    o.requestedDomains |= PROFILE_DOMAIN_XBAR | PROFILE_DOMAIN_SYS_CLK | PROFILE_DOMAIN_VIDEO_CLK;
    hardware_capture_domain(o, PROFILE_DOMAIN_XBAR, CAPTURE_COMPLETE);
    hardware_capture_domain(o, PROFILE_DOMAIN_SYS_CLK, CAPTURE_COMPLETE);
    hardware_capture_domain(o, PROFILE_DOMAIN_VIDEO_CLK, CAPTURE_COMPLETE);
    HardwareCaptureScalar* scalars[] = {&o.xbarClock, &o.systemClock, &o.videoClock};
    for (auto* s : scalars) { s->valid = 1; s->grid = 1000; s->writable = 0; }
    o.xbarVoltage.valid = 1; o.xbarVoltage.grid = 1; o.xbarVoltage.writable = 0;
}
// A memory offset outside its reported range: exact replay would be refused
// for memory too, and the user can leave the row out.
void add_refused_memory(HardwareCaptureObservation& o) {
    o.requestedDomains |= PROFILE_DOMAIN_MEMORY;
    hardware_capture_domain(o, PROFILE_DOMAIN_MEMORY, CAPTURE_COMPLETE);
    o.memory.valid = o.memory.writable = o.memory.rangeValid = 1;
    o.memory.surface = CLOCK_OFFSET_SURFACE_P0;
    o.memory.grid = 500000;
    o.memory.minimum = -1000000000; o.memory.maximum = 1000000000;
    o.memory.value = 2000000000;
}
void refresh(HardwareCaptureObservation& o) {
    o.fingerprint = hardware_capture_fingerprint(o);
    o.revision = service_control_review_revision(o.fingerprint, nullptr);
}
bool prepare(const HardwareCaptureObservation& o, ProfileCaptureProposals& out) {
    char why[256]{};
    return hardware_capture_observation_valid(o) && profile_capture_prepare_proposals(o, {}, out, why, sizeof(why));
}
}  // namespace

int run_capture_curve_choice_tests() {
    auto baseline = std::make_unique<ProfileCaptureProposals>();
    if (!prepare(curve(), *baseline) || !baseline->flatten.selectable()) return 12260;

    // Unflagged advanced clocks used to make the whole flatten proposal
    // "unavailable", so the dialog offered only an exact copy.
    auto o = std::make_unique<HardwareCaptureObservation>(curve());
    add_unflagged_advanced_clocks(*o);
    refresh(*o);
    auto withAdvanced = std::make_unique<ProfileCaptureProposals>();
    if (!prepare(*o, *withAdvanced) || !withAdvanced->flatten.selectable() ||
        withAdvanced->flatten.controls.disposition != baseline->flatten.controls.disposition ||
        !(profile_capture_available_representations(*withAdvanced) &
          profile_capture_representation_bit(PROFILE_CAPTURE_FLATTEN)))
        return 12261;
    // Judging the curve alone never removes a domain from what is saved.
    if (!(withAdvanced->flatten.document.domains & PROFILE_DOMAIN_XBAR) ||
        !(withAdvanced->flatten.document.domains & PROFILE_DOMAIN_SYS_CLK) ||
        !(withAdvanced->flatten.document.domains & PROFILE_DOMAIN_VIDEO_CLK) ||
        !withAdvanced->flatten.document.hasXbarOffsetHz)
        return 12262;

    add_refused_memory(*o);
    refresh(*o);
    auto withMemory = std::make_unique<ProfileCaptureProposals>();
    if (!prepare(*o, *withMemory) || !withMemory->flatten.selectable() ||
        withMemory->flatten.controls.disposition != baseline->flatten.controls.disposition ||
        !withMemory->flatten.document.hasMemoryOffset || withMemory->flatten.document.memoryOffsetHz != 2000000000)
        return 12263;

    // The summary row names the recognized flatten once it is offered.
    const auto summary = profile_capture_summary(*o, withMemory.get());
    if (summary.curve.text.find("Flat at 1000 MHz") == std::string::npos) return 12264;

    // The debug line says whether each choice is offered and why.
    char line[512]{};
    profile_capture_proposal_log_line(withMemory->flatten, "flatten", line, sizeof(line));
    const std::string text = line;
    if (text.find("profile-capture: proposal=flatten recognized=1 offered=1") != 0 ||
        text.find("anchorUv=662000") == std::string::npos || text.find('\n') != std::string::npos)
        return 12264;
    return 0;
}
