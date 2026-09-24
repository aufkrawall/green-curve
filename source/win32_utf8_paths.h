// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_WIN32_UTF8_PATHS_H
#define GREEN_CURVE_WIN32_UTF8_PATHS_H

#if defined(_WIN32)

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

// Green Curve stores paths and INI text as UTF-8.  Win32's *A entry points use
// the process ANSI code page, so passing those paths to them corrupts names that
// are not representable in that code page.  These wrappers are the sole boundary
// between the UTF-8 model and UTF-16 Win32 file/profile APIs.  Conversion is
// strict; malformed UTF-8 fails rather than silently selecting another path.

struct GcWideUtf8Arg {
    WCHAR* value;

    explicit GcWideUtf8Arg(const char* utf8) : value(nullptr) {
        if (!utf8) return;
        int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            utf8, -1, nullptr, 0);
        if (count <= 0) return;
        value = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            (SIZE_T)count * sizeof(WCHAR));
        if (!value || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                utf8, -1, value, count) != count) {
            if (value) HeapFree(GetProcessHeap(), 0, value);
            value = nullptr;
        }
    }

    ~GcWideUtf8Arg() {
        if (value) HeapFree(GetProcessHeap(), 0, value);
    }

    bool valid_for(const char* original) const {
        return original == nullptr || value != nullptr;
    }

    GcWideUtf8Arg(const GcWideUtf8Arg&) = delete;
    GcWideUtf8Arg& operator=(const GcWideUtf8Arg&) = delete;
};

using Win32Utf8Path = GcWideUtf8Arg;

static inline void gc_utf8_path_conversion_failed(const char* operation) {
    DWORD error = GetLastError();
    if (error == ERROR_SUCCESS) error = ERROR_NO_UNICODE_TRANSLATION;
    char message[160] = {};
    StringCchPrintfA(message, sizeof(message),
        "Green Curve UTF-8 path conversion failed: %s (error %lu)\n",
        operation ? operation : "unknown", (unsigned long)error);
    OutputDebugStringA(message);
    SetLastError(error);
}

static inline bool gc_wide_to_utf8(const WCHAR* wide, char* out,
    DWORD outSize, DWORD* writtenOut = nullptr) {
    if (writtenOut) *writtenOut = 0;
    if (!wide || !out || outSize == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        wide, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 0) return false;
    if ((DWORD)required > outSize) {
        out[0] = 0;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return false;
    }
    int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        wide, -1, out, (int)outSize, nullptr, nullptr);
    if (written <= 0) {
        out[0] = 0;
        return false;
    }
    if (writtenOut) *writtenOut = (DWORD)(written - 1);
    return true;
}

static inline WCHAR* gc_utf8_multisz_to_wide(const char* input) {
    if (!input) return nullptr;
    size_t totalWide = 1;
    const char* cursor = input;
    while (*cursor) {
        int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            cursor, -1, nullptr, 0);
        if (count <= 0) return nullptr;
        totalWide += (size_t)count;
        cursor += strlen(cursor) + 1;
    }
    WCHAR* result = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        totalWide * sizeof(WCHAR));
    if (!result) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    WCHAR* destination = result;
    cursor = input;
    while (*cursor) {
        int remaining = (int)(totalWide - (size_t)(destination - result));
        int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            cursor, -1, destination, remaining);
        if (count <= 0) {
            HeapFree(GetProcessHeap(), 0, result);
            return nullptr;
        }
        destination += count;
        cursor += strlen(cursor) + 1;
    }
    *destination = L'\0';
    return result;
}

static inline DWORD gc_wide_multisz_to_utf8(const WCHAR* input, char* output,
    DWORD outputSize, bool multiString) {
    if (!output || outputSize == 0 || !input) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    output[0] = 0;
    if (multiString && outputSize > 1) output[1] = 0;
    DWORD used = 0;
    const WCHAR* cursor = input;
    do {
        int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            cursor, -1, nullptr, 0, nullptr, nullptr);
        if (required <= 0) return 0;
        DWORD reserve = multiString ? 1u : 0u;
        if (used + (DWORD)required + reserve > outputSize) {
            if (multiString) {
                output[outputSize - 1] = 0;
                if (outputSize > 1) output[outputSize - 2] = 0;
                return outputSize > 1 ? outputSize - 2 : 0;
            }
            output[0] = 0;
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return outputSize - 1;
        }
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, cursor, -1,
                output + used, (int)(outputSize - used), nullptr, nullptr) <= 0) {
            output[0] = 0;
            return 0;
        }
        used += (DWORD)required;
        if (!multiString) return used - 1;
        cursor += wcslen(cursor) + 1;
    } while (*cursor);
    if (used >= outputSize) return outputSize > 1 ? outputSize - 2 : 0;
    output[used] = 0;
    return used;
}

