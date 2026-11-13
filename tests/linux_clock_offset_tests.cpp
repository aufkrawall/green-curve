// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Exercise the production NVML offset setters with independently scripted
// P0 and global controls. No driver library or GPU is loaded.
#include "linux_backend.h"

static void lb_log(const char*, ...) {}
#include "linux_backend_clock_offsets.h"
#include "linux_graphics_offset_control.h"

namespace {
struct Driver {
    int p0Value, globalValue;
    int p0Writes, globalWrites, p0Reads, globalReads;
    bool snap, failP0Read, failGlobalRead, failP0Set, badMetadata;
    bool layoutValid;
    bool badRange, globalRangeFails;
    bool narrowRange, failReadAfterWrite;
};
static Driver* driver(nvmlDevice_t device) { return reinterpret_cast<Driver*>(device); }
static nvmlReturn_t set_p0(nvmlDevice_t device, nvmlClockOffset_t* info) {
    Driver* d = driver(device);
    ++d->p0Writes;
    d->layoutValid &= info->version == nvmlClockOffset_v1 && info->pstate == NVML_PSTATE_0;
    d->p0Value = d->snap ? (info->clockOffsetMHz / 2) * 2 : info->clockOffsetMHz;
    return d->failP0Set ? NVML_ERROR_NOT_SUPPORTED : NVML_SUCCESS;
}
static nvmlReturn_t get_p0(nvmlDevice_t device, nvmlClockOffset_t* info) {
    Driver* d = driver(device);
    ++d->p0Reads;
    d->layoutValid &= info->version == nvmlClockOffset_v1 && info->pstate == NVML_PSTATE_0;
    if (d->failP0Read || (d->failReadAfterWrite && d->p0Writes)) return NVML_ERROR_NOT_SUPPORTED;
    info->clockOffsetMHz = d->p0Value;
    info->minClockOffsetMHz = -3000; info->maxClockOffsetMHz = 3000;
    if (d->narrowRange) { info->minClockOffsetMHz = -100; info->maxClockOffsetMHz = 100; }
    if (d->badRange) info->minClockOffsetMHz = 4000;
    if (d->badMetadata) info->pstate = 1;
    return NVML_SUCCESS;
}
static nvmlReturn_t set_global(nvmlDevice_t device, int value) {
    Driver* d = driver(device);
    ++d->globalWrites;
    d->globalValue = d->snap ? (value / 2) * 2 : value;
    return NVML_SUCCESS;
}
static nvmlReturn_t get_global(nvmlDevice_t device, int* value) {
    Driver* d = driver(device);
    ++d->globalReads;
    *value = d->globalValue;
    return d->failGlobalRead ? NVML_ERROR_NOT_SUPPORTED : NVML_SUCCESS;
}
static nvmlReturn_t get_global_range(nvmlDevice_t device, int* minimum, int* maximum) {
    *minimum = -3000; *maximum = 3000;
    return driver(device)->globalRangeFails ? NVML_ERROR_UNKNOWN : NVML_SUCCESS;
}
static void initialize(Driver* d, LinuxGpuState* g) {
    *d = {}; *g = {}; d->layoutValid = true;
    g->nvmlDevice = reinterpret_cast<nvmlDevice_t>(d);
    g->nvml.setClockOffsets = set_p0; g->nvml.getClockOffsets = get_p0;
    g->nvml.setMemClkVfOffset = set_global; g->nvml.getMemClkVfOffset = get_global;
    g->nvml.setGpcClkVfOffset = set_global; g->nvml.getGpcClkVfOffset = get_global;
}

static int graphics_control_cases() {
    Driver driverState{}; LinuxGpuState g{}; initialize(&driverState, &g);
    auto& state = driverState;
    DesiredSettings d{}; d.hasGpuOffset = 1; d.gpuOffsetMHz = -247;
    d.authoring.graphicsPrecise = 1; d.authoring.graphicsOffsetHz = -247000000;
    d.authoring.graphicsSurface = DESIRED_GRAPHICS_GLOBAL;
    state.p0Value = 91; state.globalValue = 17;
    LinuxHardwareSnapshot snapshot{}; snapshot.gpuOffsetMHz = 91; snapshot.gpuOffsetValid = true;
    char why[160]{};
    if (!linux_bind_graphics_snapshot(&g, &d, &snapshot, why, sizeof(why)) ||
        !snapshot.gpuOffsetExact || snapshot.gpuOffsetSurface != CLOCK_OFFSET_SURFACE_GLOBAL ||
        snapshot.gpuOffsetMHz != 17 || state.p0Reads || state.globalWrites || state.p0Writes) return 8828;
    if (!linux_apply_named_graphics_offset(&g, &d) || state.globalValue != -247 || state.p0Value != 91 ||
        state.globalWrites != 1 || state.p0Writes || state.p0Reads) return 8829;
    if (!linux_apply_named_graphics_offset(&g, &d) || state.globalWrites != 1) return 8830;
    initialize(&state, &g); d.authoring.graphicsSurface = DESIRED_GRAPHICS_P0;
    state.failP0Set = true;
    if (linux_apply_named_graphics_offset(&g, &d) || state.p0Writes != 1 || state.globalWrites ||
        state.globalReads || state.p0Value != -247) return 8831;
    state.failP0Set = false; state.p0Value = 0; state.snap = true;
    if (linux_apply_named_graphics_offset(&g, &d) || state.p0Writes != 2 || state.globalWrites) return 8832;
    initialize(&state, &g); state.failP0Read = true;
    const auto before = snapshot;
    if (linux_bind_graphics_snapshot(&g, &d, &snapshot, why, sizeof(why)) ||
        memcmp(&before, &snapshot, sizeof(before)) || state.p0Writes || state.globalWrites || state.globalReads) return 8833;
    initialize(&state, &g); state.badMetadata = true;
    if (linux_apply_named_graphics_offset(&g, &d) || state.p0Writes || state.globalWrites || state.globalReads) return 8834;
    initialize(&state, &g); d.authoring.graphicsOffsetHz = -247000500;
    if (graphics_offset_control_plan(d, g.nvml, g.nvmlDevice).status != GRAPHICS_OFFSET_OFF_GRID ||
        linux_apply_named_graphics_offset(&g, &d) || state.p0Writes || state.p0Reads || state.globalWrites) return 8835;
    d.authoring.graphicsOffsetHz = -247000000; state.badRange = true;
    auto plan = graphics_offset_control_plan(d, g.nvml, g.nvmlDevice);
    if (plan.status != GRAPHICS_OFFSET_READY || plan.before.rangeValid || !plan.before.valid ||
        !linux_apply_named_graphics_offset(&g, &d) || state.globalReads || state.globalWrites) return 8836;
    initialize(&state, &g); d.authoring.graphicsSurface = DESIRED_GRAPHICS_GLOBAL;
    g.nvml.getGpcClkMinMaxVfOffset = get_global_range; state.globalRangeFails = true;
    if (!linux_apply_named_graphics_offset(&g, &d) || state.globalWrites != 1 || state.p0Writes) return 8837;
    initialize(&state, &g); g.nvml.setClockOffsets = nullptr; d.authoring.graphicsSurface = DESIRED_GRAPHICS_P0;
    if (linux_apply_named_graphics_offset(&g, &d) || state.globalWrites || state.p0Writes || state.globalReads) return 8838;
    d.authoring.graphicsSurface = DESIRED_GRAPHICS_AUTO;
    if (graphics_offset_control_plan(d, g.nvml, g.nvmlDevice).status != GRAPHICS_OFFSET_INVALID) return 8839;
    d.authoring.graphicsSurface = DESIRED_GRAPHICS_VF_COMPONENT; d.gpuOffsetMHz = 0; d.authoring.graphicsOffsetHz = 500;
    if (graphics_offset_control_plan(d, {}, g.nvmlDevice).status != GRAPHICS_OFFSET_VF_COMPONENT) return 8840;
    initialize(&state, &g); d.gpuOffsetMHz = -247; d.authoring.graphicsOffsetHz = -247000000;
    d.authoring.graphicsSurface = DESIRED_GRAPHICS_P0; state.narrowRange = true;
    if (graphics_offset_control_plan(d, g.nvml, g.nvmlDevice).status != GRAPHICS_OFFSET_OUT_OF_RANGE ||
        linux_apply_named_graphics_offset(&g, &d) || state.p0Writes || state.globalWrites) return 8841;
    state.narrowRange = false; state.failReadAfterWrite = true;
    if (linux_apply_named_graphics_offset(&g, &d) || state.p0Writes != 1 || state.p0Value != -247 ||
        state.globalWrites || state.globalReads) return 8842;
    return 0;
}
}

