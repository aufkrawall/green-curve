// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Failure log and small Win32 helpers for the setup program.
//
// The log is buffered in memory for the whole run and written out only if a
// step failed.  A successful install must leave nothing behind next to the
// setup file, but a failed one has to explain itself without asking the user to
// reproduce the problem under a debugger — so the full transcript, including
// the steps that succeeded, is kept until the outcome is known.

#include "installer_common.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Enough for the whole transcript of an install; older lines are dropped rather
// than growing without bound, and the drop is recorded so a truncated log never
// looks complete.
#define GC_LOG_CAPACITY 32768

static char s_logBuffer[GC_LOG_CAPACITY];
static size_t s_logUsed = 0;
static bool s_logTruncated = false;
static bool s_logFailed = false;
static WCHAR s_logPath[MAX_PATH] = {};
static bool s_logPathResolved = false;
static CRITICAL_SECTION s_logLock;
static bool s_logLockReady = false;

[[gnu::format(printf, 2, 0)]]
static void gc_log_append(const char* prefix, const char* fmt, va_list args) {
    char line[1024] = {};
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    // snprintf (C99) rather than the _s variants: llvm-mingw's ucrt headers only
    // declare those behind __STDC_WANT_SECURE_LIB__, and truncation is handled
    // explicitly below anyway.
    int header = snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u] %s",
                          now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, prefix);
    if (header < 0) header = 0;
    if ((size_t)header >= sizeof(line)) header = (int)sizeof(line) - 1;
    int body = vsnprintf(line + header, sizeof(line) - (size_t)header, fmt, args);
    if (body < 0) body = 0;
    size_t length = (size_t)header + (size_t)body;
    if (length + 2 >= sizeof(line)) length = sizeof(line) - 2;
    line[length++] = '\n';
    line[length] = 0;

    if (s_logLockReady) EnterCriticalSection(&s_logLock);
    if (s_logUsed + length + 1 >= sizeof(s_logBuffer)) {
        s_logTruncated = true;
    } else {
        memcpy(s_logBuffer + s_logUsed, line, length);
        s_logUsed += length;
        s_logBuffer[s_logUsed] = 0;
    }
    if (s_logLockReady) LeaveCriticalSection(&s_logLock);

    OutputDebugStringA(line);
}

void gc_log_init(const WCHAR* overridePath) {
    if (!s_logLockReady) {
        InitializeCriticalSection(&s_logLock);
        s_logLockReady = true;
    }
    if (overridePath && overridePath[0]) {
        WCHAR directory[MAX_PATH] = {};
        if (gc_module_directory(directory, GC_ARRAY_COUNT(directory)) &&
            gc_join_path(directory, overridePath, s_logPath,
                         GC_ARRAY_COUNT(s_logPath))) {
            s_logPathResolved = true;
        }
    }
    if (!s_logPathResolved) {
        // Next to the setup executable, which is where a user looking for "the
        // log" will actually look.
        WCHAR directory[MAX_PATH] = {};
        if (gc_module_directory(directory, GC_ARRAY_COUNT(directory)) &&
            gc_join_path(directory, L"greencurve-setup-error.log",
                         s_logPath, GC_ARRAY_COUNT(s_logPath))) {
            s_logPathResolved = true;
        }
    }
    gc_log_step("Green Curve setup " APP_VERSION " starting");
}

void gc_log_step(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    gc_log_append("", fmt, args);
    va_end(args);
}

void gc_log_fail(const char* fmt, ...) {
    s_logFailed = true;
    va_list args;
    va_start(args, fmt);
    gc_log_append("FAILED: ", fmt, args);
    va_end(args);
}

bool gc_log_had_failure() { return s_logFailed; }