// The on-disk form of an INI file this program writes byte-for-byte.
//
// The profile API (GetPrivateProfileStringW) decodes a file without a UTF-16
// BOM in the system ANSI code page, and WritePrivateProfileStringW encodes into
// it.  A writer that emits Green Curve's in-memory UTF-8 text verbatim
// therefore stores bytes the reader later decodes as a DIFFERENT string: an
// "ä" written as C3 A4 comes back as "Ã¤", and every later save encodes that
// again.  This is the one conversion that keeps a whole-file INI writer in the
// encoding the reader uses.
//
// Strict both ways: malformed UTF-8, or a character the ANSI code page cannot
// hold, fails instead of being replaced with '?' -- a lossy save would turn a
// rule's pattern into one that silently never matches.  ASCII text (every
// numeric and key field) is copied unchanged.  Returns a HeapAlloc'd,
// NUL-terminated buffer the caller frees with HeapFree.
enum GcIniEncodeResult {
    GC_INI_ENCODE_OK = 0,
    GC_INI_ENCODE_INVALID_UTF8 = 1,
    GC_INI_ENCODE_UNREPRESENTABLE = 2,
    GC_INI_ENCODE_NO_MEMORY = 3,
};

static inline const char* gc_ini_encode_result_name(GcIniEncodeResult result) {
    switch (result) {
        case GC_INI_ENCODE_OK: return "ok";
        case GC_INI_ENCODE_INVALID_UTF8: return "invalid-utf8";
        case GC_INI_ENCODE_UNREPRESENTABLE: return "unrepresentable-in-ansi-code-page";
        case GC_INI_ENCODE_NO_MEMORY: return "no-memory";
    }
    return "unknown";
}

static inline GcIniEncodeResult gc_utf8_to_ini_file_bytes(const char* utf8,
    char** out, size_t* outLen) {
    if (out) *out = nullptr;
    if (outLen) *outLen = 0;
    if (!utf8 || !out || !outLen) return GC_INI_ENCODE_INVALID_UTF8;
    size_t len = strlen(utf8);
    bool ascii = true;
    for (size_t i = 0; i < len; ++i) {
        if ((unsigned char)utf8[i] >= 0x80) { ascii = false; break; }
    }
    // CP_UTF8 as the system code page (the "Beta: use UTF-8" option) makes the
    // reader decode UTF-8, so the text is already in the reader's encoding.
    if (ascii || GetACP() == CP_UTF8) {
        if (!ascii && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                utf8, (int)len, nullptr, 0) <= 0)
            return GC_INI_ENCODE_INVALID_UTF8;
        char* copy = (char*)HeapAlloc(GetProcessHeap(), 0, len + 1);
        if (!copy) return GC_INI_ENCODE_NO_MEMORY;
        memcpy(copy, utf8, len + 1);
        *out = copy;
        *outLen = len;
        return GC_INI_ENCODE_OK;
    }
    int wideCount = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        utf8, (int)len, nullptr, 0);
    if (wideCount <= 0) return GC_INI_ENCODE_INVALID_UTF8;
    WCHAR* wide = (WCHAR*)HeapAlloc(GetProcessHeap(), 0,
        (SIZE_T)wideCount * sizeof(WCHAR));
    if (!wide) return GC_INI_ENCODE_NO_MEMORY;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, (int)len,
            wide, wideCount) != wideCount) {
        HeapFree(GetProcessHeap(), 0, wide);
        return GC_INI_ENCODE_INVALID_UTF8;
    }
    BOOL usedDefault = FALSE;
    int bytes = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide,
        wideCount, nullptr, 0, nullptr, &usedDefault);
    if (bytes <= 0 || usedDefault) {
        HeapFree(GetProcessHeap(), 0, wide);
        return GC_INI_ENCODE_UNREPRESENTABLE;
    }
    char* encoded = (char*)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)bytes + 1);
    if (!encoded) {
        HeapFree(GetProcessHeap(), 0, wide);
        return GC_INI_ENCODE_NO_MEMORY;
    }
    usedDefault = FALSE;
    int written = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide,
        wideCount, encoded, bytes, nullptr, &usedDefault);
    HeapFree(GetProcessHeap(), 0, wide);
    if (written != bytes || usedDefault) {
        HeapFree(GetProcessHeap(), 0, encoded);
        return GC_INI_ENCODE_UNREPRESENTABLE;
    }
    encoded[bytes] = 0;
    *out = encoded;
    *outLen = (size_t)bytes;
    return GC_INI_ENCODE_OK;
}

