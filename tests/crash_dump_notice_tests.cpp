// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The user-facing service crash-dump notice (crash_dump_notice_policy.h,
// crash_dump_notice_win32.h), assertion codes 7140-7159.  Selection and text
// run on every host; on Windows the real scan and write run against a scratch
// directory under %TEMP%, so nothing outside it is touched.

#include <stdio.h>
#include <string.h>

#include "crash_dump_notice_policy.h"

#if defined(_WIN32)
#include "crash_dump_notice_win32.h"
#endif

static GcCrashDumpNoticeEntry notice_entry(const char* name, unsigned long long key,
                                           unsigned long long size, const char* when) {
    GcCrashDumpNoticeEntry entry = {};
    snprintf(entry.name, sizeof(entry.name), "%s", name);
    snprintf(entry.when, sizeof(entry.when), "%s", when);
    entry.sortKey = key;
    entry.sizeBytes = size;
    return entry;
}

static int crash_dump_notice_policy_tests() {
    // 7140: newest first, capped; ties keep a stable order.
    GcCrashDumpNoticeEntry entries[4] = {
        notice_entry("b.dmp", 20, 1, "t2"), notice_entry("d.dmp", 40, 1, "t4"),
        notice_entry("a.dmp", 10, 1, "t1"), notice_entry("c.dmp", 30, 1, "t3") };
    if (gc_crash_dump_notice_keep_newest(entries, 4, 3) != 3 ||
        strcmp(entries[0].name, "d.dmp") != 0 || strcmp(entries[1].name, "c.dmp") != 0 ||
        strcmp(entries[2].name, "b.dmp") != 0 || strcmp(entries[3].name, "a.dmp") != 0 ||
        gc_crash_dump_notice_keep_newest(entries, 2, 5) != 2 ||
        gc_crash_dump_notice_keep_newest(nullptr, 2, 5) != 0) return 7140;

    // 7141: nothing to announce leaves no text.
    char text[4096];
    memset(text, 'x', sizeof(text));
    if (gc_crash_dump_notice_format("S", "W", nullptr, 0, 0, nullptr, 0, 0, "D", text,
                                    sizeof(text)) || text[0] != 0) return 7141;

    // 7142: service dumps: count, newest-first listing, "older" tail, the
    // elevated copy command naming every folder; no WER section.
    GcCrashDumpNoticeEntry dumps[2] = {
        notice_entry("greencurve_veh_20261021_215918_538_pid164.dmp", 2, 2048, "2026-10-21 21:59:18"),
        notice_entry("greencurve_veh_20261006_135254_162_pid5396.dmp", 1, 1, "2026-10-06 13:52:54") };
    if (!gc_crash_dump_notice_format("C:\\Svc", "C:\\Svc\\WerDumps", dumps, 2, 7, nullptr, 0, 0,
                                     "C:\\Me\\crash-dumps", text, sizeof(text))) return 7142;
    const char* newest = strstr(text, "pid164");
    const char* older = strstr(text, "pid5396");
    if (!strstr(text, "crash dumps available") ||
        !strstr(text, "Service crash dumps (7 in C:\\Svc):") ||
        !newest || !older || newest > older ||
        !strstr(text, "2026-10-21 21:59:18         2 KB") ||
        !strstr(text, "... and 5 older") ||
        strstr(text, "Windows Error Reporting dumps") ||
        !strstr(text, "PowerShell as administrator") ||
        !strstr(text, "$dst = 'C:\\Me\\crash-dumps'; $svc = 'C:\\Svc'; $wer = 'C:\\Svc\\WerDumps';") ||
        !strstr(text, "Get-ChildItem -LiteralPath $svc, $wer -Filter *.dmp -File") ||
        !strstr(text, "Copy-Item -LiteralPath (Join-Path $svc 'greencurve_crash.txt') -Destination $dst") ||
        strstr(text, "\"C:\\")) return 7142;

    // 7143: WER dumps alone are announced too, without an "older" tail.
    GcCrashDumpNoticeEntry wer[1] = {
        notice_entry("greencurve-service.exe.164.dmp", 5, 4096, "2026-10-21 21:59:19") };
    if (!gc_crash_dump_notice_format("C:\\Svc", "C:\\Svc\\WerDumps", nullptr, 0, 0, wer, 1, 1,
                                     "C:\\Me\\crash-dumps", text, sizeof(text)) ||
        strstr(text, "Service crash dumps (") ||
        !strstr(text, "Windows Error Reporting dumps (1 in C:\\Svc\\WerDumps):") ||
        !strstr(text, "greencurve-service.exe.164.dmp") || strstr(text, "older")) return 7143;

    // 7144: a short buffer truncates, terminated, never overruns.
    char small[64];
    memset(small, 'x', sizeof(small));
    if (!gc_crash_dump_notice_format("C:\\Svc", "C:\\Svc\\WerDumps", dumps, 2, 2, wer, 1, 1,
                                     "C:\\Me", small, 48) ||
        strlen(small) != 47 || small[48] != 'x') return 7144;

    // 7148: folder names from the user's profile reach an administrator's
    // PowerShell as inert single-quoted literals: $, $() and backticks stay
    // text, and every single quote PowerShell accepts (ASCII and U+2018..201B)
    // is doubled.  Before the fix "$(40+2)" sat in a double-quoted string and
    // PowerShell evaluated it.
    char literal[128];
    bool overflow = false;
    literal[0] = 0;
    gc_powershell_append_literal(literal, sizeof(literal), 0,
        "D:\\a$(40+2)`b\\O'Neil\\x\xE2\x80\x99y\xE2\x80\x98z\xE2\x80\x9A\xE2\x80\x9B", &overflow);
    if (overflow || strcmp(literal,
            "'D:\\a$(40+2)`b\\O''Neil\\x\xE2\x80\x99\xE2\x80\x99y\xE2\x80\x98\xE2\x80\x98"
            "z\xE2\x80\x9A\xE2\x80\x9A\xE2\x80\x9B\xE2\x80\x9B'") != 0) return 7148;
    // Other multi-byte text (here U+2026 and U+00E9) passes through untouched.
    literal[0] = 0;
    gc_powershell_append_literal(literal, sizeof(literal), 0,
        "C:\\\xE2\x80\xA6\\\xC3\xA9", &overflow);
    if (overflow || strcmp(literal, "'C:\\\xE2\x80\xA6\\\xC3\xA9'") != 0) return 7148;
    // A literal that does not fit reports overflow; a doubled quote is never
    // split into a lone one that would end the literal.
    char tight[5];
    tight[0] = 0;
    gc_powershell_append_literal(tight, sizeof(tight), 0, "ab'c", &overflow);
    if (!overflow || strcmp(tight, "'ab") != 0) return 7148;
    if (!gc_crash_dump_notice_format("C:\\Svc", "C:\\Svc\\WerDumps", dumps, 2, 2, nullptr, 0, 0,
                                     "D:\\Profiles\\a$(40+2)\\Green Curve\\crash-dumps", text,
                                     sizeof(text)) ||
        !strstr(text, "$dst = 'D:\\Profiles\\a$(40+2)\\Green Curve\\crash-dumps';")) return 7148;

    // 7149: the elevated command is all or nothing.  When it does not fit,
    // the notice still lists the dumps and says where they are, but never
    // ends in a cut-off command that could still run.
    char path[300];
    memset(path, 'p', sizeof(path) - 1);
    path[0] = 'C';
    path[1] = ':';
    path[2] = '\\';
    path[sizeof(path) - 1] = 0;
    char limited[1200];
    if (!gc_crash_dump_notice_format("C:\\Svc", "C:\\Svc\\WerDumps", dumps, 2, 2, nullptr, 0, 0,
                                     path, limited, sizeof(limited)) ||
        !strstr(limited, "pid164") || strstr(limited, "$dst") ||
        !strstr(limited, "open the folders above as an administrator.\r\n")) return 7149;
    // The same notice with room for the command carries all of it.
    if (!gc_crash_dump_notice_format("C:\\Svc", "C:\\Svc\\WerDumps", dumps, 2, 2, nullptr, 0, 0,
                                     path, text, sizeof(text)) ||
        !strstr(text, "$dst = 'C:\\ppp") ||
        !strstr(text, "-Destination $dst -ErrorAction SilentlyContinue\r\n")) return 7149;
    return 0;
}

