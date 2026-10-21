// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#include <strsafe.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "debug_log_identity.h"
#include "debug_log_rotation_policy.h"
#include "win32_utf8_paths.h"

namespace {
#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
static CRITICAL_SECTION g_debugLogLock;
static HANDLE g_debugLogFile = INVALID_HANDLE_VALUE;
static char g_debugLogOpenPath[MAX_PATH] = {};
static char s_path[MAX_PATH] = {};
static bool app_is_service_process() { return true; }
static const char* effective_debug_log_path() { return s_path; }
static const char* service_early_debug_log_path() { return s_path; }
static DWORD debug_log_file_attributes() { return FILE_ATTRIBUTE_NORMAL; }
static int format_log_timestamp_prefix(char*, size_t) { return 0; }
static void write_crash_breadcrumb_direct(const char*) {}
static HANDLE open_debug_log_file_locked(const char* path, bool userDestination) {
    if (userDestination && !gc_debug_log_user_parent(path)) return INVALID_HANDLE_VALUE;
    return gc_CreateFileUtf8(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}
static void debug_log_set_route_path(const char*, HANDLE = nullptr);
#include "main_debug_log_writer.cpp"

static bool write_file(const char* path, const char* value) {
    HANDLE file = gc_CreateFileUtf8(path, GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, value, (DWORD)std::strlen(value), &written, nullptr) != FALSE;
    CloseHandle(file);
    return ok;
}

static bool protect_file(const char* path, HANDLE token, bool allowRestricted) {
    unsigned char buffer[512] = {};
    DWORD bytes = 0;
    if (!GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &bytes)) return false;
    WCHAR* sid = nullptr;
    if (!ConvertSidToStringSidW(((TOKEN_USER*)buffer)->User.Sid, &sid)) return false;
    WCHAR sddl[512] = {};
    StringCchPrintfW(sddl, ARRAY_COUNT(sddl), L"D:P(A;;FA;;;%ls)%ls", sid,
                     allowRestricted ? L"(A;;FA;;;RC)" : L"");
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl,
            SDDL_REVISION_1, &descriptor, nullptr)) return false;
    GcWideUtf8Arg wide(path);
    bool ok = wide.value && SetFileSecurityW(wide.value,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
    LocalFree(descriptor);
    return ok;
}

static unsigned long long file_size(const char* path) {
    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (!gc_GetFileAttributesExUtf8(path, GetFileExInfoStandard, &attributes)) return ~0ULL;
    return ((unsigned long long)attributes.nFileSizeHigh << 32) | attributes.nFileSizeLow;
}

