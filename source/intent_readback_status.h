// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Pure intended-vs-actual GPU state comparison shared by the Linux TUI,
// machine-readable live output, and the regression harness.  Active desired
// settings describe Green Curve's last successful write; they are not proof
// that another controller has not subsequently replaced that hardware state.

#ifndef GREEN_CURVE_INTENT_READBACK_STATUS_H
#define GREEN_CURVE_INTENT_READBACK_STATUS_H

#include "gpu_core.h"

#define INTENT_VF_READBACK_TOLERANCE_MHZ 30

struct IntentReadbackStatus {
    gc_u32 requestedDomains;
    gc_u32 checkedDomains;
    gc_u32 divergedDomains;
    gc_u32 unavailableDomains;
    int maxVfDeltaMHz;
};

static inline int intent_abs_int(int value) {
    return value < 0 ? -value : value;
}

static inline int intent_point_base_mhz(
    const ServiceSnapshot* snapshot, int index) {
    if (!snapshot || index < 0 || index >= VF_NUM_POINTS) return 0;
    long long base = (long long)snapshot->curve[index].freq_kHz -
                     (long long)snapshot->freqOffsets[index];
    return (int)((base >= 0 ? base + 500 : base - 500) / 1000);
}

static inline int intent_point_live_mhz(
    const ServiceSnapshot* snapshot, int index) {
    if (!snapshot || index < 0 || index >= VF_NUM_POINTS) return 0;
    return (int)((snapshot->curve[index].freq_kHz + 500u) / 1000u);
}

// Returns true only for a point the active intent owns.  Unowned curve points
// are telemetry and must not be compared with themselves or treated as drift.
static inline bool intent_owned_point_target_mhz(
    const ServiceResponse* response, int index, int* targetMHz) {
    if (!response || !response->state.activeDesiredValid ||
        index < 0 || index >= VF_NUM_POINTS ||
        response->snapshot.curve[index].freq_kHz == 0) return false;
    const DesiredSettings* desired = &response->desired;
    if (desired->hasLock && desired->lockCi >= 0 &&
        index >= desired->lockCi && desired->lockMHz > 0) {
        if (targetMHz) *targetMHz = (int)desired->lockMHz;
        return true;
    }
    if (desired->hasCurvePoint[index] &&
        desired->curvePointMHz[index] > 0) {
        if (targetMHz) *targetMHz = (int)desired->curvePointMHz[index];
        return true;
    }
    if (!desired->hasGpuOffset ||
        (service_desired_mutation_domains(desired) &
            SERVICE_MUTATION_DOMAIN_VF_CURVE) == 0) return false;
    int ordinal = 0;
    for (int i = 0; i < index; ++i)
        if (response->snapshot.curve[i].freq_kHz != 0) ++ordinal;
    if (ordinal < desired->gpuOffsetExcludeLowCount) return false;
    if (targetMHz) {
        *targetMHz = intent_point_base_mhz(&response->snapshot, index) +
                     desired->gpuOffsetMHz;
    }
    return true;
}

