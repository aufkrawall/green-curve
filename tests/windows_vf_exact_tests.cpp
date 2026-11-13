// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Execute the Windows production exact writer/precheck using fake driver calls.
#include "gpu_core.h"
#include "vf_backends.h"
#include "vf_offset_range_policy.h"
#include "curve_preservation_precision.h"
#include "vf_exact_control_write.h"
#include "apply_correction_budget_policy.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>

namespace {
using ULONGLONG = unsigned long long;
using NvApiFunc = int (*)(void*, void*);
struct App {
    const VfBackendSpec* vfBackend = &g_vfBackendPascal;
    void* gpuHandle = nullptr;
    unsigned char vfMask[32]{};
    VFCurvePoint curve[VF_NUM_POINTS]{};
    int freqOffsets[VF_NUM_POINTS]{};
    gc_i64 freqOffsetsHz[VF_NUM_POINTS]{};
    bool vfOffsetsHzValid = true, loaded = true;
} g_app;
struct Driver {
    int raw[VF_NUM_POINTS]{};
    int gets = 0, sets = 0, rebuilds = 0;
    bool failRead = false, failSet = false, ignore = false, one = false, metadataBad = false;
    bool exactAvailable = true, untouched = true;
} driver;
static void debug_log(const char*, ...) {}
static void set_last_apply_phase(const char*) {}
static void set_message(char* out, size_t size, const char* format, ...) {
    va_list args; va_start(args, format); vsnprintf(out, size, format, args); va_end(args);
}
static bool apply_plan_mem_offset_write(const App&, int) { return true; }
static VfOffsetRange vf_offset_range_current() { return vf_offset_range_fallback(); }
static ULONGLONG GetTickCount64() { return 50; }
static void rebuild_visible_map() { ++driver.rebuilds; }
static bool nvapi_read_curve() { return true; }
static bool nvapi_read_offsets() {
    if (driver.failRead) return false;
    g_app.vfOffsetsHzValid = driver.exactAvailable;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        g_app.freqOffsets[i] = vf_graphics_offset_khz(g_app.vfBackend, driver.raw[i]);
        vf_graphics_offset_hz(g_app.vfBackend, driver.raw[i], &g_app.freqOffsetsHz[i]);
    }
    return true;
}
static int get(void*, void* data) {
    ++driver.gets; if (driver.failRead) return -9;
    auto* bytes = (unsigned char*)data; const auto& b = *g_app.vfBackend;
    if (driver.metadataBad) bytes[0] ^= 1;
    for (unsigned int i = 0; i < VF_NUM_POINTS; ++i)
        memcpy(bytes + b.controlEntryBaseOffset + i * b.controlEntryStride + b.controlEntryDeltaOffset, &driver.raw[i], 4);
    return 0;
}
static int set(void*, void* data) {
    ++driver.sets; if (driver.failSet) return -1;
    const auto* bytes = (const unsigned char*)data; const auto& b = *g_app.vfBackend;
    bool changed = false;
    for (unsigned int i = 0; i < VF_NUM_POINTS; ++i) {
        int value = 0; memcpy(&value, bytes + b.controlEntryBaseOffset + i * b.controlEntryStride + b.controlEntryDeltaOffset, 4);
        if (!(bytes[b.controlMaskOffset + i / 8] & (1u << (i % 8)))) {
            driver.untouched &= value == driver.raw[i]; continue;
        }
        if (driver.ignore || (driver.one && changed)) continue;
        driver.raw[i] = value; changed = true;
    }
    return 0;
}
static void* nvapi_qi(unsigned int id) {
    if (id == g_app.vfBackend->getControlId) return (void*)get;
    if (id == g_app.vfBackend->setControlId) return (void*)set;
    return nullptr;
}
#include "gpu_backend_exact_curve.h"
static void initialize() {
    driver = {}; g_app = {}; g_app.gpuHandle = &driver;
    for (int i = 0; i < 8; ++i) {
        g_app.vfMask[i / 8] |= (unsigned char)(1u << (i % 8));
        g_app.curve[i] = {1500000, 650000u + (unsigned int)i * 6250};
    }
    driver.raw[7] = 30001;
    nvapi_read_offsets();
}
}

