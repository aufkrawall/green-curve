// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_DESIRED_SETTINGS_SCHEMA_H
#define GREEN_CURVE_DESIRED_SETTINGS_SCHEMA_H

// FROZEN on-disk layouts of DesiredSettings, and the rule that keeps them
// honest.
//
// Three files embed DesiredSettings as raw bytes: the Linux daemon state
// record, the Linux daemon startup-policy record, and the Windows controlled-
// restart snapshot.  Each carries a version number -- and every one of those
// version numbers stayed put while the struct grew underneath it.  Version 5
// of the Windows snapshot has meant several different layouts since 0.19.2;
// version 1 of the Linux state record meant at least three.  The loaders
// papered over that by demanding `st_size == sizeof(CurrentStruct)`, so every
// older record was classified corrupt and deleted, and -- worse -- every
// backward-compatibility branch written to migrate an older generation was
// declared over a struct that had since changed, which made it unreachable
// code that could never once have fired.  Adding curvePointFromGpuOffset[]
// for 0.26.0 is what made that visible: 836 -> 964 bytes, discarding the
// restore-last intent and the startup policy of every 0.25.2 Linux install.
//
// The model that actually works, and that every loader now implements:
//
//   SIZE selects the LAYOUT.  VERSION selects the SEMANTICS within it.
//
// A record's byte count is the one thing a writer cannot get wrong, so it is
// what a reader dispatches on.  The version field then says what the fields
// MEAN -- the 0.25.2 memory-offset unit reinterpretation is exactly such a
// change: same layout, different meaning -- and it is never again asked to
// imply a layout.
//
// Adding, removing, reordering or resizing a field in DesiredSettings (or in
// anything it embeds, or in GpuAdapterInfo, which sits in the same records)
// therefore requires, in one commit:
//
//   1. Freeze the outgoing layout here as DesiredSettingsSchema<N>, with its
//      exact byte size asserted, plus a widening copy into the live struct
//      that gives every NEW field the value preserving the OLD behaviour.
//   2. Freeze the outgoing record layouts that embed it (see
//      linux_daemon_state.h and main_service_persist.cpp).
//   3. Bump LINUX_DAEMON_RECORD_VERSION, LINUX_DAEMON_STARTUP_VERSION,
//      SERVICE_ACTIVE_DESIRED_VERSION and SERVICE_PROTOCOL_VERSION.
//
// The static_asserts here and beside each record are what make that a build
// break rather than a silent upgrade regression: they name this file and the
// exact list above, so the next person to add a field cannot miss the step.
//
// Nothing here ever WRITES an old layout.  These structs exist to read files
// this program already put on disk, and to be deleted once the generation
// they describe can no longer be in the field.

// Generation of the live DesiredSettings layout.  Schema 1 shipped through
// 0.25.2; schema 2 added curvePointFromGpuOffset[] for 0.26.0.
enum { DESIRED_SETTINGS_SCHEMA_CURRENT = 2 };

// Schema 1: DesiredSettings exactly as it was written to disk up to and
// including 0.25.2.  FROZEN -- never edit a field here, and never "keep it in
// sync" with the live struct.  Its whole purpose is to differ from it.
struct DesiredSettingsSchema1 {
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
};

// Contractual, not incidental: a 0.25.2 record on disk is this many bytes and
// no build may ever change that.  FanCurveConfig is embedded by value, so this
// also pins the fan-curve layout that shipped with it.
static_assert(sizeof(DesiredSettingsSchema1) == 836,
              "DesiredSettingsSchema1 is a FROZEN on-disk layout and must stay "
              "836 bytes; freeze a new DesiredSettingsSchema2 instead of "
              "editing this one (see desired_settings_schema.h)");

// Widen a schema-1 record into the live struct.  Field by field on purpose: a
// memcpy of the common prefix would keep compiling -- and start corrupting --
// the moment someone reorders the live struct instead of appending to it.
//
// curvePointFromGpuOffset[] is left zeroed, which is the value that preserves
// 0.25.2 behaviour exactly: that build had no provenance, so every point it
// stored was honoured as an absolute target, and an unflagged point still is.
static inline void desired_settings_widen_from_schema1(
    DesiredSettings* out, const DesiredSettingsSchema1* in) {
    if (!out || !in) return;
    *out = {};
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        out->hasCurvePoint[ci] = in->hasCurvePoint[ci];
        out->curvePointMHz[ci] = in->curvePointMHz[ci];
        out->curvePointFromGpuOffset[ci] = 0;
    }
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
}

// Narrow the live struct back down to schema 1.  Used ONLY by the regression
// harness, to build byte-exact fixtures of what a 0.25.2 build really wrote: a
// test that constructs the CURRENT struct and relabels its version number
// cannot catch a layout change, which is precisely how this class of bug
// reached a release candidate.  Nothing in the product writes schema 1.
static inline void desired_settings_narrow_to_schema1(
    DesiredSettingsSchema1* out, const DesiredSettings* in) {
    if (!out || !in) return;
    *out = {};
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        out->hasCurvePoint[ci] = in->hasCurvePoint[ci];
        out->curvePointMHz[ci] = in->curvePointMHz[ci];
    }
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
}

#endif
