// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included after the production capture fixture dependencies.
namespace capture_clock_surfaces {
struct Script {
    Driver driver;
    int globalGraphics = 17, globalMemory = -247;
    int modernReads = 0, graphicsReads = 0, memoryReads = 0, setters = 0;
    bool failModern = false, failGlobal = false, changingGraphics = false;
};
static Script* script;
static nvmlReturn_t modern(nvmlDevice_t, nvmlClockOffset_t* value) {
    ++script->modernReads;
    if (script->failModern) return NVML_ERROR_NOT_SUPPORTED;
    value->clockOffsetMHz = value->type == NVML_CLOCK_MEM ? -1 : 0;
    value->minClockOffsetMHz = -1000; value->maxClockOffsetMHz = 2000;
    return NVML_SUCCESS;
}
static nvmlReturn_t graphics(nvmlDevice_t, int* value) {
    ++script->graphicsReads;
    *value = script->globalGraphics + (script->changingGraphics ? script->graphicsReads : 0);
    return script->failGlobal ? NVML_ERROR_NOT_SUPPORTED : NVML_SUCCESS;
}
static nvmlReturn_t memory(nvmlDevice_t, int* value) {
    ++script->memoryReads; *value = script->globalMemory;
    return script->failGlobal ? NVML_ERROR_NOT_SUPPORTED : NVML_SUCCESS;
}
static nvmlReturn_t range(nvmlDevice_t, int* low, int* high) {
    *low = -1000; *high = 2000; return NVML_SUCCESS;
}
static nvmlReturn_t set_modern(nvmlDevice_t, nvmlClockOffset_t*) { ++script->setters; return NVML_ERROR_UNKNOWN; }
static nvmlReturn_t set_global(nvmlDevice_t, int) { ++script->setters; return NVML_ERROR_UNKNOWN; }
static int run() {
    Script state{}; script = &state;
    NvmlApi api{}; api.getClockOffsets = modern; api.setClockOffsets = set_modern;
    api.getGpcClkVfOffset = graphics; api.getMemClkVfOffset = memory;
    api.getGpcClkMinMaxVfOffset = api.getMemClkMinMaxVfOffset = range;
    api.setGpcClkVfOffset = api.setMemClkVfOffset = set_global;
    auto c = context(state.driver, api); c.composition = CAPTURE_COMPOSITION_INDEPENDENT;
    constexpr gc_u32 domains = PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_GRAPHICS_OFFSET | PROFILE_DOMAIN_MEMORY;
    HardwareCaptureObservation o{}; char why[192]{};
    auto read = [&](auto& out, char* reason, size_t size) { return collect(c, domains, out, reason, size); };
    if (!hardware_capture_consistent(read, o, why, sizeof(why)) || state.setters ||
        o.graphics.surface != CLOCK_OFFSET_SURFACE_P0 || o.graphics.value ||
        o.graphicsAlternate.surface != CLOCK_OFFSET_SURFACE_GLOBAL || o.graphicsAlternate.value != 17000000 ||
        o.memory.value != -500000 || o.memoryAlternate.value != -123500000 ||
        !o.graphicsAlternate.writable || !o.memoryAlternate.rangeValid ||
        o.domainStatus[1] != CAPTURE_PARTIAL || o.domainStatus[2] != CAPTURE_PARTIAL) return 9100;
    const auto hash = hardware_capture_fingerprint(o);
    auto changed = o; ++changed.graphicsAlternate.value;
    if (!hardware_capture_observation_valid(changed) || hardware_capture_fingerprint(changed) == hash) return 9101;
    for (int field = 0; field < 4; ++field) {
        changed = o;
        if (field == 0) changed.graphicsAlternate.valid = 2;
        if (field == 1) changed.graphicsAlternate.surface = o.graphics.surface;
        if (field == 2) changed.memoryAlternate.reserved = 1;
        if (field == 3) changed.graphicsAlternate.surface = 0;
        if (hardware_capture_observation_valid(changed)) return 9102 + field;
    }
    state.failModern = true;
    if (!hardware_capture_consistent(read, o, why, sizeof(why)) ||
        o.graphics.surface != CLOCK_OFFSET_SURFACE_GLOBAL || o.graphics.value != 17000000 ||
        o.graphicsAlternate.surface != CLOCK_OFFSET_SURFACE_P0 || o.graphicsAlternate.valid ||
        o.memory.surface != CLOCK_OFFSET_SURFACE_GLOBAL || o.memory.value != -123500000 || state.setters) return 9106;
    state.failModern = false; state.failGlobal = true;
    if (!hardware_capture_consistent(read, o, why, sizeof(why)) || !o.graphics.valid ||
        o.graphicsAlternate.valid || !o.graphicsAlternate.rangeValid ||
        o.graphicsAlternate.status != NVML_ERROR_NOT_SUPPORTED || state.setters) return 9107;
    state.failGlobal = false;
    if (!collect(c, PROFILE_DOMAIN_VF_CURVE, o, why, sizeof(why)) ||
        !hardware_capture_bytes_zero(&o.memoryAlternate, sizeof(o.memoryAlternate))) return 9108;
    o.memoryAlternate = o.graphicsAlternate;
    if (hardware_capture_observation_valid(o)) return 9109;
    state.changingGraphics = true; state.graphicsReads = state.memoryReads = 0;
    if (hardware_capture_consistent(read, o, why, sizeof(why)) || state.graphicsReads != 6 ||
        state.memoryReads != 6 || state.setters || !strstr(why, "changed")) return 9110;
    return 0;
}
}