int run_linux_clock_offset_tests() {
    if (int failure = graphics_control_cases()) return failure;
    Driver d{}; LinuxGpuState g{};
    initialize(&d, &g); d.snap = true;
    if (!nvml_set_clock_offset(&g, NVML_CLOCK_MEM, 247) || d.p0Value != 246) return 8150;
    initialize(&d, &g); d.snap = true;
    if (nvml_set_clock_offset(&g, NVML_CLOCK_MEM, 247, CLOCK_OFFSET_SURFACE_P0) ||
        d.p0Writes != 1 || d.globalWrites || d.p0Value != 246) return 8151;
    initialize(&d, &g);
    if (!nvml_set_clock_offset(&g, NVML_CLOCK_MEM, 247, CLOCK_OFFSET_SURFACE_P0) ||
        d.p0Value != 247 || !d.layoutValid || d.globalReads || d.globalWrites) return 8152;
    initialize(&d, &g); d.failP0Read = true; d.globalValue = 247;
    if (nvml_set_clock_offset(&g, NVML_CLOCK_MEM, 247, CLOCK_OFFSET_SURFACE_P0) ||
        d.p0Writes != 1 || d.p0Reads != 1 || d.globalReads || d.globalWrites) return 8153;
    initialize(&d, &g); d.failP0Set = true;
    if (nvml_set_clock_offset(&g, NVML_CLOCK_MEM, 247, CLOCK_OFFSET_SURFACE_P0) ||
        d.p0Writes != 1 || d.globalWrites || d.p0Value != 247) return 8154;
    initialize(&d, &g); d.p0Value = 999;
    if (!nvml_set_clock_offset(&g, NVML_CLOCK_MEM, -247, CLOCK_OFFSET_SURFACE_GLOBAL) ||
        d.globalValue != -247 || d.p0Value != 999 || d.p0Writes || d.p0Reads) return 8155;
    initialize(&d, &g); d.snap = true;
    if (nvml_set_clock_offset(&g, NVML_CLOCK_MEM, -247, CLOCK_OFFSET_SURFACE_GLOBAL) ||
        d.globalWrites != 1 || d.globalValue != -246) return 8156;
    initialize(&d, &g); d.failGlobalRead = true;
    if (nvml_set_clock_offset(&g, NVML_CLOCK_MEM, 0, CLOCK_OFFSET_SURFACE_GLOBAL) ||
        d.globalWrites != 1) return 8157;
    initialize(&d, &g); g.nvml.getMemClkVfOffset = nullptr;
    if (nvml_set_clock_offset(&g, NVML_CLOCK_MEM, 247, CLOCK_OFFSET_SURFACE_GLOBAL) ||
        d.p0Writes || d.globalWrites) return 8158;
    initialize(&d, &g); g.nvml.setClockOffsets = nullptr;
    if (nvml_set_clock_offset(&g, NVML_CLOCK_MEM, 247, CLOCK_OFFSET_SURFACE_P0) ||
        d.p0Writes || d.globalWrites) return 8159;
    initialize(&d, &g); d.badMetadata = true;
    if (nvml_set_clock_offset(&g, NVML_CLOCK_MEM, 247, CLOCK_OFFSET_SURFACE_P0) ||
        d.p0Writes != 1 || d.globalReads) return 8160;
    d.globalValue = 77;
    const NvmlClockOffsetReadback fallback = nvml_read_clock_offset(&g, NVML_CLOCK_MEM);
    if (!fallback.offsetValid || fallback.modernApi || fallback.offsetMHz != 77 || d.globalReads != 1) return 8161;
    initialize(&d, &g);
    if (nvml_set_clock_offset(&g, NVML_CLOCK_MEM, 247, 99) ||
        d.p0Writes || d.globalWrites) return 8162;
    if (!nvml_set_clock_offset(&g, NVML_CLOCK_MEM, 0, CLOCK_OFFSET_SURFACE_P0) ||
        d.p0Value || d.p0Writes != 1) return 8163;
    initialize(&d, &g); d.p0Value = 247;
    auto read = nvml_clock_observe(g.nvml, g.nvmlDevice, NVML_CLOCK_MEM);
    if (!read.control.valid || !read.control.rangeValid || read.control.value != 247 ||
        read.surface != CLOCK_OFFSET_SURFACE_P0 || d.globalReads) return 8265;
    initialize(&d, &g); d.failP0Read = true; d.globalValue = -247;
    read = nvml_clock_observe(g.nvml, g.nvmlDevice, NVML_CLOCK_MEM);
    if (!read.control.valid || read.control.value != -247 || read.control.rangeValid ||
        read.control.rangeStatus != NVML_ERROR_NOT_SUPPORTED || read.surface != CLOCK_OFFSET_SURFACE_GLOBAL) return 8266;
    const NvmlClockOffsetReadback controlOnly = nvml_read_clock_offset(&g, NVML_CLOCK_MEM);
    if (!controlOnly.offsetValid || controlOnly.rangeValid || controlOnly.offsetMHz != -247) return 8267;
    initialize(&d, &g); d.failP0Read = d.failGlobalRead = true;
    g.nvml.getMemClkMinMaxVfOffset = get_global_range;
    read = nvml_clock_observe(g.nvml, g.nvmlDevice, NVML_CLOCK_MEM);
    if (read.control.valid || !read.control.rangeValid || read.control.rangeStatus != NVML_SUCCESS ||
        read.control.status != NVML_ERROR_NOT_SUPPORTED) return 8268;
    initialize(&d, &g); d.badRange = true; d.p0Value = 247; d.globalValue = 88;
    g.nvml.getMemClkMinMaxVfOffset = get_global_range;
    read = nvml_clock_observe(g.nvml, g.nvmlDevice, NVML_CLOCK_MEM);
    if (!read.control.valid || read.control.rangeValid || read.control.value != 247 ||
        d.globalReads || read.control.rangeStatus != NVML_SUCCESS) return 8269;
    read = nvml_clock_observe(g.nvml, g.nvmlDevice, NVML_CLOCK_MEM, true);
    if (!read.control.valid || !read.control.rangeValid || read.control.value != 88 ||
        read.surface != CLOCK_OFFSET_SURFACE_GLOBAL) return 8270;
    initialize(&d, &g); d.failP0Read = d.globalRangeFails = true; d.globalValue = 247;
    g.nvml.getMemClkMinMaxVfOffset = get_global_range;
    read = nvml_clock_observe(g.nvml, g.nvmlDevice, NVML_CLOCK_MEM);
    if (!read.control.valid || read.control.rangeValid || read.control.value != 247 ||
        read.control.rangeStatus != NVML_ERROR_UNKNOWN || read.control.status != NVML_SUCCESS) return 8271;
    return 0;
}
