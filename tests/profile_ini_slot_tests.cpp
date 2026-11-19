// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Ordinary slots export (and import) as what the slot loader applies: curves
// saved before the curve marker, and the keys newer slots carry (application
// scope, precise values, captured points, voltage binding). Cases 9930-9949.
#include "profile_ini_slot.h"
#include <memory>
#include <cstring>
#include <string>

namespace {
std::string text(long long value) { return std::to_string(value); }
// The layout older Windows builds wrote: no curve marker, so the stored point
// MHz are either final clocks or base clocks plus the core offset.
std::string unmarked_windows(unsigned lockPointMHz, bool lock = true) {
    std::string s = "[profile1]\ngpu_offset_mhz=100\ngpu_offset_exclude_low_count=0\n";
    if (lock) s += "lock_ci=40\nlock_mhz=1900\nlock_tracks_anchor=0\nlock_mode=" + text(LOCK_MODE_FLATTEN) + "\n";
    return s + "[profile1_curve]\nformat=explicit_vf_points_v1\ngpu_offset_mhz=100\ngpu_offset_exclude_low_count=0\n"
        "point30_mhz=1700\npoint30_mv=850\npoint30_offset_khz=100000\npoint30_visible=1\n"
        "point40_mhz=" + text(lockPointMHz) + "\npoint40_mv=900\npoint40_offset_khz=100000\npoint40_visible=1\n";
}
int at(const ProfileDocument& d, int ci) {
    for (int i = 0; i < d.topologyCount; ++i) if (d.topology[i].sourceIndex == ci) return i;
    return -1;
}
bool kind(const ProfileDocument& d, int ci, ProfilePointKind expected) {
    const int i = at(d, ci);
    return i >= 0 && d.points[i].kind == expected;
}
// An explicit-scope record written the way the ordinary slot writers write
// one: present domains only, absolute_with_origin points, then the extension
// keys from the writers' own helpers.
std::string ordinary_slot(int slot, const DesiredSettings& d) {
    const std::string root = "profile" + text(slot);
    std::string controls = "[" + root + "]\n", curve = "[" + root + "_curve]\nformat=explicit_vf_points_v1\n";
    const auto writer = [](std::string& out) {
        return [&out](const char* key, const char* value) { out += key; out += '='; out += value; out += '\n'; return true; };
    };
    if (d.hasGpuOffset) {
        const std::string g = "gpu_offset_mhz=" + text(d.gpuOffsetMHz) + "\ngpu_offset_exclude_low_count=" +
            text(d.gpuOffsetExcludeLowCount) + "\n";
        controls += g; curve += g;
    }
    if (d.hasLock)
        controls += "lock_ci=" + text(d.lockCi) + "\nlock_mhz=" + text(d.lockMHz) + "\nlock_tracks_anchor=" +
            text(d.lockTracksAnchor ? 1 : 0) + "\nlock_mode=" + text(d.lockMode) + "\n";
    if (d.hasMemOffset) controls += "mem_offset_mhz=" + text(d.memOffsetMHz) + "\n";
    if (d.hasPowerLimit) controls += "power_limit_pct=" + text(d.powerLimitPct) + "\n";
    desired_ini_append_controls_ext(writer(controls), &d);
    curve += "curve_semantics=absolute_with_origin\n";
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        if (!d.hasCurvePoint[ci] || !d.curvePointMHz[ci]) continue;
        curve += "point" + text(ci) + "_mhz=" + text(d.curvePointMHz[ci]) + "\n";
        if (d.curvePointFromGpuOffset[ci]) curve += "point" + text(ci) + "_from_gpu_offset=1\n";
    }
    desired_ini_append_curve_ext(writer(curve), &d);
    return controls + curve;
}
bool same_curve(const DesiredSettings& a, const DesiredSettings& b) {
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        if (!!a.hasCurvePoint[ci] != !!b.hasCurvePoint[ci] || !!a.curvePointCaptured[ci] != !!b.curvePointCaptured[ci] ||
            !!a.curvePointFromGpuOffset[ci] != !!b.curvePointFromGpuOffset[ci] ||
            !!a.authoring.graphicsExcluded[ci] != !!b.authoring.graphicsExcluded[ci] ||
            a.authoring.voltageUv[ci] != b.authoring.voltageUv[ci]) return false;
        if (a.hasCurvePoint[ci] && a.curvePointMHz[ci] != b.curvePointMHz[ci]) return false;
        if (a.curvePointCaptured[ci] && a.curvePointCapturedOffsetHz[ci] != b.curvePointCapturedOffsetHz[ci]) return false;
        if (a.authoring.pointFrequencyPrecise[ci] && a.authoring.pointFrequencyHz[ci] != b.authoring.pointFrequencyHz[ci])
            return false;
    }
    return true;
}
}  // namespace

