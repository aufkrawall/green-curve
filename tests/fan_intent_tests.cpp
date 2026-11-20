// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// A profile switch must never leave a fan curve released while the profile is
// reported as active (cases 12720-12779).
//
// The reported sequence: Profile 4 (fan curve) applied, then Profile 3 (same
// fan curve, zero-RPM off) applied from the GUI.  The GUI omitted the
// unchanged fan from its reset-before-apply request (legacy meaning: keep), the
// service upgraded that request to a profile REPLACEMENT (meaning: release),
// wrote driver Auto and stopped the curve, and then still reported Curve and
// Profile 3 as active -- the fan stood still at idle with zero-RPM "disabled".
//
// Pins the ownership layers of the fix: the shared scope upgrade (Windows
// service and Linux daemon), the GUI's request shape and the relaxed profile
// match.
#include "app_shared.h"
#include "fan_curve.h"
#include "gui_apply_shape_policy.h"
#include "profile_ownership_policy.h"
#include "service_scoped_apply_policy.h"
#include "service_desired_mutation_policy.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace fan_intent_cases {

static FanCurveConfig curve_with_floor(int pct) {
    FanCurveConfig c = {};
    fan_curve_set_default(&c);
    c.zeroRpmEnabled = 0;
    for (auto& point : c.points)
        if (point.enabled && point.fanPercent < pct) point.fanPercent = pct;
    return c;
}

// Profile 4 as the service owns it after a successful explicit Apply: exact
// memory/power intent (the trigger for the scope upgrade) and a fan curve.
static DesiredSettings slot4_intent() {
    DesiredSettings d = {};
    d.lockCi = -1;
    d.applicationScope = DESIRED_SCOPE_REPLACE_PROFILE;
    d.hasMemOffset = 1;
    if (!desired_memory_set_precise(&d, 3000LL * 1000000LL, CLOCK_OFFSET_SURFACE_GLOBAL)) d.hasMemOffset = 0;
    d.hasPowerLimit = 1;
    desired_power_set_ratio(&d, tuning_power_ratio_from_percent(100));
    d.hasFan = 1;
    d.fanMode = FAN_MODE_CURVE;
    d.fanAuto = 0;
    d.fanCurve = curve_with_floor(30);
    return d;
}

// The GUI's reset-before-apply request for Profile 3 before the fix: power
// changed, fan unchanged and therefore omitted.
static DesiredSettings gui_reset_request_without_fan() {
    DesiredSettings r = {};
    r.lockCi = -1;
    r.applicationScope = DESIRED_SCOPE_LEGACY;
    r.resetOcBeforeApply = 1;
    r.hasMemOffset = 1; r.memOffsetMHz = 3000;
    r.hasPowerLimit = 1; r.powerLimitPct = 85;
    r.fanMode = FAN_MODE_AUTO;
    return r;
}

static int policy_cases() {
    // GUI request shape: an owned manual fan always rides along.
    if (!gui_full_apply_carries_fan(false, true) || !gui_full_apply_carries_fan(true, false) ||
        gui_full_apply_carries_fan(false, false)) return 12734;
    // Relaxed profile match: only an Auto profile may match an unclaimed fan.
    if (!profile_ownership_fan_mismatch_allowed(true, true, FAN_MODE_AUTO, false)) return 12735;
    if (profile_ownership_fan_mismatch_allowed(true, true, FAN_MODE_CURVE, false) ||
        profile_ownership_fan_mismatch_allowed(true, true, FAN_MODE_FIXED, false)) return 12736;
    return 0;
}

