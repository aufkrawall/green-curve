// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_GPU_LOCK_OWNERSHIP_POLICY_H
#define GREEN_CURVE_GPU_LOCK_OWNERSHIP_POLICY_H

#include "gpu_wire_types.h"

// Pure ownership policy gating implicit live hardware adoption (§5.3 / §8.5).
// External VF flat tails and external pre-anchor offsets must never become
// Green Curve applied lock state or request intent outside explicit Capture.
struct GpuLiveLockOwnershipContext {
    bool isServiceProcess = false;
    bool serviceHasActiveDesired = false;
    bool serviceActiveHasLock = false;
    int serviceLockCi = -1;
    unsigned int serviceLockMHz = 0;
    bool usingBackgroundService = false;
    int appliedLockCi = -1;
    unsigned int appliedLockFreq = 0;
    int appliedLockMode = 0; // LOCK_MODE_NONE == 0
};

static inline bool gpu_live_lock_ownership_proven(const GpuLiveLockOwnershipContext& ctx) {
    if (ctx.isServiceProcess) {
        return ctx.serviceHasActiveDesired && ctx.serviceActiveHasLock &&
            ctx.serviceLockCi >= 0 && ctx.serviceLockCi < VF_NUM_POINTS &&
            ctx.serviceLockMHz > 0;
    }
    if (ctx.usingBackgroundService) {
        // GUI client receives authoritative lock ownership solely from service snapshots.
        return false;
    }
    return ctx.appliedLockMode != 0 && ctx.appliedLockCi >= 0 &&
        ctx.appliedLockCi < VF_NUM_POINTS && ctx.appliedLockFreq > 0;
}

static inline int gpu_live_lock_preferred_ci(const GpuLiveLockOwnershipContext& ctx) {
    if (!gpu_live_lock_ownership_proven(ctx)) return -1;
    return ctx.isServiceProcess ? ctx.serviceLockCi : ctx.appliedLockCi;
}

struct GuiPreTailPointDecision {
    bool retain = false;
    bool fromGpuOffset = false;
    bool captured = false;
    unsigned int overrideMHz = 0;
    long long capturedOffsetHz = 0;
};

// Decide whether a pre-anchor point with no explicit draft edit retains
// Green Curve's own applied pre-anchor shape when re-locking (§5.3 / PLAN-11).
// Live hardware programmed offsets (freqOffsets) are intentionally excluded:
// external offsets must become intent only through explicit Capture.
static inline GuiPreTailPointDecision gui_pre_tail_point_decision(
    bool inPreTailWindow,
    bool explicitInDraft,
    unsigned int appliedMHz,
    bool appliedFromGpuOffset,
    bool serviceActiveHasPoint,
    unsigned int servicePointMHz,
    bool servicePointFromGpuOffset,
    bool servicePointCaptured,
    long long serviceCapturedOffsetHz) {
    GuiPreTailPointDecision out;
    if (!inPreTailWindow || explicitInDraft) return out;
    if (servicePointCaptured) {
        out.retain = true;
        out.captured = true;
        out.capturedOffsetHz = serviceCapturedOffsetHz;
        return out;
    }
    if (appliedMHz > 0) {
        out.retain = true;
        out.fromGpuOffset = appliedFromGpuOffset;
        out.overrideMHz = appliedMHz;
        return out;
    }
    if (serviceActiveHasPoint) {
        out.retain = true;
        out.fromGpuOffset = servicePointFromGpuOffset;
        out.overrideMHz = servicePointMHz;
        return out;
    }
    return out;
}

#endif
