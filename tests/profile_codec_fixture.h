// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_CODEC_FIXTURE_H
#define GREEN_CURVE_PROFILE_CODEC_FIXTURE_H
#include "profile_codec.h"

static inline ProfileDocument profile_codec_fixture(int count = 4) {
    ProfileDocument d = profile_document_make_empty();
    d.domains = (1 << 9) - 1;
    strcpy(d.displayName, "Curve \"A\" \\ \xc3\xa9");
    d.pointSetPolicy = PROFILE_POINTS_REPLACE;
    d.sourceTopologyComplete = true;
    d.source.osFamily = PROFILE_SOURCE_OS_WINDOWS;
    d.source.backendFamily = 1;
    d.source.deviceId = UINT_MAX; d.source.subSystemId = 17;
    strcpy(d.source.driverVersion, "1.2.3");
    d.source.captureComplete = true;
    d.source.capturedDomains = (gc_u32)d.domains;
    d.source.omittedDomains = PROFILE_DOMAIN_FAN;
    d.source.unavailableDomains = PROFILE_DOMAIN_LOCK;
    d.source.pinKnowledge = PROFILE_PIN_UNKNOWN;
    d.topologyCount = count;
    for (int i = 0; i < count; ++i) {
        d.topology[i] = {1000 + i, i, 600000 + (long long)i * 6250, i % 3};
        auto& p = d.points[i];
        p.kind = (ProfilePointKind)(i % 4);
        if (p.kind == PROFILE_POINT_ABSOLUTE_FREQUENCY) p.absoluteFrequencyHz = LLONG_MAX;
        if (p.kind == PROFILE_POINT_CAPTURED_OFFSET) p.capturedOffsetHz = LLONG_MIN;
        p.hasObservedFrequency = p.hasObservedOffset = true;
        p.observedFrequencyHz = 1700000500; p.observedOffsetHz = -500;
    }
    d.graphicsOffset.present = true; d.graphicsOffset.offsetHz = LLONG_MIN;
    d.graphicsOffset.exclusionPolicy = PROFILE_EXCLUDE_IDENTITIES;
    d.graphicsOffset.excludedCount = count;
    for (int i = 0; i < count; ++i) d.graphicsOffset.excludedTopologyIds[i] = 1000 + count - 1 - i;
    d.hasMemoryOffset = true; d.memoryOffsetHz = 500000; d.memorySurface = CLOCK_OFFSET_SURFACE_GLOBAL;
    d.power.present = true; d.power.ratio = {498, 500};
    d.power.hasObservedDefaultMw = d.power.hasObservedCurrentMw = true;
    d.power.observedDefaultMw = 250000; d.power.observedCurrentMw = 249000;
    d.lock.present = true; d.lock.mode = LOCK_MODE_FLATTEN;
    d.lock.anchorTopologyId = 1001; d.lock.targetFrequencyHz = 1700000500;
    d.lock.trackAnchor = true; d.lock.provenance = PROFILE_LOCK_PROVENANCE_RECOGNIZED_FLATTEN;
    d.lock.hasRecognitionEvidence = true; d.lock.recognizedAnchorVoltageUv = 606250;
    d.lock.recognizedTailCount = count - 1; d.lock.recognitionToleranceHz = 1000000;
    d.lock.hasObservedPinRange = true;
    d.lock.observedPinMinimumHz = 1700000000; d.lock.observedPinMaximumHz = 1800000000;
    d.fan.present = d.fan.fanAuto = d.fan.hasCurveConfig = true;
    d.fan.fanMode = FAN_MODE_AUTO; d.fan.fanPercent = 63;
    fan_curve_set_default(&d.fan.fanCurve);
    d.fan.fanCurve.points[7].temperatureC = 98; // dormant data is retained too
    d.hasXbarOffsetHz = d.hasXbarMsvddOffsetUv = d.hasSysClkOffsetHz = d.hasVideoClkOffsetHz = true;
    d.xbarOffsetHz = LLONG_MAX; d.xbarMsvddOffsetUv = LLONG_MIN;
    d.sysClkOffsetHz = -1; d.videoClkOffsetHz = 0;
    return d;
}
#endif