static inline IntentReadbackStatus compare_intent_to_readback(
    const ServiceResponse* response) {
    IntentReadbackStatus status = {};
    if (!response || !response->state.activeDesiredValid) return status;
    const DesiredSettings* desired = &response->desired;
    const ServiceSnapshot* snapshot = &response->snapshot;
    const ControlState* actual = &response->controlState;
    status.requestedDomains =
        service_desired_mutation_domains(desired) &
        ~((gc_u32)SERVICE_MUTATION_DOMAIN_RESET_BASELINE);

    if (status.requestedDomains & SERVICE_MUTATION_DOMAIN_GPU_OFFSET) {
        if (actual->valid && actual->hasGpuOffset &&
            actual->gpuOffsetReadbackValid) {
            status.checkedDomains |= SERVICE_MUTATION_DOMAIN_GPU_OFFSET;
            if (!nvml_clock_offset_readback_matches(
                    desired->gpuOffsetMHz, actual->gpuOffsetMHz,
                    nvml_clock_offset_grid_step(NVML_CLOCK_GRAPHICS))) {
                status.divergedDomains |= SERVICE_MUTATION_DOMAIN_GPU_OFFSET;
            }
        } else {
            status.unavailableDomains |= SERVICE_MUTATION_DOMAIN_GPU_OFFSET;
        }
    }

    if (status.requestedDomains & SERVICE_MUTATION_DOMAIN_MEM_OFFSET) {
        if (actual->valid && actual->hasMemOffset &&
            actual->memOffsetReadbackValid) {
            status.checkedDomains |= SERVICE_MUTATION_DOMAIN_MEM_OFFSET;
            if (!nvml_clock_offset_readback_matches(
                    desired->memOffsetMHz, actual->memOffsetMHz,
                    nvml_clock_offset_grid_step(NVML_CLOCK_MEM))) {
                status.divergedDomains |= SERVICE_MUTATION_DOMAIN_MEM_OFFSET;
            }
        } else {
            status.unavailableDomains |= SERVICE_MUTATION_DOMAIN_MEM_OFFSET;
        }
    }

    if (status.requestedDomains & SERVICE_MUTATION_DOMAIN_POWER) {
        if (actual->valid && actual->hasPowerLimit &&
            actual->powerLimitReadbackValid &&
            snapshot->powerLimitDefaultmW > 0 &&
            snapshot->powerLimitCurrentmW > 0) {
            status.checkedDomains |= SERVICE_MUTATION_DOMAIN_POWER;
            if (desired->powerLimitPct != actual->powerLimitPct)
                status.divergedDomains |= SERVICE_MUTATION_DOMAIN_POWER;
        } else {
            status.unavailableDomains |= SERVICE_MUTATION_DOMAIN_POWER;
        }
    }

    gc_u32 vfRequested = status.requestedDomains &
        (SERVICE_MUTATION_DOMAIN_VF_CURVE | SERVICE_MUTATION_DOMAIN_LOCK);
    if (vfRequested) {
        if (snapshot->loaded && snapshot->health.vfSnapshotFresh) {
            bool ownedPointFound = false;
            bool diverged = false;
            for (int i = 0; i < VF_NUM_POINTS; ++i) {
                int target = 0;
                if (!intent_owned_point_target_mhz(response, i, &target))
                    continue;
                ownedPointFound = true;
                int delta = intent_abs_int(
                    intent_point_live_mhz(snapshot, i) - target);
                if (delta > status.maxVfDeltaMHz)
                    status.maxVfDeltaMHz = delta;
                if (delta > INTENT_VF_READBACK_TOLERANCE_MHZ)
                    diverged = true;
            }
            if (ownedPointFound) {
                status.checkedDomains |= vfRequested;
                if (diverged) status.divergedDomains |= vfRequested;
            } else {
                status.unavailableDomains |= vfRequested;
            }
        } else {
            status.unavailableDomains |= vfRequested;
        }
    }

    if (status.requestedDomains & SERVICE_MUTATION_DOMAIN_FAN) {
        if (snapshot->fanSupported && snapshot->fanCount > 0 &&
            actual->fanPolicyReadbackValid) {
            bool diverged = false;
            bool targetUnreadable = false;
            for (unsigned int i = 0;
                 i < snapshot->fanCount && i < MAX_GPU_FANS; ++i) {
                bool expectAuto = desired->fanMode == FAN_MODE_AUTO ||
                                  desired->fanAuto;
                bool isAuto = snapshot->fanPolicy[i] ==
                    NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW;
                if (expectAuto != isAuto) {
                    // The policy itself was taken over.  Some drivers stop
                    // answering the target getter once a fan is back on the
                    // automatic curve, so demanding a duty readback here would
                    // downgrade a detected override into "unknown".
                    diverged = true;
                    continue;
                }
                if (desired->fanMode != FAN_MODE_FIXED || expectAuto) continue;
                if (!actual->fanTargetReadbackValid) {
                    targetUnreadable = true;
                    continue;
                }
                int expected = desired->fanPercent;
                if (snapshot->fanRangeKnown) {
                    if (expected < (int)snapshot->fanMinPct)
                        expected = (int)snapshot->fanMinPct;
                    if (expected > (int)snapshot->fanMaxPct)
                        expected = (int)snapshot->fanMaxPct;
                }
                if ((int)snapshot->fanTargetPercent[i] != expected)
                    diverged = true;
            }
            if (diverged) {
                status.checkedDomains |= SERVICE_MUTATION_DOMAIN_FAN;
                status.divergedDomains |= SERVICE_MUTATION_DOMAIN_FAN;
            } else if (targetUnreadable) {
                status.unavailableDomains |= SERVICE_MUTATION_DOMAIN_FAN;
            } else {
                status.checkedDomains |= SERVICE_MUTATION_DOMAIN_FAN;
            }
        } else {
            status.unavailableDomains |= SERVICE_MUTATION_DOMAIN_FAN;
        }
    }
    if (status.requestedDomains & SERVICE_MUTATION_DOMAIN_XBAR) {
        bool freqReadable = !desired->hasXbarOffsetKhz ||
            (actual->valid && actual->hasXbarOffset &&
             actual->xbarOffsetReadbackValid);
        bool voltageReadable = !desired->hasXbarMsvddOffsetUv ||
            (actual->valid && actual->hasXbarMsvddOffset &&
             actual->xbarMsvddOffsetReadbackValid);
        if (freqReadable && voltageReadable) {
            status.checkedDomains |= SERVICE_MUTATION_DOMAIN_XBAR;
            if ((desired->hasXbarOffsetKhz &&
                 desired->xbarOffsetKhz != actual->xbarOffsetKhz) ||
                (desired->hasXbarMsvddOffsetUv &&
                 desired->xbarMsvddOffsetUv !=
                    actual->xbarMsvddOffsetUv)) {
                status.divergedDomains |= SERVICE_MUTATION_DOMAIN_XBAR;
            }
        } else {
            status.unavailableDomains |= SERVICE_MUTATION_DOMAIN_XBAR;
        }
    }
    if (status.requestedDomains & SERVICE_MUTATION_DOMAIN_SYS_CLK) {
        if (actual->valid && actual->hasSysClkOffset &&
            actual->sysClkOffsetReadbackValid) {
            status.checkedDomains |= SERVICE_MUTATION_DOMAIN_SYS_CLK;
            if (desired->sysClkOffsetKhz != actual->sysClkOffsetKhz)
                status.divergedDomains |= SERVICE_MUTATION_DOMAIN_SYS_CLK;
        } else {
            status.unavailableDomains |= SERVICE_MUTATION_DOMAIN_SYS_CLK;
        }
    }
    if (status.requestedDomains & SERVICE_MUTATION_DOMAIN_VIDEO_CLK) {
        if (actual->valid && actual->hasVideoClkOffset &&
            actual->videoClkOffsetReadbackValid) {
            status.checkedDomains |= SERVICE_MUTATION_DOMAIN_VIDEO_CLK;
            if (desired->videoClkOffsetKhz != actual->videoClkOffsetKhz)
                status.divergedDomains |= SERVICE_MUTATION_DOMAIN_VIDEO_CLK;
        } else {
            status.unavailableDomains |= SERVICE_MUTATION_DOMAIN_VIDEO_CLK;
        }
    }
    return status;
}

static inline bool intent_readback_matches(
    const IntentReadbackStatus* status) {
    return status && status->requestedDomains != 0 &&
           status->divergedDomains == 0 &&
           status->unavailableDomains == 0;
}

#endif  // GREEN_CURVE_INTENT_READBACK_STATUS_H