// UTF-8 from a UTF-16 string that may not fit: truncated at a whole code point
// instead of failing outright (gc_wide_to_utf8 returns nothing when the buffer
// is short, which turned a long window title into an empty one).
static inline void gc_wide_to_utf8_truncating(const WCHAR* wide, char* out,
    size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = 0;
    if (!wide) return;
    size_t used = 0;
    for (const WCHAR* p = wide; *p; ) {
        int units = 1;
        if (*p >= 0xD800 && *p <= 0xDBFF && p[1] >= 0xDC00 && p[1] <= 0xDFFF)
            units = 2;
        char encoded[8] = {};
        int bytes = WideCharToMultiByte(CP_UTF8, 0, p, units, encoded,
            (int)sizeof(encoded), nullptr, nullptr);
        if (bytes <= 0) break;
        if (used + (size_t)bytes + 1 > outSize) break;
        memcpy(out + used, encoded, (size_t)bytes);
        used += (size_t)bytes;
        p += units;
    }
    out[used] = 0;
}

static inline HANDLE gc_CreateFileUtf8(const char* path, DWORD access,
    DWORD share, LPSECURITY_ATTRIBUTES security, DWORD creation,
    DWORD attributes, HANDLE templateFile) {
    GcWideUtf8Arg wide(path);
    if (!wide.valid_for(path)) {
        gc_utf8_path_conversion_failed("CreateFile");
        return INVALID_HANDLE_VALUE;
    }
    return CreateFileW(wide.value, access, share, security, creation,
        attributes, templateFile);
}

static inline DWORD gc_GetFileAttributesUtf8(const char* path) {
    GcWideUtf8Arg wide(path);
    if (!wide.valid_for(path)) {
        gc_utf8_path_conversion_failed("GetFileAttributes");
        return INVALID_FILE_ATTRIBUTES;
    }
    return GetFileAttributesW(wide.value);
}

static inline BOOL gc_GetFileAttributesExUtf8(const char* path,
    GET_FILEEX_INFO_LEVELS level, LPVOID data) {
    GcWideUtf8Arg wide(path);
    if (!wide.valid_for(path)) {
        gc_utf8_path_conversion_failed("GetFileAttributesEx");
        return FALSE;
    }
    return GetFileAttributesExW(wide.value, level, data);
}

static inline BOOL gc_SetFileAttributesUtf8(const char* path, DWORD attributes) {
    GcWideUtf8Arg wide(path);
    if (!wide.valid_for(path)) {
        gc_utf8_path_conversion_failed("SetFileAttributes");
        return FALSE;
    }
    return SetFileAttributesW(wide.value, attributes);
}

static inline BOOL gc_DeleteFileUtf8(const char* path) {
    GcWideUtf8Arg wide(path);
    if (!wide.valid_for(path)) {
        gc_utf8_path_conversion_failed("DeleteFile");
        return FALSE;
    }
    return DeleteFileW(wide.value);
}

static inline BOOL gc_MoveFileExUtf8(const char* from, const char* to,
    DWORD flags) {
    GcWideUtf8Arg wideFrom(from), wideTo(to);
    if (!wideFrom.valid_for(from) || !wideTo.valid_for(to)) {
        gc_utf8_path_conversion_failed("MoveFileEx");
        return FALSE;
    }
    return MoveFileExW(wideFrom.value, wideTo.value, flags);
}

static inline BOOL gc_CopyFileUtf8(const char* from, const char* to,
    BOOL failIfExists) {
    GcWideUtf8Arg wideFrom(from), wideTo(to);
    if (!wideFrom.valid_for(from) || !wideTo.valid_for(to)) {
        gc_utf8_path_conversion_failed("CopyFile");
        return FALSE;
    }
    return CopyFileW(wideFrom.value, wideTo.value, failIfExists);
}

static inline BOOL gc_CreateDirectoryUtf8(const char* path,
    LPSECURITY_ATTRIBUTES security) {
    GcWideUtf8Arg wide(path);
    if (!wide.valid_for(path)) {
        gc_utf8_path_conversion_failed("CreateDirectory");
        return FALSE;
    }
    return CreateDirectoryW(wide.value, security);
}

static inline BOOL gc_RemoveDirectoryUtf8(const char* path) {
    GcWideUtf8Arg wide(path);
    if (!wide.valid_for(path)) {
        gc_utf8_path_conversion_failed("RemoveDirectory");
        return FALSE;
    }
    return RemoveDirectoryW(wide.value);
}

