// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "profile_codec_fixture.h"

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
    return 0;
}
