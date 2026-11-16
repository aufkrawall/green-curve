// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Compatible-curve assistance regressions (HELP-01..08, PLAN-03), codes 9800-9825.
#include "profile_curve_assistance.h"
#include "profile_codec.h"
#include <memory>
#include <string.h>

namespace {

HardwareCaptureObservation assist_fixture_capture(bool shaped = false, gc_i64 vfGrid = 1000,
    gc_i64 anchorBaseHz = 1162000000LL, gc_i64 tailBaseHz = 1800000000LL) {
    HardwareCaptureObservation o{};
    o.valid = 1; o.schemaVersion = HARDWARE_CAPTURE_SCHEMA_CURRENT; o.gpuGeneration = 12;
    o.binding.valid = 1; o.requestedDomains = 511u; o.revision = o.fingerprint = 42;
    for (int i = 0; i < 9; ++i) hardware_capture_domain(o, 1 << i, CAPTURE_UNSUPPORTED, -100001);
    o.topologyComplete = 1; o.pointCount = 10;
    const gc_i64 bases[10] = {
        300000000LL, 300000000LL, 900000000LL, 1050000000LL, 1100000000LL,
        1120000000LL, anchorBaseHz, 1350000000LL, 1500000000LL, tailBaseHz
    };
    for (gc_u32 i = 0; i < 10; ++i) {
        auto& pt = o.points[i];
        pt.index = i;
        pt.voltageUv = 550000 + (gc_i64)i * 25000; // points 0..5 < 700 mV
        pt.writableState = i < 2 ? 2 : 1;
        pt.offsetValid = pt.frequencyValid = 1;
        pt.offsetHz = (shaped && i == 2) ? 50000000LL : 0LL;
        pt.frequencyHz = bases[i] + pt.offsetHz;
    }
    hardware_capture_domain(o, PROFILE_DOMAIN_VF_CURVE, CAPTURE_COMPLETE);
    o.vfLimits = {1, 1, 1, 1, VF_OFFSET_RANGE_SOURCE_NVML_GRAPHICS, -500000000LL, 1000000000LL, vfGrid};
    o.composition = CAPTURE_COMPOSITION_INDEPENDENT;
    o.graphics.valid = o.graphics.writable = 1; o.graphics.surface = CLOCK_OFFSET_SURFACE_P0; o.graphics.grid = 1000000;
    hardware_capture_domain(o, PROFILE_DOMAIN_GRAPHICS_OFFSET, CAPTURE_COMPLETE);
    return o;
}

ProfileDocument assist_base_doc(const HardwareCaptureObservation& cap, LockMode mode, gc_i64 lockHz) {
    auto d = profile_document_make_empty();
    d.domains = PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_LOCK | PROFILE_DOMAIN_MEMORY | PROFILE_DOMAIN_POWER | PROFILE_DOMAIN_FAN;
    d.topologyCount = (int)cap.pointCount; d.sourceTopologyComplete = true;
    for (int i = 0; i < d.topologyCount; ++i) {
        d.topology[i] = {100 + i, (int)cap.points[i].index, cap.points[i].voltageUv, cap.points[i].writableState};
    }
    d.lock.present = true; d.lock.mode = mode; d.lock.anchorTopologyId = 106; d.lock.targetFrequencyHz = lockHz;
    d.hasMemoryOffset = true; d.memoryOffsetHz = -500000; d.memorySurface = CLOCK_OFFSET_SURFACE_P0;
    d.power.present = true; d.power.ratio = {249, 250};
    d.fan.present = true; d.fan.fanMode = FAN_MODE_FIXED; d.fan.fanPercent = 55;
    return d;
}

} // namespace

