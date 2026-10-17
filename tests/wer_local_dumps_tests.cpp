// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// WER LocalDumps backstop registration (wer_local_dumps_policy.h,
// wer_local_dumps_win.h), assertion codes 6570-6589.  The policy runs on every
// host; on Windows the real registry code runs against a scratch HKCU key, so
// no elevation is needed and the machine's real LocalDumps keys are untouched.

#include <wchar.h>

#include "wer_local_dumps_policy.h"

#if defined(_WIN32)
#include "wer_local_dumps_win.h"
#include <stdio.h>
#endif

static int wer_policy_tests() {
    // 6570: only an absent or our own key is written.
    if (gc_wer_register_action(GC_WER_KEY_ABSENT) != GC_WER_ACTION_WRITE ||
        gc_wer_register_action(GC_WER_KEY_OURS) != GC_WER_ACTION_WRITE ||
        gc_wer_register_action(GC_WER_KEY_FOREIGN) != GC_WER_ACTION_LEAVE ||
        gc_wer_register_action(GC_WER_KEY_UNKNOWN) != GC_WER_ACTION_LEAVE) return 6570;
    // 6571: only our own key is ever deleted.
    if (gc_wer_unregister_action(GC_WER_KEY_OURS) != GC_WER_ACTION_DELETE ||
        gc_wer_unregister_action(GC_WER_KEY_ABSENT) != GC_WER_ACTION_LEAVE ||
        gc_wer_unregister_action(GC_WER_KEY_FOREIGN) != GC_WER_ACTION_LEAVE ||
        gc_wer_unregister_action(GC_WER_KEY_UNKNOWN) != GC_WER_ACTION_LEAVE) return 6571;
    // 6572: the service's SYSTEM dumps go to its own admin-only profile
    // directory; the GUI keeps WER's per-user default.
    bool sawService = false, sawGui = false;
    for (size_t i = 0; i < GC_WER_IMAGE_COUNT; ++i) {
        const GcWerImage& img = kGcWerImages[i];
        if (wcscmp(img.imageName, L"greencurve-service.exe") == 0) {
            sawService = img.dumpFolder &&
                wcsstr(img.dumpFolder, L"\\System32\\config\\systemprofile\\") != nullptr &&
                wcsstr(img.dumpFolder, L"ProgramData") == nullptr;
        }
        if (wcscmp(img.imageName, L"greencurve.exe") == 0) sawGui = img.dumpFolder == nullptr;
    }
    if (!sawService || !sawGui) return 6572;
    // 6573: bounded count, custom type, no full-memory flag (0x2).
    if (GC_WER_DUMP_COUNT == 0 || GC_WER_DUMP_COUNT > 10 ||
        GC_WER_DUMP_TYPE_CUSTOM != 0 || (GC_WER_CUSTOM_DUMP_FLAGS & 0x2u) != 0) return 6573;
    return 0;
}

