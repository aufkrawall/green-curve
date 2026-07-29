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
#include "gpu_capability_policy.h"
#include "linux_architecture_policy.h"
#include "linux_gpu_binding_policy.h"
#include "linux_transaction.h"
#include "platform.h"

struct LinuxGpuState {
    // NvAPI
    PlLib nvapiLib;
    void* (*nvapiQi)(unsigned int);
    GPU_HANDLE gpuHandle;
    bool nvapiInitialized;

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
    unsigned int adapterCount;
    unsigned int selectedAdapterIndex;
    GpuAdapterInfo adapters[MAX_GPU_ADAPTERS];
    GpuAdapterInfo selectedGpu;
    bool writeIdentityResolved;
    LinuxGpuMatchMethod nvapiMatchMethod;
    unsigned int architectureSource;
    unsigned int cachedArchitecture;
    GpuFamily cachedFamily;
    GpuAdapterInfo cachedArchitectureGpu;
    bool cachedArchitectureValid;

    // VF curve state
    VFCurvePoint curve[VF_NUM_POINTS];
    int freqOffsets[VF_NUM_POINTS];
    unsigned char vfMask[32];
    unsigned int vfNumClocks;
    bool vfInfoCached;
    int numPopulated;
    bool vfInfoFresh;
    bool vfStatusFresh;
    bool vfControlFresh;
    bool vfStructureValid;
    bool vfSnapshotFresh;
    int vfInfoStatus;
    int vfStatusStatus;
    int vfControlStatus;
    ServiceGpuHealth health;
    ServiceGpuHealth lastLoggedHealth;
    bool healthLogged;
    // Per-domain control surface, mirroring the Windows probe so both platforms
    // report the same thing.  DERIVED from health.availableMutationDomains and
    // the same per-domain read validity that produced it — never re-probed — so
    // the two representations cannot disagree.  See gpu_capability_policy.h.
    GpuCapabilityProbe capability;
    // The status table's graphics-domain boundary is structural driver state,
    // not per-poll telemetry. Log it on first observation and on a boundary
    // transition only, so a 1 Hz refresh cannot flood the journal.
    int lastLoggedGraphicsDomainEnd;
    bool graphicsDomainBoundaryLogged;

    // Offset ranges (kHz for curve; MHz for clock domains)
    int curveOffsetMinKHz;
    int curveOffsetMaxKHz;
    bool curveOffsetRangeKnown;
    int gpuOffsetMinMHz, gpuOffsetMaxMHz;
    int memOffsetMinMHz, memOffsetMaxMHz;
    int offsetReadPstate;

    // Power
    int powerLimitMinmW, powerLimitMaxmW, powerLimitDefaultmW, powerLimitCurrentmW;

    // Fan duty range the driver will actually honor (nvmlDeviceGetMinMaxFanSpeed).
    // A write below the minimum is silently clamped by the driver, so the range
    // has to be known before a manual duty can be verified — and before the UI
    // can advertise a percentage it cannot deliver.
    int fanMinPct, fanMaxPct;
    bool fanRangeKnown;
    // Curve mode re-asserts the duty every poll interval, so the manual-write
    // outcome is logged only on a transition (F-15-011).  A line per poll would
    // push several thousand identical entries an hour into the journal and bury
    // the failures that matter.
    int lastLoggedFanPercent;
    bool lastLoggedFanOk;
    bool fanWriteLogged;
};

struct LinuxHardwareSnapshot {
    bool valid;
    gc_u32 availableMutationDomains;
    bool gpuOffsetValid;
    bool memOffsetValid;
    bool powerValid;
    bool curveValid;
    bool fanValid;
    int gpuOffsetMHz;
    int memOffsetMHz;
    unsigned int powerLimitmW;
    int curveOffsets[VF_NUM_POINTS];
    bool curveMask[VF_NUM_POINTS];
    unsigned int fanCount;
    unsigned int fanPolicy[MAX_GPU_FANS];
    // The *intended* duty per fan, not the measured one.  Rollback has to
    // restore what the driver was told to hold; restoring a measured 0% from a
    // stopped fan would strand the GPU at a duty nobody asked for.
    unsigned int fanTargetPercent[MAX_GPU_FANS];
    bool fanTargetKnown[MAX_GPU_FANS];
};

struct LinuxBackendRefreshResult {
    bool nvmlReady;
    bool vfFresh;
    ServiceGpuHealth health;
};

// Load the driver libraries and initialise the selected GPU (ordinal `index`).
// On success the curve, masks and ranges are read.  Returns false + message on
// failure (no driver, no GPU, etc.).
bool linux_backend_init(LinuxGpuState* g, const GpuAdapterInfo* target,
                        char* err, size_t errSize);
void linux_backend_shutdown(LinuxGpuState* g);

bool linux_backend_select_target(LinuxGpuState* g, const GpuAdapterInfo* target,
                                 char* err, size_t errSize);
bool linux_backend_capture_snapshot(LinuxGpuState* g, LinuxHardwareSnapshot* snapshot,
                                    char* err, size_t errSize);
bool linux_backend_restore_snapshot(LinuxGpuState* g, const LinuxHardwareSnapshot* snapshot,
                                    unsigned int phaseMask, char* err, size_t errSize);

// Refresh the live curve + offsets + ranges from the driver.
LinuxBackendRefreshResult linux_backend_refresh(LinuxGpuState* g);

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
// fan.  For a VF-domain replacement, previousIntent and committedIntent let
// the transaction release points that are no longer owned.  Startup replay
// passes no previous intent.  Writes a human-readable summary to `result`.
LinuxMutationResult linux_backend_apply(LinuxGpuState* g, const DesiredSettings* d,
                                        const DesiredSettings* previousIntent,
                                        const DesiredSettings* committedIntent,
                                        char* result, size_t resultSize);

// Reset OC/UV to driver defaults (curve offsets 0, clock offsets 0, power
// default, locked clocks released, fan auto).
LinuxMutationResult linux_backend_reset(LinuxGpuState* g, char* result, size_t resultSize);
bool linux_backend_set_curve_fan_percent(LinuxGpuState* g, unsigned int percent);
bool linux_backend_set_fan_auto(LinuxGpuState* g);

#endif // GREEN_CURVE_LINUX_BACKEND_H
