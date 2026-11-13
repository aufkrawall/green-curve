// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "profile_document.h"

static ProfileDocument complete_document() {
    ProfileDocument d = profile_document_make_empty();
    d.domains = (1 << 9) - 1;
    d.sourceTopologyComplete = true;
    d.topologyCount = 3;
    d.topology[0] = {10, 2, 700000, 1};
    d.topology[1] = {11, 4, 725000, 1};
    d.topology[2] = {12, 6, 750000, 2};
    d.points[0].kind = PROFILE_POINT_CAPTURED_OFFSET;
    d.points[1].kind = PROFILE_POINT_ABSOLUTE_FREQUENCY;
    d.points[1].absoluteFrequencyHz = 1700000000;
    d.points[2].kind = PROFILE_POINT_PROJECTION;
    d.graphicsOffset.present = true;
    d.graphicsOffset.offsetHz = 25000000;
    d.graphicsOffset.exclusionPolicy = PROFILE_EXCLUDE_IDENTITIES;
    d.graphicsOffset.excludedCount = 2;
    d.graphicsOffset.excludedTopologyIds[0] = 10; d.graphicsOffset.excludedTopologyIds[1] = 12;
    d.hasMemoryOffset = true; d.memoryOffsetHz = 500000; d.memorySurface = CLOCK_OFFSET_SURFACE_P0;
    d.power.present = true; d.power.ratio = {249, 250};
    d.power.hasObservedDefaultMw = d.power.hasObservedCurrentMw = true;
    d.power.observedDefaultMw = 250000; d.power.observedCurrentMw = 249000;
    d.lock.present = true; d.lock.mode = LOCK_MODE_FLATTEN;
    d.lock.anchorTopologyId = 11; d.lock.targetFrequencyHz = 1700000000;
    d.fan.present = d.fan.hasCurveConfig = true;
    d.fan.fanMode = FAN_MODE_CURVE; d.fan.fanPercent = 50;
    fan_curve_set_default(&d.fan.fanCurve);
    d.hasXbarOffsetHz = d.hasXbarMsvddOffsetUv = d.hasSysClkOffsetHz = d.hasVideoClkOffsetHz = true;
    d.xbarOffsetHz = 1000; d.xbarMsvddOffsetUv = -500; d.sysClkOffsetHz = 2000; d.videoClkOffsetHz = 3000;
    return d;
}

