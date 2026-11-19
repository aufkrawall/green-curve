// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Execute the Windows production verified VF writer (gpu_backend_curve_write.h)
// against a fake control table.  The contract under test: convergence is
// judged against the table read inside the call, never against the cached
// offsets of an earlier snapshot that another tool has since invalidated.
#include "gpu_core.h"
#include "vf_backends.h"
#include "vf_offset_range_policy.h"
#include "vf_curve_units.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace verified_write_fixture {
using ULONGLONG = unsigned long long;
using NvApiFunc = int (*)(void*, void*);
#ifndef ARRAY_COUNT
#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#endif

struct HeapBuffer {
    void* ptr;
    explicit HeapBuffer(size_t size) : ptr(calloc(1, size)) {}
    ~HeapBuffer() { free(ptr); }
    HeapBuffer(const HeapBuffer&) = delete;
    HeapBuffer& operator=(const HeapBuffer&) = delete;
    operator unsigned char*() const { return (unsigned char*)ptr; }
    explicit operator bool() const { return ptr != nullptr; }
};

struct App {
    const VfBackendSpec* vfBackend = &g_vfBackendBlackwell;
    void* gpuHandle = nullptr;
    unsigned char vfMask[32]{};
    VFCurvePoint curve[VF_NUM_POINTS]{};
    int freqOffsets[VF_NUM_POINTS]{};
    gc_i64 freqOffsetsHz[VF_NUM_POINTS]{};
    bool vfOffsetsHzValid = true;
} g_app;

struct Driver {
    int raw[VF_NUM_POINTS]{};
    int gets = 0, batchSets = 0, pointSets = 0, curveReads = 0, rebuilds = 0;
    bool ignoreBatch = false, pointWriteActive = false;
} driver;

static void debug_log(const char*, ...) {}
static void set_last_apply_phase(const char*) {}
static void Sleep(unsigned long) {}
static ULONGLONG GetTickCount64() { return 1; }
static int StringCchPrintfA(char* out, size_t size, const char* format, ...) {
    va_list args; va_start(args, format); vsnprintf(out, size, format, args); va_end(args);
    return 0;
}
static int clamp_freq_delta_khz(int value) { return value; }
static bool nvapi_get_vf_info_cached(unsigned char* mask) {
    memcpy(mask, g_app.vfMask, sizeof(g_app.vfMask));
    return true;
}
static unsigned int delta_offset(int i) {
    const VfBackendSpec& b = *g_app.vfBackend;
    return b.controlEntryBaseOffset + (unsigned int)i * b.controlEntryStride + b.controlEntryDeltaOffset;
}
static int get(void*, void* data) {
    ++driver.gets;
    auto* bytes = (unsigned char*)data;
    for (int i = 0; i < VF_NUM_POINTS; ++i) memcpy(bytes + delta_offset(i), &driver.raw[i], 4);
    return 0;
}
static int set(void*, void* data) {
    const auto* bytes = (const unsigned char*)data;
    const VfBackendSpec& b = *g_app.vfBackend;
    // The per-point path is the ~1 s-per-call cost the fix removes; count it
    // apart from the batch writes.
    if (driver.pointWriteActive) ++driver.pointSets; else ++driver.batchSets;
    if (!driver.pointWriteActive && driver.ignoreBatch) return 0;
    for (int i = 0; i < VF_NUM_POINTS; ++i)
        if (bytes[b.controlMaskOffset + i / 8] & (1u << (i % 8)))
            memcpy(&driver.raw[i], bytes + delta_offset(i), 4);
    return 0;
}
static void* nvapi_qi(unsigned int id) {
    if (id == g_app.vfBackend->getControlId) return (void*)get;
    if (id == g_app.vfBackend->setControlId) return (void*)set;
    return nullptr;
}
static bool nvapi_read_offsets();
static bool nvapi_read_curve() { ++driver.curveReads; return true; }
static void rebuild_visible_map() { ++driver.rebuilds; }
static bool nvapi_set_point(int pointIndex, int freqDelta_kHz) {
    int raw = 0;
    if (!vf_graphics_offset_driver_units(g_app.vfBackend, freqDelta_kHz, &raw)) return false;
    unsigned char buf[0x4000]{};
    const VfBackendSpec& b = *g_app.vfBackend;
    get(nullptr, buf);
    buf[b.controlMaskOffset + pointIndex / 8] = (unsigned char)(1u << (pointIndex % 8));
    memcpy(buf + delta_offset(pointIndex), &raw, 4);
    driver.pointWriteActive = true;
    const bool ok = set(nullptr, buf) == 0;
    driver.pointWriteActive = false;
    return ok;
}

#include "gpu_backend_curve_write.h"

static bool nvapi_read_offsets() {
    HeapBuffer buf(0x4000);
    if (!buf || !nvapi_read_control_table(buf, g_app.vfBackend->controlBufferSize)) return false;
    g_app.vfOffsetsHzValid = vf_control_offsets_decode(g_app.vfBackend, buf,
        g_app.vfBackend->controlBufferSize, g_app.freqOffsets, g_app.freqOffsetsHz);
    return true;
}

constexpr int kPoints = 8;

static void initialize(const VfBackendSpec* backend = &g_vfBackendBlackwell) {
    driver = {};
    g_app = {};
    g_app.vfBackend = backend;
    g_app.gpuHandle = &driver;
    for (int i = 0; i < kPoints; ++i) {
        g_app.vfMask[i / 8] |= (unsigned char)(1u << (i % 8));
        g_app.curve[i] = {1500000u + (unsigned int)i * 15000u, 650000u + (unsigned int)i * 6250u};
    }
}

