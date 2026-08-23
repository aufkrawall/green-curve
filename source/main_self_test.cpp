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

static const unsigned int CLK_PROBE_MAX_DOMAINS = 32;

// Public NvAPI clock ids used to label measured physical domains.  Only
// PROCESSOR/VIDEO are new here: gpu_core.h already carries GRAPHICS/MEMORY
// for the VF path.
enum {
    NVAPI_GPU_PUBLIC_CLOCK_PROCESSOR = 7,
    NVAPI_GPU_PUBLIC_CLOCK_VIDEO = 8,
};

// Shared direct-NvAPI preparation for --self-test / --clk-domain-probe:
// deliberately NOT the shared GUI init helper, which resolves the VF curve
// through the background service and would report nothing useful on a
// machine without a healthy service.
static bool self_test_prepare_direct_nvapi(FILE* out) {
    if (!nvapi_init()) {
        fprintf(out, "NvAPI init             : FAILED\n");
        return false;
    }
    fprintf(out, "NvAPI init             : ok\n");
    if (!nvapi_enum_gpu()) {
        fprintf(out, "NvAPI enumerate        : FAILED (no NVIDIA GPU found)\n");
        return false;
    }
    fprintf(out, "NvAPI enumerate        : ok\n");
    nvapi_read_gpu_metadata();
    fprintf(out, "GPU                    : %s\n",
            g_app.gpuName[0] ? g_app.gpuName : "?");
    if (g_app.gpuPciInfoValid) {
        fprintf(out,
                "PCI identity           : device=0x%04X subsystem=0x%08X"
                " revision=0x%02X\n",
                g_app.gpuDeviceId, g_app.gpuSubSystemId, g_app.gpuPciRevisionId);
    }
    return true;
}

// --- Read-only ClkDomains domain survey --------------------------------
//
// Positive-evidence identification of additional clock domains (video/NVDEC
// candidates) inside the same version-keyed ClkDomains surface the XBAR
// control uses.  It measures every candidate physical clock-domain id, reads
// the public GPU/Memory/Processor/Video frequencies for cross-labeling, and
// dumps the validated control block's per-domain entries.  A future owned
// domain must be pinned from THIS kind of matching evidence, never from
// guessed offsets.  Strictly read-only.
struct SelfTestClockFrequencies {
    unsigned int version;
    unsigned int clockType;  // 0 = CURRENT_FREQ
    struct { unsigned int present; unsigned int frequency; } domain[32];
};

static bool self_test_measure_domain(NvApiFunc measureFunc, void* gpuHandle,
                                     unsigned int domainId,
                                     unsigned int* khzOut) {
    if (!measureFunc || !gpuHandle || !khzOut) return false;
    unsigned int params[3] = {};
    params[0] = XBAR_NVAPI_CLK_MEASURE_VERSION;
    params[1] = domainId;
    if (measureFunc(gpuHandle, params) != 0) return false;
    *khzOut = params[2];
    return params[2] != 0;
}

