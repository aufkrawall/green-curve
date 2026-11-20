// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Execute the production VF writer and readback with a counted fake driver.

#include "linux_backend.h"
#include "linux_vf_validation.h"
#include "vf_backends.h"
#include "linux_curve_targets.h"
#include "service_scoped_apply_policy.h"
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
#include "linux_backend_curve_correction.h"
#include "vf_exact_control_write.h"
static bool nvml_set_clock_offset(LinuxGpuState*, unsigned int, int, gc_u32);
#include "linux_memory_curve_preservation.h"

namespace {
struct Driver {
    const VfBackendSpec* backend;
    int raw[VF_NUM_POINTS];
    int setCalls;
    bool onePointPerCall;
    bool ignoreWrites;
    bool bufferValid;
    bool afterMemory, moveTopology, memoryFails;
    int memoryWrites, memoryEffective;
    gc_u32 memorySurface;
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
        const unsigned int voltage = 800000u + i * 10000u + (d->afterMemory && d->moveTopology ? 6250u : 0u);
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

static bool nvml_set_clock_offset(LinuxGpuState* g, unsigned int domain, int value, gc_u32 surface) {
    auto& d = *(Driver*)g->gpuHandle;
    if (domain != NVML_CLOCK_MEM) return false;
    ++d.memoryWrites; d.memoryEffective = value; d.memorySurface = surface; d.afterMemory = true;
    memset(d.raw, 0, sizeof(d.raw)); // A memory setter can erase programmed VF controls.
    return !d.memoryFails;
}

static int run_linux_memory_curve_tests() {
    Driver d{}; LinuxGpuState g{}; LinuxHardwareSnapshot snapshot{};
    DesiredSettings request{}; request.hasMemOffset = 1; request.memOffsetMHz = 1;
    char why[192]{};
    const auto prepare = [&]() {
        initialize(&d, &g, &g_vfBackendPascal); d.raw[0] = 1; d.raw[1] = -30001;
        if (!nvapi_read_vf_snapshot(&g, why, sizeof(why))) return false;
        snapshot = {}; snapshot.valid = snapshot.curveValid = snapshot.curveOffsetsExact = true;
        for (int i = 0; i < VF_NUM_POINTS; ++i) {
            snapshot.curveMask[i] = g.curve[i].freq_kHz != 0;
            snapshot.curveOffsets[i] = g.freqOffsets[i]; snapshot.curveOffsetsHz[i] = g.freqOffsetsHz[i];
        }
        return true;
    };
    if (!prepare() || !linux_apply_memory_preserving_curve(&g, &request, &snapshot, 0) ||
        d.raw[0] != 1 || d.raw[1] != -30001 || d.raw[7] != 30001 || d.memoryWrites != 1 ||
        d.memoryEffective != 2 || d.setCalls != 1 || !d.bufferValid || !g.vfOffsetsHzValid ||
        g.freqOffsetsHz[0] != 500 || request.curvePointCaptured[0] || request.hasCurvePoint[0]) return 8610;
    if (!prepare()) return 8611;
    snapshot.curveOffsetsExact = false;
    if (linux_apply_memory_preserving_curve(&g, &request, &snapshot, 0) || d.memoryWrites || d.setCalls) return 8612;
    if (!prepare()) return 8611;
    snapshot.curveOffsetsHz[0] = 1000000500;
    if (linux_apply_memory_preserving_curve(&g, &request, &snapshot, 0) || d.memoryWrites || d.setCalls) return 8613;
    if (!prepare()) return 8611;
    g.vfMask[0] &= (unsigned char)~1u;
    if (linux_apply_memory_preserving_curve(&g, &request, &snapshot, 0) || d.memoryWrites || d.setCalls) return 8614;
    if (!prepare()) return 8611;
    d.moveTopology = true;
    if (linux_apply_memory_preserving_curve(&g, &request, &snapshot, 0) || d.memoryWrites != 1 || d.setCalls) return 8615;
    if (!prepare()) return 8611;
    d.raw[0] = 2000001; // Changed since the transaction snapshot: recheck before memory.
    if (linux_apply_memory_preserving_curve(&g, &request, &snapshot, 0) || d.memoryWrites || d.setCalls) return 8616;
    if (!prepare()) return 8611;
    d.ignoreWrites = true;
    if (linux_apply_memory_preserving_curve(&g, &request, &snapshot, 0) || d.memoryWrites != 1 || d.setCalls != 4) return 8617;
    if (!prepare()) return 8611;
    request.memOffsetPrecise = 1; request.memOffsetHz = -500000; request.memOffsetMHz = 0;
    request.memOffsetSurface = snapshot.memOffsetSurface = CLOCK_OFFSET_SURFACE_GLOBAL;
    if (!linux_apply_memory_preserving_curve(&g, &request, &snapshot, 0) || d.memoryEffective != -1 ||
        d.memorySurface != CLOCK_OFFSET_SURFACE_GLOBAL || d.raw[0] != 1) return 8618;
    if (!prepare()) return 8611;
    snapshot.curveValid = false; g.backend = nullptr;
    if (!linux_apply_memory_preserving_curve(&g, &request, &snapshot, 0) || d.memoryWrites != 1 || d.setCalls) return 8619;
    if (!prepare()) return 8611;
    d.memoryFails = true;
    if (linux_apply_memory_preserving_curve(&g, &request, &snapshot, 0) || d.memoryWrites != 1 || d.setCalls) return 8620;
    return 0;
}

static int run_single_batch_cases() {
    Driver d{}; LinuxGpuState g{};
    int targets[VF_NUM_POINTS]{}; bool mask[VF_NUM_POINTS]{}, exact[VF_NUM_POINTS]{};
    gc_i64 hz[VF_NUM_POINTS]{}; hz[0] = 500; hz[1] = -500;
    for (int i = 0; i < 3; ++i) mask[i] = exact[i] = true;
    initialize(&d, &g, &g_vfBackendPascal); d.raw[2] = 2; d.onePointPerCall = true;
    if (apply_curve_offsets_verified(&g, targets, mask, 25, hz, exact, VF_WRITE_SINGLE_BATCH) ||
        d.setCalls != 1 || d.raw[0] != 1 || d.raw[1] || d.raw[2] != 2 || !d.bufferValid) return 8877;
    initialize(&d, &g, &g_vfBackendPascal); d.raw[2] = 2;
    if (!apply_curve_offsets_verified(&g, targets, mask, 25, hz, exact, VF_WRITE_SINGLE_BATCH) ||
        d.setCalls != 1 || d.raw[0] != 1 || d.raw[1] != -1 || d.raw[2] || !d.bufferValid) return 8878;
    initialize(&d, &g, &g_vfBackendFuture); hz[0] = 1000; hz[1] = -1000;
    if (!apply_curve_offsets_verified(&g, targets, mask, 25, hz, exact, VF_WRITE_SINGLE_BATCH) ||
        d.setCalls != 1 || d.raw[0] != 1 || d.raw[1] != -1) return 8879;
    initialize(&d, &g, &g_vfBackendPascal); g.vfMask[0] &= (unsigned char)~2u;
    if (apply_curve_offsets_verified(&g, targets, mask, 25, hz, exact, VF_WRITE_SINGLE_BATCH) || d.setCalls) return 8880;
    initialize(&d, &g, &g_vfBackendPascal); g.curve[1] = {};
    if (apply_curve_offsets_verified(&g, targets, mask, 25, hz, exact) || d.setCalls) return 8881;
    initialize(&d, &g, &g_vfBackendPascal);
    if (apply_curve_offsets_verified(&g, targets, mask, 25, hz, exact, (VfControlWritePolicy)19) || d.setCalls) return 8882;
    initialize(&d, &g, &g_vfBackendPascal); d.onePointPerCall = true;
    struct IO {
        Driver* d;
        bool read(unsigned char* bytes, size_t) { return read_control(d, bytes) == 0; }
        bool write(unsigned char* bytes, size_t) { return set_control(d, bytes) == 0; }
        bool mayWrite() { return true; }
    } io{&d};
    hz[0] = 500; hz[1] = -500;
    const auto result = vf_exact_control_write(g.backend, g.vfMask, mask, hz, 25, io, VF_WRITE_SINGLE_BATCH);
    if (result.status != VF_EXACT_MISMATCH || !result.strategyRestricted || result.writes != 1 ||
        result.batchWrites != 1 || result.pointWrites || result.firstCi != 1 || result.targetHz != -500) return 8883;
    return 0;
}

int run_linux_vf_write_tests() {
    if (int failure = run_single_batch_cases()) return failure;
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

    // An out-of-range target is refused outright, never silently clamped
    // into range (contract 12): the shared planner already proved
    // representability before any setter could run, so a write-time
    // mismatch must surface, not be hidden by a second silent clamp.
    initialize(&d, &g, &g_vfBackendPascal);
    targets[2] = 2000000; // 2000 MHz, outside the +-1000 MHz fallback range
    mask[2] = true; mask[3] = false;
    if (apply_curve_offsets_verified(&g, targets, mask, 2) || d.setCalls != 0)
        return 7940;

    // linux_build_curve_targets tags each written point with whether its
    // intent is a frequency (verify absolute MHz) or an offset (verify via
    // apply_curve_offsets_verified's own convergence) -- CT-02 parity with
    // Windows gpu_backend_apply_verify.h / curve_point_offset_policy.h.
    initialize(&d, &g, &g_vfBackendPascal);
    DesiredSettings explicitAbs = {};
    explicitAbs.hasCurvePoint[2] = true;
    explicitAbs.curvePointMHz[2] = 1700;
    LinuxCurveTargetBuildResult r = linux_build_curve_targets(
        g.curve, g.freqOffsets, &explicitAbs, -1000000, targets, mask);
    if (!r.pointAbsoluteIntent[2] || r.pointTargetMHz[2] != 1700) return 7941;

    DesiredSettings explicitOffset = {};
    explicitOffset.hasCurvePoint[2] = true;
    explicitOffset.curvePointMHz[2] = 1700;
    explicitOffset.curvePointFromGpuOffset[2] = true;
    r = linux_build_curve_targets(g.curve, g.freqOffsets, &explicitOffset,
        -1000000, targets, mask);
    if (r.pointAbsoluteIntent[2]) return 7942;

    DesiredSettings flatten = {};
    flatten.hasLock = true;
    flatten.lockMode = LOCK_MODE_FLATTEN;
    flatten.lockMHz = 1600;
    flatten.lockCi = 2;
    r = linux_build_curve_targets(g.curve, g.freqOffsets, &flatten,
        -1000000, targets, mask);
    if (!r.pointAbsoluteIntent[2] || r.pointTargetMHz[2] != 1600 ||
        !r.pointAbsoluteIntent[3] || r.pointTargetMHz[3] != 1600 ||
        !r.pointAbsoluteIntent[7] || r.pointTargetMHz[7] != 1600)
        return 7943;

    DesiredSettings hard = {};
    hard.hasLock = true;
    hard.lockMode = LOCK_MODE_HARD;
    hard.lockMHz = 1600;
    hard.lockCi = 2;
    r = linux_build_curve_targets(g.curve, g.freqOffsets, &hard,
        -1000000, targets, mask);
    if (r.pointAbsoluteIntent[2] || r.pointAbsoluteIntent[3] ||
        r.pointAbsoluteIntent[7] || !r.pointMask[2] || !r.pointMask[7])
        return 7944;

    // linux_verify_curve_absolute_targets: the pure post-write check shared
    // across every absolute-intent point built above.
    LinuxCurveTargetBuildResult build = {};
    build.pointMask[2] = true;
    build.pointAbsoluteIntent[2] = true;
    build.pointTargetMHz[2] = 1600;
    VFCurvePoint fresh[VF_NUM_POINTS] = {};
    fresh[2] = {1604000u, 800000u}; // within the quantization-aware tolerance
    char detail[160] = {};
    if (!linux_verify_curve_absolute_targets(fresh, build, detail, sizeof(detail)))
        return 7945;
    fresh[2].freq_kHz = 1700000u; // far outside tolerance
    if (linux_verify_curve_absolute_targets(fresh, build, detail, sizeof(detail)) ||
        !detail[0])
        return 7946;
    fresh[2].freq_kHz = 0; // masked absolute point lost its readback
    if (linux_verify_curve_absolute_targets(fresh, build, detail, sizeof(detail)))
        return 7947;
    build.pointAbsoluteIntent[2] = false; // offset-intent points are ignored
    if (!linux_verify_curve_absolute_targets(fresh, build, detail, sizeof(detail)))
        return 7948;

    // Exact driver controls retain both signs of the half-kHz remainder.
    initialize(&d, &g, &g_vfBackendPascal);
    gc_i64 exact[VF_NUM_POINTS]{};
    memset(mask, 0, sizeof(mask));
    mask[2] = mask[3] = true;
    exact[2] = 15000500; exact[3] = -15000500;
    targets[2] = 15000; targets[3] = -15000;
    if (!apply_curve_offsets_verified(&g, targets, mask, 2, exact, mask) ||
        d.raw[2] != 30001 || d.raw[3] != -30001 || !g.vfOffsetsHzValid ||
        g.freqOffsetsHz[2] != exact[2] || g.freqOffsetsHz[3] != exact[3] ||
        !d.bufferValid || d.raw[7] != 30001) return 8090;
    if (!apply_curve_offsets_verified(&g, targets, mask, 2, exact, mask) ||
        d.setCalls != 1) return 8091;
    if (!nvapi_read_vf_snapshot(&g, why, sizeof(why)) ||
        !g.vfOffsetsHzValid || g.freqOffsetsHz[7] != 15000500) return 8092;
    // A zero-kHz projection must not hide a nonzero raw half step at Reset.
    d.raw[2] = 1; d.raw[3] = -1;
    exact[2] = exact[3] = 0; targets[2] = targets[3] = 0;
    if (!apply_curve_offsets_verified(&g, targets, mask, 2, exact, mask) ||
        d.raw[2] || d.raw[3] || d.setCalls != 2 || !d.bufferValid) return 8093;
    // No unit guessing or range clamping, even for a sub-kHz excess.
    exact[2] = 501;
    if (apply_curve_offsets_verified(&g, targets, mask, 2, exact, mask) ||
        d.setCalls != 2) return 8094;
    exact[2] = 1000000500;
    if (apply_curve_offsets_verified(&g, targets, mask, 2, exact, mask) ||
        d.setCalls != 2) return 8095;
    exact[2] = 500; exact[3] = -500; d.ignoreWrites = true;
    if (apply_curve_offsets_verified(&g, targets, mask, 2, exact, mask) ||
        d.setCalls != 4 || g.freqOffsets[2] || g.freqOffsetsHz[2]) return 8096;
    initialize(&d, &g, &g_vfBackendFuture);
    if (apply_curve_offsets_verified(&g, targets, mask, 2, exact, mask) ||
        d.setCalls || vf_graphics_offset_grid_hz(g.backend) != 1000 ||
        vf_graphics_offset_grid_hz(&g_vfBackendPascal) != 500) return 8097;
    if (vf_graphics_offset_readback_matches(0, true, 500, 0) ||
        !vf_graphics_offset_readback_matches(0, true, 0, 0) ||
        !vf_graphics_offset_readback_matches(0, false, 500, 0)) return 8098;

    // Execute scoped update/replacement through production target compilation
    // and the driver writer. Unowned raw controls survive both transitions.
    initialize(&d, &g, &g_vfBackendPascal);
    d.raw[2] = 30001; d.raw[4] = 7777;
    DesiredSettings previous{};
    previous.curvePointCaptured[2] = 1; previous.curvePointCapturedOffsetHz[2] = 15000500;
    DesiredSettings incoming{};
    incoming.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED;
    incoming.curvePointCaptured[3] = 1; incoming.curvePointCapturedOffsetHz[3] = -500;
    const VfFloorTopology topology = vf_floor_topology_compute(g.curve, g.freqOffsets, VF_NUM_POINTS);
    ServiceScopedApplyPlan scoped{};
    if (!service_build_scoped_apply_plan(&previous, incoming, g.curve, topology, false,
            &scoped, detail, sizeof(detail))) return 8127;
    r = linux_build_curve_targets(g.curve, g.freqOffsets, &scoped.hardware, -1000000, targets, mask);
    if (!apply_curve_offsets_verified(&g, targets, mask, 2, r.pointCapturedOffsetHz, r.pointCapturedIntent) ||
        d.raw[2] != 30001 || d.raw[3] != -1 || d.raw[4] != 7777 || d.setCalls != 1 ||
        !d.bufferValid || scoped.hardware.hasMemOffset || scoped.hardware.hasFan) return 8128;
    previous = scoped.owned;
    incoming.applicationScope = DESIRED_SCOPE_REPLACE_PROFILE;
    if (!service_build_scoped_apply_plan(&previous, incoming, g.curve, topology, false,
            &scoped, detail, sizeof(detail))) return 8129;
    r = linux_build_curve_targets(g.curve, g.freqOffsets, &scoped.hardware, -1000000, targets, mask);
    if (!apply_curve_offsets_verified(&g, targets, mask, 2, r.pointCapturedOffsetHz, r.pointCapturedIntent) ||
        d.raw[2] || d.raw[3] != -1 || d.raw[4] != 7777 || d.setCalls != 2 ||
        !d.bufferValid || scoped.owned.curvePointCaptured[2] || !scoped.owned.curvePointCaptured[3]) return 8130;
    initialize(&d, &g, &g_vfBackendPascal);
    d.raw[0] = 7777; d.raw[2] = 8889; d.raw[1] = 400000;
    for (int ci = 3; ci < 8; ++ci) d.raw[ci] = 400001;
    previous = {}; previous.hasGpuOffset = 1; previous.gpuOffsetMHz = 200;
    previous.gpuOffsetExcludeLowCount = 3;
    previous.hasCurvePoint[1] = 1; previous.curvePointMHz[1] = 1700;
    incoming = {}; incoming.applicationScope = DESIRED_SCOPE_REPLACE_PROFILE;
    incoming.hasGpuOffset = 1; incoming.gpuOffsetMHz = 100;
    if (!service_build_scoped_apply_plan(&previous, incoming, g.curve, topology, false,
            &scoped, detail, sizeof(detail), true) || scoped.graphicsOffsetViaVf) return 8145;
    r = linux_build_curve_targets(g.curve, g.freqOffsets, &scoped.hardware, -1000000, targets, mask, 1);
    if (r.composedGpuOffset || r.pointCount != 6 ||
        !apply_curve_offsets_verified(&g, targets, mask, 2, r.pointCapturedOffsetHz, r.pointCapturedIntent) ||
        d.raw[1] || d.raw[3] || d.raw[7] || d.raw[0] != 7777 || d.raw[2] != 8889 ||
        !d.bufferValid) return 8146;

    // Linux post-write curve correction (12630-12637): budget, Blackwell-style
    // flatten anchor quantization convergence, captured/cleanup preservation,
    // fixed-point stop, monotonicity refusal, and authoring shape/monotonicity.
    if (!linux_curve_correction_pass_may_start(0, 999999ULL, 999999ULL) ||
        !linux_curve_correction_pass_may_start(1, 1000ULL, 500ULL) ||
        linux_curve_correction_pass_may_start(1, linux_curve_correction_budget_ms(), 1ULL))
        return 12630;
    {
        // Simulate Profile 2/3 on Blackwell: ci=2..4 are offset-intent (+475 MHz),
        // ci=5 is flatten anchor (stock 2482 MHz, target 2957 MHz), ci=6..7 are
        // tail (-1000 MHz). On initial write, driver shifts ci=5 base by -10 MHz
        // when tail is flattened, landing at 2947 MHz (delta 10 > 8 MHz tol).
        VFCurvePoint simCurve[VF_NUM_POINTS] = {};
        int simOffsets[VF_NUM_POINTS] = {};
        gc_i64 simOffsetsHz[VF_NUM_POINTS] = {};
        const unsigned int stockKHz[8] = {
            1800000u, 2200000u, 2380000u, 2410000u, 2452000u, 2482000u, 2505000u, 2527000u
        };
        for (int i = 0; i < 8; ++i) {
            simCurve[i] = {stockKHz[i], 860000u + (unsigned int)i * 10000u};
        }
        DesiredSettings prof{};
        prof.hasGpuOffset = 1;
        prof.gpuOffsetMHz = 475;
        prof.gpuOffsetExcludeLowCount = 2;
        prof.hasLock = 1;
        prof.lockMode = LOCK_MODE_FLATTEN;
        prof.lockCi = 5;
        prof.lockMHz = 2957;
        for (int i = 2; i < 8; ++i) {
            prof.hasCurvePoint[i] = 1;
            prof.curvePointFromGpuOffset[i] = 1;
            prof.curvePointMHz[i] = 0;
        }
        int initTargets[VF_NUM_POINTS] = {};
        bool initMask[VF_NUM_POINTS] = {};
        LinuxCurveTargetBuildResult initBuild = linux_build_curve_targets(
            simCurve, simOffsets, &prof, -1000000, initTargets, initMask);
        // Add a captured cleanup point at ci=1 (0 Hz) to prove exact preservation.
        initBuild.pointMask[1] = initMask[1] = true;
        initBuild.pointCapturedIntent[1] = true;
        initBuild.pointCapturedOffsetHz[1] = 0;
        initTargets[1] = 0;
        auto applySimDriver = [&](const int* offs, const bool* msk,
                                  const gc_i64* exactHz, const bool* exactMsk) {
            for (int i = 0; i < 8; ++i) {
                if (!msk[i]) continue;
                simOffsets[i] = offs[i];
                simOffsetsHz[i] = (exactMsk && exactMsk[i]) ? exactHz[i] : (gc_i64)offs[i] * 1000LL;
            }
            for (int i = 0; i < 8; ++i) {
                long long base = (long long)stockKHz[i];
                // Driver cross-talk / quantization when tail is flattened:
                // anchor base drops by 10 MHz (2482 -> 2472 MHz).
                if (i == 5 && simOffsets[6] <= -900000) base -= 10000;
                long long freq = base + simOffsets[i];
                if (i > 5 && freq < (long long)simCurve[5].freq_kHz) freq = (long long)simCurve[5].freq_kHz;
                simCurve[i].freq_kHz = freq > 0 ? (unsigned int)freq : 0u;
            }
            // Monotonic clamp tail to anchor.
            for (int i = 6; i < 8; ++i) {
                if (simCurve[i].freq_kHz < simCurve[5].freq_kHz)
                    simCurve[i].freq_kHz = simCurve[5].freq_kHz;
            }
        };
        applySimDriver(initTargets, initMask, initBuild.pointCapturedOffsetHz, initBuild.pointCapturedIntent);
        // Initial write lands at 2947 MHz -> must fail verification (10 > 8).
        if (linux_verify_curve_absolute_targets(simCurve, initBuild, detail, sizeof(detail)))
            return 12631;
        VfOffsetRange simRange = {-1000000, 1000000, true};
        unsigned long long fakeClock = 100;
        int writeCalls = 0;
        ApplyCorrectionLoopOutcome out = linux_run_legacy_curve_correction(
            &prof, initBuild, initTargets, simRange, 0,
            [&](const int* offs, const bool* msk, const gc_i64* eHz, const bool* eMsk) {
                ++writeCalls;
                fakeClock += 25;
                applySimDriver(offs, msk, eHz, eMsk);
                return true;
            },
            [&](int ci) -> unsigned int { return simCurve[ci].freq_kHz; },
            [&](int ci) -> int { return simOffsets[ci]; },
            [&](char* det, size_t detSz) -> bool {
                return linux_verify_curve_absolute_targets(simCurve, initBuild, det, detSz) &&
                       linux_verify_curve_captured_targets(simOffsetsHz, initBuild, det, detSz, true);
            },
            [&]() -> unsigned long long { return fakeClock; },
            [](const char*) {});
        if (!out.verified || writeCalls != 1 || simOffsets[5] != 485000 ||
            simOffsets[4] != 475000 || simOffsets[6] != -1000000 ||
            simOffsets[1] != 0 || simOffsetsHz[1] != 0 ||
            curve_display_mhz_from_khz(simCurve[5].freq_kHz) != 2957)
            return 12632;

        // Fixed-point stop when driver ignores anchor offset changes and stays at 2947 MHz.
        applySimDriver(initTargets, initMask, initBuild.pointCapturedOffsetHz, initBuild.pointCapturedIntent);
        int stuckWrites = 0;
        ApplyCorrectionLoopOutcome stuck = linux_run_legacy_curve_correction(
            &prof, initBuild, initTargets, simRange, 0,
            [&](const int*, const bool*, const gc_i64*, const bool*) {
                ++stuckWrites;
                fakeClock += 25;
                return true;
            },
            [&](int ci) -> unsigned int { return simCurve[ci].freq_kHz; },
            [&](int ci) -> int { return simOffsets[ci]; },
            [&](char* det, size_t detSz) -> bool {
                return linux_verify_curve_absolute_targets(simCurve, initBuild, det, detSz);
            },
            [&]() -> unsigned long long { return fakeClock; },
            [](const char*) {});
        if (stuck.verified || !stuck.fixedPoint || stuckWrites > 3)
            return 12633;

        // Strict flatten monotonicity check fails closed when predecessor exceeds plateau.
        VFCurvePoint monoCurve[VF_NUM_POINTS] = {};
        int monoOffsets[VF_NUM_POINTS] = {};
        for (int i = 0; i < 8; ++i) monoCurve[i] = simCurve[i];
        monoCurve[4].freq_kHz = 2990000u; // ci=4 live 2990 MHz > lock 2957 MHz
        monoOffsets[4] = 475000;
        monoCurve[5].freq_kHz = 2957000u;
        monoOffsets[5] = 485000;
        monoCurve[6].freq_kHz = 2957000u;
        monoOffsets[6] = -1000000;
        monoCurve[7].freq_kHz = 2957000u;
        monoOffsets[7] = -1000000;
        bool monoViolated = false;
        int monoCi = -1;
        ApplyCorrectionLoopOutcome monoOut = linux_run_legacy_curve_correction(
            &prof, initBuild, initTargets, simRange, 0,
            [&](const int*, const bool*, const gc_i64*, const bool*) { return true; },
            [&](int ci) -> unsigned int { return monoCurve[ci].freq_kHz; },
            [&](int ci) -> int { return monoOffsets[ci]; },
            [&](char* det, size_t detSz) -> bool {
                return linux_verify_curve_absolute_targets(monoCurve, initBuild, det, detSz);
            },
            [&]() -> unsigned long long { return 50; },
            [](const char*) {},
            &monoViolated, &monoCi, nullptr);
        if (monoOut.verified || !monoViolated || monoCi != 5)
            return 12634;
    }
    {
        // Authoring correction shape validation and flatten monotonicity.
        LinuxCurveTargetBuildResult orig{};
        orig.pointCount = 3;
        orig.composedGpuOffset = true;
        orig.lockTail = true;
        for (int i = 4; i <= 6; ++i) {
            orig.pointMask[i] = true;
            orig.pointVoltageUv[i] = 900000u + (unsigned int)i * 5000u;
            orig.pointExactOffset[i] = true;
        }
        orig.pointExactOffsetHz[4] = 475000000;
        orig.pointBaselineDependent[4] = false;
        orig.pointAbsoluteIntent[5] = true;
        orig.pointFrequencyPrecise[5] = true;
        orig.pointTargetHz[5] = 2957000000LL;
        orig.pointExactOffsetHz[5] = 475000000;
        orig.pointBaselineDependent[5] = true;
        orig.pointAbsoluteIntent[6] = true;
        orig.pointFrequencyPrecise[6] = true;
        orig.pointTargetHz[6] = 2957000000LL;
        orig.pointExactOffsetHz[6] = -1000000000;
        orig.pointBaselineDependent[6] = false;

        LinuxCurveTargetBuildResult updated = orig;
        updated.pointExactOffsetHz[5] = 485000000; // baseline-dependent anchor may move
        if (!linux_authoring_correction_shape_matches(orig, updated)) return 12635;
        updated.pointExactOffsetHz[4] = 480000000; // offset-intent point may NOT drift
        if (linux_authoring_correction_shape_matches(orig, updated)) return 12636;

        VFCurvePoint authCurve[VF_NUM_POINTS] = {};
        gc_i64 authFreqHz[VF_NUM_POINTS] = {};
        gc_i64 authOffHz[VF_NUM_POINTS] = {};
        authCurve[4] = {2927000u, 920000u}; authFreqHz[4] = 2927000000LL; authOffHz[4] = 475000000LL;
        authCurve[5] = {2957000u, 925000u}; authFreqHz[5] = 2957000000LL; authOffHz[5] = 485000000LL;
        authCurve[6] = {2957000u, 935000u}; authFreqHz[6] = 2957000000LL; authOffHz[6] = -1000000000LL;
        if (!linux_verify_authoring_flatten_monotonicity(
                authCurve, authFreqHz, authOffHz, true, true, orig, LOCK_MODE_FLATTEN, 5, detail, sizeof(detail)))
            return 12637;
        authCurve[4].freq_kHz = 3000000u; authFreqHz[4] = 3000000000LL;
        if (linux_verify_authoring_flatten_monotonicity(
                authCurve, authFreqHz, authOffHz, true, true, orig, LOCK_MODE_FLATTEN, 5, detail, sizeof(detail)))
            return 12638;
        // Exact-frequency verify mismatch diagnostic includes actual Hz.
        authCurve[5].freq_kHz = 2947000u; authFreqHz[5] = 2947000000LL;
        if (linux_verify_curve_absolute_targets(authCurve, orig, detail, sizeof(detail), authFreqHz, true) ||
            strstr(detail, "2947000000") == nullptr || strstr(detail, "2957000000") == nullptr)
            return 12639;
    }
    return run_linux_memory_curve_tests();
}

#ifdef GC_VF_WRITE_TEST_MAIN
int main() {
    const int failure = run_linux_vf_write_tests();
    printf("Linux VF write regression: %d\n", failure);
    return failure;
}
#endif
