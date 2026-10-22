// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The mapped-image version reader (version_resource_policy.h,
// module_version_win32.h), assertion codes 7180-7189.  The layout rules run
// on every host against a synthesised image; on Windows a real image is
// mapped, its file is renamed away and replaced by a copy carrying another
// version -- the in-place driver update -- and the mapping must still report
// the version it was mapped with while the path now names the new one.

#include <stdio.h>
#include <string.h>
#include <initializer_list>

#include "version_resource_policy.h"

#if defined(_WIN32)
#include <windows.h>
#include <strsafe.h>
#include "module_version_win32.h"
#endif

namespace {

void put16(unsigned char* p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
void put32(unsigned char* p, uint32_t v) { put16(p, v & 0xFFFFu); put16(p + 2, v >> 16); }

// A VS_VERSIONINFO root block with the given fixed file version.
void make_version_block(unsigned char* block, uint32_t ms, uint32_t ls) {
    memset(block, 0, 92);
    put16(block, 92);
    put16(block + 2, GC_VS_FIXED_FILE_INFO_SIZE);
    const char key[] = "VS_VERSION_INFO";
    for (size_t i = 0; i < sizeof(key); ++i) block[6 + 2 * i] = (unsigned char)key[i];
    put32(block + 40, GC_VS_FIXED_FILE_INFO_SIGNATURE);
    put32(block + 44, 0x00010000u);
    put32(block + 48, ms);
    put32(block + 52, ls);
}

// A minimal image-layout PE: RT_VERSION / 1 / 0x409 -> the block at 0x300.
// PE32+ unless `pe32`.  SizeOfImage is 0x400.
void make_image(unsigned char* image, bool pe32, uint32_t ms, uint32_t ls) {
    memset(image, 0, 0x400);
    image[0] = 'M';
    image[1] = 'Z';
    put32(image + 0x3C, 0x40);
    put32(image + 0x40, 0x00004550u);
    const uint32_t directories = pe32 ? 96u : 112u;
    put16(image + 0x40 + 20, directories + 16 * 8);  // SizeOfOptionalHeader
    unsigned char* optional = image + 0x58;
    put16(optional, pe32 ? 0x10Bu : 0x20Bu);
    put32(optional + 56, 0x400);                        // SizeOfImage
    put32(optional + directories - 4, 16);              // NumberOfRvaAndSizes
    put32(optional + directories + 16, 0x200);          // resource RVA
    put32(optional + directories + 20, 0x100);          // resource size
    unsigned char* rsrc = image + 0x200;
    put16(rsrc + 14, 1);                                // root: one ID entry
    put32(rsrc + 16, GC_PE_RT_VERSION);
    put32(rsrc + 20, 0x80000000u | 0x18);
    put16(rsrc + 0x18 + 14, 1);                         // name level
    put32(rsrc + 0x18 + 16, GC_PE_VS_VERSION_INFO_ID);
    put32(rsrc + 0x18 + 20, 0x80000000u | 0x30);
    put16(rsrc + 0x30 + 14, 1);                         // language level
    put32(rsrc + 0x30 + 16, 0x409);
    put32(rsrc + 0x30 + 20, 0x48);                      // data entry
    put32(rsrc + 0x48, 0x300);
    put32(rsrc + 0x48 + 4, 92);
    make_version_block(image + 0x300, ms, ls);
}

bool image_version(const unsigned char* image, size_t size, uint32_t* ms, uint32_t* ls) {
    const unsigned char* data = nullptr;
    size_t dataSize = 0;
    return gc_pe_image_version_resource(image, size, &data, &dataSize) &&
           gc_version_resource_file_version(data, dataSize, ms, ls);
}

int version_resource_policy_tests() {
    // 7180: the root block yields MS/LS from VS_FIXEDFILEINFO; anything that is
    // not exactly that layout is refused rather than misread.
    unsigned char block[96];
    uint32_t ms = 0, ls = 0;
    make_version_block(block, 0x00200000u, 0x00100835u);
    if (!gc_version_resource_file_version(block, 92, &ms, &ls) ||
        ms != 0x00200000u || ls != 0x00100835u) return 7180;
    if (gc_version_resource_file_version(block, 91, &ms, &ls) ||
        gc_version_resource_file_version(nullptr, 92, &ms, &ls)) return 7180;
    block[6] = 'X';
    if (gc_version_resource_file_version(block, 92, &ms, &ls)) return 7180;
    make_version_block(block, 1, 2);
    block[40] ^= 1;
    if (gc_version_resource_file_version(block, 92, &ms, &ls)) return 7180;
    make_version_block(block, 1, 2);
    put16(block + 2, GC_VS_FIXED_FILE_INFO_SIZE - 1);
    if (gc_version_resource_file_version(block, 92, &ms, &ls)) return 7180;
    make_version_block(block, 1, 2);
    put16(block, 96);  // declared longer than the resource
    if (gc_version_resource_file_version(block, 92, &ms, &ls)) return 7180;

    // 7181: the resource walk finds the block in PE32+ and PE32 images.
    static unsigned char image[0x400];
    for (bool pe32 : {false, true}) {
        make_image(image, pe32, 0x00200000u, 0x00100835u);
        ms = ls = 0;
        if (!image_version(image, sizeof(image), &ms, &ls) ||
            ms != 0x00200000u || ls != 0x00100835u) return 7181;
        ms = ls = 0;
        if (!image_version(image, SIZE_MAX, &ms, &ls) || ms != 0x00200000u) return 7181;
    }

    // 7182: every offset is bounded; a malformed or foreign image is refused.
    struct Mutation { size_t offset; uint32_t value; bool wide; };
    const Mutation mutations[] = {
        { 0, 'N', false },                         // not MZ
        { 0x40, 0x00004551u, true },               // not "PE\0\0"
        { 0x58, 0x0107u, false },                  // unknown optional-header magic
        { 0x58 + 108, 2u, true },                  // no resource directory slot
        { 0x58 + 56, 0x340u, true },               // SizeOfImage ends inside the block
        { 0x58 + 112 + 16, 0x3F0u, true },         // resource section past SizeOfImage
        { 0x200 + 16, 0x80000000u | 16u, true },   // RT_VERSION only as a NAMED entry
        { 0x200 + 20, 0x18u, true },               // type level not a directory
        { 0x218 + 16, 2u, true },                  // no VS_VERSION_INFO id 1
        { 0x230 + 20, 0x80000000u | 0x48u, true }, // language leaf is a directory
        { 0x230 + 14, 0u, true },                  // empty language level
        { 0x248, 0x3F0u, true },                   // data past SizeOfImage
        { 0x200 + 14, 0x7FFFu, false },            // entry count past the section
    };
    for (const Mutation& m : mutations) {
        make_image(image, false, 1, 2);
        if (m.wide) put32(image + m.offset, m.value);
        else put16(image + m.offset, m.value);
        if (image_version(image, sizeof(image), &ms, &ls)) return 7182;
    }
    make_image(image, false, 1, 2);
    if (image_version(image, 0x300, &ms, &ls) || image_version(image, 0x50, &ms, &ls) ||
        image_version(nullptr, sizeof(image), &ms, &ls)) return 7182;

    // 7189: the log spelling of a driver image version, with the NVIDIA
    // release read from the last digit of field 3 and all of field 4.
    struct Spelling { uint32_t ms, ls; const char* text; };
    const Spelling spellings[] = {
        { 0x00200000u, 0x000F1FF4u, "32.0.15.8180 (581.80)" },
        { 0x00200000u, 0x000F17CEu, "32.0.15.6094 (560.94)" },
        { 0x001F0000u, 0x000F0005u, "31.0.15.5 (500.05)" },
        { 0x00200000u, 0x000E2710u, "32.0.14.10000" },  // no release reading
        { 0u, 0u, "unknown" },
    };
    char text[48];
    for (const Spelling& s : spellings) {
        memset(text, 'x', sizeof(text));
        gc_driver_version_text(s.ms, s.ls, text, sizeof(text));
        if (strcmp(text, s.text) != 0) {
            printf("  7189: %08x/%08x -> '%s', expected '%s'\n", (unsigned)s.ms, (unsigned)s.ls,
                   text, s.text);
            return 7189;
        }
    }
    char small[8];
    gc_driver_version_text(0x00200000u, 0x000F1FF4u, small, sizeof(small));
    if (strlen(small) != sizeof(small) - 1) return 7189;  // truncated, still terminated
    gc_driver_version_text(1, 2, nullptr, 0);
    return 0;
}

#if defined(_WIN32)

bool read_whole_file(const wchar_t* path, unsigned char** out, DWORD* size) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER length = {};
    bool ok = GetFileSizeEx(file, &length) && length.QuadPart > 0 && length.QuadPart < (64 << 20);
    unsigned char* bytes = ok ? (unsigned char*)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)length.QuadPart) : nullptr;
    DWORD got = 0;
    ok = bytes && ReadFile(file, bytes, (DWORD)length.QuadPart, &got, nullptr) && got == length.QuadPart;
    CloseHandle(file);
    if (!ok) {
        if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
        return false;
    }
    *out = bytes;
    *size = got;
    return true;
}

