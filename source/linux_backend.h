// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_backend.h — native Linux GPU control backend.
//
// Drives the NVIDIA driver on Linux (NvAPI via libnvidia-api.so.1 + NVML via
// libnvidia-ml.so.1) to read and apply the voltage-frequency curve, clock/mem
// offsets, power limit, locked clocks and fan control.  It reuses the shared
// data model (gpu_core.h) and the shared per-family layout tables
// (vf_backends.h), and faithfully ports the apply/correction algorithm from the
// Windows backend (gpu_backend.cpp / gpu_backend_apply.cpp / main_runtime_gpu.cpp
// / main_runtime_nvml.cpp) — each ported function names its Windows counterpart
// so the two stay in sync.  Self-contained: holds its own LinuxGpuState instead
// of the Win32-coupled AppData, so it compiles and links without the GUI/service
// glue.

#ifndef GREEN_CURVE_LINUX_BACKEND_H
#define GREEN_CURVE_LINUX_BACKEND_H

#include "gpu_core.h"
#include "platform.h"

struct LinuxGpuState {
    // NvAPI
    PlLib nvapiLib;
    void* (*nvapiQi)(unsigned int);
    GPU_HANDLE gpuHandle;

    // NVML
    PlLib nvmlLib;
    NvmlApi nvml;
    nvmlDevice_t nvmlDevice;
    bool nvmlReady;

    // Identity / capability
    unsigned int architecture;
    GpuFamily family;
    const VfBackendSpec* backend;
    char gpuName[96];
    unsigned int nvapiIndex;
    unsigned int nvmlIndex;

    // VF curve state
    VFCurvePoint curve[VF_NUM_POINTS];
    int freqOffsets[VF_NUM_POINTS];
    unsigned char vfMask[32];
    unsigned int vfNumClocks;
    bool vfInfoCached;
    int numPopulated;

    // Offset ranges (kHz for curve; MHz for clock domains)
    int curveOffsetMinKHz;
    int curveOffsetMaxKHz;
    bool curveOffsetRangeKnown;
    int gpuOffsetMinMHz, gpuOffsetMaxMHz;
    int memOffsetMinMHz, memOffsetMaxMHz;
    int offsetReadPstate;

    // Power
    int powerLimitMinmW, powerLimitMaxmW, powerLimitDefaultmW, powerLimitCurrentmW;
};

// Load the driver libraries and initialise the selected GPU (ordinal `index`).
// On success the curve, masks and ranges are read.  Returns false + message on
// failure (no driver, no GPU, etc.).
bool linux_backend_init(LinuxGpuState* g, unsigned int index, char* err, size_t errSize);
void linux_backend_shutdown(LinuxGpuState* g);

// Refresh the live curve + offsets + ranges from the driver.
bool linux_backend_refresh(LinuxGpuState* g);

// Sanity-check the VF curve read so a struct-layout / ABI mismatch on a new
// driver/arch can't be mistaken for real data (and can't drive garbage writes).
// Returns false + a reason when the read looks implausible.
bool linux_backend_curve_plausible(const LinuxGpuState* g, char* why, size_t whySize);

// Read-only validation that exercises every NVAPI/NVML struct used by apply —
// including the CONTROL struct that writes use (read via getControl, without
// writing) — and the curve plausibility check.  Safe to run on any GPU; it does
// not modify GPU state.  Intended as an arm64/driver pre-flight.  Writes a report
// to `out` (stdout when null) and returns true when the apply path should work.
bool linux_backend_self_test(LinuxGpuState* g, FILE* out);

// Apply a desired settings request (validated by the caller).  Mirrors the
// Windows apply_desired_settings_service() ordering: reset baseline (optional),
// GPU clock offset, memory clock offset, power limit, VF curve, locked clocks,
// fan.  Writes a human-readable summary to `result`.  Returns true if every
// requested phase succeeded.
bool linux_backend_apply(LinuxGpuState* g, const DesiredSettings* d,
                         char* result, size_t resultSize);

// Reset OC/UV to driver defaults (curve offsets 0, clock offsets 0, power
// default, locked clocks released, fan auto).
bool linux_backend_reset(LinuxGpuState* g, char* result, size_t resultSize);

#endif // GREEN_CURVE_LINUX_BACKEND_H
