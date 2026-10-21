// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Tells the user, in their own Green Curve folder, that the background service
// has crash dumps and where they are (crash_dump_notice_policy.h).  The dumps
// themselves stay in the admin-only machine data directory.
//
// Two halves around resolve_service_user_data_paths(): the scan runs with the
// service's own identity (the directory is admin-only), the write runs under
// the user's token so the service never writes into a user-controlled folder
// with its own rights.

#include "crash_artifact_policy.h"
#include "crash_dump_notice_win32.h"

enum ServiceCrashNoticeScan {
    SERVICE_CRASH_NOTICE_SCAN_FAILED = 0,  // leave any existing notice alone
    SERVICE_CRASH_NOTICE_SCAN_NONE,        // no dumps: remove the notice
    SERVICE_CRASH_NOTICE_SCAN_FOUND,       // write `text`
};

// Scans the service's dump folders and composes the notice for `userDataDir`.
static ServiceCrashNoticeScan service_compose_crash_dump_notice(
    const char* userDataDir, char* text, size_t textSize,
    size_t* dumpsOut, size_t* werDumpsOut) {
    *dumpsOut = 0;
    *werDumpsOut = 0;
    if (text && textSize) text[0] = 0;
    char machineDir[MAX_PATH] = {};
    char werDir[MAX_PATH] = {};
    char destination[MAX_PATH] = {};
    WCHAR machineDirW[MAX_PATH] = {};
    WCHAR werDirW[MAX_PATH] = {};
    if (!text || !userDataDir || !userDataDir[0] ||
        !resolve_service_machine_data_dir(machineDir, sizeof(machineDir)) ||
        FAILED(StringCchPrintfA(werDir, ARRAY_COUNT(werDir), "%s\\WerDumps", machineDir)) ||
        FAILED(StringCchPrintfA(destination, ARRAY_COUNT(destination), "%s\\crash-dumps",
            userDataDir)) ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, machineDir, -1, machineDirW, MAX_PATH) ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, werDir, -1, werDirW, MAX_PATH)) {
        return SERVICE_CRASH_NOTICE_SCAN_FAILED;
    }
    GcCrashDumpNoticeEntry dumps[16] = {};
    GcCrashDumpNoticeEntry werDumps[16] = {};
    size_t dumpsKept = 0, werKept = 0;
    bool scanned = false;
    {
        // Scan as the service even when the caller is impersonating a client:
        // the folder is readable by SYSTEM and Administrators only.
        GcDebugLogIdentity self(nullptr, false);
        scanned = self.valid() &&
            gc_crash_dump_notice_scan(machineDirW, L"greencurve_*.dmp", dumps,
                ARRAY_COUNT(dumps), &dumpsKept, dumpsOut) &&
            gc_crash_dump_notice_scan(werDirW, L"*.dmp", werDumps,
                ARRAY_COUNT(werDumps), &werKept, werDumpsOut);
    }
    if (!scanned) return SERVICE_CRASH_NOTICE_SCAN_FAILED;
    const size_t dumpsListed = gc_crash_dump_notice_keep_newest(
        dumps, dumpsKept, GC_CRASH_DUMP_NOTICE_MAX_LISTED);
    const size_t werListed = gc_crash_dump_notice_keep_newest(
        werDumps, werKept, GC_CRASH_DUMP_NOTICE_MAX_LISTED);
    return gc_crash_dump_notice_format(machineDir, werDir, dumps, dumpsListed, *dumpsOut,
               werDumps, werListed, *werDumpsOut, destination, text, textSize)
        ? SERVICE_CRASH_NOTICE_SCAN_FOUND : SERVICE_CRASH_NOTICE_SCAN_NONE;
}

// Writes or removes the notice.  Call under the destination user's token.
static void service_store_crash_dump_notice_as_user(const char* userDataDir,
    ServiceCrashNoticeScan scan, const char* text, size_t dumps, size_t werDumps) {
    if (scan == SERVICE_CRASH_NOTICE_SCAN_FAILED) {
        debug_log("crash dump notice: service dump folders could not be scanned; existing notice left as is\n");
        return;
    }
    char path[MAX_PATH] = {};
    WCHAR pathW[MAX_PATH] = {};
    if (FAILED(StringCchPrintfA(path, ARRAY_COUNT(path), "%s\\%s", userDataDir,
            GC_CRASH_DUMP_NOTICE_FILE_NAME)) ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, pathW, MAX_PATH)) {
        debug_log("crash dump notice: notice path unavailable\n");
        return;
    }
    DWORD error = ERROR_SUCCESS;
    const bool found = scan == SERVICE_CRASH_NOTICE_SCAN_FOUND;
    const bool ok = gc_crash_dump_notice_store(pathW, found ? text : nullptr, &error);
    debug_log("crash dump notice: %s serviceDumps=%zu werDumps=%zu ok=%d error=%lu\n",
        found ? "written" : "none (notice removed)", dumps, werDumps, ok ? 1 : 0,
        (unsigned long)error);
}
