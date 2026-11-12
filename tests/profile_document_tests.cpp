// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Pure tests for the new authoring document (profile_document.h,
// profile_point_intent.h, tuning_units.h). No GPU, no driver, no IPC: every
// case here constructs values directly and checks structural validity,
// semantic equality and unit conversions.

#include "profile_document.h"

static int run_profile_unit_tests() {
    // UNIT-04: a captured 249000/250000 mW limit reduces canonically and
    // resolves back to 249000 mW on the SAME default, never 250000 mW.
    TuningPowerRatio r = tuning_power_ratio_from_mw(249000, 250000);
    if (r.numerator != 249 || r.denominator != 250) return 7960;
    TuningPowerRatioResolution res = tuning_power_ratio_resolve(r, 250000);
    if (!res.exact || res.resolvedMw != 249000) return 7961;

    // Resolving against a DIFFERENT destination default that does not
    // divide evenly must report conversion-required (exact=false), never
    // silently round.
    TuningPowerRatioResolution res2 = tuning_power_ratio_resolve(r, 100000);
    // 249 * 100000 / 250 = 99600 exactly -- pick a default that is NOT exact.
    if (res2.exact) {
        // 100000 happens to divide evenly (249*100000 = 24,900,000 / 250 =
        // 99600 exactly); use a default that does not.
        TuningPowerRatioResolution res2b = tuning_power_ratio_resolve(r, 100001);
        if (res2b.exact) return 7962;
    }
    TuningPowerRatioResolution res3 = tuning_power_ratio_resolve(r, 300000);
    if (!res3.exact || res3.resolvedMw != 298800) return 7963; // 249*300000/250

    // A user-entered percentage.
    TuningPowerRatio half = tuning_power_ratio_from_percent(50);
    if (half.numerator != 1 || half.denominator != 2) return 7964;
    TuningPowerRatioResolution halfRes = tuning_power_ratio_resolve(half, 250000);
    if (!halfRes.exact || halfRes.resolvedMw != 125000) return 7965;

    // Invalid construction never invents a ratio.
    if (tuning_power_ratio_valid(tuning_power_ratio_from_mw(0, 250000))) return 7966;
    if (tuning_power_ratio_valid(tuning_power_ratio_from_mw(249000, 0))) return 7967;
    if (tuning_power_ratio_valid(tuning_power_ratio_from_mw(-1, 250000))) return 7968;
    if (tuning_power_ratio_valid(tuning_power_ratio_from_percent(0))) return 7969;
    if (tuning_power_ratio_valid(tuning_power_ratio_from_percent(-5))) return 7970;

    // Checked overflow: an enormous ratio against an enormous default must
    // report overflow, never a silently wrapped value.
    TuningPowerRatio huge = tuning_power_ratio_reduce(LLONG_MAX / 2, 3);
    TuningPowerRatioResolution hugeRes = tuning_power_ratio_resolve(huge, LLONG_MAX / 2);
    if (!hugeRes.overflowed) return 7971;

    // Resolving against an invalid destination default refuses, not crashes.
    TuningPowerRatioResolution badDest = tuning_power_ratio_resolve(r, 0);
    if (!badDest.overflowed || badDest.exact) return 7972;

    // UNIT-02: the scale-2 Pascal half-kHz remainder case. 15000500 Hz must
    // report hadRemainder, not silently become a clean 15000 kHz.
    TuningHzToKHzResult half1 = tuning_hz_to_khz(15000500);
    if (half1.kHz != 15000 || !half1.hadRemainder) return 7973;
    TuningHzToKHzResult whole = tuning_hz_to_khz(15000000);
    if (whole.kHz != 15000 || whole.hadRemainder) return 7974;
    TuningHzToKHzResult negative = tuning_hz_to_khz(-1500500);
    if (negative.kHz != -1500 || !negative.hadRemainder) return 7975;

    if (tuning_khz_to_hz(1700) != 1700000) return 7976;
    if (tuning_mhz_to_hz(1700) != 1700000000LL) return 7977;
    const TuningHzToKHzResult minimum = tuning_hz_to_khz(LLONG_MIN);
    if (minimum.kHz != LLONG_MIN / 1000 || !minimum.hadRemainder) return 8070;
    long long scaled = 0;
    if (tuning_scale_i64(LLONG_MAX, 1000, &scaled)) return 8071;
    if (tuning_scale_i64(LLONG_MIN, 1000, &scaled)) return 8072;
    if (!tuning_scale_i64(LLONG_MIN, 1, &scaled) || scaled != LLONG_MIN) return 8073;
    if (tuning_gcd_i64(LLONG_MIN, 1) != 0) return 8074;
    const auto cancelled = tuning_power_ratio_resolve({LLONG_MAX, LLONG_MAX}, 250000);
    if (!cancelled.exact || cancelled.overflowed || cancelled.resolvedMw != 250000) return 8075;
    const auto fractional = tuning_power_ratio_resolve({LLONG_MAX, LLONG_MAX - 1}, 250000);
    if (fractional.exact || fractional.overflowed || fractional.requestedMw != 250000) return 8076;
    const auto overflow = tuning_power_ratio_resolve({LLONG_MAX, 1}, 2);
    if (!overflow.overflowed) return 8077;
    if (!tuning_scale_i64(0, 1000, &scaled) || scaled != 0 ||
        tuning_scale_i64(1, 0, &scaled)) return 8078;
    auto impossible = profile_point_intent_type_absolute_mhz({}, LLONG_MAX);
    if (profile_point_intent_structurally_valid(impossible)) return 8079;
    return 0;
}