static HANDLE gc_create_new_log_file(WCHAR* path, size_t pathCount) {
    if (!path || pathCount == 0 || !path[0]) return INVALID_HANDLE_VALUE;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (file != INVALID_HANDLE_VALUE ||
        (GetLastError() != ERROR_FILE_EXISTS &&
         GetLastError() != ERROR_ALREADY_EXISTS)) return file;

    WCHAR directory[MAX_PATH] = {};
    if (!gc_directory_of(path, directory, GC_ARRAY_COUNT(directory)))
        return INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 8; ++attempt) {
        GUID id = {};
        if (FAILED(CoCreateGuid(&id))) break;
        WCHAR leaf[96] = {};
        if (FAILED(StringCchPrintfW(leaf, GC_ARRAY_COUNT(leaf),
                L"greencurve-setup-error-%08lx%04x%04x.log",
                id.Data1, id.Data2, id.Data3)) ||
            !gc_join_path(directory, leaf, path, pathCount)) break;
        file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        if (file != INVALID_HANDLE_VALUE) return file;
        if (GetLastError() != ERROR_FILE_EXISTS &&
            GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
    return INVALID_HANDLE_VALUE;
}

const WCHAR* gc_log_flush_on_failure() {
    if (!s_logFailed || !s_logPathResolved) return nullptr;
    HANDLE file = gc_create_new_log_file(
        s_logPath, GC_ARRAY_COUNT(s_logPath));
    if (file == INVALID_HANDLE_VALUE) {
        // Falling back to %TEMP% matters: the common failure mode is a setup
        // file launched from a read-only location such as a mounted image.
        WCHAR tempDir[MAX_PATH] = {};
        WCHAR fallback[MAX_PATH] = {};
        if (GetTempPathW(GC_ARRAY_COUNT(tempDir), tempDir) > 0 &&
            gc_join_path(tempDir, L"greencurve-setup-error.log", fallback, GC_ARRAY_COUNT(fallback))) {
            file = gc_create_new_log_file(fallback, GC_ARRAY_COUNT(fallback));
            if (file != INVALID_HANDLE_VALUE) {
                StringCchCopyW(s_logPath, GC_ARRAY_COUNT(s_logPath), fallback);
            }
        }
        if (file == INVALID_HANDLE_VALUE) return nullptr;
    }
    const char* note = s_logTruncated
        ? "(earlier lines were dropped: the transcript exceeded the in-memory buffer)\r\n"
        : nullptr;
    DWORD written = 0;
    if (note) WriteFile(file, note, (DWORD)strlen(note), &written, nullptr);
    // The buffer holds LF line endings; expand to CRLF so Notepad shows it as
    // one line per step.
    size_t start = 0;
    for (size_t i = 0; i <= s_logUsed; i++) {
        if (i == s_logUsed || s_logBuffer[i] == '\n') {
            if (i > start) WriteFile(file, s_logBuffer + start, (DWORD)(i - start), &written, nullptr);
            if (i < s_logUsed) WriteFile(file, "\r\n", 2, &written, nullptr);
            start = i + 1;
        }
    }
    CloseHandle(file);
    return s_logPath;
}

// ---------------------------------------------------------------------------
// Path and process helpers
// ---------------------------------------------------------------------------

bool gc_utf8_to_wide(const char* utf8, WCHAR* out, int outCount) {
    if (!out || outCount <= 0) return false;
    out[0] = 0;
    if (!utf8) return false;
    if (!utf8[0]) return true;
    int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, outCount);
    if (written <= 0) {
        out[0] = 0;
        return false;
    }
    return true;
}

bool gc_wide_to_utf8(const WCHAR* wide, char* out, int outCount) {
    if (!out || outCount <= 0) return false;
    out[0] = 0;
    if (!wide) return false;
    if (!wide[0]) return true;
    int written = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, outCount, nullptr, nullptr);
    if (written <= 0) {
        out[0] = 0;
        return false;
    }
    return true;
}

bool gc_module_path(WCHAR* out, size_t outCount) {
    if (!out || outCount == 0) return false;
    out[0] = 0;
    DWORD length = GetModuleFileNameW(nullptr, out, (DWORD)outCount);
    if (length == 0 || length >= outCount) {
        out[0] = 0;
        return false;
    }
    return true;
}

bool gc_directory_of(const WCHAR* filePath, WCHAR* out, size_t outCount) {
    if (!out || outCount == 0) return false;
    out[0] = 0;
    if (!filePath || !filePath[0]) return false;
    if (FAILED(StringCchCopyW(out, outCount, filePath))) return false;
    WCHAR* slash = wcsrchr(out, L'\\');
    WCHAR* forward = wcsrchr(out, L'/');
    if (forward && (!slash || forward > slash)) slash = forward;
    if (!slash) {
        out[0] = 0;
        return false;
    }
    *slash = 0;
    return out[0] != 0;
}

