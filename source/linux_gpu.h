// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_gpu.h — public interface of the native Linux GPU driver probe.

#ifndef GREEN_CURVE_LINUX_GPU_H
#define GREEN_CURVE_LINUX_GPU_H

#include <stdio.h>

// Intentionally does NOT include gpu_core.h: this header is included by
// linux_main.cpp, which also pulls in linux_port.h's (currently parallel)
// DesiredSettings/fan structs.  Keeping this header self-contained avoids the
// collision until the Linux client is unified onto gpu_core.h (Phase 3).
// `firstFamily` is a GpuFamily value stored as int.

struct LinuxNvmlProbe {
    bool nvmlLoaded;
    bool gpcOffsetRangeReadable;
    unsigned int nvmlDeviceCount;
    char driverVersion[96];
    char nvmlVersion[96];
};

struct LinuxNvapiProbe {
    bool nvapiLoaded;
    bool nvapiInitOk;
    int nvapiGpuCount;
    int firstFamily;       // GpuFamily value
    int vfPointsPopulated; // VF curve points read on the first GPU (0 = none)
};

struct LinuxGpuProbeResult {
    LinuxNvmlProbe nvml;
    LinuxNvapiProbe nvapi;
};

// Probe the NVIDIA driver control surfaces (NVML + NvAPI) on Linux, writing a
// human-readable report to `out` (defaults to stdout when null) and filling
// `result` when non-null.  Returns true when both NVML and NvAPI are reachable.
bool linux_gpu_probe(FILE* out, LinuxGpuProbeResult* result);

// True on an integrated-GPU platform that shares one memory pool with the CPU
// (Tegra/Jetson L4T today; the GB10 / RTX Spark class is the same shape).  Used
// both by the probe report and by the daemon's memory-topology classification,
// so the detection exists exactly once.
bool linux_platform_is_integrated_soc();

#endif // GREEN_CURVE_LINUX_GPU_H