int run_profile_document_contract_tests() {
    const ProfileDocument base = complete_document();
    ProfileDocument d = base;
    char why[160]{};
    const auto valid = [&] { return profile_document_structurally_valid(d, why, sizeof(why)); };
    if (!valid()) return 8280;
    d.documentVersion = 2; if (valid()) return 8281;
    d = base; d.applicationScope = (ProfileApplicationScope)9; if (valid()) return 8282;
    d = base; d.domains |= 1 << 20; if (valid()) return 8283;
    d = base; d.domains &= ~PROFILE_DOMAIN_MEMORY; if (valid()) return 8284;
    d = base; d.points[3].kind = PROFILE_POINT_CAPTURED_OFFSET; if (valid()) return 8285;
    d = base; d.topology[2].topologyId = 10; if (valid()) return 8286;
    d = base; d.topology[0].sourceIndex = VF_NUM_POINTS; if (valid()) return 8287;
    d = base; d.topology[0].voltageUv = 0; if (valid()) return 8288;
    d = base; d.sourceTopologyComplete = false; d.legacyIndicesUnresolved = true;
    for (int i = 0; i < d.topologyCount; ++i) d.topology[i].voltageUv = 0;
    if (!valid()) return 8289;
    d.sourceTopologyComplete = true; if (valid()) return 8290;
    d = base; d.graphicsOffset.present = false; d.domains &= ~PROFILE_DOMAIN_GRAPHICS_OFFSET;
    if (valid()) return 8291;
    d = base; d.graphicsOffset.excludedTopologyIds[1] = 10; if (valid()) return 8292;
    d = base; d.graphicsOffset.excludedTopologyIds[1] = 99; if (valid()) return 8293;
    d = base; d.graphicsOffset.excludedTopologyIds[0] = 12; d.graphicsOffset.excludedTopologyIds[1] = 10;
    if (!valid() || !profile_document_semantically_equal(base, d)) return 8294;
    d = base; d.memoryOffsetHz = LLONG_MIN; d.xbarOffsetHz = LLONG_MAX;
    if (!valid()) return 8295; // storage does not impose live hardware bounds
    d = base; d.lock.mode = (LockMode)9; if (valid()) return 8296;
    d = base; d.lock.targetFrequencyHz = 0; if (valid()) return 8297;
    d = base; d.lock.hasRecognitionEvidence = true; d.lock.recognitionToleranceHz = -1;
    d.lock.recognizedAnchorVoltageUv = 725000; d.lock.recognizedTailCount = 2;
    if (valid()) return 8298;
    d.lock.recognitionToleranceHz = 1000000;
    if (!valid() || !profile_document_semantically_equal(base, d)) return 8299;
    d = base; d.lock.hasObservedPinRange = true; d.lock.observedPinMinimumHz = 1700000000;
    d.lock.observedPinMaximumHz = 1600000000; if (valid()) return 8300;
    d.lock.observedPinMaximumHz = 1700000000; if (!valid()) return 8301;
    d = base; d.fan.fanMode = 9; if (valid()) return 8302;
    d = base; d.fan.hasCurveConfig = false; if (valid()) return 8303;
    d = base; d.fan.fanCurve.pollIntervalMs = 249; if (valid()) return 8304;
    d = base; d.fan.fanCurve.points[0].enabled = 2; if (valid()) return 8305;
    d = base; strcpy(d.source.driverVersion, "\xc3"); if (valid()) return 8306;
    if (profile_utf8_valid("\xc0\xaf", 2) || profile_utf8_valid("\xed\xa0\x80", 3) ||
        profile_utf8_valid("\xf4\x90\x80\x80", 4) || profile_utf8_valid("\x80", 1) ||
        !profile_utf8_valid("Curve \xc3\xa9", 8)) return 8307;
    d = base; memset(d.displayName, 'x', PROFILE_DOCUMENT_MAX_DISPLAY_NAME);
    d.displayName[PROFILE_DOCUMENT_MAX_DISPLAY_NAME] = 0;
    if (!valid()) return 8308;
    d.displayName[PROFILE_DOCUMENT_MAX_DISPLAY_NAME] = 'x'; if (valid()) return 8309;
    d = profile_document_make_empty(); d.applicationScope = PROFILE_SCOPE_EXPLICIT_RELEASE;
    d.domains = PROFILE_DOMAIN_POWER; if (!valid()) return 8310;
    d = base; d.points[0].kind = PROFILE_POINT_ABSENT;
    if (profile_document_semantically_equal(base, d)) return 8311;
    d.points[0].hasObservedFrequency = d.points[0].hasObservedOffset = true;
    d.points[0].observedFrequencyHz = 1000000000; d.points[0].observedOffsetHz = -15000500;
    const ProfilePointIntent point = profile_point_intent_canonical(d.points[0]);
    if (!point.hasObservedFrequency || !point.hasObservedOffset || point.observedOffsetHz != -15000500 ||
        !profile_point_intent_semantically_equal(point, ProfilePointIntent{})) return 8312;
    d = base; d.points[1].capturedOffsetHz = 1; if (valid()) return 8313;
    d = base; d.power.ratio = {498, 500}; d.power.observedDefaultMw = 300000;
    if (!valid() || !profile_document_semantically_equal(base, d)) return 8314;
    d = base; d.pointSetPolicy = PROFILE_POINTS_REPLACE;
    if (profile_document_semantically_equal(base, d)) return 8315;
    d = base; d.memorySurface = CLOCK_OFFSET_SURFACE_GLOBAL;
    if (profile_document_semantically_equal(base, d)) return 8316;
    d = base; d.topology[0].sourceWritable = 2;
    if (!profile_document_semantically_equal(base, d)) return 8317;
    if (!profile_utf8_valid("a\r\nb", 4, true) || profile_utf8_valid("a\r\nb", 4) ||
        profile_utf8_valid("a\0b", 3, true)) return 8318;
    d = base; d.source.unavailableDomains = 1u << 20; if (valid()) return 8319;
    d = base; d.lock.present = false; d.domains &= ~PROFILE_DOMAIN_LOCK;
    d.lock.hasRecognitionEvidence = true; d.lock.recognizedAnchorVoltageUv = 725000;
    d.lock.recognizedTailCount = 2; d.lock.recognitionToleranceHz = 1000000;
    if (!valid()) return 8320;
    d = base; d.power.present = false; d.domains &= ~PROFILE_DOMAIN_POWER;
    if (!valid() || !d.power.hasObservedDefaultMw || !d.power.hasObservedCurrentMw) return 8321;
    return 0;
}
