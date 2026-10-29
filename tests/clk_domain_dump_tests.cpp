// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "app_shared.h"
#include <cstdio>
#include <cstring>
#define XBAR_LOG(...) ((void)0)
#include "gpu_backend_xbar.h"
#undef XBAR_LOG
#include "clk_domain_dump.h"

namespace {
struct Driver {
    enum Shape { MISMATCH, UNKNOWN, VALID, REFUSED } shape;
    unsigned int infoCalls, controlCalls, clockCalls, measureCalls;
    bool cleanRequests, refuseInfo;
};
static unsigned int queryCount;
static bool onlyReadQueries;
static int get_info(void* gpu, void* data) {
    auto* state = (Driver*)gpu;
    auto* bytes = (unsigned char*)data;
    unsigned int request = xbar_get_u32(bytes, 0);
    ++state->infoCalls;
    if (xbar_get_u32(bytes, 32) || xbar_get_u32(bytes, XBAR_CONTROL_BUF_SIZE - 4))
        state->cleanRequests = false;
    xbar_put_u32(bytes, 32, 0x12345678u); // even rejection changes memory
    if (request != 0x00020928u || state->refuseInfo) return -9;
    xbar_put_u32(bytes, 4, 17); // no modern domain-count assumptions
    return 0;
}
static int get_control(void* gpu, void* data) {
    auto* state = (Driver*)gpu;
    auto* bytes = (unsigned char*)data;
    ++state->controlCalls;
    if (xbar_get_u32(bytes, 0) != XBAR_NVAPI_CLK_DOMAINS_VERSION ||
        xbar_get_u32(bytes, XBAR_CONTROL_MASK_OFFSET) != XBAR_CONTROL_DOMAIN_MASK ||
        xbar_get_u32(bytes, 32) || xbar_get_u32(bytes, XBAR_CONTROL_BUF_SIZE - 4))
        state->cleanRequests = false;
    xbar_put_u32(bytes, XBAR_CONTROL_BUF_SIZE - 4, 0xAABBCCDDu);
    if (state->shape == Driver::REFUSED) return -137;
    if (state->shape == Driver::UNKNOWN) xbar_put_u32(bytes, 0, 0xFFFF0004u);
    if (state->shape == Driver::VALID) {
        const auto* schema = xbar_schema_for_version_word(XBAR_NVAPI_CLK_DOMAINS_VERSION);
        for (unsigned int i = 0; i < schema->domainCount; ++i)
            xbar_put_u32(bytes, schema->entryBase + i * schema->entryStride, schema->entryMarker);
    }
    return 0;
}
static int get_clocks(void* gpu, void* data) {
    auto* state = (Driver*)gpu;
    ++state->clockCalls;
    auto* words = (unsigned int*)data;
    if (words[0] != 0x00030108u || words[1]) state->cleanRequests = false;
    words[2] = 1;
    words[3] = 321000;
    return 0;
}
static int measure(void* gpu, void* data) {
    auto* state = (Driver*)gpu;
    auto* words = (unsigned int*)data;
    if (words[0] != XBAR_NVAPI_CLK_MEASURE_VERSION || words[1] != state->measureCalls || words[2])
        state->cleanRequests = false;
    ++state->measureCalls;
    words[2] = words[1] ? 265000 + words[1] : 0;
    return words[1] == 17 ? -1 : 0;
}
static int get_driver(unsigned int* version, char*) { *version = 98765; return 0; }
static void* query(unsigned int id) {
    ++queryCount;
    if (id == 0x64B43A6Au) return (void*)get_info;
    if (id == XBAR_NVAPI_CLK_DOMAINS_GET_CONTROL) return (void*)get_control;
    if (id == 0xDCB616C3u) return (void*)get_clocks;
    if (id == XBAR_NVAPI_CLK_MEASURE) return (void*)measure;
    if (id == 0x2926AAADu) return (void*)get_driver;
    onlyReadQueries = false;
    return nullptr;
}
static bool contains(const ClkDomainDumpReport& report, const char* text) {
    return report.text && std::strstr(report.text, text);
}
}

