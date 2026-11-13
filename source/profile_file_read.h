// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_FILE_READ_H
#define GREEN_CURVE_PROFILE_FILE_READ_H
#include "profile_codec_types.h"
#if defined(_WIN32)
#include "win32_utf8_paths.h"
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// Client-side only. Validate a bounded regular file before allocating/reading;
// missing configuration may be empty, but a missing import never is.
static inline bool profile_file_read(const char* path, std::string& output, size_t maximum,
    bool missingAllowed, ProfileCodecError& error) {
    std::string result;
#if defined(_WIN32)
    HANDLE file = gc_CreateFileUtf8(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD status = GetLastError();
        if (missingAllowed && (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)) { output.clear(); return true; }
        return profile_codec::fail(&error, 0, "could not open profile file");
    }
    LARGE_INTEGER size{};
    if (GetFileType(file) != FILE_TYPE_DISK || !GetFileSizeEx(file, &size) ||
        size.QuadPart < 0 || (unsigned long long)size.QuadPart > maximum) {
        CloseHandle(file); return profile_codec::fail(&error, 0, "profile input is not a bounded regular file");
    }
    result.resize((size_t)size.QuadPart);
    size_t total = 0;
    while (total < result.size()) {
        DWORD read = 0;
        if (!ReadFile(file, result.data() + total, (DWORD)(result.size() - total), &read, nullptr) || !read) {
            CloseHandle(file); return profile_codec::fail(&error, 0, "profile file changed or could not be read");
        }
        total += read;
    }
    char extra = 0; DWORD read = 0;
    const bool stable = ReadFile(file, &extra, 1, &read, nullptr) && !read;
    CloseHandle(file);
#else
    const int file = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (file < 0) {
        if (missingAllowed && errno == ENOENT) { output.clear(); return true; }
        return profile_codec::fail(&error, 0, "could not open profile file");
    }
    struct stat status{};
    if (fstat(file, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0 ||
        (unsigned long long)status.st_size > maximum) {
        close(file); return profile_codec::fail(&error, 0, "profile input is not a bounded regular file");
    }
    result.resize((size_t)status.st_size);
    size_t total = 0;
    while (total < result.size()) {
        const ssize_t n = read(file, result.data() + total, result.size() - total);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(file); return profile_codec::fail(&error, 0, "profile file changed or could not be read"); }
        total += (size_t)n;
    }
    char extra = 0;
    ssize_t extraSize;
    do { extraSize = read(file, &extra, 1); } while (extraSize < 0 && errno == EINTR);
    const bool stable = extraSize == 0;
    close(file);
#endif
    if (!stable) return profile_codec::fail(&error, 0, "profile file changed while reading");
    output.swap(result);
    return true;
}
#endif
