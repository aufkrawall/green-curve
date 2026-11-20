// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Memory offset evidence from the running memory clock
// (source/memory_clock_evidence.h) and what passive capture reports from it,
// assertion codes 12306-12319. The reported case (RTX 5070): another tool set
// +3000 MHz through Pstates20, so the P0 memory clock maximum read 17001 MHz
// against a 14001 MHz board maximum while both NVML getter surfaces read 0,
// and Import current GPU settings showed memory as stock.

#include <memory>
#include <string.h>
#include <string>

#include "hardware_capture_getters.h"
#include "hardware_capture_runtime.h"
#include "profile_summary.h"

namespace {

constexpr unsigned int kBoardMaxMHz = 14001;

struct Fixture {
    unsigned int memoryMaxKHz = 17001000;  // P0 memory maximum
    int ver3Status = 0, ver2Status = 0;
    int calls = 0, ver2Calls = 0;
    nvmlReturn_t boardStatus = NVML_SUCCESS;
    int getterEffectiveMHz = 0;  // NVML offset getter, effective units
};
static Fixture* g_fixture = nullptr;

void fill_pstates(nvapiPerfPstates20Info_t& info, unsigned int memoryMaxKHz, bool range) {
    const unsigned int version = info.version;
    info = {};
    info.version = version;
    info.numPstates = 2;
    info.numClocks = 2;
    info.pstates[0].pstateId = 0;
    info.pstates[0].clocks[0].domainId = NVAPI_GPU_PUBLIC_CLOCK_GRAPHICS;
    info.pstates[0].clocks[0].typeId = NVAPI_GPU_PERF_PSTATE20_CLOCK_TYPE_RANGE;
    info.pstates[0].clocks[0].data.range.maxFreq_kHz = 99000000;  // never counted as memory
    info.pstates[0].clocks[1].domainId = NVAPI_GPU_PUBLIC_CLOCK_MEMORY;
    info.pstates[0].clocks[1].typeId = range ? NVAPI_GPU_PERF_PSTATE20_CLOCK_TYPE_RANGE
                                             : NVAPI_GPU_PERF_PSTATE20_CLOCK_TYPE_SINGLE;
    if (range) info.pstates[0].clocks[1].data.range.maxFreq_kHz = memoryMaxKHz;
    else info.pstates[0].clocks[1].data.single.freq_kHz = memoryMaxKHz;
    info.pstates[1].pstateId = 8;
    info.pstates[1].clocks[1].domainId = NVAPI_GPU_PUBLIC_CLOCK_MEMORY;
    info.pstates[1].clocks[1].typeId = NVAPI_GPU_PERF_PSTATE20_CLOCK_TYPE_SINGLE;
    info.pstates[1].clocks[1].data.single.freq_kHz = 810000;
}

int get_pstates(void*, void* buffer) {
    auto& f = *g_fixture;
    ++f.calls;
    auto* info = (nvapiPerfPstates20Info_t*)buffer;
    const bool ver3 = info->version == NVAPI_PERF_PSTATES20_INFO_VER3;
    if (!ver3) ++f.ver2Calls;
    const int status = ver3 ? f.ver3Status : f.ver2Status;
    if (!status) fill_pstates(*info, f.memoryMaxKHz, false);
    return status;
}
void* pstates_query(unsigned int id) { return id == NVAPI_GPU_GET_PSTATES20_ID ? (void*)get_pstates : nullptr; }
void* no_query(unsigned int) { return nullptr; }

nvmlReturn_t board_max(nvmlDevice_t, unsigned int domain, unsigned int* mhz) {
    if (domain != NVML_CLOCK_MEM) return NVML_ERROR_INVALID_ARGUMENT;
    *mhz = kBoardMaxMHz;
    return g_fixture->boardStatus;
}
nvmlReturn_t offsets(nvmlDevice_t, nvmlClockOffset_t* v) {
    v->clockOffsetMHz = v->type == NVML_CLOCK_MEM ? g_fixture->getterEffectiveMHz : 0;
    v->minClockOffsetMHz = -4000;
    v->maxClockOffsetMHz = 6000;
    return NVML_SUCCESS;
}

HardwareCaptureContext context(Fixture& f, const NvmlApi& api, bool runningClock) {
    g_fixture = &f;
    HardwareCaptureContext c{};
    c.usable = c.nvmlUsable = true;
    c.gpuHandle = &f;
    c.query = pstates_query;
    c.nvml = &api;
    c.nvmlDevice = (nvmlDevice_t)&f;
    c.binding.valid = c.binding.pciInfoValid = 1;
    c.binding.deviceId = 0x2F04;
    c.gpuGeneration = 7;
    c.osFamily = PROFILE_SOURCE_OS_WINDOWS;
    c.backendFamily = GPU_FAMILY_BLACKWELL;
    snprintf(c.driverVersion, sizeof(c.driverVersion), "%s", "fixture");
    if (runningClock) c.memoryClockReader = hardware_capture_memory_clock;
    return c;
}

bool capture(const HardwareCaptureContext& c, HardwareCaptureObservation& o) {
    char why[192]{};
    const auto read = [&](HardwareCaptureObservation& out, char* reason, size_t size) {
        return hardware_capture_collect(c, PROFILE_DOMAIN_MEMORY, [](const auto&, auto&) {}, out, reason, size);
    };
    return hardware_capture_consistent(read, o, why, sizeof(why));
}

static int g_observed = 0;
static gc_i64 g_observedGetterHz = -1;
void observe(const MemoryClockReading&, gc_i64 getterHz, const HardwareCaptureObservation&) {
    ++g_observed;
    g_observedGetterHz = getterHz;
}

}  // namespace

