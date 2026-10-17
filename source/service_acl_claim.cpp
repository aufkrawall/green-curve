// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Claim-or-create for the service's machine-scope objects: an existing object
// is used only when a handle that does not follow reparse points proves it is
// already ours; anything else is set aside and replaced by an object whose
// security descriptor is part of its creation.  See service_acl.h.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <sddl.h>
#include <strsafe.h>

#include "service_acl.h"

namespace {

void set_claim_err(char* err, size_t errSize, const char* msg, DWORD code) {
    if (!err || errSize == 0) return;
    StringCchPrintfA(err, errSize, "%s (error %lu)", msg, (unsigned long)code);
}

// The descriptor goes into the create call itself, so there is no moment in
// which the new object carries an inherited ACL someone else could act on.
bool build_creation_descriptor(GcServiceAclKind kind, bool requireAdminOwner,
                               PSECURITY_DESCRIPTOR* out) {
    *out = nullptr;
    WCHAR sddl[160] = {};
    if (FAILED(StringCchPrintfW(sddl, ARRAYSIZE(sddl), L"%ls%ls",
                                requireAdminOwner ? L"O:BA" : L"",
                                service_acl_kind_sddl(kind))))
        return false;
    return ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1,
                                                                out, nullptr) != FALSE;
}

bool handle_facts(HANDLE handle, bool* isDirectory, bool* isReparse) {
    FILE_ATTRIBUTE_TAG_INFO tag = {};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)))
        return false;
    *isDirectory = (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    *isReparse = (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    return true;
}

// Exactly ours: a real (non-reparse) object of the right type, carrying
// exactly our protected DACL, owned by an account a standard user cannot
// assign.  Every fact comes from the handle, never from a path lookup.
bool handle_is_proven(HANDLE handle, GcServiceAclKind kind, bool requireAdminOwner) {
    bool isDirectory = false;
    bool isReparse = false;
    if (!handle_facts(handle, &isDirectory, &isReparse)) return false;
    if (isReparse || isDirectory != service_acl_kind_is_directory(kind)) return false;
    if (!service_handle_dacl_is_ours(handle, kind)) return false;
    return !requireAdminOwner || service_handle_owner_is_privileged(handle);
}

bool same_object(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
           a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
}

// Pinned: no FILE_SHARE_DELETE, so the object cannot be renamed or deleted
// while the handle is open.  No write access is requested; a proven object
// needs none, and an unproven one is never written through.
HANDLE open_pinned_directory(const wchar_t* path) {
    return CreateFileW(path, READ_CONTROL | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES |
                           SYNCHRONIZE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
}

enum ClaimStep { STEP_DONE, STEP_RETRY, STEP_FAIL };

// Something holds the name and is not provably ours.  Move exactly that
// object aside: the name must still carry the file id the inspection saw, and
// the moved object must carry it too, so a concurrent claimer's fresh
// directory is never the one moved and then deleted.
ClaimStep set_aside_unproven(const wchar_t* path, bool haveIdentity,
                             const BY_HANDLE_FILE_INFORMATION& inspected,
                             GcServiceAclKind kind, bool requireAdminOwner,
                             char* err, size_t errSize) {
    const DWORD flags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    // A squatter's DACL can deny READ_CONTROL; the parent's list right still
    // yields FILE_READ_ATTRIBUTES, which is all an identity check needs.
    HANDLE probe = CreateFileW(path, READ_CONTROL | FILE_READ_ATTRIBUTES, share, nullptr,
                               OPEN_EXISTING, flags, nullptr);
    bool canReadDescriptor = probe != INVALID_HANDLE_VALUE;
    if (probe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED)
        probe = CreateFileW(path, FILE_READ_ATTRIBUTES, share, nullptr, OPEN_EXISTING, flags,
                            nullptr);
    if (probe == INVALID_HANDLE_VALUE) {
        DWORD openErr = GetLastError();
        if (openErr == ERROR_FILE_NOT_FOUND || openErr == ERROR_PATH_NOT_FOUND)
            return STEP_RETRY;  // someone else already moved it
        set_claim_err(err, errSize, "Cannot inspect the unproven object", openErr);
        return STEP_FAIL;
    }
    BY_HANDLE_FILE_INFORMATION now = {};
    const bool identityKnown = GetFileInformationByHandle(probe, &now) != FALSE;
    const bool provenNow = canReadDescriptor && handle_is_proven(probe, kind, requireAdminOwner);
    CloseHandle(probe);
    if (provenNow) return STEP_RETRY;  // a concurrent claimer won; adopt next pass
    if (haveIdentity && (!identityKnown || !same_object(now, inspected)))
        return STEP_RETRY;  // the name changed hands; look again
    if (!identityKnown) {
        set_claim_err(err, errSize, "Cannot identify the unproven object", ERROR_INVALID_DATA);
        return STEP_FAIL;
    }
    return service_set_aside_by_name(path, &now, err, errSize) ? STEP_DONE : STEP_FAIL;
}

}  // namespace

const char* service_claim_result_name(int result) {
    switch (result) {
        case GC_SERVICE_CLAIM_CREATED: return "created";
        case GC_SERVICE_CLAIM_ADOPTED: return "adopted";
        case GC_SERVICE_CLAIM_REPLACED: return "replaced";
        case GC_SERVICE_CLAIM_FAILED: return "failed";
        default: return "unknown";
    }
}

int service_claim_protected_directory(const wchar_t* path, GcServiceAclKind kind,
                                      bool requireAdminOwner, bool allowAdopt,
                                      void** pinnedOut, char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (pinnedOut) *pinnedOut = INVALID_HANDLE_VALUE;
    if (!path || !path[0] || !service_acl_kind_is_directory(kind)) {
        set_claim_err(err, errSize, "Invalid directory claim", ERROR_INVALID_PARAMETER);
        return GC_SERVICE_CLAIM_FAILED;
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!build_creation_descriptor(kind, requireAdminOwner, &descriptor)) {
        set_claim_err(err, errSize, "Cannot build the creation descriptor", GetLastError());
        return GC_SERVICE_CLAIM_FAILED;
    }
    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), descriptor, FALSE};
    int result = GC_SERVICE_CLAIM_FAILED;
    bool replaced = false;
    // Bounded: each pass either finishes, or observed another claimer change
    // the name between two of its own steps.
    for (int pass = 0; pass < 4 && result == GC_SERVICE_CLAIM_FAILED; pass++) {
        bool created = false;
        if (CreateDirectoryW(path, &attributes)) {
            created = true;
        } else {
            DWORD createErr = GetLastError();
            if (createErr != ERROR_ALREADY_EXISTS) {
                set_claim_err(err, errSize, "Cannot create the directory", createErr);
                break;
            }
            if (!allowAdopt) {
                set_claim_err(err, errSize, "The directory name is already taken", createErr);
                break;
            }
        }
        HANDLE pinned = open_pinned_directory(path);
        DWORD openErr = pinned == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        if (pinned != INVALID_HANDLE_VALUE && handle_is_proven(pinned, kind, requireAdminOwner)) {
            result = created ? (replaced ? GC_SERVICE_CLAIM_REPLACED : GC_SERVICE_CLAIM_CREATED)
                             : GC_SERVICE_CLAIM_ADOPTED;
            if (pinnedOut) *pinnedOut = pinned;
            else CloseHandle(pinned);
            break;
        }
        if (created) {
            // We just created it with our descriptor and it does not prove:
            // nothing a retry could change.
            if (pinned != INVALID_HANDLE_VALUE) CloseHandle(pinned);
            set_claim_err(err, errSize, "The created directory did not prove as ours",
                          openErr != ERROR_SUCCESS ? openErr : (DWORD)ERROR_INVALID_ACL);
            break;
        }
        if (openErr == ERROR_FILE_NOT_FOUND || openErr == ERROR_PATH_NOT_FOUND) continue;
        if (openErr == ERROR_SHARING_VIOLATION) {
            set_claim_err(err, errSize, "The existing object is held open exclusively", openErr);
            break;
        }
        BY_HANDLE_FILE_INFORMATION inspected = {};
        bool haveIdentity = false;
        if (pinned != INVALID_HANDLE_VALUE) {
            haveIdentity = GetFileInformationByHandle(pinned, &inspected) != FALSE;
            // Closed before the move: this handle denies FILE_SHARE_DELETE
            // and would block the rename.
            CloseHandle(pinned);
        }
        ClaimStep step = set_aside_unproven(path, haveIdentity, inspected, kind,
                                            requireAdminOwner, err, errSize);
        if (step == STEP_FAIL) break;
        if (step == STEP_DONE) replaced = true;
    }
    if (result == GC_SERVICE_CLAIM_FAILED && err && errSize && !err[0])
        set_claim_err(err, errSize, "The directory name kept changing hands", ERROR_RETRY);
    LocalFree(descriptor);
    return result;
}

