// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Imported (document) slots must load as the same runtime record every apply
// path uses -- the tray, hotkeys, auto-switch, logon and the editor's Load --
// instead of being refused. Cases 9950-9969.
#include "profile_slot_runtime.h"
#include "profile_legacy_document.h"
#include "profile_codec.h"
#include "profile_summary.h"
#include <climits>
#include <memory>
#include <cstring>

namespace {
DesiredSettings editor_profile() {
    DesiredSettings d;
    initialize_desired_settings_defaults(&d);
    d.applicationScope = DESIRED_SCOPE_LEGACY;
    d.hasGpuOffset = 1; d.gpuOffsetMHz = 75; d.gpuOffsetExcludeLowCount = 7;
    for (int ci = 10; ci <= 12; ++ci) { d.hasCurvePoint[ci] = 1; d.curvePointMHz[ci] = 1500 + (unsigned)ci; }
    d.hasCurvePoint[13] = 1; d.curvePointFromGpuOffset[13] = 1; d.curvePointMHz[13] = 1600;
    d.hasLock = 1; d.lockCi = 40; d.lockMHz = 1905; d.lockMode = LOCK_MODE_FLATTEN; d.lockTracksAnchor = 0;
    d.hasMemOffset = 1; d.memOffsetMHz = 1000;
    d.hasPowerLimit = 1; d.powerLimitPct = 90;
    d.hasFan = 1; d.fanMode = FAN_MODE_FIXED; d.fanAuto = 0; d.fanPercent = 55;
    return d;
}
bool same_points(const DesiredSettings& a, const DesiredSettings& b) {
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        if (!!a.hasCurvePoint[ci] != !!b.hasCurvePoint[ci] || !!a.curvePointCaptured[ci] != !!b.curvePointCaptured[ci] ||
            !!a.curvePointFromGpuOffset[ci] != !!b.curvePointFromGpuOffset[ci]) return false;
        if (a.hasCurvePoint[ci] && a.curvePointMHz[ci] != b.curvePointMHz[ci]) return false;
        if (a.curvePointCaptured[ci] && a.curvePointCapturedOffsetHz[ci] != b.curvePointCapturedOffsetHz[ci]) return false;
    }
    return true;
}
// A plain curve: 100 graphics points from 600 mV in 6.25 mV steps, rising.
void live_curve(VFCurvePoint* curve) {
    memset(curve, 0, sizeof(VFCurvePoint) * VF_NUM_POINTS);
    for (int ci = 0; ci < 100; ++ci) {
        curve[ci].volt_uV = 600000u + (unsigned)ci * 6250u;
        curve[ci].freq_kHz = 900000u + (unsigned)ci * 15000u;
    }
}
}  // namespace