int run_capture_memory_clock_tests() {
    // The running maximum: the highest memory clock of any pstate, kHz
    // truncated to MHz, single or range entries, graphics never counted.
    auto info = std::make_unique<nvapiPerfPstates20Info_t>();
    fill_pstates(*info, 17001999, false);
    if (pstates20_memory_max_mhz(*info) != 17001) return 12306;
    fill_pstates(*info, 17001000, true);
    if (pstates20_memory_max_mhz(*info) != 17001) return 12306;
    info->numPstates = 1000;  // clamped, never read past the table
    info->numClocks = 1000;
    if (pstates20_memory_max_mhz(*info) != 17001) return 12306;
    info->numPstates = 0;
    if (pstates20_memory_max_mhz(*info) != 0) return 12306;

    // Offset = running maximum - board maximum, display MHz; unknown either way
    // is no reading.
    int khz = 0;
    if (!memory_clock_effective_offset_khz(17001, kBoardMaxMHz, &khz) || khz != 3000000) return 12307;
    if (!memory_clock_effective_offset_khz(13001, kBoardMaxMHz, &khz) || khz != -1000000) return 12307;
    if (memory_clock_effective_offset_khz(0, kBoardMaxMHz, &khz) ||
        memory_clock_effective_offset_khz(17001, 0, &khz))
        return 12307;

    // The getter-only reader: VER3 refused falls back to VER2; no Pstates20
    // or no board maximum is no reading.
    Fixture f;
    NvmlApi api{};
    api.getClockOffsets = offsets;
    api.getMaxClock = board_max;
    g_fixture = &f;
    f.ver3Status = -9;
    MemoryClockReading r = memory_clock_read(pstates_query, &f, &api, (nvmlDevice_t)&f);
    gc_i64 hz = 0;
    if (!r.pstatesValid || !r.boardValid || r.runningMaxMHz != 17001 || f.ver2Calls != 1 ||
        !memory_clock_reading_offset_hz(r, &hz) || hz != 3000000000LL)
        return 12308;
    f.ver3Status = 0;
    r = memory_clock_read(no_query, &f, &api, (nvmlDevice_t)&f);
    if (r.pstatesValid || r.pstatesStatus != -100001 || memory_clock_reading_offset_hz(r, &hz)) return 12308;
    f.boardStatus = NVML_ERROR_NOT_SUPPORTED;
    r = memory_clock_read(pstates_query, &f, &api, (nvmlDevice_t)&f);
    if (r.boardValid || memory_clock_reading_offset_hz(r, &hz)) return 12308;
    f.boardStatus = NVML_SUCCESS;

    // The reported case through the production collector: the getter reads 0,
    // the running clock +3000 MHz, so capture reports +3000 MHz.
    auto o = std::make_unique<HardwareCaptureObservation>();
    auto c = context(f, api, true);
    c.memoryObserved = observe;
    if (!capture(c, *o) || !o->memory.valid || o->memory.value != 3000000000LL ||
        o->memoryEvidence != CAPTURE_MEMORY_EVIDENCE_RUNNING_CLOCK ||
        o->schemaVersion != HARDWARE_CAPTURE_SCHEMA_CURRENT ||
        !(o->capturedDomains & PROFILE_DOMAIN_MEMORY) || o->omittedDomains ||
        o->memory.surface != CLOCK_OFFSET_SURFACE_P0 || !hardware_capture_observation_valid(*o))
        return 12309;
    if (!g_observed || g_observedGetterHz != 0) return 12309;
    const auto summary = profile_capture_summary(*o, nullptr);
    if (summary.memory.text != "+3000 MHz" || !summary.memory.changed) return 12310;
    char line[512]{};
    hardware_capture_summary(*o, line, sizeof(line));
    if (!strstr(line, "memory=3000000000Hz(running-clock)")) return 12310;

    // Without a running-clock reader (Linux, whose live state reads NVML only)
    // the getter stands alone, as before.
    auto plain = context(f, api, false);
    if (!capture(plain, *o) || o->memory.value != 0 || o->memoryEvidence != CAPTURE_MEMORY_EVIDENCE_GETTER) return 12311;

    // Agreement keeps the getter's finer value: +3000.5 MHz (6001 effective)
    // against a running reading truncated to 3000 MHz.
    f.getterEffectiveMHz = 6001;
    if (!capture(c, *o) || o->memory.value != 3000500000LL || o->memoryEvidence != CAPTURE_MEMORY_EVIDENCE_AGREES)
        return 12312;
    // A stale getter after another tool reset memory: the running clock (0) wins.
    f.memoryMaxKHz = kBoardMaxMHz * 1000;
    if (!capture(c, *o) || o->memory.value != 0 || o->memoryEvidence != CAPTURE_MEMORY_EVIDENCE_RUNNING_CLOCK)
        return 12313;
    // No running reading: the getter's value, marked as such.
    f.memoryMaxKHz = 17001000;
    f.ver3Status = f.ver2Status = -5;
    if (!capture(c, *o) || o->memory.value != 3000500000LL || o->memoryEvidence != CAPTURE_MEMORY_EVIDENCE_GETTER)
        return 12314;
    f.ver3Status = f.ver2Status = 0;
    f.getterEffectiveMHz = 0;

    // Reset uses the same resolution on signed physical kHz. A failed
    // getter cannot preserve a cached fraction and a full MHz wins outright.
    if (memory_clock_resolve_live_khz(0, true, 500) != 500 ||
        memory_clock_resolve_live_khz(-1000, true, -500) != -500 ||
        memory_clock_resolve_live_khz(0, false, 500) != 0 ||
        memory_clock_resolve_live_khz(0, true, 1000) != 0)
        return 12706;

    // A failed getter keeps the domain unread: replay writes the getter's
    // surface, so the running clock alone is not a capture.
    HardwareCaptureObservation unread{};
    unread.memory.value = 0;
    MemoryClockReading running{};
    running.pstatesValid = running.boardValid = true;
    running.runningMaxMHz = 17001;
    running.boardMaxMHz = kBoardMaxMHz;
    hardware_capture_apply_memory_clock(running, unread);
    if (unread.memory.valid || unread.memory.value || unread.memoryEvidence != CAPTURE_MEMORY_EVIDENCE_GETTER) return 12315;

    // Wire validation: the evidence byte is bounded and needs a read memory
    // domain; other schemas are refused.
    if (!capture(c, *o)) return 12316;
    auto bad = std::make_unique<HardwareCaptureObservation>(*o);
    bad->memoryEvidence = 3;
    if (hardware_capture_observation_valid(*bad)) return 12316;
    *bad = *o;
    bad->memory = {};
    bad->domainStatus[2] = CAPTURE_READ_FAILED;
    bad->capturedDomains &= ~(gc_u32)PROFILE_DOMAIN_MEMORY;
    bad->omittedDomains |= PROFILE_DOMAIN_MEMORY;
    if (hardware_capture_observation_valid(*bad)) return 12317;
    bad->memoryEvidence = CAPTURE_MEMORY_EVIDENCE_GETTER;
    if (!hardware_capture_observation_valid(*bad)) return 12317;
    *bad = *o;
    bad->schemaVersion = 4;
    if (hardware_capture_observation_valid(*bad)) return 12318;

    // The saved document carries the running offset on the getter's surface,
    // the control replay writes.
    auto document = std::make_unique<ProfileDocument>();
    char why[192]{};
    if (!hardware_capture_to_document(*o, *document, why, sizeof(why)) || !document->hasMemoryOffset ||
        document->memoryOffsetHz != 3000000000LL || document->memorySurface != CLOCK_OFFSET_SURFACE_P0)
        return 12319;
    return 0;
}
