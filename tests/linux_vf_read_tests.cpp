// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Execute the production atomic VF reader without loading a GPU driver.

#include "linux_backend.h"
#include "linux_vf_validation.h"
#include "vf_backends.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static bool nvapi_ok(int status) { return status == 0; }
static const char* nvapi_status_name(int) { return "fixture"; }
static void lb_log(const char*, ...) {}
using nvapi_buf_t = int (*)(void*, void*);
#include "linux_backend_vf_read.h"

namespace {
struct VfDriver {
    const VfBackendSpec* backend;
    unsigned char mask[32];
    int points;
    int calls[3];
    int refusedCall;
    int malformedVersionCall;
    int changedMaskCall;
    bool malformedPoint;
    bool malformedOffset;
    bool requestValid;
};

static void put_word(unsigned char* buffer, unsigned int offset,
                     unsigned int word) {
    memcpy(buffer + offset, &word, sizeof(word));
}

static int read_table(void* handle, void* buffer, int call) {
    VfDriver* driver = static_cast<VfDriver*>(handle);
    const VfBackendSpec* b = driver->backend;
    unsigned char* bytes = static_cast<unsigned char*>(buffer);
    const unsigned int sizes[] = {
        b->infoBufferSize, b->statusBufferSize, b->controlBufferSize};
    const unsigned int versions[] = {
        b->infoVersion, b->statusVersion, b->controlVersion};
    const unsigned int maskOffsets[] = {
        b->infoMaskOffset, b->statusMaskOffset, b->controlMaskOffset};
    unsigned int version = 0;
    memcpy(&version, bytes, sizeof(version));
    driver->requestValid &= version == ((versions[call] << 16) | sizes[call]);
    ++driver->calls[call];
    if (call != 0) {
        driver->requestValid &=
            memcmp(bytes + maskOffsets[call], driver->mask, sizeof(driver->mask)) == 0;
        // There is no clock-count input at 0x24: keep reserved words zero.
        unsigned int reserved = 0;
        memcpy(&reserved, bytes + 0x24, sizeof(reserved));
        driver->requestValid &= reserved == 0;
    }
    if (driver->refusedCall == call) return -9;
    memset(bytes, 0, sizes[call]);
    if (driver->malformedVersionCall == call) version ^= 1u << 16;
    put_word(bytes, 0, version);
    memcpy(bytes + maskOffsets[call], driver->mask, sizeof(driver->mask));
    if (driver->changedMaskCall == call) bytes[maskOffsets[call]] ^= 1u;
    for (int i = 0; i < driver->points; ++i) {
        if (call == 1) {
            unsigned int offset = b->statusEntriesOffset + (unsigned int)i * b->statusEntryStride;
            put_word(bytes, offset, driver->malformedPoint && i == 2
                ? 7000000u : 600000u + (unsigned int)i * 15000u);
            put_word(bytes, offset + 4u, 650000u + (unsigned int)i * 5000u);
        } else if (call == 2) {
            unsigned int offset = b->controlEntryBaseOffset +
                (unsigned int)i * b->controlEntryStride + b->controlEntryDeltaOffset;
            put_word(bytes, offset, driver->malformedOffset && i == 2
                ? 6000000u : 15000u);
        }
    }
    return 0;
}

static int get_info(void* handle, void* buffer) { return read_table(handle, buffer, 0); }
static int get_status(void* handle, void* buffer) { return read_table(handle, buffer, 1); }
static int get_control(void* handle, void* buffer) { return read_table(handle, buffer, 2); }

static void* query_interface(unsigned int id) {
    if (id == g_vfBackendPascal.getInfoId) return reinterpret_cast<void*>(get_info);
    if (id == g_vfBackendPascal.getStatusId) return reinterpret_cast<void*>(get_status);
    if (id == g_vfBackendPascal.getControlId) return reinterpret_cast<void*>(get_control);
    return nullptr;
}

static void initialize(VfDriver* driver, LinuxGpuState* gpu, int points = 80) {
    *driver = {};
    *gpu = {};
    driver->backend = &g_vfBackendPascal;
    driver->points = points;
    driver->refusedCall = -1;
    driver->malformedVersionCall = -1;
    driver->changedMaskCall = -1;
    driver->requestValid = true;
    for (int i = 0; i < points; ++i)
        driver->mask[i / 8] |= (unsigned char)(1u << (i % 8));
    gpu->backend = driver->backend;
    gpu->gpuHandle = driver;
    gpu->nvapiQi = query_interface;
}
} // namespace

