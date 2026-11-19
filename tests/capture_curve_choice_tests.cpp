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
bool prepare(const HardwareCaptureObservation& o, ProfileCaptureProposals& out,
             ProfileCaptureProposalContext context = {}) {
    char why[256]{};
    return hardware_capture_observation_valid(o) && profile_capture_prepare_proposals(o, context, out, why, sizeof(why));
}
// A curve flattened the way other tools do it: every point past the anchor
// pushed far below this GPU's reported offset range, so the driver's
// monotonic rule holds the whole tail at the anchor's frequency.
HardwareCaptureObservation pushed_tail_flatten() {
    HardwareCaptureObservation o = curve();
    o.pointCount = 10;
    for (int i = 0; i < 10; ++i) {
        auto& p = o.points[i];
        p = {};
        p.index = 70 + i;
        p.voltageUv = 900000 + i * 6250;
        p.writableState = p.offsetValid = p.frequencyValid = 1;
        p.frequencyHz = i < 3 ? 2392000000LL + i * 30000000LL : 2482000000LL;
        p.offsetHz = i < 3 ? 3199000 - i * 1000000 : i == 3 ? 0 : -2045000000LL;
    }
    refresh(o);
    return o;
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

    // A pushed-tail flatten: the flatten is recommended, and the exact copy,
    // whose tail offsets lie past the range, is labeled as not applicable.
    auto pushed = std::make_unique<HardwareCaptureObservation>(pushed_tail_flatten());
    auto p = std::make_unique<ProfileCaptureProposals>();
    if (!prepare(*pushed, *p) || !p->flatten.selectable() || p->flatten.anchorVoltageUv != 918750 ||
        p->flatten.frequencyHz != 2482000000LL || p->flatten.tailCount != 6 ||
        p->exactControls.disposition != PROFILE_CONTROL_REFUSED || p->suggested != PROFILE_CAPTURE_FLATTEN)
        return 12268;
    if (profile_capture_choice_label(*p, PROFILE_CAPTURE_EXACT).find("cannot be applied") == std::string::npos ||
        profile_capture_choice_label(*baseline, PROFILE_CAPTURE_EXACT) != "Copy every curve point exactly")
        return 12269;
    // Green Curve's own pin at the same frequency describes the combined
    // state and is recommended; a different pin leaves the choice to the user.
    if (!prepare(*pushed, *p, {2482000000LL, true, true}) || !p->pin.selectable() || !p->sameTarget ||
        p->suggested != PROFILE_CAPTURE_PIN)
        return 12270;
    if (!prepare(*pushed, *p, {2000000000LL, true, true}) || !p->conflictingTargets ||
        p->suggested != PROFILE_CAPTURE_EXACT)
        return 12271;
    // A workload observation is never recommended, even when it agrees.
    auto observed = std::make_unique<ProfileCaptureProposals>(*p);
    observed->pin.provenance = PROFILE_LOCK_PROVENANCE_USER_CONFIRMED_OBSERVATION;
    observed->pin.frequencyHz = observed->flatten.frequencyHz;
    observed->sameTarget = true; observed->conflictingTargets = false;
    if (profile_capture_suggest(*observed) != PROFILE_CAPTURE_EXACT) return 12272;
    char exactLine[512]{};
    profile_capture_exact_log_line(*p, exactLine, sizeof(exactLine));
    if (std::string(exactLine).find("profile-capture: proposal=exact disposition=refused suggested=exact") != 0)
        return 12272;
    return 0;
}
