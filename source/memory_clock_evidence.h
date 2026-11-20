// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_MEMORY_CLOCK_EVIDENCE_H
#define GREEN_CURVE_MEMORY_CLOCK_EVIDENCE_H

// The memory clock offset in force, read from the running clock: the highest
// Pstates20 memory clock minus the board's maximum memory clock
// (nvmlDeviceGetMaxClockInfo). The Windows live state has derived the memory
// offset this way for a long time (detect_clock_offsets()); passive capture
// uses the same rule through memory_clock_read().
//
// Why the NVML offset getter is not enough on its own: an offset another tool
// programmed through Pstates20 raises the running memory clock while both NVML
// getter surfaces (P0 and global) keep reading 0. Observed 2026-11-19 on an
// RTX 5070: running maximum 17001 MHz against a 14001 MHz board maximum
// (+3000 MHz), getter 0, so Import current GPU settings reported memory as
// stock. An NVML write replaces that offset (writing 0 brought the running
// maximum back to 14001 MHz), so replaying the running offset through the
// NVML setter reproduces it.
//
// Units: both maxima are display MHz (half NVML's "effective" offset unit), so
// the difference is the display/physical offset the editor shows.

#include <memory>
#include "gpu_core.h"

enum : unsigned int { NVAPI_GPU_GET_PSTATES20_ID = 0x6FF81213u };

// Highest memory clock any pstate reports, in whole MHz (kHz truncated, as the
// live derivation always has); 0 when the reading names no memory clock.
static inline unsigned int pstates20_memory_max_mhz(const nvapiPerfPstates20Info_t& info) {
    unsigned int numPstates = info.numPstates;
    if (numPstates > NVAPI_MAX_GPU_PSTATE20_PSTATES) numPstates = NVAPI_MAX_GPU_PSTATE20_PSTATES;
    unsigned int numClocks = info.numClocks;
    if (numClocks > NVAPI_MAX_GPU_PSTATE20_CLOCKS) numClocks = NVAPI_MAX_GPU_PSTATE20_CLOCKS;
    unsigned int best = 0;
    for (unsigned int pi = 0; pi < numPstates; ++pi) {
        for (unsigned int ci = 0; ci < numClocks; ++ci) {
            const nvapiPstate20ClockEntry_t& clock = info.pstates[pi].clocks[ci];
            if (clock.domainId != NVAPI_GPU_PUBLIC_CLOCK_MEMORY) continue;
            unsigned int maxKHz = 0;
            if (clock.typeId == NVAPI_GPU_PERF_PSTATE20_CLOCK_TYPE_SINGLE) maxKHz = clock.data.single.freq_kHz;
            else if (clock.typeId == NVAPI_GPU_PERF_PSTATE20_CLOCK_TYPE_RANGE) maxKHz = clock.data.range.maxFreq_kHz;
            const unsigned int mhz = maxKHz / 1000;
            if (mhz > best) best = mhz;
        }
    }
    return best;
}

// The offset in physical kHz, or false when either maximum is unknown.
static inline bool memory_clock_effective_offset_khz(unsigned int runningMaxMHz, unsigned int boardMaxMHz,
                                                     int* outKHz) {
    if (!outKHz || !runningMaxMHz || !boardMaxMHz) return false;
    const long long khz = ((long long)runningMaxMHz - (long long)boardMaxMHz) * 1000;
    if (khz < -2000000000LL || khz > 2000000000LL) return false;
    *outKHz = (int)khz;
    return true;
}

// Running maxima are in whole MHz. A fresh getter resolves the fraction
// when both readings lie within one step of that measurement.
static inline bool memory_clock_getter_agrees(gc_i64 getterHz, gc_i64 runningHz) {
    return getterHz > runningHz - 1000000 && getterHz < runningHz + 1000000;
}

static inline int memory_clock_resolve_live_khz(int runningKHz, bool getterValid, int getterKHz) {
    return getterValid && memory_clock_getter_agrees((gc_i64)getterKHz * 1000, (gc_i64)runningKHz * 1000)
        ? getterKHz : runningKHz;
}

struct MemoryClockReading {
    bool pstatesValid = false, boardValid = false;
    int pstatesStatus = -100001;   // NvAPI status; -100001: getter unavailable
    int boardStatus = NVML_ERROR_NOT_SUPPORTED;
    unsigned int runningMaxMHz = 0, boardMaxMHz = 0;
};

static inline bool memory_clock_reading_offset_hz(const MemoryClockReading& r, gc_i64* outHz) {
    int khz = 0;
    if (!outHz || !r.pstatesValid || !r.boardValid ||
        !memory_clock_effective_offset_khz(r.runningMaxMHz, r.boardMaxMHz, &khz)) return false;
    *outHz = (gc_i64)khz * 1000;
    return true;
}

// Getters only: Pstates20 through `query` and NVML's board maximum. Nothing is
// initialized, written or refreshed.
template <class Query>
static inline MemoryClockReading memory_clock_read(Query query, void* handle, const NvmlApi* nvml,
                                                   nvmlDevice_t device) {
    using Function = int (*)(void*, void*);
    MemoryClockReading r;
    auto getPstates = query && handle ? (Function)query(NVAPI_GPU_GET_PSTATES20_ID) : nullptr;
    if (getPstates) {
        auto info = std::make_unique<nvapiPerfPstates20Info_t>();
        info->version = NVAPI_PERF_PSTATES20_INFO_VER3;
        r.pstatesStatus = getPstates(handle, info.get());
        if (r.pstatesStatus != 0) {
            *info = {};
            info->version = NVAPI_PERF_PSTATES20_INFO_VER2;
            r.pstatesStatus = getPstates(handle, info.get());
        }
        if (r.pstatesStatus == 0) {
            r.runningMaxMHz = pstates20_memory_max_mhz(*info);
            r.pstatesValid = r.runningMaxMHz > 0;
        }
    }
    if (nvml && device && nvml->getMaxClock) {
        unsigned int mhz = 0;
        r.boardStatus = nvml->getMaxClock(device, NVML_CLOCK_MEM, &mhz);
        r.boardValid = r.boardStatus == NVML_SUCCESS && mhz > 0;
        if (r.boardValid) r.boardMaxMHz = mhz;
    }
    return r;
}

#endif