static inline DWORD gc_GetFullPathNameUtf8(const char* path, DWORD size,
    char* output, char** filePart) {
    if (filePart) *filePart = nullptr;
    GcWideUtf8Arg wide(path);
    if (!wide.valid_for(path) || !output || size == 0) {
        gc_utf8_path_conversion_failed("GetFullPathName");
        return 0;
    }
    WCHAR full[MAX_PATH] = {};
    DWORD length = GetFullPathNameW(wide.value, (DWORD)(sizeof(full) / sizeof(full[0])), full, nullptr);
    if (length == 0 || length >= (DWORD)(sizeof(full) / sizeof(full[0]))) return length;
    DWORD written = 0;
    if (!gc_wide_to_utf8(full, output, size, &written)) return size;
    if (filePart) {
        char* slash = strrchr(output, '\\');
        char* alt = strrchr(output, '/');
        if (!slash || (alt && alt > slash)) slash = alt;
        *filePart = slash ? slash + 1 : output;
    }
    return written;
}

static inline DWORD gc_GetFinalPathNameByHandleUtf8(HANDLE file, char* output,
    DWORD size, DWORD flags) {
    WCHAR full[MAX_PATH] = {};
    DWORD length = GetFinalPathNameByHandleW(file, full, (DWORD)(sizeof(full) / sizeof(full[0])), flags);
    if (length == 0 || length >= (DWORD)(sizeof(full) / sizeof(full[0]))) return length;
    DWORD written = 0;
    if (!gc_wide_to_utf8(full, output, size, &written)) return size;
    return written;
}

static inline DWORD gc_GetModuleFileNameUtf8(HMODULE module, char* output,
    DWORD size) {
    WCHAR path[MAX_PATH] = {};
    DWORD length = GetModuleFileNameW(module, path, (DWORD)(sizeof(path) / sizeof(path[0])));
    if (length == 0 || length >= (DWORD)(sizeof(path) / sizeof(path[0]))) return length;
    DWORD written = 0;
    if (!gc_wide_to_utf8(path, output, size, &written)) return size;
    return written;
}

static inline UINT gc_GetSystemDirectoryUtf8(char* output, UINT size) {
    WCHAR path[MAX_PATH] = {};
    UINT length = GetSystemDirectoryW(path, (UINT)(sizeof(path) / sizeof(path[0])));
    if (length == 0 || length >= (UINT)(sizeof(path) / sizeof(path[0]))) return length;
    DWORD written = 0;
    if (!gc_wide_to_utf8(path, output, size, &written)) return size;
    return (UINT)written;
}

static inline DWORD gc_GetPrivateProfileStringUtf8(const char* section,
    const char* key, const char* defaultValue, char* output, DWORD size,
    const char* path) {
    GcWideUtf8Arg wideSection(section), wideKey(key), wideDefault(defaultValue),
        widePath(path);
    if (!wideSection.valid_for(section) || !wideKey.valid_for(key) ||
        !wideDefault.valid_for(defaultValue) || !widePath.valid_for(path)) {
        gc_utf8_path_conversion_failed("GetPrivateProfileString");
        if (output && size) output[0] = 0;
        return 0;
    }
    DWORD wideSize = size ? size : 1;
    WCHAR* wideOutput = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        (SIZE_T)wideSize * sizeof(WCHAR));
    if (!wideOutput) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    DWORD result = GetPrivateProfileStringW(wideSection.value, wideKey.value,
        wideDefault.value, wideOutput, wideSize, widePath.value);
    bool multiString = section == nullptr || key == nullptr;
    DWORD converted = gc_wide_multisz_to_utf8(wideOutput, output, size,
        multiString);
    HeapFree(GetProcessHeap(), 0, wideOutput);
    (void)result;
    return converted;
}

static inline DWORD gc_GetPrivateProfileSectionUtf8(const char* section,
    char* output, DWORD size, const char* path) {
    GcWideUtf8Arg wideSection(section), widePath(path);
    if (!wideSection.valid_for(section) || !widePath.valid_for(path)) {
        gc_utf8_path_conversion_failed("GetPrivateProfileSection");
        return 0;
    }
    WCHAR* wideOutput = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        (SIZE_T)(size ? size : 1) * sizeof(WCHAR));
    if (!wideOutput) return 0;
    GetPrivateProfileSectionW(wideSection.value, wideOutput, size,
        widePath.value);
    DWORD converted = gc_wide_multisz_to_utf8(wideOutput, output, size, true);
    HeapFree(GetProcessHeap(), 0, wideOutput);
    return converted;
}

