// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Execute production advanced writes and recovery through a fake ClkDomains
// driver. A failed setter deliberately mutates the live block before failing.
#include "app_shared.h"
#include "desired_advanced_domains_policy.h"
#include <cstdio>
#include <cstring>

namespace advanced_apply_fixture {
static AppData g_app = {};
static void debug_log(const char*, ...) {}
static void StringCchCatA(char* out, size_t size, const char* text) {
    const size_t used = std::strlen(out);
    if (used < size) std::snprintf(out + used, size - used, "%s", text);
}
#define XBAR_LOG debug_log
#include "gpu_backend_xbar.h"
#undef XBAR_LOG
static unsigned char live[XBAR_CONTROL_BUF_SIZE] = {};
static int writes = 0, failWrite = 0;
static bool failCleanup = false, functionsAvailable = true;
static int get_control(void*, void* out) {
    std::memcpy(out, live, sizeof(live)); return 0;
}
static int set_control(void*, void* in) {
    ++writes;
    if (failCleanup && writes > failWrite) return -1;
    std::memcpy(live, in, sizeof(live));
    return writes == failWrite ? -1 : 0;
}
static int measure(void*, void*) { return 0; }
static NvApiFunc nvapi_qi(unsigned int id) {
    if (!functionsAvailable) return nullptr;
    if (id == XBAR_NVAPI_CLK_DOMAINS_GET_CONTROL) return get_control;
    if (id == XBAR_NVAPI_CLK_DOMAINS_SET_CONTROL) return set_control;
    return measure;
}
#include "gpu_backend_apply_advanced.h"
static unsigned int freq_field(unsigned int index) {
    return g_xbarSchemas[0].entryBase + index * g_xbarSchemas[0].entryStride +
           g_xbarSchemas[0].freqOffsetField;
}
static void initialize() {
    g_app = {};
    g_app.gpuHandle = (GPU_HANDLE)live;
    g_app.xbarProbeValid = g_app.sysClkProbeValid = g_app.videoClkProbeValid = true;
    writes = failWrite = 0; failCleanup = false; functionsAvailable = true;
    std::memset(live, 0, sizeof(live));
    const auto& schema = g_xbarSchemas[0];
    xbar_put_u32(live, 0, schema.versionWord);
    for (unsigned int i = 0; i < schema.domainCount; ++i)
        xbar_put_u32(live, schema.entryBase + i * schema.entryStride, schema.entryMarker);
}
// A portable profile names advanced fields the selected GPU may not offer.
// Every unavailable surface is dropped alike, so the service apply never
// reaches the refusing preflight with them (desired_advanced_domains_policy.h).
static int run_portable_field_tests() {
    DesiredSettings all = {};
    all.hasXbarOffsetKhz = all.hasXbarMsvddOffsetUv = true;
    all.hasSysClkOffsetKhz = all.hasVideoClkOffsetKhz = true;
    all.xbarOffsetKhz = 90000; all.xbarMsvddOffsetUv = 5000;
    all.sysClkOffsetKhz = 30000; all.videoClkOffsetKhz = 60000;
    all.hasPowerLimit = true; all.powerLimitPct = 90;
    DesiredSettings desired = all;
    if (desired_strip_unavailable_advanced_fields(&desired, true, true, true) ||
        !desired.hasXbarOffsetKhz || !desired.hasXbarMsvddOffsetUv ||
        !desired.hasSysClkOffsetKhz || !desired.hasVideoClkOffsetKhz ||
        desired.sysClkOffsetKhz != 30000 || desired.videoClkOffsetKhz != 60000)
        return 7513;
    desired = all;
    if (desired_strip_unavailable_advanced_fields(&desired, true, false, true) !=
            DESIRED_ADVANCED_FIELD_SYS_CLOCK ||
        desired.hasSysClkOffsetKhz || desired.sysClkOffsetKhz ||
        !desired.hasXbarOffsetKhz || !desired.hasVideoClkOffsetKhz ||
        !desired.hasPowerLimit) return 7514;
    desired = all;
    if (desired_strip_unavailable_advanced_fields(&desired, true, true, false) !=
            DESIRED_ADVANCED_FIELD_VIDEO_CLOCK ||
        desired.hasVideoClkOffsetKhz || desired.videoClkOffsetKhz) return 7515;
    desired = all;
    if (desired_strip_unavailable_advanced_fields(&desired, false, true, true) !=
            (DESIRED_ADVANCED_FIELD_XBAR_CLOCK | DESIRED_ADVANCED_FIELD_XBAR_MSVDD) ||
        desired.hasXbarOffsetKhz || desired.hasXbarMsvddOffsetUv ||
        desired.xbarOffsetKhz || desired.xbarMsvddOffsetUv) return 7516;
    // What survives the strip passes the backend preflight that refuses an
    // unavailable control outright.
    initialize(); g_app.sysClkProbeValid = g_app.videoClkProbeValid = false;
    desired = all;
    char detail[256] = {};
    if (validate_advanced_clock_request(&desired, detail, sizeof(detail))) return 7517;
    desired_strip_unavailable_advanced_fields(&desired, g_app.xbarProbeValid,
        g_app.sysClkProbeValid, g_app.videoClkProbeValid);
    if (!validate_advanced_clock_request(&desired, detail, sizeof(detail)) ||
        !desired.hasPowerLimit) return 7518;
    if (desired_strip_unavailable_advanced_fields(nullptr, false, false, false))
        return 7519;
    return 0;
}

static int run() {
    DesiredSettings desired = {};
    desired.hasXbarOffsetKhz = desired.hasXbarMsvddOffsetUv = true;
    desired.xbarOffsetKhz = 90000; desired.xbarMsvddOffsetUv = 5000;
    desired.hasSysClkOffsetKhz = desired.hasVideoClkOffsetKhz = true;
    desired.sysClkOffsetKhz = 30000; desired.videoClkOffsetKhz = 60000;
    char detail[1024] = {};
    // Every setter may fail after changing hardware. Recovery must clear the
    // failed domain as well as earlier successes and invoke core recovery once.
    for (int failure = 1; failure <= 3; ++failure) {
        initialize(); failWrite = failure;
        int success = 2, failed = 0, recovered = 0;
        bool risk = false; detail[0] = 0;
        int coreOffset = 150, hardPin = 2400, fanDuty = 70;
        if (apply_advanced_clock_transaction(&desired, success, failed, risk,
                detail, sizeof(detail), [&]() {
                    ++recovered; coreOffset = hardPin = fanDuty = 0;
                })) return 7500;
        if (recovered != 1 || failed != 1 || !risk || coreOffset || hardPin || fanDuty)
            return 7501;
        if (xbar_get_u32(live, freq_field(XBAR_PINNED_XBAR_ENTRY_INDEX)) ||
            xbar_get_u32(live, freq_field(XBAR_PINNED_SYS_ENTRY_INDEX)) ||
            xbar_get_u32(live, freq_field(XBAR_PINNED_VIDEO_ENTRY_INDEX))) return 7502;
        XbarControlSnapshot snap = {};
        if (!xbar_read_control(get_control, live, &snap) || snap.msvddOffsetUv) return 7503;
        if (g_app.appliedAdvancedOwnedXbar || g_app.appliedAdvancedOwnedMsvdd ||
            g_app.appliedAdvancedOwnedSysClk || g_app.appliedAdvancedOwnedVideoClk) return 7504;
    }
    initialize(); failWrite = 2; failCleanup = true;
    int success = 0, failed = 0, recovered = 0; bool risk = false; detail[0] = 0;
    if (apply_advanced_clock_transaction(&desired, success, failed, risk,
            detail, sizeof(detail), [&]() { ++recovered; })) return 7505;
    if (recovered != 1 || !g_app.appliedAdvancedOwnedXbar ||
        !g_app.appliedAdvancedOwnedSysClk || !std::strstr(detail, "ownership retained")) return 7506;
    initialize();
    // A sparse rollback must preserve every unrequested field.
    xbar_put_i32(live, freq_field(XBAR_PINNED_VIDEO_ENTRY_INDEX), 123000);
    desired.hasVideoClkOffsetKhz = false;
    success = failed = recovered = 0; risk = false; failWrite = 2;
    if (apply_advanced_clock_transaction(&desired, success, failed, risk,
            detail, sizeof(detail), [&]() { ++recovered; })) return 7507;
    if (xbar_get_u32(live, freq_field(XBAR_PINNED_VIDEO_ENTRY_INDEX)) != 123000) return 7508;
    initialize(); g_app.sysClkProbeValid = false;
    if (validate_advanced_clock_request(&desired, detail, sizeof(detail)) || writes) return 7509;
    desired.hasSysClkOffsetKhz = false;
    if (!validate_advanced_clock_request(&desired, detail, sizeof(detail))) return 7510;
    functionsAvailable = false;
    if (validate_advanced_clock_request(&desired, detail, sizeof(detail))) return 7511;
    initialize(); success = failed = recovered = 0; risk = false;
    if (!apply_advanced_clock_transaction(&desired, success, failed, risk,
            detail, sizeof(detail), [&]() { ++recovered; }) || recovered || failed) return 7512;
    return run_portable_field_tests();
}
} // namespace advanced_apply_fixture
int run_advanced_apply_tests() { return advanced_apply_fixture::run(); }
