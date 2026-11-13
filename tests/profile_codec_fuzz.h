// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "profile_codec_fixture.h"
#include "profile_slot_codec.h"
#include "profile_mapping.h"
#include "profile_conversion_grid.h"

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
    auto bank = std::make_unique<ProfileBank>();
    bank->count = 1; bank->documents[0] = profile_document_make_empty();
    strcpy(bank->documents[0].displayName, "unchanged");
    if (profile_bank_decode(std::string_view((const char*)data, size), *bank)) {
        profile_codec_fuzz_round_trip(*bank);
    } else {
        GC_FUZZ_CHECK(bank->count == 1 && !strcmp(bank->documents[0].displayName, "unchanged"),
            "failed parse published partial state");
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
    return 0;
}
