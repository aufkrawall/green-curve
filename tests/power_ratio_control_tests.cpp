// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "power_ratio_control.h"

namespace {
struct Driver {
    unsigned int currentMw, defaultMw, minimumMw, maximumMw;
    int currentReads, defaultReads, writes;
    bool failCurrent, failDefault, failSet, roundWrite, changeDefaultBefore, changeDefaultAfter, changeRangeAfter;
};
static Driver* driver(nvmlDevice_t d) { return reinterpret_cast<Driver*>(d); }
static nvmlReturn_t get_current(nvmlDevice_t device, unsigned int* value) {
    Driver* d = driver(device); ++d->currentReads; *value = d->currentMw;
    return d->failCurrent ? NVML_ERROR_NOT_SUPPORTED : NVML_SUCCESS;
}
static nvmlReturn_t get_default(nvmlDevice_t device, unsigned int* value) {
    Driver* d = driver(device); ++d->defaultReads;
    if (d->changeDefaultBefore && d->defaultReads == 2) d->defaultMw = 260000;
    *value = d->defaultMw;
    return d->failDefault ? NVML_ERROR_NOT_SUPPORTED : NVML_SUCCESS;
}
static nvmlReturn_t get_range(nvmlDevice_t device, unsigned int* lo, unsigned int* hi) {
    Driver* d = driver(device); *lo = d->minimumMw; *hi = d->maximumMw; return NVML_SUCCESS;
}
static nvmlReturn_t set_limit(nvmlDevice_t device, unsigned int value) {
    Driver* d = driver(device); ++d->writes;
    d->currentMw = value + (d->roundWrite ? 1000 : 0);
    if (d->changeDefaultAfter) d->defaultMw = 260000;
    if (d->changeRangeAfter) d->maximumMw = 340000;
    return d->failSet ? NVML_ERROR_NOT_SUPPORTED : NVML_SUCCESS;
}
static void initialize(Driver* d, NvmlApi* a) {
    *d = {}; *a = {};
    d->currentMw = d->defaultMw = 250000; d->minimumMw = 100000; d->maximumMw = 350000;
    a->getPowerLimit = get_current; a->getPowerDefaultLimit = get_default;
    a->getPowerConstraints = get_range; a->setPowerLimit = set_limit;
}
static PowerRatioControlResult apply(Driver* d, const NvmlApi& a, TuningPowerRatio ratio,
    bool force = false, unsigned int grid = 1, unsigned int expectedDefault = 0) {
    const auto device = reinterpret_cast<nvmlDevice_t>(d);
    return power_ratio_control_apply(ratio,
        [&] { PowerRatioSurface s = power_ratio_read_nvml(a, device); s.gridMw = grid; return s; },
        [&](unsigned int target) {
            if (!a.setPowerLimit) return PowerRatioControlWrite{};
            return PowerRatioControlWrite{true, a.setPowerLimit(device, target) == NVML_SUCCESS};
        }, force, expectedDefault);
}
}

int run_power_ratio_control_tests() {
    Driver d{}; NvmlApi a{};
    initialize(&d, &a);
    PowerRatioControlResult r = apply(&d, a, {249, 250});
    if (!r.success || !r.writeAttempted || d.writes != 1 || d.currentMw != 249000 ||
        r.plan.targetMw != 249000 || r.plan.defaultMw != 250000) return 8170;
    r = apply(&d, a, {249, 250});
    if (!r.success || r.writeAttempted || d.writes != 1 || r.plan.writeRequired) return 8171;
    initialize(&d, &a);
    r = apply(&d, a, {1, 3});
    if (r.success || r.writeAttempted || d.writes || r.plan.status != POWER_RATIO_CONVERSION_REQUIRED ||
        r.plan.lowerRepresentableMw != 83333) return 8172;
    initialize(&d, &a);
    r = apply(&d, a, {249001, 250000}, false, 1000);
    if (r.success || d.writes || r.plan.status != POWER_RATIO_CONVERSION_REQUIRED ||
        r.plan.lowerRepresentableMw != 249000) return 8173;
    initialize(&d, &a);
    r = apply(&d, a, {249001, 250000});
    if (!r.success || d.currentMw != 249001 || d.writes != 1) return 8174;
    initialize(&d, &a); d.failSet = true;
    r = apply(&d, a, {249, 250});
    if (r.success || !r.writeAttempted || d.writes != 1 || d.currentMw != 249000 ||
        r.after.currentValid || r.plan.status != POWER_RATIO_WRITE_FAILED) return 8175;
    initialize(&d, &a); d.roundWrite = true;
    r = apply(&d, a, {249, 250});
    if (r.success || !r.writeAttempted || d.writes != 1 || r.plan.status != POWER_RATIO_VERIFY_FAILED) return 8176;
    initialize(&d, &a); d.changeDefaultBefore = true;
    r = apply(&d, a, {249, 250});
    if (r.success || d.writes || r.plan.status != POWER_RATIO_STATE_CHANGED) return 8177;
    initialize(&d, &a); d.changeDefaultAfter = true;
    r = apply(&d, a, {249, 250});
    if (r.success || d.writes != 1 || r.plan.status != POWER_RATIO_VERIFY_FAILED) return 8178;
    initialize(&d, &a); d.changeRangeAfter = true;
    r = apply(&d, a, {249, 250});
    if (r.success || d.writes != 1 || r.plan.status != POWER_RATIO_VERIFY_FAILED) return 8179;
    initialize(&d, &a); a.setPowerLimit = nullptr; d.currentMw = 249000;
    r = apply(&d, a, {1, 1});
    if (!r.success || d.writes || r.plan.status != POWER_RATIO_INERT) return 8180;
    r = apply(&d, a, {249, 250});
    if (r.success || d.writes || r.plan.status != POWER_RATIO_UNAVAILABLE) return 8181;
    r = apply(&d, a, {1, 1}, true);
    if (r.success || d.writes || r.plan.status != POWER_RATIO_UNAVAILABLE) return 8182;
    initialize(&d, &a); d.failDefault = true;
    r = apply(&d, a, {249, 250});
    if (r.success || d.writes || r.after.defaultValid) return 8183;
    initialize(&d, &a); d.maximumMw = 240000;
    r = apply(&d, a, {249, 250});
    if (r.success || d.writes || r.plan.status != POWER_RATIO_OUT_OF_RANGE) return 8184;
    initialize(&d, &a); a.getPowerConstraints = nullptr;
    r = apply(&d, a, {2, 1});
    if (r.success || d.writes || r.plan.status != POWER_RATIO_OUT_OF_RANGE || !r.plan.fallbackBounds) return 8185;
    initialize(&d, &a);
    r = apply(&d, a, {0, 1});
    if (r.success || d.currentReads || d.defaultReads || d.writes || r.plan.status != POWER_RATIO_INVALID) return 8186;
    r = apply(&d, a, {LLONG_MAX, 1});
    if (r.success || d.writes || r.plan.status != POWER_RATIO_INVALID) return 8187;
    initialize(&d, &a);
    r = apply(&d, a, {249, 250}, false, 1, 240000);
    if (r.success || d.writes || r.plan.status != POWER_RATIO_STATE_CHANGED) return 8188;
    return 0;
}