bool write_whole_file(const wchar_t* path, const unsigned char* bytes, DWORD size) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, bytes, size, &written, nullptr) && written == size;
    CloseHandle(file);
    return ok;
}

// The fixed file version LS in a raw file, located through the version key.
unsigned char* find_file_version_ls(unsigned char* bytes, DWORD size) {
    const char key[] = "VS_VERSION_INFO";
    const DWORD keyBytes = (DWORD)sizeof(key) * 2;
    for (DWORD at = 6; at + 34 + 16 <= size; at += 2) {
        bool match = true;
        for (DWORD i = 0; i < keyBytes && match; i += 2)
            match = bytes[at + i] == (unsigned char)key[i / 2] && bytes[at + i + 1] == 0;
        if (match && gc_version_resource_u32(bytes + at - 6 + 40) == GC_VS_FIXED_FILE_INFO_SIGNATURE)
            return bytes + at - 6 + 40 + 12;
    }
    return nullptr;
}

// 7185-7188: the in-place update.  Map the copy at `live`, move the file away
// under the mapping, put a copy with another version at the original path.
// The path now names the new version; the mapping keeps the one it ran with.
int in_place_update_test(const wchar_t* live, const wchar_t* moved, unsigned char* bytes,
                         DWORD size, unsigned char* ls) {
    const DWORD imageFlags = LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE;
    HMODULE old = LoadLibraryExW(live, nullptr, imageFlags);
    DWORD oldMs = 0, oldLs = 0, newMs = 0, newLs = 0, afterMs = 0, afterLs = 0;
    if (!old || !gc_module_mapped_version(old, &oldMs, &oldLs)) {
        if (old) FreeLibrary(old);
        return 7185;
    }
    if (!MoveFileW(live, moved)) {
        printf("module version fixture: moving a mapped image failed (%lu)\n", GetLastError());
        FreeLibrary(old);
        return 7186;
    }
    put32(ls, gc_version_resource_u32(ls) ^ 0x00005A5Au);
    HMODULE updated = write_whole_file(live, bytes, size)
        ? LoadLibraryExW(live, nullptr, imageFlags) : nullptr;
    int failure = 0;
    if (!updated || !gc_module_mapped_version(updated, &newMs, &newLs) ||
        !gc_module_mapped_version(old, &afterMs, &afterLs)) {
        failure = 7187;
    } else if (newMs != oldMs || newLs != (oldLs ^ 0x00005A5Au) ||
               afterMs != oldMs || afterLs != oldLs) {
        printf("module version fixture: mapped %08lx.%08lx, after %08lx.%08lx, path %08lx.%08lx\n",
               oldMs, oldLs, afterMs, afterLs, newMs, newLs);
        failure = 7188;
    }
    if (updated) FreeLibrary(updated);
    FreeLibrary(old);
    return failure;
}