int run_profile_slot_runtime_tests() {
    char why[192]{};
    // 9950: an ordinary editor profile survives export (document) and back.
    const DesiredSettings original = editor_profile();
    auto doc = std::make_unique<ProfileDocument>();
    if (!profile_document_from_legacy_desired(original, *doc, why, sizeof(why)) || !doc->legacyIndicesUnresolved)
        return 9950;
    auto back = std::make_unique<DesiredSettings>();
    if (!profile_slot_document_runtime(*doc, nullptr, *back, why, sizeof(why))) return 9951;
    if (back->applicationScope != DESIRED_SCOPE_LEGACY || !back->hasGpuOffset || back->gpuOffsetMHz != 75 ||
        back->gpuOffsetExcludeLowCount != 7 || !same_points(original, *back) || !back->hasLock || back->lockCi != 40 ||
        back->lockMHz != 1905 || back->lockMode != LOCK_MODE_FLATTEN || back->lockTracksAnchor ||
        !back->hasMemOffset || back->memOffsetMHz != 1000 || back->memOffsetPrecise ||
        back->powerLimitPct != 90 || back->powerLimitRatio || !back->hasFan || back->fanMode != FAN_MODE_FIXED ||
        back->fanPercent != 55)
        return 9952;
    // 9953: the projected point keeps its saved MHz as a labeled observation.
    if (!doc->points[3].hasObservedFrequency || doc->points[3].observedFrequencyHz != 1600000000LL ||
        doc->points[3].kind != PROFILE_POINT_PROJECTION)
        return 9953;
    // 9954: the same record survives the portable file encoding.
    auto bank = std::make_unique<ProfileBank>();
    bank->count = 1; bank->documents[0] = *doc;
    std::string bytes;
    if (!profile_bank_encode(*bank, bytes)) return 9954;
    auto decoded = std::make_unique<ProfileBank>();
    auto again = std::make_unique<DesiredSettings>();
    if (!profile_bank_decode(bytes, *decoded) || decoded->count != 1 ||
        !profile_slot_document_runtime(decoded->documents[0], nullptr, *again, why, sizeof(why)) ||
        !same_points(*back, *again) || again->lockMHz != 1905 || again->memOffsetMHz != 1000)
        return 9955;

    // 9956: precise and captured intent keep an explicit scope and exact values.
    DesiredSettings precise;
    initialize_desired_settings_defaults(&precise);
    precise.applicationScope = DESIRED_SCOPE_REPLACE_PROFILE;
    precise.hasGpuOffset = 1; precise.gpuOffsetMHz = 12;
    precise.authoring.graphicsPrecise = 1; precise.authoring.graphicsOffsetHz = 12500000;
    precise.curvePointCaptured[30] = 1; precise.curvePointCapturedOffsetHz[30] = 37500000;
    precise.hasCurvePoint[31] = 1; precise.curvePointMHz[31] = 1700;
    precise.authoring.pointFrequencyPrecise[31] = 1; precise.authoring.pointFrequencyHz[31] = 1700250000;
    if (!desired_memory_set_precise(&precise, 1000500000LL, CLOCK_OFFSET_SURFACE_P0) ||
        !desired_power_set_ratio(&precise, {249, 250}) || !desired_authoring_valid(&precise))
        return 9956;
    if (!profile_document_from_legacy_desired(precise, *doc, why, sizeof(why)) ||
        !profile_slot_document_runtime(*doc, nullptr, *back, why, sizeof(why)))
        return 9957;
    if (back->applicationScope != DESIRED_SCOPE_REPLACE_PROFILE || !back->authoring.graphicsPrecise ||
        back->authoring.graphicsOffsetHz != 12500000 || !back->curvePointCaptured[30] ||
        back->curvePointCapturedOffsetHz[30] != 37500000 || !back->authoring.pointFrequencyPrecise[31] ||
        back->authoring.pointFrequencyHz[31] != 1700250000 || back->memOffsetHz != 1000500000LL ||
        back->memOffsetSurface != CLOCK_OFFSET_SURFACE_P0 || back->powerRatioNumerator != 249 ||
        back->powerRatioDenominator != 250 || !desired_authoring_valid(back.get()) ||
        !desired_scalar_intent_valid(back.get()))
        return 9958;

    // 9959: an included-controls profile keeps its point-replacement scope.
    DesiredSettings update;
    initialize_desired_settings_defaults(&update);
    update.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED | DESIRED_SCOPE_REPLACE_VF_POINTS;
    update.hasCurvePoint[20] = 1; update.curvePointMHz[20] = 1400;
    if (!profile_document_from_legacy_desired(update, *doc, why, sizeof(why)) ||
        !profile_slot_document_runtime(*doc, nullptr, *back, why, sizeof(why)) ||
        back->applicationScope != (DESIRED_SCOPE_UPDATE_INCLUDED | DESIRED_SCOPE_REPLACE_VF_POINTS) ||
        !back->hasCurvePoint[20] || back->curvePointMHz[20] != 1400)
        return 9959;

    // 9960: a capture names points by voltage and maps onto the live curve.
    auto curve = std::make_unique<VFCurvePoint[]>(VF_NUM_POINTS);
    live_curve(curve.get());
    const VfFloorTopology floor = vf_floor_topology_compute(curve.get(), nullptr, VF_NUM_POINTS);
    auto live = std::make_unique<ProfileDestinationTopology>();
    if (!profile_destination_from_vf_curve(curve.get(), floor, *live) || live->count != 100 || !live->complete)
        return 9960;
    auto capture = std::make_unique<ProfileDocument>(profile_document_make_empty());
    capture->applicationScope = PROFILE_SCOPE_REPLACE_PROFILE;
    capture->domains = PROFILE_DOMAIN_VF_CURVE;
    capture->sourceTopologyComplete = false;
    for (int i = 0; i < 3; ++i) {
        capture->topology[i] = {i + 1, 90 + i, 600000LL + (60 + i) * 6250LL, 1};
        capture->points[i].kind = PROFILE_POINT_CAPTURED_OFFSET;
        capture->points[i].capturedOffsetHz = 15000000LL * (i + 1);
    }
    capture->topologyCount = 3;
    if (!profile_document_structurally_valid(*capture, why, sizeof(why)) || !profile_document_requires_destination(*capture))
        return 9961;
    if (!profile_slot_document_runtime(*capture, live.get(), *back, why, sizeof(why)) ||
        !back->curvePointCaptured[60] || back->curvePointCapturedOffsetHz[62] != 45000000LL ||
        back->curvePointCaptured[90] || !back->authoring.topologyBound ||
        back->authoring.voltageUv[61] != 600000 + 61 * 6250 || !desired_authoring_valid(back.get()) ||
        back->applicationScope != DESIRED_SCOPE_REPLACE_PROFILE)
        return 9962;
    // 9963: without a GPU it refuses with an actionable reason, never a guess.
    why[0] = 0;
    if (profile_slot_document_runtime(*capture, nullptr, *back, why, sizeof(why)) || !strstr(why, "needs the GPU"))
        return 9963;
    // 9964: a different GPU (no matching voltage) refuses the whole record.
    capture->topology[1].voltageUv = 1234567;
    why[0] = 0;
    if (profile_slot_document_runtime(*capture, live.get(), *back, why, sizeof(why)) ||
        !strstr(why, "different GPU") || !strstr(why, "1 curve point has"))
        return 9964;
    capture->topology[1].voltageUv = 600000LL + 61 * 6250LL;
    // 9965: a record that changes a fixed point refuses instead of failing at Apply.
    for (int i = 0; i < live->count; ++i) if (live->points[i].index == 61) live->points[i].writable = false;
    why[0] = 0;
    if (profile_slot_document_runtime(*capture, live.get(), *back, why, sizeof(why)) || !strstr(why, "fixed curve point"))
        return 9965;
    // 9966: a scalar-only capture needs no GPU and keeps its named memory control.
    auto scalar = std::make_unique<ProfileDocument>(profile_document_make_empty());
    scalar->applicationScope = PROFILE_SCOPE_REPLACE_PROFILE;
    scalar->domains = PROFILE_DOMAIN_MEMORY | PROFILE_DOMAIN_POWER;
    scalar->hasMemoryOffset = true; scalar->memoryOffsetHz = 750000000LL; scalar->memorySurface = CLOCK_OFFSET_SURFACE_GLOBAL;
    scalar->power.present = true; scalar->power.ratio = {9, 10};
    if (!profile_slot_document_runtime(*scalar, nullptr, *back, why, sizeof(why)) ||
        back->applicationScope != DESIRED_SCOPE_REPLACE_PROFILE || back->memOffsetSurface != CLOCK_OFFSET_SURFACE_GLOBAL ||
        back->memOffsetHz != 750000000LL || back->powerLimitPct != 90)
        return 9966;
    // 9967: the same scalars without a named control stay an ordinary record.
    scalar->memorySurface = 0;
    if (!profile_slot_document_runtime(*scalar, nullptr, *back, why, sizeof(why)) ||
        back->applicationScope != DESIRED_SCOPE_LEGACY || back->memOffsetMHz != 750 || back->memOffsetPrecise ||
        back->powerLimitPct != 90 || back->powerLimitRatio)
        return 9967;
    // 9968: duplicate point positions are refused, not silently merged.
    if (!profile_document_from_legacy_desired(original, *doc, why, sizeof(why))) return 9968;
    doc->topology[1].sourceIndex = doc->topology[0].sourceIndex;
    if (profile_slot_document_runtime(*doc, nullptr, *back, why, sizeof(why))) return 9969;

    // 12500-12501: out-of-range graphics offset reports exact MHz and valid bounds.
    if (!profile_document_from_legacy_desired(original, *doc, why, sizeof(why))) return 12500;
    doc->graphicsOffset.offsetHz = -1927000000LL;
    why[0] = 0;
    if (profile_slot_document_runtime(*doc, nullptr, *back, why, sizeof(why)) ||
        !strstr(why, "-1927 MHz") || !strstr(why, "-1000..1000 MHz"))
        return 12500;
    doc->graphicsOffset.offsetHz = 1000500000LL;
    why[0] = 0;
    if (profile_slot_document_runtime(*doc, nullptr, *back, why, sizeof(why)) ||
        !strstr(why, "+1000.5 MHz") || !strstr(why, "-1000..1000 MHz"))
        return 12501;

    // 12502-12503: legacy-scope memory offset out of range is refused with MHz and bounds.
    auto memDoc = std::make_unique<ProfileDocument>(profile_document_make_empty());
    memDoc->applicationScope = PROFILE_SCOPE_REPLACE_PROFILE;
    memDoc->domains = PROFILE_DOMAIN_MEMORY;
    memDoc->hasMemoryOffset = true;
    memDoc->memoryOffsetHz = 3500000000LL;
    why[0] = 0;
    if (profile_slot_document_runtime(*memDoc, nullptr, *back, why, sizeof(why)) ||
        !strstr(why, "+3500 MHz") || !strstr(why, "-3000..3000 MHz"))
        return 12502;
    memDoc->memoryOffsetHz = -3250000000LL;
    why[0] = 0;
    if (profile_slot_document_runtime(*memDoc, nullptr, *back, why, sizeof(why)) ||
        !strstr(why, "-3250 MHz") || !strstr(why, "-3000..3000 MHz"))
        return 12503;

    // 12504: captured voltage-bound flatten (driver_offset=0 pre-tail, flatten
    // lock, precise P0 memory, fractional power ratio) survives file encoding
    // and maps onto the live curve cleanly.
    auto capFlat = std::make_unique<ProfileDocument>(profile_document_make_empty());
    capFlat->applicationScope = PROFILE_SCOPE_UPDATE_INCLUDED;
    capFlat->pointSetPolicy = PROFILE_POINTS_REPLACE;
    capFlat->domains = PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_LOCK | PROFILE_DOMAIN_MEMORY | PROFILE_DOMAIN_POWER;
    capFlat->topologyCount = 3;
    capFlat->topology[0] = {60, 60, 600000LL + 60 * 6250LL, 1};
    capFlat->points[0].kind = PROFILE_POINT_CAPTURED_OFFSET;
    capFlat->points[0].capturedOffsetHz = 0;
    capFlat->topology[1] = {61, 61, 600000LL + 61 * 6250LL, 1};
    capFlat->points[1].kind = PROFILE_POINT_CAPTURED_OFFSET;
    capFlat->points[1].capturedOffsetHz = 0;
    capFlat->topology[2] = {62, 62, 600000LL + 62 * 6250LL, 1};
    capFlat->lock.present = true;
    capFlat->lock.mode = LOCK_MODE_FLATTEN;
    capFlat->lock.anchorTopologyId = 62;
    capFlat->lock.targetFrequencyHz = 2962000000LL;
    capFlat->lock.trackAnchor = false;
    capFlat->hasMemoryOffset = true;
    capFlat->memoryOffsetHz = 3000000000LL;
    capFlat->memorySurface = CLOCK_OFFSET_SURFACE_P0;
    capFlat->power.present = true;
    capFlat->power.ratio = {17, 20};
    for (int i = 0; i < live->count; ++i) if (live->points[i].index == 61) live->points[i].writable = true;
    bank->count = 1; bank->documents[0] = *capFlat;
    bytes.clear();
    why[0] = 0;
    if (!profile_bank_encode(*bank, bytes) || !profile_bank_decode(bytes, *decoded) || decoded->count != 1 ||
        !profile_slot_document_runtime(decoded->documents[0], live.get(), *back, why, sizeof(why)) ||
        !back->curvePointCaptured[60] || back->curvePointCapturedOffsetHz[60] != 0 ||
        !back->hasLock || back->lockCi != 62 || back->lockMode != LOCK_MODE_FLATTEN || back->lockMHz != 2962 ||
        !back->memOffsetPrecise || back->memOffsetHz != 3000000000LL || back->memOffsetSurface != CLOCK_OFFSET_SURFACE_P0 ||
        !back->powerLimitRatio || back->powerRatioNumerator != 17 || back->powerRatioDenominator != 20)
        return 12504;

    // 12600-12622: legacy pre-2026-11-19 flat-tail graphics offset artifact
    // repair (e.g. -1927 MHz with exclude_low_count=76 alongside a flatten lock
    // at ci=76 -> 2962 MHz and absolute tail points 76..126) and strict
    // rejection of every non-artifact out-of-range case.
    {
        auto makeFlatTailArtifactDoc = []() {
            ProfileDocument d = profile_document_make_empty();
            d.documentId = 1;
            d.applicationScope = PROFILE_SCOPE_REPLACE_PROFILE;
            d.pointSetPolicy = PROFILE_POINTS_REPLACE;
            d.legacyIndicesUnresolved = true;
            d.domains = PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_GRAPHICS_OFFSET |
                        PROFILE_DOMAIN_MEMORY | PROFILE_DOMAIN_POWER |
                        PROFILE_DOMAIN_LOCK | PROFILE_DOMAIN_FAN;
            d.graphicsOffset.present = true;
            d.graphicsOffset.offsetHz = -1927000000LL;
            d.graphicsOffset.exclusionPolicy = PROFILE_EXCLUDE_LEGACY_COUNT;
            d.graphicsOffset.excludeLowCount = 76;
            d.hasMemoryOffset = true;
            d.memoryOffsetHz = 3000000000LL;
            d.power.present = true;
            d.power.ratio = {17, 20};
            d.fan.present = true;
            d.fan.fanMode = FAN_MODE_AUTO;
            d.fan.fanAuto = true;
            d.lock.present = true;
            d.lock.mode = LOCK_MODE_FLATTEN;
            d.lock.anchorTopologyId = 76;
            d.lock.targetFrequencyHz = 2962000000LL;
            d.lock.trackAnchor = true;
            for (int ci = 76; ci <= 126; ++ci) {
                const int idx = d.topologyCount++;
                d.topology[idx] = {ci, ci, 0, 0};
                d.points[idx].kind = PROFILE_POINT_ABSOLUTE_FREQUENCY;
                d.points[idx].absoluteFrequencyHz = 2962000000LL;
                d.points[idx].hasObservedOffset = true;
                d.points[idx].observedOffsetHz = (ci == 76) ? 480000000LL : -1927000000LL;
            }
            return d;
        };

        // 12600-12602: exact user Profile 1 shape loads cleanly, survives
        // portable bank round-trip, clears the dead -1927 MHz / exclude 76 /
        // lockTracksAnchor=1 artifact, and summarizes as core 0 MHz.
        auto artifact = std::make_unique<ProfileDocument>(makeFlatTailArtifactDoc());
        if (!profile_indexed_document_has_legacy_flat_tail_offset_artifact(*artifact))
            return 12600;
        why[0] = 0;
        if (!profile_slot_document_runtime(*artifact, nullptr, *back, why, sizeof(why)) ||
            back->applicationScope != DESIRED_SCOPE_LEGACY ||
            !back->hasGpuOffset || back->gpuOffsetMHz != 0 || back->gpuOffsetExcludeLowCount != 0 ||
            !back->hasLock || back->lockCi != 76 || back->lockMode != LOCK_MODE_FLATTEN ||
            back->lockMHz != 2962 || back->lockTracksAnchor ||
            !back->hasMemOffset || back->memOffsetMHz != 3000 ||
            !back->hasPowerLimit || back->powerLimitPct != 85 ||
            !back->hasCurvePoint[76] || back->curvePointMHz[76] != 2962 ||
            !back->hasCurvePoint[126] || back->curvePointMHz[126] != 2962 ||
            back->hasCurvePoint[75])
            return 12601;
        bank->count = 1; bank->documents[0] = *artifact;
        bytes.clear();
        if (!profile_bank_encode(*bank, bytes) || !profile_bank_decode(bytes, *decoded) ||
            decoded->count != 1 ||
            !profile_slot_document_runtime(decoded->documents[0], nullptr, *again, why, sizeof(why)) ||
            again->gpuOffsetMHz != 0 || again->gpuOffsetExcludeLowCount != 0 ||
            again->lockMHz != 2962 || again->lockTracksAnchor)
            return 12602;
        const std::string summary = profile_document_summary(*artifact);
        if (summary.find("core 0 MHz") == std::string::npos ||
            summary.find("-1927") != std::string::npos)
            return 12603;

        // 12604: excludeLowCount == lockCi + 1 (77) also repairs cleanly.
        auto excl77 = std::make_unique<ProfileDocument>(makeFlatTailArtifactDoc());
        excl77->graphicsOffset.excludeLowCount = 77;
        if (!profile_slot_document_runtime(*excl77, nullptr, *back, why, sizeof(why)) ||
            back->gpuOffsetMHz != 0 || back->gpuOffsetExcludeLowCount != 0 || back->lockTracksAnchor)
            return 12604;

        // 12605: selective artifact without per-point observed offsets (e.g.
        // exported from Linux INI) still repairs because excludeLowCount >= lockCi
        // excludes every pre-tail point and the flatten owns the entire tail.
        auto noObs = std::make_unique<ProfileDocument>(makeFlatTailArtifactDoc());
        for (int i = 0; i < noObs->topologyCount; ++i) {
            noObs->points[i].hasObservedOffset = false;
            noObs->points[i].observedOffsetHz = 0;
        }
        if (!profile_slot_document_runtime(*noObs, nullptr, *back, why, sizeof(why)) ||
            back->gpuOffsetMHz != 0 || back->gpuOffsetExcludeLowCount != 0)
            return 12605;

        // 12606: uniform pre-fix flat-tail artifact (excludeLowCount == 0, low
        // lockCi, zero pre-tail points, anchor observed offset != tail, >= 4
        // matching tail observed offsets) repairs cleanly.
        auto uniformArt = std::make_unique<ProfileDocument>(makeFlatTailArtifactDoc());
        uniformArt->graphicsOffset.exclusionPolicy = PROFILE_EXCLUDE_NONE;
        uniformArt->graphicsOffset.excludeLowCount = 0;
        if (!profile_slot_document_runtime(*uniformArt, nullptr, *back, why, sizeof(why)) ||
            back->gpuOffsetMHz != 0 || back->gpuOffsetExcludeLowCount != 0 || back->lockTracksAnchor)
            return 12606;

        // 12607: excludeLowCount < lockCi leaves pre-tail points governed by
        // -1927 MHz and MUST still be refused.
        auto excl70 = std::make_unique<ProfileDocument>(makeFlatTailArtifactDoc());
        excl70->graphicsOffset.excludeLowCount = 70;
        why[0] = 0;
        if (profile_slot_document_runtime(*excl70, nullptr, *back, why, sizeof(why)) ||
            !strstr(why, "-1927 MHz"))
            return 12607;

        // 12608: any projected point depends on graphicsOffset -> MUST refuse.
        auto withProj = std::make_unique<ProfileDocument>(makeFlatTailArtifactDoc());
        withProj->points[1].kind = PROFILE_POINT_PROJECTION;
        withProj->points[1].absoluteFrequencyHz = 0;
        withProj->points[1].hasObservedFrequency = true;
        withProj->points[1].observedFrequencyHz = 2962000000LL;
        why[0] = 0;
        if (!profile_document_structurally_valid(*withProj, why, sizeof(why)) ||
            profile_slot_document_runtime(*withProj, nullptr, *back, why, sizeof(why)) ||
            !strstr(why, "-1927 MHz"))
            return 12608;

        // 12609: HARD lock is not a VF tail flatten -> MUST refuse.
        auto hardLock = std::make_unique<ProfileDocument>(makeFlatTailArtifactDoc());
        hardLock->lock.mode = LOCK_MODE_HARD;
        why[0] = 0;
        if (profile_slot_document_runtime(*hardLock, nullptr, *back, why, sizeof(why)) ||
            !strstr(why, "-1927 MHz"))
            return 12609;

        // 12610: anchor-only curve with no tail points above lockCi -> MUST refuse.
        auto anchorOnly = std::make_unique<ProfileDocument>(makeFlatTailArtifactDoc());
        for (int i = 1; i < anchorOnly->topologyCount; ++i) {
            anchorOnly->topology[i] = {};
            anchorOnly->points[i] = {};
        }
        anchorOnly->topologyCount = 1;
        why[0] = 0;
        if (!profile_document_structurally_valid(*anchorOnly, why, sizeof(why)) ||
            profile_slot_document_runtime(*anchorOnly, nullptr, *back, why, sizeof(why)) ||
            !strstr(why, "-1927 MHz"))
            return 12610;

        // 12611: recorded tail observed offsets contradicting -1927 MHz -> MUST refuse.
        auto wrongObs = std::make_unique<ProfileDocument>(makeFlatTailArtifactDoc());
        for (int i = 1; i < wrongObs->topologyCount; ++i)
            wrongObs->points[i].observedOffsetHz = -200000000LL;
        why[0] = 0;
        if (!profile_document_structurally_valid(*wrongObs, why, sizeof(why)) ||
            profile_slot_document_runtime(*wrongObs, nullptr, *back, why, sizeof(why)) ||
            !strstr(why, "-1927 MHz"))
            return 12611;

        // 12612: positive out-of-range offset -> MUST refuse.
        auto positiveOor = std::make_unique<ProfileDocument>(makeFlatTailArtifactDoc());
        positiveOor->graphicsOffset.offsetHz = 1927000000LL;
        why[0] = 0;
        if (!profile_document_structurally_valid(*positiveOor, why, sizeof(why)) ||
            profile_slot_document_runtime(*positiveOor, nullptr, *back, why, sizeof(why)) ||
            !strstr(why, "+1927 MHz"))
            return 12612;

        // 12613: uniform (exclude=0) without observed-offset proof -> MUST refuse.
        auto uniformNoObs = std::make_unique<ProfileDocument>(*uniformArt);
        for (int i = 0; i < uniformNoObs->topologyCount; ++i) {
            uniformNoObs->points[i].hasObservedOffset = false;
            uniformNoObs->points[i].observedOffsetHz = 0;
        }
        why[0] = 0;
        if (!profile_document_structurally_valid(*uniformNoObs, why, sizeof(why)) ||
            profile_slot_document_runtime(*uniformNoObs, nullptr, *back, why, sizeof(why)) ||
            !strstr(why, "-1927 MHz"))
            return 12613;

        // 12614: uniform (exclude=0) with a pre-tail point -> MUST refuse.
        auto uniformPreTail = std::make_unique<ProfileDocument>(*uniformArt);
        for (int i = uniformPreTail->topologyCount; i > 0; --i) {
            uniformPreTail->topology[i] = uniformPreTail->topology[i - 1];
            uniformPreTail->points[i] = uniformPreTail->points[i - 1];
        }
        uniformPreTail->topologyCount++;
        uniformPreTail->topology[0] = {75, 75, 0, 0};
        uniformPreTail->points[0] = {};
        uniformPreTail->points[0].kind = PROFILE_POINT_ABSOLUTE_FREQUENCY;
        uniformPreTail->points[0].absoluteFrequencyHz = 2400000000LL;
        why[0] = 0;
        if (!profile_document_structurally_valid(*uniformPreTail, why, sizeof(why)) ||
            profile_slot_document_runtime(*uniformPreTail, nullptr, *back, why, sizeof(why)) ||
            !strstr(why, "-1927 MHz"))
            return 12614;

        // 12615: uniform (exclude=0) where anchor also carries -1927 MHz -> MUST refuse.
        auto uniformAnchorSame = std::make_unique<ProfileDocument>(*uniformArt);
        uniformAnchorSame->points[0].observedOffsetHz = -1927000000LL;
        why[0] = 0;
        if (!profile_document_structurally_valid(*uniformAnchorSame, why, sizeof(why)) ||
            profile_slot_document_runtime(*uniformAnchorSame, nullptr, *back, why, sizeof(why)) ||
            !strstr(why, "-1927 MHz"))
            return 12615;

        // 12616: extreme LLONG_MIN/LLONG_MAX values do not overflow or trigger UB.
        auto extreme = std::make_unique<ProfileDocument>(makeFlatTailArtifactDoc());
        extreme->graphicsOffset.offsetHz = LLONG_MIN;
        extreme->points[1].observedOffsetHz = LLONG_MAX;
        if (profile_indexed_document_has_legacy_flat_tail_offset_artifact(*extreme))
            return 12616;

        // 12617-12622: DesiredSettings ordinary-INI helper (Windows & Linux).
        DesiredSettings iniArt;
        initialize_desired_settings_defaults(&iniArt);
        iniArt.applicationScope = DESIRED_SCOPE_LEGACY;
        iniArt.hasGpuOffset = 1;
        iniArt.gpuOffsetMHz = -1927;
        iniArt.gpuOffsetExcludeLowCount = 76;
        iniArt.hasLock = 1;
        iniArt.lockCi = 76;
        iniArt.lockMHz = 2962;
        iniArt.lockMode = LOCK_MODE_FLATTEN;
        iniArt.lockTracksAnchor = 1;
        bool hasObs[VF_NUM_POINTS]{};
        int obsKHz[VF_NUM_POINTS]{};
        for (int ci = 76; ci <= 126; ++ci) {
            iniArt.hasCurvePoint[ci] = 1;
            iniArt.curvePointMHz[ci] = 2962;
            hasObs[ci] = true;
            obsKHz[ci] = (ci == 76) ? 480000 : -1927000;
        }
        if (!profile_desired_has_legacy_flat_tail_offset_artifact(&iniArt, hasObs, obsKHz))
            return 12617;
        if (!profile_desired_has_legacy_flat_tail_offset_artifact(&iniArt, nullptr, nullptr))
            return 12618;
        DesiredSettings inRange = iniArt;
        inRange.gpuOffsetMHz = -500;
        if (profile_desired_has_legacy_flat_tail_offset_artifact(&inRange, hasObs, obsKHz))
            return 12619;
        DesiredSettings partialExcl = iniArt;
        partialExcl.gpuOffsetExcludeLowCount = 70;
        if (profile_desired_has_legacy_flat_tail_offset_artifact(&partialExcl, hasObs, obsKHz))
            return 12620;
        DesiredSettings withIniProj = iniArt;
        withIniProj.curvePointFromGpuOffset[80] = 1;
        if (profile_desired_has_legacy_flat_tail_offset_artifact(&withIniProj, hasObs, obsKHz))
            return 12621;
        profile_desired_repair_legacy_flat_tail_offset(&iniArt);
        if (!iniArt.hasGpuOffset || iniArt.gpuOffsetMHz != 0 ||
            iniArt.gpuOffsetExcludeLowCount != 0 || iniArt.lockTracksAnchor != 0)
            return 12622;
    }
    return 0;
}
