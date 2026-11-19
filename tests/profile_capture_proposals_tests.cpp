// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "profile_capture_commands.h"
#include "profile_capture_proposal_fixture.h"
#include "profile_review_fixture.h"
#include <climits>

namespace {
using profile_capture_proposal_fixture::curve;

struct IO {
    HardwareCaptureObservation observation = curve();
    ProfileCaptureProposalContext context{};
    int captures = 0, writes = 0;
    std::string output = "previous", messages;
    bool distinct(const std::string &a, const std::string &b, ProfileCodecError &) { return a != b; }
    bool capture(gc_u32 domains, HardwareCaptureObservation &out, ProfileCodecError &) {
        ++captures;
        if (domains != observation.requestedDomains)
            return false;
        out = observation;
        return true;
    }
    ProfileCaptureProposalContext capture_proposal_context() const { return context; }
    bool write(const std::string &, const std::string &bytes, ProfileCodecError &) {
        ++writes;
        output = bytes;
        return true;
    }
    void message(const std::string &text) { messages += text; }
};
} // namespace
int run_profile_capture_proposals_tests() {
    auto o = std::make_unique<HardwareCaptureObservation>(curve());
    const auto original = *o;
    auto proposals = std::make_unique<ProfileCaptureProposals>();
    char why[256]{};
    if (!profile_capture_prepare_proposals(*o, {}, *proposals, why, sizeof(why)) || !proposals->flatten.selectable() ||
        proposals->flatten.anchorVoltageUv != 662000 || proposals->flatten.frequencyHz != 1000000000 ||
        proposals->flatten.tailCount != 4 || proposals->flatten.maximumDeviationHz != 1000 ||
        proposals->pin.recognized || proposals->suggested != PROFILE_CAPTURE_FLATTEN ||
        memcmp(o.get(), &original, sizeof(original)))
        return 9680;
    auto selected = std::make_unique<ProfileDocument>();
    if (!profile_capture_select_proposal(*proposals, PROFILE_CAPTURE_FLATTEN, *selected, why, sizeof(why)) ||
        !selected->lock.present || selected->lock.mode != LOCK_MODE_FLATTEN || selected->lock.anchorTopologyId != 22 ||
        selected->lock.provenance != PROFILE_LOCK_PROVENANCE_RECOGNIZED_FLATTEN ||
        !selected->lock.hasRecognitionEvidence || selected->lock.recognizedTailCount != 4 ||
        selected->lock.recognitionToleranceHz != 1000000 || selected->source.captureComplete ||
        selected->source.pinKnowledge != PROFILE_PIN_UNKNOWN ||
        selected->points[0].kind != PROFILE_POINT_CAPTURED_OFFSET || selected->points[0].capturedOffsetHz ||
        selected->points[1].kind != PROFILE_POINT_ABSENT || selected->points[1].capturedOffsetHz ||
        !selected->points[1].hasObservedOffset || selected->points[1].observedOffsetHz != -100000500)
        return 9681;
    auto bank = std::make_unique<ProfileBank>();
    bank->count = 1;
    bank->documents[0] = *selected;
    std::string encoded;
    ProfileCodecError error;
    if (!profile_bank_encode(*bank, encoded, &error) || !profile_bank_decode(encoded, *bank, &error) ||
        bank->documents[0].lock.provenance != PROFILE_LOCK_PROVENANCE_RECOGNIZED_FLATTEN ||
        bank->documents[0].points[5].observedOffsetHz != -500000500)
        return 9682;
    if (!profile_capture_select_proposal(*proposals, PROFILE_CAPTURE_EXACT, *selected, why, sizeof(why)) ||
        selected->lock.present || selected->points[1].kind != PROFILE_POINT_CAPTURED_OFFSET ||
        selected->points[1].capturedOffsetHz != -100000500)
        return 9683;
    // Exact replay remains a separate immutable choice, including owned zero.
    if (profile_capture_select_proposal(*proposals, PROFILE_CAPTURE_PIN, *selected, why, sizeof(why)) ||
        selected->lock.present || selected->points[1].capturedOffsetHz != -100000500)
        return 9684;
    if (!profile_capture_prepare_proposals(*o, {1000000000, true, true}, *proposals, why, sizeof(why)) ||
        !proposals->sameTarget || proposals->conflictingTargets || !proposals->pin.selectable() ||
        proposals->pin.document.lock.provenance != PROFILE_LOCK_PROVENANCE_APPLIED_INTENT ||
        proposals->pin.document.lock.hasObservedPinRange ||
        proposals->pin.document.source.pinKnowledge != PROFILE_PIN_UNKNOWN)
        return 9685;
    if (!profile_capture_prepare_proposals(*o, {900000000, true, true}, *proposals, why, sizeof(why)) ||
        proposals->sameTarget || !proposals->conflictingTargets || proposals->pin.document.lock.anchorTopologyId != 21)
        return 9686;
    if (!profile_capture_prepare_proposals(*o, {1000000000, true, false}, *proposals, why, sizeof(why)) ||
        !proposals->pin.recognized || proposals->pin.selectable() || !proposals->flatten.selectable())
        return 9687;
    o->pinGetterValid = o->pinPresent = 1;
    o->pinMinimumHz = o->pinMaximumHz = 1000000000;
    hardware_capture_domain(*o, PROFILE_DOMAIN_LOCK, CAPTURE_COMPLETE);
    if (!profile_capture_prepare_proposals(*o, {900000000, true, true}, *proposals, why, sizeof(why)) ||
        !proposals->pin.selectable() || proposals->pin.provenance != PROFILE_LOCK_PROVENANCE_DRIVER_GETTER ||
        !proposals->sameTarget || !proposals->pin.document.lock.hasObservedPinRange)
        return 9688;
    o->pinMinimumHz = 900000000;
    if (!profile_capture_prepare_proposals(*o, {1000000000, true, true}, *proposals, why, sizeof(why)) ||
        proposals->pin.recognized)
        return 9689;
    o->pinPresent = 0;
    o->pinMinimumHz = o->pinMaximumHz = 0;
    if (!profile_capture_prepare_proposals(*o, {1000000000, true, true}, *proposals, why, sizeof(why)) ||
        proposals->pin.recognized || proposals->exact.source.pinKnowledge != PROFILE_PIN_ABSENT)
        return 9690;
    o->pinPresent = 1;
    o->pinMinimumHz = o->pinMaximumHz = 999500000;
    if (!profile_capture_prepare_proposals(*o, {0, true, true}, *proposals, why, sizeof(why)) ||
        !proposals->pin.recognized || proposals->pin.controls.disposition != PROFILE_CONTROL_REFUSED ||
        proposals->pin.selectable())
        return 9707;
    *o = original;
    o->points[0].writableState = 2;
    if (!profile_capture_prepare_proposals(*o, {}, *proposals, why, sizeof(why)) ||
        proposals->flatten.anchorVoltageUv != 662000 || proposals->exact.points[0].kind != PROFILE_POINT_ABSENT ||
        !proposals->exact.points[0].hasObservedOffset)
        return 9691;
    o->points[2].writableState = 2;
    if (!profile_capture_prepare_proposals(*o, {}, *proposals, why, sizeof(why)) || proposals->flatten.recognized)
        return 9692;
    *o = original;
    std::reverse(o->points, o->points + o->pointCount);
    if (!profile_capture_prepare_proposals(*o, {}, *proposals, why, sizeof(why)) || !proposals->flatten.selectable() ||
        proposals->flatten.document.lock.anchorTopologyId != 22 || proposals->exact.topology[0].topologyId != 26)
        return 9693;
    *o = original;
    o->points[2].voltageUv = o->points[1].voltageUv;
    if (!profile_capture_prepare_proposals(*o, {}, *proposals, why, sizeof(why)) || proposals->flatten.recognized)
        return 9694;
    *o = original;
    o->points[1].frequencyHz = LLONG_MAX;
    if (!profile_capture_prepare_proposals(*o, {}, *proposals, why, sizeof(why)) || proposals->flatten.recognized ||
        proposals->exact.points[1].observedFrequencyHz != LLONG_MAX)
        return 9695;
    *o = original;
    o->vfLimits.gridValid = 0;
    o->vfLimits.gridHz = 0;
    if (!profile_capture_prepare_proposals(*o, {}, *proposals, why, sizeof(why)) || !proposals->flatten.recognized ||
        proposals->flatten.selectable())
        return 9696;
    *o = original;
    o->vfLimits.minimumHz = 0;
    if (!profile_capture_prepare_proposals(*o, {}, *proposals, why, sizeof(why)) ||
        proposals->flatten.controls.disposition != PROFILE_CONTROL_REFUSED || proposals->flatten.selectable())
        return 9708;
    *o = original;
    o->points[0].writableState = 2;
    o->points[0].frequencyHz = 1050000000;
    if (!profile_capture_prepare_proposals(*o, {}, *proposals, why, sizeof(why)) ||
        proposals->flatten.controls.disposition != PROFILE_CONTROL_CONDITIONAL || !proposals->flatten.selectable())
        return 9710;
    const auto *conflict = curve_plan_lead_issue(proposals->flatten.controls.curve);
    if (!conflict || conflict->proven || conflict->ci != 21)
        return 9711;
    *o = original;
    o->pointCount = 128;
    o->backendFamily = 0;
    o->vfLimits.rangeReported = 0;
    o->vfLimits.rangeSource = VF_OFFSET_RANGE_SOURCE_FALLBACK;
    for (int i = 0; i < 128; ++i) {
        auto &point = o->points[i];
        point = {};
        point.index = i;
        point.voltageUv = 500000 + i * 10000;
        point.offsetValid = point.frequencyValid = 1;
        point.writableState = i < 10 ? 2 : 1;
        point.frequencyHz = i < 10 ? 180000000 : 1000000000;
    }
    if (!profile_capture_prepare_proposals(*o, {}, *proposals, why, sizeof(why)) || !proposals->flatten.selectable() ||
        proposals->flatten.anchorVoltageUv != 600000 || proposals->flatten.tailCount != 117 ||
        proposals->suggested != PROFILE_CAPTURE_FLATTEN ||
        proposals->exact.topologyCount != 128 || proposals->exact.points[127].kind != PROFILE_POINT_CAPTURED_OFFSET)
        return 9712;
    *o = original;
    o->pointCount = 129;
    selected->documentId = 17;
    if (profile_capture_prepare_proposals(*o, {}, *proposals, why, sizeof(why)))
        return 9697;
    auto response = std::make_unique<ServiceResponse>();
    profile_review_fixture::live(*response);
    response->capture = original;
    response->capture.binding = hardware_capture_binding(response->snapshot.adapters[0]);
    response->state.activeDesiredValid = 1;
    response->desired.hasLock = 1;
    response->desired.lockMode = LOCK_MODE_HARD;
    response->desired.lockCi = 22;
    response->desired.lockMHz = 1000;
    response->capture.fingerprint = hardware_capture_fingerprint(response->capture);
    response->capture.revision = service_control_review_revision(response->capture.fingerprint, &response->desired);
    auto context = profile_capture_proposal_context(*response);
    if (context.ownedPinHz != 1000000000 || !context.pinAvailabilityKnown || !context.pinAvailable)
        return 9698;
    response->snapshot = {};
    response->state.validSections = SERVICE_STATE_SECTION_ACTIVE_INTENT;
    response->state.gpuPhase = SERVICE_GPU_PHASE_DEGRADED;
    context = profile_capture_proposal_context(*response);
    if (context.ownedPinHz != 1000000000 || context.pinAvailabilityKnown)
        return 9699;
    ++response->desired.lockMHz;
    if (profile_capture_proposal_context(*response).ownedPinHz)
        return 9700;
    --response->desired.lockMHz;
    ++response->state.gpuGeneration;
    if (profile_capture_proposal_context(*response).ownedPinHz)
        return 9701;
    IO io;
    int code = -1;
    const std::vector<std::string> review{"--capture-current", "capture.gcprofile", "--domains", "vf,graphics,pin",
                                          "--allow-partial"};
    if (!profile_capture_commands_run(review, "config.ini", io, code) || code != PROFILE_COMMAND_DECISION ||
        io.writes || io.messages.find("anchor=662000 uV") == std::string::npos ||
        io.messages.find("Pin knowledge: unknown") == std::string::npos)
        return 9702;
    auto flattened = review;
    flattened.insert(flattened.end(), {"--representation", "flatten"});
    if (!profile_capture_commands_run(flattened, "config.ini", io, code) || code != PROFILE_COMMAND_OK ||
        io.writes != 1 || !profile_bank_decode(io.output, *bank, &error) || !bank->documents[0].lock.present ||
        bank->documents[0].points[1].observedOffsetHz != -100000500 ||
        bank->documents[0].points[1].kind != PROFILE_POINT_ABSENT)
        return 9703;
    auto pinned = review;
    pinned.insert(pinned.end(), {"--representation", "pin"});
    const auto prior = io.output;
    if (!profile_capture_commands_run(pinned, "config.ini", io, code) || code != PROFILE_COMMAND_DECISION ||
        io.writes != 1 || io.output != prior)
        return 9704;
    io.context = {1000000000, true, true};
    if (!profile_capture_commands_run(pinned, "config.ini", io, code) || code != PROFILE_COMMAND_OK || io.writes != 2 ||
        !profile_bank_decode(io.output, *bank, &error) || bank->documents[0].lock.mode != LOCK_MODE_HARD ||
        bank->documents[0].lock.provenance != PROFILE_LOCK_PROVENANCE_APPLIED_INTENT ||
        io.messages.find("not read from driver") == std::string::npos)
        return 9705;
    auto strict = pinned;
    strict.erase(std::find(strict.begin(), strict.end(), "--allow-partial"));
    const auto savedPin = io.output;
    if (!profile_capture_commands_run(strict, "config.ini", io, code) || code != PROFILE_COMMAND_DECISION ||
        io.writes != 2 || io.output != savedPin)
        return 9713;
    io.context.pinAvailable = false;
    if (!profile_capture_commands_run(pinned, "config.ini", io, code) || code != PROFILE_COMMAND_DECISION ||
        io.writes != 2 || io.output != savedPin)
        return 9714;
    IO exact;
    exact.context = {1000000000, true, true};
    auto raw = review;
    raw.insert(raw.end(), {"--representation", "exact"});
    if (!profile_capture_commands_run(raw, "config.ini", exact, code) || code != PROFILE_COMMAND_OK ||
        exact.writes != 1 || !profile_bank_decode(exact.output, *bank, &error) || bank->documents[0].lock.present ||
        bank->documents[0].points[1].kind != PROFILE_POINT_CAPTURED_OFFSET ||
        bank->documents[0].points[1].capturedOffsetHz != -100000500)
        return 9715;
    const int reads = io.captures;
    pinned.insert(pinned.end(), {"--representation", "exact"});
    if (!profile_capture_commands_run(pinned, "config.ini", io, code) || code != PROFILE_COMMAND_INVALID ||
        io.captures != reads || io.writes != 2)
        return 9706;
    return 0;
}