static int upgrade_cases() {
    char why[192] = {};
    const DesiredSettings previous = slot4_intent();

    // The reported transition.  Before the fix: REPLACE_PROFILE with no fan,
    // so the plan wrote driver Auto and the published intent dropped the fan.
    DesiredSettings request = gui_reset_request_without_fan();
    const gc_u32 kept = service_upgrade_legacy_scalar_scope(&previous, &request);
    if (request.applicationScope != DESIRED_SCOPE_REPLACE_PROFILE || request.resetOcBeforeApply)
        return 12740;
    if (!(kept & SERVICE_LEGACY_UPGRADE_KEPT_FAN) || (kept & SERVICE_LEGACY_UPGRADE_KEPT_MEM) ||
        (kept & SERVICE_LEGACY_UPGRADE_KEPT_POWER)) return 12741;
    if (!request.hasFan || request.fanMode != FAN_MODE_CURVE ||
        !fan_curve_equals(&request.fanCurve, &previous.fanCurve)) return 12742;
    ServiceScopedApplyPlan plan = {};
    if (!service_build_scoped_apply_plan(&previous, request, nullptr, {}, false, &plan, why, sizeof(why)))
        return 12743;
    if (!plan.owned.hasFan || plan.owned.fanMode != FAN_MODE_CURVE) return 12744;
    if (plan.hardware.hasFan && fan_request_effective_mode(plan.hardware.fanMode,
            plan.hardware.fanAuto != 0) == FAN_MODE_AUTO) return 12745;
    // Requested domains still win; only omissions are carried.
    if (plan.owned.powerLimitPct != 85 || plan.owned.memOffsetMHz != 3000) return 12746;

    // Linux merges before its scoped compile: the merged record keeps the fan too.
    DesiredSettings linuxRequest = gui_reset_request_without_fan();
    service_upgrade_legacy_scalar_scope(&previous, &linuxRequest);
    const DesiredSettings merged = service_merge_desired_after_mutation(&previous, &linuxRequest);
    if (!merged.hasFan || merged.fanMode != FAN_MODE_CURVE) return 12747;

    // Omitted memory and power are kept with their exact units.
    DesiredSettings sparse = gui_reset_request_without_fan();
    sparse.hasMemOffset = 0; sparse.memOffsetMHz = 0;
    sparse.hasPowerLimit = 0; sparse.powerLimitPct = 0;
    const gc_u32 keptAll = service_upgrade_legacy_scalar_scope(&previous, &sparse);
    if (keptAll != (SERVICE_LEGACY_UPGRADE_KEPT_MEM | SERVICE_LEGACY_UPGRADE_KEPT_POWER |
            SERVICE_LEGACY_UPGRADE_KEPT_FAN)) return 12748;
    if (!sparse.memOffsetPrecise || sparse.memOffsetHz != previous.memOffsetHz ||
        sparse.memOffsetSurface != previous.memOffsetSurface || !sparse.powerLimitRatio ||
        sparse.powerRatioNumerator != previous.powerRatioNumerator) return 12749;

    // An explicit Auto in the request is a real release, not an omission.
    DesiredSettings explicitAuto = gui_reset_request_without_fan();
    explicitAuto.hasFan = 1; explicitAuto.fanAuto = 1; explicitAuto.fanMode = FAN_MODE_AUTO;
    if (service_upgrade_legacy_scalar_scope(&previous, &explicitAuto) & SERVICE_LEGACY_UPGRADE_KEPT_FAN)
        return 12750;
    if (!service_build_scoped_apply_plan(&previous, explicitAuto, nullptr, {}, false, &plan, why, sizeof(why)) ||
        !plan.hardware.hasFan || plan.hardware.fanMode != FAN_MODE_AUTO ||
        plan.owned.fanMode != FAN_MODE_AUTO) return 12751;

    // A verified named-profile replacement is a complete declaration: an
    // omitted fan is still released, exactly once.
    DesiredSettings replacement = gui_reset_request_without_fan();
    replacement.resetOcBeforeApply = 0;
    if (service_upgrade_legacy_scalar_scope(&previous, &replacement, true) != SERVICE_LEGACY_UPGRADE_KEPT_NONE ||
        replacement.applicationScope != DESIRED_SCOPE_REPLACE_PROFILE || replacement.hasFan) return 12752;
    if (!service_build_scoped_apply_plan(&previous, replacement, nullptr, {}, false, &plan, why, sizeof(why)) ||
        !plan.hardware.hasFan || plan.hardware.fanMode != FAN_MODE_AUTO || plan.owned.hasFan) return 12753;

    // A sparse (non-reset) legacy request becomes UPDATE_INCLUDED: the fan is
    // kept by the merge itself and nothing is written for it.
    DesiredSettings powerOnly = {};
    powerOnly.lockCi = -1;
    powerOnly.hasPowerLimit = 1; powerOnly.powerLimitPct = 90;
    if (service_upgrade_legacy_scalar_scope(&previous, &powerOnly) != SERVICE_LEGACY_UPGRADE_KEPT_NONE ||
        powerOnly.applicationScope != DESIRED_SCOPE_UPDATE_INCLUDED) return 12754;
    if (!service_build_scoped_apply_plan(&previous, powerOnly, nullptr, {}, false, &plan, why, sizeof(why)) ||
        plan.hardware.hasFan || !plan.owned.hasFan || plan.owned.fanMode != FAN_MODE_CURVE) return 12755;

    // Without exact previous intent nothing is upgraded (legacy backends keep
    // omitted domains on their own).
    DesiredSettings plainPrevious = {};
    plainPrevious.lockCi = -1; plainPrevious.hasFan = 1; plainPrevious.fanMode = FAN_MODE_CURVE;
    DesiredSettings legacy = gui_reset_request_without_fan();
    if (service_upgrade_legacy_scalar_scope(&plainPrevious, &legacy) != SERVICE_LEGACY_UPGRADE_KEPT_NONE ||
        legacy.applicationScope != DESIRED_SCOPE_LEGACY || !legacy.resetOcBeforeApply || legacy.hasFan)
        return 12756;
    return 0;
}

static int run() {
    if (int failure = policy_cases()) return failure;
    return upgrade_cases();
}
} // namespace fan_intent_cases

int run_fan_intent_tests() {
    int result = fan_intent_cases::run();
    if (result) std::fprintf(stderr, "fan intent regression failed: %d\n", result);
    return result;
}
#ifdef GC_FAN_INTENT_TEST_MAIN
int main() { return run_fan_intent_tests(); }
#endif
