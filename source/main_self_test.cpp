// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Windows read-only driver/arch pre-flight (`--self-test`), the counterpart of
// linux_backend_self_test().  Exercises every surface an apply depends on --
// including the CONTROL struct that writes use, read via getControl without
// writing -- and reports the per-domain control surface.
//
// Runs in-process and needs no background service: it is strictly read-only and
// mutates nothing.  Intended as the first thing to run on unvalidated hardware
// (arm64 / Windows on Arm, integrated Grace-Blackwell parts, a new driver)
// so the answer is a report rather than a failed apply.
//
// Included by main_shell.cpp after the capability probe shard.

#include "driver_self_test_policy.h"

static const char* self_test_yes_no(bool value) { return value ? "yes" : "NO"; }

static int self_test_report(FILE* out) {
    if (!out) out = stdout;
    fprintf(out, "=== Green Curve driver/arch self-test (read-only, no GPU changes) ===\n");
#if defined(__aarch64__) || defined(_M_ARM64)
    fprintf(out, "Green Curve build      : windows-arm64\n");
#else
    fprintf(out, "Green Curve build      : windows-x64\n");
#endif

    // Reading the private VF surface needs the same privilege the service runs
    // with.  Report that up front, so an unelevated run is not mistaken for
    // unsupported hardware — the failure looks identical in both cases.
    bool elevated = is_elevated();
    fprintf(out, "Elevated               : %s\n", self_test_yes_no(elevated));

    // Deliberately NOT the shared init helper: in the GUI binary that resolves
    // the VF curve through the background service, so on a machine where the
    // service is absent or wedged it reports "VF curve read failed" and tells
    // us nothing about the driver.  A pre-flight has to answer "can this
    // driver/arch drive the hardware at all", so it drives NvAPI directly and
    // in-process, exactly as linux_backend_self_test() does.  Enforced by a
    // build gate.
    if (!nvapi_init()) {
        fprintf(out, "NvAPI init             : FAILED\n");
        fprintf(out, "\nVerdict: UNUSABLE — NvAPI did not initialize. On Windows on Arm this\n"
                     "is what a missing native nvapia64.dll looks like.\n");
        return driver_self_test_exit_code(DRIVER_SELF_TEST_UNUSABLE);
    }
    fprintf(out, "NvAPI init             : ok\n");
    if (!nvapi_enum_gpu()) {
        fprintf(out, "NvAPI enumerate        : FAILED (no NVIDIA GPU found)\n");
        fprintf(out, "\nVerdict: UNUSABLE — NvAPI initialized but enumerated no GPU.\n");
        return driver_self_test_exit_code(DRIVER_SELF_TEST_UNUSABLE);
    }
    fprintf(out, "NvAPI enumerate        : ok\n");
    nvapi_read_gpu_metadata();

    // Which NVAPI image actually loaded matters most on arm64: a driver package
    // ships nvapia64.dll (native), nvapi64.dll (x64) and nvapi.dll (x86) side by
    // side, and only the matching one can bind into this process.
    char nvapiPath[MAX_PATH] = {};
    if (g_app.hNvApi) {
        gc_GetModuleFileNameUtf8(g_app.hNvApi, nvapiPath, ARRAY_COUNT(nvapiPath));
    }
    fprintf(out, "GPU                    : %s\n",
            g_app.gpuName[0] ? g_app.gpuName : "?");
    if (g_app.gpuPciInfoValid) {
        fprintf(out, "PCI identity           : device=0x%04X subsystem=0x%08X revision=0x%02X\n",
                g_app.gpuDeviceId, g_app.gpuSubSystemId, g_app.gpuPciRevisionId);
    }
    fprintf(out, "NvAPI module           : %s\n",
            nvapiPath[0] ? nvapiPath : "(not loaded)");
    fprintf(out, "VF backend             : %s%s\n",
            g_app.vfBackend && g_app.vfBackend->name ? g_app.vfBackend->name : "<none>",
            g_app.vfBackend && g_app.vfBackend->bestGuessOnly
                ? " (best-effort unrecognized family)" : "");
    fprintf(out, "GPU family             : %d\n", (int)g_app.gpuFamily);
    fprintf(out, "expected struct sizes  : pstates20=%u arch=%u (compile-time pinned)\n",
            (unsigned)sizeof(nvapiPerfPstates20Info_t),
            (unsigned)sizeof(nvapiGpuArchInfo_t));

    // Direct private-NVAPI reads.  This is the step that actually proves the
    // pstates20 entry points resolve on this driver/architecture — the thing an
    // export listing cannot tell you, because they come back by ordinal from
    // nvapi_QueryInterface.
    bool curveOk = nvapi_read_curve();
    bool offsetsOk = nvapi_read_offsets();
    int populated = 0;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        if (g_app.curve[i].freq_kHz) populated++;
    }
    fprintf(out, "NvAPI getStatus (curve): %s (%d/%d points populated)\n",
            curveOk ? "ok" : "FAILED", populated, VF_NUM_POINTS);
    fprintf(out, "NvAPI getControl       : %s (read-only; the struct writes use)\n",
            offsetsOk ? "ok" : "FAILED");

    // Per-domain control surface, probed read-only here.  NVML is reported
    // after this call, not before: the probe is what brings NVML up, so reading
    // g_app.nvmlReady earlier would always print "NO".
    gpu_probe_control_surface();
    fprintf(out, "NVML ready             : %s\n", self_test_yes_no(g_app.nvmlReady));
    const GpuCapabilityProbe* probe = &g_app.gpuCapability;
    fprintf(out, "Memory topology        : %s\n",
            gpu_memory_topology_name(probe->memoryTopology));
    fprintf(out, "Control surface        : %s\n",
            gpu_control_surface_class_name(gpu_capability_surface_class(probe)));
    for (int i = 0; i < GPU_CAP_DOMAIN_COUNT; ++i) {
        gc_u32 mask = gpu_capability_mask_for_index(i);
        fprintf(out, "  %-18s   : %s\n", gpu_capability_domain_name(i),
                gpu_capability_name(gpu_capability_get(probe, mask)));
    }

    bool vfWritable = g_app.vfBackend && g_app.vfBackend->writeSupported &&
                      curveOk && populated > 0;
    gc_u32 surface = gpu_capability_surface_class(probe);
    DriverSelfTestFacts facts = {
        elevated, true, true, curveOk, offsetsOk, g_app.nvmlReady,
        vfWritable, surface
    };
    DriverSelfTestVerdict verdict = driver_self_test_verdict(&facts);
    fprintf(out, "\n");
    switch (verdict) {
        case DRIVER_SELF_TEST_INCONCLUSIVE:
            fprintf(out, "Verdict: INCONCLUSIVE — this process is not elevated and a private\n"
                         "NvAPI VF read failed. Re-run elevated before concluding anything\n"
                         "about this GPU or driver.\n");
            break;
        case DRIVER_SELF_TEST_MONITOR_ONLY:
            fprintf(out, "Verdict: MONITOR-ONLY — telemetry works; no control domain answered.\n");
            break;
        case DRIVER_SELF_TEST_PARTIAL:
            fprintf(out, "Verdict: PARTIAL — NVML, the VF CONTROL read, or one or more\n"
                         "control domains is unavailable (see the exact results above).\n");
            break;
        case DRIVER_SELF_TEST_FULL:
            fprintf(out, "Verdict: FULL — every required read and control domain answered;\n"
                         "the apply path should work on this driver and GPU.\n");
            break;
        default:
            fprintf(out, "Verdict: UNUSABLE — driver initialization did not complete.\n");
            break;
    }
    if (probe->memoryTopology == GPU_MEMORY_TOPOLOGY_UNIFIED) {
        fprintf(out, "NOTE: memory is unified with the CPU. A memory clock offset here\n"
                     "targets system RAM, not a separate VRAM pool.\n");
    }
    if (g_app.vfBackend && g_app.vfBackend->bestGuessOnly) {
        fprintf(out, "NOTE: unrecognized GPU family; the fallback VF layout is a best-effort\n"
                     "guess. Writes stay enabled — verify applied clocks carefully.\n");
    }
    return driver_self_test_exit_code(verdict);
}
