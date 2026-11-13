// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Production capture getters with no driver loading, initialization or setters.
#include "hardware_capture_getters.h"
#include "hardware_capture_runtime.h"
#include "profile_codec.h"
#include "vf_backends.h"

namespace {
struct Driver {
    int reads[3]{}, unexpectedQueries = 0, refused = -1, malformed = -1;
    int changingControls = 0, frequencyTick = 0;
    bool badMask = false, rangeFails = false, defaultFails = false;
    bool fixedFloor = false;
};
static Driver* active = nullptr;
static int table(void* handle, void* buffer, int kind) {
    auto& d = *(Driver*)handle;
    const auto& b = g_vfBackendPascal;
    const unsigned int sizes[] = {b.infoBufferSize, b.statusBufferSize, b.controlBufferSize};
    const unsigned int masks[] = {b.infoMaskOffset, b.statusMaskOffset, b.controlMaskOffset};
    const unsigned int versions[] = {b.infoVersion, b.statusVersion, b.controlVersion};
    ++d.reads[kind];
    if (d.refused == kind) return -9;
    auto* bytes = (unsigned char*)buffer;
    unsigned int version = 0; memcpy(&version, bytes, 4);
    if (version != ((versions[kind] << 16) | sizes[kind])) return -99;
    memset(bytes, 0, sizes[kind]);
    if (d.malformed == kind) version ^= 1u << 16;
    memcpy(bytes, &version, 4);
    bytes[masks[kind]] = kind && d.badMask ? 1 : d.fixedFloor ? 31 : 7;
    for (unsigned int i = 0; i < (d.fixedFloor ? 5u : 3u); ++i) {
        if (kind == 1) {
            const unsigned int at = b.statusEntriesOffset + i * b.statusEntryStride;
            const unsigned int freq = d.fixedFloor ? (i < 4 ? 360000 : 400000) :
                2000001 + i * 30000 + (unsigned int)d.frequencyTick++;
            const unsigned int voltage = (d.fixedFloor ? 700000 : 650000) + i * 6250;
            memcpy(bytes + at, &freq, 4); memcpy(bytes + at + 4, &voltage, 4);
        } else if (kind == 2) {
            const unsigned int at = b.controlEntryBaseOffset + i * b.controlEntryStride + b.controlEntryDeltaOffset;
            const int offset = d.fixedFloor ? 0 : i == 0 ? 30001 + (d.changingControls ? d.reads[2] : 0) : i == 1 ? -30001 : 0;
            memcpy(bytes + at, &offset, 4);
        }
    }
    return 0;
}
static int info(void* h, void* b) { return table(h, b, 0); }
static int status(void* h, void* b) { return table(h, b, 1); }
static int control(void* h, void* b) { return table(h, b, 2); }
static void* query(unsigned int id) {
    if (id == g_vfBackendPascal.getInfoId) return (void*)info;
    if (id == g_vfBackendPascal.getStatusId) return (void*)status;
    if (id == g_vfBackendPascal.getControlId) return (void*)control;
    ++active->unexpectedQueries;
    return nullptr;
}
static nvmlReturn_t clock(nvmlDevice_t, nvmlClockOffset_t* v) {
    v->clockOffsetMHz = v->type == NVML_CLOCK_MEM ? -1 : 0;
    v->minClockOffsetMHz = active->rangeFails ? 3000 : -3000;
    v->maxClockOffsetMHz = 2000;
    return NVML_SUCCESS;
}
static nvmlReturn_t power(nvmlDevice_t, unsigned int* v) { *v = 249000; return NVML_SUCCESS; }
static nvmlReturn_t baseline(nvmlDevice_t, unsigned int* v) {
    *v = 250000; return active->defaultFails ? NVML_ERROR_NOT_SUPPORTED : NVML_SUCCESS;
}
static nvmlReturn_t constraints(nvmlDevice_t, unsigned int* low, unsigned int* high) {
    *low = 100000; *high = 300000; return NVML_SUCCESS;
}
static bool pin(void*, bool& present, gc_i64& low, gc_i64& high, int& result) {
    present = true; low = high = 1000000500; result = 0; return true;
}
static HardwareCaptureContext context(Driver& driver, const NvmlApi& api) {
    active = &driver;
    HardwareCaptureContext c{};
    c.usable = c.nvmlUsable = true; c.gpuHandle = &driver; c.query = query;
    c.backend = &g_vfBackendPascal; c.nvml = &api; c.nvmlDevice = (nvmlDevice_t)&driver;
    c.binding.valid = c.binding.pciInfoValid = 1; c.binding.deviceId = 0x1234;
    c.gpuGeneration = 4; c.osFamily = PROFILE_SOURCE_OS_WINDOWS;
    c.backendFamily = GPU_FAMILY_PASCAL; snprintf(c.driverVersion, sizeof(c.driverVersion), "%s", "fixture");
    return c;
}
static bool collect(const HardwareCaptureContext& c, gc_u32 requested,
    HardwareCaptureObservation& o, char* why, size_t size) {
    return hardware_capture_collect(c, requested, [](const auto&, auto&) {}, o, why, size);
}
}

