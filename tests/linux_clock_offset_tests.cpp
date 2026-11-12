// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Exercise the production NVML offset setters with independently scripted
// P0 and global controls. No driver library or GPU is loaded.
#include "linux_backend.h"

static void lb_log(const char*, ...) {}
#include "linux_backend_clock_offsets.h"

namespace {
struct Driver {
    int p0Value, globalValue;
    int p0Writes, globalWrites, p0Reads, globalReads;
    bool snap, failP0Read, failGlobalRead, failP0Set, badMetadata;
    bool layoutValid;
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
    if (d->failP0Read) return NVML_ERROR_NOT_SUPPORTED;
    info->clockOffsetMHz = d->p0Value;
    info->minClockOffsetMHz = -3000; info->maxClockOffsetMHz = 3000;
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
static void initialize(Driver* d, LinuxGpuState* g) {
    *d = {}; *g = {}; d->layoutValid = true;
    g->nvmlDevice = reinterpret_cast<nvmlDevice_t>(d);
    g->nvml.setClockOffsets = set_p0; g->nvml.getClockOffsets = get_p0;
    g->nvml.setMemClkVfOffset = set_global; g->nvml.getMemClkVfOffset = get_global;
}
}

int run_linux_clock_offset_tests() {
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
    return 0;
}