static int exercise(HANDLE processToken, HANDLE restrictedToken, const char* directory) {
    char protectedPath[MAX_PATH] = {};
    char allowedPath[MAX_PATH] = {};
    StringCchPrintfA(protectedPath, ARRAY_COUNT(protectedPath), "%s\\protected.txt", directory);
    StringCchPrintfA(allowedPath, ARRAY_COUNT(allowedPath), "%s\\allowed.txt", directory);
    if (!protect_file(directory, processToken, true)) return 7018;
    if (!write_file(protectedPath, "kept") || !protect_file(protectedPath, processToken, false) ||
        !write_file(allowedPath, "") || !protect_file(allowedPath, processToken, true)) return 7000;

    // A cached process-identity handle must not cross into a user route, even
    // when the pathname itself is identical.
    debug_log_set_route_path(protectedPath);
    debug_log_write_line_locked("machine");
    if (file_size(protectedPath) != 11) return 7001;
    debug_log_set_route_path(protectedPath, restrictedToken);
    debug_log_write_line_locked("forbidden");
    if (file_size(protectedPath) != 11) return 7002;

    // A destination and its token survive slot replacement and source-handle
    // closure. Queue drain must not borrow the next session's permissions.
    debug_log_set_route_path(allowedPath, restrictedToken);
    GcDebugLogRoute saved;
    if (!debug_log_capture_route_locked(g_debugLogCurrentRouteGen, &saved)) return 7003;
    debug_log_set_route_path(protectedPath);
    for (unsigned int i = 0; i < gc_debug_log_queue::kMaxRouteSlots + 1; ++i)
        debug_log_set_route_path(allowedPath, restrictedToken);
    debug_log_write_line_to_path_locked("ok", saved);
    if (file_size(allowedPath) != 2) return 7004;

    GcDebugLogRoute absent;
    StringCchCopyA(absent.path, ARRAY_COUNT(absent.path), protectedPath);
    absent.userDestination = true;
    debug_log_write_line_to_path_locked("forbidden", absent);
    if (file_size(protectedPath) != 11) return 7005;

    // Actual production rotation runs with the destination's restricted token.
    debug_log_close_file_locked();
    HANDLE large = gc_CreateFileUtf8(allowedPath, GENERIC_WRITE, 0, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (large == INVALID_HANDLE_VALUE) return 7006;
    LARGE_INTEGER end = {};
    end.QuadPart = gc_debug_log_rotation::kRotateBytes + 1;
    bool resized = SetFilePointerEx(large, end, nullptr, FILE_BEGIN) && SetEndOfFile(large);
    CloseHandle(large);
    if (!resized) return 7007;
    debug_log_write_line_to_path_locked("after rotation", saved);
    if (file_size(allowedPath) >= (unsigned long long)end.QuadPart ||
        file_size(allowedPath) < std::strlen("after rotation")) return 7008;

    char redirected[MAX_PATH] = {}, leaf[MAX_PATH] = {};
    StringCchPrintfA(redirected, ARRAY_COUNT(redirected), "%s\\redirect", directory);
    if (CreateSymbolicLinkA(redirected, directory, 3)) {
        StringCchPrintfA(saved.path, ARRAY_COUNT(saved.path), "%s\\allowed.txt", redirected);
        const auto before = file_size(allowedPath);
        debug_log_write_line_to_path_locked("linked", saved);
        if (file_size(allowedPath) != before + 6) return 7019;
        RemoveDirectoryA(redirected);
    } else {
        std::printf("debug log identity: directory symlink fixture unavailable (%lu)\n", GetLastError());
    }
    StringCchPrintfA(leaf, ARRAY_COUNT(leaf), "%s\\leaf.txt", directory);
    if (CreateSymbolicLinkA(leaf, protectedPath, 2)) {
        StringCchCopyA(saved.path, ARRAY_COUNT(saved.path), leaf);
        debug_log_write_line_to_path_locked("forbidden", saved);
        if (file_size(protectedPath) != 11) return 7020;
        DeleteFileA(leaf);
    } else {
        std::printf("debug log identity: file symlink fixture unavailable (%lu)\n", GetLastError());
    }

    if (!SetThreadToken(nullptr, saved.userToken)) return 7009;
    {
        GcDebugLogIdentity machine(nullptr, false);
        if (!machine.valid()) return 7010;
        HANDLE unexpected = nullptr;
        if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &unexpected)) {
            CloseHandle(unexpected);
            return 7011;
        }
        if (GetLastError() != ERROR_NO_TOKEN) return 7012;
    }
    HANDLE restored = nullptr;
    bool restoredOk = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &restored) &&
                      IsTokenRestricted(restored);
    if (restored) CloseHandle(restored);
    RevertToSelf();
    return restoredOk ? 0 : 7013;
}
} // namespace
#endif

int run_debug_log_identity_tests() {
#if defined(_WIN32)
    InitializeCriticalSection(&g_debugLogLock);
    debug_log_initialize_locks();
    HANDLE process = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &process)) return 7014;
    unsigned char sid[SECURITY_MAX_SID_SIZE] = {};
    DWORD sidBytes = sizeof(sid);
    HANDLE restricted = nullptr;
    SID_AND_ATTRIBUTES restricting = {sid, 0};
    bool created = CreateWellKnownSid(WinRestrictedCodeSid, nullptr, sid, &sidBytes) &&
        CreateRestrictedToken(process, DISABLE_MAX_PRIVILEGE, 0, nullptr, 0,
                              nullptr, 1, &restricting, &restricted);
    if (!created) { CloseHandle(process); return 7015; }
    char temp[MAX_PATH] = {}, directory[MAX_PATH] = {};
    if (!GetTempPathA(MAX_PATH, temp) || !GetTempFileNameA(temp, "gcl", 0, directory)) return 7016;
    DeleteFileA(directory);
    if (!CreateDirectoryA(directory, nullptr)) return 7017;
    StringCchCopyA(s_path, ARRAY_COUNT(s_path), directory);
    int result = exercise(process, restricted, directory);
    RevertToSelf();
    debug_log_close_file_locked();
    for (auto& slot : g_debugLogRoutes) {
        if (slot.userToken) CloseHandle(slot.userToken);
        slot.userToken = nullptr;
    }
    CloseHandle(restricted);
    CloseHandle(process);
    char path[MAX_PATH] = {};
    for (const char* name : {"protected.txt", "allowed.txt"}) {
        StringCchPrintfA(path, ARRAY_COUNT(path), "%s\\%s", directory, name);
        DeleteFileA(path);
    }
    StringCchPrintfA(path, ARRAY_COUNT(path), "%s\\leaf.txt", directory);
    DeleteFileA(path);
    StringCchPrintfA(path, ARRAY_COUNT(path), "%s\\redirect", directory);
    RemoveDirectoryA(path);
    RemoveDirectoryA(directory);
    DeleteCriticalSection(&g_debugLogLock);
    DeleteCriticalSection(&g_debugLogFileLock);
    return result;
#else
    return 0;
#endif
}
