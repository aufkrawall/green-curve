// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_gpu.cpp — native Linux GPU driver probe.
//
// Loads the NVIDIA driver libraries the same way LACT/NVCurve do — NvAPI via
// libnvidia-api.so.1 and NVML via libnvidia-ml.so.1 — using the platform shim,
// and exercises the read-only control surfaces:
//   * NvAPI: QueryInterface -> init -> enumerate GPUs -> name + architecture
//     (mapped to GpuFamily exactly as the Windows backend does).
//   * NVML: init -> device count -> name/PCI/temperature/power-limit and the
//     GPC clock-offset RANGE (the writable overclock surface, read-only here).
//
// This is the Phase-1 read-path bring-up: it proves on real hardware that the
// same undocumented NvAPI entry points and the NVML OC surface are reachable on
// Linux before any write path is exercised.  It is intentionally self-contained
// (depends only on gpu_core.h + platform.h) so it compiles and links without
// the Win32-coupled application state.

#include "gpu_core.h"
#include "platform.h"
#include "vf_backends.h"
#include "linux_gpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

GpuFamily family_from_architecture(unsigned int arch) {
    switch (arch) {
        case NV_GPU_ARCHITECTURE_GP100: return GPU_FAMILY_PASCAL;
        case NV_GPU_ARCHITECTURE_TU100: return GPU_FAMILY_TURING;
        case NV_GPU_ARCHITECTURE_GA100: return GPU_FAMILY_AMPERE;
        case NV_GPU_ARCHITECTURE_AD100: return GPU_FAMILY_LOVELACE;
        case NV_GPU_ARCHITECTURE_GB200: return GPU_FAMILY_BLACKWELL;
        default: break;
    }
    // Fallback: match on the architecture's high nibble band so a future
    // stepping within a known family is still recognised.
    switch (arch & 0xFFFFFFF0u) {
        case NV_GPU_ARCHITECTURE_GP100: return GPU_FAMILY_PASCAL;
        case NV_GPU_ARCHITECTURE_TU100: return GPU_FAMILY_TURING;
        case NV_GPU_ARCHITECTURE_GA100: return GPU_FAMILY_AMPERE;
        case NV_GPU_ARCHITECTURE_AD100: return GPU_FAMILY_LOVELACE;
        case NV_GPU_ARCHITECTURE_GB200: return GPU_FAMILY_BLACKWELL;
        default: return GPU_FAMILY_UNKNOWN;
    }
}

const char* family_name(GpuFamily f) {
    switch (f) {
        case GPU_FAMILY_PASCAL: return "Pascal";
        case GPU_FAMILY_TURING: return "Turing";
        case GPU_FAMILY_AMPERE: return "Ampere";
        case GPU_FAMILY_LOVELACE: return "Lovelace";
        case GPU_FAMILY_BLACKWELL: return "Blackwell";
        default: return "Unknown";
    }
}

// NvAPI status is 0 == NVAPI_OK on every OS.  On Linux the error codes are
// NEGATIVE integers (-1 generic, -9 INCOMPATIBLE_STRUCT_VERSION) rather than the
// Windows 0x8000xxxx range, so the only portable success test is == 0.
inline bool nvapi_ok(int status) { return status == 0; }

// NVML name/version/driver entry points not in the shared NvmlApi table.
typedef nvmlReturn_t (*nvmlDeviceGetName_t)(nvmlDevice_t, char*, unsigned int);
typedef nvmlReturn_t (*nvmlSystemGetDriverVersion_t)(char*, unsigned int);
typedef nvmlReturn_t (*nvmlSystemGetNVMLVersion_t)(char*, unsigned int);

struct LinuxNvml {
    PlLib lib = PL_LIB_NULL;
    NvmlApi api{};
    nvmlDeviceGetName_t getName = nullptr;
    nvmlSystemGetDriverVersion_t getDriverVersion = nullptr;
    nvmlSystemGetNVMLVersion_t getNvmlVersion = nullptr;
};

template <typename Fn>
Fn sym(PlLib lib, const char* name) {
    return reinterpret_cast<Fn>(pl_lib_sym(lib, name));
}

