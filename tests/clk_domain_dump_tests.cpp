// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "app_shared.h"
#include <cstdio>
#include <cstring>
#define XBAR_LOG(...) ((void)0)
#include "gpu_backend_xbar.h"
#undef XBAR_LOG
#include "clk_domain_dump.h"
#include "clk_domain_dump_cli_policy.h"

namespace {
struct Driver {
    enum Shape { MISMATCH, UNKNOWN, VALID, REFUSED, WIDE_REFUSED } shape;
    unsigned int infoCalls, controlCalls, clockCalls, measureCalls;
    bool cleanRequests, refuseInfo;
    unsigned int domainInfoCalls = 0;
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
static int get_domain_info(void* gpu, void* data) {
    auto* state = (Driver*)gpu;
    auto* bytes = (unsigned char*)data;
    static const unsigned int requests[] = {0x000109D8u, 0x00021624u, 0x00034128u, 0x000486ACu};
    unsigned int request = xbar_get_u32(bytes, 0);
    if (state->domainInfoCalls >= 4 || request != requests[state->domainInfoCalls] ||
        xbar_get_u32(bytes, 4) || xbar_get_u32(bytes, 32) ||
        xbar_get_u32(bytes, 0x86A8u) || xbar_get_u32(bytes, XBAR_CONTROL_BUF_SIZE - 4))
        state->cleanRequests = false;
    ++state->domainInfoCalls;
    xbar_put_u32(bytes, 32, 0x13572468u);
    xbar_put_u32(bytes, 0x86A8u, 0xCAFEBABEu);
    if (request != 0x000486ACu || state->refuseInfo) return -9;
    if (state->shape == Driver::UNKNOWN) xbar_put_u32(bytes, 0, 0xFFFF0004u);
    return 0;
}
static int get_control(void* gpu, void* data) {
    auto* state = (Driver*)gpu;
    auto* bytes = (unsigned char*)data;
    ++state->controlCalls;
    if (xbar_get_u32(bytes, 0) != XBAR_NVAPI_CLK_DOMAINS_VERSION ||
        (xbar_get_u32(bytes, XBAR_CONTROL_MASK_OFFSET) != XBAR_CONTROL_DOMAIN_MASK &&
         xbar_get_u32(bytes, XBAR_CONTROL_MASK_OFFSET) != 0xffffffffu) ||
        xbar_get_u32(bytes, 32) || xbar_get_u32(bytes, XBAR_CONTROL_BUF_SIZE - 4))
        state->cleanRequests = false;
    xbar_put_u32(bytes, XBAR_CONTROL_BUF_SIZE - 4, 0xAABBCCDDu);
    if (state->shape == Driver::REFUSED) return -137;
    if (state->shape == Driver::WIDE_REFUSED &&
        xbar_get_u32(bytes, XBAR_CONTROL_MASK_OFFSET) == 0xffffffffu) return -5;
    if (state->shape == Driver::UNKNOWN) xbar_put_u32(bytes, 0, 0xFFFF0004u);
    if (state->shape == Driver::VALID) {
        const auto* schema = xbar_schema_for_version_word(XBAR_NVAPI_CLK_DOMAINS_VERSION);
        for (unsigned int i = 0; i < schema->domainCount; ++i)
            xbar_put_u32(bytes, schema->entryBase + i * schema->entryStride, schema->entryMarker);
    } else if (state->shape == Driver::MISMATCH || state->shape == Driver::WIDE_REFUSED) {
        // GP102's returned wire types, with the same entry geometry as V2.
        static const unsigned int types[] = {4, 5, 4, 5, 5, 2, 2, 4};
        for (unsigned int i = 0; i < 8; ++i)
            xbar_put_u32(bytes, XBAR_PINNED_ENTRY_BASE + i * XBAR_PINNED_ENTRY_STRIDE, types[i]);
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
    if (id == 0x57B5A5DFu) return (void*)get_domain_info;
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
    // 7740-7760, 7770-7772: production capture without a GPU driver.
    queryCount = 0;
    onlyReadQueries = true;
    ClkDomainDumpApi api = clk_domain_dump_api(query);
    if (queryCount != 6 || !onlyReadQueries) return 7740;
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
        if (!state.cleanRequests || state.infoCalls != 3 || state.domainInfoCalls != 4 || state.controlCalls != 2 ||
            state.clockCalls != 1 || state.measureCalls != 32) return 7744;
        if (!contains(report, "mask=0x000000FF") ||
            !contains(report, "mask=0xFFFFFFFF")) return 7759;
        if (!contains(report, "id=0 flags=0x00000001 frequency_khz=321000") ||
            !contains(report, "returned_id=0 status=0 value_raw=0") ||
            !contains(report, "returned_id=17 status=-1 value_raw=265017") ||
            !contains(report, "returned_id=31 status=0 value_raw=265031")) return 7745;
        if (!contains(report, "gpu=GPU?spoofed") || contains(report, "GPU\nspoofed") ||
            !contains(report, "driver: status=0 version_raw=98765") ||
            !contains(report, "complete=yes") || report.size > 650000) return 7746;
        if (!contains(report, "capture format=2") ||
            !contains(report, "get-client-info: request=0x00010928") ||
            !contains(report, "get-domain-info: interface=0x57B5A5DF request=0x000109D8") ||
            !contains(report, "request=0x000486AC returned=0x000486AC status=0 captured=0x086AC") ||
            !contains(report, "+086A0 00000000 00000000 CAFEBABE") ||
            !contains(report, "client_info_reads=3 domain_info_reads=4")) return 7770;
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
        if (!contains(report, "request=0x000486AC returned=0xFFFF0004 status=0 captured=0x086AC") ||
            !contains(report, "+086A0 00000000 00000000 CAFEBABE")) return 7771;
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
        state = {Driver::WIDE_REFUSED, 0, 0, 0, 0, true, true};
        ClkDomainDumpReport report;
        if (!clk_domain_dump_collect(&report, &state, api, identity) ||
            !contains(report, "status=0 layout=mismatch") ||
            !contains(report, "status=-5 layout=request-refused")) return 7760;
    }
    {
        state = {Driver::MISMATCH, 0, 0, 0, 0, true, false};
        ClkDomainDumpReport report;
        if (clk_domain_dump_collect(&report, nullptr, api, identity) ||
            !contains(report, "get-control: unavailable (GPU handle)") ||
            state.infoCalls || state.domainInfoCalls || state.controlCalls || state.clockCalls || state.measureCalls) return 7751;
    }
    {
        ClkDomainDumpReport report;
        if (clk_domain_dump_collect(&report, &state, clk_domain_dump_api(nullptr), identity) ||
            !contains(report, "get-client-info: unavailable (entry point)") ||
            !contains(report, "get-domain-info: unavailable (entry point)") ||
            !contains(report, "driver: unavailable")) return 7752;
    }
    {
        ClkDomainDumpReport report;
        report.size = ClkDomainDumpReport::capacity - 2;
        report.text[report.size] = 0;
        if (clk_domain_dump_collect(&report, &state, api, identity) || report.complete) return 7753;
    }
    {
        const char* args[] = {"greencurve", "--clk-domain-dump", "--config", "settings.ini",
                              "--gpu", "0000:01:00.0"};
        if (!gc_clk_dump::args_read_only(6, args, true) ||
            gc_clk_dump::args_read_only(6, args, false)) return 7754;
        const wchar_t* wide[] = {L"greencurve", L"--config", L"settings.ini", L"--clk-domain-dump"};
        if (!gc_clk_dump::args_read_only(4, wide, false)) return 7755;
    }
    {
        // Reject operations even when they precede the diagnostic: service and
        // startup dispatch normally happen before the Windows dump branch.
        const char* operations[] = {"--service-install", "--service-remove", "--daemon",
            "--apply-config", "--save-config", "--reset", "--resume-restore", "--startup-profile",
            "--fan", "--point49", "--clk-domain-probe", "--self-test", "--probe", "--unknown"};
        for (const char* operation : operations) {
            const char* before[] = {"greencurve", operation, "--clk-domain-dump"};
            const char* after[] = {"greencurve", "--clk-domain-dump", operation};
            if (gc_clk_dump::args_read_only(3, before, true) ||
                gc_clk_dump::args_read_only(3, after, true)) return 7756;
        }
    }
    {
        const char* missing[] = {"greencurve", "--clk-domain-dump", "--config"};
        const char* empty[] = {"greencurve", "--clk-domain-dump", "--config", ""};
        if (gc_clk_dump::args_read_only(3, missing, true) ||
            gc_clk_dump::args_read_only(4, empty, true)) return 7757;
        if (gc_clk_dump::args_read_only<char>(0, nullptr, true)) return 7758;
    }
    {
        // An available advanced INFO remains useful when the other interfaces
        // are absent; the two INFO families are independently optional.
        state = {Driver::MISMATCH, 0, 0, 0, 0, true, false};
        ClkDomainDumpApi domainOnly = {};
        domainOnly.getDomainInfo = get_domain_info;
        ClkDomainDumpReport report;
        if (!clk_domain_dump_collect(&report, &state, domainOnly, identity) ||
            state.domainInfoCalls != 4 || !state.cleanRequests ||
            state.infoCalls || state.controlCalls ||
            !contains(report, "get-client-info: unavailable") ||
            !contains(report, "domain_get_accepted=1 complete=yes")) return 7772;
    }
    return 0;
}