int run_windows_vf_exact_tests() {
    gc_i64 hz[VF_NUM_POINTS]{}; bool mask[VF_NUM_POINTS]{};
    initialize(); mask[0] = mask[1] = mask[2] = true;
    hz[0] = 15000500; hz[1] = -15000500; driver.raw[2] = 1;
    if (!apply_curve_offsets_exact_hz(hz, mask, 2, 0) || driver.raw[0] != 30001 || driver.raw[1] != -30001 ||
        driver.raw[2] || driver.raw[7] != 30001 || !driver.untouched || driver.sets != 1 || driver.rebuilds != 1) return 8580;
    if (!apply_curve_offsets_exact_hz(hz, mask, 2, 0) || driver.sets != 1) return 8581;
    initialize(); driver.one = true; hz[2] = 1000000;
    if (!apply_curve_offsets_exact_hz(hz, mask, 1, 0) || driver.sets != 3 || !driver.untouched || driver.raw[2] != 2000) return 8582;
    initialize(); hz[0] = 15000501;
    if (apply_curve_offsets_exact_hz(hz, mask, 2, 0) || driver.sets) return 8583;
    hz[0] = 15000500; driver.metadataBad = true;
    if (apply_curve_offsets_exact_hz(hz, mask, 2, 0) || driver.sets) return 8584;
    initialize(); driver.ignore = true;
    if (apply_curve_offsets_exact_hz(hz, mask, 1, 0) || driver.sets != 2) return 8585;
    initialize(); driver.failSet = true;
    if (apply_curve_offsets_exact_hz(hz, mask, 2, 0) || driver.sets != 1) return 8586;
    initialize();
    if (apply_curve_offsets_exact_hz(hz, mask, 2, 49) || driver.sets) return 8587;
    initialize(); mask[8] = true;
    if (apply_curve_offsets_exact_hz(hz, mask, 2, 0) || driver.sets) return 8588;
    mask[8] = false;
    DesiredSettings desired{}; desired.hasMemOffset = 1; desired.memOffsetMHz = 1;
    char why[192]{}; initialize(); driver.raw[0] = 1; nvapi_read_offsets();
    if (!curve_control_nonzero(0, true, 500) || curve_control_nonzero(1, true, 0) ||
        !apply_curve_preservation_precheck(&desired, why, sizeof(why)) || driver.sets) return 8589;
    driver.failRead = true;
    if (apply_curve_preservation_precheck(&desired, why, sizeof(why)) || driver.sets) return 8590;
    initialize(); driver.exactAvailable = false;
    if (apply_curve_preservation_precheck(&desired, why, sizeof(why)) || driver.sets) return 8591;
    initialize(); driver.raw[0] = 2000001;
    if (apply_curve_preservation_precheck(&desired, why, sizeof(why)) || driver.sets) return 8592;
    initialize(); driver.raw[0] = 1; g_app.vfMask[0] &= (unsigned char)~1u;
    if (apply_curve_preservation_precheck(&desired, why, sizeof(why)) || driver.sets) return 8593;
    initialize(); int old[VF_NUM_POINTS]{}, target[VF_NUM_POINTS]{}; gc_i64 original[VF_NUM_POINTS]{};
    bool tail[VF_NUM_POINTS]{}; original[0] = 500; original[1] = -500; original[2] = 15000500;
    old[2] = 15000; target[0] = 1000; target[1] = -1000; target[2] = 17000;
    desired.curvePointCaptured[2] = 1; desired.curvePointCapturedOffsetHz[2] = 0;
    if (!curve_preservation_targets(desired, mask, tail, target, old, original, true, hz) ||
        hz[0] != 1000500 || hz[1] != -1000500 || hz[2] != 0) return 8594;
    if (curve_preservation_targets(desired, mask, tail, target, old, original, false, hz)) return 8595;
    tail[0] = true; mask[1] = false;
    if (!curve_preservation_targets(desired, mask, tail, target, old, original, true, hz) || hz[0] != 1000000) return 8596;
    initialize(); desired = {}; desired.hasMemOffset = 1; desired.memOffsetMHz = 1;
    driver.raw[0] = 1; nvapi_read_offsets();
    mask[0] = true; mask[1] = mask[2] = false; tail[0] = false;
    target[0] = old[0] = 0; original[0] = g_app.freqOffsetsHz[0];
    if (!apply_curve_preservation_precheck(&desired, why, sizeof(why)) ||
        !curve_preservation_targets(desired, mask, tail, target, old, original, true, hz)) return 8597;
    driver.raw[0] = 0; // A memory setter reset an unowned half-kHz control.
    if (!apply_curve_offsets_exact_hz(hz, mask, 2, 0) || driver.raw[0] != 1 ||
        desired.curvePointCaptured[0] || desired.hasCurvePoint[0]) return 8598;
    initialize(); g_app.loaded = false; driver.failRead = true;
    if (!apply_curve_preservation_precheck(&desired, why, sizeof(why)) || driver.sets) return 8599;
    initialize(); unsigned int voltages[VF_NUM_POINTS]{};
    for (int i = 0; i < VF_NUM_POINTS; ++i) voltages[i] = g_app.curve[i].volt_uV;
    g_app.curve[0].volt_uV += 1250;
    if (apply_curve_offsets_exact_hz(hz, mask, 2, 0, voltages) || driver.sets) return 8600;
    return 0;
}
