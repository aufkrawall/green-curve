// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included by service_protocol.h after mutation-domain declarations.

#ifndef GREEN_CURVE_SERVICE_DESIRED_MUTATION_POLICY_H
#define GREEN_CURVE_SERVICE_DESIRED_MUTATION_POLICY_H

static inline int service_desired_curve_point_count(
    const DesiredSettings* desired) {
    if (!desired) return 0;
    int count = 0;
    for (int i = 0; i < VF_NUM_POINTS; ++i)
        if (desired->hasCurvePoint[i]) ++count;
    return count;
}

// Build an editor-facing projection without inventing mutations in unavailable
// domains. This is not a server filter: the server still rejects every mixed
// request atomically. It lets a degraded client start a genuinely NVML-only
// draft while retaining the full active intent inside the daemon.
static inline void service_project_desired_to_available_domains(
    DesiredSettings* desired, gc_u32 availableDomains) {
    if (!desired) return;
    bool hasCurvePoint = service_desired_has_curve_point(desired);
    bool gpuOffsetUsesCurve = desired->hasGpuOffset &&
        (desired->gpuOffsetExcludeLowCount > 0 || desired->hasLock ||
         hasCurvePoint);
    if ((availableDomains & SERVICE_MUTATION_DOMAIN_RESET_BASELINE) == 0)
        desired->resetOcBeforeApply = false;
    if (gpuOffsetUsesCurve &&
        (availableDomains & SERVICE_MUTATION_DOMAIN_VF_CURVE) == 0) {
        desired->hasGpuOffset = false;
        desired->gpuOffsetExcludeLowCount = 0;
    } else if (!gpuOffsetUsesCurve &&
        (availableDomains & SERVICE_MUTATION_DOMAIN_GPU_OFFSET) == 0) {
        desired->hasGpuOffset = false;
    }
    if ((availableDomains & SERVICE_MUTATION_DOMAIN_MEM_OFFSET) == 0)
        desired->hasMemOffset = false;
    if ((availableDomains & SERVICE_MUTATION_DOMAIN_POWER) == 0)
        desired->hasPowerLimit = false;
    if ((availableDomains & SERVICE_MUTATION_DOMAIN_VF_CURVE) == 0) {
        for (int i = 0; i < VF_NUM_POINTS; ++i) {
            desired->hasCurvePoint[i] = false;
            desired->curvePointMHz[i] = 0;
        }
        // Every current lock mode composes a VF anchor/tail write.
        desired->hasLock = false;
        desired->lockCi = -1;
        desired->lockMHz = 0;
        desired->lockMode = LOCK_MODE_NONE;
        desired->lockTracksAnchor = false;
    } else if ((availableDomains & SERVICE_MUTATION_DOMAIN_LOCK) == 0) {
        desired->hasLock = false;
        desired->lockCi = -1;
        desired->lockMHz = 0;
        desired->lockMode = LOCK_MODE_NONE;
        desired->lockTracksAnchor = false;
    }
    if ((availableDomains & SERVICE_MUTATION_DOMAIN_FAN) == 0)
        desired->hasFan = false;
}

// Successful sparse Apply operations update only the requested domains of the
// daemon's durable intent. Otherwise a safe degraded power-only Apply would
// accidentally discard an existing fan/VF intent even though it never touched
// that hardware domain.
static inline DesiredSettings service_merge_desired_after_mutation(
    const DesiredSettings* previous, const DesiredSettings* requested) {
    DesiredSettings merged = {};
    if (previous) merged = *previous;
    if (!requested) return merged;
    gc_u32 domains = service_desired_mutation_domains(requested);
    if (domains & SERVICE_MUTATION_DOMAIN_RESET_BASELINE) {
        merged.hasGpuOffset = false;
        merged.gpuOffsetMHz = 0;
        merged.gpuOffsetExcludeLowCount = 0;
        merged.hasMemOffset = false;
        merged.memOffsetMHz = 0;
        for (int i = 0; i < VF_NUM_POINTS; ++i) {
            merged.hasCurvePoint[i] = false;
            merged.curvePointMHz[i] = 0;
        }
        merged.hasLock = false;
        merged.lockCi = -1;
        merged.lockMHz = 0;
        merged.lockMode = LOCK_MODE_NONE;
        merged.lockTracksAnchor = false;
    }
    if (requested->hasGpuOffset) {
        merged.hasGpuOffset = true;
        merged.gpuOffsetMHz = requested->gpuOffsetMHz;
        merged.gpuOffsetExcludeLowCount =
            requested->gpuOffsetExcludeLowCount;
    }
    if (domains & SERVICE_MUTATION_DOMAIN_MEM_OFFSET) {
        merged.hasMemOffset = requested->hasMemOffset;
        merged.memOffsetMHz = requested->memOffsetMHz;
    }
    if (domains & SERVICE_MUTATION_DOMAIN_POWER) {
        merged.hasPowerLimit = requested->hasPowerLimit;
        merged.powerLimitPct = requested->powerLimitPct;
    }
    if (domains & SERVICE_MUTATION_DOMAIN_VF_CURVE) {
        for (int i = 0; i < VF_NUM_POINTS; ++i) {
            merged.hasCurvePoint[i] = requested->hasCurvePoint[i];
            merged.curvePointMHz[i] = requested->curvePointMHz[i];
        }
    }
    if (domains & SERVICE_MUTATION_DOMAIN_LOCK) {
        merged.hasLock = requested->hasLock;
        merged.lockCi = requested->lockCi;
        merged.lockMHz = requested->lockMHz;
        merged.lockMode = requested->lockMode;
        merged.lockTracksAnchor = requested->lockTracksAnchor;
    }
    if (domains & SERVICE_MUTATION_DOMAIN_FAN) {
        merged.hasFan = requested->hasFan;
        merged.fanAuto = requested->fanAuto;
        merged.fanMode = requested->fanMode;
        merged.fanPercent = requested->fanPercent;
        merged.fanCurve = requested->fanCurve;
    }
    // This is a one-shot transaction instruction, never durable intent.
    merged.resetOcBeforeApply = false;
    return merged;
}

#endif // GREEN_CURVE_SERVICE_DESIRED_MUTATION_POLICY_H