#include "hardware_capture_constraints_cases.h"
int run_hardware_capture_tests() {
    if (int failure = hardware_capture_constraints_cases()) return failure;
    static_assert(sizeof(HardwareCaptureObservation) == 4864, "capture wire size changed");
    Driver driver{};
    NvmlApi api{}; api.getClockOffsets = clock; api.getPowerLimit = power;
    api.getPowerDefaultLimit = baseline; api.getPowerConstraints = constraints;
    auto c = context(driver, api);
    constexpr gc_u32 requested = PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_GRAPHICS_OFFSET |
        PROFILE_DOMAIN_MEMORY | PROFILE_DOMAIN_POWER | PROFILE_DOMAIN_LOCK;
    auto observation = std::make_unique<HardwareCaptureObservation>();
    char why[192]{};
    const auto read = [&](auto& o, char* reason, size_t size) { return collect(c, requested, o, reason, size); };
    if (!hardware_capture_consistent(read, *observation, why, sizeof(why)) ||
        observation->points[0].offsetHz != 15000500 || observation->points[1].offsetHz != -15000500 ||
        observation->points[2].offsetHz != 0 || observation->points[0].frequencyHz != 1000002000 ||
        observation->readAttempts != 1 || !observation->revision || driver.unexpectedQueries ||
        driver.reads[0] != 2 || driver.reads[1] != 2 || driver.reads[2] != 2) return 8480;
    if (observation->memory.value != -500000 || !observation->memory.valid ||
        observation->graphics.value != 0 || !observation->graphics.valid ||
        observation->pinGetterValid || observation->pinPresent ||
        observation->omittedDomains != (PROFILE_DOMAIN_GRAPHICS_OFFSET | PROFILE_DOMAIN_LOCK)) return 8481;
    auto document = std::make_unique<ProfileDocument>();
    if (!hardware_capture_to_document(*observation, *document, why, sizeof(why)) ||
        document->applicationScope != PROFILE_SCOPE_UPDATE_INCLUDED ||
        document->points[2].kind != PROFILE_POINT_CAPTURED_OFFSET || document->points[2].capturedOffsetHz ||
        document->graphicsOffset.present || !document->graphicsOffset.hasObservedOffset ||
        document->graphicsOffset.observedOffsetHz || document->source.pinKnowledge != PROFILE_PIN_UNKNOWN ||
        document->memoryOffsetHz != -500000 || document->power.ratio.numerator != 249 ||
        document->power.ratio.denominator != 250 || document->lock.present) return 8482;
    auto bank = std::make_unique<ProfileBank>(); bank->count = 1; bank->documents[0] = *document;
    std::string text; auto decoded = std::make_unique<ProfileBank>();
    if (!profile_bank_encode(*bank, text) ||
        text.find("graphics_observation") == std::string::npos ||
        !profile_bank_decode(text, *decoded) || decoded->count != 1 ||
        !decoded->documents[0].graphicsOffset.hasObservedOffset ||
        decoded->documents[0].graphicsOffset.observedComposition != CAPTURE_COMPOSITION_UNKNOWN ||
        !profile_document_semantically_equal(*document, decoded->documents[0])) return 8483;
    const auto stable = *observation;
    const auto fingerprint = hardware_capture_fingerprint(stable);
    observation->points[0].frequencyHz++;
    if (hardware_capture_fingerprint(*observation) != fingerprint) return 8484;
    observation->points[0].offsetHz++;
    if (hardware_capture_fingerprint(*observation) == fingerprint) return 8485;
    *observation = stable; driver.changingControls = 1;
    const int prior = driver.reads[0];
    if (hardware_capture_consistent(read, *observation, why, sizeof(why)) ||
        driver.reads[0] != prior + 6 || memcmp(observation.get(), &stable, sizeof(stable))) return 8486;
    driver.changingControls = 0; c.usable = false;
    const int before = driver.reads[0];
    if (hardware_capture_consistent(read, *observation, why, sizeof(why)) ||
        driver.reads[0] != before || memcmp(observation.get(), &stable, sizeof(stable))) return 8487;
    c.usable = true;
    for (int kind = 0; kind < 3; ++kind) {
        driver.refused = kind;
        if (!collect(c, requested, *observation, why, sizeof(why)) ||
            !hardware_capture_observation_valid(*observation) || observation->pointCount ||
            (observation->capturedDomains & PROFILE_DOMAIN_VF_CURVE) ||
            observation->driverStatus[0] != -9) return 8488;
        driver.refused = -1; driver.malformed = kind;
        if (!collect(c, requested, *observation, why, sizeof(why)) || observation->pointCount ||
            !hardware_capture_observation_valid(*observation)) return 8489;
        driver.malformed = -1;
    }
    driver.badMask = true;
    if (!collect(c, requested, *observation, why, sizeof(why)) || observation->pointCount) return 8490;
    driver.badMask = false; driver.rangeFails = driver.defaultFails = true;
    if (!collect(c, requested, *observation, why, sizeof(why)) ||
        !hardware_capture_observation_valid(*observation) || !observation->memory.valid ||
        observation->memory.rangeValid || observation->power.defaultValid || !observation->power.currentValid ||
        !hardware_capture_to_document(*observation, *document, why, sizeof(why)) ||
        document->power.present || !document->power.hasObservedCurrentMw ||
        document->power.hasObservedDefaultMw) return 8491;
    driver.rangeFails = driver.defaultFails = false;
    c.composition = CAPTURE_COMPOSITION_INDEPENDENT; c.pinGetter = pin;
    if (!collect(c, requested, *observation, why, sizeof(why)) || observation->omittedDomains ||
        !hardware_capture_to_document(*observation, *document, why, sizeof(why)) ||
        !document->graphicsOffset.present || document->source.pinKnowledge != PROFILE_PIN_PRESENT ||
        !document->lock.hasObservedPinRange || document->lock.present ||
        document->lock.observedPinMinimumHz != 1000000500) return 8492;
    c.composition = CAPTURE_COMPOSITION_ALIASED;
    if (!collect(c, requested, *observation, why, sizeof(why)) || observation->omittedDomains ||
        !hardware_capture_to_document(*observation, *document, why, sizeof(why)) ||
        document->graphicsOffset.present || document->points[0].kind != PROFILE_POINT_CAPTURED_OFFSET) return 8493;
    if (!collect(c, PROFILE_DOMAIN_FAN, *observation, why, sizeof(why)) || observation->fan.valid ||
        observation->capturedDomains || observation->unavailableDomains != PROFILE_DOMAIN_FAN) return 8494;
    DesiredSettings owned{}; owned.hasFan = 1; owned.fanMode = FAN_MODE_AUTO;
    fan_curve_set_default(&owned.fanCurve); c.ownedFanIntent = &owned;
    if (!collect(c, PROFILE_DOMAIN_FAN, *observation, why, sizeof(why)) ||
        !hardware_capture_observation_valid(*observation) || !observation->fan.valid ||
        !hardware_capture_to_document(*observation, *document, why, sizeof(why)) || !document->fan.present) return 8495;
    observation->fan.pollIntervalMs = UINT_MAX;
    if (hardware_capture_observation_valid(*observation)) return 8496;
    *observation = stable; observation->reserved[0] = 1;
    if (hardware_capture_observation_valid(*observation)) return 8497;
    *observation = stable; observation->points[1].index = observation->points[0].index;
    if (hardware_capture_observation_valid(*observation)) return 8498;
    *observation = stable; observation->capturedDomains ^= PROFILE_DOMAIN_MEMORY;
    if (hardware_capture_observation_valid(*observation)) return 8499;
    *observation = {}; observation->memory.value = 1;
    if (hardware_capture_observation_valid(*observation)) return 8500;
    *observation = {}; if (!hardware_capture_observation_valid(*observation)) return 8501;
    ServiceRequest request{};
    request.magic = SERVICE_PROTOCOL_MAGIC; request.version = SERVICE_PROTOCOL_VERSION;
    request.callerPid = 1; request.command = SERVICE_CMD_CAPTURE_CURRENT; request.captureDomains = requested;
    if (!validate_service_request_for_ipc(&request)) return 8502;
    const auto valid = request;
    request.captureDomains = 0; if (validate_service_request_for_ipc(&request)) return 8503;
    request = valid; request.captureDomains = 512; if (validate_service_request_for_ipc(&request)) return 8504;
    request = valid; request.path[0] = 'x'; if (validate_service_request_for_ipc(&request)) return 8505;
    request = valid; request.desired.hasMemOffset = 1; if (validate_service_request_for_ipc(&request)) return 8506;
    request = valid; request.desired.memOffsetMHz = 1; if (validate_service_request_for_ipc(&request)) return 8507;
    request = valid; request.flags = SERVICE_REQUEST_FLAG_INTERACTIVE; if (validate_service_request_for_ipc(&request)) return 8508;
    request = valid; request.operationId = 1; if (validate_service_request_for_ipc(&request)) return 8509;
    request = valid; request.command = SERVICE_CMD_PING; if (validate_service_request_for_ipc(&request)) return 8510;
    auto response = std::make_unique<ServiceResponse>();
    response->magic = SERVICE_PROTOCOL_MAGIC; response->version = SERVICE_PROTOCOL_VERSION;
    response->status = SERVICE_STATUS_ERROR;
    response->outcomeSeverity = service_response_resolve_outcome_severity(response->status, 0);
    if (!validate_service_response_for_ipc(response.get())) return 8511;
    response->capture.memory.value = 1;
    if (validate_service_response_for_ipc(response.get()) || service_response_payload_is_absent(response.get())) return 8512;
    response->capture = stable;
    if (validate_service_response_for_ipc(response.get())) return 8513;
    driver = {}; driver.fixedFloor = true; c = context(driver, api);
    const auto floorRead = [&](auto& o, char* reason, size_t size) {
        return collect(c, PROFILE_DOMAIN_VF_CURVE, o, reason, size);
    };
    if (!hardware_capture_consistent(floorRead, *observation, why, sizeof(why)) || observation->pointCount != 5 ||
        !hardware_capture_to_document(*observation, *document, why, sizeof(why))) return 8695;
    for (int i = 0; i < 4; ++i) if (observation->points[i].writableState != 2 ||
        document->points[i].kind != PROFILE_POINT_ABSENT) return 8696;
    if (observation->points[4].writableState != 1 ||
        document->points[4].kind != PROFILE_POINT_CAPTURED_OFFSET || document->points[4].capturedOffsetHz ||
        driver.unexpectedQueries) return 8697;
    return 0;
}