static int run_profile_point_intent_tests() {
    // Each kind's own structural-validity rule.
    ProfilePointIntent absent{};
    absent.kind = PROFILE_POINT_ABSENT;
    if (!profile_point_intent_structurally_valid(absent)) return 7980;

    ProfilePointIntent absoluteOk{};
    absoluteOk.kind = PROFILE_POINT_ABSOLUTE_FREQUENCY;
    absoluteOk.absoluteFrequencyHz = 1700000000LL;
    if (!profile_point_intent_structurally_valid(absoluteOk)) return 7981;

    ProfilePointIntent absoluteBad{};
    absoluteBad.kind = PROFILE_POINT_ABSOLUTE_FREQUENCY;
    absoluteBad.absoluteFrequencyHz = 0; // contradictory: typed kind, no target
    if (profile_point_intent_structurally_valid(absoluteBad)) return 7982;

    // Contract 6: zero is a valid OWNED captured offset, never confused with
    // absent.
    ProfilePointIntent capturedZero{};
    capturedZero.kind = PROFILE_POINT_CAPTURED_OFFSET;
    capturedZero.capturedOffsetHz = 0;
    if (!profile_point_intent_structurally_valid(capturedZero)) return 7983;
    if (profile_point_intent_semantically_equal(capturedZero, absent)) return 7984;

    ProfilePointIntent capturedNeg{};
    capturedNeg.kind = PROFILE_POINT_CAPTURED_OFFSET;
    capturedNeg.capturedOffsetHz = -75000000LL;
    if (!profile_point_intent_structurally_valid(capturedNeg)) return 7985;

    // Canonicalization clears inactive payload fields at a trust boundary.
    ProfilePointIntent stray{};
    stray.kind = PROFILE_POINT_PROJECTION;
    stray.absoluteFrequencyHz = 999; // must not survive canonicalization
    stray.capturedOffsetHz = -1;
    ProfilePointIntent canon = profile_point_intent_canonical(stray);
    if (canon.absoluteFrequencyHz != 0 || canon.capturedOffsetHz != 0) return 7986;

    // Semantic equality ignores the observed-frequency preview.
    ProfilePointIntent a = capturedNeg;
    a.hasObservedFrequency = true;
    a.observedFrequencyHz = 1600000000LL;
    ProfilePointIntent b = capturedNeg;
    b.hasObservedFrequency = true;
    b.observedFrequencyHz = 1650000000LL; // moved under load; must not matter
    if (!profile_point_intent_semantically_equal(a, b)) return 7987;

    // A different authoritative payload under the same kind is a real
    // difference.
    ProfilePointIntent c = capturedNeg;
    c.capturedOffsetHz = -50000000LL;
    if (profile_point_intent_semantically_equal(capturedNeg, c)) return 7988;

    // Typing an absolute MHz converts the point and drops captured/projected
    // authority (DOC-07): only that point's kind/value changes.
    ProfilePointIntent typed = profile_point_intent_type_absolute_mhz(capturedNeg, 1700);
    if (typed.kind != PROFILE_POINT_ABSOLUTE_FREQUENCY ||
        typed.absoluteFrequencyHz != 1700000000LL || typed.capturedOffsetHz != 0)
        return 7989;
    return 0;
}

