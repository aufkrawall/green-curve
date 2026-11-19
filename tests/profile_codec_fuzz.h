// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "profile_codec_fixture.h"
#include "profile_slot_codec.h"
#include "profile_ini_slot.h"
#include "profile_mapping.h"
#include "profile_conversion_grid.h"
#include "profile_runtime_resolution.h"
#include "profile_curve_assistance.h"
#include "profile_pin_observation.h"
#include "curve_feasibility_message.h"
#include "desired_settings_ui_policy.h"

static void profile_codec_fuzz_round_trip(const ProfileBank& input) {
    std::string first, second;
    auto decoded = std::make_unique<ProfileBank>();
    GC_FUZZ_CHECK(profile_bank_encode(input, first), "valid bank could not be encoded");
    GC_FUZZ_CHECK(first.size() <= PROFILE_FILE_MAX_BYTES, "writer escaped file bound");
    GC_FUZZ_CHECK(profile_bank_decode(first, *decoded), "writer produced invalid document");
    GC_FUZZ_CHECK(decoded->count == input.count, "bank count changed");
    for (int i = 0; i < input.count; ++i)
        GC_FUZZ_CHECK(profile_document_semantically_equal(input.documents[i], decoded->documents[i]),
            "document meaning changed during round trip");
    GC_FUZZ_CHECK(profile_bank_encode(*decoded, second) && first == second,
        "canonical encoding changed on second pass");
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string_view raw((const char*)data, size);
    std::string configText = "unchanged", configBytes;
    if (profile_config::decode(raw, configText)) {
        GC_FUZZ_CHECK(profile_config::encode_like(raw, configText, configBytes) && configBytes == raw,
            "configuration encoding round trip changed bytes");
    } else GC_FUZZ_CHECK(configText == "unchanged", "failed encoding decode published partial text");
    auto bank = std::make_unique<ProfileBank>();
    bank->count = 1; bank->documents[0] = profile_document_make_empty();
    strcpy(bank->documents[0].displayName, "unchanged");
    if (profile_bank_decode(std::string_view((const char*)data, size), *bank)) {
        profile_codec_fuzz_round_trip(*bank);
    } else {
        GC_FUZZ_CHECK(bank->count == 1 && !strcmp(bank->documents[0].displayName, "unchanged"),
            "failed parse published partial state");
    }
    for (auto format : {PROFILE_LEGACY_WINDOWS_INI, PROFILE_LEGACY_LINUX_INI}) {
        bank->count = 1; bank->documents[0] = profile_document_make_empty();
        strcpy(bank->documents[0].displayName, "unchanged");
        ProfileLegacyOptions options{format, PROFILE_LEGACY_MEMORY_EFFECTIVE, PROFILE_LEGACY_CURVE_UNKNOWN};
        if (format == PROFILE_LEGACY_WINDOWS_INI) options.memory = PROFILE_LEGACY_MEMORY_DISPLAY;
        if (profile_legacy_ini_decode(std::string_view((const char*)data, size), options, {}, *bank))
            profile_codec_fuzz_round_trip(*bank);
        else GC_FUZZ_CHECK(bank->count == 1 && !strcmp(bank->documents[0].displayName, "unchanged"),
            "failed legacy parse published partial state");
    }
    // Reach the complete accepted shape while raw mutations explore syntax.
    FuzzInput in(data, size);
    bank->count = 1;
    auto& d = bank->documents[0];
    d = profile_codec_fixture();
    const unsigned long long bits = ((unsigned long long)in.u32() << 32) | in.u32();
    const long long signedValue = bits <= (unsigned long long)LLONG_MAX ? (long long)bits :
        LLONG_MIN + (long long)(bits - (1ull << 63));
    d.memoryOffsetHz = signedValue;
    d.graphicsOffset.offsetHz = signedValue;
    d.points[3].capturedOffsetHz = signedValue;
    d.power.ratio = {(long long)in.u32() + 1, (long long)in.u32() + 1};
    d.sourceTopologyComplete = in.byte() % 2 != 0;
    d.applicationScope = (ProfileApplicationScope)(in.byte() % 3);
    d.pointSetPolicy = (ProfilePointSetPolicy)(in.byte() % 2);
    profile_codec_fuzz_round_trip(*bank);
    std::string slot;
    auto stored = std::make_unique<ProfileDocument>();
    bool found = false;
    GC_FUZZ_CHECK(profile_slot_encode(d, 1, slot), "valid document could not be stored");
    GC_FUZZ_CHECK(profile_slot_decode(std::string_view(slot).substr(slot.find('\n') + 1), *stored, found) && found &&
        profile_document_semantically_equal(d, *stored), "local storage changed document meaning");
    stored->memoryOffsetHz = 17;
    if (!profile_slot_decode(std::string_view((const char*)data, size), *stored, found))
        GC_FUZZ_CHECK(stored->memoryOffsetHz == 17, "failed local parse published partial state");
    ProfileDestinationTopology destination;
    destination.available = destination.complete = true;
    destination.gpuGeneration = 1; destination.revision = 1; destination.count = d.topologyCount;
    for (int i = 0; i < d.topologyCount; ++i)
        destination.points[i] = {i, d.topology[i].voltageUv, true, in.byte() % 2 != 0};
    if (in.byte() % 2) destination.points[3].voltageUv = destination.points[1].voltageUv;
    const auto mapping = profile_map_topology(d, destination, 1);
    if (mapping.ready()) for (int i = 0; i < d.topologyCount; ++i) {
        if (d.points[i].kind == PROFILE_POINT_ABSENT &&
            (!d.lock.present || d.lock.anchorTopologyId != d.topology[i].topologyId)) continue;
        const int at = profile_destination_find_index(destination, mapping.destinationByTopology[i]);
        GC_FUZZ_CHECK(at >= 0 && destination.points[at].graphics && destination.points[at].writable &&
            destination.points[at].voltageUv == d.topology[i].voltageUv, "mapping guessed or accepted a read-only point");
    }
    auto runtimeSource = std::make_unique<ProfileDocument>(d);
    runtimeSource->lock = {}; runtimeSource->domains &= ~PROFILE_DOMAIN_LOCK;
    runtimeSource->graphicsOffset.offsetHz = signedValue % 1000000001LL;
    runtimeSource->memoryOffsetHz = signedValue % 3000000001LL;
    runtimeSource->points[1].absoluteFrequencyHz = 1000000000LL + in.u32() % 4000000001ULL;
    runtimeSource->points[3].capturedOffsetHz = signedValue % 1000000001LL;
    runtimeSource->xbarOffsetHz = runtimeSource->sysClkOffsetHz = runtimeSource->videoClkOffsetHz =
        (gc_i64)(in.i32() % 1000001) * 1000;
    runtimeSource->xbarMsvddOffsetUv = in.i32() % 100001;
    auto runtime = std::make_unique<DesiredSettings>();
    desired_memory_set_precise(runtime.get(), 17000000, CLOCK_OFFSET_SURFACE_GLOBAL);
    const DesiredSettings beforeResolution = *runtime;
    ProfileMappingReport resolutionMap; ProfileRuntimeRoutes routes;
    routes.graphics = DESIRED_GRAPHICS_VF_COMPONENT;
    char resolutionWhy[192]{};
    const auto sourceBeforeResolution = std::make_unique<ProfileDocument>(*runtimeSource);
    if (profile_resolve_runtime(*runtimeSource, destination, 1, routes, *runtime, resolutionMap,
            resolutionWhy, sizeof(resolutionWhy))) {
        GC_FUZZ_CHECK(desired_authoring_valid(runtime.get()) && desired_curve_intent_valid(runtime.get()) &&
            desired_scalar_intent_valid(runtime.get()), "runtime resolution published invalid authority");
        GC_FUZZ_CHECK(desired_graphics_hz(*runtime) == runtimeSource->graphicsOffset.offsetHz &&
            desired_memory_hz(*runtime) == runtimeSource->memoryOffsetHz &&
            desired_absolute_hz(*runtime, resolutionMap.destinationByTopology[1]) == runtimeSource->points[1].absoluteFrequencyHz &&
            runtime->curvePointCapturedOffsetHz[resolutionMap.destinationByTopology[3]] == runtimeSource->points[3].capturedOffsetHz,
            "runtime resolution rounded exact authority");
    } else GC_FUZZ_CHECK(desired_settings_equal(runtime.get(), &beforeResolution),
        "failed runtime resolution published partial authority");
    GC_FUZZ_CHECK(profile_document_semantically_equal(*runtimeSource, *sourceBeforeResolution),
        "runtime resolution changed its source document");
    const ProfileConversionGrid grid{true, (long long)in.u32() + 1, (long long)in.i32()};
    const auto lower = profile_grid_lower(signedValue, grid);
    if (lower.valid) {
        const auto difference = (unsigned long long)signedValue - (unsigned long long)lower.lower;
        GC_FUZZ_CHECK(lower.lower <= signedValue && difference == lower.error &&
            lower.error < (unsigned long long)grid.step && lower.exact == !lower.error,
            "grid conversion raised a clock or reported an invalid error");
        long long actual = lower.lower % grid.step, origin = grid.origin % grid.step;
        if (actual < 0) actual += grid.step;
        if (origin < 0) origin += grid.step;
        GC_FUZZ_CHECK(actual == origin, "converted value does not belong to the destination grid");
    }
    // Structured planner and pin-observation property fuzzing (overflow, invalid masks, extreme ranges, non-mutation).
    auto nextI64 = [&]() -> long long {
        const unsigned long long bits = ((unsigned long long)in.u32() << 32) | in.u32();
        return bits <= (unsigned long long)LLONG_MAX ? (long long)bits : LLONG_MIN + (long long)(bits - (1ull << 63));
    };
    VFCurvePoint fuzzCurve[VF_NUM_POINTS]{};
    int fuzzOffsetsKHz[VF_NUM_POINTS]{};
    long long fuzzFreqHz[VF_NUM_POINTS]{};
    long long fuzzOffsetsHz[VF_NUM_POINTS]{};
    auto fuzzDesired = std::make_unique<DesiredSettings>();
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        if ((in.byte() & 3) == 0) {
            fuzzCurve[i].freq_kHz = in.u32() % 6000001u;
            fuzzCurve[i].volt_uV = 400000u + (in.u32() % 1200001u);
            fuzzOffsetsKHz[i] = in.i32();
            fuzzFreqHz[i] = (in.byte() & 1) ? nextI64() : (long long)fuzzCurve[i].freq_kHz * 1000LL;
            fuzzOffsetsHz[i] = (in.byte() & 1) ? nextI64() : (long long)fuzzOffsetsKHz[i] * 1000LL;
        }
        const uint8_t dMode = in.byte();
        if ((dMode & 3) == 1) {
            fuzzDesired->hasCurvePoint[i] = 1;
            fuzzDesired->curvePointMHz[i] = in.u32() % 6001u;
        } else if ((dMode & 3) == 2) {
            fuzzDesired->curvePointCaptured[i] = 1;
            fuzzDesired->curvePointCapturedOffsetHz[i] = (in.byte() & 1) ? nextI64() : (long long)(in.i32() % 2000001) * 1000LL;
        }
    }
    fuzzDesired->hasLock = in.byte() % 2;
    fuzzDesired->lockMode = (LockMode)(in.byte() % 4);
    fuzzDesired->lockCi = (in.i32() % (VF_NUM_POINTS + 8)) - 4;
    fuzzDesired->lockMHz = in.u32() % 6001u;
    fuzzDesired->hasGpuOffset = in.byte() % 2;
    fuzzDesired->gpuOffsetMHz = in.i32() % 2001;
    const auto snapshotFuzzDesired = std::make_unique<DesiredSettings>(*fuzzDesired);

    CurvePlanInput planInput{};
    planInput.curve = fuzzCurve;
    planInput.programmedOffsetsKHz = fuzzOffsetsKHz;
    planInput.floor.valid = (in.byte() & 1) != 0;
    planInput.floor.firstPopulated = (int)(in.byte() % (VF_NUM_POINTS + 4));
    planInput.floor.floorRun = (int)(in.byte() % 8);
    planInput.floor.floorEnd = planInput.floor.floorRun;
    planInput.floor.graphicsEnd = (int)(in.byte() % (VF_NUM_POINTS + 4));
    planInput.evidence = (CurvePlanEvidence)(in.byte() % 4);
    planInput.range.minKHz = in.i32();
    planInput.range.maxKHz = in.i32();
    planInput.range.source = (CurvePlanRangeSource)(in.byte() % 3);
    planInput.tailStrategy = (CurvePlanTailStrategy)(in.byte() % 3);
    planInput.provenanceMode = (ApplyLockPretailProvenance)(in.byte() % 2);
    planInput.unownedAtStock = (in.byte() & 1) != 0;
    planInput.prefixAppliesToHardLock = (in.byte() & 1) != 0;
    planInput.desired = fuzzDesired.get();
    planInput.lockMode = (LockMode)(in.byte() % 4);
    planInput.offsetGridHz = (in.byte() & 1) ? (int)(in.u32() % 1000001u) : 0;
    if (in.byte() & 1) {
        planInput.frequencyHz = fuzzFreqHz;
        planInput.programmedOffsetsHz = fuzzOffsetsHz;
    }
    planInput.lockGridHz = (in.byte() & 1) ? 1000000 : (int)(in.u32() % 1000001u);

    const CurvePlanResult planRes = curve_plan_evaluate(planInput);
    GC_FUZZ_CHECK(planRes.issueCount >= 0 && planRes.issueCount <= CURVE_PLAN_MAX_ISSUES && planRes.omittedIssues >= 0,
        "curve planner exceeded bounded issue capacity");
    GC_FUZZ_CHECK(!memcmp(fuzzDesired.get(), snapshotFuzzDesired.get(), sizeof(DesiredSettings)),
        "curve planner mutated its DesiredSettings input");
    char planText[512]{};
    curve_plan_format_message(planRes, (in.byte() & 1) != 0, planText, sizeof(planText));
    GC_FUZZ_CHECK(planText[sizeof(planText) - 1] == '\0', "unterminated planner message");

    PinWorkloadSample obsSamples[PROFILE_PIN_OBS_MAX_SAMPLES + 2]{};
    const int nObs = (int)(in.byte() % (PROFILE_PIN_OBS_MAX_SAMPLES + 3));
    for (int i = 0; i < nObs && i < PROFILE_PIN_OBS_MAX_SAMPLES + 2; ++i) {
        obsSamples[i].clockValid = (in.byte() & 1) != 0;
        obsSamples[i].graphicsClockHz = nextI64();
        obsSamples[i].utilValid = (in.byte() & 2) != 0;
        obsSamples[i].utilGpuPct = (unsigned int)in.byte();
        obsSamples[i].perfStateValid = (in.byte() & 4) != 0;
        obsSamples[i].perfState = (unsigned int)(in.byte() % 16);
        obsSamples[i].voltageValid = (in.byte() & 8) != 0;
        obsSamples[i].operatingVoltageUv = nextI64();
        obsSamples[i].throttleValid = (in.byte() & 16) != 0;
        obsSamples[i].throttleReasons = ((unsigned long long)in.u32() << 32) | in.u32();
    }
    auto obsCap = std::make_unique<HardwareCaptureObservation>();
    obsCap->valid = obsCap->binding.valid = obsCap->topologyComplete = 1;
    obsCap->schemaVersion = HARDWARE_CAPTURE_SCHEMA_CURRENT;
    obsCap->requestedDomains = PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_LOCK;
    obsCap->capturedDomains = (in.byte() & 1) ? (PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_LOCK) : 0u;
    obsCap->pointCount = std::min<unsigned int>(4u, (unsigned int)(in.byte() % 6));
    for (gc_u32 i = 0; i < obsCap->pointCount; ++i) {
        obsCap->points[i].index = 20 + i;
        obsCap->points[i].voltageUv = 700000 + (gc_i64)i * 100000;
        obsCap->points[i].writableState = 1;
        obsCap->points[i].frequencyValid = 1;
        obsCap->points[i].frequencyHz = 1200000000LL + (gc_i64)i * 250000000LL;
    }
    const auto snapshotCap = std::make_unique<HardwareCaptureObservation>(*obsCap);
    PinObservationResult obsResult{};
    profile_pin_observation_analyze(*obsCap, (in.byte() & 2) != 0, obsSamples, nObs, obsResult);
    GC_FUZZ_CHECK(obsResult.totalSamples >= 0 && obsResult.totalSamples <= PROFILE_PIN_OBS_MAX_SAMPLES,
        "pin analyzer exceeded sample budget");
    GC_FUZZ_CHECK(!memcmp(obsCap.get(), snapshotCap.get(), sizeof(HardwareCaptureObservation)),
        "pin analyzer mutated capture observation");
    return 0;
}
