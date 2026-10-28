// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_GPU_SELECTION_WRITE_EVIDENCE_H
#define GREEN_CURVE_GPU_SELECTION_WRITE_EVIDENCE_H

#include "app_shared.h"
#include "control_readback_policy.h"

// Readback and write uncertainty describe the GPU that answered or received
// the write. A newly selected GPU starts from its own reads: a carried-over
// uncertain flag would force writes and report unverified recovery on a GPU
// nothing touched, and a carried-over valid readback could let a setter skip
// a write it believes is already in place.
static inline void reset_selected_gpu_write_evidence(AppData* app) {
    if (!app) return;
    invalidate_scalar_readbacks(&app->readback);
    app->memOffsetWriteUncertain = false;
    app->powerLimitPct = POWER_LIMIT_DEFAULT_PCT;
    app->powerLimitDefaultmW = 0;
    app->powerLimitCurrentmW = 0;
    app->powerLimitMinmW = 0;
    app->powerLimitMaxmW = 0;
    app->powerLimitSurfaceProven = false;
    app->powerLimitWriteUncertain = false;
    app->powerLimitRecoveryDefaultmW = 0;
    app->powerLimitRecoveryMinmW = 0;
    app->powerLimitRecoveryMaxmW = 0;
}

#endif