int run_clk_domain_dump_tests() {
    // 7740-7759: production capture on both OSes, without a GPU driver.
    queryCount = 0;
    onlyReadQueries = true;
    ClkDomainDumpApi api = clk_domain_dump_api(query);
    if (queryCount != 5 || !onlyReadQueries) return 7740;
    const ClkDomainDumpIdentity identity = {"fixture", "dev", 123,
        "GPU\nspoofed", 0x130, 0x1234, 0x5678};
    Driver state = {Driver::MISMATCH, 0, 0, 0, 0, true, false};
    {
        ClkDomainDumpReport report;
        // Pascal's GET succeeds with the pinned version and unknown markers.
        if (!clk_domain_dump_collect(&report, &state, api, identity)) return 7741;
        if (!contains(report, "status=0 layout=mismatch") ||
            !contains(report, "+12FF0 00000000 00000000 00000000 AABBCCDD") ||
            !contains(report, "+00010 00000000 00000000 00000000 00000000")) return 7742;
        if (!contains(report, "request=0x00010928 returned=0x00010928 status=-9") ||
            !contains(report, "request=0x00020928 returned=0x00020928 status=0") ||
            !contains(report, "request=0x00030928 returned=0x00030928 status=-9")) return 7743;
        if (!state.cleanRequests || state.infoCalls != 3 || state.controlCalls != 1 ||
            state.clockCalls != 1 || state.measureCalls != 32) return 7744;
        if (!contains(report, "id=0 flags=0x00000001 frequency_khz=321000") ||
            !contains(report, "returned_id=0 status=0 value_raw=0") ||
            !contains(report, "returned_id=17 status=-1 value_raw=265017") ||
            !contains(report, "returned_id=31 status=0 value_raw=265031")) return 7745;
        if (!contains(report, "gpu=GPU?spoofed") || contains(report, "GPU\nspoofed") ||
            !contains(report, "driver: status=0 version_raw=98765") ||
            !contains(report, "complete=yes") || report.size > 300000) return 7746;
    }
    {
        // Producing a diagnostic must not relax normal tuning validation.
        XbarControlSnapshot snapshot = {};
        if (xbar_read_control_quiet(get_control, &state, &snapshot) ||
            snapshot.refusal != XBAR_READ_REFUSAL_LAYOUT_MISMATCH) return 7747;
    }
    {
        state = {Driver::UNKNOWN, 0, 0, 0, 0, true, false};
        ClkDomainDumpReport report;
        if (!clk_domain_dump_collect(&report, &state, api, identity) ||
            !contains(report, "returned=0xFFFF0004") ||
            !contains(report, "layout=unknown-version") ||
            !contains(report, "captured=0x13000")) return 7748;
    }
    {
        state = {Driver::VALID, 0, 0, 0, 0, true, false};
        ClkDomainDumpReport report;
        if (!clk_domain_dump_collect(&report, &state, api, identity) ||
            !contains(report, "status=0 layout=validated")) return 7749;
    }
    {
        state = {Driver::REFUSED, 0, 0, 0, 0, true, true};
        ClkDomainDumpReport report;
        if (clk_domain_dump_collect(&report, &state, api, identity) ||
            !contains(report, "status=-137 layout=request-refused") ||
            !contains(report, "domain_get_accepted=0 complete=yes")) return 7750;
    }
    {
        state = {Driver::MISMATCH, 0, 0, 0, 0, true, false};
        ClkDomainDumpReport report;
        if (clk_domain_dump_collect(&report, nullptr, api, identity) ||
            !contains(report, "get-control: unavailable (GPU handle)") ||
            state.infoCalls || state.controlCalls || state.clockCalls || state.measureCalls) return 7751;
    }
    {
        ClkDomainDumpReport report;
        if (clk_domain_dump_collect(&report, &state, clk_domain_dump_api(nullptr), identity) ||
            !contains(report, "get-info: unavailable (entry point)") ||
            !contains(report, "driver: unavailable")) return 7752;
    }
    {
        ClkDomainDumpReport report;
        report.size = ClkDomainDumpReport::capacity - 2;
        report.text[report.size] = 0;
        if (clk_domain_dump_collect(&report, &state, api, identity) || report.complete) return 7753;
    }
    return 0;
}
