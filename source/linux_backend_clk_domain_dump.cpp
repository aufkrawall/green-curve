// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included by linux_backend.cpp; do not compile separately.
#include "clk_domain_dump.h"

int linux_backend_clk_domain_dump(const GpuAdapterInfo* target, FILE* out) {
    if (!out) out = stdout;
    LinuxGpuState gpu = {};
    char initErr[256] = {};
    bool initialized = linux_backend_init(&gpu, target, initErr, sizeof(initErr));
    // Discovery can expose a telemetry fallback for an unresolved selection.
    // A diagnostic must not present that other GPU's raw data as the target's.
    bool selected = initialized && gpu.writeIdentityResolved;
    ClkDomainDumpIdentity identity = {"linux", APP_VERSION, APP_BUILD_NUMBER,
        gpu.gpuName, gpu.architecture, gpu.selectedGpu.deviceId, gpu.selectedGpu.subSystemId};
    ClkDomainDumpReport report;
    bool usable = clk_domain_dump_collect(&report, selected ? gpu.gpuHandle : nullptr,
        clk_domain_dump_api(gpu.nvapiQi), identity);
    // The private NvAPI system-version interface is optional on Linux. NVML
    // supplies the installed driver label even when that interface is absent.
    auto getDriverVersion = gpu.nvmlLib
        ? sym<nvmlReturn_t (*)(char*, unsigned int)>(gpu.nvmlLib, "nvmlSystemGetDriverVersion")
        : nullptr;
    if (getDriverVersion) {
        char version[96] = {};
        int status = (int)getDriverVersion(version, sizeof(version));
        version[sizeof(version) - 1] = 0;
        for (char* p = version; *p; ++p)
            if ((unsigned char)*p < 32 || (unsigned char)*p > 126) *p = '?';
        report.append("nvml-driver: status=%d version=%s\n", status, version);
    } else report.append("nvml-driver: unavailable\n");
    report.append("backend: initialized=%u selection_resolved=%u\n",
                  initialized ? 1u : 0u, selected ? 1u : 0u);
    if (initialized && !selected)
        report.append("gpu-selection: unresolved; choose the intended GPU with --gpu DDDD:BB:DD.F\n");
    // Partial initialization still owns any libraries it opened. Release them
    // on every exit; this command never talks to or starts the daemon.
    linux_backend_shutdown(&gpu);
    bool written = report.complete && fwrite(report.text, 1, report.size, out) == report.size &&
                   fflush(out) == 0;
    if (!report.complete) fprintf(out, "clk_dump: report allocation or capacity failed\n");
    lb_log("clk_dump: initialized=%u selected=%u domain_get_accepted=%u written=%u bytes=%zu\n",
           initialized ? 1u : 0u, selected ? 1u : 0u, usable ? 1u : 0u,
           written ? 1u : 0u, report.size);
    return usable && written ? 0 : 2;
}