int run_linux_vf_read_tests() {
    VfDriver driver = {};
    LinuxGpuState gpu = {};
    char why[160] = {};
    initialize(&driver, &gpu);
    // Pascal has a valid mask with no bits at indices 128..159 (info +0x14).
    // The previous reader misread that word as numClocks and stopped at info.
    if (!nvapi_read_vf_snapshot(&gpu, why, sizeof(why))) return 7680;
    if (!driver.requestValid || driver.calls[0] != 1 ||
        driver.calls[1] != 1 || driver.calls[2] != 1) return 7681;
    if (!gpu.vfSnapshotFresh || !gpu.vfStructureValid || gpu.numPopulated != 80 ||
        gpu.curve[79].freq_kHz != 1785000u || gpu.freqOffsets[79] != 15000)
        return 7682;

    // Higher mask words are bitfields, even when their value exceeds 64.
    initialize(&driver, &gpu, 128);
    driver.mask[16] = 0xa5;
    if (!nvapi_read_vf_snapshot(&gpu, why, sizeof(why)) ||
        !driver.requestValid || gpu.numPopulated != 128) return 7683;

    // Empty and incomplete answers still refuse, without publishing a curve.
    initialize(&driver, &gpu);
    memset(driver.mask, 0, sizeof(driver.mask));
    if (nvapi_read_vf_snapshot(&gpu, why, sizeof(why)) ||
        driver.calls[1] || gpu.vfSnapshotFresh ||
        gpu.vfInfoStatus != LB_NVAPI_INVALID_DATA ||
        gpu.vfStatusStatus != LB_NVAPI_NOT_ATTEMPTED ||
        gpu.vfControlStatus != LB_NVAPI_NOT_ATTEMPTED) return 7684;
    for (int call = 0; call < 3; ++call) {
        initialize(&driver, &gpu);
        driver.refusedCall = call;
        if (nvapi_read_vf_snapshot(&gpu, why, sizeof(why)) ||
            gpu.vfSnapshotFresh || gpu.numPopulated) return 7685 + call;
        initialize(&driver, &gpu);
        driver.malformedVersionCall = call;
        if (nvapi_read_vf_snapshot(&gpu, why, sizeof(why)) ||
            gpu.vfSnapshotFresh || gpu.numPopulated) return 7688 + call;
    }
    for (int call = 1; call < 3; ++call) {
        initialize(&driver, &gpu);
        driver.changedMaskCall = call;
        if (nvapi_read_vf_snapshot(&gpu, why, sizeof(why)) ||
            gpu.vfSnapshotFresh || gpu.numPopulated) return 7690 + call;
    }
    initialize(&driver, &gpu);
    driver.malformedPoint = true;
    if (nvapi_read_vf_snapshot(&gpu, why, sizeof(why)) ||
        gpu.vfSnapshotFresh || gpu.numPopulated) return 7693;
    initialize(&driver, &gpu);
    driver.malformedOffset = true;
    if (nvapi_read_vf_snapshot(&gpu, why, sizeof(why)) ||
        gpu.vfSnapshotFresh || gpu.numPopulated) return 7694;
    initialize(&driver, &gpu, 7);
    if (nvapi_read_vf_snapshot(&gpu, why, sizeof(why)) ||
        gpu.vfSnapshotFresh || gpu.numPopulated) return 7695;

    // A refused refresh keeps the previous values only as explicitly stale.
    initialize(&driver, &gpu);
    if (!nvapi_read_vf_snapshot(&gpu, why, sizeof(why))) return 7696;
    driver.refusedCall = 1;
    if (nvapi_read_vf_snapshot(&gpu, why, sizeof(why)) || gpu.vfSnapshotFresh ||
        gpu.vfStructureValid || gpu.curve[79].freq_kHz != 1785000u ||
        gpu.freqOffsets[79] != 15000 || !why[0]) return 7697;
    return 0;
}

#ifdef GC_VF_READ_TEST_MAIN
int main() {
    const int failure = run_linux_vf_read_tests();
    printf("Linux VF read regression: %d\n", failure);
    return failure;
}
#endif
