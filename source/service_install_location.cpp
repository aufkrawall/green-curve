// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Win32 half of the install-location gate (service_install_location_policy.h):
// may Green Curve REWRITE this folder's permissions to register a LocalSystem
// service out of it?
//
// Three independent refusals, each for a class of folder that is not ours:
//   1. the folder IS a well-known shell folder (by directory identity, so an
//      8.3 spelling or an ancestor junction cannot slip past a string match),
//      or another account's profile shell folder;
//   2. the folder lies anywhere inside the Windows directory;
//   3. the folder already exists and holds something Green Curve did not put
//      there -- unless Green Curve already hardened it.
// The third is the one that makes the gate a statement about ownership rather
// than a blocklist: no list names %LOCALAPPDATA%\Programs, C:\Program
// Files\<another vendor> or a shared D:\Tools, and each of them would lose
// write access for every program in it.
//
// Every existing-folder fact is read through ONE handle.  When the caller is
// about to harden, it passes the handle it will harden through, so the object
// judged here is the object whose DACL is then written.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <shlobj.h>
#include <strsafe.h>

#include "service_acl.h"

#define GC_PATH_ARRAY_COUNT_LOCAL(a) (sizeof(a) / sizeof((a)[0]))

namespace {

struct GcDirectoryIdentity {
    BY_HANDLE_FILE_INFORMATION file = {};
    WCHAR finalPath[GC_PATH_CHAIN_MAX_PATH_CHARS] = {};
};

bool identity_from_handle(HANDLE handle, GcDirectoryIdentity* out) {
    *out = GcDirectoryIdentity{};
    if (!GetFileInformationByHandle(handle, &out->file)) return false;
    DWORD length = GetFinalPathNameByHandleW(handle, out->finalPath,
        GC_PATH_CHAIN_MAX_PATH_CHARS, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0 || length >= GC_PATH_CHAIN_MAX_PATH_CHARS) out->finalPath[0] = 0;
    return true;
}

// For the KNOWN folders only: follow an ancestor junction or short name to
// the object a DACL write through that name would actually change.
bool read_directory_identity(const WCHAR* path, GcDirectoryIdentity* out) {
    if (!path || !out) return false;
    HANDLE handle = CreateFileW(path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    bool ok = identity_from_handle(handle, out);
    CloseHandle(handle);
    return ok;
}

bool same_directory_identity(const GcDirectoryIdentity& a, const GcDirectoryIdentity& b) {
    return a.file.dwVolumeSerialNumber == b.file.dwVolumeSerialNumber &&
           a.file.nFileIndexHigh == b.file.nFileIndexHigh &&
           a.file.nFileIndexLow == b.file.nFileIndexLow;
}

size_t trimmed_length(const WCHAR* path, size_t length) {
    while (length > 0 && (path[length - 1] == L'\\' || path[length - 1] == L'/')) length--;
    return length;
}

// Rule 3.  Every entry must be one Green Curve itself ships, leaves behind or
// stages.  Enumerated through the caller's handle; any enumeration failure
// refuses, because this stands in front of a DACL rewrite.
int content_verdict(HANDLE directory, GcServiceLocationDetail* detail) {
    alignas(8) BYTE buffer[16384];
    FILE_INFO_BY_HANDLE_CLASS infoClass = FileFullDirectoryRestartInfo;
    for (;;) {
        if (!GetFileInformationByHandleEx(directory, infoClass, buffer, sizeof(buffer))) {
            if (GetLastError() == ERROR_NO_MORE_FILES) return GC_SVC_LOCATION_OK;
            return GC_SVC_LOCATION_UNREADABLE;
        }
        infoClass = FileFullDirectoryInfo;
        const BYTE* cursor = buffer;
        for (;;) {
            const FILE_FULL_DIR_INFO* entry = (const FILE_FULL_DIR_INFO*)cursor;
            size_t nameLength = entry->FileNameLength / sizeof(WCHAR);
            const WCHAR* name = entry->FileName;
            bool dot = (nameLength == 1 && name[0] == L'.') ||
                       (nameLength == 2 && name[0] == L'.' && name[1] == L'.');
            if (!dot) {
                detail->entriesScanned++;
                bool isDirectory = (entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                bool isReparse = (entry->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
                if (isReparse || !gc_service_location_entry_is_ours(name, nameLength, isDirectory)) {
                    detail->foreignIsDirectory = isDirectory;
                    detail->foreignIsReparse = isReparse;
                    return GC_SVC_LOCATION_FOREIGN_CONTENT;
                }
            }
            if (entry->NextEntryOffset == 0) break;
            cursor += entry->NextEntryOffset;
        }
    }
}

// Rule 2.  Compared both as spelled and as resolved, so neither an 8.3 name
// nor a junction that lands inside %WINDIR% gets past it.
bool within_windows_directory(const WCHAR* full, const GcDirectoryIdentity* candidate) {
    PWSTR windows = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Windows, 0, nullptr, &windows)) || !windows) {
        if (windows) CoTaskMemFree(windows);
        return false;
    }
    bool within = gc_service_location_is_within(full, windows);
    if (!within && candidate && candidate->finalPath[0]) {
        GcDirectoryIdentity windowsIdentity = {};
        if (read_directory_identity(windows, &windowsIdentity) && windowsIdentity.finalPath[0])
            within = gc_service_location_is_within(candidate->finalPath, windowsIdentity.finalPath);
    }
    CoTaskMemFree(windows);
    return within;
}

// Rule 1.  FOLDERID_Downloads and the other per-user media folders are here
// because "extract the archive anywhere" plus 7-Zip's Extract Here lands
// greencurve.exe directly in one of them.
const KNOWNFOLDERID* const kRefusedFolders[] = {
    &FOLDERID_Profile,         &FOLDERID_UserProfiles,
    &FOLDERID_Desktop,         &FOLDERID_Downloads,
    &FOLDERID_Documents,       &FOLDERID_Music,
    &FOLDERID_Pictures,        &FOLDERID_Videos,
    &FOLDERID_LocalAppData,    &FOLDERID_RoamingAppData,
    &FOLDERID_LocalAppDataLow, &FOLDERID_ProgramData,
    &FOLDERID_Windows,         &FOLDERID_System,
    &FOLDERID_SystemX86,       &FOLDERID_ProgramFiles,
    &FOLDERID_ProgramFilesX86, &FOLDERID_ProgramFilesCommon,
    &FOLDERID_UserProgramFiles,
    &FOLDERID_Public,          &FOLDERID_PublicDesktop,
    &FOLDERID_PublicDocuments, &FOLDERID_PublicDownloads,
};

// Resolve known folder `index`; false when the shell has no answer.
bool resolve_refused_folder(size_t index, PWSTR* out) {
    *out = nullptr;
    if (FAILED(SHGetKnownFolderPath(*kRefusedFolders[index], 0, nullptr, out)) || !*out) {
        // Shell32 can return an allocated pointer even on failure.
        if (*out) CoTaskMemFree(*out);
        *out = nullptr;
        return false;
    }
    return true;
}

// The refusals that need nothing but the spelling -- a known folder named
// exactly, anything under the Windows directory as spelled.  Asked BEFORE any
// open, so a folder the caller cannot even open (C:\Windows\Temp for a
// standard account) is named for what it is instead of "unreadable".
int spelling_verdict(const WCHAR* full, size_t fullLength) {
    size_t candidateLength = trimmed_length(full, fullLength);
    for (size_t i = 0; i < GC_PATH_ARRAY_COUNT_LOCAL(kRefusedFolders); i++) {
        PWSTR resolved = nullptr;
        if (!resolve_refused_folder(i, &resolved)) continue;
        size_t resolvedLength = trimmed_length(resolved, wcslen(resolved));
        bool equal = resolvedLength > 0 && resolvedLength == candidateLength &&
            _wcsnicmp(full, resolved, resolvedLength) == 0;
        CoTaskMemFree(resolved);
        if (equal) return GC_SVC_LOCATION_KNOWN_FOLDER;
    }
    if (within_windows_directory(full, nullptr)) return GC_SVC_LOCATION_SYSTEM_SUBTREE;
    return GC_SVC_LOCATION_OK;
}

int verdict_core(const WCHAR* full, HANDLE handle,
                 GcServiceLocationDetail* detail) {
    // spelling_verdict() already ran (verdict_detailed, before any open).
    if (!handle) return GC_SVC_LOCATION_OK;

    GcDirectoryIdentity candidate = {};
    detail->exists = true;
    FILE_ATTRIBUTE_TAG_INFO tag = {};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)))
        return GC_SVC_LOCATION_UNREADABLE;
    if ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return GC_SVC_LOCATION_UNREADABLE;
    if (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return GC_SVC_LOCATION_REPARSE;
    if (!identity_from_handle(handle, &candidate) || !candidate.finalPath[0])
        return GC_SVC_LOCATION_UNREADABLE;

    // The same folders again, by directory identity: an 8.3 spelling or an
    // ancestor junction names the same object under a different string.
    for (size_t i = 0; i < GC_PATH_ARRAY_COUNT_LOCAL(kRefusedFolders); i++) {
        PWSTR resolved = nullptr;
        if (!resolve_refused_folder(i, &resolved)) continue;
        GcDirectoryIdentity known = {};
        bool equal = read_directory_identity(resolved, &known) &&
            same_directory_identity(candidate, known);
        CoTaskMemFree(resolved);
        if (equal) return GC_SVC_LOCATION_KNOWN_FOLDER;
    }
    if (within_windows_directory(full, &candidate)) return GC_SVC_LOCATION_SYSTEM_SUBTREE;

    // SHGetKnownFolderPath with a null token names the approving account's
    // folders. Under UAC that may be a DIFFERENT user from the one whose
    // directory is being installed into, so the common per-profile shell
    // folders also get a machine-wide check against FOLDERID_UserProfiles.
    PWSTR profilesPath = nullptr;
    HRESULT result = SHGetKnownFolderPath(FOLDERID_UserProfiles, 0, nullptr, &profilesPath);
    if (FAILED(result) || !profilesPath) {
        if (profilesPath) CoTaskMemFree(profilesPath);
        return GC_SVC_LOCATION_UNREADABLE;
    }
    GcDirectoryIdentity profiles = {};
    bool readable = read_directory_identity(profilesPath, &profiles) && profiles.finalPath[0];
    CoTaskMemFree(profilesPath);
    if (!readable) return GC_SVC_LOCATION_UNREADABLE;
    if (gc_service_location_is_profile_shell_folder(candidate.finalPath, profiles.finalPath))
        return GC_SVC_LOCATION_KNOWN_FOLDER;

    // Rule 3.  A folder Green Curve already hardened is ours by construction
    // (nobody else writes exactly that DACL), and re-hardening it changes
    // nothing, so its content is not second-guessed: an existing install with
    // a stray file keeps working.
    if (service_handle_dacl_is_ours(handle, GC_SERVICE_ACL_DIRECTORY)) {
        detail->alreadyHardened = true;
        return GC_SVC_LOCATION_OK;
    }
    return content_verdict(handle, detail);
}

