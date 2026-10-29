// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Execute the production VF writer and readback with a counted fake driver.

#include "linux_backend.h"
#include "linux_vf_validation.h"
#include "vf_backends.h"
#include "linux_curve_targets.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

static bool nvapi_ok(int status) { return status == 0; }
static const char* nvapi_status_name(int) { return "fixture"; }
static void lb_log(const char*, ...) {}
using nvapi_buf_t = int (*)(void*, void*);
static const unsigned int LB_CONTROL_BUF_SIZE = 0x4000;
#include "linux_backend_vf_read.h"
#include "vf_offset_range_policy.h"
#include "linux_backend_vf_write.h"

namespace {
struct Driver {
    const VfBackendSpec* backend;
    int raw[VF_NUM_POINTS];
    int setCalls;
    bool onePointPerCall;
    bool ignoreWrites;
    bool bufferValid;
};

static int read_control(void* handle, void* buffer) {
    Driver* d = static_cast<Driver*>(handle);
    const VfBackendSpec* b = d->backend;
    auto* bytes = static_cast<unsigned char*>(buffer);
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        const unsigned int offset = b->controlEntryBaseOffset +
            (unsigned int)i * b->controlEntryStride + b->controlEntryDeltaOffset;
        memcpy(bytes + offset, &d->raw[i], sizeof(int));
    }
    return 0;
}

static int set_control(void* handle, void* buffer) {
    Driver* d = static_cast<Driver*>(handle);
    const VfBackendSpec* b = d->backend;
    const auto* bytes = static_cast<const unsigned char*>(buffer);
    ++d->setCalls;
    bool wrote = false;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        const unsigned int offset = b->controlEntryBaseOffset +
            (unsigned int)i * b->controlEntryStride + b->controlEntryDeltaOffset;
        int value = 0;
        memcpy(&value, bytes + offset, sizeof(value));
        if ((bytes[b->controlMaskOffset + i / 8] & (1u << (i % 8))) == 0) {
            // Unselected entries stay in exact driver units, even if odd.
            d->bufferValid &= value == d->raw[i];
            continue;
        }
        if (d->ignoreWrites || (d->onePointPerCall && wrote)) continue;
        d->raw[i] = value;
        wrote = true;
    }
    return 0;
}

static int read_info(void*, void* buffer) {
    auto* bytes = static_cast<unsigned char*>(buffer);
    memset(bytes + g_vfBackendPascal.infoMaskOffset, 0, 32);
    bytes[g_vfBackendPascal.infoMaskOffset] = 0xff;
    return 0;
}

static int read_status(void* handle, void* buffer) {
    const Driver* d = static_cast<Driver*>(handle);
    const VfBackendSpec* b = d->backend;
    auto* bytes = static_cast<unsigned char*>(buffer);
    for (unsigned int i = 0; i < 8; ++i) {
        const unsigned int offset = b->statusEntriesOffset + i * b->statusEntryStride;
        const unsigned int frequency = (unsigned int)(
            (b->family == GPU_FAMILY_PASCAL ? 3000000 : 1500000) + d->raw[i]);
        const unsigned int voltage = 800000u + i * 10000u;
        memcpy(bytes + offset, &frequency, sizeof(frequency));
        memcpy(bytes + offset + 4, &voltage, sizeof(voltage));
    }
    return 0;
}

static void* query(unsigned int id) {
    if (id == g_vfBackendPascal.getInfoId)
        return reinterpret_cast<void*>(read_info);
    if (id == g_vfBackendPascal.getStatusId)
        return reinterpret_cast<void*>(read_status);
    if (id == g_vfBackendPascal.getControlId)
        return reinterpret_cast<void*>(read_control);
    if (id == g_vfBackendPascal.setControlId)
        return reinterpret_cast<void*>(set_control);
    return nullptr;
}

static void initialize(Driver* d, LinuxGpuState* g, const VfBackendSpec* backend) {
    *d = {};
    *g = {};
    d->backend = backend;
    d->bufferValid = true;
    g->backend = backend;
    g->gpuHandle = d;
    g->nvapiQi = query;
    g->vfInfoFresh = true;
    for (int i = 0; i < 8; ++i) {
        g->vfMask[i / 8] |= (unsigned char)(1u << (i % 8));
        g->curve[i] = {1500000u, 800000u + (unsigned int)i * 10000u};
    }
    d->raw[7] = 30001; // an untouched fractional-kHz offset
}
} // namespace