static void self_test_clk_domain_survey(FILE* out) {
    fprintf(out, "\n--- ClkDomains domain survey (read-only evidence) ---\n");
    void* gpu = g_app.gpuHandle;
    if (!gpu) {
        fprintf(out, "survey skipped: no GPU handle\n");
        return;
    }
    // Public interface DCB616C3 = NvAPI_GPU_GetAllClockFrequencies (V2 struct,
    // VER_3): labels the physical domains measured below.
    auto getAllClocks = (NvApiFunc)nvapi_qi(0xDCB616C3u);
    if (getAllClocks) {
        static SelfTestClockFrequencies freqs;  // ~264 bytes, static anyway
        memset(&freqs, 0, sizeof(freqs));
        freqs.version = (3u << 16) | (unsigned int)sizeof(SelfTestClockFrequencies);
        if (getAllClocks(gpu, &freqs) == 0) {
            static const struct { unsigned int id; const char* name; } publics[] = {
                { NVAPI_GPU_PUBLIC_CLOCK_GRAPHICS, "graphics" },
                { NVAPI_GPU_PUBLIC_CLOCK_MEMORY, "memory" },
                { NVAPI_GPU_PUBLIC_CLOCK_PROCESSOR, "processor" },
                { NVAPI_GPU_PUBLIC_CLOCK_VIDEO, "video" },
            };
            for (unsigned int i = 0; i < 4; ++i) {
                fprintf(out, "public %-9s: present=%u %u kHz\n",
                        publics[i].name,
                        freqs.domain[publics[i].id].present ? 1 : 0,
                        freqs.domain[publics[i].id].frequency);
            }
        } else {
            fprintf(out, "public clocks         : GetAllClockFrequencies FAILED\n");
        }
    }

    auto measure = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_MEASURE);
    if (measure) {
        for (unsigned int id = 0; id < CLK_PROBE_MAX_DOMAINS; ++id) {
            unsigned int khz = 0;
            if (self_test_measure_domain(measure, gpu, id, &khz))
                fprintf(out, "CLK_MEASURE domain %2u : %u kHz\n", id, khz);
            else
                fprintf(out, "CLK_MEASURE domain %2u : refused\n", id);
        }
    }

    auto getControl = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_DOMAINS_GET_CONTROL);
    if (!getControl) {
        fprintf(out, "control block         : GET_CONTROL entry point missing\n");
        return;
    }
    static XbarControlSnapshot snap;  // carries the full 0x13000 block
    memset(&snap, 0, sizeof(snap));
    if (!xbar_read_control(getControl, gpu, &snap)) {
        fprintf(out, "control block         : no pinned schema matched"
                     " (see debug log)\n");
        return;
    }
    fprintf(out, "control block         : schema=0x%08X base=0x%03X"
                 " stride=0x%03X domains=%u\n",
            snap.versionWord, snap.entryBase, snap.entryStride,
            g_xbarSchemas[0].domainCount);
    static const unsigned short entryWords[] = {
        0x00, 0x04, 0x08, 0x0c, 0x10, 0x14,
        0x110, 0x114, 0x118, 0x11c, 0x120, 0x124,
    };
    // Correlate each control entry with the physical domains measured above:
    // scan the entry's full stride for words matching a measured clock
    // (±2%).  Read-only diagnostics — this maps entries to domains from
    // evidence instead of assuming the entry order means anything.
    unsigned int measuredKhz[16] = {};
    bool measuredValid[16] = {};
    if (measure) {
        for (unsigned int id = 0; id < CLK_PROBE_MAX_DOMAINS; ++id)
            measuredValid[id] =
                self_test_measure_domain(measure, gpu, id, &measuredKhz[id]);
    }
    for (unsigned int i = 0; i < g_xbarSchemas[0].domainCount; ++i) {
        unsigned int entry = snap.entryBase + i * snap.entryStride;
        fprintf(out, "entry %u:", i);
        for (unsigned int w = 0; w < sizeof(entryWords) / sizeof(entryWords[0]);
             ++w) {
            fprintf(out, " +%03X:%08X", entryWords[w],
                    xbar_get_u32(snap.buf, entry + entryWords[w]));
        }
        fprintf(out, "\n");
        for (unsigned int id = 0; id < CLK_PROBE_MAX_DOMAINS; ++id) {
            if (!measuredValid[id]) continue;
            unsigned long long low = measuredKhz[id] - measuredKhz[id] / 50;
            unsigned long long high = measuredKhz[id] + measuredKhz[id] / 50;
            unsigned int hitOffsets = 0;
            unsigned int printed = 0;
            char hits[96] = {};
            for (unsigned int off = 0; off + 4 <= snap.entryStride &&
                                       entry + off + 4 <= XBAR_CONTROL_BUF_SIZE;
                 off += 4) {
                unsigned long long v = xbar_get_u32(snap.buf, entry + off);
                if (v >= low && v <= high) {
                    hitOffsets++;
                    if (printed < 3) {
                        char one[24];
                        snprintf(one, sizeof(one), " +%03X:%08X", off,
                                 (unsigned int)v);
                        if (hits[0]) strncat(hits, one, sizeof(hits) - strlen(hits) - 1);
                        else strncpy(hits, one, sizeof(hits) - 1);
                        printed++;
                    }
                }
            }
            if (hitOffsets)
                fprintf(out, "  ~ measure %u (%u kHz): %u match(es)%s%s\n",
                        id, measuredKhz[id], hitOffsets,
                        hits[0] ? ":" : "", hits);
        }
    }

    // ClkDomains GetInfo (read-only): per-domain ranges/identity table.
    // This is the interface that reports WHICH domains exist, so it can name
    // the entries the control block only numbers.
    {
        auto getInfo = (NvApiFunc)nvapi_qi(0x64B43A6Au);
        if (!getInfo) {
            fprintf(out, "clkdomains getinfo   : entry point missing\n");
        } else {
            static unsigned char ibuf[0x13000];
            const unsigned int kInfoWords = 0x928u / 4u;
            memset(ibuf, 0, sizeof(ibuf));
            unsigned int infoWord = 0;
            int status = -1;
            static const unsigned int kCandidateWords[] = {
                0x00010928u,  // (version 1 << 16) | 0x928
                0x00020928u,
                0x00030928u,
            };
            for (unsigned int t = 0;
                 t < sizeof(kCandidateWords) / sizeof(kCandidateWords[0]); ++t) {
                xbar_put_u32(ibuf, 0, kCandidateWords[t]);
                status = getInfo(gpu, ibuf);
                if (status == 0) {
                    infoWord = kCandidateWords[t];
                    break;
                }
            }
            if (status != 0) {
                fprintf(out,
                        "clkdomains getinfo   : FAILED status=%d (no candidate"
                        " version accepted)\n", status);
            } else {
                fprintf(out, "clkdomains getinfo   : ok word=0x%08X\n",
                        infoWord);
                unsigned int numDomains = xbar_get_u32(ibuf, 4);
                fprintf(out, "getinfo domains=%u\n", numDomains);
                // Observed row shape (stride 0x48): [id, pad*7,
                // maxOffsetKHz, minOffsetKHz, extra, pad*7].  Verified:
                // id 0 -> +/-1000000 (GPU), id 4 -> +3000000/-1000000
                // (memory).
                if (numDomains > 8) numDomains = 8;
                for (unsigned int k = 0; k < numDomains; ++k) {
                    unsigned int rowWord = 12 + k * 18;
                    unsigned int id = xbar_get_u32(ibuf, rowWord * 4);
                    int maxKhz = (int)xbar_get_u32(ibuf, (rowWord + 8) * 4);
                    int minKhz = (int)xbar_get_u32(ibuf, (rowWord + 9) * 4);
                    unsigned int extra =
                        xbar_get_u32(ibuf, (rowWord + 10) * 4);
                    fprintf(out, "getinfo row %u: domainId=%u"
                                 " offsetRange=[%d..%d] kHz extra=0x%08X\n",
                            k, id, minKhz, maxKhz, extra);
                }
                fprintf(out, "getinfo raw:");
                for (unsigned int w = 0; w < kInfoWords; ++w)
                    fprintf(out, " %08X", xbar_get_u32(ibuf, w * 4));
                fprintf(out, "\n");
            }
        }
    }

    // PSTATE20 clock-domain table (read-only): which domains exist per
    // pstate, their ranges, and whether the driver marks them editable.
    {
        auto getPerfInfo = (NvApiFunc)nvapi_qi(0x6FF81213u);
        if (!getPerfInfo) {
            fprintf(out, "pstates20             : entry point missing\n");
            return;
        }
        static const unsigned int kPstates20BufSize = 0x2000;
        static unsigned char pbuf[kPstates20BufSize];
        memset(pbuf, 0, sizeof(pbuf));
        // NV_GPU_PERF_PSTATES20_INFO_V1: VER_1 => (1<<16)|0x1C94.
        unsigned int version = 0x00011C94u;
        memcpy(pbuf, &version, sizeof(version));
        if (getPerfInfo(gpu, pbuf) != 0) {
            fprintf(out, "pstates20             : GetPstates20 FAILED\n");
            return;
        }
        // Layout (V1): header = version, flags, numPstates, numClocks,
        // numBaseVoltages.  Each pstate entry = pstateId(4), flags(4),
        // numClocks × clockEntry(0x1C), numBaseVoltages × voltEntry.
        unsigned int flags = xbar_get_u32(pbuf, 4);
        unsigned int numPstates = xbar_get_u32(pbuf, 8);
        unsigned int numClocks = xbar_get_u32(pbuf, 12);
        unsigned int numVolts = xbar_get_u32(pbuf, 16);
        fprintf(out, "pstates20             : flags=%u numPstates=%u"
                     " numClocks=%u numVoltages=%u\n",
                xbar_get_u32(pbuf, 4), xbar_get_u32(pbuf, 8),
                xbar_get_u32(pbuf, 12), xbar_get_u32(pbuf, 16));
        // Entry layout not yet pinned from evidence: dump bounded raw words
        // instead of shipping an interpretation we cannot defend.
        fprintf(out, "pstates20 raw:");
        for (unsigned int w = 5; w < 40; ++w)
            fprintf(out, " %08X", xbar_get_u32(pbuf, w * 4));
        fprintf(out, "\n");
    }
}

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
    if (!self_test_prepare_direct_nvapi(out)) {
        fprintf(out, "\nVerdict: UNUSABLE - NvAPI did not initialize or found no GPU."
                     " On Windows on Arm a missing native nvapia64.dll looks like"
                     " this.\n");
        return driver_self_test_exit_code(DRIVER_SELF_TEST_UNUSABLE);
    }
    // Which NVAPI image actually loaded matters most on arm64: a driver package
    // ships nvapia64.dll (native), nvapi64.dll (x64) and nvapi.dll (x86) side by
    // side, and only the matching one can bind into this process.
    char nvapiPath[MAX_PATH] = {};
    if (g_app.hNvApi) {
        gc_GetModuleFileNameUtf8(g_app.hNvApi, nvapiPath, ARRAY_COUNT(nvapiPath));
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
    self_test_clk_domain_survey(out);

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

// --- --clk-domain-probe: ClkDomains entry identification ---------------
//
// The version-keyed ClkDomains control block carries eight repeated domain
// entries; only entry 1 (XBAR) is publicly mapped.  GetInfo does not name the
// aux domains (it lists GPU/memory ranges only, yet XBAR writes work), and
// every unprobed entry reads as zero — so no read-only evidence can
// distinguish them.  This probe identifies the mapping the only way possible:
// it writes a small +50 MHz offset into ONE candidate entry's frequency-
// offset field through the exact production transaction (fresh full-block
// preimage, single dword change, exact readback), measures every physical
// clock domain before/during/after, then restores the original value.
//
// Transiently mutates GPU clock state by design; each entry is probed and
// restored independently, refusing entries are skipped harmlessly, and the
// user's applied offsets (e.g. XBAR) live in untouched entries and survive.


static void self_test_measure_all(NvApiFunc measureFunc, void* gpuHandle,
                                  unsigned int* khzOut, bool* okOut) {
    // Median of 5 samples per phase: single shots were contaminated by
    // ordinary DVFS state transitions (idle<->boost swings of 1.5 GHz).
    const int kSamples = 5;
    for (unsigned int id = 0; id < CLK_PROBE_MAX_DOMAINS; ++id) {
        unsigned int samples[kSamples] = {};
        int validCount = 0;
        for (int s = 0; s < kSamples; ++s) {
            if (self_test_measure_domain(measureFunc, gpuHandle, id,
                                         &samples[s]))
                validCount++;
        }
        okOut[id] = validCount > 0;
        if (!okOut[id]) {
            khzOut[id] = 0;
            continue;
        }
        // Insertion sort of the few valid samples, then take the median.
        for (int a = 1; a < validCount; ++a) {
            unsigned int v = samples[a];
            int b = a - 1;
            while (b >= 0 && samples[b] > v) {
                samples[b + 1] = samples[b];
                --b;
            }
            samples[b + 1] = v;
        }
        khzOut[id] = samples[validCount / 2];
    }
}

static void self_test_print_measure_deltas(FILE* out,
                                           const unsigned int* baseKhz,
                                           const bool* baseOk,
                                           const unsigned int* nowKhz,
                                           const bool* nowOk) {
    // Public clocks alongside: parks the physical ids against labeled
    // domains (video/engine clocks only move while their engine is active).
    unsigned int publicKhz[16] = {};
    bool publicOk[16] = {};
    auto getAllClocks = (NvApiFunc)nvapi_qi(0xDCB616C3u);
    if (getAllClocks && g_app.gpuHandle) {
        static SelfTestClockFrequencies freqs;
        memset(&freqs, 0, sizeof(freqs));
        freqs.version =
            (3u << 16) | (unsigned int)sizeof(SelfTestClockFrequencies);
        if (getAllClocks(g_app.gpuHandle, &freqs) == 0) {
            publicOk[NVAPI_GPU_PUBLIC_CLOCK_GRAPHICS] = true;
            publicKhz[NVAPI_GPU_PUBLIC_CLOCK_GRAPHICS] =
                freqs.domain[NVAPI_GPU_PUBLIC_CLOCK_GRAPHICS].frequency;
            publicOk[NVAPI_GPU_PUBLIC_CLOCK_MEMORY] = true;
            publicKhz[NVAPI_GPU_PUBLIC_CLOCK_MEMORY] =
                freqs.domain[NVAPI_GPU_PUBLIC_CLOCK_MEMORY].frequency;
            if (freqs.domain[NVAPI_GPU_PUBLIC_CLOCK_PROCESSOR].present) {
                publicOk[NVAPI_GPU_PUBLIC_CLOCK_PROCESSOR] = true;
                publicKhz[NVAPI_GPU_PUBLIC_CLOCK_PROCESSOR] =
                    freqs.domain[NVAPI_GPU_PUBLIC_CLOCK_PROCESSOR].frequency;
            }
            if (freqs.domain[NVAPI_GPU_PUBLIC_CLOCK_VIDEO].present) {
                publicOk[NVAPI_GPU_PUBLIC_CLOCK_VIDEO] = true;
                publicKhz[NVAPI_GPU_PUBLIC_CLOCK_VIDEO] =
                    freqs.domain[NVAPI_GPU_PUBLIC_CLOCK_VIDEO].frequency;
            }
        }
    }
    fprintf(out, "    [public] graphics=%u memory=%u processor=%u"
                 " video=%u kHz\n",
            publicKhz[NVAPI_GPU_PUBLIC_CLOCK_GRAPHICS],
            publicKhz[NVAPI_GPU_PUBLIC_CLOCK_MEMORY],
            publicKhz[NVAPI_GPU_PUBLIC_CLOCK_PROCESSOR],
            publicKhz[NVAPI_GPU_PUBLIC_CLOCK_VIDEO]);
    for (unsigned int id = 0; id < CLK_PROBE_MAX_DOMAINS; ++id) {
        if (!baseOk[id] || !nowOk[id]) continue;
        int delta = (int)nowKhz[id] - (int)baseKhz[id];
        // Print every measurable domain; flag anything beyond ~noise.
        fprintf(out, "    measure %2u: %u -> %u kHz (%+d)%s\n", id,
                baseKhz[id], nowKhz[id], delta,
                (delta > 150000 || delta < -150000) ? "  <<< MOVED" : "");
    }
}

static bool self_test_clk_probe_restore(NvApiFunc getControl,
                                        NvApiFunc setControl, void* gpuHandle,
                                        unsigned int entryFieldOffset,
                                        unsigned int originalValue,
                                        XbarControlSnapshot* snap) {
    if (!xbar_read_control(getControl, gpuHandle, snap)) return false;
    xbar_put_u32(snap->buf, entryFieldOffset, originalValue);
    if (setControl(gpuHandle, snap->buf) != 0) return false;
    if (!xbar_read_control(getControl, gpuHandle, snap)) return false;
    return xbar_get_u32(snap->buf, entryFieldOffset) == originalValue;
}

static int clk_domain_probe_report(FILE* out);

// Dispatches the direct-NvAPI pre-flight/identification commands shared by
// --self-test and --clk-domain-probe.  Returns true when a flag was handled
// and stores the process exit code; the caller owns closing the log handle
// (g_cliExitCode is entry.cpp-local, so the code crosses as a parameter).
static bool self_test_cli_dispatch(const CliOptions& opts, FILE* out,
                                   int* exitCodeOut) {
    if (opts.selfTest) {
        *exitCodeOut = self_test_report(out);
        return true;
    }
    // --clk-domain-probe also drives NvAPI directly.  Unlike --self-test it
    // transiently mutates GPU clock state (small, restored offsets) to map
    // ClkDomains entries to physical domains; it is strictly opt-in.
    if (opts.clkDomainProbe) {
        *exitCodeOut = clk_domain_probe_report(out);
        return true;
    }
    return false;
}

static int clk_domain_probe_report(FILE* out) {
    if (!out) out = stdout;
    fprintf(out, "=== Green Curve ClkDomains entry probe ===\n");
    fprintf(out, "Writes a +50 MHz clock offset into each candidate\n");
    fprintf(out, "control-block entry, median-measures which physical clock\n");
    fprintf(out, "moves, then restores the original value. Run elevated\n");
    fprintf(out, "with steady GPU load for unambiguous results.\n\n");
    if (!self_test_prepare_direct_nvapi(out)) {
        fprintf(out, "\nVerdict: UNUSABLE - NvAPI did not initialize.\n");
        return 2;
    }
    void* gpu = g_app.gpuHandle;
    auto getControl = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_DOMAINS_GET_CONTROL);
    auto setControl = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_DOMAINS_SET_CONTROL);
    auto measure = (NvApiFunc)nvapi_qi(XBAR_NVAPI_CLK_MEASURE);
    if (!getControl || !setControl || !measure || !gpu) {
        fprintf(out, "probe interfaces unavailable (get=%d set=%d"
                     " measure=%d gpu=%d)\n",
                getControl ? 1 : 0, setControl ? 1 : 0,
                measure ? 1 : 0, gpu ? 1 : 0);
        return 2;
    }

    static XbarControlSnapshot snap;  // carries the full 0x13000 block

    unsigned int baseKhz[CLK_PROBE_MAX_DOMAINS] = {};
    bool baseOk[CLK_PROBE_MAX_DOMAINS] = {};
    self_test_measure_all(measure, gpu, baseKhz, baseOk);

    const unsigned int kProbeDeltaKhz = 50000;
    const unsigned int kFreqField = g_xbarSchemas[0].freqOffsetField;
    // Optional single-entry targeting for follow-up experiments while a
    // specific engine is loaded (GC_CLK_PROBE_ENTRY env var, or a
    // clk_probe_entry.txt marker beside the INI for elevated launches, which
    // do not inherit caller environments), e.g. video offsets only show
    // while the video engine is decoding.
    int onlyEntry = -1;
    {
        char envBuf[16] = {};
        size_t envLen = 0;
        if (getenv_s(&envLen, envBuf, sizeof(envBuf), "GC_CLK_PROBE_ENTRY") ==
                0 &&
            envLen > 0) {
            onlyEntry = atoi(envBuf);
        } else {
            const char* base = getenv("LOCALAPPDATA");
            if (base && base[0]) {
                char path[MAX_PATH] = {};
                snprintf(path, sizeof(path),
                         "%s\\Green Curve\\clk_probe_entry.txt", base);
                FILE* f = gc_fopen_utf8(path, "r");
                if (f) {
                    char line[16] = {};
                    if (fgets(line, sizeof(line), f)) onlyEntry = atoi(line);
                    fclose(f);
                    remove(path);
                }
            }
        }
    }
    for (unsigned int k = 0; k < g_xbarSchemas[0].domainCount; ++k) {
        if (onlyEntry >= 0 && (unsigned int)onlyEntry != k) continue;
        if (k == g_xbarSchemas[0].entryIndex && onlyEntry < 0) {
            fprintf(out, "entry %u: XBAR (known owner, skipped)\n", k);
            continue;
        }
        if (!xbar_read_control(getControl, gpu, &snap)) {
            fprintf(out, "entry %u: baseline read failed, aborting probe\n", k);
            break;
        }
        unsigned int entryBase =
            snap.entryBase + k * snap.entryStride;
        unsigned int fieldOffset = entryBase + kFreqField;
        unsigned int original = xbar_get_u32(snap.buf, fieldOffset);
        if (original != 0) {
            fprintf(out, "entry %u: field +%03X already owns %d kHz,"
                         " skipped\n", k, kFreqField, (int)original);
            continue;
        }
        xbar_put_u32(snap.buf, fieldOffset, kProbeDeltaKhz);
        int setStatus = setControl(gpu, snap.buf);
        if (setStatus != 0) {
            fprintf(out, "entry %u: SET_CONTROL refused status=0x%X\n", k,
                    (unsigned)setStatus);
            continue;
        }
        if (!xbar_read_control(getControl, gpu, &snap)) {
            fprintf(out, "entry %u: post-set read failed;"
                         " attempting restore\n", k);
            self_test_clk_probe_restore(getControl, setControl, gpu,
                                        fieldOffset, original, &snap);
            continue;
        }
        unsigned int readback = xbar_get_u32(snap.buf, fieldOffset);
        if (readback != kProbeDeltaKhz) {
            fprintf(out, "entry %u: field did not retain the write"
                         " (readback %d kHz); inert entry, restoring\n", k,
                    (int)readback);
            if (!self_test_clk_probe_restore(getControl, setControl, gpu,
                                             fieldOffset, original, &snap)) {
                fprintf(out, "entry %u: RESTORE FAILED\n", k);
            }
            continue;
        }
        fprintf(out, "entry %u: accepted +%d kHz offset;\n", k,
                (int)kProbeDeltaKhz);
        unsigned int nowKhz[CLK_PROBE_MAX_DOMAINS] = {};
        bool nowOk[CLK_PROBE_MAX_DOMAINS] = {};
        self_test_measure_all(measure, gpu, nowKhz, nowOk);
        self_test_print_measure_deltas(out, baseKhz, baseOk, nowKhz, nowOk);

        // Restore stock on this entry, then re-measure so the next entry's
        // baseline reflects a clean state.
        bool restored = self_test_clk_probe_restore(
            getControl, setControl, gpu, fieldOffset, original, &snap);
        fprintf(out, "entry %u: restored to %d kHz: %s\n", k, (int)original,
                restored ? "ok" : "FAILED");
        if (!restored) break;
        self_test_measure_all(measure, gpu, nowKhz, nowOk);
        memcpy(baseKhz, nowKhz, sizeof(baseKhz));
        memcpy(baseOk, nowOk, sizeof(baseOk));
    }
    fprintf(out, "\nProbe complete. Map probed entries to the domains whose\n");
    fprintf(out, "measured clock moved by ~+50 MHz above.\n");
    return 0;
}