#if defined(_WIN32)
namespace {

struct ScratchRoot {
    wchar_t base[160] = {};
    GcWerRegistryRoot where = {};
    ScratchRoot() {
        StringCchPrintfW(base, ARRAYSIZE(base),
            L"Software\\GreenCurveRegressionTests\\WerLocalDumps-%lu",
            (unsigned long)GetCurrentProcessId());
        where.root = HKEY_CURRENT_USER;
        where.basePath = base;
    }
    ~ScratchRoot() {
        RegDeleteTreeW(HKEY_CURRENT_USER, base);
        // Only succeeds once empty, so a concurrent run's key survives.
        RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\GreenCurveRegressionTests");
    }
};

bool read_dword(const ScratchRoot& s, const wchar_t* image, const wchar_t* name, DWORD* out) {
    wchar_t path[512] = {};
    if (!gc_wer_image_key_path(s.where, image, path, ARRAYSIZE(path))) return false;
    DWORD size = sizeof(*out), type = 0;
    return RegGetValueW(HKEY_CURRENT_USER, path, name, RRF_RT_REG_DWORD, &type, out, &size) == ERROR_SUCCESS;
}

bool read_expand(const ScratchRoot& s, const wchar_t* image, wchar_t* out, DWORD bytes) {
    wchar_t path[512] = {};
    if (!gc_wer_image_key_path(s.where, image, path, ARRAYSIZE(path))) return false;
    DWORD type = 0;
    return RegGetValueW(HKEY_CURRENT_USER, path, L"DumpFolder",
        RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, &type, out, &bytes) == ERROR_SUCCESS &&
        type == REG_EXPAND_SZ;
}

bool key_exists(const ScratchRoot& s, const wchar_t* image) {
    return gc_wer_read_key_state(s.where, image) != GC_WER_KEY_ABSENT;
}

int registry_tests() {
    // 6574: a fresh registration writes every image with our marker and values.
    {
        ScratchRoot s;
        GcWerResult r[GC_WER_IMAGE_COUNT] = {};
        gc_wer_register_all(r, s.where);
        for (size_t i = 0; i < GC_WER_IMAGE_COUNT; ++i) {
            if (r[i].before != GC_WER_KEY_ABSENT || r[i].action != GC_WER_ACTION_WRITE ||
                r[i].status != ERROR_SUCCESS) return 6574;
            DWORD marker = 0, count = 0, type = 99, flags = 0;
            if (!read_dword(s, r[i].imageName, GC_WER_MARKER_VALUE, &marker) || marker != 1 ||
                !read_dword(s, r[i].imageName, L"DumpCount", &count) || count != GC_WER_DUMP_COUNT ||
                !read_dword(s, r[i].imageName, L"DumpType", &type) || type != 0 ||
                !read_dword(s, r[i].imageName, L"CustomDumpFlags", &flags) ||
                flags != GC_WER_CUSTOM_DUMP_FLAGS) return 6575;
            wchar_t folder[MAX_PATH] = {};
            bool hasFolder = read_expand(s, r[i].imageName, folder, sizeof(folder));
            if ((kGcWerImages[i].dumpFolder != nullptr) != hasFolder) return 6576;
            if (hasFolder && wcscmp(folder, kGcWerImages[i].dumpFolder) != 0) return 6576;
        }
        // 6577: re-registering recognises the keys as ours and refreshes them.
        gc_wer_register_all(r, s.where);
        for (size_t i = 0; i < GC_WER_IMAGE_COUNT; ++i)
            if (r[i].before != GC_WER_KEY_OURS || r[i].status != ERROR_SUCCESS) return 6577;
        // 6578: removal deletes exactly our keys; a second removal is a no-op.
        gc_wer_unregister_all(r, s.where);
        for (size_t i = 0; i < GC_WER_IMAGE_COUNT; ++i) {
            if (r[i].action != GC_WER_ACTION_DELETE || r[i].status != ERROR_SUCCESS ||
                key_exists(s, r[i].imageName)) return 6578;
        }
        gc_wer_unregister_all(r, s.where);
        for (size_t i = 0; i < GC_WER_IMAGE_COUNT; ++i)
            if (r[i].before != GC_WER_KEY_ABSENT || r[i].action != GC_WER_ACTION_LEAVE) return 6579;
    }
    // 6580: an administrator's own key (no marker) is never overwritten nor
    // deleted.
    {
        ScratchRoot s;
        wchar_t path[512] = {};
        if (!gc_wer_image_key_path(s.where, kGcWerImages[0].imageName, path, ARRAYSIZE(path))) return 6580;
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
            ERROR_SUCCESS) return 6580;
        DWORD full = 2, count = 1;
        RegSetValueExW(key, L"DumpType", 0, REG_DWORD, (const BYTE*)&full, sizeof(full));
        RegSetValueExW(key, L"DumpCount", 0, REG_DWORD, (const BYTE*)&count, sizeof(count));
        RegCloseKey(key);
        GcWerResult r[GC_WER_IMAGE_COUNT] = {};
        gc_wer_register_all(r, s.where);
        DWORD type = 0, marker = 0;
        if (r[0].before != GC_WER_KEY_FOREIGN || r[0].action != GC_WER_ACTION_LEAVE ||
            !read_dword(s, kGcWerImages[0].imageName, L"DumpType", &type) || type != 2 ||
            read_dword(s, kGcWerImages[0].imageName, GC_WER_MARKER_VALUE, &marker)) return 6581;
        gc_wer_unregister_all(r, s.where);
        if (r[0].action != GC_WER_ACTION_LEAVE || !key_exists(s, kGcWerImages[0].imageName)) return 6582;
    }
    return 0;
}

}  // namespace
#endif

int run_wer_local_dumps_tests() {
    if (int r = wer_policy_tests()) return r;
#if defined(_WIN32)
    if (int r = registry_tests()) return r;
#endif
    return 0;
}