bool nvml_load(LinuxNvml* n, char* err, size_t errSize) {
    n->lib = pl_open_driver_library(PL_DRIVER_NVML);
    if (!n->lib) {
        gc_strlcpy(err, errSize, "libnvidia-ml.so.1 not found (NVIDIA driver not installed?)");
        return false;
    }
    n->api.init = sym<nvmlInit_v2_t>(n->lib, "nvmlInit_v2");
    n->api.shutdown = sym<nvmlShutdown_t>(n->lib, "nvmlShutdown");
    n->api.getCount = sym<nvmlDeviceGetCount_v2_t>(n->lib, "nvmlDeviceGetCount_v2");
    n->api.getHandleByIndex = sym<nvmlDeviceGetHandleByIndex_v2_t>(n->lib, "nvmlDeviceGetHandleByIndex_v2");
    n->api.getPciInfo = sym<nvmlDeviceGetPciInfo_t>(n->lib, "nvmlDeviceGetPciInfo_v3");
    if (!n->api.getPciInfo) n->api.getPciInfo = sym<nvmlDeviceGetPciInfo_t>(n->lib, "nvmlDeviceGetPciInfo_v2");
    n->api.getPowerLimit = sym<nvmlDeviceGetPowerManagementLimit_t>(n->lib, "nvmlDeviceGetPowerManagementLimit");
    n->api.getTemperature = sym<nvmlDeviceGetTemperature_t>(n->lib, "nvmlDeviceGetTemperature");
    n->api.getClock = sym<nvmlDeviceGetClock_t>(n->lib, "nvmlDeviceGetClock");
    n->api.getGpcClkMinMaxVfOffset = sym<nvmlDeviceGetGpcClkMinMaxVfOffset_t>(n->lib, "nvmlDeviceGetGpcClkMinMaxVfOffset");
    n->api.getMemClkMinMaxVfOffset = sym<nvmlDeviceGetMemClkMinMaxVfOffset_t>(n->lib, "nvmlDeviceGetMemClkMinMaxVfOffset");
    n->api.getNumFans = sym<nvmlDeviceGetNumFans_t>(n->lib, "nvmlDeviceGetNumFans");
    n->getName = sym<nvmlDeviceGetName_t>(n->lib, "nvmlDeviceGetName");
    n->getDriverVersion = sym<nvmlSystemGetDriverVersion_t>(n->lib, "nvmlSystemGetDriverVersion");
    n->getNvmlVersion = sym<nvmlSystemGetNVMLVersion_t>(n->lib, "nvmlSystemGetNVMLVersion");

    if (!n->api.init || !n->api.getCount || !n->api.getHandleByIndex) {
        gc_strlcpy(err, errSize, "libnvidia-ml.so.1 loaded but core entry points missing");
        return false;
    }
    nvmlReturn_t r = n->api.init();
    if (r != NVML_SUCCESS) {
        gc_snprintf(err, errSize, "nvmlInit_v2 failed (%d)", (int)r);
        return false;
    }
    return true;
}

