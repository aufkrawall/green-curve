// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Tier 3 workload pin observation regressions (CAP-11), codes 9900-9919.
#include "profile_capture_proposals.h"
#include "profile_codec.h"
#include <memory>
#include <string.h>

namespace {

HardwareCaptureObservation pin_obs_fixture_capture() {
    HardwareCaptureObservation o{};
    o.valid = o.binding.valid = o.topologyComplete = 1;
    o.schemaVersion = HARDWARE_CAPTURE_SCHEMA_CURRENT;
    o.gpuGeneration = 7;
    o.requestedDomains = PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_GRAPHICS_OFFSET | PROFILE_DOMAIN_LOCK;
    o.osFamily = PROFILE_SOURCE_OS_WINDOWS;
    o.composition = CAPTURE_COMPOSITION_INDEPENDENT;
    for (int i = 0; i < 9; ++i)
        if (o.requestedDomains & (1u << i)) hardware_capture_domain(o, 1u << i, CAPTURE_UNSUPPORTED, -100001);
    hardware_capture_domain(o, PROFILE_DOMAIN_VF_CURVE, CAPTURE_COMPLETE);
    hardware_capture_domain(o, PROFILE_DOMAIN_GRAPHICS_OFFSET, CAPTURE_COMPLETE);
    o.graphics.valid = o.graphics.writable = 1;
    o.graphics.grid = 1000000;
    o.graphics.surface = CLOCK_OFFSET_SURFACE_P0;
    o.vfLimits = {1, 1, 1, 1, VF_OFFSET_RANGE_SOURCE_NVML_GRAPHICS, -1000000000LL, 1000000000LL, 1000};
    o.power.defaultValid = 1; o.power.defaultMw = 250000;
    o.pointCount = 5;
    const gc_i64 freqs[5] = {1200000000LL, 1750000000LL, 2000000000LL, 2250000000LL, 2500000000LL};
    for (gc_u32 i = 0; i < 5; ++i) {
        auto& p = o.points[i];
        p.index = 20 + i;
        p.voltageUv = 700000 + (gc_i64)i * 100000; // 700, 800, 900, 1000, 1100 mV
        p.writableState = 1;
        p.offsetValid = p.frequencyValid = 1;
        p.frequencyHz = freqs[i];
        p.offsetHz = 0;
    }
    o.fingerprint = hardware_capture_fingerprint(o);
    o.revision = service_control_review_revision(o.fingerprint, nullptr);
    return o;
}

} // namespace

