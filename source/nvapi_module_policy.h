// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Which NVAPI runtime image this build may load, in preference order.
//
// NVIDIA names the NVAPI runtime per machine architecture and a Windows-on-Arm
// driver package ships all of them side by side.  Measured on the RTX Spark
// developer-preview driver 616.00 (Display.Driver\):
//
//   nvapia64.dll  ARM64  <- the only one a native arm64 process can load
//   nvapi64.dll   x64    <- emulation copy for x64 apps
//   nvapi.dll     x86    <- emulation copy for 32-bit apps
//
// The historical "nvapi64.dll then nvapi.dll" order therefore finds only images
// of the wrong machine type on arm64 and leaves the backend with no NVAPI at
// all.  Pure and header-only so the regression harness can pin the ordering on
// either host — the arm64 case cannot otherwise be tested without arm64
// hardware.
//
// NVML needs no equivalent: nvml.dll is itself the native ARM64 image in that
// package (nvml_arm64ec.dll is the x64/ARM64EC compat copy).

#ifndef GREEN_CURVE_NVAPI_MODULE_POLICY_H
#define GREEN_CURVE_NVAPI_MODULE_POLICY_H

// Architecture selector, so the table can be exercised for BOTH architectures
// from one test binary rather than only the one being compiled.
enum NvapiHostArch {
    NVAPI_HOST_ARCH_X64 = 0,
    NVAPI_HOST_ARCH_ARM64 = 1,
};

// Returns the candidate module name at `index`, or nullptr past the end.  The
// first entry is the native image for that architecture; later entries are
// fallbacks kept so a future package that drops the suffixed name still
// resolves.
static inline const char* nvapi_module_candidate(int arch, int index) {
    if (arch == NVAPI_HOST_ARCH_ARM64) {
        switch (index) {
            case 0: return "nvapia64.dll";
            case 1: return "nvapi64.dll";
            default: return nullptr;
        }
    }
    switch (index) {
        case 0: return "nvapi64.dll";
        case 1: return "nvapi.dll";
        default: return nullptr;
    }
}

// The architecture this translation unit is being compiled for.
static inline int nvapi_module_host_arch() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return NVAPI_HOST_ARCH_ARM64;
#else
    return NVAPI_HOST_ARCH_X64;
#endif
}

#endif // GREEN_CURVE_NVAPI_MODULE_POLICY_H