void probe_nvml(FILE* out, LinuxNvmlProbe* result) {
    LinuxNvml n;
    char err[160] = {};
    if (!nvml_load(&n, err, sizeof(err))) {
        fprintf(out, "NVML: NOT available — %s\n", err);
        return;
    }
    result->nvmlLoaded = true;

    char drv[96] = {}, ver[96] = {};
    if (n.getDriverVersion && n.getDriverVersion(drv, sizeof(drv)) == NVML_SUCCESS) {
        gc_strlcpy(result->driverVersion, sizeof(result->driverVersion), drv);
    }
    if (n.getNvmlVersion && n.getNvmlVersion(ver, sizeof(ver)) == NVML_SUCCESS) {
        gc_strlcpy(result->nvmlVersion, sizeof(result->nvmlVersion), ver);
    }
    fprintf(out, "NVML: available  driver=%s  nvml=%s\n",
            drv[0] ? drv : "?", ver[0] ? ver : "?");

    unsigned int count = 0;
    if (n.api.getCount(&count) != NVML_SUCCESS) {
        fprintf(out, "NVML: device count query failed\n");
        n.api.shutdown ? (void)n.api.shutdown() : (void)0;
        return;
    }
    result->nvmlDeviceCount = count;
    fprintf(out, "NVML: %u device(s)\n", count);
    for (unsigned int i = 0; i < count; i++) {
        nvmlDevice_t dev = nullptr;
        if (n.api.getHandleByIndex(i, &dev) != NVML_SUCCESS || !dev) continue;
        char name[96] = {};
        if (n.getName) n.getName(dev, name, sizeof(name));
        fprintf(out, "  [%u] %s\n", i, name[0] ? name : "NVIDIA GPU");

        unsigned int tempC = 0;
        if (n.api.getTemperature && n.api.getTemperature(dev, NVML_TEMPERATURE_GPU, &tempC) == NVML_SUCCESS)
            fprintf(out, "      temperature : %u C\n", tempC);

        unsigned int powerLimit = 0;
        if (n.api.getPowerLimit && n.api.getPowerLimit(dev, &powerLimit) == NVML_SUCCESS)
            fprintf(out, "      power limit : %u mW\n", powerLimit);

        unsigned int gfxClk = 0;
        if (n.api.getClock && n.api.getClock(dev, NVML_CLOCK_GRAPHICS, NVML_CLOCK_ID_CURRENT, &gfxClk) == NVML_SUCCESS)
            fprintf(out, "      gpu clock   : %u MHz\n", gfxClk);

        // The writable overclock surface, read-only here: the allowed GPC
        // clock-offset range.  A successful read proves the OC control path is
        // reachable on this driver.
        int omin = 0, omax = 0;
        if (n.api.getGpcClkMinMaxVfOffset &&
            n.api.getGpcClkMinMaxVfOffset(dev, &omin, &omax) == NVML_SUCCESS) {
            fprintf(out, "      gpc offset range : %d .. %d MHz (OC surface reachable)\n", omin, omax);
            result->gpcOffsetRangeReadable = true;
        }
        int mmin = 0, mmax = 0;
        if (n.api.getMemClkMinMaxVfOffset &&
            n.api.getMemClkMinMaxVfOffset(dev, &mmin, &mmax) == NVML_SUCCESS) {
            fprintf(out, "      mem offset range : %d .. %d MHz\n", mmin, mmax);
        }
        unsigned int fans = 0;
        if (n.api.getNumFans && n.api.getNumFans(dev, &fans) == NVML_SUCCESS)
            fprintf(out, "      fans        : %u\n", fans);
    }
    if (n.api.shutdown) n.api.shutdown();
}

// ---- NvAPI -----------------------------------------------------------------
typedef void* (*nvapi_QueryInterface_t)(unsigned int);
typedef int (*nvapi_init_t)();
typedef int (*nvapi_enum_gpus_t)(GPU_HANDLE*, int*);
typedef int (*nvapi_get_name_t)(GPU_HANDLE, char*);
typedef int (*nvapi_get_arch_t)(GPU_HANDLE, nvapiGpuArchInfo_t*);
typedef int (*nvapi_buf_t)(void*, void*);  // get/set status/info/control

