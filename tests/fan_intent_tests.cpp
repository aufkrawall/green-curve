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
// Pins every layer of the fix: the shared scope upgrade (Windows service and
// Linux daemon), the GUI's request shape, the reported fan mode, the relaxed
// profile match, and -- executed against the production Windows fan step --
// the service's post-Apply fan/intent invariant.
#include "app_shared.h"
#include "fan_curve.h"
#include "fan_intent_policy.h"
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
    // Owned mode: an unclaimed fan is the driver's, whatever its stale fields say.
    if (fan_owned_intent_mode(false, FAN_MODE_CURVE, false) != FAN_MODE_AUTO) return 12720;
    if (fan_owned_intent_mode(true, FAN_MODE_CURVE, false) != FAN_MODE_CURVE) return 12721;
    if (fan_owned_intent_mode(true, -1, true) != FAN_MODE_AUTO ||
        fan_owned_intent_mode(true, -1, false) != FAN_MODE_FIXED) return 12722;
    if (fan_runtime_driven_mode(false, false) != FAN_MODE_AUTO ||
        fan_runtime_driven_mode(true, false) != FAN_MODE_CURVE ||
        fan_runtime_driven_mode(false, true) != FAN_MODE_FIXED) return 12723;
    // Reported mode after the Apply released the curve: no owned fan, no
    // runtime -> Auto.  The old fallback answered Curve from a stale mirror.
    if (service_reported_fan_intent_mode(false, FAN_MODE_CURVE, false, false, false) != FAN_MODE_AUTO)
        return 12724;
    if (service_reported_fan_intent_mode(false, FAN_MODE_AUTO, true, true, false) != FAN_MODE_CURVE)
        return 12725;
    if (service_reported_fan_intent_mode(true, FAN_MODE_FIXED, false, false, false) != FAN_MODE_FIXED)
        return 12726;
    // Verdicts.
    if (fan_runtime_intent_verdict(FAN_MODE_CURVE, false, false, true) != FAN_RUNTIME_MISSING) return 12727;
    if (fan_runtime_intent_verdict(FAN_MODE_CURVE, true, false, true) != FAN_RUNTIME_MATCHES_INTENT) return 12728;
    if (fan_runtime_intent_verdict(FAN_MODE_CURVE, true, false, false) != FAN_RUNTIME_STALE) return 12729;
    if (fan_runtime_intent_verdict(FAN_MODE_FIXED, true, false, true) != FAN_RUNTIME_STALE) return 12730;
    if (fan_runtime_intent_verdict(FAN_MODE_AUTO, true, false, true) != FAN_RUNTIME_UNOWNED) return 12731;
    if (fan_runtime_intent_verdict(FAN_MODE_AUTO, false, false, false) != FAN_RUNTIME_MATCHES_INTENT) return 12732;
    if (std::strcmp(fan_runtime_intent_verdict_name(FAN_RUNTIME_MISSING), "runtime-missing")) return 12733;
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

