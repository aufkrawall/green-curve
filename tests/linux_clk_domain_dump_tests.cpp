// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "app_shared.h"
#include "linux_backend.h"
#include <cstdio>
#include <cstring>
#include <string>
#define XBAR_LOG(...) ((void)0)
#include "gpu_backend_xbar.h"
#undef XBAR_LOG
#include "clk_domain_dump.h"

namespace {
struct Lifecycle {
    bool initialized, selected;
    unsigned int opens, closes, controls, versions, queries;
    const GpuAdapterInfo* target;
};
static Lifecycle state;
static int fixture_control(void*, void* buffer) {
    ++state.controls;
    // Leave the markers absent: a successful unknown layout is captured.
    return xbar_get_u32((unsigned char*)buffer, 0) == XBAR_NVAPI_CLK_DOMAINS_VERSION ? 0 : -9;
}
static void* fixture_query(unsigned int id) {
    ++state.queries;
    return id == XBAR_NVAPI_CLK_DOMAINS_GET_CONTROL ? (void*)fixture_control : nullptr;
}
static bool fixture_init(LinuxGpuState* gpu, const GpuAdapterInfo* target, char*, size_t) {
    ++state.opens;
    state.target = target;
    gpu->nvmlLib = (PlLib)1;
    gpu->nvapiQi = fixture_query;
    gpu->gpuHandle = &state;
    gpu->writeIdentityResolved = state.selected;
    std::strcpy(gpu->gpuName, "fixture GPU");
    return state.initialized;
}
static void fixture_shutdown(LinuxGpuState* gpu) {
    if (gpu->nvmlLib) ++state.closes;
    std::memset(gpu, 0, sizeof(*gpu));
}
static nvmlReturn_t fixture_driver(char* version, unsigned int size) {
    ++state.versions;
    std::snprintf(version, size, "580.00\n");
    return NVML_SUCCESS;
}
template<typename T>
static T fixture_symbol(PlLib, const char* name) {
    return std::strcmp(name, "nvmlSystemGetDriverVersion") == 0 ? (T)fixture_driver : nullptr;
}
static void fixture_log(const char*, ...) {}
#define linux_backend_init fixture_init
#define linux_backend_shutdown fixture_shutdown
#define linux_backend_clk_domain_dump fixture_dump
#define sym fixture_symbol
#define lb_log fixture_log
#include "linux_backend_clk_domain_dump.cpp"
#undef lb_log
#undef sym
#undef linux_backend_clk_domain_dump
#undef linux_backend_shutdown
#undef linux_backend_init

static std::string read_report(FILE* file) {
    std::string text(ClkDomainDumpReport::capacity, '\0');
    std::rewind(file);
    text.resize(std::fread(&text[0], 1, text.size(), file));
    return text;
}
}

int run_linux_clk_domain_dump_tests() {
    // 7761-7769: production Linux dispatch/lifecycle with fake driver bindings.
    for (unsigned int mode = 0; mode < 3; ++mode) {
        state = {mode != 2, mode != 1, 0, 0, 0, 0, 0, nullptr};
        GpuAdapterInfo target = {};
        target.valid = true;
        FILE* file = std::tmpfile();
        if (!file) return 7761;
        int result = fixture_dump(&target, file);
        std::string report = read_report(file);
        std::fclose(file);
        if (state.opens != 1 || state.closes != 1 || state.target != &target ||
            state.versions != 1 || state.queries != 5) return 7762;
        if (report.find("nvml-driver: status=0 version=580.00?") == std::string::npos) return 7763;
        if (mode == 0) {
            if (result != 0 || state.controls != 2 ||
                report.find("status=0 layout=mismatch") == std::string::npos ||
                report.find("initialized=1 selection_resolved=1") == std::string::npos) return 7764;
        } else {
            if (result != 2 || state.controls ||
                report.find("get-control: unavailable (GPU handle)") == std::string::npos) return 7765;
            if (mode == 1 && report.find("gpu-selection: unresolved") == std::string::npos) return 7766;
            if (mode == 2 && report.find("initialized=0") == std::string::npos) return 7767;
        }
    }
    {
        state = {true, true, 0, 0, 0, 0, 0, nullptr};
#ifdef _WIN32
        FILE* file = std::fopen("NUL", "rb");
#else
        FILE* file = std::fopen("/dev/null", "rb");
#endif
        if (!file) return 7768;
        int result = fixture_dump(nullptr, file);
        std::fclose(file);
        if (result != 2 || state.closes != 1) return 7769;
    }
    return 0;
}