bool gc_module_directory(WCHAR* out, size_t outCount) {
    WCHAR path[MAX_PATH] = {};
    if (!gc_module_path(path, GC_ARRAY_COUNT(path))) return false;
    return gc_directory_of(path, out, outCount);
}

bool gc_join_path(const WCHAR* directory, const WCHAR* leaf, WCHAR* out, size_t outCount) {
    if (!out || outCount == 0) return false;
    out[0] = 0;
    if (!directory || !directory[0] || !leaf || !leaf[0]) return false;
    size_t length = wcslen(directory);
    while (length > 0 && (directory[length - 1] == L'\\' || directory[length - 1] == L'/')) length--;
    if (length == 0) return false;
    if (FAILED(StringCchCopyNW(out, outCount, directory, length))) return false;
    if (FAILED(StringCchCatW(out, outCount, L"\\"))) return false;
    return SUCCEEDED(StringCchCatW(out, outCount, leaf));
}

bool gc_file_exists(const WCHAR* path) {
    if (!path || !path[0]) return false;
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool gc_directory_exists(const WCHAR* path) {
    if (!path || !path[0]) return false;
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool gc_create_private_temp_directory(WCHAR* out, size_t outCount) {
    if (!out || outCount == 0) return false;
    out[0] = 0;
    PWSTR tempRoot = nullptr;
    if (FAILED(SHGetKnownFolderPath(
            FOLDERID_ProgramFiles, 0, nullptr, &tempRoot)) || !tempRoot)
        return false;

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;FA;;;SY)(A;;FA;;;BA)", SDDL_REVISION_1,
            &descriptor, nullptr)) {
        CoTaskMemFree(tempRoot);
        return false;
    }
    SECURITY_ATTRIBUTES security = {
        sizeof(security), descriptor, FALSE,
    };
    bool created = false;
    for (int attempt = 0; attempt < 8 && !created; ++attempt) {
        GUID id = {};
        if (FAILED(CoCreateGuid(&id))) break;
        WCHAR leaf[96] = {};
        if (FAILED(StringCchPrintfW(leaf, GC_ARRAY_COUNT(leaf),
                L"greencurve-setup-%08lx%04x%04x%02x%02x",
                id.Data1, id.Data2, id.Data3, id.Data4[0], id.Data4[1])) ||
            !gc_join_path(tempRoot, leaf, out, outCount)) break;
        if (CreateDirectoryW(out, &security)) {
            created = true;
            break;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
    LocalFree(descriptor);
    CoTaskMemFree(tempRoot);
    if (!created) {
        out[0] = 0;
        return false;
    }
    DWORD attributes = GetFileAttributesW(out);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        RemoveDirectoryW(out);
        out[0] = 0;
        return false;
    }
    return true;
}

static bool gc_path_is_direct_child_of_root(
    const WCHAR* path, const WCHAR* root) {
    if (!path || !path[0] || !root || !root[0]) return false;
    WCHAR fullPath[GC_INSTALLER_MAX_PATH_CHARS] = {};
    WCHAR fullRoot[GC_INSTALLER_MAX_PATH_CHARS] = {};
    DWORD pathLength = GetFullPathNameW(path, GC_ARRAY_COUNT(fullPath),
        fullPath, nullptr);
    DWORD rootLength = GetFullPathNameW(root, GC_ARRAY_COUNT(fullRoot),
        fullRoot, nullptr);
    if (pathLength == 0 || pathLength >= GC_ARRAY_COUNT(fullPath) ||
        rootLength == 0 || rootLength >= GC_ARRAY_COUNT(fullRoot))
        return false;
    while (rootLength > 0 &&
        (fullRoot[rootLength - 1] == L'\\' ||
         fullRoot[rootLength - 1] == L'/')) --rootLength;
    if (pathLength <= rootLength ||
        _wcsnicmp(fullPath, fullRoot, rootLength) != 0 ||
        (fullPath[rootLength] != L'\\' && fullPath[rootLength] != L'/'))
        return false;
    const WCHAR* leaf = fullPath + rootLength + 1;
    return leaf[0] && !wcschr(leaf, L'\\') && !wcschr(leaf, L'/');
}

