// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Registry side of wer_local_dumps_policy.h.  Header-only so the service
// install (main_service_install.cpp) and setup's rollback
// (installer_transaction.cpp) share one implementation; the caller logs the
// per-image results in its own log.  Production needs an elevated caller
// (HKLM); the regression harness drives the same code against scratch HKCU.
#ifndef GREEN_CURVE_WER_LOCAL_DUMPS_WIN_H
#define GREEN_CURVE_WER_LOCAL_DUMPS_WIN_H

#include <windows.h>
#include <dbghelp.h>
#include <strsafe.h>
#include <wchar.h>

#include "wer_local_dumps_policy.h"

static_assert(GC_WER_CUSTOM_DUMP_FLAGS == (unsigned)(
    MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpScanMemory |
    MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory |
    MiniDumpWithProcessThreadData | MiniDumpWithThreadInfo | MiniDumpWithFullMemoryInfo),
    "WER dumps carry the same streams as the program's own minidumps");

struct GcWerResult {
    const wchar_t* imageName;
    GcWerKeyState before;
    GcWerAction action;
    LONG status;  // ERROR_SUCCESS, or the first failing registry call's code
};

// Where the keys live: HKLM + GC_WER_LOCAL_DUMPS_KEY in production.
struct GcWerRegistryRoot {
    HKEY root;
    const wchar_t* basePath;
};

static inline GcWerRegistryRoot gc_wer_machine_root() {
    GcWerRegistryRoot where = {HKEY_LOCAL_MACHINE, GC_WER_LOCAL_DUMPS_KEY};
    return where;
}

static inline bool gc_wer_image_key_path(const GcWerRegistryRoot& where, const wchar_t* imageName,
                                         wchar_t* out, size_t outCount) {
    return SUCCEEDED(StringCchPrintfW(out, outCount, L"%ls\\%ls", where.basePath, imageName));
}

static inline GcWerKeyState gc_wer_read_key_state(const GcWerRegistryRoot& where,
                                                   const wchar_t* imageName) {
    wchar_t path[512] = {};
    if (!gc_wer_image_key_path(where, imageName, path, ARRAYSIZE(path))) return GC_WER_KEY_UNKNOWN;
    HKEY key = nullptr;
    LONG st = RegOpenKeyExW(where.root, path, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
    if (st == ERROR_FILE_NOT_FOUND) return GC_WER_KEY_ABSENT;
    if (st != ERROR_SUCCESS) return GC_WER_KEY_UNKNOWN;
    DWORD type = 0, value = 0, size = sizeof(value);
    st = RegQueryValueExW(key, GC_WER_MARKER_VALUE, nullptr, &type, (BYTE*)&value, &size);
    RegCloseKey(key);
    if (st == ERROR_SUCCESS && type == REG_DWORD && value == 1) return GC_WER_KEY_OURS;
    if (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND || st == ERROR_MORE_DATA)
        return GC_WER_KEY_FOREIGN;
    return GC_WER_KEY_UNKNOWN;
}

static inline LONG gc_wer_write_image(const GcWerRegistryRoot& where, const GcWerImage& image) {
    wchar_t path[512] = {};
    if (!gc_wer_image_key_path(where, image.imageName, path, ARRAYSIZE(path)))
        return ERROR_BUFFER_OVERFLOW;
    HKEY key = nullptr;
    LONG st = RegCreateKeyExW(where.root, path, 0, nullptr, REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
    if (st != ERROR_SUCCESS) return st;
    auto set_dword = [&](const wchar_t* name, DWORD v) {
        return RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    };
    // Marker first: a partially written key is still recognisably ours, so a
    // later install or removal can finish or undo it.
    st = set_dword(GC_WER_MARKER_VALUE, 1);
    if (st == ERROR_SUCCESS) st = set_dword(L"DumpCount", GC_WER_DUMP_COUNT);
    if (st == ERROR_SUCCESS) st = set_dword(L"DumpType", GC_WER_DUMP_TYPE_CUSTOM);
    if (st == ERROR_SUCCESS) st = set_dword(L"CustomDumpFlags", GC_WER_CUSTOM_DUMP_FLAGS);
    if (st == ERROR_SUCCESS) {
        if (image.dumpFolder) {
            DWORD bytes = (DWORD)((wcslen(image.dumpFolder) + 1) * sizeof(wchar_t));
            st = RegSetValueExW(key, L"DumpFolder", 0, REG_EXPAND_SZ,
                (const BYTE*)image.dumpFolder, bytes);
        } else {
            LONG del = RegDeleteValueW(key, L"DumpFolder");
            if (del != ERROR_SUCCESS && del != ERROR_FILE_NOT_FOUND) st = del;
        }
    }
    RegCloseKey(key);
    return st;
}

static inline LONG gc_wer_delete_image(const GcWerRegistryRoot& where, const GcWerImage& image) {
    wchar_t path[512] = {};
    if (!gc_wer_image_key_path(where, image.imageName, path, ARRAYSIZE(path)))
        return ERROR_BUFFER_OVERFLOW;
    LONG st = RegDeleteKeyExW(where.root, path, KEY_WOW64_64KEY, 0);
    return st == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : st;
}

// Register (or refresh) every image whose key is absent or ours.  Never fails
// an install: the caller logs the results; a missing backstop costs evidence.
static inline void gc_wer_register_all(GcWerResult* results,
                                       const GcWerRegistryRoot& where = gc_wer_machine_root()) {
    for (size_t i = 0; i < GC_WER_IMAGE_COUNT; ++i) {
        GcWerResult& r = results[i];
        r.imageName = kGcWerImages[i].imageName;
        r.before = gc_wer_read_key_state(where, r.imageName);
        r.action = gc_wer_register_action(r.before);
        r.status = r.action == GC_WER_ACTION_WRITE
            ? gc_wer_write_image(where, kGcWerImages[i]) : ERROR_SUCCESS;
    }
}

static inline void gc_wer_unregister_all(GcWerResult* results,
                                         const GcWerRegistryRoot& where = gc_wer_machine_root()) {
    for (size_t i = 0; i < GC_WER_IMAGE_COUNT; ++i) {
        GcWerResult& r = results[i];
        r.imageName = kGcWerImages[i].imageName;
        r.before = gc_wer_read_key_state(where, r.imageName);
        r.action = gc_wer_unregister_action(r.before);
        r.status = r.action == GC_WER_ACTION_DELETE
            ? gc_wer_delete_image(where, kGcWerImages[i]) : ERROR_SUCCESS;
    }
}

#endif  // GREEN_CURVE_WER_LOCAL_DUMPS_WIN_H