int run_profile_curve_assistance_tests() {
    char why[256]{};
    auto assist = std::make_unique<ProfileCurveAssistance>();
    auto derived = std::make_unique<ProfileDocument>();

    // HELP-03 + HELP-01: captured prefix offsets keep PROFILE_POINT_CAPTURED_OFFSET when proven baseline holds;
    // hidden < 700 mV points repaired; unrelated memory/power/fan stay byte-identical.
    HardwareCaptureObservation cap = assist_fixture_capture(false, 1000, 1000000000LL, 1300000000LL);
    ProfileDestinationTopology dest{};
    if (!profile_destination_from_capture(cap, dest, why, sizeof(why))) return 9800;
    ProfileDocument capPrefix = assist_base_doc(cap, LOCK_MODE_FLATTEN, 1000000000LL);
    capPrefix.points[2].kind = PROFILE_POINT_CAPTURED_OFFSET; capPrefix.points[2].capturedOffsetHz = 200000000LL; // 900+200=1100 > 1000
    capPrefix.points[4].kind = PROFILE_POINT_CAPTURED_OFFSET; capPrefix.points[4].capturedOffsetHz = 150000000LL; // 1100+150=1250 > 1000
    capPrefix.points[5].kind = PROFILE_POINT_ABSENT; // 1120 stock > 1000
    profile_curve_assistance_prepare(capPrefix, cap, dest, 10, {}, PROFILE_ASSIST_PREFIX, *assist);
    if (assist->status != PROFILE_ASSIST_PREPARED || !assist->proven || assist->prefixChangedCount != 4 ||
        assist->conversionCount != 0 ||
        assist->previewDocument.points[2].kind != PROFILE_POINT_CAPTURED_OFFSET ||
        assist->previewDocument.points[2].capturedOffsetHz != 100000000LL ||
        assist->previewDocument.points[3].kind != PROFILE_POINT_ABSOLUTE_FREQUENCY ||
        assist->previewDocument.points[3].absoluteFrequencyHz != 1000000000LL ||
        assist->previewDocument.points[4].kind != PROFILE_POINT_CAPTURED_OFFSET ||
        assist->previewDocument.points[4].capturedOffsetHz != -100000000LL ||
        assist->previewDocument.points[5].kind != PROFILE_POINT_ABSOLUTE_FREQUENCY ||
        assist->previewDocument.points[5].absoluteFrequencyHz != 1000000000LL ||
        assist->previewDocument.memoryOffsetHz != -500000 || assist->previewDocument.power.ratio.numerator != 249 ||
        assist->previewDocument.fan.fanPercent != 55) return 9801;

    // HELP-03 projected point converts explicitly to ABSOLUTE_FREQUENCY when accepted with proven baseline.
    ProfileDocument projPrefix = assist_base_doc(cap, LOCK_MODE_FLATTEN, 1000000000LL);
    projPrefix.domains |= PROFILE_DOMAIN_GRAPHICS_OFFSET;
    projPrefix.graphicsOffset.present = true; projPrefix.graphicsOffset.offsetHz = 200000000LL;
    projPrefix.points[2].kind = PROFILE_POINT_PROJECTION; // 900+200=1100 > 1000
    ProfileRuntimeRoutes vfRoute{DESIRED_GRAPHICS_VF_COMPONENT, 0};
    profile_curve_assistance_prepare(projPrefix, cap, dest, 10, vfRoute, PROFILE_ASSIST_PREFIX, *assist);
    if (assist->status != PROFILE_ASSIST_PREPARED || !assist->proven || assist->conversionCount < 1 ||
        !strstr(assist->explanation, "projection->absolute") ||
        assist->previewDocument.points[2].kind != PROFILE_POINT_ABSOLUTE_FREQUENCY ||
        assist->previewDocument.points[2].absoluteFrequencyHz != 1000000000LL ||
        assist->previewDocument.graphicsOffset.offsetHz != 200000000LL) return 9802;

    // HELP-03 over shaped baseline / unresolved composition: refuses conversion over untrusted base.
    HardwareCaptureObservation shaped = assist_fixture_capture(true, 1000, 1000000000LL, 1300000000LL);
    ProfileDestinationTopology shapedDest{};
    if (!profile_destination_from_capture(shaped, shapedDest, why, sizeof(why))) return 9803;
    profile_curve_assistance_prepare(capPrefix, shaped, shapedDest, 10, {}, PROFILE_ASSIST_PREFIX, *assist);
    if (assist->status != PROFILE_ASSIST_BASELINE_REQUIRED || assist->proven) return 9804;
    profile_curve_assistance_prepare(projPrefix, cap, dest, 10, {}, PROFILE_ASSIST_PREFIX, *assist); // unresolved AUTO route
    if (assist->status != PROFILE_ASSIST_BASELINE_REQUIRED) return 9805;

    // HELP-02: prefix point requiring drop below minimum offset refuses repair cleanly.
    cap.vfLimits.minimumHz = -150000000LL;
    capPrefix.points[2].kind = PROFILE_POINT_ABSOLUTE_FREQUENCY;
    capPrefix.points[2].capturedOffsetHz = 0;
    capPrefix.points[2].absoluteFrequencyHz = 1200000000LL; // point 5 base 1120 -> lock 800 MHz needs -320 < -150
    capPrefix.lock.targetFrequencyHz = 800000000LL;
    profile_curve_assistance_prepare(capPrefix, cap, dest, 10, {}, PROFILE_ASSIST_PREFIX, *assist);
    if (assist->status != PROFILE_ASSIST_LIMITED || !strstr(assist->explanation, "minimum")) return 9806;

    // PLAN-03 + HELP-04: anchor cap 2162 MHz with tail floor 2205 MHz -> complete proven empty set.
    HardwareCaptureObservation tailBlock = assist_fixture_capture(false, 1000, 1162000000LL, 2305000000LL);
    tailBlock.vfLimits.minimumHz = -100000000LL; // tail floor 2305 - 100 = 2205 MHz
    ProfileDestinationTopology tailDest{};
    if (!profile_destination_from_capture(tailBlock, tailDest, why, sizeof(why))) return 9807;
    ProfileDocument ask2400 = assist_base_doc(tailBlock, LOCK_MODE_FLATTEN, 2400000000LL);
    profile_curve_assistance_prepare(ask2400, tailBlock, tailDest, 10, {}, PROFILE_ASSIST_CLOSEST_TARGET, *assist);
    if (assist->status != PROFILE_ASSIST_NO_COMPATIBLE_TARGET) return 9808;

    // HELP-04: tail floor 2050 MHz, anchor cap 2162 MHz, ask 2400 MHz -> closest supported 2162 MHz!
    tailBlock.points[9].frequencyHz = 2150000000LL; // 2150 - 100 = 2050 MHz
    profile_curve_assistance_prepare(ask2400, tailBlock, tailDest, 10, {}, PROFILE_ASSIST_CLOSEST_TARGET, *assist);
    if (assist->status != PROFILE_ASSIST_PREPARED || !assist->proven ||
        assist->suggestedLockHz != 2162000000LL ||
        assist->previewDocument.lock.targetFrequencyHz != 2162000000LL ||
        assist->previewDocument.lock.anchorTopologyId != 106) return 9809;

    // HELP-04 combined vs closest-target: conflicting typed prefix 1900 MHz vs anchor cap 1800 MHz.
    HardwareCaptureObservation combinedCap = assist_fixture_capture(false, 1000, 800000000LL, 1500000000LL);
    ProfileDestinationTopology combinedDest{};
    if (!profile_destination_from_capture(combinedCap, combinedDest, why, sizeof(why))) return 9810;
    ProfileDocument combinedDoc = assist_base_doc(combinedCap, LOCK_MODE_FLATTEN, 2000000000LL);
    combinedDoc.points[5].kind = PROFILE_POINT_ABSOLUTE_FREQUENCY;
    combinedDoc.points[5].absoluteFrequencyHz = 1900000000LL; // prefix > anchor cap (1800 MHz)
    profile_curve_assistance_prepare(combinedDoc, combinedCap, combinedDest, 10, {}, PROFILE_ASSIST_CLOSEST_TARGET, *assist);
    if (assist->status != PROFILE_ASSIST_NO_COMPATIBLE_TARGET) return 9811;
    profile_curve_assistance_prepare(combinedDoc, combinedCap, combinedDest, 10, {}, PROFILE_ASSIST_COMBINED, *assist);
    if (assist->status != PROFILE_ASSIST_PREPARED || !assist->proven ||
        assist->suggestedLockHz != 1800000000LL || assist->prefixChangedCount < 1 ||
        assist->previewDocument.points[5].absoluteFrequencyHz != 1800000000LL) return 9812;

    // HELP-05: budget exhaustion yields INDETERMINATE (never claims no solution), distinct from NO_COMPATIBLE_TARGET.
    HardwareCaptureObservation hardCap = assist_fixture_capture(false, 1000, 1500000000LL, 1800000500LL);
    hardCap.vfLimits.minimumHz = -300000000LL; hardCap.vfLimits.maximumHz = 300000000LL;
    ProfileDestinationTopology hardDest{};
    if (!profile_destination_from_capture(hardCap, hardDest, why, sizeof(why))) return 9813;
    ProfileDocument hardDoc = assist_base_doc(hardCap, LOCK_MODE_HARD, 1700000000LL);
    profile_curve_assistance_prepare(hardDoc, hardCap, hardDest, 10, {}, PROFILE_ASSIST_CLOSEST_TARGET, *assist, 4u);
    if (assist->status != PROFILE_ASSIST_INDETERMINATE || !assist->budgetExhausted ||
        assist->candidatesEvaluated != 4u) return 9814;
    profile_curve_assistance_prepare(hardDoc, hardCap, hardDest, 10, {}, PROFILE_ASSIST_CLOSEST_TARGET, *assist, 65536u);
    if (assist->status != PROFILE_ASSIST_NO_COMPATIBLE_TARGET || assist->budgetExhausted) return 9825;

    // HELP-06: sub-kHz base (1500000500) on 1000 Hz VF grid -> FLATTEN hits exact 1600000500, HARD pin fails CRT (mod 1000).
    HardwareCaptureObservation pascalCap = assist_fixture_capture(false, 1000, 1500000500LL, 1550000500LL);
    pascalCap.vfLimits.minimumHz = -500000000LL; pascalCap.vfLimits.maximumHz = 100000000LL;
    ProfileDestinationTopology pascalDest{};
    if (!profile_destination_from_capture(pascalCap, pascalDest, why, sizeof(why))) return 9815;
    ProfileDocument pascalDoc = assist_base_doc(pascalCap, LOCK_MODE_FLATTEN, 1700000000LL);
    profile_curve_assistance_prepare(pascalDoc, pascalCap, pascalDest, 10, {}, PROFILE_ASSIST_CLOSEST_TARGET, *assist);
    if (assist->status != PROFILE_ASSIST_PREPARED || assist->suggestedLockHz != 1600000500LL) return 9816;
    ProfileDocument pascalHard = assist_base_doc(pascalCap, LOCK_MODE_HARD, 1700000000LL);
    profile_curve_assistance_prepare(pascalHard, pascalCap, pascalDest, 10, {}, PROFILE_ASSIST_CLOSEST_TARGET, *assist);
    if (assist->status != PROFILE_ASSIST_NO_COMPATIBLE_TARGET) return 9817;

    // HELP-06 estimate label over shaped live curve when explicit typed prefix conflicts.
    ProfileDocument shapedDoc = assist_base_doc(shaped, LOCK_MODE_FLATTEN, 1000000000LL);
    shapedDoc.points[2].kind = PROFILE_POINT_ABSOLUTE_FREQUENCY; shapedDoc.points[2].absoluteFrequencyHz = 1200000000LL;
    profile_curve_assistance_prepare(shapedDoc, shaped, shapedDest, 10, {}, PROFILE_ASSIST_PREFIX, *assist);
    // Point 5 is absent on shaped live -> BASELINE_REQUIRED unless all prefix points <= plateau are explicit:
    // Make points 3,4,5 explicit <= 1000 MHz so only point 2 is modified and baseline is unproven -> ESTIMATE!
    for (int i = 3; i <= 5; ++i) { shapedDoc.points[i].kind = PROFILE_POINT_ABSOLUTE_FREQUENCY; shapedDoc.points[i].absoluteFrequencyHz = 950000000LL; }
    profile_curve_assistance_prepare(shapedDoc, shaped, shapedDest, 10, {}, PROFILE_ASSIST_PREFIX, *assist);
    if (assist->status != PROFILE_ASSIST_ESTIMATE || assist->proven ||
        !strstr(assist->explanation, "ESTIMATE ONLY")) return 9818;

    // HELP-07 + HELP-08: stale acceptance refuses; valid acceptance encodes/decodes explicit intent with no recipe.
    profile_curve_assistance_prepare(ask2400, tailBlock, tailDest, 10, {}, PROFILE_ASSIST_CLOSEST_TARGET, *assist);
    if (assist->status != PROFILE_ASSIST_PREPARED) return 9819;
    if (profile_curve_assistance_accept(*assist, ask2400, 11, tailBlock, tailDest, {}, *derived, why, sizeof(why))) return 9820;
    auto staleDoc = ask2400; ++staleDoc.memoryOffsetHz;
    if (profile_curve_assistance_accept(*assist, staleDoc, 10, tailBlock, tailDest, {}, *derived, why, sizeof(why))) return 9821;
    auto staleCap = tailBlock; ++staleCap.revision;
    if (profile_curve_assistance_accept(*assist, ask2400, 10, staleCap, tailDest, {}, *derived, why, sizeof(why))) return 9822;
    if (!profile_curve_assistance_accept(*assist, ask2400, 10, tailBlock, tailDest, {}, *derived, why, sizeof(why)) ||
        derived->lock.targetFrequencyHz != 2162000000LL) return 9823;
    auto bank = std::make_unique<ProfileBank>(); bank->count = 1; bank->documents[0] = *derived;
    std::string encoded; ProfileCodecError codecErr{};
    if (!profile_bank_encode(*bank, encoded, &codecErr) || !profile_bank_decode(encoded, *bank, &codecErr) ||
        bank->documents[0].lock.targetFrequencyHz != 2162000000LL ||
        !profile_document_semantically_equal(bank->documents[0], *derived)) return 9824;

    return 0;
}
