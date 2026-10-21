// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_DEBUG_LOG_IDENTITY_H
#define GREEN_CURVE_DEBUG_LOG_IDENTITY_H

#include <windows.h>
#include <shlobj.h>
#include <string.h>

// A queued destination owns an impersonation token, independently of the
// session that is current when the writer eventually drains the record.
struct GcDebugLogRoute {
    char path[MAX_PATH] = {};
    HANDLE userToken = nullptr;
    unsigned short generation = 0;
    LUID tokenId = {};
    bool userDestination = false;

    GcDebugLogRoute() = default;
    GcDebugLogRoute(const GcDebugLogRoute&) = delete;
    GcDebugLogRoute& operator=(const GcDebugLogRoute&) = delete;
    ~GcDebugLogRoute() { reset(); }
    void reset() {
        if (userToken) CloseHandle(userToken);
        userToken = nullptr;
        path[0] = 0;
        generation = 0;
        tokenId = {};
        userDestination = false;
    }
};

static inline HANDLE gc_debug_log_user_token(HANDLE primary) {
    if (!primary) return nullptr;
    TOKEN_ELEVATION_TYPE elevation = TokenElevationTypeDefault;
    DWORD bytes = 0;
    if (!GetTokenInformation(primary, TokenElevationType, &elevation,
                              sizeof(elevation), &bytes)) return nullptr;
    TOKEN_LINKED_TOKEN linked = {};
    HANDLE source = primary;
    if (elevation == TokenElevationTypeFull && !IsTokenRestricted(primary)) {
        if (!GetTokenInformation(primary, TokenLinkedToken, &linked,
                                  sizeof(linked), &bytes)) return nullptr;
        source = linked.LinkedToken;
    }
    HANDLE result = nullptr;
    if (!DuplicateTokenEx(source, TOKEN_QUERY | TOKEN_IMPERSONATE | TOKEN_DUPLICATE,
                          nullptr, SecurityImpersonation, TokenImpersonation,
                          &result)) result = nullptr;
    if (linked.LinkedToken) CloseHandle(linked.LinkedToken);
    return result;
}

// Preserve a caller's existing impersonation. Never substitute process rights
// when a user destination's identity is unavailable.
class GcDebugLogIdentity {
    HANDLE previous_ = nullptr;
    bool changed_ = false;
public:
    GcDebugLogIdentity(HANDLE token, bool userDestination) {
        if (userDestination && !token) return;
        if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY | TOKEN_IMPERSONATE,
                              TRUE, &previous_) && GetLastError() != ERROR_NO_TOKEN)
            return;
        changed_ = SetThreadToken(nullptr, token) != FALSE;
    }
    GcDebugLogIdentity(const GcDebugLogIdentity&) = delete;
    GcDebugLogIdentity& operator=(const GcDebugLogIdentity&) = delete;
    ~GcDebugLogIdentity() {
        if (changed_ && !SetThreadToken(nullptr, previous_)) {
            // Continuing under an unintended identity is never safe.
            TerminateProcess(GetCurrentProcess(), ERROR_CANNOT_IMPERSONATE);
        }
        if (previous_) CloseHandle(previous_);
    }
    bool valid() const { return changed_; }
};

// Called under the destination user's token. Known-folder redirects remain
// usable: traversing them carries that user's rights and network identity.
static inline bool gc_debug_log_user_directory(const char* path) {
    if (!path || !path[0]) return false;
    WCHAR wide[MAX_PATH] = {};
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide,
                             MAX_PATH)) return false;
    const DWORD existing = GetFileAttributesW(wide);
    if (existing != INVALID_FILE_ATTRIBUTES)
        return (existing & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const DWORD lookupError = GetLastError();
    if (lookupError != ERROR_FILE_NOT_FOUND && lookupError != ERROR_PATH_NOT_FOUND)
        return false;
    const int result = SHCreateDirectoryExW(nullptr, wide, nullptr);
    if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS &&
        result != ERROR_FILE_EXISTS) return false;
    const DWORD attributes = GetFileAttributesW(wide);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static inline bool gc_debug_log_user_parent(const char* path) {
    if (!path || !path[0] || strlen(path) >= MAX_PATH) return false;
    char parent[MAX_PATH] = {};
    memcpy(parent, path, strlen(path) + 1);
    char* slash = strrchr(parent, '\\');
    if (!slash) slash = strrchr(parent, '/');
    if (!slash) return true;
    *slash = 0;
    return gc_debug_log_user_directory(parent);
}

#endif