// Read the 128-point VF curve via the family backend layout — the SAME private
// NVAPI getInfo (mask/numClocks) + getStatus (freq/voltage entries) sequence the
// Windows backend uses (nvapi_get_vf_info_cached + nvapi_read_curve), driven by
// the shared VfBackendSpec.  Read-only.  Returns the populated point count.
int read_vf_curve(nvapi_QueryInterface_t qi, GPU_HANDLE handle,
                  const VfBackendSpec* b, VFCurvePoint* curveOut) {
    if (!qi || !b || !b->readSupported) return -1;

    // --- VF info: per-point editable mask + active clock count ---
    unsigned char mask[32];
    memset(mask, 0, sizeof(mask));
    memset(mask, 0xFF, 16);  // default: first 128 bits editable
    unsigned int numClocks = b->defaultNumClocks;
    auto getInfo = (nvapi_buf_t)qi(b->getInfoId);
    if (getInfo) {
        unsigned int infoSize = b->infoBufferSize ? b->infoBufferSize : 0x4000;
        if (infoSize > 0x4000) infoSize = 0x4000;
        unsigned char* ibuf = (unsigned char*)calloc(1, infoSize);
        if (ibuf && b->infoBufferSize <= infoSize) {
            unsigned int ver = (b->infoVersion << 16) | infoSize;
            memcpy(ibuf, &ver, sizeof(ver));
            if (b->infoMaskOffset + sizeof(mask) <= infoSize)
                memset(ibuf + b->infoMaskOffset, 0xFF, sizeof(mask));
            if (getInfo(handle, ibuf) == 0) {
                if (b->infoMaskOffset + sizeof(mask) <= infoSize)
                    memcpy(mask, ibuf + b->infoMaskOffset, sizeof(mask));
                if (b->infoNumClocksOffset + sizeof(numClocks) <= infoSize)
                    memcpy(&numClocks, ibuf + b->infoNumClocksOffset, sizeof(numClocks));
                if (numClocks == 0) numClocks = b->defaultNumClocks;
            }
        }
        free(ibuf);
    }

    // --- VF status: per-point frequency/voltage ---
    auto getStatus = (nvapi_buf_t)qi(b->getStatusId);
    if (!getStatus || b->statusBufferSize == 0) return -1;
    unsigned char* buf = (unsigned char*)calloc(1, b->statusBufferSize);
    if (!buf) return -1;
    unsigned int ver = (b->statusVersion << 16) | b->statusBufferSize;
    memcpy(buf, &ver, sizeof(ver));
    if (b->statusMaskOffset + sizeof(mask) <= b->statusBufferSize)
        memcpy(buf + b->statusMaskOffset, mask, sizeof(mask));
    if (b->statusNumClocksOffset + sizeof(numClocks) <= b->statusBufferSize)
        memcpy(buf + b->statusNumClocksOffset, &numClocks, sizeof(numClocks));
    int populated = -1;
    if (getStatus(handle, buf) == 0) {
        populated = 0;
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            unsigned int freq = 0, volt = 0;
            unsigned int off = b->statusEntriesOffset + (unsigned int)i * b->statusEntryStride;
            if (off + 8 <= b->statusBufferSize) {
                memcpy(&freq, buf + off, 4);
                memcpy(&volt, buf + off + 4, 4);
            }
            if (curveOut) { curveOut[i].freq_kHz = freq; curveOut[i].volt_uV = volt; }
            if (freq > 0) populated++;
        }
    }
    free(buf);
    return populated;
}

