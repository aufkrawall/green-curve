// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Shared synthetic VF tables and planner helpers for tests/curve_plan_tests.cpp
// and tests/curve_prefix_repair_tests.cpp.  Fixture shapes are described in
// tests/curve_plan_tests.cpp.

#ifndef GREEN_CURVE_TESTS_CURVE_PLAN_FIXTURES_H
#define GREEN_CURVE_TESTS_CURVE_PLAN_FIXTURES_H

#include <string.h>

#include "curve_feasibility_plan.h"

namespace {

struct Table {
    VFCurvePoint curve[VF_NUM_POINTS];
    int offsets[VF_NUM_POINTS];
};

// ---- Pascal --------------------------------------------------------------
unsigned int pascal_volt_uv(int i) {
    if (i <= 25) return (unsigned int)(700000 - (25 - i) * 12500);
    return (unsigned int)(700000 + (long long)(i - 25) * 425000 / 42);
}

unsigned int pascal_base_khz(int i) {
    if (i <= 21) return (unsigned int)(1000000 - (21 - i) * 25000);
    if (i == 22) return 1050000;
    if (i == 23) return 1096500;
    if (i == 24) return 1143000;
    if (i <= 67) return (unsigned int)(1189500 + (long long)(i - 25) * (1974000 - 1189500) / 42);
    return 1974000;
}

void pascal_stock(Table* t) {
    memset(t, 0, sizeof(*t));
    for (int i = 0; i < 80; ++i) {
        t->curve[i].volt_uV = pascal_volt_uv(i);
        t->curve[i].freq_kHz = pascal_base_khz(i);
    }
}

// Another writer's flatten at 1000 MHz from point 21: 22..66 read with
// sub-MHz jitter, 67..79 at exactly 1000000 kHz.
void pascal_external_flatten(Table* t) {
    pascal_stock(t);
    for (int i = 22; i < 80; ++i) {
        const unsigned int live = i >= 67 ? 1000000u : ((i % 2) ? 999600u : 1000400u);
        t->offsets[i] = (int)live - (int)pascal_base_khz(i);
        t->curve[i].freq_kHz = live;
    }
}

// ---- Blackwell-shaped tables ----------------------------------------------
// 100 points from 700 mV in 5 mV steps; bases supplied per case.
void linear_table(Table* t, int count, unsigned int startUv, unsigned int stepUv) {
    memset(t, 0, sizeof(*t));
    for (int i = 0; i < count; ++i) {
        t->curve[i].volt_uV = startUv + (unsigned int)i * stepUv;
        t->curve[i].freq_kHz = 900000u + (unsigned int)i * 10000u;
    }
}

// ---- RTX 5070 floor (same table as tests/vf_point_visibility_tests.cpp) ---
unsigned int rtx_volt_mv(int i) {
    if (i == 127) return 540;
    unsigned int mv = 450;
    for (int k = 1; k <= i; ++k) mv += (k % 4 == 1) ? 10 : 5;
    return mv;
}

void rtx_stock(Table* t, int rampStart) {
    memset(t, 0, sizeof(*t));
    for (int i = 0; i < 127; ++i) {
        t->curve[i].volt_uV = rtx_volt_mv(i) * 1000u;
        unsigned int mhz = 180;
        if (i >= rampStart) {
            mhz = 202 + (unsigned int)(i - rampStart) * 85u;
            if (mhz > 2400) mhz = 2400;
        }
        t->curve[i].freq_kHz = mhz * 1000u;
    }
}

DesiredSettings flatten_request(int lockCi, unsigned int lockMHz) {
    DesiredSettings d = {};
    d.hasLock = true;
    d.lockCi = lockCi;
    d.lockMHz = lockMHz;
    d.lockMode = LOCK_MODE_FLATTEN;
    d.resetOcBeforeApply = true;
    return d;
}

CurvePlanRange nvml_range(int minMHz, int maxMHz) {
    CurvePlanRange r = {};
    r.minKHz = minMHz * 1000;
    r.maxKHz = maxMHz * 1000;
    r.source = CURVE_PLAN_RANGE_NVML_GRAPHICS;
    return r;
}

// The Windows pre-write input shape; `t` and `d` must outlive the input.
CurvePlanInput plan_input_windows(const Table& t, const DesiredSettings& d,
                                  CurvePlanEvidence evidence, CurvePlanRange range) {
    CurvePlanInput in = {};
    in.curve = t.curve;
    in.programmedOffsetsKHz = t.offsets;
    in.floor = vf_floor_topology_compute(t.curve, t.offsets, VF_NUM_POINTS);
    in.evidence = evidence;
    in.range = range;
    in.tailStrategy = CURVE_PLAN_TAIL_UNIFORM_MIN_OFFSET;
    in.provenanceMode = APPLY_LOCK_PRETAIL_PROVENANCE_AGREEMENT;
    in.unownedAtStock = d.resetOcBeforeApply != 0;
    in.prefixAppliesToHardLock = true;
    in.desired = &d;
    in.lockMode = d.lockMode;
    return in;
}

CurvePlanResult plan_windows(const Table& t, const DesiredSettings& d,
                             CurvePlanEvidence evidence, CurvePlanRange range) {
    return curve_plan_evaluate(plan_input_windows(t, d, evidence, range));
}

const CurvePlanIssue* find_issue(const CurvePlanResult& r, CurvePlanReason reason) {
    for (int i = 0; i < r.issueCount; ++i)
        if (r.issues[i].reason == reason) return &r.issues[i];
    return nullptr;
}

}  // namespace

#endif  // GREEN_CURVE_TESTS_CURVE_PLAN_FIXTURES_H
