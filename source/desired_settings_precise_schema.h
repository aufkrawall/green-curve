// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_DESIRED_SETTINGS_PRECISE_SCHEMA_H
#define GREEN_CURVE_DESIRED_SETTINGS_PRECISE_SCHEMA_H
// FROZEN outgoing precise-memory and rational-power payload.
struct DesiredSettingsSchema4 {
    gc_bool8 hasCurvePoint[VF_NUM_POINTS];
    unsigned int curvePointMHz[VF_NUM_POINTS];
    gc_bool8 hasLock;
    int lockCi;
    unsigned int lockMHz;
    LockMode lockMode;
    gc_bool8 lockTracksAnchor;
    gc_bool8 hasGpuOffset;
    int gpuOffsetMHz;
    int gpuOffsetExcludeLowCount;
    gc_bool8 hasMemOffset;
    int memOffsetMHz;
    gc_bool8 hasPowerLimit;
    int powerLimitPct;
    gc_bool8 hasFan;
    gc_bool8 fanAuto;
    int fanMode;
    int fanPercent;
    FanCurveConfig fanCurve;
    gc_bool8 resetOcBeforeApply;
    gc_bool8 hasXbarOffsetKhz;
    int xbarOffsetKhz;
    gc_bool8 hasXbarMsvddOffsetUv;
    int xbarMsvddOffsetUv;
    gc_bool8 hasSysClkOffsetKhz;
    int sysClkOffsetKhz;
    gc_bool8 hasVideoClkOffsetKhz;
    int videoClkOffsetKhz;
    // Provenance of curvePointMHz[], not a setting.  Per POINT, because one
    // request mixes both kinds: load a profile saved as base + GPU offset and
    // hand-edit one field, and that point is a real absolute target while its
    // neighbours are projections.  A projected point's MHz is only as current as
    // the stock base it was projected over, and that base moves with load (point
    // 70: 2322 MHz saved idle, 2352 MHz at 99% util -- a whole VF bin), so it is
    // written and verified against its OFFSET.  An unflagged point keeps
    // absolute authority and still fails the apply on its own readback.  The GUI
    // sets this too, and must: its VF fields show the same projection for any
    // point it did not get from the user.
    gc_bool8 curvePointFromGpuOffset[VF_NUM_POINTS];
    // Independent programmed-offset intent; presence includes an owned zero.
    gc_bool8 curvePointCaptured[VF_NUM_POINTS];
    gc_u32 applicationScope; // DesiredApplicationScope; independent of reset
    gc_i64 curvePointCapturedOffsetHz[VF_NUM_POINTS];
    gc_bool8 memOffsetPrecise;
    gc_bool8 powerLimitRatio;
    gc_u32 memOffsetSurface; // zero selects a fresh destination surface
    gc_i64 memOffsetHz;
    gc_i64 powerRatioNumerator;
    gc_i64 powerRatioDenominator;
};
static_assert(sizeof(DesiredSettingsSchema4) == 2152, "precise scalar layout is frozen");