int module_version_win32_tests_in(const wchar_t* dir) {
    wchar_t system[MAX_PATH] = {}, source[MAX_PATH] = {};
    wchar_t live[MAX_PATH] = {}, moved[MAX_PATH] = {};
    UINT n = GetSystemDirectoryW(system, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 7183;
    StringCchPrintfW(source, MAX_PATH, L"%ls\\kernel32.dll", system);
    StringCchPrintfW(live, MAX_PATH, L"%ls\\gc_mapped.dll", dir);
    StringCchPrintfW(moved, MAX_PATH, L"%ls\\gc_mapped.old", dir);
    const DWORD imageFlags = LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE;

    // 7183: a module the loader mapped (no tag bits) and a fresh image-resource
    // mapping of the same file agree, and the version is real.
    DWORD loadedMs = 0, loadedLs = 0, freshMs = 0, freshLs = 0;
    HMODULE fresh = LoadLibraryExW(source, nullptr, imageFlags);
    const bool agree = fresh &&
        gc_module_mapped_version(GetModuleHandleW(L"kernel32.dll"), &loadedMs, &loadedLs) &&
        gc_module_mapped_version(fresh, &freshMs, &freshLs) &&
        loadedMs == freshMs && loadedLs == freshLs && (loadedMs >> 16) >= 6;
    if (fresh) FreeLibrary(fresh);
    if (!agree) return 7183;

    unsigned char* bytes = nullptr;
    DWORD size = 0;
    if (!read_whole_file(source, &bytes, &size)) return 7185;
    unsigned char* ls = find_file_version_ls(bytes, size);
    if (!ls || !write_whole_file(live, bytes, size)) {
        HeapFree(GetProcessHeap(), 0, bytes);
        DeleteFileW(live);
        return 7185;
    }

    // 7184: a file-layout data mapping is refused, never misread as an image.
    // (A copy: for a module already loaded, LoadLibraryEx returns that module.)
    HMODULE dataFile = LoadLibraryExW(live, nullptr, LOAD_LIBRARY_AS_DATAFILE);
    DWORD ignoredMs = 0, ignoredLs = 0;
    const bool refused = dataFile && ((ULONG_PTR)dataFile & 3) == 1 &&
        !gc_module_mapped_version(dataFile, &ignoredMs, &ignoredLs);
    if (dataFile) FreeLibrary(dataFile);
    const int failure = refused ? in_place_update_test(live, moved, bytes, size, ls) : 7184;
    HeapFree(GetProcessHeap(), 0, bytes);
    DeleteFileW(live);
    DeleteFileW(moved);
    return failure;
}

int module_version_win32_tests() {
    wchar_t temp[MAX_PATH] = {}, dir[MAX_PATH] = {};
    DWORD n = GetTempPathW(MAX_PATH, temp);
    if (n == 0 || n >= MAX_PATH) return 7183;
    StringCchPrintfW(dir, MAX_PATH, L"%lsgc_module_version_%lu_%llu", temp,
                     (unsigned long)GetCurrentProcessId(), (unsigned long long)GetTickCount64());
    if (!CreateDirectoryW(dir, nullptr)) return 7183;
    int failure = module_version_win32_tests_in(dir);
    RemoveDirectoryW(dir);
    return failure;
}

#endif

}  // namespace

int run_module_version_tests() {
    if (int failure = version_resource_policy_tests()) return failure;
#if defined(_WIN32)
    if (int failure = module_version_win32_tests()) return failure;
#endif
    return 0;
}