int run_profile_pin_observation_tests() {
    auto cap = pin_obs_fixture_capture();
    auto proposals = std::make_unique<ProfileCaptureProposals>();
    auto doc = std::make_unique<ProfileDocument>();
    auto bank = std::make_unique<ProfileBank>();
    char why[256]{};
    PinObservationResult res{};

    // CAP-11: absent getter passive prepare leaves PIN unknown & unselectable; never auto-runs workload sampling.
    ProfileCaptureProposalContext ctx{0, true, true};
    if (!profile_capture_prepare_proposals(cap, ctx, *proposals, why, sizeof(why)) ||
        proposals->pin.recognized || proposals->pin.selectable() ||
        proposals->exact.source.pinKnowledge != PROFILE_PIN_UNKNOWN) return 9900;

    // CAP-11: idle observation (GpuIdle flag, low util, P8 state, or utilValid=false without P0+power) yields UNKNOWN.
    PinWorkloadSample idleGpuIdle[4]{};
    for (auto& s : idleGpuIdle) {
        s.clockValid = true; s.graphicsClockHz = 210000000LL;
        s.utilValid = true; s.utilGpuPct = 5;
        s.throttleValid = true; s.throttleReasons = PIN_OBS_REASON_GPU_IDLE;
    }
    profile_pin_observation_analyze(cap, false, idleGpuIdle, 4, res);
    if (res.status != PIN_OBS_IDLE || res.proposedPinHz != 0 ||
        profile_capture_apply_pin_observation(cap, ctx, res, *proposals) ||
        proposals->pin.recognized || proposals->pin.selectable()) return 9901;

    PinWorkloadSample lowUtil[4]{};
    for (auto& s : lowUtil) {
        s.clockValid = true; s.graphicsClockHz = 1200000000LL;
        s.utilValid = true; s.utilGpuPct = 10;
    }
    profile_pin_observation_analyze(cap, false, lowUtil, 4, res);
    if (res.status != PIN_OBS_IDLE) return 9902;

    PinWorkloadSample p8Idle[4]{};
    for (auto& s : p8Idle) {
        s.clockValid = true; s.graphicsClockHz = 800000000LL;
        s.utilValid = true; s.utilGpuPct = 50;
        s.perfStateValid = true; s.perfState = 8u;
    }
    profile_pin_observation_analyze(cap, false, p8Idle, 4, res);
    if (res.status != PIN_OBS_IDLE) return 9903;

    PinWorkloadSample unprovenUtil[4]{};
    for (auto& s : unprovenUtil) { s.clockValid = true; s.graphicsClockHz = 1700000000LL; }
    profile_pin_observation_analyze(cap, false, unprovenUtil, 4, res);
    if (res.status != PIN_OBS_IDLE) return 9904;

    // Insufficient sample count yields idle/unknown (< 3 active samples).
    PinWorkloadSample tooFew[2]{};
    for (auto& s : tooFew) { s.clockValid = true; s.graphicsClockHz = 1700000000LL; s.utilValid = true; s.utilGpuPct = 95; }
    profile_pin_observation_analyze(cap, false, tooFew, 2, res);
    if (res.status != PIN_OBS_IDLE) return 9905;

    // Fluctuating clock under load (> 15 MHz spread) yields NOT_PINNED_VARYING (unknown).
    PinWorkloadSample varying[4]{};
    const gc_i64 varFreqs[4] = {1700000000LL, 1750000000LL, 1800000000LL, 1900000000LL};
    for (int i = 0; i < 4; ++i) {
        varying[i].clockValid = true; varying[i].graphicsClockHz = varFreqs[i];
        varying[i].utilValid = true; varying[i].utilGpuPct = 95;
    }
    profile_pin_observation_analyze(cap, false, varying, 4, res);
    if (res.status != PIN_OBS_NOT_PINNED_VARYING ||
        profile_capture_apply_pin_observation(cap, ctx, res, *proposals) || proposals->pin.recognized) return 9906;

    // Operating voltage curve allowance check: at 900 mV, captured curve allows 2000 MHz (peak is 2500 MHz).
    // Sustained 1990 MHz at 900 mV is within 30 MHz margin -> AT_CURVE (no false pin).
    PinWorkloadSample atVoltageCurve[4]{};
    for (auto& s : atVoltageCurve) {
        s.clockValid = true; s.graphicsClockHz = 1990000000LL;
        s.utilValid = true; s.utilGpuPct = 95;
        s.voltageValid = true; s.operatingVoltageUv = 900000;
    }
    profile_pin_observation_analyze(cap, false, atVoltageCurve, 4, res);
    if (res.status != PIN_OBS_AT_CURVE || res.curveAllowedHz != 2000000000LL ||
        profile_capture_apply_pin_observation(cap, ctx, res, *proposals)) return 9907;

    // Sustained 1700 MHz at 900 mV + limit reasons proposes suspected pin (bounded at max 16 samples);
    // never modifies suggested representation until user confirms.
    PinWorkloadSample pinnedSamples[20]{};
    for (int i = 0; i < 20; ++i) {
        pinnedSamples[i].clockValid = true;
        pinnedSamples[i].graphicsClockHz = 1700000000LL + (i % 3) * 2000000LL; // 1700..1704, median 1702
        pinnedSamples[i].utilValid = true; pinnedSamples[i].utilGpuPct = 96;
        pinnedSamples[i].voltageValid = true; pinnedSamples[i].operatingVoltageUv = 900000;
        pinnedSamples[i].throttleValid = true;
        pinnedSamples[i].throttleReasons = PIN_OBS_REASON_APPLICATIONS_CLOCKS | PIN_OBS_REASON_SW_POWER_CAP;
    }
    profile_pin_observation_analyze(cap, false, pinnedSamples, 20, res);
    if (res.status != PIN_OBS_SUSPECTED_PIN || res.totalSamples != 16 || res.activeSamples != 16 ||
        res.curveAllowedHz != 2000000000LL || res.proposedPinHz != 1702000000LL ||
        !res.hasThermalOrPowerThrottle || !res.hasAppClockLimit ||
        !strstr(res.limitReasonsText, "app-clocks") || !strstr(res.limitReasonsText, "sw-power-cap") ||
        !strstr(res.explanation, "WARNING")) return 9908;

    // User confirms proposed pin -> provenance USER_CONFIRMED_OBSERVATION, lowest writable anchor >= 1702 MHz (pt 21 @ 1750 MHz).
    const auto beforeSuggested = proposals->suggested;
    if (!profile_capture_apply_pin_observation(cap, ctx, res, *proposals, 0) ||
        !proposals->pin.recognized || !proposals->pin.selectable() ||
        proposals->pin.provenance != PROFILE_LOCK_PROVENANCE_USER_CONFIRMED_OBSERVATION ||
        proposals->pin.frequencyHz != 1702000000LL ||
        proposals->pin.document.lock.anchorTopologyId != 21 ||
        proposals->suggested != beforeSuggested) return 9909;
    if (!profile_capture_select_proposal(*proposals, PROFILE_CAPTURE_PIN, *doc, why, sizeof(why)) ||
        doc->lock.provenance != PROFILE_LOCK_PROVENANCE_USER_CONFIRMED_OBSERVATION ||
        doc->lock.targetFrequencyHz != 1702000000LL ||
        doc->source.pinKnowledge != PROFILE_PIN_UNKNOWN) return 9910;
    bank->count = 1; bank->documents[0] = *doc;
    std::string encoded; ProfileCodecError codecErr{};
    if (!profile_bank_encode(*bank, encoded, &codecErr) || !profile_bank_decode(encoded, *bank, &codecErr) ||
        bank->documents[0].lock.provenance != PROFILE_LOCK_PROVENANCE_USER_CONFIRMED_OBSERVATION ||
        bank->documents[0].lock.targetFrequencyHz != 1702000000LL) return 9911;

    // User edits observed value (e.g. 1800 MHz -> provenance USER, anchor pt 22 @ 2000 MHz) + round-trip.
    if (!profile_capture_apply_pin_observation(cap, ctx, res, *proposals, 1800000000LL) ||
        proposals->pin.provenance != PROFILE_LOCK_PROVENANCE_USER ||
        proposals->pin.frequencyHz != 1800000000LL ||
        proposals->pin.document.lock.anchorTopologyId != 22) return 9912;
    // Reject off-grid pin edit (1800.5 MHz).
    if (profile_capture_apply_pin_observation(cap, ctx, res, *proposals, 1800500000LL) ||
        proposals->pin.recognized) return 9913;

    // CAP-11: Tier 1 (validated getter) and Tier 2 (applied intent) are NEVER overwritten by Tier 3 observation.
    auto getterCap = cap;
    getterCap.pinGetterValid = 1; getterCap.pinPresent = 1;
    getterCap.pinMinimumHz = getterCap.pinMaximumHz = 1900000000LL;
    hardware_capture_domain(getterCap, PROFILE_DOMAIN_LOCK, CAPTURE_COMPLETE, 0);
    getterCap.fingerprint = hardware_capture_fingerprint(getterCap);
    getterCap.revision = service_control_review_revision(getterCap.fingerprint, nullptr);
    if (!profile_capture_prepare_proposals(getterCap, ctx, *proposals, why, sizeof(why))) return 9914;
    profile_pin_observation_analyze(getterCap, false, pinnedSamples, 16, res);
    if (res.status != PIN_OBS_SUPERSEDED ||
        profile_capture_apply_pin_observation(getterCap, ctx, res, *proposals, 1700000000LL) ||
        proposals->pin.provenance != PROFILE_LOCK_PROVENANCE_DRIVER_GETTER ||
        proposals->pin.frequencyHz != 1900000000LL) return 9915;

    ProfileCaptureProposalContext tier2Ctx{2000000000LL, true, true};
    if (!profile_capture_prepare_proposals(cap, tier2Ctx, *proposals, why, sizeof(why))) return 9916;
    profile_pin_observation_analyze(cap, true, pinnedSamples, 16, res);
    if (res.status != PIN_OBS_SUPERSEDED ||
        profile_capture_apply_pin_observation(cap, tier2Ctx, res, *proposals, 1700000000LL) ||
        proposals->pin.provenance != PROFILE_LOCK_PROVENANCE_APPLIED_INTENT ||
        proposals->pin.frequencyHz != 2000000000LL) return 9917;

    // Validated getter reporting ABSENT pin also prevents Tier 3 override.
    auto absentCap = cap;
    absentCap.pinGetterValid = 1; absentCap.pinPresent = 0;
    absentCap.pinMinimumHz = absentCap.pinMaximumHz = 0;
    hardware_capture_domain(absentCap, PROFILE_DOMAIN_LOCK, CAPTURE_COMPLETE, 0);
    absentCap.fingerprint = hardware_capture_fingerprint(absentCap);
    absentCap.revision = service_control_review_revision(absentCap.fingerprint, nullptr);
    profile_pin_observation_analyze(absentCap, false, pinnedSamples, 16, res);
    if (res.status != PIN_OBS_SUPERSEDED) return 9918;

    return 0;
}
