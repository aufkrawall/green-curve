// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#pragma once

// Service write-path reparse-point and directory-containment checks.

static bool service_path_has_reparse_component(const char* absPath, char* err, size_t errSize) {
    if (!absPath || !absPath[0]) return false;
    char probe[MAX_PATH] = {};
    if (FAILED(StringCchCopyA(probe, ARRAY_COUNT(probe), absPath))) {
        set_message(err, errSize, "Path is too long");
        return true;
    }

    size_t len = strlen(probe);
    size_t rootLen = 0;
    if (len >= 3 && probe[1] == ':' && (probe[2] == '\\' || probe[2] == '/')) {
        rootLen = 3;
    } else if (len >= 2 && probe[0] == '\\' && probe[1] == '\\') {
        const char* serverEnd = strpbrk(probe + 2, "\\/");
        if (serverEnd) {
            const char* shareEnd = strpbrk(serverEnd + 1, "\\/");
            if (shareEnd) rootLen = (size_t)(shareEnd - probe + 1);
        }
    }

    if (rootLen == 0 || rootLen >= len) return false;
    for (size_t i = rootLen; i < len; i++) {
        if (probe[i] != '\\' && probe[i] != '/') continue;
        char saved = probe[i];
        probe[i] = 0;
        DWORD attrs = gc_GetFileAttributesUtf8(probe);
        probe[i] = saved;
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
            set_message(err, errSize, "Path crosses a reparse point");
            return true;
        }
    }

    char* slash = strrchr(probe, '\\');
    char* slashAlt = strrchr(probe, '/');
    if (!slash || (slashAlt && slashAlt > slash)) slash = slashAlt;
    if (slash && (size_t)(slash - probe) >= rootLen) {
        *slash = 0;
        DWORD attrs = gc_GetFileAttributesUtf8(probe);
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
            set_message(err, errSize, "Path crosses a reparse point");
            return true;
        }
    }
    return false;
}

static bool normalize_windows_compare_path(const WCHAR* input, WCHAR* output,
    size_t outputCount) {
    if (!input || !output || outputCount == 0) return false;
    output[0] = 0;
    if (wcsncmp(input, L"\\\\?\\UNC\\", 8) == 0) {
        return SUCCEEDED(StringCchCopyW(output, outputCount, L"\\\\")) &&
            SUCCEEDED(StringCchCatW(output, outputCount, input + 8));
    }
    const WCHAR* start = wcsncmp(input, L"\\\\?\\", 4) == 0
        ? input + 4 : input;
    return SUCCEEDED(StringCchCopyW(output, outputCount, start));
}

// Containment against an arbitrary root.
//
// Both service write scopes need identical canonicalization and identical
// boundary handling -- equal, or followed by a separator, so `%USERPROFILE%2`
// is not treated as inside `%USERPROFILE%`.  Only the root and the refusal
// wording differ, so factoring this out is what lets the machine-config scope
// exist without a second, subtly different comparison.
static bool service_path_is_within_directory(const char* candidateUtf8,
    const char* rootUtf8, const char* outsideMessage, char* err, size_t errSize) {
    if (!candidateUtf8 || !candidateUtf8[0] || !rootUtf8 || !rootUtf8[0]) {
        set_message(err, errSize, "Containment root is unavailable");
        return false;
    }
    GcWideUtf8Arg candidate(candidateUtf8);
    GcWideUtf8Arg profile(rootUtf8);
    if (!candidate.valid_for(candidateUtf8) ||
        !profile.valid_for(rootUtf8)) {
        set_message(err, errSize, "Path contains invalid UTF-8");
        return false;
    }
    WCHAR candidateFull[MAX_PATH] = {};
    WCHAR profileFull[MAX_PATH] = {};
    DWORD candidateLength = GetFullPathNameW(candidate.value,
        ARRAY_COUNT(candidateFull), candidateFull, nullptr);
    DWORD profileLength = GetFullPathNameW(profile.value,
        ARRAY_COUNT(profileFull), profileFull, nullptr);
    if (candidateLength == 0 || candidateLength >= ARRAY_COUNT(candidateFull) ||
        profileLength == 0 || profileLength >= ARRAY_COUNT(profileFull)) {
        set_message(err, errSize, "Path cannot be canonicalized");
        return false;
    }
    WCHAR candidateNormalized[MAX_PATH] = {};
    WCHAR profileNormalized[MAX_PATH] = {};
    if (!normalize_windows_compare_path(candidateFull, candidateNormalized,
            ARRAY_COUNT(candidateNormalized)) ||
        !normalize_windows_compare_path(profileFull, profileNormalized,
            ARRAY_COUNT(profileNormalized))) {
        set_message(err, errSize, "Canonical path is too long");
        return false;
    }
    size_t profileChars = wcslen(profileNormalized);
    // A root carrying a trailing separator would make every child fail the
    // boundary test below -- `candidate[profileChars]` would land on the
    // child's first character rather than on a separator -- and the result is a
    // silent refusal that looks exactly like a genuine containment violation.
    while (profileChars > 0 && profileNormalized[profileChars - 1] == L'\\') {
        profileNormalized[--profileChars] = L'\0';
    }
    size_t candidateChars = wcslen(candidateNormalized);
    if (candidateChars < profileChars ||
        CompareStringOrdinal(candidateNormalized, (int)profileChars,
            profileNormalized, (int)profileChars, TRUE) != CSTR_EQUAL ||
        (candidateChars > profileChars &&
         candidateNormalized[profileChars] != L'\\')) {
        set_message(err, errSize, "%s", outsideMessage ? outsideMessage
                                                       : "Path is outside its allowed directory");
        return false;
    }
    return true;
}
