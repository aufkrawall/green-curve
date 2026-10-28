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
    int foreignPoints;       // another clock domain right after the curve
    bool foreignZeroVoltage;
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
            if (i + 1 == driver->points) {
                for (int f = 0; f < driver->foreignPoints; ++f) {
                    offset += b->statusEntryStride;
                    put_word(bytes, offset, 405000u + (unsigned int)f * 1000u);
                    put_word(bytes, offset + 4u, driver->foreignZeroVoltage
                        ? 0u : 540000u + (unsigned int)f * 5000u);
                }
            }
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

static void add_foreign_domain(VfDriver* driver, int count, bool zeroVoltage) {
    driver->foreignPoints = count;
    driver->foreignZeroVoltage = zeroVoltage;
    for (int i = driver->points; i < driver->points + count; ++i)
        driver->mask[i / 8] |= (unsigned char)(1u << (i % 8));
}

static bool layout_text_is(const LinuxGpuState& gpu, const char* expected) {
    return strcmp(linux_vf_layout_text(&gpu.vfLayout).text, expected) == 0;
}

// The probe/self-test VF layout line: what the driver returned, also when the
// snapshot is refused, so one report shows why a new family did not validate.
static int run_vf_layout_cases() {
    VfDriver driver = {};
    LinuxGpuState gpu = {};
    char why[160] = {};
    initialize(&driver, &gpu);
    if (!nvapi_read_vf_snapshot(&gpu, why, sizeof(why)) ||
        !layout_text_is(gpu, "mask=80[0..79] status=80 graphics=80[0..79] "
            "600MHz@650mV..1785MHz@1045mV unmasked=0 dropped=0") ||
        gpu.vfLayout.firstDroppedIndex != -1) return 7698;

    // A lower-voltage domain after the curve is dropped and still reported.
    initialize(&driver, &gpu);
    add_foreign_domain(&driver, 3, false);
    if (!nvapi_read_vf_snapshot(&gpu, why, sizeof(why)) ||
        gpu.numPopulated != 80 || gpu.curve[80].freq_kHz != 0) return 7699;
    if (!layout_text_is(gpu, "mask=83[0..82] status=83 graphics=80[0..79] "
            "600MHz@650mV..1785MHz@1045mV unmasked=0 "
            "dropped=3[first 80=405MHz@540mV]")) return 7700;

    // A zero-voltage foreign point does not end the run, so the snapshot is
    // refused; the layout keeps the evidence the published state cannot.
    initialize(&driver, &gpu);
    add_foreign_domain(&driver, 1, true);
    if (nvapi_read_vf_snapshot(&gpu, why, sizeof(why)) ||
        gpu.vfSnapshotFresh || gpu.numPopulated ||
        strstr(why, "point 80 out of range") == nullptr) return 7701;
    if (!layout_text_is(gpu, "mask=81[0..80] status=81 graphics=81[0..80] "
            "600MHz@650mV..405MHz@0mV unmasked=0 dropped=0")) return 7702;

    // A populated point outside the returned mask is counted.
    initialize(&driver, &gpu);
    driver.mask[0] &= (unsigned char)~(1u << 5);
    if (!nvapi_read_vf_snapshot(&gpu, why, sizeof(why)) ||
        gpu.vfLayout.graphicsUnmasked != 1 || gpu.vfLayout.maskBits != 79)
        return 7703;

    // Refusals before the status table name the stage that was reached.
    initialize(&driver, &gpu);
    memset(driver.mask, 0, sizeof(driver.mask));
    if (nvapi_read_vf_snapshot(&gpu, why, sizeof(why)) ||
        !layout_text_is(gpu, "mask=0 status=not-read")) return 7704;
    initialize(&driver, &gpu);
    driver.refusedCall = 0;
    if (nvapi_read_vf_snapshot(&gpu, why, sizeof(why)) ||
        !layout_text_is(gpu, "mask=none status=not-read")) return 7705;
    initialize(&driver, &gpu);
    driver.refusedCall = 1;
    if (nvapi_read_vf_snapshot(&gpu, why, sizeof(why)) ||
        !layout_text_is(gpu, "mask=80[0..79] status=not-read")) return 7706;
    initialize(&driver, &gpu);
    driver.changedMaskCall = 1;
    if (nvapi_read_vf_snapshot(&gpu, why, sizeof(why)) ||
        !layout_text_is(gpu, "mask=80[0..79] status=not-read")) return 7707;

    // Every read starts from an empty layout: no stale status shape.
    initialize(&driver, &gpu);
    if (!nvapi_read_vf_snapshot(&gpu, why, sizeof(why))) return 7708;
    driver.refusedCall = 0;
    if (nvapi_read_vf_snapshot(&gpu, why, sizeof(why)) ||
        gpu.vfLayout.maskValid || gpu.vfLayout.statusValid ||
        !layout_text_is(gpu, "mask=none status=not-read")) return 7708;

    // Extreme values still fit the fixed line, suffix included.
    LinuxVfLayoutSummary wide;
    linux_vf_layout_reset(&wide);
    unsigned char full[32];
    memset(full, 0xFF, sizeof(full));
    linux_vf_layout_note_mask(&wide, full, sizeof(full));
    VFCurvePoint raw[VF_NUM_POINTS] = {};
    for (int i = 0; i < VF_NUM_POINTS; ++i)
        raw[i] = {0xFFFFFFFFu, i == 64 ? 1u : 0xFFFFFFFFu};
    linux_vf_layout_note_status(&wide, raw, VF_NUM_POINTS, nullptr, 0);
    const LinuxVfLayoutText text = linux_vf_layout_text(&wide);
    const size_t length = strnlen(text.text, sizeof(text.text));
    if (wide.maskBits != 256 || wide.droppedPopulated != 64 ||
        length == 0 || length >= sizeof(text.text) ||
        text.text[length - 1] != ']' ||
        strstr(text.text, "dropped=64[first 64=4294967MHz@0mV]") == nullptr)
        return 7709;
    return 0;
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
    return run_vf_layout_cases();
}

#ifdef GC_VF_READ_TEST_MAIN
int main() {
    const int failure = run_linux_vf_read_tests();
    printf("Linux VF read regression: %d\n", failure);
    return failure;
}
#endif
