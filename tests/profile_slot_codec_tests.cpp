// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "profile_codec_fixture.h"
#include "profile_bank_edit.h"

int run_profile_slot_codec_tests() {
    auto document = std::make_unique<ProfileDocument>(profile_codec_fixture(VF_NUM_POINTS));
    auto loaded = std::make_unique<ProfileDocument>();
    std::string section, portable, roundTrip;
    ProfileCodecError error;
    bool found = false;
    if (!profile_slot_encode(*document, 3, section, &error)) return 8370;
    for (unsigned char c : section) if (c >= 128) return 8371;
    const auto body = std::string_view(section).substr(section.find('\n') + 1);
    if (!profile_slot_decode(body, *loaded, found, &error) || !found ||
        !profile_document_semantically_equal(*document, *loaded) || strcmp(document->displayName, loaded->displayName) ||
        !loaded->points[0].hasObservedOffset || loaded->points[0].observedOffsetHz != -500) return 8372;
    auto bank = std::make_unique<ProfileBank>();
    bank->count = 1; bank->documents[0] = *document;
    if (!profile_bank_encode(*bank, portable)) return 8373;
    bank->documents[0] = *loaded;
    if (!profile_bank_encode(*bank, roundTrip) || portable != roundTrip) return 8374;
    loaded->memoryOffsetHz = 42;
    const auto reject = [&](std::string text) {
        return !profile_slot_decode(text, *loaded, found, &error) && loaded->memoryOffsetHz == 42;
    };
    if (!reject("document.document_id=1\n") ||
        !reject(std::string(body) + "document.format=green_curve_profile_v1\n") ||
        !reject(std::string(body) + "document.display_name_hex=xx\n") ||
        !reject(std::string(body) + "document.display_name=\"bad\"\n") ||
        !reject(std::string(body) + "document.unknown=0\n")) return 8375;
    if (!profile_slot_decode("gpu_offset_mhz=25\n", *loaded, found) || found || loaded->memoryOffsetHz != 42) return 8376;
    bank->count = 5;
    for (int i = 0; i < 5; ++i) { bank->documents[i] = *document; bank->documents[i].documentId = (gc_u32)i + 1; }
    const int destinations[] = {1, 2, 3, 4, 5};
    ProfileBankEdit edit;
    // Unknown preferences and existing assignments are preserved byte for byte.
    const std::string original = "[profiles]\r\nlogon_slot=3\r\n[hotkeys]\r\nslot1=123\r\n[preferences]\r\nopaque=value\r\n[profile3]\r\ngpu_offset_mhz=25\r\n[profile3_curve]\r\npoint25=1000\r\n";
    if (!profile_bank_prepare_edit(original, *bank, destinations, edit, &error) ||
        edit.replacement.find("[profiles]\r\nlogon_slot=3\r\n[hotkeys]\r\nslot1=123\r\n[preferences]\r\nopaque=value\r\n") != 0 ||
        edit.replacement.find("point25=1000") != std::string::npos ||
        edit.replacement.size() + 131072 >= 1024 * 1024) return 8377;
    std::string disk = original;
    int writes = 0;
    const auto read = [&](std::string& out) { out = disk; return true; };
    const auto write = [&](const std::string& next) { ++writes; disk = next; return true; };
    if (!profile_bank_commit_edit(edit, read, write, &error) || writes != 1 || disk != edit.replacement) return 8378;
    // Fresh process, no GPU: read the committed bytes, decode and export again.
    for (int slot = 1; slot <= 5; ++slot) {
        std::string extracted;
        bool sectionFound = false;
        if (!profile_slot::extract(disk, slot, extracted, sectionFound, &error) || !sectionFound ||
            !profile_slot_decode(extracted, *loaded, found, &error) || !found ||
            !profile_document_semantically_equal(*document, *loaded) ||
            loaded->topology[0].voltageUv != 600000 || loaded->points[3].capturedOffsetHz != LLONG_MIN ||
            loaded->source.deviceId != UINT_MAX || strcmp(loaded->source.driverVersion, "1.2.3")) return 8379;
    }
    disk = original + "\n[other]\nchanged=1\n"; writes = 0;
    if (profile_bank_commit_edit(edit, read, write, &error) || writes || disk == edit.replacement) return 8380;
    disk = original;
    if (profile_bank_commit_edit(edit, read, [&](const std::string&) { ++writes; return false; }, &error) ||
        disk != original || writes != 1) return 8381;
    const int duplicate[] = {1, 1, 3, 4, 5};
    const std::string oldReplacement = edit.replacement;
    if (profile_bank_prepare_edit(original, *bank, duplicate, edit, &error) || edit.replacement != oldReplacement) return 8382;
    std::string extracted;
    if (profile_slot::extract("[profile1]\na=1\n[PROFILE1]\na=2\n", 1, extracted, found, &error)) return 8383;
    fprintf(stdout, "local profile documents: offline full-bank preservation and atomic conflict checks passed\n");
    return 0;
}