static inline BOOL gc_WritePrivateProfileStringUtf8(const char* section,
    const char* key, const char* value, const char* path) {
    GcWideUtf8Arg wideSection(section), wideKey(key), wideValue(value),
        widePath(path);
    if (!wideSection.valid_for(section) || !wideKey.valid_for(key) ||
        !wideValue.valid_for(value) || !widePath.valid_for(path)) {
        gc_utf8_path_conversion_failed("WritePrivateProfileString");
        return FALSE;
    }
    return WritePrivateProfileStringW(wideSection.value, wideKey.value,
        wideValue.value, widePath.value);
}

static inline BOOL gc_WritePrivateProfileSectionUtf8(const char* section,
    const char* values, const char* path) {
    GcWideUtf8Arg wideSection(section), widePath(path);
    WCHAR* wideValues = gc_utf8_multisz_to_wide(values);
    if (!wideSection.valid_for(section) || !widePath.valid_for(path) ||
        (values && !wideValues)) {
        if (wideValues) HeapFree(GetProcessHeap(), 0, wideValues);
        gc_utf8_path_conversion_failed("WritePrivateProfileSection");
        return FALSE;
    }
    BOOL result = WritePrivateProfileSectionW(wideSection.value, wideValues,
        widePath.value);
    if (wideValues) HeapFree(GetProcessHeap(), 0, wideValues);
    return result;
}

static inline bool gc_copy_find_data_utf8(const WIN32_FIND_DATAW* input,
    WIN32_FIND_DATAA* output) {
    if (!input || !output) return false;
    memset(output, 0, sizeof(*output));
    output->dwFileAttributes = input->dwFileAttributes;
    output->ftCreationTime = input->ftCreationTime;
    output->ftLastAccessTime = input->ftLastAccessTime;
    output->ftLastWriteTime = input->ftLastWriteTime;
    output->nFileSizeHigh = input->nFileSizeHigh;
    output->nFileSizeLow = input->nFileSizeLow;
    output->dwReserved0 = input->dwReserved0;
    output->dwReserved1 = input->dwReserved1;
    return gc_wide_to_utf8(input->cFileName, output->cFileName,
            (DWORD)(sizeof(output->cFileName) / sizeof(output->cFileName[0]))) &&
        gc_wide_to_utf8(input->cAlternateFileName,
            output->cAlternateFileName,
            (DWORD)(sizeof(output->cAlternateFileName) / sizeof(output->cAlternateFileName[0])));
}

static inline HANDLE gc_FindFirstFileUtf8(const char* pattern,
    WIN32_FIND_DATAA* data) {
    GcWideUtf8Arg wide(pattern);
    if (!wide.valid_for(pattern)) {
        gc_utf8_path_conversion_failed("FindFirstFile");
        return INVALID_HANDLE_VALUE;
    }
    WIN32_FIND_DATAW wideData = {};
    HANDLE handle = FindFirstFileW(wide.value, &wideData);
    if (handle != INVALID_HANDLE_VALUE && !gc_copy_find_data_utf8(&wideData, data)) {
        FindClose(handle);
        return INVALID_HANDLE_VALUE;
    }
    return handle;
}

static inline BOOL gc_FindNextFileUtf8(HANDLE handle, WIN32_FIND_DATAA* data) {
    WIN32_FIND_DATAW wideData = {};
    if (!FindNextFileW(handle, &wideData)) return FALSE;
    if (!gc_copy_find_data_utf8(&wideData, data)) return FALSE;
    return TRUE;
}

static inline HANDLE gc_FindFirstChangeNotificationUtf8(const char* path,
    BOOL subtree, DWORD filter) {
    GcWideUtf8Arg wide(path);
    if (!wide.valid_for(path)) {
        gc_utf8_path_conversion_failed("FindFirstChangeNotification");
        return INVALID_HANDLE_VALUE;
    }
    return FindFirstChangeNotificationW(wide.value, subtree, filter);
}

static inline FILE* gc_fopen_utf8(const char* path, const char* mode) {
    GcWideUtf8Arg widePath(path), wideMode(mode);
    if (!widePath.valid_for(path) || !wideMode.valid_for(mode)) {
        gc_utf8_path_conversion_failed("fopen");
        errno = EINVAL;
        return nullptr;
    }
    return _wfopen(widePath.value, wideMode.value);
}

#endif // _WIN32
#endif // GREEN_CURVE_WIN32_UTF8_PATHS_H
