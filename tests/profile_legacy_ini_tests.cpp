// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "profile_legacy_ini.h"
#include "profile_mapping.h"

int run_profile_legacy_ini_tests() {
    auto bank = std::make_unique<ProfileBank>();
    ProfileCodecError error;
    ProfileSourceSelection selection;
    ProfileLegacyOptions windows{PROFILE_LEGACY_WINDOWS_INI}, linux{PROFILE_LEGACY_LINUX_INI};
    const std::string input =
        "[meta]\nformat_version=2\n[profile1]\ngpu_offset_mhz=-25\ngpu_offset_exclude_low_count=7\n"
        "mem_offset_mhz=-1001\npower_limit_pct=99\nlock_ci=22\nlock_mhz=1000\nlock_mode=1\nlock_tracks_anchor=1\n"
        "xbar_offset_khz=-250\nxbar_msvdd_offset_uv=0\nsys_clk_offset_khz=500\nvideo_clk_offset_khz=-750\n"
        "fan_mode=fixed\nfan=40\nfan_fixed_pct=40\n[profile1_curve]\nformat=explicit_vf_points_v1\n"
        "curve_semantics=absolute_with_origin\ngpu_offset_mhz=-25\n"
        "point3_mhz=1000\npoint3_mv=650\npoint3_offset_khz=-5000\npoint3_visible=0\n"
        "point4_mhz=1025\npoint4_from_gpu_offset=1\n"
        "[profile4]\nmem_offset_mhz=2\n[preferences]\nignored=unchanged\n";
    if (!profile_legacy_ini_decode(input, windows, selection, *bank, &error) || bank->count != 2) return 8986;
    const auto& d = bank->documents[0];
    if (d.domains != 511 || d.source.osFamily != PROFILE_SOURCE_OS_WINDOWS || d.source.captureComplete ||
        d.sourceTopologyComplete || !d.legacyIndicesUnresolved || d.topologyCount != 3 ||
        d.topology[0].sourceIndex != 3 || d.topology[0].voltageUv || d.topology[0].sourceWritable ||
        d.graphicsOffset.offsetHz != -25000000 || d.graphicsOffset.excludeLowCount != 7 ||
        d.graphicsOffset.exclusionPolicy != PROFILE_EXCLUDE_LEGACY_COUNT ||
        d.memoryOffsetHz != -1001000000 || d.power.ratio.numerator != 99 || d.power.ratio.denominator != 100 ||
        d.xbarOffsetHz != -250000 || !d.hasXbarMsvddOffsetUv || d.xbarMsvddOffsetUv ||
        d.sysClkOffsetHz != 500000 || d.videoClkOffsetHz != -750000 ||
        d.points[0].kind != PROFILE_POINT_ABSOLUTE_FREQUENCY || d.points[0].absoluteFrequencyHz != 1000000000 ||
        !d.points[0].hasObservedOffset || d.points[0].observedOffsetHz != -5000000 ||
        d.points[1].kind != PROFILE_POINT_PROJECTION || !d.points[1].hasObservedFrequency ||
        d.points[1].observedFrequencyHz != 1025000000 || d.points[1].capturedOffsetHz ||
        !d.lock.present || d.lock.anchorTopologyId != 22 || d.lock.targetFrequencyHz != 1000000000 ||
        !d.lock.trackAnchor || d.fan.fanMode != FAN_MODE_FIXED || d.fan.fanPercent != 40) return 8987;
    std::string encoded, again;
    auto decoded = std::make_unique<ProfileBank>();
    if (!profile_bank_encode(*bank, encoded) || !profile_bank_decode(encoded, *decoded) ||
        !profile_document_semantically_equal(d, decoded->documents[0]) ||
        !profile_bank_encode(*decoded, again) || encoded != again) return 8988;
    ProfileDestinationTopology destination;
    destination.available = destination.complete = true; destination.count = 3;
    destination.points[0] = {3, 650000, true, true};
    destination.points[1] = {4, 656250, true, true};
    destination.points[2] = {22, 700000, true, true};
    if (profile_map_topology(d, destination).ready()) return 8989;
    selection.all = false; selection.count = 2; selection.slots[0] = 4; selection.slots[1] = 1;
    if (!profile_legacy_ini_decode(input, windows, selection, *bank) ||
        bank->documents[0].memoryOffsetHz != 2000000 || bank->documents[1].memoryOffsetHz != -1001000000) return 8990;
    selection = {};
    auto refuses = [&](const std::string& bytes, ProfileLegacyOptions options) {
        bank->count = 1; bank->documents[0] = profile_document_make_empty();
        strcpy(bank->documents[0].displayName, "unchanged");
        return !profile_legacy_ini_decode(bytes, options, selection, *bank, &error) &&
            bank->count == 1 && !strcmp(bank->documents[0].displayName, "unchanged") && error.message[0];
    };
    if (!refuses("[profile1]\nmem_offset_mhz=-1001\n", linux) ||
        !strstr(error.message, "source-unit")) return 8991;
    linux.memory = PROFILE_LEGACY_MEMORY_EFFECTIVE;
    if (!profile_legacy_ini_decode("[profile1]\nmem_offset_mhz=-1001\n", linux, selection, *bank) ||
        bank->documents[0].memoryOffsetHz != -500500000) return 8992;
    if (!refuses("[meta]\nlinux_mem_migrated=1\n[profile1]\nmem_offset_mhz=-1001\n", linux)) return 8993;
    linux.memory = PROFILE_LEGACY_MEMORY_UNKNOWN;
    if (!profile_legacy_ini_decode("[meta]\nlinux_mem_migrated=1\n[profile1]\nmem_offset_mhz=-1001\n",
        linux, selection, *bank) || bank->documents[0].memoryOffsetHz != -1001000000) return 8994;
    if (!refuses("[meta]\nlinux_mem_migrated=0\n[profile1]\nmem_offset_mhz=1\n", linux) ||
        !refuses("[profile1_curve]\npoint3=1000\n", linux) || !strstr(error.message, "curve-semantics")) return 8995;
    linux.curve = PROFILE_LEGACY_CURVE_ABSOLUTE;
    if (!profile_legacy_ini_decode("[curve]\npoint3=1000\n", linux, selection, *bank) ||
        bank->documents[0].points[0].absoluteFrequencyHz != 1000000000) return 8996;
    if (!refuses("[profile1_curve]\ncurve_semantics=base_plus_gpu_offset\npoint3=1000\n", linux)) return 8997;
    linux.curve = PROFILE_LEGACY_CURVE_UNKNOWN;
    const std::string base = "[controls]\ngpu_offset_mhz=25\n[curve]\ncurve_semantics=base_plus_gpu_offset\npoint3=1000\n";
    if (!profile_legacy_ini_decode(base, linux, selection, *bank) ||
        bank->documents[0].points[0].kind != PROFILE_POINT_PROJECTION ||
        bank->documents[0].points[0].hasObservedFrequency || bank->documents[0].points[0].absoluteFrequencyHz) return 8998;
    const std::string malformed[] = {
        "[profile1]\nmem_offset_mhz=1\nMEM_OFFSET_MHZ=2\n",
        "[profile1]\nmem_offset_mhz=1\n[Profile1]\npower_limit_pct=90\n",
        "[profile1]\nmem_offset_mhz=2147483648\n", "[profile1]\npower_limit_pct=0\n",
        "[profile1]\nlock_ci=22\n", "[profile1]\nlock_mhz=1000\n",
        "[profile1]\nlock_ci=22\nlock_mhz=1000\nlock_mode=0\n",
        "[profile1]\ngpu_offset_exclude_low_count=4\n",
        "[profile1_curve]\npoint3_mhz=1000\npoint3_from_gpu_offset=1\n",
        "[profile1_curve]\ncurve_semantics=unknown\npoint3_mhz=1000\n",
        "[profile1_curve]\nformat=unknown\npoint3_mhz=1000\n",
        "[profile1_curve]\nformat=explicit_vf_points_v1\npoint3_mv=650\n",
        "[profile1_curve]\nformat=explicit_vf_points_v1\npoint3_mhz=1000\n",
        "[profile1_curve]\nformat=explicit_vf_points_v1\npoint128_mhz=1000\n",
        "[profile1_curve]\nformat=explicit_vf_points_v1\npoint3=1000\n",
        "[profile1]\ngpu_offset_mhz=25\n[profile1_curve]\ngpu_offset_mhz=26\n",
        "[profile1]\ndocument.version=1\n", "[profile1]\nunknown_tuning=1\n",
        "[profile6]\nmem_offset_mhz=0\n", "[profile10]\nmem_offset_mhz=0\n",
        "[meta]\nformat_version=3\n[profile1]\nmem_offset_mhz=1\n",
        "[profile1]\nfan_mode=fixed\n", "[profile1]\nfan=40\nfan_fixed_pct=50\n",
        "[profile1]\nfan_mode=curve\n", "[profile1_fan_curve]\nzero_rpm_enabled=1\n",
        "[profile1]\nfan_mode=auto\n[profile1_fan_curve]\nunknown=1\n",
        "[profile1]\nmem_offset_mhz=1.5\n", "[profile1]\nmem_offset_mhz=1x\n",
        "[profile1]\nmem_offset_mhz=+1\n"
    };
    for (const auto& bytes : malformed) if (!refuses(bytes, windows)) return 8999;
    if (!refuses(std::string("[profile1]\n") + std::string(4097, 'x'), windows) ||
        !refuses(std::string("[profile1]\nmem_offset_mhz=1\0junk", 33), windows)) return 9000;
    std::string utf16("\xff\xfe", 2);
    for (char c : std::string("[PrOfIlE1]\r\nMeM_OfFsEt_MhZ=-1001\r\n")) { utf16 += c; utf16 += '\0'; }
    if (!profile_legacy_ini_decode(utf16, windows, selection, *bank) ||
        bank->documents[0].memoryOffsetHz != -1001000000) return 9001;
    if (!refuses(utf16 + std::string(1, '\0'), windows) ||
        !refuses(utf16 + std::string("\x00\xd8\x00\x00", 4), windows)) return 9002;
    selection.all = false; selection.count = 1; selection.slots[0] = 4;
    if (!refuses("[profile1]\nmem_offset_mhz=1\n[profile4]\n", windows)) return 9003;
    selection = {};
    if (!profile_legacy_ini_decode("[profile1]\n[controls]\nmem_offset_mhz=2\n", windows, selection, *bank) ||
        bank->count != 1 || bank->documents[0].memoryOffsetHz != 2000000) return 9004;
    const std::string fan = "[profile1]\nfan_mode=curve\nfan=auto\nfan_fixed_pct=40\n"
        "[profile1_fan_curve]\nzero_rpm_enabled=1\nhysteresis_c=3\n";
    if (!profile_legacy_ini_decode(fan, windows, selection, *bank, &error) ||
        !bank->documents[0].fan.hasCurveConfig || bank->documents[0].fan.fanCurve.zeroRpmHysteresisC != 3) return 9005;
    bool found = false; ProfileDocument native{};
    if (!profile_legacy_native_slot_decode(input, PROFILE_LEGACY_WINDOWS_INI, 1, native, found, &error) || !found ||
        native.memoryOffsetHz != -1001000000 ||
        !profile_legacy_native_slot_decode(input, PROFILE_LEGACY_WINDOWS_INI, 2, native, found, &error) || found) return 9006;
    if (!refuses("[profile1]\nlock_ci=128\nlock_mhz=1000\n", windows)) return 9007;
    windows.memory = (ProfileLegacyMemoryUnit)9;
    if (!refuses(input, windows)) return 9008;
    fprintf(stdout, "legacy profile adapters: units, origins, unresolved indices, strict parsing and atomic decode passed\n");
    return 0;
}
#ifdef GC_PROFILE_LEGACY_FIXTURE_MAIN
int main() {
    const int code = run_profile_legacy_ini_tests();
    if (code) fprintf(stderr, "legacy profile regression: %d\n", code);
    return code;
}
#endif