#if defined(_WIN32)

static bool notice_make_file(const WCHAR* dir, const WCHAR* name, unsigned long long writeTime) {
    WCHAR path[MAX_PATH] = {};
    StringCchPrintfW(path, MAX_PATH, L"%ls\\%ls", dir, name);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, "dump", 4, &written, nullptr) && written == 4;
    FILETIME ft = {};
    ft.dwLowDateTime = (DWORD)writeTime;
    ft.dwHighDateTime = (DWORD)(writeTime >> 32);
    ok = ok && SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
    return ok;
}

static void notice_remove_tree(const WCHAR* dir) {
    WCHAR query[MAX_PATH] = {};
    StringCchPrintfW(query, MAX_PATH, L"%ls\\*", dir);
    WIN32_FIND_DATAW found = {};
    HANDLE search = FindFirstFileW(query, &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(found.cFileName, L".") || !wcscmp(found.cFileName, L"..")) continue;
            WCHAR child[MAX_PATH] = {};
            StringCchPrintfW(child, MAX_PATH, L"%ls\\%ls", dir, found.cFileName);
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(child);
            else DeleteFileW(child);
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    RemoveDirectoryW(dir);
}

static int crash_dump_notice_win32_tests_in(const WCHAR* dir) {
    GcCrashDumpNoticeEntry entries[2] = {};
    size_t kept = 9, total = 9;
    // 7145: a folder that does not exist is an empty, successful scan.
    WCHAR missing[MAX_PATH] = {};
    StringCchPrintfW(missing, MAX_PATH, L"%ls\\absent", dir);
    if (!gc_crash_dump_notice_scan(missing, L"*.dmp", entries, 2, &kept, &total) ||
        kept != 0 || total != 0) return 7145;

    // 7146: only matching regular files count; a directory named like a dump
    // is skipped; with more matches than capacity the newest are kept.
    const unsigned long long base = 133000000000000000ULL;
    WCHAR subdir[MAX_PATH] = {};
    StringCchPrintfW(subdir, MAX_PATH, L"%ls\\greencurve_fake.dmp", dir);
    if (!notice_make_file(dir, L"greencurve_veh_1.dmp", base + 10) ||
        !notice_make_file(dir, L"greencurve_veh_2.dmp", base + 30) ||
        !notice_make_file(dir, L"greencurve_crash_3.dmp", base + 20) ||
        !notice_make_file(dir, L"other.dmp", base + 99) ||
        !notice_make_file(dir, L"greencurve_crash.txt", base + 98) ||
        !CreateDirectoryW(subdir, nullptr)) return 7146;
    if (!gc_crash_dump_notice_scan(dir, L"greencurve_*.dmp", entries, 2, &kept, &total) ||
        kept != 2 || total != 3) return 7146;
    gc_crash_dump_notice_keep_newest(entries, kept, 2);
    if (strcmp(entries[0].name, "greencurve_veh_2.dmp") != 0 ||
        strcmp(entries[1].name, "greencurve_crash_3.dmp") != 0 ||
        entries[0].sizeBytes != 4 || entries[0].when[4] != '-') return 7146;

    // 7147: write, replace, remove; removing an absent notice is fine.
    WCHAR notice[MAX_PATH] = {};
    StringCchPrintfW(notice, MAX_PATH, L"%ls\\notice.txt", dir);
    DWORD error = 1;
    char read[64] = {};
    DWORD got = 0;
    auto read_notice = [&]() {
        memset(read, 0, sizeof(read));
        got = 0;
        HANDLE h = CreateFileW(notice, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        bool ok = ReadFile(h, read, sizeof(read) - 1, &got, nullptr) != FALSE;
        CloseHandle(h);
        return ok;
    };
    if (!gc_crash_dump_notice_store(notice, "first", &error) || error != 0 ||
        !read_notice() || strcmp(read, "first") != 0) return 7147;
    if (!gc_crash_dump_notice_store(notice, "second", &error) ||
        !read_notice() || strcmp(read, "second") != 0) return 7147;
    WCHAR temp[MAX_PATH] = {};
    StringCchPrintfW(temp, MAX_PATH, L"%ls.tmp", notice);
    if (GetFileAttributesW(temp) != INVALID_FILE_ATTRIBUTES) return 7147;
    if (!gc_crash_dump_notice_store(notice, nullptr, &error) ||
        GetFileAttributesW(notice) != INVALID_FILE_ATTRIBUTES ||
        !gc_crash_dump_notice_store(notice, "", &error) ||
        gc_crash_dump_notice_store(nullptr, "x", &error) ||
        error != ERROR_INVALID_PARAMETER) return 7147;
    return 0;
}

static int crash_dump_notice_win32_tests() {
    WCHAR tempRoot[MAX_PATH] = {};
    DWORD n = GetTempPathW(MAX_PATH, tempRoot);
    if (n == 0 || n >= MAX_PATH) return 7145;
    WCHAR dir[MAX_PATH] = {};
    StringCchPrintfW(dir, MAX_PATH, L"%lsgc_crash_notice_%lu_%llu", tempRoot,
             (unsigned long)GetCurrentProcessId(), (unsigned long long)GetTickCount64());
    if (!CreateDirectoryW(dir, nullptr)) return 7145;
    int failure = crash_dump_notice_win32_tests_in(dir);
    notice_remove_tree(dir);
    return failure;
}

#endif

int run_crash_dump_notice_tests() {
    if (int failure = crash_dump_notice_policy_tests()) return failure;
#if defined(_WIN32)
    if (int failure = crash_dump_notice_win32_tests()) return failure;
#endif
    return 0;
}
