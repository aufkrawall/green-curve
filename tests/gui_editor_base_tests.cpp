// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The editor's base record and the overlay of the user's edits onto it
// (source/gui_editor_base_policy.h), assertion codes 12320-12333. The base is
// an imported flatten like the reported slot: explicit scope, a 2962 MHz lock
// at point 76 (925 mV) with exact Hz, captured offsets below it (some
// nonzero, as another tool's raised lower points would be), exact memory, a
// power ratio and a topology binding. Before the fix the editor rebuilt the
// request from its fields: captured points went back to stock and the request
// fell back to legacy scope.

#include <memory>
#include <string.h>
#include <string>

#include "gui_editor_base_policy.h"

namespace {

constexpr int kAnchor = 76;
constexpr int kLast = 126;

std::unique_ptr<DesiredSettings> imported_flatten() {
    auto d = std::make_unique<DesiredSettings>();
    d->lockCi = -1;
    d->applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED | DESIRED_SCOPE_REPLACE_VF_POINTS;
    d->authoring.topologyBound = 1;
    d->authoring.topologyCount = kLast + 1;
    for (int ci = 0; ci <= kLast; ++ci) d->authoring.voltageUv[ci] = 450000 + ci * 5000;
    for (int ci = 45; ci < kAnchor; ++ci) {
        d->curvePointCaptured[ci] = 1;
        d->curvePointCapturedOffsetHz[ci] = ci >= 70 ? 115000000LL : 0;
    }
    d->hasLock = 1;
    d->lockCi = kAnchor;
    d->lockMHz = 2962;
    d->lockMode = LOCK_MODE_FLATTEN;
    d->authoring.lockPrecise = 1;
    d->authoring.lockFrequencyHz = 2962000000LL;
    d->hasMemOffset = 1;
    d->memOffsetMHz = 1500;
    d->memOffsetPrecise = 1;
    d->memOffsetHz = 1500500000LL;
    d->memOffsetSurface = CLOCK_OFFSET_SURFACE_P0;
    d->hasPowerLimit = 1;
    d->powerLimitPct = 85;
    d->powerLimitRatio = 1;
    d->powerRatioNumerator = 17;
    d->powerRatioDenominator = 20;
    d->hasXbarOffsetKhz = d->hasXbarMsvddOffsetUv = d->hasSysClkOffsetKhz = d->hasVideoClkOffsetKhz = 1;
    return d;  // no fan, no GPU offset: not captured
}

// What capture_gui_desired_settings() builds from the fields right after Load:
// every global present (current values where the record had none), the lock
// with its expanded tail, and pre-anchor values the editor merely projects
// (here a previously applied profile's shape, never typed).
std::unique_ptr<DesiredSettings> editor_after_load(const DesiredSettings& base) {
    auto e = std::make_unique<DesiredSettings>();
    e->lockCi = -1;
    e->hasGpuOffset = 1;
    e->hasMemOffset = 1;
    e->memOffsetMHz = base.memOffsetMHz;
    e->hasPowerLimit = 1;
    e->powerLimitPct = base.powerLimitPct;
    e->hasFan = 1;
    e->fanAuto = 1;
    e->fanMode = FAN_MODE_AUTO;
    e->hasXbarOffsetKhz = e->hasXbarMsvddOffsetUv = e->hasSysClkOffsetKhz = e->hasVideoClkOffsetKhz = 1;
    e->hasLock = 1;
    e->lockCi = kAnchor;
    e->lockMHz = base.lockMHz;
    e->lockMode = LOCK_MODE_FLATTEN;
    for (int ci = kAnchor; ci <= kLast; ++ci) { e->hasCurvePoint[ci] = 1; e->curvePointMHz[ci] = base.lockMHz; }
    for (int ci = 70; ci < kAnchor; ++ci) { e->hasCurvePoint[ci] = 1; e->curvePointMHz[ci] = 2300 + ci; }
    return e;
}

bool overlay(const DesiredSettings& base, const DesiredSettings& editor, const GuiEditorCurrentScalars& current,
             const bool* typed, DesiredSettings& out, GuiEditorEdits* editsOut = nullptr, char* why = nullptr,
             size_t whySize = 0) {
    const GuiEditorEdits e = gui_editor_edits(base, editor, current, typed);
    if (editsOut) *editsOut = e;
    return gui_editor_overlay(base, editor, e, &out, why, whySize);
}

}  // namespace