#if defined(_WIN32)
// Executes the production post-Apply invariant (main_service_fan_intent.cpp)
// over the production fan step (main_fan_apply.h) with a scripted driver.
namespace fan_intent_service_fixture {
static AppData g_app = {};
static bool g_serviceHasActiveDesired;
static DesiredSettings g_serviceActiveDesired;
static int autoWrites, curveStarts, fixedStarts, runtimeStops, violations;
static bool refuseCurveStart;

static void debug_log(const char* fmt, ...) {
    char line[512] = {};
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (std::strstr(line, "FAN INVARIANT VIOLATION")) ++violations;
}
static void set_last_apply_phase(const char*) {}
static bool app_is_service_process() { return true; }
static int clamp_percent(int value) { return value < 0 ? 0 : (value > 100 ? 100 : value); }
static const char* fan_mode_label(int mode) {
    return mode == FAN_MODE_CURVE ? "Curve" : (mode == FAN_MODE_FIXED ? "Fixed" : "Auto");
}
static void copy_fan_curve(FanCurveConfig* destination, const FanCurveConfig* source) {
    if (destination && source) std::memcpy(destination, source, sizeof(*destination));
}
static bool manual_fan_readback_matches_target(int wantPct, int actualPct, unsigned int, bool) {
    return wantPct == actualPct;
}
static bool nvml_read_fans(char*, size_t) { return g_app.fanSupported != 0; }
static bool nvml_set_fan_auto(char*, size_t) {
    ++autoWrites;
    g_app.fanIsAuto = true;
    return true;
}
static bool nvml_set_fan_manual(int, bool*, char*, size_t) { return true; }
static bool validate_manual_fan_percent_for_runtime(int, char*, size_t) { return true; }
static bool validate_fan_curve_for_runtime(const FanCurveConfig*, char*, size_t) { return true; }
// The production runtime start/stop effects that the invariant observes.
static void stop_fan_curve_runtime() {
    if (g_app.fanCurveRuntimeActive || g_app.fanFixedRuntimeActive) ++runtimeStops;
    g_app.fanCurveRuntimeActive = false;
    g_app.fanFixedRuntimeActive = false;
    g_app.fanRuntimeLastApplyTickMs = 0;
    g_app.activeFanMode = FAN_MODE_AUTO;
}
static void start_fan_curve_runtime() {
    ++curveStarts;
    if (refuseCurveStart) return;
    fan_curve_normalize(&g_app.activeFanCurve);
    g_app.fanCurveRuntimeActive = true;
    g_app.fanFixedRuntimeActive = false;
    g_app.fanRuntimeLastApplyTickMs = 1;
    g_app.activeFanMode = FAN_MODE_CURVE;
    g_app.fanIsAuto = false;
}
static void start_fixed_fan_runtime() {
    ++fixedStarts;
    g_app.fanCurveRuntimeActive = false;
    g_app.fanFixedRuntimeActive = true;
    g_app.fanRuntimeLastApplyTickMs = 1;
    g_app.activeFanMode = FAN_MODE_FIXED;
    g_app.fanIsAuto = false;
}

#include "main_fan_apply.h"
#include "main_service_fan_intent.cpp"

static void reset_gpu() {
    g_app = {};
    g_app.fanSupported = true;
    g_app.fanCount = 2;
    g_app.fanRangeKnown = true;
    g_app.fanMinPct = 30;
    g_app.fanMaxPct = 100;
    g_app.fanIsAuto = true;
    g_app.activeFanMode = FAN_MODE_AUTO;
    gpu_capability_set(&g_app.gpuCapability, SERVICE_MUTATION_DOMAIN_FAN, GPU_DOMAIN_CAP_AVAILABLE);
    g_serviceHasActiveDesired = false;
    g_serviceActiveDesired = {};
    autoWrites = curveStarts = fixedStarts = runtimeStops = violations = 0;
    refuseCurveStart = false;
}
static void own_curve(const FanCurveConfig& c) {
    g_serviceHasActiveDesired = true;
    g_serviceActiveDesired = {};
    g_serviceActiveDesired.lockCi = -1;
    g_serviceActiveDesired.hasFan = 1;
    g_serviceActiveDesired.fanMode = FAN_MODE_CURVE;
    g_serviceActiveDesired.fanCurve = c;
}
static void run_curve(const FanCurveConfig& c) {
    g_app.activeFanCurve = c;
    fan_curve_normalize(&g_app.activeFanCurve);
    fan_curve_clamp_percentages(&g_app.activeFanCurve, 30, 100);
    g_app.fanCurveRuntimeActive = true;
    g_app.fanRuntimeLastApplyTickMs = 1;
    g_app.activeFanMode = FAN_MODE_CURVE;
    g_app.fanIsAuto = false;
}

static int run() {
    char note[384] = {};
    const FanCurveConfig profileCurve = fan_intent_cases::curve_with_floor(30);

    // The state the reported Apply left behind: Profile 3's curve owned, the
    // runtime stopped, the driver in Auto.  The invariant re-establishes it.
    reset_gpu();
    own_curve(profileCurve);
    if (service_enforce_fan_runtime_matches_intent("test", note, sizeof(note)) !=
            SERVICE_FAN_INTENT_REPAIRED) return 12760;
    if (violations != 1 || curveStarts != 1 || !g_app.fanCurveRuntimeActive ||
        !std::strstr(note, "re-established")) return 12761;
    if (service_fan_runtime_intent_verdict(service_owned_fan_mode()) != FAN_RUNTIME_MATCHES_INTENT)
        return 12762;

    // A consistent state is left alone: no write, no log line about a violation.
    violations = curveStarts = 0;
    if (service_enforce_fan_runtime_matches_intent("test", note, sizeof(note)) !=
            SERVICE_FAN_INTENT_HELD || violations || curveStarts || autoWrites || note[0]) return 12763;

    // A runtime that outlived its ownership is handed back to the driver.
    reset_gpu();
    run_curve(profileCurve);
    if (service_enforce_fan_runtime_matches_intent("test", note, sizeof(note)) !=
            SERVICE_FAN_INTENT_REPAIRED || autoWrites != 1 || runtimeStops != 1 ||
        g_app.fanCurveRuntimeActive) return 12764;

    // A runtime driving another curve than the owned one is restarted.
    reset_gpu();
    FanCurveConfig other = fan_intent_cases::curve_with_floor(55);
    own_curve(profileCurve);
    run_curve(other);
    if (service_enforce_fan_runtime_matches_intent("test", note, sizeof(note)) !=
            SERVICE_FAN_INTENT_REPAIRED || curveStarts != 1) return 12765;
    FanCurveConfig wanted = profileCurve;
    fan_curve_normalize(&wanted);
    fan_curve_clamp_percentages(&wanted, 30, 100);
    if (!fan_curve_equals(&wanted, &g_app.activeFanCurve)) return 12766;

    // An owned fixed duty that is running holds.
    reset_gpu();
    g_serviceHasActiveDesired = true;
    g_serviceActiveDesired.hasFan = 1;
    g_serviceActiveDesired.fanMode = FAN_MODE_FIXED;
    g_serviceActiveDesired.fanPercent = 60;
    g_app.activeFanFixedPercent = 60;
    g_app.fanFixedRuntimeActive = true;
    g_app.activeFanMode = FAN_MODE_FIXED;
    if (service_enforce_fan_runtime_matches_intent("test", note, sizeof(note)) !=
            SERVICE_FAN_INTENT_HELD || fixedStarts) return 12767;

    // A repair that cannot take effect is reported, never hidden.
    reset_gpu();
    own_curve(profileCurve);
    refuseCurveStart = true;
    if (service_enforce_fan_runtime_matches_intent("test", note, sizeof(note)) !=
            SERVICE_FAN_INTENT_REPAIR_FAILED || !std::strstr(note, "does not match")) return 12768;

    // No owned fan and no runtime (Auto, including a GPU without a
    // controllable fan): nothing to enforce.
    reset_gpu();
    g_app.fanSupported = false;
    g_app.fanCount = 0;
    gpu_capability_set(&g_app.gpuCapability, SERVICE_MUTATION_DOMAIN_FAN, GPU_DOMAIN_CAP_ABSENT);
    g_serviceHasActiveDesired = true;
    g_serviceActiveDesired.hasFan = 1;
    g_serviceActiveDesired.fanMode = FAN_MODE_AUTO;
    g_serviceActiveDesired.fanAuto = 1;
    if (service_enforce_fan_runtime_matches_intent("test", note, sizeof(note)) !=
            SERVICE_FAN_INTENT_HELD || autoWrites) return 12769;
    return 0;
}
} // namespace fan_intent_service_fixture
#endif

int run_fan_intent_tests() {
    int result = fan_intent_cases::run();
#if defined(_WIN32)
    if (!result) result = fan_intent_service_fixture::run();
#endif
    if (result) std::fprintf(stderr, "fan intent regression failed: %d\n", result);
    return result;
}
#ifdef GC_FAN_INTENT_TEST_MAIN
int main() { return run_fan_intent_tests(); }
#endif