static inline void desired_settings_widen_schema4(DesiredSettings* out,
    const DesiredSettingsSchema4* in) {
    if (!out || !in) return;
    memset(out, 0, sizeof(*out));
    memcpy(out->hasCurvePoint, in->hasCurvePoint, sizeof(out->hasCurvePoint));
    memcpy(out->curvePointMHz, in->curvePointMHz, sizeof(out->curvePointMHz));
    out->hasLock = in->hasLock;
    out->lockCi = in->lockCi;
    out->lockMHz = in->lockMHz;
    out->lockMode = in->lockMode;
    out->lockTracksAnchor = in->lockTracksAnchor;
    out->hasGpuOffset = in->hasGpuOffset;
    out->gpuOffsetMHz = in->gpuOffsetMHz;
    out->gpuOffsetExcludeLowCount = in->gpuOffsetExcludeLowCount;
    out->hasMemOffset = in->hasMemOffset;
    out->memOffsetMHz = in->memOffsetMHz;
    out->hasPowerLimit = in->hasPowerLimit;
    out->powerLimitPct = in->powerLimitPct;
    out->hasFan = in->hasFan;
    out->fanAuto = in->fanAuto;
    out->fanMode = in->fanMode;
    out->fanPercent = in->fanPercent;
    out->fanCurve = in->fanCurve;
    out->resetOcBeforeApply = in->resetOcBeforeApply;
    out->hasXbarOffsetKhz = in->hasXbarOffsetKhz;
    out->xbarOffsetKhz = in->xbarOffsetKhz;
    out->hasXbarMsvddOffsetUv = in->hasXbarMsvddOffsetUv;
    out->xbarMsvddOffsetUv = in->xbarMsvddOffsetUv;
    out->hasSysClkOffsetKhz = in->hasSysClkOffsetKhz;
    out->sysClkOffsetKhz = in->sysClkOffsetKhz;
    out->hasVideoClkOffsetKhz = in->hasVideoClkOffsetKhz;
    out->videoClkOffsetKhz = in->videoClkOffsetKhz;
    memcpy(out->curvePointFromGpuOffset, in->curvePointFromGpuOffset, sizeof(out->curvePointFromGpuOffset));
    memcpy(out->curvePointCaptured, in->curvePointCaptured, sizeof(out->curvePointCaptured));
    out->applicationScope = in->applicationScope;
    memcpy(out->curvePointCapturedOffsetHz, in->curvePointCapturedOffsetHz, sizeof(out->curvePointCapturedOffsetHz));
    out->memOffsetPrecise = in->memOffsetPrecise;
    out->powerLimitRatio = in->powerLimitRatio;
    out->memOffsetSurface = in->memOffsetSurface;
    out->memOffsetHz = in->memOffsetHz;
    out->powerRatioNumerator = in->powerRatioNumerator;
    out->powerRatioDenominator = in->powerRatioDenominator;
}

static inline void desired_settings_narrow_schema4(DesiredSettingsSchema4* out,
    const DesiredSettings* in) {
    if (!out || !in) return;
    memset(out, 0, sizeof(*out));
    memcpy(out->hasCurvePoint, in->hasCurvePoint, sizeof(out->hasCurvePoint));
    memcpy(out->curvePointMHz, in->curvePointMHz, sizeof(out->curvePointMHz));
    out->hasLock = in->hasLock;
    out->lockCi = in->lockCi;
    out->lockMHz = in->lockMHz;
    out->lockMode = in->lockMode;
    out->lockTracksAnchor = in->lockTracksAnchor;
    out->hasGpuOffset = in->hasGpuOffset;
    out->gpuOffsetMHz = in->gpuOffsetMHz;
    out->gpuOffsetExcludeLowCount = in->gpuOffsetExcludeLowCount;
    out->hasMemOffset = in->hasMemOffset;
    out->memOffsetMHz = in->memOffsetMHz;
    out->hasPowerLimit = in->hasPowerLimit;
    out->powerLimitPct = in->powerLimitPct;
    out->hasFan = in->hasFan;
    out->fanAuto = in->fanAuto;
    out->fanMode = in->fanMode;
    out->fanPercent = in->fanPercent;
    out->fanCurve = in->fanCurve;
    out->resetOcBeforeApply = in->resetOcBeforeApply;
    out->hasXbarOffsetKhz = in->hasXbarOffsetKhz;
    out->xbarOffsetKhz = in->xbarOffsetKhz;
    out->hasXbarMsvddOffsetUv = in->hasXbarMsvddOffsetUv;
    out->xbarMsvddOffsetUv = in->xbarMsvddOffsetUv;
    out->hasSysClkOffsetKhz = in->hasSysClkOffsetKhz;
    out->sysClkOffsetKhz = in->sysClkOffsetKhz;
    out->hasVideoClkOffsetKhz = in->hasVideoClkOffsetKhz;
    out->videoClkOffsetKhz = in->videoClkOffsetKhz;
    memcpy(out->curvePointFromGpuOffset, in->curvePointFromGpuOffset, sizeof(out->curvePointFromGpuOffset));
    memcpy(out->curvePointCaptured, in->curvePointCaptured, sizeof(out->curvePointCaptured));
    out->applicationScope = in->applicationScope;
    memcpy(out->curvePointCapturedOffsetHz, in->curvePointCapturedOffsetHz, sizeof(out->curvePointCapturedOffsetHz));
    out->memOffsetPrecise = in->memOffsetPrecise;
    out->powerLimitRatio = in->powerLimitRatio;
    out->memOffsetSurface = in->memOffsetSurface;
    out->memOffsetHz = in->memOffsetHz;
    out->powerRatioNumerator = in->powerRatioNumerator;
    out->powerRatioDenominator = in->powerRatioDenominator;
}
#include "desired_settings_authoring_schema.h"
#endif