static int run_profile_document_tests() {
    char why[128] = {};
    ProfileDocument d = profile_document_make_empty();
    if (!profile_document_structurally_valid(d, why, sizeof(why))) return 7990;

    // DOC-01/DOC-02: mixed absolute/projected/captured points, including an
    // owned zero, plus exact microvolt topology identity -- all survive
    // regardless of any UI-visibility concept, because none exists here.
    d.topologyCount = 3;
    d.topology[0] = {0, 22, 662500};  // a hidden sub-700 mV point (Pascal)
    d.topology[1] = {1, 40, 950000};
    d.topology[2] = {2, 67, 1125000};
    d.points[0].kind = PROFILE_POINT_CAPTURED_OFFSET;
    d.points[0].capturedOffsetHz = 0; // owned zero
    d.points[1].kind = PROFILE_POINT_ABSOLUTE_FREQUENCY;
    d.points[1].absoluteFrequencyHz = 1700000000LL;
    d.points[2].kind = PROFILE_POINT_PROJECTION;
    if (!profile_document_structurally_valid(d, why, sizeof(why))) return 7991;

    // DOC-04: a document outside current hardware limits (an absurd
    // absolute target) is still a structurally valid document; only
    // planning judges feasibility, never storage.
    ProfileDocument extreme = d;
    extreme.points[1].absoluteFrequencyHz = 9999000000000LL;
    if (!profile_document_structurally_valid(extreme, why, sizeof(why))) return 7992;

    // Duplicate topology identity is a real structural defect.
    ProfileDocument dupIds = d;
    dupIds.topology[2].topologyId = 0;
    if (profile_document_structurally_valid(dupIds, why, sizeof(why))) return 7993;

    // A lock anchor must reference a real topology point in this document.
    ProfileDocument badLock = d;
    badLock.lock.present = true;
    badLock.lock.mode = LOCK_MODE_FLATTEN;
    badLock.lock.anchorTopologyId = 77; // not in topology[]
    badLock.lock.targetFrequencyHz = 1600000000LL;
    if (profile_document_structurally_valid(badLock, why, sizeof(why))) return 7994;
    badLock.lock.anchorTopologyId = 0;
    if (!profile_document_structurally_valid(badLock, why, sizeof(why))) return 7995;

    // A power intent present without a valid ratio is a structural defect
    // (never an implicit 1/1).
    ProfileDocument badPower = d;
    badPower.power.present = true;
    badPower.power.ratio = TuningPowerRatio{0, 0};
    if (profile_document_structurally_valid(badPower, why, sizeof(why))) return 7996;

    // DOC-05: a moving captured preview never changes semantic equality.
    ProfileDocument withPreview = d;
    withPreview.points[0].hasObservedFrequency = true;
    withPreview.points[0].observedFrequencyHz = 1050000000LL;
    ProfileDocument withDifferentPreview = d;
    withDifferentPreview.points[0].hasObservedFrequency = true;
    withDifferentPreview.points[0].observedFrequencyHz = 1051000000LL;
    if (!profile_document_semantically_equal(withPreview, withDifferentPreview))
        return 7997;

    // DOC-06: a fan-only edit leaves the curve/lock/power intent untouched,
    // so it alone must not break equality with the pre-edit document.
    ProfileDocument fanEdited = d;
    fanEdited.fan.present = true;
    fanEdited.fan.fanMode = 1;
    fanEdited.fan.fanPercent = 60;
    ProfileDocument fanEditedAgain = fanEdited;
    fanEditedAgain.fan.fanPercent = 70;
    if (profile_document_semantically_equal(fanEdited, fanEditedAgain)) return 7998;
    fanEditedAgain.fan.fanPercent = 60;
    if (!profile_document_semantically_equal(fanEdited, fanEditedAgain)) return 7999;

    // A real difference in a typed point is not ignored.
    ProfileDocument retyped = d;
    retyped.points[1] = profile_point_intent_type_absolute_mhz(d.points[1], 1800);
    if (profile_document_semantically_equal(d, retyped)) return 8000;
    return 0;
}

int run_profile_document_fixture_tests() {
    if (int f = run_profile_unit_tests()) return f;
    if (int f = run_profile_point_intent_tests()) return f;
    if (int f = run_profile_document_tests()) return f;
    return 0;
}

#ifdef GC_PROFILE_DOCUMENT_TEST_MAIN
#include <cstdio>
int main() {
    const int failure = run_profile_document_fixture_tests();
    printf("Profile document regression: %d\n", failure);
    return failure;
}
#endif
