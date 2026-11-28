// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Configured-intent versus hardware readback under a HARD clock pin,
// assertion codes 14500-14509.
//
// A HARD pin owns the anchor and every point above it through the driver's
// locked clock range. The tail it programs is offset-based, so the driver's
// temperature-dependent base curve moves those points' live MHz by tens of MHz
// (observed on Blackwell: +35 MHz at the anchor and +103 MHz at the top point
// after cooling from 69 C to 35 C) while the pin keeps capping the clock.
// compare_intent_to_readback() used to hold each such point to the pin
// frequency and reported "HW OVERRIDDEN" for an untouched profile.

#include "gpu_core.h"
#include "service_protocol.h"
#include "intent_readback_status.h"

#include <string.h>

namespace {

constexpr int kAnchor = 76;
constexpr int kFirst = 68;   // two excluded low points, then offset points
constexpr int kLast = 80;
constexpr unsigned int kPinMHz = 2957;
constexpr int kOffsetMHz = 475;

// Stock base MHz per point, as read cold (35 C) on the reporting machine.
constexpr int kBaseMHz[kLast - kFirst + 1] = {
    2205, 2407, 2322, 2352, 2382, 2397, 2427, 2442,  // 68..75
    2480, 2502, 2517, 2548, 2570,                     // 76..80
};
// The offsets the daemon programmed for the pin's anchor and tail while the
// GPU was hot (69 C); unchanged since, as the exact offset readback shows.
constexpr int kTailOffsetMHz[kLast - kAnchor + 1] = {512, 490, 475, 452, 437};

ServiceResponse slot_like_response(LockMode mode) {
    ServiceResponse r = {};
    r.magic = SERVICE_PROTOCOL_MAGIC;
    r.version = SERVICE_PROTOCOL_VERSION;
    r.status = SERVICE_STATUS_OK;
    r.state.gpuPhase = SERVICE_GPU_PHASE_READY;
    r.state.activeDesiredValid = 1;
    r.snapshot.initialized = 1;
    r.snapshot.loaded = 1;
    r.snapshot.health.vfSnapshotFresh = 1;
    r.snapshot.gpuTemperatureValid = 1;
    r.snapshot.gpuTemperatureC = 35;
    for (int i = kFirst; i <= kLast; ++i) {
        const int base = kBaseMHz[i - kFirst];
        int offset = 0;
        if (i >= kAnchor) offset = kTailOffsetMHz[i - kAnchor];
        else if (i >= kFirst + 2) offset = kOffsetMHz;
        r.snapshot.curve[i].volt_uV = 850000u + (unsigned int)(i - kFirst) * 5000u;
        r.snapshot.curve[i].freq_kHz = (unsigned int)(base + offset) * 1000u;
        r.snapshot.freqOffsets[i] = offset * 1000;
    }
    r.snapshot.numPopulated = kLast - kFirst + 1;
    DesiredSettings& d = r.desired;
    d.hasGpuOffset = 1;
    d.gpuOffsetMHz = kOffsetMHz;
    d.gpuOffsetExcludeLowCount = 2;
    d.hasLock = 1;
    d.lockMode = mode;
    d.lockCi = kAnchor;
    d.lockMHz = kPinMHz;
    return r;
}

constexpr gc_u32 kVfLock =
    SERVICE_MUTATION_DOMAIN_VF_CURVE | SERVICE_MUTATION_DOMAIN_LOCK;

}  // namespace