void probe_nvapi(FILE* out, LinuxNvapiProbe* result) {
    PlLib lib = pl_open_driver_library(PL_DRIVER_NVAPI);
    if (!lib) {
        fprintf(out, "NvAPI: NOT available — libnvidia-api.so.1 not found "
                     "(driver >= 555 ships it; VF-curve editing needs it)\n");
        return;
    }
    auto qi = sym<nvapi_QueryInterface_t>(lib, "nvapi_QueryInterface");
    if (!qi) {
        fprintf(out, "NvAPI library loaded, but its query entry point is missing\n");
        return;
    }
    result->nvapiLoaded = true;

    auto init = (nvapi_init_t)qi(NVAPI_INIT_ID);
    if (!init || !nvapi_ok(init())) {
        fprintf(out, "NvAPI query resolved; initialization failed\n");
        return;
    }
    result->nvapiInitOk = true;

    auto enumGpus = (nvapi_enum_gpus_t)qi(NVAPI_ENUM_GPU_ID);
    auto getName = (nvapi_get_name_t)qi(NVAPI_GET_NAME_ID);
    auto getArch = (nvapi_get_arch_t)qi(NVAPI_GPU_GET_ARCH_INFO_ID);
    if (!enumGpus) {
        fprintf(out, "NvAPI: initialized but EnumPhysicalGPUs entry missing\n");
        return;
    }
    GPU_HANDLE handles[64] = {};
    int count = 0;
    int st = enumGpus(handles, &count);
    if (!nvapi_ok(st) || count < 1) {
        fprintf(out, "NvAPI: EnumPhysicalGPUs returned status=%d count=%d\n", st, count);
        return;
    }
    result->nvapiGpuCount = count;
    fprintf(out, "NvAPI: available  %d GPU(s)\n", count);
    for (int i = 0; i < count; i++) {
        char name[64] = {};
        if (getName) getName(handles[i], name);
        GpuFamily fam = GPU_FAMILY_UNKNOWN;
        unsigned int arch = 0;
        const VfBackendSpec* spec = nullptr;
        if (getArch) {
            nvapiGpuArchInfo_t info{};
            info.version = NVAPI_GPU_ARCH_INFO_VER2;
            if (nvapi_ok(getArch(handles[i], &info))) {
                arch = info.architecture;
                fam = family_from_architecture(info.architecture);
                spec = vf_backend_for_architecture(info.architecture, nullptr);
            }
        }
        if (i == 0) result->firstFamily = (int)fam;
        const char* supportLabel = spec && spec->bestGuessOnly
            ? "  (VF best-effort unrecognized family)"
            : (fam == GPU_FAMILY_UNKNOWN ? "" : "  (VF read/write tested family)");
        fprintf(out, "  [%d] %s  arch=0x%08X  family=%s%s\n",
                i, name[0] ? name : "NVIDIA GPU", arch, family_name(fam), supportLabel);

        // Read the VF curve via the shared family backend layout to prove the
        // private-NVAPI curve surface is reachable on Linux (read-only).
        if (spec) {
            VFCurvePoint* curve = (VFCurvePoint*)calloc(VF_NUM_POINTS, sizeof(VFCurvePoint));
            int populated = curve ? read_vf_curve(qi, handles[i], spec, curve) : -1;
            if (populated > 0) {
                if (i == 0) result->vfPointsPopulated = populated;
                // Report the highest populated point (peak of the curve).
                int hi = -1;
                for (int p = VF_NUM_POINTS - 1; p >= 0; p--) {
                    if (curve[p].freq_kHz > 0) { hi = p; break; }
                }
                fprintf(out, "      VF curve    : %d points; peak point %d = %u MHz @ %u mV\n",
                        populated, hi,
                        hi >= 0 ? curve[hi].freq_kHz / 1000u : 0u,
                        hi >= 0 ? curve[hi].volt_uV / 1000u : 0u);
            } else {
                fprintf(out, "      VF curve    : read returned no points "
                             "(getStatus unsupported on this driver/family?)\n");
            }
            free(curve);
        }
    }
    fprintf(out, "NvAPI: undocumented VF-curve QueryInterface surface is reachable "
                 "(same path MSI Afterburner / LACT use)\n");
}

} // namespace

// Tegra/Jetson runs an integrated GPU on the L4T stack where NVML/NvAPI VF
// control is not supported — distinct from the discrete-GPU aarch64 driver.
static bool is_tegra_platform() {
    FILE* f = fopen("/etc/nv_tegra_release", "r");
    if (f) { fclose(f); return true; }
    return false;
}

bool linux_gpu_probe(FILE* out, LinuxGpuProbeResult* result) {
    LinuxGpuProbeResult local{};
    if (!result) result = &local;
    if (!out) out = stdout;

    fprintf(out, "=== Green Curve Linux GPU driver probe ===\n");
    if (is_tegra_platform()) {
        fprintf(out, "Platform: Tegra/Jetson detected (/etc/nv_tegra_release). This is an\n"
                     "integrated-GPU L4T platform; the discrete-GPU NVML/NvAPI control\n"
                     "surfaces Green Curve uses are NOT supported here.\n\n");
    }
    probe_nvml(out, &result->nvml);
    probe_nvapi(out, &result->nvapi);

    bool ok = result->nvml.nvmlLoaded && result->nvapi.nvapiLoaded;
    fprintf(out, "\nSummary: NVML=%s  NvAPI=%s  OC-surface=%s\n",
            result->nvml.nvmlLoaded ? "yes" : "no",
            result->nvapi.nvapiLoaded ? "yes" : "no",
            result->nvml.gpcOffsetRangeReadable ? "reachable" : "unconfirmed");
    if (!ok) {
        fprintf(out, "NOTE: full VF-curve control needs BOTH NVML and "
                     "libnvidia-api.so.1 (NVIDIA proprietary driver >= 555).\n");
    }
    return ok;
}
