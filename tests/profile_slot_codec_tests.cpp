// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "profile_codec_fixture.h"
#include "profile_bank_edit.h"
#if defined(_WIN32)
#include "app_shared.h"
#include "profile_config_windows.h"
#endif

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
    // A Unicode bank keeps its original encoding, opaque preferences and exact
    // raw generation while only the selected slot is replaced.
    const std::u16string prefix16 = u"[profiles]\r\nlogon_slot=3\r\n[preferences]\r\nopaque=\u66f2\U0001f600\r\n"
        u"[profile4]\r\nmem_offset_mhz=1\r\n";
    std::string prefixBytes("\xff\xfe", 2);
    for (char16_t c : prefix16) { prefixBytes += (char)(c & 255); prefixBytes += (char)(c >> 8); }
    const std::string prefix8 = "[profiles]\r\nlogon_slot=3\r\n[preferences]\r\nopaque=\xe6\x9b\xb2\xf0\x9f\x98\x80\r\n"
        "[profile4]\r\nmem_offset_mhz=1\r\n";
    std::string decodedText, encodedText;
    if (!profile_config::decode(prefixBytes, decodedText, &error) || decodedText != prefix8 ||
        !profile_config::encode_like(prefixBytes, decodedText, encodedText, &error) || encodedText != prefixBytes) return 9018;
    const std::string original16 = prefixBytes + std::string("[\0p\0r\0o\0f\0i\0l\0e\0""3\0]\0\n\0", 22);
    bank->count = 1; bank->documents[0] = profile_codec_fixture();
    const int unicodeSlot[] = {3};
    if (!profile_bank_prepare_edit(original16, *bank, unicodeSlot, edit, &error) ||
        edit.previous != original16 || edit.replacement.compare(0, prefixBytes.size(), prefixBytes) ||
        !profile_config::decode(edit.replacement, decodedText, &error) ||
        !profile_slot::extract(decodedText, 3, extracted, found, &error) || !found ||
        !profile_slot_decode(extracted, *loaded, found, &error) || !found ||
        !profile_document_semantically_equal(bank->documents[0], *loaded)) return 9019;
    disk = original16; writes = 0;
    if (!profile_bank_commit_edit(edit, read, write, &error) || writes != 1 || disk != edit.replacement) return 9020;
    disk = original16 + std::string("x\0", 2); writes = 0;
    if (profile_bank_commit_edit(edit, read, write, &error) || writes) return 9021;
    const std::string prepared16 = edit.replacement;
    decodedText = "unchanged";
    const std::string malformed[] = {std::string("\xff\xfe\0\xd8", 4), std::string("\xff\xfe\0\xdc", 4),
        std::string("\xff\xfe\0\xd8x\0", 6), std::string("\xff\xfe\0", 3), std::string("\xfe\xff", 2),
        std::string("\xff\xfe\0\0", 4), std::string("x\0", 2)};
    for (const auto& bytes : malformed) {
        if (profile_config::decode(bytes, decodedText, &error) || decodedText != "unchanged") return 9022;
        if (profile_bank_prepare_edit(bytes, *bank, unicodeSlot, edit, &error) ||
            edit.previous != original16 || edit.replacement != prepared16) return 9023;
    }
    encodedText = "unchanged";
    for (const auto& bytes : {std::string("\xc0\x80", 2), std::string("\xed\xa0\x80", 3),
        std::string("\xf4\x90\x80\x80", 4), std::string("\xf0\x80", 2), std::string("\xe6x", 2)})
        if (profile_config::encode_like(prefixBytes, bytes, encodedText, &error) || encodedText != "unchanged") return 9024;
    const std::string opaqueAnsi = "[preferences]\r\nopaque=\xe9\r\n";
    if (!profile_bank_prepare_edit(opaqueAnsi, *bank, unicodeSlot, edit, &error) ||
        edit.replacement.compare(0, opaqueAnsi.size(), opaqueAnsi)) return 9025;
    const std::string large(profile_slot::MAX_CONFIG_BYTES + 1, 'x');
    if (profile_config::decode(large, decodedText, &error) || decodedText != "unchanged" ||
        profile_config::encode_like(prefixBytes, large, encodedText, &error) || encodedText != "unchanged") return 9026;
    const std::string controls16("\xff\xfe\x01\0\x7f\0", 6);
    if (!profile_config::decode(controls16, decodedText, &error) ||
        !profile_config::encode_like(controls16, decodedText, encodedText, &error) || encodedText != controls16) return 9027;
#if defined(_WIN32)
    struct TempBank {
        WCHAR name[MAX_PATH]{};
        HANDLE file = INVALID_HANDLE_VALUE;
        ~TempBank() { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); if (name[0]) DeleteFileW(name); }
    } temporary;
    WCHAR directory[MAX_PATH]{};
    const DWORD directorySize = GetTempPathW(MAX_PATH, directory);
    if (!directorySize || directorySize >= MAX_PATH ||
        !GetTempFileNameW(directory, L"gcu", 0, temporary.name)) return 9028;
    temporary.file = CreateFileW(temporary.name, GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    if (temporary.file == INVALID_HANDLE_VALUE ||
        !WriteFile(temporary.file, prepared16.data(), (DWORD)prepared16.size(), &written, nullptr) ||
        written != prepared16.size()) return 9028;
    CloseHandle(temporary.file); temporary.file = INVALID_HANDLE_VALUE;
    char utf8Path[MAX_PATH * 4]{};
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, temporary.name, -1, utf8Path, sizeof(utf8Path), nullptr, nullptr))
        return 9028;
    char why[192]{};
    if (!profile_windows_document_section(utf8Path, "profile3", extracted, found, why, sizeof(why)) || !found ||
        !profile_slot_decode(extracted, *loaded, found) || !found ||
        !profile_document_semantically_equal(bank->documents[0], *loaded)) return 9029;
    if (!profile_windows_document_section(utf8Path, "profile4", extracted, found, why, sizeof(why)) || found) return 9030;
    WCHAR marker[64]{};
    if (GetPrivateProfileStringW(L"profile3", L"document.format", L"", marker, 64, temporary.name) == 0 ||
        wcscmp(marker, L"green_curve_profile_v1") || GetPrivateProfileIntW(L"profile4", L"mem_offset_mhz", 0, temporary.name) != 1)
        return 9031;
    if (!DeleteFileW(temporary.name) || GetFileAttributesW(temporary.name) != INVALID_FILE_ATTRIBUTES ||
        GetLastError() != ERROR_FILE_NOT_FOUND) return 9032;
    temporary.name[0] = 0;
#endif
    fprintf(stdout, "local profile documents: offline full-bank preservation and atomic conflict checks passed\n");
    return 0;
}
#ifdef GC_PROFILE_SLOT_FIXTURE_MAIN
int main() {
    const int code = run_profile_slot_codec_tests();
    if (code) fprintf(stderr, "profile slot regression: %d\n", code);
    return code;
}
#endif