int run_intent_readback_hard_pin_tests() {
    // Cooled GPU, untouched hard-pin profile: the tail sits 35..50 MHz above
    // the pin here (103 MHz at the real top point), yet nothing diverged. The
    // offset-owned prefix is still compared exactly.
    {
        ServiceResponse r = slot_like_response(LOCK_MODE_HARD);
        IntentReadbackStatus s = compare_intent_to_readback(&r);
        if (s.divergedDomains != 0 || s.unavailableDomains != 0 ||
            (s.checkedDomains & kVfLock) != kVfLock ||
            !intent_readback_matches(&s)) return 14500;
        if (s.maxVfDeltaMHz != 0 || s.vfDivergedPoints != 0 ||
            s.firstVfDivergedCi != -1 ||
            s.hardPinTailPointsSkipped != kLast - kAnchor + 1) return 14501;
        for (int i = kAnchor; i <= kLast; ++i) {
            int target = -1;
            if (intent_owned_point_target_mhz(&r, i, &target) || target != -1)
                return 14502;
        }
        int target = 0;
        if (!intent_owned_point_target_mhz(&r, kAnchor - 1, &target) ||
            target != kBaseMHz[kAnchor - 1 - kFirst] + kOffsetMHz) return 14503;
    }

    // The same live table under a FLATTEN lock keeps today's contract: the
    // flat tail is the frequency intent, so a drift past tolerance is shown.
    {
        ServiceResponse r = slot_like_response(LOCK_MODE_FLATTEN);
        IntentReadbackStatus s = compare_intent_to_readback(&r);
        if ((s.divergedDomains & kVfLock) != kVfLock ||
            s.hardPinTailPointsSkipped != 0 || s.firstVfDivergedCi != kAnchor ||
            s.maxVfDeltaMHz != kBaseMHz[kLast - kFirst] +
                kTailOffsetMHz[kLast - kAnchor] - (int)kPinMHz) return 14504;
    }

    // Another tool zeroing one offset-owned point below the anchor is still
    // reported, and the diagnostics name that point.
    {
        ServiceResponse r = slot_like_response(LOCK_MODE_HARD);
        const int ci = 72;
        r.snapshot.freqOffsets[ci] = 0;
        r.snapshot.curve[ci].freq_kHz = (unsigned int)kBaseMHz[ci - kFirst] * 1000u;
        IntentReadbackStatus s = compare_intent_to_readback(&r);
        if ((s.divergedDomains & kVfLock) != kVfLock ||
            s.vfDivergedPoints != 1 || s.firstVfDivergedCi != ci ||
            s.maxVfDeltaMHz != kOffsetMHz || intent_readback_matches(&s))
            return 14505;
    }

    // A tail point that also carries captured or explicit intent does not fall
    // through to those rules: the pin's tail write replaced them.
    {
        ServiceResponse r = slot_like_response(LOCK_MODE_HARD);
        r.desired.curvePointCaptured[78] = 1;
        r.desired.curvePointCapturedOffsetHz[78] = (long long)kOffsetMHz * 1000000LL;
        r.desired.hasCurvePoint[79] = 1;
        r.desired.curvePointMHz[79] = 2000;
        int target = -1;
        if (intent_owned_point_target_mhz(&r, 78, &target) ||
            intent_owned_point_target_mhz(&r, 79, &target)) return 14506;
        IntentReadbackStatus s = compare_intent_to_readback(&r);
        if (s.divergedDomains != 0 || !intent_readback_matches(&s)) return 14507;
    }

    // A pin with nothing else owned on the curve has no comparable VF point.
    // NVML has no locked-clock getter, so that is partial readback, never a
    // fabricated match and never an override.
    {
        ServiceResponse r = slot_like_response(LOCK_MODE_HARD);
        r.desired.hasGpuOffset = 0;
        r.desired.gpuOffsetExcludeLowCount = 0;
        IntentReadbackStatus s = compare_intent_to_readback(&r);
        if (s.divergedDomains != 0 || (s.unavailableDomains & kVfLock) != kVfLock ||
            intent_readback_matches(&s)) return 14508;
    }

    // No active intent: nothing is compared and the diagnostics stay neutral.
    {
        ServiceResponse r = slot_like_response(LOCK_MODE_HARD);
        r.state.activeDesiredValid = 0;
        IntentReadbackStatus s = compare_intent_to_readback(&r);
        if (s.requestedDomains || s.hardPinTailPointsSkipped ||
            s.vfDivergedPoints || s.firstVfDivergedCi != -1) return 14509;
    }
    return 0;
}