int verdict_prepare(const wchar_t* directory, WCHAR* full, size_t* fullLength) {
    if (!directory || !directory[0]) return GC_SVC_LOCATION_EMPTY;
    // Canonicalize first: the shape rules and the known-folder comparison both
    // read characters, and "%USERPROFILE%\Downloads\." must refuse for the
    // same reason "%USERPROFILE%\Downloads" does.
    DWORD length = GetFullPathNameW(directory, GC_PATH_CHAIN_MAX_PATH_CHARS, full, nullptr);
    if (length == 0 || length >= GC_PATH_CHAIN_MAX_PATH_CHARS) {
        // An unresolvable path is refused, not waved through: this gate stands
        // in front of a DACL rewrite, so unproven must mean no.
        return GC_SVC_LOCATION_NOT_ABSOLUTE;
    }
    *fullLength = length;
    return gc_service_location_shape_verdict(full);
}

}  // namespace

int gc_service_install_location_verdict_detailed(const wchar_t* directory, void* directoryHandle,
                                                 GcServiceLocationDetail* detailOut) {
    GcServiceLocationDetail scratch = {};
    GcServiceLocationDetail* detail = detailOut ? detailOut : &scratch;
    *detail = GcServiceLocationDetail{};
    WCHAR full[GC_PATH_CHAIN_MAX_PATH_CHARS] = {};
    size_t fullLength = 0;
    int shape = verdict_prepare(directory, full, &fullLength);
    if (shape != GC_SVC_LOCATION_OK) return shape;
    int spelled = spelling_verdict(full, fullLength);
    if (spelled != GC_SVC_LOCATION_OK) return spelled;

    if (directoryHandle && directoryHandle != INVALID_HANDLE_VALUE)
        return verdict_core(full, (HANDLE)directoryHandle, detail);

    // No handle from the caller: open one.  FILE_FLAG_OPEN_REPARSE_POINT so a
    // leaf junction is SEEN (and refused) rather than silently followed.
    HANDLE handle = CreateFileW(full, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            return GC_SVC_LOCATION_UNREADABLE;
        return verdict_core(full, nullptr, detail);
    }
    int verdict = verdict_core(full, handle, detail);
    CloseHandle(handle);
    return verdict;
}

int gc_service_install_location_verdict(const wchar_t* directory) {
    return gc_service_install_location_verdict_detailed(directory, nullptr, nullptr);
}

int gc_service_install_location_verdict_for_handle(const wchar_t* directory,
                                                   void* directoryHandle) {
    return gc_service_install_location_verdict_detailed(directory, directoryHandle, nullptr);
}