int run_linux_vf_write_tests() {
    Driver d = {};
    LinuxGpuState g = {};
    int targets[VF_NUM_POINTS] = {};
    bool mask[VF_NUM_POINTS] = {};
    mask[2] = mask[3] = true;
    targets[2] = 125000;
    targets[3] = -125000;
    initialize(&d, &g, &g_vfBackendPascal);
    if (!apply_curve_offsets_verified(&g, targets, mask, 2) ||
        d.raw[2] != 250000 || d.raw[3] != -250000 ||
        g.freqOffsets[2] != 125000 || g.freqOffsets[3] != -125000 ||
        !d.bufferValid || d.raw[7] != 30001 || d.setCalls != 1) return 7718;
    // Comparing against the raw preimage must recognize a physical no-op.
    if (!apply_curve_offsets_verified(&g, targets, mask, 2) ||
        d.setCalls != 1) return 7719;
    targets[2] = targets[3] = 0;
    if (!apply_curve_offsets_verified(&g, targets, mask, 2) ||
        d.raw[2] || d.raw[3] || g.freqOffsets[2] || g.freqOffsets[3] ||
        d.setCalls != 2 || !d.bufferValid) return 7720;

    // A partial batch refreshes the raw preimage before its next pass.
    initialize(&d, &g, &g_vfBackendPascal);
    d.onePointPerCall = true;
    targets[2] = 125000;
    targets[3] = -125000;
    if (!apply_curve_offsets_verified(&g, targets, mask, 2) ||
        d.setCalls != 2 || d.raw[2] != 250000 || d.raw[3] != -250000 ||
        !d.bufferValid || d.raw[7] != 30001) return 7721;

    initialize(&d, &g, &g_vfBackendPascal);
    d.ignoreWrites = true;
    if (apply_curve_offsets_verified(&g, targets, mask, 2) ||
        d.setCalls != 2 || g.freqOffsets[2] || !d.bufferValid) return 7722;

    // Every other known backend and the future fallback retain plain kHz.
    for (const VfBackendSpec* b : {&g_vfBackendTuring, &g_vfBackendAmpere,
            &g_vfBackendLovelace, &g_vfBackendBlackwell, &g_vfBackendFuture}) {
        initialize(&d, &g, b);
        if (!apply_curve_offsets_verified(&g, targets, mask, 2) ||
            d.raw[2] != 125000 || d.raw[3] != -125000 ||
            !b->readSupported || !b->writeSupported || !d.bufferValid)
            return 7723;
    }
    int raw = 123;
    if (vf_graphics_offset_driver_units(&g_vfBackendPascal, INT_MAX, &raw) ||
        vf_graphics_offset_driver_units(&g_vfBackendPascal, INT_MIN, &raw) ||
        raw != 123 || vf_graphics_offset_driver_units(&g_vfBackendPascal, 0, nullptr))
        return 7724;
    for (int value : {-1000000, -125000, 0, 125000, 1000000}) {
        if (!vf_graphics_offset_driver_units(&g_vfBackendPascal, value, &raw) ||
            raw != value * 2 || vf_graphics_offset_khz(&g_vfBackendPascal, raw) != value)
            return 7725;
    }
    // An applied curve's status and offsets agree in the same physical units.
    initialize(&d, &g, &g_vfBackendPascal);
    char why[100] = {};
    if (!apply_curve_offsets_verified(&g, targets, mask, 2) ||
        !nvapi_read_vf_snapshot(&g, why, sizeof(why)) ||
        g.curve[2].freq_kHz != 1625000u || g.freqOffsets[2] != 125000 ||
        g.curve[3].freq_kHz != 1375000u || g.freqOffsets[3] != -125000 ||
        g.curve[2].freq_kHz - (unsigned int)g.freqOffsets[2] != 1500000u)
        return 7726;
    DesiredSettings desired = {};
    desired.hasCurvePoint[2] = true;
    desired.curvePointMHz[2] = 1700;
    const LinuxCurveTargetBuildResult plan = linux_build_curve_targets(
        g.curve, g.freqOffsets, &desired, -1000000, targets, mask);
    if (plan.pointCount != 1 || targets[2] != 200000 || !mask[2] || mask[3] ||
        !apply_curve_offsets_verified(&g, targets, mask, 2) ||
        !nvapi_read_vf_snapshot(&g, why, sizeof(why)) ||
        d.raw[2] != 400000 || g.curve[2].freq_kHz != 1700000u ||
        g.freqOffsets[2] != 200000 || d.raw[3] != -250000 || !d.bufferValid)
        return 7728;
    return 0;
}

#ifdef GC_VF_WRITE_TEST_MAIN
int main() {
    const int failure = run_linux_vf_write_tests();
    printf("Linux VF write regression: %d\n", failure);
    return failure;
}
#endif