static void set_raw_khz(int i, int khz) {
    int raw = 0;
    vf_graphics_offset_driver_units(g_app.vfBackend, khz, &raw);
    driver.raw[i] = raw;
}

static int stale_cache_after_external_reset_writes_nothing(const VfBackendSpec* backend, int base) {
    // The incident: the last snapshot saw another tool's flattened curve
    // (+480 MHz on the anchor, -1927 MHz on the tail); that tool then reset the
    // GPU, so the hardware is already at stock when Reset runs.
    initialize(backend);
    g_app.freqOffsets[3] = 480000;
    for (int i = 4; i < kPoints; ++i) g_app.freqOffsets[i] = -1927000;
    for (int i = 3; i < kPoints; ++i) g_app.freqOffsetsHz[i] = (gc_i64)g_app.freqOffsets[i] * 1000;
    int zeros[VF_NUM_POINTS]{};
    bool mask[VF_NUM_POINTS]{};
    for (int i = 0; i < kPoints; ++i) mask[i] = true;
    if (!apply_curve_offsets_verified(zeros, mask, 2, 0)) return base;
    // Before the fix every stale point went through a ~1 s per-point write.
    if (driver.pointSets != 0 || driver.batchSets != 0) return base + 1;
    for (int i = 0; i < kPoints; ++i)
        if (g_app.freqOffsets[i] != 0 || g_app.freqOffsetsHz[i] != 0) return base + 2;
    if (!g_app.vfOffsetsHzValid) return base + 2;
    // The cache moved, so the curve it pairs with is re-read even with no write.
    if (driver.curveReads != 1 || driver.rebuilds != 1) return base + 3;
    return 0;
}

int run_windows_vf_verified_write_tests() {
    if (int failure = stale_cache_after_external_reset_writes_nothing(&g_vfBackendBlackwell, 12350))
        return failure;
    // A backend with a fractional graphics unit decodes through the same path.
    if (int failure = stale_cache_after_external_reset_writes_nothing(&g_vfBackendPascal, 12354))
        return failure;

    {   // Single-point request: no batch pass runs at all, so the entry table
        // is the only readback; a stale cache must not force a write.
        initialize();
        g_app.freqOffsets[2] = 100000;
        int targets[VF_NUM_POINTS]{};
        bool mask[VF_NUM_POINTS]{};
        mask[2] = true;
        if (!apply_curve_offsets_verified(targets, mask, 2, 0) ||
            driver.pointSets != 0 || driver.batchSets != 0 || g_app.freqOffsets[2] != 0) return 12358;
    }
    {   // Inverse: the cache claims stock but another tool raised the tail.
        // The fresh table drives one batch write and everything converges.
        initialize();
        for (int i = 4; i < kPoints; ++i) set_raw_khz(i, -1927000);
        int zeros[VF_NUM_POINTS]{};
        bool mask[VF_NUM_POINTS]{};
        for (int i = 0; i < kPoints; ++i) mask[i] = true;
        if (!apply_curve_offsets_verified(zeros, mask, 2, 0)) return 12359;
        if (driver.batchSets != 1 || driver.pointSets != 0) return 12360;
        for (int i = 0; i < kPoints; ++i) if (driver.raw[i] != 0 || g_app.freqOffsets[i] != 0) return 12361;
    }
    {   // A driver that ignores the batch still gets the per-point fallback,
        // but only for points the fresh readback shows as different.
        initialize();
        set_raw_khz(5, 30000);
        set_raw_khz(6, -15000);
        g_app.freqOffsets[1] = 45000;  // stale, hardware is already at target
        driver.ignoreBatch = true;
        int zeros[VF_NUM_POINTS]{};
        bool mask[VF_NUM_POINTS]{};
        for (int i = 0; i < kPoints; ++i) mask[i] = true;
        if (!apply_curve_offsets_verified(zeros, mask, 2, 0)) return 12362;
        if (driver.pointSets != 2 || driver.raw[5] != 0 || driver.raw[6] != 0) return 12363;
    }
    {   // Coherent cache, hardware at target: nothing written, nothing re-read.
        initialize();
        int zeros[VF_NUM_POINTS]{};
        bool mask[VF_NUM_POINTS]{};
        for (int i = 0; i < kPoints; ++i) mask[i] = true;
        if (!apply_curve_offsets_verified(zeros, mask, 2, 0) ||
            driver.batchSets || driver.pointSets || driver.curveReads || driver.rebuilds) return 12364;
    }
    {   // Pure helpers: the adoption diff counts only targeted points, and a
        // table too small for an entry decodes to an inexact zero.
        int cached[VF_NUM_POINTS]{}, live[VF_NUM_POINTS]{};
        bool mask[VF_NUM_POINTS]{};
        cached[3] = 480000; cached[9] = 1000; mask[3] = mask[4] = true;
        VfControlAdoptionDiff diff = vf_control_adoption_diff(cached, live, mask);
        if (diff.changed != 1 || diff.firstCi != 3 || diff.firstCachedKHz != 480000 ||
            diff.firstLiveKHz != 0) return 12365;
        unsigned char small[16]{};
        int offsets[VF_NUM_POINTS]; gc_i64 hz[VF_NUM_POINTS];
        memset(offsets, 0x5A, sizeof(offsets));
        if (vf_control_offsets_decode(&g_vfBackendBlackwell, small, sizeof(small), offsets, hz) ||
            offsets[0] != 0 || hz[VF_NUM_POINTS - 1] != 0) return 12366;
    }
    return 0;
}
}  // namespace verified_write_fixture

int run_windows_vf_verified_write_tests() {
    return verified_write_fixture::run_windows_vf_verified_write_tests();
}