int run_gui_editor_base_tests() {
    auto base = imported_flatten();
    if (!desired_curve_intent_valid(base.get()) || !desired_scalar_intent_valid(base.get()) ||
        !desired_authoring_valid(base.get()))
        return 12320;

    // Which record the editor builds on.
    auto legacy = std::make_unique<DesiredSettings>();
    legacy->lockCi = -1;
    if (gui_editor_base_mode_for_record(*base) != GUI_EDITOR_BASE_LOADED_RECORD ||
        gui_editor_base_mode_for_record(*legacy) != GUI_EDITOR_BASE_LOADED_LEGACY ||
        gui_editor_base_select(GUI_EDITOR_BASE_LOADED_RECORD, base.get(), legacy.get()) != base.get() ||
        gui_editor_base_select(GUI_EDITOR_BASE_LOADED_LEGACY, base.get(), base.get()) != nullptr ||
        gui_editor_base_select(GUI_EDITOR_BASE_ACTIVE, legacy.get(), base.get()) != base.get() ||
        gui_editor_base_select(GUI_EDITOR_BASE_ACTIVE, base.get(), legacy.get()) != nullptr ||
        gui_editor_base_select(GUI_EDITOR_BASE_ACTIVE, base.get(), nullptr) != nullptr)
        return 12321;

    // Load, then Apply with no edits: the request is the record itself, the
    // projected pre-anchor values and the expanded tail are not edits.
    auto editor = editor_after_load(*base);
    GuiEditorCurrentScalars current;
    bool typed[VF_NUM_POINTS] = {};
    auto out = std::make_unique<DesiredSettings>();
    GuiEditorEdits edits;
    if (!overlay(*base, *editor, current, typed, *out, &edits) || gui_editor_edits_any(edits)) return 12322;
    if (memcmp(out.get(), base.get(), sizeof(*out)) != 0) return 12322;

    // ...and once prepared it equals what the tray sends for the slot.
    auto tray = std::make_unique<DesiredSettings>();  // zeroed, so memcmp sees no padding
    *tray = *base;
    desired_prepare_replacement_request(tray.get());
    desired_prepare_replacement_request(out.get());
    if (memcmp(out.get(), tray.get(), sizeof(*out)) != 0 || out->resetOcBeforeApply ||
        out->applicationScope != DESIRED_SCOPE_REPLACE_PROFILE)
        return 12323;

    // A power edit replaces power only; the ratio goes, captured points,
    // exact memory and the exact lock stay.
    editor->powerLimitPct = 90;
    if (!overlay(*base, *editor, current, typed, *out, &edits) || !edits.power || edits.memOffset ||
        out->powerLimitPct != 90 || out->powerLimitRatio || out->powerRatioNumerator ||
        !out->memOffsetPrecise || out->memOffsetHz != 1500500000LL || !out->authoring.lockPrecise ||
        !out->curvePointCaptured[72] || out->curvePointCapturedOffsetHz[72] != 115000000LL)
        return 12324;
    editor->powerLimitPct = base->powerLimitPct;

    // A typed point becomes an absolute target; its neighbours stay captured.
    typed[72] = true;
    editor->curvePointMHz[72] = 2400;
    if (!overlay(*base, *editor, current, typed, *out, &edits) || edits.pointCount != 1 || !edits.point[72] ||
        !out->hasCurvePoint[72] || out->curvePointMHz[72] != 2400 || out->curvePointCaptured[72] ||
        !out->curvePointCaptured[71] || !out->curvePointCaptured[73] || !out->authoring.lockPrecise)
        return 12325;
    typed[72] = false;

    // A moved lock owns its new tail: captured points at or above it are
    // replaced by the editor's tail, the exact lock Hz is dropped, lower
    // captured points stay.
    auto moved = editor_after_load(*base);
    moved->lockCi = 74;
    moved->lockMHz = 2900;
    for (int ci = 74; ci <= kLast; ++ci) { moved->hasCurvePoint[ci] = 1; moved->curvePointMHz[ci] = 2900; }
    if (!overlay(*base, *moved, current, typed, *out, &edits) || !edits.lock || edits.lockRegionStart != 74 ||
        out->lockCi != 74 || out->lockMHz != 2900 || out->authoring.lockPrecise || out->authoring.lockFrequencyHz ||
        out->curvePointCaptured[74] || out->curvePointCaptured[75] || !out->hasCurvePoint[75] ||
        out->curvePointMHz[kLast] != 2900 || !out->curvePointCaptured[73] || !desired_curve_intent_valid(out.get()))
        return 12326;

    // Unlocking drops the lock and its tail; the captured shape below stays.
    auto unlocked = editor_after_load(*base);
    unlocked->hasLock = 0;
    unlocked->lockCi = -1;
    unlocked->lockMHz = 0;
    unlocked->lockMode = LOCK_MODE_NONE;
    for (int ci = kAnchor; ci <= kLast; ++ci) { unlocked->hasCurvePoint[ci] = 0; unlocked->curvePointMHz[ci] = 0; }
    if (!overlay(*base, *unlocked, current, typed, *out, &edits) || !edits.lock || out->hasLock || out->lockCi != -1 ||
        out->authoring.lockPrecise || out->hasCurvePoint[kAnchor] || !out->curvePointCaptured[75])
        return 12327;

    // A domain the record left out stays out unless the user changed it from
    // what the GPU runs: fan, GPU offset.
    if (!overlay(*base, *editor, current, typed, *out) || out->hasFan || out->hasGpuOffset) return 12328;
    current.fanChanged = true;
    editor->fanMode = FAN_MODE_FIXED;
    editor->fanAuto = 0;
    editor->fanPercent = 55;
    if (!overlay(*base, *editor, current, typed, *out, &edits) || !edits.fan || !out->hasFan ||
        out->fanMode != FAN_MODE_FIXED || out->fanPercent != 55)
        return 12328;
    current.fanChanged = false;
    editor->fanMode = FAN_MODE_AUTO;
    editor->fanAuto = 1;
    editor->fanPercent = 0;
    editor->gpuOffsetMHz = 50;
    if (!overlay(*base, *editor, current, typed, *out, &edits) || !edits.gpuOffset || !out->hasGpuOffset ||
        out->gpuOffsetMHz != 50 || out->authoring.graphicsPrecise)
        return 12329;
    current.gpuOffsetMHz = 50;  // the GPU already runs +50: not an edit
    if (!overlay(*base, *editor, current, typed, *out, &edits) || edits.gpuOffset || out->hasGpuOffset) return 12329;
    current.gpuOffsetMHz = 0;
    editor->gpuOffsetMHz = 0;

    // A scoped profile omits all advanced fields. Their projected values
    // can lag live state without acquiring ownership in Apply or Save.
    auto sparseBase = std::make_unique<DesiredSettings>(*base);
    sparseBase->hasXbarOffsetKhz = sparseBase->hasXbarMsvddOffsetUv = 0;
    sparseBase->hasSysClkOffsetKhz = sparseBase->hasVideoClkOffsetKhz = 0;
    auto advancedEditor = editor_after_load(*sparseBase);
    advancedEditor->hasXbarOffsetKhz = advancedEditor->hasXbarMsvddOffsetUv = 1;
    advancedEditor->hasSysClkOffsetKhz = advancedEditor->hasVideoClkOffsetKhz = 1;
    GuiEditorCurrentScalars advanced;
    advanced.xbarOffsetKhz = 50000; advanced.xbarMsvddOffsetUv = 12000;
    advanced.sysClkOffsetKhz = -20000; advanced.videoClkOffsetKhz = 30000;
    advanced.xbarProjected = advanced.xbarMsvddProjected = true;
    advanced.sysClkProjected = advanced.videoClkProjected = true;
    if (!overlay(*sparseBase, *advancedEditor, advanced, typed, *out, &edits) ||
        edits.xbar || edits.xbarMsvdd || edits.sysClk || edits.videoClk ||
        out->hasXbarOffsetKhz || out->hasXbarMsvddOffsetUv ||
        out->hasSysClkOffsetKhz || out->hasVideoClkOffsetKhz) return 12681;
    advanced.xbarProjected = false;
    if (!overlay(*sparseBase, *advancedEditor, advanced, typed, *out, &edits) ||
        !edits.xbar || !out->hasXbarOffsetKhz || out->xbarOffsetKhz ||
        out->hasXbarMsvddOffsetUv || out->hasSysClkOffsetKhz || out->hasVideoClkOffsetKhz) return 12682;

    // A typed point equal to the record's own explicit point is no edit.
    auto explicitBase = imported_flatten();
    explicitBase->curvePointCaptured[60] = 0;
    explicitBase->curvePointCapturedOffsetHz[60] = 0;
    explicitBase->hasCurvePoint[60] = 1;
    explicitBase->curvePointMHz[60] = 1900;
    auto explicitEditor = editor_after_load(*explicitBase);
    explicitEditor->hasCurvePoint[60] = 1;
    explicitEditor->curvePointMHz[60] = 1900;
    typed[60] = true;
    if (!overlay(*explicitBase, *explicitEditor, current, typed, *out, &edits) || edits.point[60] ||
        memcmp(out.get(), explicitBase.get(), sizeof(*out)) != 0)
        return 12330;
    typed[60] = false;

    // A result that is not a valid request is refused with a reason, never
    // sent as something else: a typed point outside the curve binding.
    auto unbound = imported_flatten();
    unbound->authoring.voltageUv[30] = 0;
    unbound->authoring.topologyCount = kLast;
    auto unboundEditor = editor_after_load(*unbound);
    unboundEditor->hasCurvePoint[30] = 1;
    unboundEditor->curvePointMHz[30] = 700;
    typed[30] = true;
    char why[160] = {};
    if (overlay(*unbound, *unboundEditor, current, typed, *out, nullptr, why, sizeof(why)) ||
        std::string(why).find("binding") == std::string::npos)
        return 12331;
    typed[30] = false;

    // Projected-only, untyped tail and pre-anchor points never count, even
    // with the typed flag set inside an unchanged lock's tail.
    typed[kAnchor + 3] = true;
    if (!overlay(*base, *editor, current, typed, *out, &edits) || edits.pointCount ||
        out->hasCurvePoint[kAnchor + 3])
        return 12332;
    typed[kAnchor + 3] = false;

    char line[160] = {};
    if (!overlay(*base, *moved, current, typed, *out, &edits)) return 12333;
    gui_editor_edits_describe(edits, line, sizeof(line));
    if (!strstr(line, "lock=1 lockFrom=74") || !strstr(line, "power=0")) return 12333;
    return 0;
}
