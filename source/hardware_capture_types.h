// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_HARDWARE_CAPTURE_TYPES_H
#define GREEN_CURVE_HARDWARE_CAPTURE_TYPES_H
#include "gpu_wire_types.h"
#include "profile_domain_mask.h"

// Fixed version-one observation layout. It contains no pointers, C++ bool,
// ownership claim, file path, permission or automatic-restore acknowledgment.
enum HardwareCaptureStatus : gc_u32 {
    CAPTURE_NOT_REQUESTED, CAPTURE_COMPLETE, CAPTURE_UNSUPPORTED, CAPTURE_READ_FAILED,
    CAPTURE_STATE_CHANGED, CAPTURE_COMPOSITION_UNRESOLVED, CAPTURE_PARTIAL
};
enum HardwareCaptureComposition : gc_u32 {
    CAPTURE_COMPOSITION_UNKNOWN, CAPTURE_COMPOSITION_INDEPENDENT, CAPTURE_COMPOSITION_ALIASED
};
struct HardwareCaptureBinding {
    gc_bool8 valid, pciInfoValid;
    gc_u8 reserved[2];
    gc_u32 deviceId, subSystemId, pciDomain, pciBus, pciDevice, pciFunction, nvapiIndex, nvmlIndex;
};
struct HardwareCaptureScalar {
    gc_bool8 valid, rangeValid, writable;
    gc_u8 reserved;
    gc_u32 surface;
    int32_t status, rangeStatus;
    gc_i64 value, minimum, maximum, grid;
};
struct HardwareCapturePower {
    gc_bool8 currentValid, defaultValid, rangeValid, writable;
    int32_t currentStatus, defaultStatus, rangeStatus;
    gc_i64 currentMw, defaultMw, minimumMw, maximumMw, gridMw;
};
struct HardwareCapturePoint {
    gc_u32 index;
    gc_bool8 offsetValid, frequencyValid;
    gc_u8 writableState, reserved; // 0 unknown, 1 writable, 2 read-only/fixed floor
    gc_i64 voltageUv, offsetHz, frequencyHz;
};
struct HardwareCaptureFanPoint {
    gc_bool8 enabled;
    gc_u8 reserved[3];
    int32_t temperatureC, dutyPct;
};
struct HardwareCaptureFan {
    gc_bool8 valid, curvePresent, zeroRpmEnabled;
    gc_u8 reserved;
    gc_u32 mode, dutyPct, pollIntervalMs, hysteresisC, zeroRpmHysteresisC;
    HardwareCaptureFanPoint points[8];
};
struct HardwareCaptureObservation {
    gc_bool8 valid, topologyComplete, pinGetterValid, pinPresent;
    gc_u8 reserved[4];
    gc_u32 schemaVersion, requestedDomains, capturedDomains, omittedDomains, unavailableDomains;
    gc_u32 domainStatus[9];
    int32_t driverStatus[9];
    gc_u32 osFamily, backendFamily, composition, pointCount;
    gc_u32 alignmentReserved;
    gc_u64 gpuGeneration, revision, fingerprint;
    HardwareCaptureBinding binding;
    char driverVersion[65];
    gc_u8 textReserved[3];
    HardwareCaptureScalar graphics, memory, xbarClock, xbarVoltage, systemClock, videoClock;
    HardwareCapturePower power;
    gc_i64 pinMinimumHz, pinMaximumHz;
    int32_t pinStatus;
    gc_u32 readAttempts;
    HardwareCaptureFan fan;
    HardwareCapturePoint points[128];
};
static_assert(sizeof(HardwareCapturePoint) == 32, "capture point wire layout changed");
static_assert(sizeof(HardwareCaptureScalar) == 48, "capture scalar wire layout changed");
static_assert(sizeof(HardwareCapturePower) == 56, "capture power wire layout changed");
static_assert(sizeof(HardwareCaptureFan) == 120, "capture fan wire layout changed");
static_assert(sizeof(HardwareCaptureBinding) == 36, "capture binding wire layout changed");
static_assert(sizeof(HardwareCaptureObservation) == 4832, "capture observation wire layout changed");
#endif
