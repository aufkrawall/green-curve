// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Imported (document) slots must load as the same runtime record every apply
// path uses -- the tray, hotkeys, auto-switch, logon and the editor's Load --
// instead of being refused. Cases 9950-9969.
#include "profile_slot_runtime.h"
#include "profile_legacy_document.h"
#include "profile_codec.h"
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
    return 0;
}