int run_profile_ini_slot_tests() {
    ProfileCodecError error{};
    bool found = false;
    auto doc = std::make_unique<ProfileDocument>();
    // 9930: an unmarked slot whose lock point plus the core offset lands on the
    // lock frequency was saved as base clocks; export reads it as the loader does.
    const std::string base = unmarked_windows(1800);
    if (!profile_legacy_native_slot_decode(base, PROFILE_LEGACY_WINDOWS_INI, 1, *doc, found, &error) || !found ||
        !doc->graphicsOffset.present || doc->graphicsOffset.offsetHz != 100000000LL ||
        !kind(*doc, 30, PROFILE_POINT_PROJECTION) || !kind(*doc, 40, PROFILE_POINT_PROJECTION) ||
        !doc->lock.present || doc->lock.targetFrequencyHz != 1900000000LL) {
        fprintf(stderr, "9930: %s\n", error.message);
        return 9930;
    }
    // 9931: the same text in a file being imported still asks how it was saved.
    ProfileLegacyOptions windows; windows.format = PROFILE_LEGACY_WINDOWS_INI;
    auto bank = std::make_unique<ProfileBank>();
    if (profile_legacy_ini_decode(base, windows, ProfileSourceSelection{}, *bank, &error) ||
        !strstr(error.message, "curve-semantics choice"))
        return 9931;
    // 9932: a slot whose stored lock point already is the lock frequency holds final clocks.
    if (!profile_legacy_native_slot_decode(unmarked_windows(1900), PROFILE_LEGACY_WINDOWS_INI, 1, *doc, found, &error) ||
        !kind(*doc, 40, PROFILE_POINT_ABSOLUTE_FREQUENCY) || doc->points[at(*doc, 40)].absoluteFrequencyHz != 1900000000LL ||
        doc->points[at(*doc, 30)].absoluteFrequencyHz != 1700000000LL)
        return 9932;
    // 9933: without a lock the loader keeps final clocks too.
    if (!profile_legacy_native_slot_decode(unmarked_windows(1800, false), PROFILE_LEGACY_WINDOWS_INI, 1, *doc, found, &error) ||
        !kind(*doc, 40, PROFILE_POINT_ABSOLUTE_FREQUENCY) || doc->points[at(*doc, 40)].absoluteFrequencyHz != 1800000000LL)
        return 9933;
    // 9934: Linux cannot tell either, so its loader keeps the offset and drops
    // the points; without an offset the points are final clocks.
    const std::string linuxOffset = "[profile2]\ngpu_offset_mhz=60\n[profile2_curve]\npoint30=1700\npoint40=1800\n";
    if (!profile_legacy_native_slot_decode(linuxOffset, PROFILE_LEGACY_LINUX_INI, 2, *doc, found, &error) || !found ||
        doc->graphicsOffset.offsetHz != 60000000LL || (doc->domains & PROFILE_DOMAIN_VF_CURVE) || doc->topologyCount)
        return 9934;
    if (!profile_legacy_native_slot_decode("[profile2]\n[profile2_curve]\npoint30=1700\n", PROFILE_LEGACY_LINUX_INI, 2,
            *doc, found, &error) || !kind(*doc, 30, PROFILE_POINT_ABSOLUTE_FREQUENCY))
        return 9935;
    // 9936: the exported base-clock slot imports back without a GPU, with its
    // points still following the core offset.
    auto record = std::make_unique<DesiredSettings>();
    char why[192]{};
    if (!profile_legacy_native_slot_decode(base, PROFILE_LEGACY_WINDOWS_INI, 1, *doc, found, &error) ||
        !profile_slot_document_runtime(*doc, nullptr, *record, why, sizeof(why)) || !record->hasGpuOffset ||
        record->gpuOffsetMHz != 100 || !record->curvePointFromGpuOffset[40] || !record->hasLock || record->lockMHz != 1900) {
        fprintf(stderr, "9936: %s\n", why);
        return 9936;
    }

    // 9937: a slot with precise values and captured points (written by the
    // capture dialog or an import) exports instead of failing on its keys.
    auto exact = std::make_unique<DesiredSettings>();
    initialize_desired_settings_defaults(exact.get());
    exact->applicationScope = DESIRED_SCOPE_REPLACE_PROFILE; exact->lockCi = -1;
    exact->hasFan = 0; exact->fanAuto = 1; exact->fanMode = FAN_MODE_AUTO;
    exact->curvePointCaptured[50] = 1; exact->curvePointCapturedOffsetHz[50] = 30500000LL;
    exact->hasCurvePoint[51] = 1; exact->curvePointMHz[51] = 1750;
    exact->authoring.pointFrequencyPrecise[51] = 1; exact->authoring.pointFrequencyHz[51] = 1750500000LL;
    if (!desired_memory_set_precise(exact.get(), 1000500000LL, CLOCK_OFFSET_SURFACE_GLOBAL) ||
        !desired_power_set_ratio(exact.get(), tuning_power_ratio_reduce(7, 8)) || !desired_curve_intent_valid(exact.get()) ||
        !desired_scalar_intent_valid(exact.get()) || !desired_authoring_valid(exact.get()))
        return 9937;
    const std::string exactSlot = ordinary_slot(3, *exact);
    if (!profile_legacy_native_slot_decode(exactSlot, PROFILE_LEGACY_WINDOWS_INI, 3, *doc, found, &error) || !found ||
        !kind(*doc, 50, PROFILE_POINT_CAPTURED_OFFSET) || doc->points[at(*doc, 51)].absoluteFrequencyHz != 1750500000LL ||
        doc->memoryOffsetHz != 1000500000LL || doc->applicationScope != PROFILE_SCOPE_REPLACE_PROFILE) {
        fprintf(stderr, "9937: %s\n", error.message);
        return 9938;
    }
    // 9939: ... and imports back as the same record, without a GPU.
    if (!profile_slot_document_runtime(*doc, nullptr, *record, why, sizeof(why)) || !same_curve(*exact, *record) ||
        record->applicationScope != DESIRED_SCOPE_REPLACE_PROFILE || record->memOffsetHz != 1000500000LL ||
        record->memOffsetSurface != CLOCK_OFFSET_SURFACE_GLOBAL ||
        !tuning_power_ratio_equal(desired_power_ratio(*record), tuning_power_ratio_reduce(7, 8))) {
        fprintf(stderr, "9939: %s\n", why);
        return 9939;
    }
    // 9940: another installation's settings file with such a slot imports too.
    if (!profile_legacy_ini_decode(exactSlot, windows, ProfileSourceSelection{}, *bank, &error) || bank->count != 1 ||
        bank->documents[0].documentId != 3 || !kind(bank->documents[0], 50, PROFILE_POINT_CAPTURED_OFFSET))
        return 9940;

    // 9941: a capture bound to its GPU's voltages exports by those voltages,
    // with its graphics exclusions named by identity.
    VFCurvePoint curve[VF_NUM_POINTS]{};
    for (int ci = 0; ci < 100; ++ci) {
        curve[ci].volt_uV = 600000u + (unsigned)ci * 6250u;
        curve[ci].freq_kHz = 900000u + (unsigned)ci * 15000u;
    }
    const VfFloorTopology floor = vf_floor_topology_compute(curve, nullptr, VF_NUM_POINTS);
    auto live = std::make_unique<ProfileDestinationTopology>();
    if (!profile_destination_from_vf_curve(curve, floor, *live)) return 9941;
    auto capture = std::make_unique<ProfileDocument>(profile_document_make_empty());
    capture->applicationScope = PROFILE_SCOPE_REPLACE_PROFILE;
    capture->domains = PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_GRAPHICS_OFFSET;
    capture->graphicsOffset.present = true; capture->graphicsOffset.offsetHz = 45000000LL;
    capture->graphicsOffset.exclusionPolicy = PROFILE_EXCLUDE_IDENTITIES;
    capture->graphicsOffset.excludedCount = 1; capture->graphicsOffset.excludedTopologyIds[0] = 1;
    for (int i = 0; i < 3; ++i) {
        capture->topology[i] = {i + 1, 90 + i, 600000LL + (60 + i) * 6250LL, 1};
        if (i) { capture->points[i].kind = PROFILE_POINT_CAPTURED_OFFSET; capture->points[i].capturedOffsetHz = 15000000LL * i; }
    }
    capture->topologyCount = 3;
    auto bound = std::make_unique<DesiredSettings>();
    if (!profile_slot_document_runtime(*capture, live.get(), *bound, why, sizeof(why)) || !bound->authoring.topologyBound ||
        !bound->authoring.graphicsExcluded[60] || !bound->curvePointCaptured[61]) {
        fprintf(stderr, "9941: %s\n", why);
        return 9942;
    }
    const std::string boundSlot = ordinary_slot(4, *bound);
    if (!profile_legacy_native_slot_decode(boundSlot, PROFILE_LEGACY_WINDOWS_INI, 4, *doc, found, &error) ||
        doc->legacyIndicesUnresolved || doc->topology[at(*doc, 61)].voltageUv != 600000LL + 61 * 6250LL ||
        doc->graphicsOffset.exclusionPolicy != PROFILE_EXCLUDE_IDENTITIES || doc->graphicsOffset.excludedCount != 1 ||
        doc->graphicsOffset.excludedTopologyIds[0] != 60 || doc->graphicsOffset.offsetHz != 45000000LL) {
        fprintf(stderr, "9943: %s\n", error.message);
        return 9943;
    }
    // 9944: on the same GPU it imports back as the same record; without the GPU
    // it refuses with a reason instead of guessing positions.
    if (!profile_slot_document_runtime(*doc, live.get(), *record, why, sizeof(why)) || !same_curve(*bound, *record) ||
        !record->authoring.topologyBound || record->authoring.graphicsOffsetHz != 45000000LL) {
        fprintf(stderr, "9944: %s\n", why);
        return 9944;
    }
    why[0] = 0;
    if (profile_slot_document_runtime(*doc, nullptr, *record, why, sizeof(why)) || !strstr(why, "needs the GPU")) return 9945;
    // 9946: a contradictory extension key is refused with the loader's reason.
    if (profile_legacy_native_slot_decode(exactSlot + "point60_captured_offset_hz=5\n", PROFILE_LEGACY_WINDOWS_INI, 3,
            *doc, found, &error) || !strstr(error.message, "captured"))
        return 9946;
    // 9947: an empty slot is not found and is not an error.
    if (!profile_legacy_native_slot_decode(base, PROFILE_LEGACY_WINDOWS_INI, 5, *doc, found, &error) || found) return 9947;
    // 9948: the exclusion count Windows copies into the curve section must
    // agree with the one the loader reads.
    std::string disagree = base;
    disagree.replace(disagree.rfind("gpu_offset_exclude_low_count=0"), 30, "gpu_offset_exclude_low_count=3");
    if (profile_legacy_native_slot_decode(disagree, PROFILE_LEGACY_WINDOWS_INI, 1, *doc, found, &error) ||
        !strstr(error.message, "exclusions disagree"))
        return 9948;
    return 0;
}