bool gc_install_directory_is_secure_rooted(const WCHAR* path) {
    if (!path || !path[0]) return false;
    const KNOWNFOLDERID* roots[] = {
        &FOLDERID_ProgramFiles,
        &FOLDERID_ProgramFilesX86,
    };
    for (const KNOWNFOLDERID* rootId : roots) {
        PWSTR root = nullptr;
        HRESULT status = SHGetKnownFolderPath(*rootId, 0, nullptr, &root);
        bool accepted = SUCCEEDED(status) && root &&
            gc_path_is_direct_child_of_root(path, root);
        if (root) CoTaskMemFree(root);
        if (accepted) return true;
    }
    return false;
}

bool gc_create_directory_tree(const WCHAR* path) {
    if (!path || !path[0]) return false;
    if (gc_directory_exists(path)) return true;
    WCHAR work[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (FAILED(StringCchCopyW(work, GC_ARRAY_COUNT(work), path))) return false;
    // Walk forwards creating each component.  Starting after the root avoids
    // trying to "create" C:\ or a UNC share, which always fails.
    size_t index = 0;
    if (work[0] && work[1] == L':') {
        index = 2;
        if (work[2] == L'\\' || work[2] == L'/') index = 3;
    } else if ((work[0] == L'\\' || work[0] == L'/') && (work[1] == L'\\' || work[1] == L'/')) {
        // \\server\share — skip both components; neither can be created.
        index = 2;
        int seen = 0;
        while (work[index] && seen < 2) {
            if (work[index] == L'\\' || work[index] == L'/') seen++;
            index++;
        }
    }
    for (; work[index]; index++) {
        if (work[index] != L'\\' && work[index] != L'/') continue;
        WCHAR saved = work[index];
        work[index] = 0;
        if (work[0] && !gc_directory_exists(work)) {
            if (!CreateDirectoryW(work, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
                gc_log_fail("could not create directory %ls (error %lu)", work, GetLastError());
                return false;
            }
        }
        work[index] = saved;
    }
    if (!CreateDirectoryW(work, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        gc_log_fail("could not create directory %ls (error %lu)", work, GetLastError());
        return false;
    }
    return gc_directory_exists(path);
}

bool gc_service_image_directory(WCHAR* out, size_t outCount) {
    if (!out || outCount == 0) return false;
    out[0] = 0;
    GcScopedServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm.valid()) return false;
    GcScopedServiceHandle service(OpenServiceW(scm.get(), GC_SETUP_SERVICE_NAME, SERVICE_QUERY_CONFIG));
    if (!service.valid()) return false;
    DWORD needed = 0;
    QueryServiceConfigW(service.get(), nullptr, 0, &needed);
    if (needed == 0) return false;
    BYTE* buffer = (BYTE*)HeapAlloc(GetProcessHeap(), 0, needed);
    if (!buffer) return false;
    bool ok = false;
    if (QueryServiceConfigW(service.get(), (QUERY_SERVICE_CONFIGW*)buffer, needed, &needed)) {
        const QUERY_SERVICE_CONFIGW* config = (const QUERY_SERVICE_CONFIGW*)buffer;
        const WCHAR* imagePath = config->lpBinaryPathName;
        if (imagePath && imagePath[0]) {
            // The registered command line is `"<path>" --service-run`; take the
            // quoted program and drop everything after it.
            WCHAR exePath[GC_INSTALLER_MAX_PATH_CHARS] = {};
            if (imagePath[0] == L'"') {
                const WCHAR* closing = wcschr(imagePath + 1, L'"');
                if (closing && closing > imagePath + 1) {
                    size_t length = (size_t)(closing - (imagePath + 1));
                    if (length < GC_ARRAY_COUNT(exePath)) {
                        StringCchCopyNW(exePath, GC_ARRAY_COUNT(exePath), imagePath + 1, length);
                    }
                }
            } else {
                const WCHAR* space = wcschr(imagePath, L' ');
                size_t length = space ? (size_t)(space - imagePath) : wcslen(imagePath);
                if (length > 0 && length < GC_ARRAY_COUNT(exePath)) {
                    StringCchCopyNW(exePath, GC_ARRAY_COUNT(exePath), imagePath, length);
                }
            }
            if (exePath[0]) ok = gc_directory_of(exePath, out, outCount);
        }
    }
    HeapFree(GetProcessHeap(), 0, buffer);
    return ok;
}

// The name the primary data stream is parked under while the file is unlinked.
// It never survives the call; it exists only because a stream has to be renamed
// to *something*.
#define GC_SELF_DELETE_STREAM L":greencurve-uninstall-pending"

// The two toolchains disagree about whether FileDispositionInfoEx exists.
//
// Both ship FILE_DISPOSITION_INFO_EX and its flags, but they gate the
// FILE_INFO_BY_HANDLE_CLASS enumerator differently: llvm-mingw on
// `NTDDI_VERSION >= 0x0A000002`, and Zig's bundled mingw copy on
// `_WIN32_WINNT >= 0x0A000002` — a comparison against an NTDDI constant that a
// _WIN32_WINNT value can never reach.  At this project's 0x0A00 target the x64
// build therefore sees the name and the arm64 build does not.
//
// The enumerator's value is fixed by the OS ABI, so it is spelled out where the
// name is missing and checked against the header where it is present.  A future
// header that declares the name under a condition this does not detect fails
// loudly at compile time rather than silently reverting to the classic
// disposition, which does not work (see below).
#if defined(NTDDI_VERSION) && NTDDI_VERSION >= 0x0A000002
#define GC_FILE_DISPOSITION_INFO_EX_CLASS FileDispositionInfoEx
static_assert((int)FileDispositionInfoEx == 21,
              "FileDispositionInfoEx must stay FILE_INFO_BY_HANDLE_CLASS 21");
#else
#define GC_FILE_DISPOSITION_INFO_EX_CLASS ((FILE_INFO_BY_HANDLE_CLASS)21)
#endif

// Delete the running executable, now rather than at the next restart.
//
// DeleteFile on a mapped image fails with ERROR_ACCESS_DENIED: the file cannot
// go away while a section object is backed by it.  The section is attached to
// the *default data stream*, though, not to the directory entry — so renaming
// `::$DATA` into an alternate stream (which a mapped image does permit) leaves
// the primary stream empty, after which the directory entry can be unlinked.
// The already-mapped code keeps running from memory, so this returns into a
// process whose executable no longer exists.
//
// **Both halves are required, and the second one has to be the POSIX form.**
// Measured on Windows 11 26200, all three combinations, one fresh binary each:
//
//   FileDispositionInfoEx(POSIX) alone            -> ERROR_ACCESS_DENIED
//   rename + FileDispositionInfo (classic)        -> ERROR_ACCESS_DENIED
//   rename + FileDispositionInfoEx(DELETE|POSIX)  -> file and folder gone
//
// The classic disposition is the spelling every published version of this
// technique uses, and it does not work here: the image-section check still
// refuses it after the rename.  Only the POSIX-semantics delete, which unlinks
// the name and lets existing sections outlive it, gets through — which is
// exactly the semantics wanted, since the section in question is this process.
// The classic form is kept only as a fallback for a system that rejects
// FileDispositionInfoEx outright (pre-1607, or a filesystem without it).
//
// This is the difference between "uninstalled" and "uninstalled, but the folder
// is still there until you reboot".  It needs alternate data streams, so it is
// NTFS-only; every caller keeps the reboot path as its fallback.
bool gc_delete_running_module(const WCHAR* path) {
    if (!path || !path[0]) return false;
    if (!gc_file_exists(path)) return true;

    // FILE_RENAME_INFO carries the new name inline, so the structure and the
    // name share one buffer.
    BYTE storage[sizeof(FILE_RENAME_INFO) + sizeof(GC_SELF_DELETE_STREAM)] = {};
    auto* rename = (FILE_RENAME_INFO*)storage;
    const size_t streamChars = GC_ARRAY_COUNT(GC_SELF_DELETE_STREAM) - 1;
    rename->RootDirectory = nullptr;
    // Documented as the length of the name in bytes, excluding the terminator.
    rename->FileNameLength = (DWORD)(streamChars * sizeof(WCHAR));
    memcpy(rename->FileName, GC_SELF_DELETE_STREAM, sizeof(GC_SELF_DELETE_STREAM));

    HANDLE file = CreateFileW(path, DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        gc_log_step("self-delete: %ls could not be opened for deletion (error %lu)",
                    path, GetLastError());
        return false;
    }
    BOOL renamed = SetFileInformationByHandle(file, FileRenameInfo, rename, (DWORD)sizeof(storage));
    DWORD renameError = renamed ? 0 : GetLastError();
    CloseHandle(file);
    if (!renamed) {
        // The usual reason is a volume without alternate data streams.
        gc_log_step("self-delete: the data stream of %ls could not be renamed (error %lu)",
                    path, renameError);
        return false;
    }

    file = CreateFileW(path, DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        gc_log_step("self-delete: %ls could not be reopened after the stream rename (error %lu)",
                    path, GetLastError());
        return false;
    }
    FILE_DISPOSITION_INFO_EX posix = {};
    posix.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;
    BOOL marked = SetFileInformationByHandle(file, GC_FILE_DISPOSITION_INFO_EX_CLASS, &posix,
                                             (DWORD)sizeof(posix));
    DWORD markError = marked ? 0 : GetLastError();
    if (!marked) {
        // Only reachable where FileDispositionInfoEx does not exist; on every
        // system that has it, the classic form is the one that gets refused.
        gc_log_step("self-delete: the POSIX deletion of %ls was refused (error %lu); "
                    "trying the classic disposition", path, markError);
        FILE_DISPOSITION_INFO classic = {};
        classic.DeleteFile = TRUE;
        marked = SetFileInformationByHandle(file, FileDispositionInfo, &classic,
                                            (DWORD)sizeof(classic));
        markError = marked ? 0 : GetLastError();
    }
    // The unlink happens here, on the last close.
    CloseHandle(file);
    if (!marked) {
        gc_log_step("self-delete: %ls could not be marked for deletion (error %lu)", path, markError);
        return false;
    }
    // State, not return codes: the only thing that matters is whether the
    // directory entry is gone, because the caller's next step removes the
    // folder that contains it.
    if (gc_file_exists(path)) {
        gc_log_step("self-delete: %ls still exists after the deletion was accepted", path);
        return false;
    }
    gc_log_step("self-delete: removed %ls while it was running", path);
    return true;
}

bool gc_run_and_wait(const WCHAR* exePath, const WCHAR* commandLine, DWORD timeoutMs,
                     DWORD* exitCodeOut) {
    if (exitCodeOut) *exitCodeOut = (DWORD)-1;
    if (!exePath || !exePath[0]) return false;
    WCHAR mutableCommandLine[2048] = {};
    if (FAILED(StringCchCopyW(mutableCommandLine, GC_ARRAY_COUNT(mutableCommandLine),
                              commandLine ? commandLine : exePath))) {
        gc_log_fail("command line for %ls is too long", exePath);
        return false;
    }
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(exePath, mutableCommandLine, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        gc_log_fail("could not start %ls (error %lu)", exePath, GetLastError());
        return false;
    }
    GcScopedHandle processHandle(process.hProcess);
    GcScopedHandle threadHandle(process.hThread);
    DWORD wait = WaitForSingleObject(processHandle.get(), timeoutMs);
    if (wait != WAIT_OBJECT_0) {
        // Never leave the child behind.  A helper that outlives its timeout
        // would still be holding files setup is about to replace, and (when it
        // is greencurve.exe) may be showing a window the user did not ask for.
        gc_log_fail("%ls did not finish within %lu ms (wait result %lu); terminating it",
                    exePath, timeoutMs, wait);
        TerminateProcess(processHandle.get(), 1);
        WaitForSingleObject(processHandle.get(), 5000);
        return false;
    }
    DWORD exitCode = (DWORD)-1;
    if (!GetExitCodeProcess(processHandle.get(), &exitCode)) {
        gc_log_fail("could not read the exit code of %ls (error %lu)", exePath, GetLastError());
        return false;
    }
    if (exitCodeOut) *exitCodeOut = exitCode;
    return true;
}