void* service_create_protected_file(const wchar_t* path, GcServiceAclKind kind,
                                    bool requireAdminOwner, unsigned long access,
                                    unsigned long shareMode, char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!path || !path[0] || service_acl_kind_is_directory(kind)) {
        set_claim_err(err, errSize, "Invalid protected file request", ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!build_creation_descriptor(kind, requireAdminOwner, &descriptor)) {
        set_claim_err(err, errSize, "Cannot build the creation descriptor", GetLastError());
        return INVALID_HANDLE_VALUE;
    }
    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), descriptor, FALSE};
    HANDLE file = CreateFileW(path, access, shareMode, &attributes, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    DWORD createErr = file == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    LocalFree(descriptor);
    if (file == INVALID_HANDLE_VALUE)
        set_claim_err(err, errSize, "Cannot create the protected file", createErr);
    return file;
}

bool service_directory_holds_only(void* directoryRaw, GcServiceAclKind kind,
                                  bool requireAdminOwner, const wchar_t* expectedLeaf,
                                  char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    HANDLE directory = (HANDLE)directoryRaw;
    if (!directory || directory == INVALID_HANDLE_VALUE || !expectedLeaf || !expectedLeaf[0] ||
        !service_acl_kind_is_directory(kind)) {
        set_claim_err(err, errSize, "Invalid directory content check", ERROR_INVALID_PARAMETER);
        return false;
    }
    if (!handle_is_proven(directory, kind, requireAdminOwner)) {
        set_claim_err(err, errSize, "The directory's DACL or owner no longer proves as ours",
                      ERROR_INVALID_ACL);
        return false;
    }
    alignas(8) BYTE buffer[8192];
    int entries = 0;
    bool expectedSeen = false;
    FILE_INFO_BY_HANDLE_CLASS infoClass = FileFullDirectoryRestartInfo;
    for (;;) {
        if (!GetFileInformationByHandleEx(directory, infoClass, buffer, sizeof(buffer))) {
            DWORD listErr = GetLastError();
            if (listErr == ERROR_NO_MORE_FILES) break;
            set_claim_err(err, errSize, "Cannot list the directory", listErr);
            return false;
        }
        infoClass = FileFullDirectoryInfo;
        const BYTE* at = buffer;
        for (;;) {
            const FILE_FULL_DIR_INFO* entry = (const FILE_FULL_DIR_INFO*)at;
            const int nameChars = (int)(entry->FileNameLength / sizeof(WCHAR));
            const bool dot = (nameChars == 1 && entry->FileName[0] == L'.') ||
                (nameChars == 2 && entry->FileName[0] == L'.' && entry->FileName[1] == L'.');
            if (!dot) {
                entries++;
                const bool regular = (entry->FileAttributes &
                    (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
                if (regular && CompareStringOrdinal(entry->FileName, nameChars, expectedLeaf,
                                                    -1, TRUE) == CSTR_EQUAL)
                    expectedSeen = true;
            }
            if (entry->NextEntryOffset == 0) break;
            at += entry->NextEntryOffset;
        }
    }
    if (entries != 1 || !expectedSeen) {
        set_claim_err(err, errSize, entries == 0
                          ? "The directory no longer holds the expected file"
                          : "The directory holds something besides the expected file",
                      ERROR_INVALID_DATA);
        return false;
    }
    return true;
}
